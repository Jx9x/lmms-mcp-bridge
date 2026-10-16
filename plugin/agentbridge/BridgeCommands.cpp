/*
 * BridgeCommands.cpp - command handlers + ABI self-test.
 *                      Main thread only.
 */

#include "BridgeCommands.h"
#include "CompositionCommands.h"
#include "BridgeModel.h"
#include "BridgeRenderer.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QScopedValueRollback>
#include <QtCore/QSet>
#include <QtWidgets/QApplication>
#include <QtWidgets/QAction>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>

#include "AgentBridge.h"
#include "lmmsversion.h"

#include "AutomationPattern.h"
#include "AutomationTrack.h"
#include "BBTrack.h"
#include "BBTrackContainer.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "GuiApplication.h"
#include "InstrumentTrack.h"
#include "MainWindow.h"
#include "MeterModel.h"
#include "MidiTime.h"
#include "Mixer.h"
#include "Note.h"
#include "Pattern.h"
#include "ProjectJournal.h"
#include "SampleTrack.h"
#include "Song.h"
#include "Track.h"
#include "TrackContainer.h"


namespace
{

const char * const kPluginVersion = "0.4.0";

QByteArray toJson( const QJsonObject & _o )
{
	return QJsonDocument( _o ).toJson( QJsonDocument::Compact );
}

QJsonObject errReply( const QString & _name, const QString & _msg )
{
	QJsonObject o;
	o["ok"] = false;
	o["cmd"] = _name;
	o["error"] = _msg;
	return o;
}

QJsonObject okReply( const QString & _name )
{
	QJsonObject o;
	o["ok"] = true;
	o["cmd"] = _name;
	return o;
}

const char * trackTypeName( Track::TrackTypes _t )
{
	switch( _t )
	{
		case Track::InstrumentTrack:	return "instrument";
		case Track::BBTrack:			return "bb";
		case Track::SampleTrack:		return "sample";
		case Track::EventTrack:			return "event";
		case Track::VideoTrack:			return "video";
		case Track::AutomationTrack:	return "automation";
		case Track::HiddenAutomationTrack:
			return "hiddenAutomation";
		default:						return "unknown";
	}
}

QString ensureTrailingSlash( const QString & _s )
{
	if( !_s.isEmpty() && !_s.endsWith( '/' ) && !_s.endsWith( '\\' ) )
	{
		return _s + '/';
	}
	return _s;
}

QString tcoKind( TrackContentObject * _tco )
{
	if( dynamic_cast<Pattern *>( _tco ) )			{ return "pattern"; }
	if( dynamic_cast<BBTCO *>( _tco ) )				{ return "bb"; }
	if( dynamic_cast<SampleTCO *>( _tco ) )			{ return "sample"; }
	if( dynamic_cast<AutomationPattern *>( _tco ) )	{ return "automation"; }
	return "other";
}

QJsonObject tcoJson( TrackContentObject * _tco )
{
	QJsonObject o;
	o["start"] = (int) _tco->startPosition();
	o["length"] = (int) _tco->length();
	o["kind"] = tcoKind( _tco );
	Pattern * p = dynamic_cast<Pattern *>( _tco );
	if( p )
	{
		QJsonArray notes;
		const NoteVector & nv = p->notes();
		for( NoteVector::const_iterator it = nv.begin();
				it != nv.end(); ++it )
		{
			Note * n = *it;
			if( !n )
			{
				continue;
			}
			QJsonObject no;
			no["key"] = n->key();
			no["pos"] = (int) n->pos();
			no["len"] = (int) n->length();
			no["vol"] = (int) n->getVolume();
			no["pan"] = (int) n->getPanning();
			notes.append( no );
		}
		o["notes"] = notes;
	}
	return o;
}

QJsonObject trackJson( Track * _t, int _index )
{
	QJsonObject o;
	o["index"] = _index;
	o["name"] = _t->name();
	o["type"] = trackTypeName( _t->type() );
	o["muted"] = _t->isMuted();
	o["solo"] = _t->isSolo();
	if( _t->type() == Track::InstrumentTrack )
	{
		InstrumentTrack * it = static_cast<InstrumentTrack *>( _t );
		o["instrument"] = it->instrumentName();
	}
	QJsonArray tcos;
	const Track::tcoVector & tv = _t->getTCOs();
	for( Track::tcoVector::const_iterator it = tv.begin();
			it != tv.end(); ++it )
	{
		TrackContentObject * tco = *it;
		if( tco )
		{
			tcos.append( tcoJson( tco ) );
		}
	}
	o["tcos"] = tcos;
	return o;
}

QJsonArray trackListJson( const TrackContainer::TrackList & _tracks )
{
	QJsonArray a;
	int idx = 0;
	for( TrackContainer::TrackList::const_iterator it = _tracks.begin();
			it != _tracks.end(); ++it, ++idx )
	{
		Track * t = *it;
		if( t )
		{
			a.append( trackJson( t, idx ) );
		}
	}
	return a;
}

const char * playModeName( Song::PlayModes _m )
{
	switch( _m )
	{
		case Song::Mode_None:					return "none";
		case Song::Mode_PlaySong:				return "song";
		case Song::Mode_PlayBB:					return "bb";
		case Song::Mode_PlayPattern:			return "pattern";
		case Song::Mode_PlayAutomationPattern:	return "automationPattern";
		default:								return "unknown";
	}
}

struct Check
{
	QString name;
	bool pass;
	QString detail;
} ;

}




