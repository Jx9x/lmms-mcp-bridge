#include "CompositionCommands.h"
#include "CommandSchema.h"
#include "BridgeModel.h"
#include "BridgeRenderer.h"
#include "MusicTheory.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QMap>
#include <QtCore/QTemporaryDir>
#include <QtCore/QtEndian>
#include <QtXml/QDomDocument>

#include "AutomationPattern.h"
#include "AutomationTrack.h"
#include "BBTrack.h"
#include "BBTrackContainer.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "FxMixer.h"
#include "FxMixerView.h"
#include "GuiApplication.h"
#include "Instrument.h"
#include "MainWindow.h"
#include "PluginFactory.h"
#include "SampleBuffer.h"
#include "SampleTrack.h"
#include "Song.h"
#include "lmmsversion.h"

namespace
{

QJsonObject result( bool ok = true, const QString & error = QString() ) { return BridgeModel::reply( ok, error ); }

bool containsAny( const QJsonObject & args, const QStringList & fields )
{
	for( const QString & field : fields ) { if( args.contains( field ) ) { return true; } }
	return false;
}

QString projectGuard( Song * song, const QJsonObject & args )
{
	if( ( song->isModified() || ( gui && gui->mainWindow()->getSession() == MainWindow::Recover ) ) && !args["discard_unsaved"].toBool() )
	{
		return "unsaved project: save it first, or explicitly set discard_unsaved=true";
	}
	return QString();
}

QString absolutePath( const QString & path, QString & error )
{
	const QString local = QDir::fromNativeSeparators( path );
	if( path.isEmpty() || path.contains( QChar( 0 ) ) || !QRegExp( "^[A-Za-z]:/.*$" ).exactMatch( local ) || local.mid( 2 ).contains( ':' ) )
	{
		error = "path must be a nonempty absolute local file path";
		return QString();
	}
	return QDir::cleanPath( QDir::fromNativeSeparators( path ) );
}

QString readDocument( const QString & path, QDomDocument & doc, bool compressed )
{
	QFile input( path );
	if( !input.open( QIODevice::ReadOnly ) ) { return "cannot read file: " + input.errorString(); }
	if( input.size() > 64 * 1024 * 1024 ) { return "file exceeds the 64 MiB loading limit"; }
	QByteArray bytes = input.readAll();
	if( compressed )
	{
		if( bytes.size() < 4 || qFromBigEndian<quint32>( reinterpret_cast<const uchar *>( bytes.constData() ) ) > 64 * 1024 * 1024 ) { return "invalid or oversized compressed project"; }
		bytes = qUncompress( bytes );
	}
	QString message;
	int line = 0, column = 0;
	if( !doc.setContent( bytes, &message, &line, &column ) ) { return QString( "invalid XML at %1:%2: %3" ).arg( line ).arg( column ).arg( message ); }
	return QString();
}

QString projectDocument( const QString & path, QDomDocument & doc )
{
	const QString error = readDocument( path, doc, QFileInfo( path ).suffix().toLower() == "mmpz" );
	if( !error.isEmpty() ) { return error; }
	const QDomElement root = doc.documentElement();
	if( root.tagName() != "lmms-project" || ( root.attribute( "type" ) != "song" && root.attribute( "type" ) != "songtemplate" ) ||
		( root.firstChildElement( "song" ).isNull() && root.firstChildElement( "songtemplate" ).isNull() ) ) { return "file is not an LMMS song project"; }
	return QString();
}

QString presetInstrumentName( const QDomDocument & doc )
{
	const QDomElement track = doc.documentElement().firstChildElement( "instrumenttracksettings" ).firstChildElement( "instrumenttrack" );
	const QDomElement instrument = track.firstChildElement( "instrument" );
	if( !instrument.isNull() ) { return instrument.attribute( "name" ); }
	// Native InstrumentTrack::loadTrackSpecificSettings also accepts a direct
	// plugin element. DataFile deliberately retains this legacy preset layout.
	for( QDomElement child = track.firstChildElement(); !child.isNull(); child = child.nextSiblingElement() )
	{
		if( BridgeModel::pluginError( child.tagName(), Plugin::Instrument ).isEmpty() ) { return child.tagName(); }
	}
	return QString();
}

QString presetDocument( const QString & path, QDomDocument & doc )
{
	const QString error = readDocument( path, doc, false );
	if( !error.isEmpty() ) { return error; }
	const QDomElement root = doc.documentElement();
	if( ( root.tagName() != "lmms-project" && root.tagName() != "multimedia-project" && root.tagName() != "multimediaproject" ) || root.attribute( "type" ) != "instrumenttracksettings" ) { return "file is not an LMMS instrument preset"; }
	// Parsing and size checks precede DataFile so malformed input cannot open a
	// modal native error dialog. Its constructor performs the stock upgrades.
	DataFile upgraded( doc.toByteArray() );
	if( upgraded.type() != DataFile::InstrumentTrackSettings ) { return "file is not an LMMS instrument preset"; }
	doc = upgraded;
	if( upgraded.content().firstChildElement( "instrumenttrack" ).isNull() ) { return "preset has no upgraded instrument track"; }
	const QString name = presetInstrumentName( doc );
	return BridgeModel::pluginError( name, Plugin::Instrument );
}

QJsonArray pageList( const QJsonArray & entries, const QJsonObject & args, QJsonObject & pagination )
{
	QMap<QString, QJsonObject> ordered;
	const QString search = args["search"].toString();
	for( const QJsonValue & value : entries )
	{
		const QJsonObject entry = value.toObject();
		const QString path = entry["path"].toString( entry["name"].toString() );
		if( !search.isEmpty() && !path.contains( search, Qt::CaseInsensitive ) && !entry["name"].toString().contains( search, Qt::CaseInsensitive ) ) { continue; }
		ordered.insert( path.toLower() + path, entry );
	}
	QJsonArray page;
	const int offset = args["offset"].toInt(), limit = args["limit"].toInt( 5000 );
	int index = 0;
	for( auto it = ordered.begin(); it != ordered.end(); ++it, ++index )
	{
		if( index >= offset && page.size() < limit ) { page.append( it.value() ); }
	}
	pagination["offset"] = offset; pagination["limit"] = limit; pagination["total_count"] = ordered.size();
	pagination["next_offset"] = offset + page.size() < ordered.size() ? QJsonValue( offset + page.size() ) : QJsonValue();
	return page;
}

QJsonArray presetList( const QJsonObject & args, bool zyn, QJsonObject & pagination )
{
	QJsonArray presets;
	const QString instrument = zyn ? "zyn" : args["instrument"].toString();
	const QString category = args["category"].toString();
	const QStringList roots = { BridgeModel::resolvedSamplePath( ConfigManager::inst()->factoryPresetsDir() ), ConfigManager::inst()->userPresetsDir() };
	for( const QString & root : roots )
	{
		QDirIterator files( root, { "*.xpf", "*.xiz" }, QDir::Files, QDirIterator::Subdirectories );
		while( files.hasNext() && presets.size() < 5000 )
		{
			const QString path = files.next();
			const QString relative = QDir( root ).relativeFilePath( path );
			if( !category.isEmpty() && !relative.contains( category, Qt::CaseInsensitive ) ) { continue; }
			if( !instrument.isEmpty() && !relative.contains( instrument, Qt::CaseInsensitive ) ) { continue; }
			QJsonObject preset;
			preset["name"] = QFileInfo( path ).completeBaseName();
			preset["path"] = QFileInfo( path ).absoluteFilePath();
			preset["category"] = QFileInfo( relative ).path();
			if( QFileInfo( path ).suffix().toLower() == "xpf" )
			{
				QDomDocument doc;
				const QString error = presetDocument( path, doc );
				preset["compatible"] = error.isEmpty();
				if( !error.isEmpty() ) { preset["compatibility_error"] = error; }
				else { preset["instrument"] = presetInstrumentName( doc ); }
			}
			else { preset["compatible"] = QJsonValue(); preset["requires_instrument"] = "zynaddsubfx"; }
			presets.append( preset );
		}
	}
	return pageList( presets, args, pagination );
}

QJsonArray sampleList( const QJsonObject & args, QJsonObject & pagination )
{
	QJsonArray samples;
	const QStringList roots = { BridgeModel::resolvedSamplePath( ConfigManager::inst()->factorySamplesDir() ), ConfigManager::inst()->userSamplesDir() };
	for( const QString & root : roots )
	{
		QDirIterator files( root, { "*.wav", "*.ogg", "*.flac", "*.mp3", "*.aif", "*.aiff", "*.ds" }, QDir::Files, QDirIterator::Subdirectories );
		while( files.hasNext() && samples.size() < 10000 )
		{
			const QString path = files.next();
			const QString category = QFileInfo( QDir( root ).relativeFilePath( path ) ).path();
			if( !args["category"].toString().isEmpty() && !category.contains( args["category"].toString(), Qt::CaseInsensitive ) ) { continue; }
			QJsonObject sample;
			sample["name"] = QFileInfo( path ).completeBaseName();
			sample["path"] = QFileInfo( path ).absoluteFilePath();
			sample["resolved_file_path"] = QFileInfo( path ).absoluteFilePath();
			sample["category"] = category;
			sample["format"] = QFileInfo( path ).suffix().toLower();
			samples.append( sample );
		}
	}
	return pageList( samples, args, pagination );
}

QJsonArray mixerStates()
{
	QJsonArray states;
	for( int i = 0; i < Engine::fxMixer()->numChannels(); ++i )
	{
		FxChannel * channel = Engine::fxMixer()->effectChannel( i );
		QJsonObject state;
		state["channel_num"] = i; state["muted"] = channel->m_muteModel.value(); state["solo"] = channel->m_soloModel.value();
		states.append( state );
	}
	return states;
}

double parameterValue( const QJsonValue & value ) { return value.isBool() ? double( value.toBool() ) : value.toDouble(); }

QString pointError( AutomatableModel * model, const QJsonArray & points )
{
	QSet<int> positions;
	for( const QJsonValue & entry : points )
	{
		const QJsonObject point = entry.toObject();
		const QString error = BridgeModel::valueError( model, point["value"].toDouble() );
		if( !error.isEmpty() ) { return error; }
		const int pos = point["pos"].toInt();
		if( positions.contains( pos ) ) { return "automation point positions must be unique"; }
		positions.insert( pos );
	}
	return QString();
}

void putPoints( AutomationPattern * pattern, const QJsonArray & points, const QJsonObject & args, bool replace )
{
	if( replace ) { pattern->clear(); }
	for( const QJsonValue & entry : points )
	{
		const QJsonObject point = entry.toObject();
		pattern->putValue( MidiTime( point["pos"].toInt() ), pattern->firstObject()->inverseScaledValue( point["value"].toDouble() ), false, true );
	}
	if( args.contains( "progression" ) )
	{
		const QString type = args["progression"].toString();
		pattern->setProgressionType( type == "linear" ? AutomationPattern::LinearProgression : type == "cubic" ? AutomationPattern::CubicHermiteProgression : AutomationPattern::DiscreteProgression );
	}
	if( args.contains( "tension" ) ) { pattern->setTension( QString::number( args["tension"].toDouble() ) ); }
	if( int( pattern->length() ) < int( pattern->timeMapLength() ) ) { pattern->changeLength( pattern->timeMapLength() ); }
}

}