BridgeCommands::BridgeCommands( AgentBridge * _plugin ) :
	m_plugin( _plugin ),
	m_composition( new CompositionCommands ),
	m_handling( false )
{
}

BridgeCommands::~BridgeCommands()
{
	delete m_composition;
}


QByteArray BridgeCommands::handle( const QByteArray & _line )
{
	const QString text = QString::fromUtf8( _line ).trimmed();
	QString name;
	QString arg;
	QString parseError;
	QJsonObject args;
	QJsonValue requestId;
	bool structured = false;
	if( text.startsWith( '{' ) )
	{
		QJsonParseError error;
		const QJsonDocument doc = QJsonDocument::fromJson( _line, &error );
		const QJsonObject request = doc.object();
		if( error.error != QJsonParseError::NoError || !doc.isObject() || !request["cmd"].isString() )
		{
			parseError = "request must be a JSON object with a string cmd";
		}
		else
		{
			name = request["cmd"].toString();
			requestId = request["id"];
			if( request.contains( "args" ) )
			{
				if( !request["args"].isObject() ) { parseError = "args must be a JSON object"; }
				args = request["args"].toObject();
				for( auto it = request.begin(); it != request.end(); ++it )
				{
					if( it.key() != "cmd" && it.key() != "args" && it.key() != "id" ) { parseError = "unexpected request field: " + it.key(); }
				}
			}
			else
			{
				args = request;
				args.remove( "cmd" );
				args.remove( "id" );
			}
		}
		structured = true;
	}
	else
	{
		const int sp = text.indexOf( ' ' );
		name = sp < 0 ? text : text.left( sp );
		arg = sp < 0 ? QString() : text.mid( sp + 1 ).trimmed();
		if( arg.startsWith( '{' ) )
		{
			QJsonParseError error;
			const QJsonDocument doc = QJsonDocument::fromJson( arg.toUtf8(), &error );
			if( error.error != QJsonParseError::NoError || !doc.isObject() ) { parseError = "arguments must be a valid JSON object"; }
			args = doc.object();
			structured = true;
		}
	}
	const QString origName = name;
	const QString cmd = name.toLower();
	if( parseError.isEmpty() && ( structured || cmd != "tempo" ) )
	{
		parseError = !structured && !arg.isEmpty() ? "arguments must be a JSON object" : CompositionCommands::validate( cmd, args );
	}
	if( parseError.isEmpty() && args.contains( "project_token" ) && args["project_token"].toString() != BridgeModel::projectToken() ) { parseError = "stale project_token; inspect the current project"; }
	if( structured && ( cmd == "tempo" || cmd == "set_tempo" ) ) { arg = QString::number( args["bpm"].toInt() ); }
	if( m_handling ) { return toJson( errReply( origName, "busy: another command is executing" ) ); }
	QScopedValueRollback<bool> handling( m_handling, true );
	m_plugin->log( "command: " + name.left( 200 ) );
	QJsonObject res;
	bool modified = false;
	Song * song = Engine::getSong();
	const bool busy = song && song->isLoadingProject();
	bool readOnly = false;
	for( const QJsonValue & entry : CompositionCommands::tools() )
	{
		const QJsonObject spec = entry.toObject();
		if( spec["name"].toString() == cmd ) { readOnly = spec["annotations"].toObject()["readOnlyHint"].toBool(); break; }
	}
	if( !parseError.isEmpty() ) { res = errReply( origName, parseError ); }
	else if( busy && cmd != "ping" && cmd != "help" )
	{
		res = errReply( origName, "busy: project is loading" );
	}
	else if( !readOnly && !( cmd == "cancel_render" && m_composition->isRendering() ) &&
		( QApplication::activeModalWidget() || m_composition->isRendering() || ( song && song->isExporting() ) ) )
	{
		res = errReply( origName, "busy: LMMS has an active dialog or export" );
	}
	else if( cmd == "help" )
	{
		res = okReply( origName );
		QJsonArray cmds;
		cmds.append( "help" );
		cmds.append( "ping" );
		cmds.append( "song" );
		cmds.append( "tracks" );
		cmds.append( "selftest" );
		cmds.append( "tempo <int>" );
		cmds.append( "play" );
		cmds.append( "stop" );
		cmds.append( "show" );
		cmds.append( "project.new" );
		res["legacyCommands"] = cmds;
		const QJsonArray tools = CompositionCommands::tools();
		QJsonArray names;
		for( const QJsonValue & tool : tools ) { names.append( tool.toObject()["name"] ); }
		res["commands"] = names;
		res["tools"] = tools;
		res["protocol"] = "<command> [JSON object], or {cmd,args,id}; one UTF-8 line per request/reply";
		res["maxRequestBytes"] = 64 * 1024;
	}
	else if( cmd == "ping" )
	{
		res = okReply( origName );
		res["pong"] = true;
		res["plugin"] = "agentbridge";
		res["pluginVersion"] = kPluginVersion;
		res["lmmsHeaders"] = QString( LMMS_VERSION );
		res["qtHeaders"] = QString( QT_VERSION_STR );
		res["qtRuntime"] = QString( qVersion() );
		res["pid"] = (double) QCoreApplication::applicationPid();
	}
	else if( cmd == "song" || cmd == "get_project_info" )
	{
		if( !song )
		{
			res = errReply( origName, "no song object" );
		}
		else
		{
			res = okReply( origName );
			res["file"] = song->projectFileName();
			res["modified"] = song->isModified();
			res["loading"] = song->isLoadingProject();
			res["tempo"] = (int) song->getTempo();
			res["timeSig"] = QJsonArray() <<
				song->getTimeSigModel().getNumerator() <<
				song->getTimeSigModel().getDenominator();
			res["masterVolume"] = song->masterVolume();
			res["masterPitch"] = song->masterPitch();
			res["playMode"] = playModeName( song->playMode() );
			res["playing"] = song->isPlaying();
			res["paused"] = song->isPaused();
			res["exporting"] = song->isExporting();
			res["lengthBars"] = (int) song->length();
			res["positionTicks"] = song->playMode() == Song::Mode_None ? song->getPlayPos( Song::Mode_PlaySong ).getTicks() : song->getTicks();
			res["positionMs"] = song->getMilliseconds();
			res["ticksPerBar"] = (int) song->ticksPerTact();
			res["trackCount"] = song->tracks().size();
			BBTrackContainer * bb = Engine::getBBTrackContainer();
			res["bbTrackCount"] = bb ? bb->tracks().size() : 0;
		}
	}
	else if( cmd == "tracks" || cmd == "get_tracks" )
	{
		if( !song )
		{
			res = errReply( origName, "no song object" );
		}
		else
		{
			res = okReply( origName );
			res["song"] = trackListJson( song->tracks() );
			BBTrackContainer * bb = Engine::getBBTrackContainer();
			res["bb"] = bb ? trackListJson( bb->tracks() )
							: QJsonArray();
		}
	}
	else if( cmd == "selftest" )
	{
		res = selfTest();
		res["ok"] = res["failed"].toInt() == 0;
		res["cmd"] = origName;
	}
	else if( cmd == "tempo" || cmd == "set_tempo" )
	{
		if( !song )
		{
			res = errReply( origName, "no song object" );
		}
		else
		{
			bool conv = false;
			const int bpm = arg.toInt( &conv );
			if( !conv || bpm < (int) MinTempo || bpm > (int) MaxTempo )
			{
				res = errReply( origName,
					QString( "tempo must be an int in %1..%2" )
						.arg( (int) MinTempo ).arg( (int) MaxTempo ) );
			}
			else
			{
				if( song->getTempo() != bpm ) { song->setTempo( bpm ); modified = true; }
				res = okReply( origName );
				res["tempo"] = (int) song->getTempo();
			}
		}
	}
	else if( cmd == "play" || cmd == "stop" )
	{
		if( !song )
		{
			res = errReply( origName, "no song object" );
		}
		else
		{
			if( cmd == "play" )
			{
				if( song->masterVolume() > m_composition->playbackVolumeCap() ) { song->setMasterVolume( m_composition->playbackVolumeCap() ); modified = true; }
				if( song->isPaused() && song->playMode() == Song::Mode_PlaySong ) { song->togglePause(); }
				else if( !song->isPlaying() || song->playMode() != Song::Mode_PlaySong ) { song->playSong(); }
			}
			else
			{
				song->stop();
			}
			res = okReply( origName );
			res["playing"] = song->isPlaying();
			res["paused"] = song->isPaused();
		}
	}
	else if( cmd == "show" )
	{
		m_plugin->showView();
		res = okReply( origName );
		res["visible"] = true;
	}
	else
	{
		res = m_composition->execute( cmd, args, modified );
		res["cmd"] = origName;
	}

	if( modified && song ) { song->setModified(); }
	if( !res.contains( "changed" ) ) { res["changed"] = modified; }
	const QJsonObject context = BridgeModel::handleContext();
	for( auto it = context.begin(); it != context.end(); ++it ) { res[it.key()] = it.value(); }
	if( song ) { res["modified"] = song->isModified(); }
	if( gui && gui->mainWindow() ) { res["windowTitle"] = gui->mainWindow()->windowTitle(); }
	if( !requestId.isUndefined() ) { res["id"] = requestId; }
	QByteArray out = toJson( res );
	m_plugin->log( QString( "result ok=%1: %2" )
		.arg( res["ok"].toBool() ? "true" : "false" )
		.arg( QString::fromUtf8( out.left( 200 ) ) ) );
	return out;
}