CompositionCommands::CompositionCommands() : QObject(), m_renderer( new BridgeRenderer( this ) ), m_playbackVolumeCap( 8 )
{
	QObject::connect( Engine::getSong(), &Song::projectLoaded, this, []() { BridgeModel::resetProjectHandles(); BridgeModel::prepareMixerSolo(); } );
}
CompositionCommands::~CompositionCommands() { delete m_renderer; }
bool CompositionCommands::isRendering() const { return m_renderer->busy(); }

QString CompositionCommands::validate( const QString & command, const QJsonObject & args )
{
	for( const QJsonValue & entry : tools() )
	{
		const QJsonObject spec = entry.toObject();
		if( spec["name"].toString() == command ) { return CommandSchema::validate( args, spec["inputSchema"].toObject() ); }
	}
	return "unknown command";
}

QJsonObject CompositionCommands::execute( const QString & command, const QJsonObject & args, bool & modified )
{
	using namespace BridgeModel;
	JsonDetail detail( args );
	prepareMixerSolo();
	Song * song = Engine::getSong();
	if( !song ) { return result( false, "no song object" ); }
	QString error;
	QJsonObject o = result();
	if( command == "render_status" )
	{
		const QJsonObject status = m_renderer->status();
		if( args.contains( "job_id" ) && status["job"].toObject()["job_id"] != args["job_id"] ) { return result( false, "job_id is not the current render job" ); }
		return status;
	}
	if( command == "cancel_render" ) { return m_renderer->cancel(); }
	if( command == "render_project" || command == "export_midi" )
	{
		QJsonObject options = args;
		options["path"] = absolutePath( args["path"].toString(), error );
		if( !error.isEmpty() ) { return result( false, error ); }
		return command == "render_project" ? m_renderer->start( options ) : m_renderer->exportMidi( options );
	}
	if( command == "project.new" || command == "create_project" )
	{
		error = projectGuard( song, args );
		if( !error.isEmpty() ) { return result( false, error ); }
		song->createNewProject();
		resetProjectHandles();
		if( command == "create_project" && args["empty"].toBool( true ) ) { song->clearProject(); modified = true; }
		if( args.contains( "bpm" ) ) { song->setTempo( args["bpm"].toInt() ); modified = true; }
		if( song->masterVolume() > m_playbackVolumeCap ) { song->setMasterVolume( m_playbackVolumeCap ); modified = true; }
		o["changed"] = true;
		o["file"] = song->projectFileName();
		o["name"] = args["name"].toString();
	}
	else if( command == "load_project" )
	{
		error = projectGuard( song, args );
		if( !error.isEmpty() ) { return result( false, error ); }
		const QString path = absolutePath( args["path"].toString(), error );
		if( !error.isEmpty() ) { return result( false, error ); }
		const QString extension = QFileInfo( path ).suffix().toLower();
		if( extension != "mmp" && extension != "mmpz" ) { return result( false, "project extension must be .mmp or .mmpz" ); }
		QDomDocument doc;
		error = projectDocument( path, doc );
		if( !error.isEmpty() ) { return result( false, error ); }
		song->loadProject( path );
		if( song->isCancelled() ) { modified = true; return result( false, "project loading was cancelled; inspect the current project" ); }
		if( song->masterVolume() > m_playbackVolumeCap ) { song->setMasterVolume( m_playbackVolumeCap ); modified = true; }
		o["changed"] = true;
		o["path"] = song->projectFileName();
		if( song->hasErrors() ) { o["warnings"] = song->errorSummary(); }
	}
	else if( command == "save_project" )
	{
		const QString requested = args["path"].toString( song->projectFileName() );
		const QString path = absolutePath( requested, error );
		if( !error.isEmpty() ) { return result( false, error ); }
		const QString extension = QFileInfo( path ).suffix();
		if( extension != "mmp" && extension != "mmpz" ) { return result( false, "save path must end in .mmp or .mmpz" ); }
		const QFileInfo info( path );
		if( !QDir( info.absolutePath() ).exists() ) { return result( false, "save directory does not exist" ); }
		const bool current = QDir::cleanPath( song->projectFileName() ).compare( path, Qt::CaseInsensitive ) == 0;
		if( info.exists() && !current && !args["overwrite"].toBool() ) { return result( false, "destination exists: set overwrite=true to replace it" ); }
		if( info.exists() && !info.isFile() ) { return result( false, "save destination is not a regular file" ); }
		if( QFileInfo( path + ".new" ).exists() ) { return result( false, "LMMS save sidecar .new already exists; leave it untouched" ); }
		{
			QSaveFile probe( path );
			if( !probe.open( QIODevice::WriteOnly ) ) { return result( false, "cannot write project: " + probe.errorString() ); }
			probe.cancelWriting();
		}
		if( !song->guiSaveProjectAs( path ) || QFileInfo( path ).size() <= 0 ) { return result( false, "LMMS could not save the project" ); }
		o["path"] = song->projectFileName();
		o["bytes"] = double( QFileInfo( path ).size() );
	}
	else if( command == "get_project_xml" )
	{
		QTemporaryDir directory;
		if( !directory.isValid() ) { return result( false, "cannot create a temporary project snapshot" ); }
		const QString path = directory.path() + "/snapshot.mmp";
		if( !song->saveProjectFile( path ) ) { return result( false, "LMMS could not serialize the project" ); }
		QFile file( path );
		if( !file.open( QIODevice::ReadOnly ) ) { return result( false, file.errorString() ); }
		o["xml"] = QString::fromUtf8( file.readAll() );
	}
	else if( command == "get_lmms_info" || command == "list_available_plugins" || command == "list_effect_plugins" )
	{
		o["lmms_version"] = LMMS_VERSION;
		o["qt_runtime"] = qVersion();
		o["instruments"] = plugins( Plugin::Instrument );
		o["effects"] = plugins( Plugin::Effect );
		o["factory_presets"] = resolvedSamplePath( ConfigManager::inst()->factoryPresetsDir() );
		o["factory_samples"] = resolvedSamplePath( ConfigManager::inst()->factorySamplesDir() );
		o["user_samples"] = resolvedSamplePath( ConfigManager::inst()->userSamplesDir() );
		o["user_presets"] = ConfigManager::inst()->userPresetsDir();
		o["ticks_per_quarter"] = 48;
		o["ticks_per_bar"] = MidiTime::ticksPerTact();
		o["midi_key_min"] = 12;
		o["midi_key_max"] = 119;
		o["sample_track_mixer_routing"] = "master only in LMMS 1.2.2";
		o["audio_export_formats"] = BridgeRenderer::formats();
		o["midi_export_available"] = pluginError( "midiexport", Plugin::ExportFilter ).isEmpty();
		o["render_quality"] = "native mixer, sinc-best sample interpolation, no oversampling";
		o["playback_volume_cap"] = m_playbackVolumeCap;
		QJsonObject capabilities;
		capabilities["sample_clip_name_persistence"] = false;
		capabilities["stable_handles"] = "project_token scoped; survive index changes, expire on reload/new/delete";
		capabilities["batch_preflight"] = true;
		capabilities["vst_parameters"] = "native VeSTige normalized models; vendor/version unavailable through the public ABI";
		o["capabilities"] = capabilities;
	}
	else if( command == "pause" || command == "set_playhead" || command == "transport" )
	{
		const QString action = command == "transport" ? args["action"].toString() : command == "pause" ? "pause" : "seek";
		if( command == "transport" && ( ( action == "seek" ) != args.contains( "position" ) ) ) { return result( false, "position is required only for action=seek" ); }
		if( action == "play" )
		{
			if( song->masterVolume() > m_playbackVolumeCap ) { song->setMasterVolume( m_playbackVolumeCap ); modified = true; o["volume_capped"] = true; }
			if( song->isPaused() && song->playMode() == Song::Mode_PlaySong ) { song->togglePause(); }
			else if( !song->isPlaying() || song->playMode() != Song::Mode_PlaySong ) { song->playSong(); }
		}
		else if( action == "stop" ) { song->stop(); }
		else if( action == "pause" ) { if( song->isPlaying() ) { song->togglePause(); } }
		else
		{
			Change change;
			Song::PlayPos & position = song->getPlayPos( Song::Mode_PlaySong );
			position.setTicks( args["position"].toInt() );
			position.setCurrentFrame( 0 );
			position.setJumped( true );
			song->setMilliSeconds( position.getTicks() * 60000.0 / ( 48 * song->getTempo() ) );
			song->playbackPositionChanged();
			song->updateSampleTracks();
		}
		o["playing"] = song->isPlaying();
		o["paused"] = song->isPaused();
		o["position"] = int( song->getPlayPos( Song::Mode_PlaySong ) );
	}
	else if( command == "set_time_signature" || command == "set_master_volume" || command == "set_master_pitch" || command == "update_project" )
	{
		const bool update = command == "update_project";
		if( update && !containsAny( args, { "bpm", "numerator", "denominator", "master_volume", "master_pitch", "playback_volume_cap" } ) ) { return result( false, "provide at least one project property" ); }
		const bool meter = command == "set_time_signature" || ( update && containsAny( args, { "numerator", "denominator" } ) );
		if( meter && ( !args.contains( "numerator" ) || !args.contains( "denominator" ) ) ) { return result( false, "provide numerator and denominator together" ); }
		const int denominator = args["denominator"].toInt();
		if( meter && ( denominator & ( denominator - 1 ) ) ) { return result( false, "time-signature denominator must be 1, 2, 4, 8, 16, or 32" ); }
		if( meter && ( song->getTimeSigModel().getNumerator() != args["numerator"].toInt() || song->getTimeSigModel().getDenominator() != denominator ) )
		{
			song->getTimeSigModel().setNumerator( args["numerator"].toInt() );
			song->getTimeSigModel().setDenominator( denominator );
			modified = true;
		}
		if( command == "set_master_volume" && song->masterVolume() != args["volume"].toInt() ) { song->setMasterVolume( args["volume"].toInt() ); modified = true; }
		if( command == "set_master_pitch" && song->masterPitch() != args["pitch"].toInt() ) { song->setMasterPitch( args["pitch"].toInt() ); modified = true; }
		if( update && args.contains( "bpm" ) && song->getTempo() != args["bpm"].toInt() ) { song->setTempo( args["bpm"].toInt() ); modified = true; }
		if( update && args.contains( "master_volume" ) && song->masterVolume() != args["master_volume"].toInt() ) { song->setMasterVolume( args["master_volume"].toInt() ); modified = true; }
		if( update && args.contains( "master_pitch" ) && song->masterPitch() != args["master_pitch"].toInt() ) { song->setMasterPitch( args["master_pitch"].toInt() ); modified = true; }
		if( update && args.contains( "playback_volume_cap" ) ) { o["changed"] = modified || m_playbackVolumeCap != args["playback_volume_cap"].toInt(); m_playbackVolumeCap = args["playback_volume_cap"].toInt(); }
		o["playback_volume_cap"] = m_playbackVolumeCap;
		o["ticks_per_bar"] = MidiTime::ticksPerTact();
	}
	else if( command == "list_tracks" || command == "get_arrangement" )
	{
		QJsonArray tracks;
		const auto & list = container( args )->tracks();
		for( int i = 0; i < list.size(); ++i ) { tracks.append( trackJson( list[i], i ) ); }
		o["tracks"] = tracks;
	}
	else if( command == "add_instrument_track" || command == "add_sample_track" || command == "add_automation_track" || command == "add_pattern_track" || command == "add_vst_track" || command == "create_track" )
	{
		const QString kind = command == "create_track" ? args["type"].toString() : command == "add_sample_track" ? "sample" : command == "add_automation_track" ? "automation" : command == "add_pattern_track" ? "bb" : "instrument";
		const bool isInstrument = kind == "instrument";
		if( command == "create_track" && !isInstrument && containsAny( args, { "instrument", "volume", "panning", "fx_channel" } ) ) { return result( false, "instrument options require type=instrument" ); }
		const QString plugin = command == "add_vst_track" ? "vestige" : args["instrument"].toString( "tripleoscillator" );
		if( isInstrument ) { error = pluginError( plugin, Plugin::Instrument ); }
		QString path;
		if( command == "add_vst_track" && error.isEmpty() ) { path = vstPath( args["plugin_path"].toString(), error ); }
		const int channel = args["fx_channel"].toInt();
		if( isInstrument && channel >= Engine::fxMixer()->numChannels() ) { error = "fx_channel is outside the current mixer"; }
		if( kind == "bb" && container( args ) != song ) { error = "BB arrangement tracks can only be created in the song container"; }
		if( !error.isEmpty() ) { return result( false, error ); }
		if( container( args )->tracks().size() >= 4096 ) { return result( false, "track limit reached" ); }
		Change change;
		const Track::TrackTypes type = isInstrument ? Track::InstrumentTrack : kind == "sample" ? Track::SampleTrack : kind == "automation" ? Track::AutomationTrack : Track::BBTrack;
		Track * t = Track::create( type, container( args ) );
		modified = true;
		if( InstrumentTrack * it = dynamic_cast<InstrumentTrack *>( t ) )
		{
			it->loadInstrument( plugin );
			if( !it->instrument() || QString::fromUtf8( it->instrument()->descriptor()->name ) != plugin ) { return result( false, "native instrument creation failed; inspect the created track" ); }
			if( !path.isEmpty() )
			{
				QDomDocument doc; QDomElement state = doc.createElement( "vestige" ); state.setAttribute( "plugin", path );
				// loadSettings builds native VST automation models; loadFile alone
				// loads audio but leaves parameter discovery empty.
				it->instrument()->restoreState( state );
			}
			it->volumeModel()->setValue( args["volume"].toDouble( 100 ) );
			it->panningModel()->setValue( args["panning"].toDouble() );
			it->effectChannelModel()->setValue( channel );
		}
		if( args.contains( "name" ) ) { t->setName( args["name"].toString() ); }
		o["track"] = trackJson( t, t->trackContainer()->tracks().indexOf( t ) );
		o["track_index"] = t->trackContainer()->tracks().indexOf( t );
		if( command == "add_vst_track" )
		{
			const QJsonObject status = vstStatus( static_cast<InstrumentTrack *>( t ) );
			o["vst"] = status;
			if( !status["plugin_loaded"].toBool() ) { o["ok"] = false; o["error"] = status["error"]; }
		}
	}
	else if( command == "get_track" || command == "remove_track" || command == "clone_track" || command == "rename_track" || command == "set_track_volume" || command == "set_track_panning" || command == "mute_track" || command == "solo_track" || command == "update_track" )
	{
		Track * t = track( args, error );
		if( !t ) { return result( false, error ); }
		if( command == "get_track" ) { o["track"] = trackJson( t, t->trackContainer()->tracks().indexOf( t ) ); return o; }
		const bool update = command == "update_track";
		if( update && !containsAny( args, { "name", "volume", "panning", "muted", "solo", "fx_channel" } ) ) { return result( false, "provide at least one track property" ); }
		const bool setVolume = command == "set_track_volume" || ( update && args.contains( "volume" ) );
		const bool setPanning = command == "set_track_panning" || ( update && args.contains( "panning" ) );
		AutomatableModel * volume = setVolume ? trackVolume( t ) : NULL;
		AutomatableModel * panning = setPanning ? trackPanning( t ) : NULL;
		if( setVolume ) { error = valueError( volume, args["volume"].toDouble() ); if( !error.isEmpty() ) { return result( false, error ); } }
		if( setPanning ) { error = valueError( panning, args["panning"].toDouble() ); if( !error.isEmpty() ) { return result( false, error ); } }
		InstrumentTrack * routed = dynamic_cast<InstrumentTrack *>( t );
		if( update && args.contains( "fx_channel" ) )
		{
			if( !routed ) { return result( false, "mixer routing requires an instrument track; sample tracks route to master only" ); }
			if( args["fx_channel"].toInt() >= Engine::fxMixer()->numChannels() ) { return result( false, "fx_channel is outside the current mixer" ); }
		}
		Change change;
		if( command == "remove_track" ) { error = removeTrack( t ); modified = error.isEmpty(); return modified ? o : result( false, error ); }
		if( command == "clone_track" )
		{
			Track * source = t;
			t = t->clone();
			for( int i = 0; i < qMin( source->getTCOs().size(), t->getTCOs().size() ); ++i ) { t->getTCOs()[i]->setName( source->getTCOs()[i]->name() ); }
			modified = true;
		}
		if( args.contains( "name" ) && t->name() != args["name"].toString() ) { t->setName( args["name"].toString() ); modified = true; }
		if( command == "mute_track" || ( update && args.contains( "muted" ) ) ) { modified |= setModelValue( t->getMutedModel(), args["muted"].toBool( true ) ); }
		if( ( command == "solo_track" || ( update && args.contains( "solo" ) ) ) && t->isSolo() != args["solo"].toBool( true ) ) { t->setSolo( args["solo"].toBool( true ) ); modified = true; }
		if( setVolume ) { modified |= setModelValue( volume, args["volume"].toDouble() ); }
		if( setPanning ) { modified |= setModelValue( panning, args["panning"].toDouble() ); }
		if( update && args.contains( "fx_channel" ) ) { modified |= setModelValue( routed->effectChannelModel(), args["fx_channel"].toInt() ); }
		o["track_index"] = t->trackContainer()->tracks().indexOf( t );
		o["track"] = trackJson( t, t->trackContainer()->tracks().indexOf( t ) );
	}
	else if( command == "load_instrument" || command == "set_instrument_settings" || command == "assign_track_to_mixer" )
	{
		InstrumentTrack * it = instrument( args, error );
		if( !it ) { return result( false, error ); }
		const int channel = args["channel_num"].toInt( args["fx_channel"].toInt() );
		if( ( args.contains( "fx_channel" ) || args.contains( "channel_num" ) ) && channel >= Engine::fxMixer()->numChannels() ) { return result( false, "mixer channel is outside the current mixer" ); }
		if( command == "load_instrument" ) { error = pluginError( args["instrument"].toString(), Plugin::Instrument ); }
		if( args.contains( "pitch_range" ) ) { error = valueError( it->pitchRangeModel(), args["pitch_range"].toDouble() ); }
		const int range = args["pitch_range"].toInt( it->midiPitchRange() );
		if( args.contains( "pitch" ) && qAbs( args["pitch"].toDouble() ) > range * 100 ) { error = "pitch exceeds the selected pitch_range in cents"; }
		if( !error.isEmpty() ) { return result( false, error ); }
		Change change;
		if( command == "load_instrument" ) { const QString name = it->name(); it->loadInstrument( args["instrument"].toString() ); it->setName( name ); modified = true; }
		if( args.contains( "base_note" ) ) { modified |= setModelValue( it->baseNoteModel(), args["base_note"].toInt() - 12 ); }
		if( args.contains( "pitch_range" ) ) { modified |= setModelValue( it->pitchRangeModel(), range ); }
		if( args.contains( "pitch" ) ) { modified |= setModelValue( it->pitchModel(), args["pitch"].toDouble() ); }
		if( args.contains( "fx_channel" ) || args.contains( "channel_num" ) ) { modified |= setModelValue( it->effectChannelModel(), channel ); }
		o["track"] = trackJson( it, it->trackContainer()->tracks().indexOf( it ) );
	}
	else if( command == "get_parameters" )
	{
		const QString type = args["target_type"].toString( "track" );
		QObject * root = song;
		if( type == "track" ) { root = track( args, error ); }
		else if( type == "instrument" ) { InstrumentTrack * it = instrument( args, error ); root = it ? it->instrument() : NULL; }
		else if( type == "mixer" || type == "effect" )
		{
			QJsonObject target = args;
			target["target_type"] = type == "mixer" ? "mixer" : args["effect_target_type"].toString( "track" );
			EffectChain * chain = effectChain( target, error );
			root = chain;
			if( type == "effect" && chain )
			{
				const auto ordered = effects( chain );
				const int index = args["effect_index"].toInt();
				if( index >= ordered.size() ) { error = "effect_index is outside the current chain"; }
				else { root = ordered[index]; }
			}
		}
		if( !error.isEmpty() || !root ) { return result( false, error.isEmpty() ? "parameter target is unavailable" : error ); }
		QJsonArray parameters = parametersJson( root );
		if( type == "mixer" )
		{
			FxChannel * channel = Engine::fxMixer()->effectChannel( args["target_index"].toInt() );
			AutomatableModel * channelModels[] = { &channel->m_volumeModel, &channel->m_muteModel, &channel->m_soloModel };
			for( AutomatableModel * m : channelModels ) { parameters.append( parameterJson( m ) ); }
		}
		o["parameters"] = parameters;
		if( type == "instrument" )
		{
			InstrumentTrack * it = instrument( args, error );
			const QJsonObject vst = vstStatus( it ); if( !vst.isEmpty() ) { o["vst"] = vst; }
		}
	}
	else if( command == "set_parameter" || command == "set_parameters" )
	{
		const QJsonArray edits = command == "set_parameter" ? QJsonArray() << args : args["values"].toArray();
		QVector<AutomatableModel *> targets;
		for( const QJsonValue & entry : edits )
		{
			const QJsonObject edit = entry.toObject();
			AutomatableModel * m = model( edit["model_id"].toDouble(), error );
			if( m ) { error = valueError( m, parameterValue( edit["value"] ) ); }
			if( !error.isEmpty() ) { return result( false, error ); }
			if( targets.contains( m ) ) { return result( false, "parameter batch contains duplicate model_ids" ); }
			targets.append( m );
		}
		Change change;
		QJsonArray applied;
		for( int i = 0; i < targets.size(); ++i ) { modified |= setModelValue( targets[i], parameterValue( edits[i].toObject()["value"] ) ); applied.append( parameterJson( targets[i] ) ); }
		o["parameters"] = applied;
	}
	else if( command == "list_presets" || command == "list_zyn_presets" ) { QJsonObject page; o["presets"] = presetList( args, command == "list_zyn_presets", page ); o["pagination"] = page; }
	else if( command == "list_samples" ) { QJsonObject page; o["samples"] = sampleList( args, page ); o["pagination"] = page; }
	else if( command == "load_preset" || command == "load_zyn_preset" || command == "load_instrument_file" )
	{
		InstrumentTrack * it = instrument( args, error );
		if( !it ) { return result( false, error ); }
		const QString path = absolutePath( args[command == "load_zyn_preset" ? "preset" : "path"].toString(), error );
		if( !error.isEmpty() ) { return result( false, error ); }
		if( !QFileInfo( path ).isFile() || !QFileInfo( path ).isReadable() ) { return result( false, "instrument file is not readable" ); }
		const bool preset = QFileInfo( path ).suffix().toLower() == "xpf";
		QDomDocument doc;
		if( preset ) { error = presetDocument( path, doc ); }
		else if( !it->instrument() || !it->instrument()->descriptor()->supportsFileType( QFileInfo( path ).suffix().toLower() ) ) { error = "current instrument does not support this file extension"; }
		if( command == "load_preset" && !preset ) { error = "load_preset requires an .xpf file"; }
		if( command == "load_zyn_preset" && !preset && QFileInfo( path ).suffix().toLower() != "xiz" ) { error = "Zyn preset must be .xpf or .xiz"; }
		if( !error.isEmpty() ) { return result( false, error ); }
		if( it->instrument() && QString::fromUtf8( it->instrument()->descriptor()->name ) == "audiofileprocessor" && !preset ) { audioPath( path, error ); if( !error.isEmpty() ) { return result( false, error ); } }
		Change change;
		const QString name = it->name();
		const float volume = it->volumeModel()->value(), pan = it->panningModel()->value();
		const int route = it->effectChannelModel()->value(), base = it->baseNoteModel()->value(), range = it->pitchRangeModel()->value();
		const float pitch = it->pitchModel()->value();
		modified = true;
		if( preset ) { DataFile file( doc.toByteArray() ); InstrumentTrack::removeMidiPortNode( file ); it->setSimpleSerializing(); it->loadSettings( file.content() ); }
		else if( it->instrument()->nodeName() == "vestige" )
		{
			QDomDocument stateDoc; QDomElement state = stateDoc.createElement( "vestige" ); state.setAttribute( "plugin", path );
			it->instrument()->restoreState( state );
		}
		else { it->instrument()->loadFile( path ); }
		if( args["preserve_name"].toBool( true ) ) { it->setName( name ); }
		if( args["preserve_mix"].toBool( true ) )
		{
			it->volumeModel()->setValue( volume ); it->panningModel()->setValue( pan ); it->effectChannelModel()->setValue( route );
		}
		if( args["preserve_instrument_settings"].toBool( true ) )
		{
			it->baseNoteModel()->setValue( base ); it->pitchRangeModel()->setValue( range ); it->pitchModel()->setValue( pitch );
		}
		QJsonArray sideEffects;
		if( it->name() != name ) { sideEffects.append( "name" ); }
		if( it->volumeModel()->value() != volume ) { sideEffects.append( "volume" ); }
		if( it->panningModel()->value() != pan ) { sideEffects.append( "panning" ); }
		if( it->effectChannelModel()->value() != route ) { sideEffects.append( "fx_channel" ); }
		if( it->baseNoteModel()->value() != base ) { sideEffects.append( "base_note" ); }
		if( it->pitchModel()->value() != pitch || it->pitchRangeModel()->value() != range ) { sideEffects.append( "pitch" ); }
		o["side_effects"] = sideEffects;
		if( it->instrument() && it->instrument()->nodeName() == "vestige" && !vstStatus( it )["plugin_loaded"].toBool() ) { o["ok"] = false; o["error"] = "VeSTige failed to load the requested external plugin"; }
		o["track"] = trackJson( it, it->trackContainer()->tracks().indexOf( it ) );
	}
	else if( command == "set_zyn_params" )
	{
		InstrumentTrack * it = instrument( args, error );
		if( !it || !it->instrument() ) { return result( false, error ); }
		if( QString::fromUtf8( it->instrument()->descriptor()->name ) != "zynaddsubfx" ) { return result( false, "track is not a ZynAddSubFX instrument" ); }
		const QMap<QString, QString> names = { { "portamento", "Portamento" }, { "filterfreq", "Filter Frequency" }, { "filterq", "Filter Resonance" }, { "bandwidth", "Bandwidth" }, { "fm_gain", "FM Gain" }, { "res_centerfreq", "Resonance Center Frequency" }, { "res_bandwidth", "Resonance Bandwidth" } };
		QVector<QPair<AutomatableModel *, double> > edits;
		for( auto field = names.begin(); field != names.end(); ++field )
		{
			if( !args.contains( field.key() ) ) { continue; }
			AutomatableModel * m = namedModel( it->instrument(), "ZynAddSubFxInstrument", field.value() );
			error = valueError( m, args[field.key()].toDouble() );
			if( !error.isEmpty() ) { return result( false, error ); }
			edits.append( qMakePair( m, args[field.key()].toDouble() ) );
		}
		Change change;
		for( const auto & edit : edits ) { modified |= setModelValue( edit.first, edit.second ); }
		o["parameters"] = parametersJson( it->instrument() );
	}
	else if( command == "scan_vst_directory" )
	{
		const QString path = absolutePath( args["directory"].toString(), error );
		if( !error.isEmpty() || !QDir( path ).exists() ) { return result( false, "VST directory must be an existing absolute directory" ); }
		QJsonArray files;
		QDirIterator iterator( path, { "*.dll" }, QDir::Files, args["recursive"].toBool( true ) ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags );
		while( iterator.hasNext() && files.size() < 5000 )
		{
			const QString candidate = iterator.next();
			QString reason;
			if( vstPath( candidate, reason ).isEmpty() ) { continue; }
			QJsonObject file;
			file["path"] = candidate;
			file["name"] = QFileInfo( candidate ).completeBaseName();
			file["verified_vst"] = false;
			files.append( file );
		}
		QJsonObject page; o["candidates"] = pageList( files, args, page ); o["pagination"] = page;
	}
	else if( command == "get_pattern" || command == "get_notes" || command == "add_note" || command == "add_note_by_name" || command == "add_notes_batch" || command == "set_note" || command == "set_notes_batch" || command == "replace_notes" || command == "remove_note" || command == "clear_notes" || command == "transpose_pattern" )
	{
		const bool adding = command == "add_note" || command == "add_note_by_name" || command == "add_notes_batch";
		const bool replacing = command == "replace_notes", updating = command == "set_note" || command == "set_notes_batch";
		NoteTarget target = noteTarget( args, error, adding || replacing );
		if( !error.isEmpty() ) { return result( false, error ); }
		Pattern * p = target.pattern;
		if( command == "get_pattern" ) { o["pattern"] = clipJson( p, target.index ); return o; }
		if( command == "get_notes" )
		{
			QJsonArray notes;
			for( int i = 0; i < p->notes().size(); ++i ) { if( args["include_disabled"].toBool() || p->notes()[i]->getVolume() > 0 ) { notes.append( noteJson( p->notes()[i], i, true ) ); } }
			o["notes"] = notes;
			o["pattern_index"] = target.index;
			return o;
		}
		const int noteIndex = args["note_index"].toInt();
		if( ( command == "set_note" || command == "remove_note" ) && ( noteIndex >= p->notes().size() ) ) { return result( false, "note_index is outside the current pattern" ); }
		QVector<NoteData> data;
		QVector<Note *> updated;
		if( adding || updating || replacing )
		{
			const QJsonArray entries = command == "add_notes_batch" || replacing ? args["notes"].toArray() : command == "set_notes_batch" ? args["edits"].toArray() : QJsonArray() << args;
			QSet<int> indices;
			for( const QJsonValue & entry : entries )
			{
				const QJsonObject edit = entry.toObject();
				const int index = edit["note_index"].toInt();
				if( updating && ( index >= p->notes().size() || indices.contains( index ) ) ) { return result( false, "batch note indices must be current and unique" ); }
				if( updating && !containsAny( edit, { "key", "note_name", "lmms_key", "pos", "length", "volume", "panning" } ) ) { return result( false, "provide at least one note property" ); }
				indices.insert( index );
				data.append( noteData( edit, error, updating ? p->notes()[index] : NULL ) );
				if( !error.isEmpty() ) { return result( false, error ); }
				if( updating ) { updated.append( p->notes()[index] ); }
			}
		}
		if( command == "transpose_pattern" )
		{
			for( const Note * n : p->notes() ) { if( n->key() + args["semitones"].toInt() < 0 || n->key() + args["semitones"].toInt() >= NumKeys ) { return result( false, "transposition would move a note outside LMMS's range" ); } }
		}
		Change change;
		if( adding || replacing ) { if( !p ) { modified = true; } p = ensurePattern( target ); }
		const int previousLength = int( p->length() );
		QVector<Note *> inserted;
		if( adding || replacing )
		{
			bool same = replacing && p->notes().size() == data.size();
			for( int i = 0; same && i < data.size(); ++i )
			{
				const Note * note = p->notes()[i]; const NoteData & n = data[i];
				same = note->key() == n.key && int( note->pos() ) == n.pos && int( note->length() ) == n.length && note->getVolume() == n.volume && note->getPanning() == n.panning;
			}
			if( !same )
			{
				p->addJournalCheckPoint();
				if( replacing ) { p->clearNotes(); }
				for( const NoteData & n : data ) { inserted.append( p->addNote( Note( MidiTime( n.length ), MidiTime( n.pos ), n.key, n.volume, n.panning ), false ) ); }
				modified = true;
			}
		}
		else if( updating )
		{
			for( int i = 0; i < data.size(); ++i )
			{
				Note * note = updated[i]; const NoteData & n = data[i];
				if( note->key() == n.key && int( note->pos() ) == n.pos && int( note->length() ) == n.length && note->getVolume() == n.volume && note->getPanning() == n.panning ) { continue; }
				if( !modified ) { p->addJournalCheckPoint(); }
				note->setKey( n.key ); note->setPos( MidiTime( n.pos ) ); note->setLength( MidiTime( n.length ) ); note->setVolume( n.volume ); note->setPanning( n.panning );
				inserted.append( note ); modified = true;
			}
		}
		else if( command == "remove_note" ) { p->addJournalCheckPoint(); p->removeNote( p->notes()[noteIndex] ); modified = true; o["removed_note_index"] = noteIndex; }
		else if( command == "clear_notes" && !p->notes().isEmpty() ) { p->addJournalCheckPoint(); p->clearNotes(); modified = true; }
		else if( command == "transpose_pattern" && args["semitones"].toInt() != 0 && !p->notes().isEmpty() )
		{
			p->addJournalCheckPoint();
			for( Note * n : p->notes() ) { n->setKey( n->key() + args["semitones"].toInt() ); inserted.append( n ); }
			modified = true;
		}
		if( modified ) { refreshPattern( p, previousLength ); }
		QJsonArray added;
		for( Note * note : inserted ) { added.append( noteJson( note, p->notes().indexOf( note ), true ) ); }
		o["notes"] = added;
		o["pattern_index"] = target.index;
		o["clip_handle"] = objectHandle( p, "clip" );
		o["note_count"] = p->notes().size();
		if( args["include_pattern"].toBool( true ) ) { o["pattern"] = clipJson( p, target.index ); }
	}
	else if( command == "place_patterns_batch" )
	{
		struct Placement { Track * track; Pattern * source; int pos, length; QJsonObject args; };
		QVector<Placement> placements;
		for( const QJsonValue & value : args["placements"].toArray() )
		{
			const QJsonObject entry = value.toObject();
			Track * t = track( entry, error );
			if( !t || !dynamic_cast<InstrumentTrack *>( t ) || t->trackContainer() != song ) { return result( false, error.isEmpty() ? "batch placement requires song instrument tracks" : error ); }
			const int pos = ticks( entry, "pos_ticks", "pos_bars", error );
			const int length = ticks( entry, "length_ticks", "length_bars", error, MidiTime::ticksPerTact() );
			Pattern * source = NULL;
			if( entry.contains( "source_pattern_index" ) )
			{
				const int index = entry["source_pattern_index"].toInt();
				if( index >= t->getTCOs().size() || !( source = dynamic_cast<Pattern *>( t->getTCOs()[index] ) ) ) { return result( false, "source_pattern_index is not a current pattern" ); }
			}
			if( entry.contains( "source_pattern_handle" ) )
			{
				QJsonObject target = entry; target["clip_handle"] = entry["source_pattern_handle"];
				source = dynamic_cast<Pattern *>( clip( target, error ) );
				if( !source ) { return result( false, error.isEmpty() ? "source_pattern_handle is not a current pattern" : error ); }
			}
			if( !error.isEmpty() || length <= 0 || qint64( pos ) + qMax( length, source ? int( source->length() ) : 1 ) > MaxSongLength ) { return result( false, error.isEmpty() ? "batch clip exceeds the song range" : error ); }
			placements.append( Placement{ t, source, pos, length, entry } );
		}
		Change change;
		QJsonArray placed;
		for( const Placement & entry : placements )
		{
			TrackContentObject * c = entry.source ? duplicateClip( entry.source, entry.pos ) : entry.track->createTCO( MidiTime( entry.pos ) );
			c->changeLength( MidiTime( qMax( entry.length, entry.source ? int( entry.source->length() ) : 1 ) ) );
			c->movePosition( MidiTime( entry.pos ) );
			if( entry.args.contains( "name" ) ) { c->setName( entry.args["name"].toString() ); }
			placed.append( clipJson( c, entry.track->getTCOs().indexOf( c ) ) );
			modified = true;
		}
		o["clips"] = placed;
	}
	else if( command == "place_pattern" || command == "place_bb_clip" || command == "place_sample_clip" )
	{
		Track * t = track( args, error );
		if( !t ) { return result( false, error ); }
		if( command == "place_pattern" && !dynamic_cast<InstrumentTrack *>( t ) ) { return result( false, "place_pattern requires an instrument lane; BB arrangements use place_bb_clip" ); }
		if( command == "place_bb_clip" && !dynamic_cast<BBTrack *>( t ) ) { return result( false, "place_bb_clip requires a BB song track" ); }
		if( command == "place_sample_clip" && !dynamic_cast<SampleTrack *>( t ) ) { return result( false, "place_sample_clip requires a sample track" ); }
		if( t->trackContainer() != song ) { return result( false, "clip placement is for the song editor; BB patterns are created with their columns" ); }
		const int pos = ticks( args, "pos_ticks", "pos_bars", error );
		const int length = ticks( args, "length_ticks", "length_bars", error, MidiTime::ticksPerTact() );
		if( !error.isEmpty() || length <= 0 || qint64( pos ) + length > MaxSongLength ) { return result( false, error.isEmpty() ? "clip length/position is outside the song range" : error ); }
		Pattern * source = NULL;
		if( args.contains( "source_pattern_index" ) )
		{
			const int index = args["source_pattern_index"].toInt();
			if( index >= t->getTCOs().size() || !( source = dynamic_cast<Pattern *>( t->getTCOs()[index] ) ) ) { return result( false, "source_pattern_index is not a current pattern" ); }
		}
		if( args.contains( "source_pattern_handle" ) )
		{
			QJsonObject target = args; target["clip_handle"] = args["source_pattern_handle"];
			source = dynamic_cast<Pattern *>( clip( target, error ) );
			if( !source ) { return result( false, error.isEmpty() ? "source_pattern_handle is not a current pattern" : error ); }
		}
		if( source && qint64( pos ) + qMax( length, int( source->length() ) ) > MaxSongLength ) { return result( false, "copied pattern exceeds the song range" ); }
		QString path;
		if( command == "place_sample_clip" ) { path = audioPath( args["file_path"].toString(), error ); if( !error.isEmpty() ) { return result( false, error ); } }
		Change change;
		TrackContentObject * placed = source ? duplicateClip( source, pos ) : t->createTCO( MidiTime( pos ) );
		if( SampleTCO * sample = dynamic_cast<SampleTCO *>( placed ) ) { QMetaObject::invokeMethod( sample, "setSampleFile", Qt::DirectConnection, Q_ARG( QString, path ) ); }
		placed->movePosition( MidiTime( pos ) );
		if( args.contains( "length_ticks" ) || args.contains( "length_bars" ) || command != "place_sample_clip" ) { placed->changeLength( MidiTime( qMax( length, source ? int( source->length() ) : 1 ) ) ); }
		if( args.contains( "name" ) ) { placed->setName( args["name"].toString() ); }
		modified = true;
		o["clip_index"] = t->getTCOs().indexOf( placed );
		o["clip"] = clipJson( placed, t->getTCOs().indexOf( placed ) );
	}
	else if( command == "assign_sample_file" )
	{
		Track * t = track( args, error );
		if( !t ) { return result( false, error ); }
		const QString path = audioPath( args["file_path"].toString(), error );
		if( !error.isEmpty() ) { return result( false, error ); }
		if( InstrumentTrack * it = dynamic_cast<InstrumentTrack *>( t ) )
		{
			if( !it->instrument() || QString::fromUtf8( it->instrument()->descriptor()->name ) != "audiofileprocessor" ) { return result( false, "sample assignment requires AudioFileProcessor or a sample track" ); }
			Change change;
			const QString name = it->name();
			QDomDocument doc; QDomElement root = doc.createElement( "state" ); QDomElement state = it->instrument()->saveState( doc, root );
			if( resolvedSamplePath( state.attribute( "src" ) ) != path ) { it->instrument()->loadFile( path ); modified = true; }
			if( args["preserve_name"].toBool( true ) ) { it->setName( name ); }
			o["side_effects"] = it->name() == name ? QJsonArray() : QJsonArray() << "name";
			o["track"] = trackJson( it, it->trackContainer()->tracks().indexOf( it ) );
		}
		else
		{
			SampleTCO * sample = dynamic_cast<SampleTCO *>( clip( args, error ) );
			if( !sample ) { return result( false, error.isEmpty() ? "selected clip is not a sample" : error ); }
			Change change;
			const QString name = sample->name(); const int length = int( sample->length() );
			if( resolvedSamplePath( sample->sampleBuffer()->audioFile() ) != path )
			{
				if( !QMetaObject::invokeMethod( sample, "setSampleFile", Qt::DirectConnection, Q_ARG( QString, path ) ) ) { return result( false, "native sample file slot is unavailable" ); }
				modified = true;
			}
			if( args["preserve_name"].toBool( true ) ) { sample->setName( name ); }
			if( args["preserve_length"].toBool( true ) && int( sample->length() ) != length ) { sample->changeLength( MidiTime( length ) ); }
			QJsonArray sideEffects;
			if( sample->name() != name ) { sideEffects.append( "name" ); }
			if( int( sample->length() ) != length ) { sideEffects.append( "length" ); }
			o["side_effects"] = sideEffects;
			o["clip"] = clipJson( sample, t->getTCOs().indexOf( sample ) );
		}
	}
	else if( command == "get_clip" || command == "rename_clip" || command == "move_clip" || command == "resize_clip" || command == "duplicate_clip" || command == "delete_clip" || command == "update_clip" )
	{
		const bool update = command == "update_clip";
		if( update && !containsAny( args, { "name", "new_pos_ticks", "new_pos_bars", "length_ticks", "length_bars" } ) ) { return result( false, "provide at least one clip property" ); }
		if( update && args.contains( "pos_bars" ) && args.contains( "old_pos_bars" ) ) { return result( false, "provide pos_bars or old_pos_bars, not both" ); }
		TrackContentObject * selected = clip( args, error );
		if( !selected ) { return result( false, error ); }
		Track * t = selected->getTrack();
		if( command == "get_clip" ) { o["clip"] = clipJson( selected, t->getTCOs().indexOf( selected ) ); return o; }
		int pos = int( selected->startPosition() ), length = int( selected->length() );
		const bool moving = command == "move_clip" || command == "duplicate_clip" || ( update && containsAny( args, { "new_pos_ticks", "new_pos_bars" } ) );
		const bool resizing = command == "resize_clip" || ( update && containsAny( args, { "length_ticks", "length_bars" } ) );
		if( moving )
		{
			if( !args.contains( "new_pos_ticks" ) && !args.contains( "new_pos_bars" ) ) { return result( false, "new_pos_ticks or new_pos_bars is required" ); }
			pos = ticks( args, "new_pos_ticks", "new_pos_bars", error );
		}
		if( resizing )
		{
			if( !args.contains( "length_ticks" ) && !args.contains( "length_bars" ) ) { return result( false, "length_ticks or length_bars is required" ); }
			length = ticks( args, "length_ticks", "length_bars", error );
		}
		if( !error.isEmpty() || length <= 0 || qint64( pos ) + length > MaxSongLength ) { return result( false, error.isEmpty() ? "clip length/position is outside the song range" : error ); }
		if( t->trackContainer() != song && ( moving || command == "delete_clip" ) ) { return result( false, "BB columns are managed as BB song tracks, not independent clips" ); }
		Change change;
		if( command == "delete_clip" ) { delete selected; modified = true; return o; }
		if( command == "duplicate_clip" )
		{
			selected = duplicateClip( selected, pos ); modified = true;
		}
		if( int( selected->startPosition() ) != pos ) { selected->movePosition( MidiTime( pos ) ); modified = true; }
		if( int( selected->length() ) != length ) { selected->changeLength( MidiTime( length ) ); modified = true; }
		if( args.contains( "name" ) && selected->name() != args["name"].toString() ) { selected->setName( args["name"].toString() ); modified = true; }
		o["clip_index"] = t->getTCOs().indexOf( selected );
		o["clip"] = clipJson( selected, t->getTCOs().indexOf( selected ) );
	}
	else if( command == "get_mixer_channels" )
	{
		if( args.contains( "channel_num" ) )
		{
			if( args["channel_num"].toInt() >= Engine::fxMixer()->numChannels() ) { return result( false, "channel_num is outside the current mixer" ); }
			o["channels"] = QJsonArray() << mixerChannelJson( args["channel_num"].toInt() );
		}
		else { o["channels"] = mixerJson(); }
	}
	else if( command == "add_mixer_channel" || command == "remove_mixer_channel" || command == "set_mixer_channel_volume" || command == "set_mixer_channel_name" || command == "mute_mixer_channel" || command == "solo_mixer_channel" || command == "update_mixer_channel" )
	{
		int index = args["channel_num"].toInt();
		FxMixer * mixer = Engine::fxMixer();
		const bool update = command == "update_mixer_channel";
		if( command != "add_mixer_channel" && index >= mixer->numChannels() ) { return result( false, "channel_num is outside the current mixer" ); }
		if( command == "remove_mixer_channel" && index == 0 ) { return result( false, "cannot remove the master channel" ); }
		if( command == "add_mixer_channel" && mixer->numChannels() >= 128 ) { return result( false, "mixer channel limit reached" ); }
		if( update && !containsAny( args, { "name", "volume", "muted", "solo" } ) ) { return result( false, "provide at least one mixer channel property" ); }
		if( update && args.contains( "volume" ) )
		{
			error = valueError( &mixer->effectChannel( index )->m_volumeModel, args["volume"].toDouble() );
			if( !error.isEmpty() ) { return result( false, error ); }
		}
		Change change;
		const QJsonArray beforeStates = mixerStates();
		if( command == "add_mixer_channel" ) { index = mixer->createChannel(); modified = true; }
		if( command == "remove_mixer_channel" )
		{
			int solo = -1;
			for( int i = 0; i < mixer->numChannels(); ++i ) { if( mixer->effectChannel( i )->m_soloModel.value() ) { solo = i; setMixerSolo( i, false ); break; } }
			if( gui && gui->fxMixerView() ) { gui->fxMixerView()->deleteChannel( index ); } else { mixer->deleteChannel( index ); }
			if( solo >= 0 && solo != index ) { setMixerSolo( solo > index ? solo - 1 : solo, true ); }
			modified = true;
		}
		else
		{
			FxChannel * channel = mixer->effectChannel( index );
			if( args.contains( "name" ) && channel->m_name != args["name"].toString() )
			{
				channel->m_name = args["name"].toString(); modified = true;
				channel->m_volumeModel.setDisplayName( channel->m_name + "/Volume" );
				channel->m_muteModel.setDisplayName( channel->m_name + "/Mute" );
				channel->m_soloModel.setDisplayName( channel->m_name + "/Solo" );
			}
			if( args.contains( "volume" ) ) { modified |= setModelValue( &channel->m_volumeModel, args["volume"].toDouble() ); }
			if( command == "mute_mixer_channel" || ( update && args.contains( "muted" ) ) ) { modified |= setModelValue( &channel->m_muteModel, args["muted"].toBool( true ) ); }
			if( command == "solo_mixer_channel" || ( update && args.contains( "solo" ) ) ) { modified |= setMixerSolo( index, args["solo"].toBool( true ) ); }
		}
		if( modified ) { refreshMixer(); }
		o["channel_num"] = index;
		if( command != "remove_mixer_channel" ) { o["channel"] = mixerChannelJson( index ); }
		QJsonArray routingChanges;
		const QJsonArray states = mixerStates();
		for( int i = 0; i < states.size(); ++i ) { if( i >= beforeStates.size() || states[i] != beforeStates[i] ) { routingChanges.append( states[i] ); } }
		o["routing_changes"] = routingChanges;
		if( args["include_all_channels"].toBool( true ) ) { o["channels"] = mixerJson(); }
	}
	else if( command == "set_mixer_send" || command == "remove_mixer_send" )
	{
		FxMixer * mixer = Engine::fxMixer();
		const int from = args["from_channel"].toInt(), to = args["to_channel"].toInt();
		if( from >= mixer->numChannels() || to >= mixer->numChannels() ) { return result( false, "routing channel is outside the current mixer" ); }
		if( from == to || ( command == "set_mixer_send" && mixer->isInfiniteLoop( from, to ) ) ) { return result( false, "send would create a feedback loop" ); }
		if( command == "remove_mixer_send" && !mixer->channelSendModel( from, to ) ) { return result( false, "send does not exist" ); }
		Change change;
		if( command == "remove_mixer_send" ) { mixer->deleteChannelSend( from, to ); modified = true; }
		else
		{
			FloatModel * current = mixer->channelSendModel( from, to );
			if( !current || current->value() != float( args["amount"].toDouble( 1 ) ) ) { mixer->createChannelSend( from, to, args["amount"].toDouble( 1 ) ); modified = true; }
		}
		if( modified )
		{
			for( int i = 0; i < mixer->numChannels(); ++i ) { if( mixer->effectChannel( i )->m_soloModel.value() ) { setMixerSolo( i, false ); setMixerSolo( i, true ); break; } }
			refreshMixer();
		}
		o["channel"] = mixerChannelJson( from );
		if( args["include_all_channels"].toBool( true ) ) { o["channels"] = mixerJson(); }
	}
	else if( command == "list_effect_subplugins" )
	{
		const QString name = args["effect_name"].toString();
		error = pluginError( name, Plugin::Effect );
		if( !error.isEmpty() ) { return result( false, error ); }
		const auto descriptor = pluginFactory->pluginInfo( name.toUtf8().constData() ).descriptor;
		EffectKeyList keys;
		if( descriptor->subPluginFeatures ) { descriptor->subPluginFeatures->listSubPluginKeys( descriptor, keys ); }
		QJsonArray list;
		for( int i = 0; i < keys.size(); ++i )
		{
			QJsonObject key, attributes;
			key["index"] = i;
			key["name"] = keys[i].name;
			for( auto it = keys[i].attributes.begin(); it != keys[i].attributes.end(); ++it ) { attributes[it.key()] = it.value(); }
			key["attributes"] = attributes;
			list.append( key );
		}
		o["subplugins"] = list;
	}
	else if( command == "get_effect_chain" || command == "add_effect" || command == "remove_effect" || command == "toggle_effect" || command == "set_effect_wet" || command == "set_effect_chain_enabled" || command == "update_effect" )
	{
		EffectChain * chain = effectChain( args, error );
		if( !chain ) { return result( false, error ); }
		if( command == "get_effect_chain" ) { o["enabled"] = chainEnabled( chain ); o["effects"] = effectList( chain ); return o; }
		const auto ordered = effects( chain );
		const int index = args["effect_index"].toInt();
		EffectKey key;
		if( command == "add_effect" )
		{
			const QString name = args["effect_name"].toString();
			error = pluginError( name, Plugin::Effect );
			if( !error.isEmpty() ) { return result( false, error ); }
			const auto descriptor = pluginFactory->pluginInfo( name.toUtf8().constData() ).descriptor;
			if( descriptor->subPluginFeatures )
			{
				EffectKeyList keys;
				descriptor->subPluginFeatures->listSubPluginKeys( descriptor, keys );
				const int subindex = args["subplugin_index"].toInt( -1 );
				if( subindex < 0 || subindex >= keys.size() ) { return result( false, "select a subplugin_index from list_effect_subplugins" ); }
				key = keys[subindex];
			}
		}
		else if( command != "set_effect_chain_enabled" && index >= ordered.size() ) { return result( false, "effect_index is outside the current chain" ); }
		QVector<QPair<AutomatableModel *, double> > edits;
		if( command == "update_effect" )
		{
			if( !containsAny( args, { "enabled", "wet" } ) ) { return result( false, "provide at least one effect property" ); }
			for( const QString & field : QStringList{ "enabled", "wet" } )
			{
				if( !args.contains( field ) ) { continue; }
				AutomatableModel * m = namedModel( ordered[index], "Effect", field == "enabled" ? "Effect enabled" : "Wet/Dry mix" );
				const double value = parameterValue( args[field] );
				error = valueError( m, value );
				if( !error.isEmpty() ) { return result( false, error ); }
				edits.append( qMakePair( m, value ) );
			}
		}
		Change change;
		if( command == "add_effect" )
		{
			Effect * effect = Effect::instantiate( args["effect_name"].toString(), chain, &key );
			if( !effect || !effect->isOkay() ) { delete effect; return result( false, "native effect could not be instantiated" ); }
			chain->appendEffect( effect );
			chain->setEnabled( true );
			if( args.contains( "wet" ) ) { if( AutomatableModel * m = namedModel( effect, "Effect", "Wet/Dry mix" ) ) { m->setValue( args["wet"].toDouble() ); } }
			modified = true;
			o["effect"] = effectJson( effect, effects( chain ).size() - 1 );
		}
		else if( command == "set_effect_chain_enabled" ) { if( chainEnabled( chain ) != args["enabled"].toBool() ) { chain->setEnabled( args["enabled"].toBool() ); modified = true; } }
		else if( command == "remove_effect" ) { Effect * effect = ordered[index]; chain->removeEffect( effect ); delete effect; modified = true; }
		else if( command == "update_effect" ) { for( const auto & edit : edits ) { modified |= setModelValue( edit.first, edit.second ); } }
		else
		{
			AutomatableModel * m = namedModel( ordered[index], "Effect", command == "toggle_effect" ? "Effect enabled" : "Wet/Dry mix" );
			if( !m ) { return result( false, "native effect parameter is unavailable; inspect get_parameters" ); }
			modified |= setModelValue( m, command == "toggle_effect" ? double( args["enabled"].toBool( !ordered[index]->isEnabled() ) ) : args["wet"].toDouble() );
		}
		if( command != "add_effect" && command != "remove_effect" && command != "set_effect_chain_enabled" ) { o["effect"] = effectJson( ordered[index], index ); }
		o["enabled"] = chainEnabled( chain );
		o["effect_count"] = effects( chain ).size();
		if( args["include_all_effects"].toBool( true ) ) { o["effects"] = effectList( chain ); }
	}
	else if( command == "add_automation" )
	{
		AutomationTrack * t = dynamic_cast<AutomationTrack *>( track( args, error ) );
		if( !t ) { return result( false, error.isEmpty() ? "automation requires an automation track" : error ); }
		AutomatableModel * target = NULL;
		if( args.contains( "model_id" ) ) { target = model( args["model_id"].toDouble(), error ); }
		else if( args.contains( "target_track_index" ) )
		{
			QJsonObject targetArgs = args;
			targetArgs["track_index"] = args["target_track_index"];
			Track * targetTrack = track( targetArgs, error );
			const QString parameter = args["target_parameter"].toString( "volume" );
			if( targetTrack ) { target = parameter == "volume" ? trackVolume( targetTrack ) : parameter == "panning" ? trackPanning( targetTrack ) : dynamic_cast<InstrumentTrack *>( targetTrack ) ? static_cast<InstrumentTrack *>( targetTrack )->pitchModel() : NULL; }
		}
		if( !target ) { return result( false, error.isEmpty() ? "provide a current model_id or target_track_index/target_parameter" : error ); }
		const QJsonArray points = args["points"].toArray();
		error = pointError( target, points );
		const int pos = ticks( args, "pos_ticks", "pos_bars", error );
		const int length = ticks( args, "length_ticks", "length_bars", error, MidiTime::ticksPerTact() );
		int last = 0;
		for( const QJsonValue & entry : points ) { last = qMax( last, entry.toObject()["pos"].toInt() ); }
		if( !error.isEmpty() || length <= 0 || qint64( pos ) + qMax( length, last + 1 ) > MaxSongLength ) { return result( false, error.isEmpty() ? "automation exceeds the song range" : error ); }
		Change change;
		AutomationPattern * p = static_cast<AutomationPattern *>( t->createTCO( MidiTime( pos ) ) );
		p->addObject( target );
		putPoints( p, points, args, true );
		p->movePosition( MidiTime( pos ) );
		p->changeLength( MidiTime( qMax( length, int( p->timeMapLength() ) ) ) );
		if( args.contains( "name" ) ) { p->setName( args["name"].toString() ); }
		modified = true;
		o["clip_index"] = t->getTCOs().indexOf( p );
		o["automation"] = clipJson( p, t->getTCOs().indexOf( p ) );
	}
	else if( command == "get_automation" || command == "set_automation_points" || command == "set_automation_point" || command == "remove_automation_point" || command == "link_automation" )
	{
		AutomationPattern * p = dynamic_cast<AutomationPattern *>( clip( args, error ) );
		if( !p ) { return result( false, error.isEmpty() ? "selected clip is not automation" : error ); }
		if( command == "get_automation" ) { o["automation"] = clipJson( p, p->getTrack()->getTCOs().indexOf( p ) ); return o; }
		AutomatableModel * target = NULL;
		if( command == "link_automation" )
		{
			target = model( args["model_id"].toDouble(), error );
			if( !target ) { return result( false, error ); }
			if( !p->objects().isEmpty() && ( target->minValue<float>() != p->getMin() || target->maxValue<float>() != p->getMax() || target->isScaleLogarithmic() != p->firstObject()->isScaleLogarithmic() ) ) { return result( false, "additional targets must have the same parameter range/scaling" ); }
		}
		QJsonArray points;
		if( command == "set_automation_points" || command == "set_automation_point" )
		{
			if( p->objects().isEmpty() ) { return result( false, "automation clip has no linked live parameter" ); }
			points = command == "set_automation_points" ? args["points"].toArray() : QJsonArray() << QJsonObject{ { "pos", args["pos"] }, { "value", args["value"] } };
			error = pointError( const_cast<AutomatableModel *>( p->firstObject() ), points );
			if( !error.isEmpty() ) { return result( false, error ); }
			for( const QJsonValue & entry : points ) { if( qint64( int( p->startPosition() ) ) + entry.toObject()["pos"].toInt() + 1 > MaxSongLength ) { return result( false, "automation point exceeds the song range" ); } }
		}
		if( command == "remove_automation_point" && !p->getTimeMap().contains( args["pos"].toInt() ) ) { return result( false, "automation point does not exist" ); }
		if( command == "link_automation" ) { modified = !p->objects().contains( QPointer<AutomatableModel>( target ) ); }
		else if( command == "remove_automation_point" ) { modified = true; }
		else
		{
			AutomationPattern::timeMap desired = command == "set_automation_points" && args["mode"].toString( "replace" ) == "replace" ? AutomationPattern::timeMap() : p->getTimeMap();
			for( const QJsonValue & entry : points ) { const QJsonObject point = entry.toObject(); desired[point["pos"].toInt()] = p->firstObject()->inverseScaledValue( point["value"].toDouble() ); }
			modified = desired != p->getTimeMap();
			const QString progression = args["progression"].toString();
			if( !progression.isEmpty() ) { modified |= int( p->progressionType() ) != ( progression == "linear" ? 1 : progression == "cubic" ? 2 : 0 ); }
			if( args.contains( "tension" ) ) { modified |= p->getTension() != float( args["tension"].toDouble() ); }
		}
		if( modified )
		{
			Change change; p->addJournalCheckPoint();
			if( command == "link_automation" ) { p->addObject( target ); }
			else if( command == "remove_automation_point" ) { p->removeValue( MidiTime( args["pos"].toInt() ) ); }
			else { putPoints( p, points, args, command == "set_automation_points" && args["mode"].toString( "replace" ) == "replace" ); }
		}
		o["automation"] = clipJson( p, p->getTrack()->getTCOs().indexOf( p ) );
	}
	else if( command == "note_name_to_key" || command == "key_to_note_name" || command == "bars_to_ticks_converter" || command == "ticks_to_bars_converter" || command == "generate_scale" )
	{
		if( command == "note_name_to_key" ) { const int key = MusicTheory::midiKey( args["name"].toString(), error ); if( !error.isEmpty() ) { return result( false, error ); } o["key"] = key; o["lmms_key"] = key - 12; }
		else if( command == "key_to_note_name" ) { o["name"] = MusicTheory::noteName( args["key"].toInt() ); }
		else if( command == "bars_to_ticks_converter" ) { o["ticks"] = ticks( args, "unused", "bars", error ); if( !error.isEmpty() ) { return result( false, error ); } }
		else if( command == "ticks_to_bars_converter" ) { o["bars"] = args["ticks"].toDouble() / MidiTime::ticksPerTact(); }
		else
		{
			QString root = args["root"].toString();
			QRegExp letter( "^[A-Ga-g][#b]?$" );
			if( letter.exactMatch( root ) ) { root += QString::number( args["octave"].toInt( 4 ) ); }
			const int base = MusicTheory::midiKey( root, error );
			if( !error.isEmpty() ) { return result( false, error ); }
			QJsonArray keys, names;
			const QVector<int> intervals = MusicTheory::scales()[args["scale_type"].toString( "major" )];
			const int octaves = args["num_octaves"].toInt( 1 );
			for( int octave = 0; octave < octaves; ++octave ) { for( int interval : intervals ) { const int key = base + octave * 12 + interval; if( key > 127 ) { return result( false, "scale exceeds the MIDI range" ); } keys.append( key ); names.append( MusicTheory::noteName( key ) ); } }
			if( base + octaves * 12 > 127 ) { return result( false, "scale exceeds the MIDI range" ); }
			keys.append( base + octaves * 12 ); names.append( MusicTheory::noteName( base + octaves * 12 ) );
			o["keys"] = keys; o["notes"] = names;
		}
	}
	else { return result( false, "unknown command" ); }
	return o;
}