QJsonObject BridgeCommands::selfTest()
{
	QList<Check> checks;
	Check c;

	Song * song = Engine::getSong();
	Mixer * mixer = Engine::mixer();
	BBTrackContainer * bb = Engine::getBBTrackContainer();
	ConfigManager * cm = ConfigManager::inst();
	GuiApplication * g = gui;
	MainWindow * mw = g ? g->mainWindow() : NULL;

	// 1 qt.version
	{
		const bool pass = QString( qVersion() ) == "5.4.2" &&
						QString( QT_VERSION_STR ) == "5.4.2";
		c.name = "qt.version"; c.pass = pass;
		c.detail = QString( "runtime=%1 headers=%2" )
			.arg( qVersion(), QT_VERSION_STR );
		checks.append( c );
	}

	// 2 engine.objects
	{
		QStringList missing;
		if( !song )					{ missing << "song"; }
		if( !mixer )				{ missing << "mixer"; }
		if( !bb )					{ missing << "bbTrackContainer"; }
		if( !Engine::fxMixer() )	{ missing << "fxMixer"; }
		if( !g )					{ missing << "gui"; }
		if( g && !mw )				{ missing << "mainWindow"; }
		c.name = "engine.objects"; c.pass = missing.isEmpty();
		c.detail = missing.isEmpty() ? "all non-null"
			: "null: " + missing.join( "," );
		checks.append( c );
	}

	// 3 meta.song_class
	{
		bool pass = song &&
			QString( Song::staticMetaObject.className() ) == "Song" &&
			QString( song->metaObject()->className() ) == "Song" &&
			song->inherits( "TrackContainer" );
		c.name = "meta.song_class"; c.pass = pass;
		c.detail = song ? QString( "static=%1 dynamic=%2 inherits=%3" )
			.arg( Song::staticMetaObject.className() )
			.arg( song->metaObject()->className() )
			.arg( song->inherits( "TrackContainer" ) )
			: "no song";
		checks.append( c );
	}

	// 4 song.track_count
	{
		bool pass = song && song->tracks().size() == song->countTracks();
		c.name = "song.track_count"; c.pass = pass;
		c.detail = song ? QString( "tracks=%1 countTracks=%2" )
			.arg( song->tracks().size() ).arg( song->countTracks() )
			: "no song";
		checks.append( c );
	}

	// 5 bb.track_count
	{
		bool pass = bb && bb->tracks().size() == bb->countTracks();
		c.name = "bb.track_count"; c.pass = pass;
		c.detail = bb ? QString( "tracks=%1 countTracks=%2" )
			.arg( bb->tracks().size() ).arg( bb->countTracks() )
			: "no bb container";
		checks.append( c );
	}

	// 6 track.layout
	{
		QStringList bad;
		int nTracks = 0;
		TrackContainer * containers[2] = { song, bb };
		for( int ci = 0; ci < 2; ++ci )
		{
			TrackContainer * tc = containers[ci];
			if( !tc )
			{
				continue;
			}
			const TrackContainer::TrackList & tl = tc->tracks();
			for( TrackContainer::TrackList::const_iterator it =
					tl.begin(); it != tl.end(); ++it )
			{
				Track * t = *it;
				if( !t )
				{
					bad << "null track";
					continue;
				}
				++nTracks;
				if( t->trackContainer() != tc )
				{
					bad << t->name() + ":wrongContainer";
				}
				if( t->type() < 0 || t->type() >= Track::NumTrackTypes )
				{
					bad << t->name() + ":badType";
				}
				if( t->getTCOs().size() != t->numOfTCOs() )
				{
					bad << t->name() + ":tcoCount";
				}
				if( t->displayName() != t->name() )
				{
					bad << t->name() + ":displayName";
				}
			}
		}
		c.name = "track.layout"; c.pass = bad.isEmpty();
		c.detail = bad.isEmpty()
			? QString( "%1 tracks ok" ).arg( nTracks )
			: bad.join( "," );
		checks.append( c );
	}

	// 7 track.rtti
	{
		QStringList bad;
		TrackContainer * containers[2] = { song, bb };
		for( int ci = 0; ci < 2; ++ci )
		{
			TrackContainer * tc = containers[ci];
			if( !tc )
			{
				continue;
			}
			const TrackContainer::TrackList & tl = tc->tracks();
			for( TrackContainer::TrackList::const_iterator it =
					tl.begin(); it != tl.end(); ++it )
			{
				Track * t = *it;
				if( !t )
				{
					continue;
				}
				const Track::TrackTypes ty = t->type();
				const bool isInstr =
					dynamic_cast<InstrumentTrack *>( t ) != NULL;
				const bool isBB =
					dynamic_cast<BBTrack *>( t ) != NULL;
				const bool isSample =
					dynamic_cast<SampleTrack *>( t ) != NULL;
				const bool isAuto =
					dynamic_cast<AutomationTrack *>( t ) != NULL;
				if( ( ty == Track::InstrumentTrack ) != isInstr ||
					( ty == Track::BBTrack ) != isBB ||
					( ty == Track::SampleTrack ) != isSample ||
					( ( ty == Track::AutomationTrack ||
						ty == Track::HiddenAutomationTrack ) )
															!= isAuto )
				{
					bad << t->name();
				}
			}
		}
		c.name = "track.rtti"; c.pass = bad.isEmpty();
		c.detail = bad.isEmpty() ? "rtti consistent"
			: "mismatch: " + bad.join( "," );
		checks.append( c );
	}

	// 8 pattern.layout
	{
		int patterns = 0;
		int notes = 0;
		QStringList bad;
		TrackContainer * containers[2] = { song, bb };
		for( int ci = 0; ci < 2; ++ci )
		{
			TrackContainer * tc = containers[ci];
			if( !tc )
			{
				continue;
			}
			const TrackContainer::TrackList & tl = tc->tracks();
			for( TrackContainer::TrackList::const_iterator it =
					tl.begin(); it != tl.end(); ++it )
			{
				InstrumentTrack * itrk =
					dynamic_cast<InstrumentTrack *>( *it );
				if( !itrk )
				{
					continue;
				}
				const Track::tcoVector & tv = itrk->getTCOs();
				for( Track::tcoVector::const_iterator tco =
						tv.begin(); tco != tv.end(); ++tco )
				{
					Pattern * p =
						dynamic_cast<Pattern *>( *tco );
					if( !p )
					{
						bad << itrk->name() + ":nonPatternTCO";
						continue;
					}
					++patterns;
					if( p->instrumentTrack() != itrk ||
						p->getTrack() != itrk )
					{
						bad << itrk->name() + ":patternTrack";
					}
					const NoteVector & nv = p->notes();
					for( NoteVector::const_iterator nit =
							nv.begin(); nit != nv.end(); ++nit )
					{
						Note * n = *nit;
						if( !n )
						{
							bad << itrk->name() + ":nullNote";
							continue;
						}
						++notes;
						if( n->key() < 0 || n->key() >= NumKeys )
						{
							bad << itrk->name() + ":noteKey";
						}
						if( n->getVolume() < MinVolume ||
							n->getVolume() > MaxVolume )
						{
							bad << itrk->name() + ":noteVol";
						}
						if( n->getPanning() < PanningLeft ||
							n->getPanning() > PanningRight )
						{
							bad << itrk->name() + ":notePan";
						}
					}
				}
			}
		}
		c.name = "pattern.layout"; c.pass = song && bad.isEmpty();
		c.detail = song ? QString( "%1 patterns, %2 notes%3" )
			.arg( patterns ).arg( notes )
			.arg( bad.isEmpty() ? "" : " bad:" + bad.join( "," ) )
			: "no song";
		checks.append( c );
	}

	// 9 song.tempo (inline const model read vs exported non-const)
	{
		bool pass = song &&
			static_cast<const Song *>( song )->getTempo() ==
													song->getTempo();
		c.name = "song.tempo"; c.pass = pass;
		c.detail = song ? QString( "inline=%1 exported=%2" )
			.arg( static_cast<const Song *>( song )->getTempo() )
			.arg( song->getTempo() )
			: "no song";
		checks.append( c );
	}

	// 10 song.ticks_per_bar
	{
		bool pass = song &&
			song->ticksPerTact() == MidiTime::ticksPerTact();
		c.name = "song.ticks_per_bar"; c.pass = pass;
		c.detail = song ? QString( "song=%1 midiTime=%2" )
			.arg( song->ticksPerTact() )
			.arg( MidiTime::ticksPerTact() )
			: "no song";
		checks.append( c );
	}

	// 11 song.flags
	{
		bool pass = song &&
			song->playMode() >= 0 && song->playMode() < Song::Mode_Count &&
			( song->isExporting() ||
				song->isStopped() ==
					( !song->isPlaying() && !song->isPaused() ) );
		c.name = "song.flags"; c.pass = pass;
		c.detail = song ? QString( "playMode=%1 playing=%2 paused=%3 stopped=%4 exporting=%5" )
			.arg( (int) song->playMode() )
			.arg( song->isPlaying() )
			.arg( song->isPaused() )
			.arg( song->isStopped() )
			.arg( song->isExporting() )
			: "no song";
		checks.append( c );
	}

	// 12 song.window_title
	{
		QString prefix = song ?
			QFileInfo( song->projectFileName() ).completeBaseName()
			: QString();
		if( prefix.isEmpty() )
		{
			prefix = "Untitled";
		}
		QString title = mw ? mw->windowTitle() : QString();
		bool pass = song && mw &&
			title.startsWith( prefix ) &&
			( title.mid( prefix.length(), 1 ) == "*" ) ==
													song->isModified();
		c.name = "song.window_title"; c.pass = pass;
		c.detail = title;
		checks.append( c );
	}

	// 13 config.layout
	{
		QStringList bad;
		QString wdVal, stkVal, sfVal, artVal;
		if( cm )
		{
			wdVal = cm->value( "paths", "workingdir" );
			stkVal = cm->value( "paths", "stkdir" );
			sfVal = cm->value( "paths", "defaultsf2" );
			artVal = cm->value( "paths", "backgroundartwork" );
			if( !wdVal.isEmpty() &&
				cm->workingDir() !=
					ensureTrailingSlash( QDir::cleanPath( wdVal ) ) )
			{
				bad << "workingDir";
			}
			if( cm->stkDir() != ensureTrailingSlash( stkVal ) &&
				!cm->stkDir().endsWith( "stk/rawwaves/" ) )
			{
				bad << "stkDir";
			}
			if( cm->defaultSoundfont() != sfVal )
			{
				bad << "defaultSoundfont";
			}
			if( cm->backgroundArtwork() != artVal )
			{
				bad << "backgroundArtwork";
			}
		}
		c.name = "config.layout"; c.pass = cm && bad.isEmpty();
		c.detail = cm ? QString(
			"workingDir=%1 stkDir=%2 defaultSoundfont=%3 backgroundArtwork=%4%5" )
			.arg( cm->workingDir(), cm->stkDir(),
				cm->defaultSoundfont(), cm->backgroundArtwork() )
			.arg( bad.isEmpty() ? "" : " bad:" + bad.join( "," ) )
			: "no ConfigManager";
		checks.append( c );
	}

	// 14 mixer.frames_per_period
	{
		bool pass = mixer && cm;
		QString detail;
		if( mixer && cm )
		{
			const int v = cm->value( "mixer",
								"framesperaudiobuffer" ).toInt();
			const int expected = v < (int) MINIMUM_BUFFER_SIZE
				? (int) DEFAULT_BUFFER_SIZE
				: qMin( v, (int) DEFAULT_BUFFER_SIZE );
			pass = (int) mixer->framesPerPeriod() == expected;
			detail = QString( "config=%1 expected=%2 actual=%3" )
				.arg( v ).arg( expected )
				.arg( (int) mixer->framesPerPeriod() );
		}
		c.name = "mixer.frames_per_period"; c.pass = pass;
		c.detail = detail;
		checks.append( c );
	}

	// 15 gui.tools_menu
	{
		QString foundMenu;
		if( mw && mw->menuBar() )
		{
			const QString want =
				agentbridge_plugin_descriptor.displayName;
			const QList<QAction *> tops = mw->menuBar()->actions();
			for( QList<QAction *>::const_iterator it = tops.begin();
					it != tops.end() && foundMenu.isEmpty(); ++it )
			{
				QMenu * m = (*it)->menu();
				if( !m )
				{
					continue;
				}
				const QList<QAction *> acts = m->actions();
				for( QList<QAction *>::const_iterator ait =
						acts.begin(); ait != acts.end(); ++ait )
				{
					if( (*ait)->text() == want )
					{
						foundMenu = m->title();
						break;
					}
				}
			}
		}
		c.name = "gui.tools_menu"; c.pass = !foundMenu.isEmpty();
		c.detail = foundMenu.isEmpty() ? "action not found"
			: "menu=" + foundMenu;
		checks.append( c );
	}

	// 16 signals.connect
	{
		c.name = "signals.connect";
		c.pass = m_plugin->signalConnectionsOk();
		c.detail = m_plugin->signalConnectionsDetail();
		checks.append( c );
	}

	{
		QSet<AutomatableModel *> seen;
		QSet<jo_id_t> ids;
		int invalid = 0;
		for( AutomatableModel * model : BridgeModel::projectModels() )
		{
			if( seen.contains( model ) ) { continue; }
			seen.insert( model );
			if( !model->id() || ids.contains( model->id() ) ||
				dynamic_cast<AutomatableModel *>( Engine::projectJournal()->journallingObject( model->id() ) ) != model ) { ++invalid; }
			ids.insert( model->id() );
		}
		c.name = "composition.parameter_registry";
		c.pass = !seen.isEmpty() && !invalid;
		c.detail = QString( "%1 current parameters, %2 invalid registrations" ).arg( seen.size() ).arg( invalid );
		checks.append( c );
	}
	{
		QSet<QString> names;
		bool valid = true;
		for( const QJsonValue & tool : CompositionCommands::tools() )
		{
			const QJsonObject spec = tool.toObject();
			const QString name = spec["name"].toString();
			valid = valid && !name.isEmpty() && !names.contains( name ) && spec["inputSchema"].isObject() && spec["annotations"].toObject()["readOnlyHint"].isBool();
			names.insert( name );
		}
		c.name = "composition.tool_catalog";
		c.pass = valid && names.size() >= 100;
		c.detail = QString( "%1 unique native tool schemas" ).arg( names.size() );
		checks.append( c );
	}
	{
		const QJsonObject formats = BridgeRenderer::formats();
		c.name = "export.encoders";
		c.pass = formats["wav"].toBool();
		c.detail = QString::fromUtf8( toJson( formats ) );
		checks.append( c );
	}

	int passed = 0;
	int failed = 0;
	QJsonArray arr;
	for( QList<Check>::const_iterator it = checks.begin();
			it != checks.end(); ++it )
	{
		QJsonObject o;
		o["name"] = it->name;
		o["pass"] = it->pass;
		o["detail"] = it->detail;
		arr.append( o );
		if( it->pass )
		{
			++passed;
		}
		else
		{
			++failed;
		}
	}

	QJsonObject report;
	report["passed"] = passed;
	report["failed"] = failed;
	report["checks"] = arr;
	return report;
}
