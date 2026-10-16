#include "BridgeModel.h"
#include "AudioCodecs.h"
#include "MusicTheory.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSet>
#include <QtCore/QDir>
#include <QtCore/QUuid>
#include <QtCore/QPointer>
#include <QtCore/QtEndian>
#include <cmath>

#include "AudioPort.h"
#include "AutomationPattern.h"
#include "BBEditor.h"
#include "BBTrack.h"
#include "BBTrackContainer.h"
#include "Engine.h"
#include "ComboBoxModel.h"
#include "LadspaControl.h"
#include "LadspaBase.h"
#include "FxMixer.h"
#include "FxMixerView.h"
#include "GuiApplication.h"
#include "Instrument.h"
#include "PluginFactory.h"
#include "ProjectJournal.h"
#include "SampleBuffer.h"
#include "SampleTrack.h"
#include "Song.h"
#include "SongEditor.h"

namespace BridgeModel
{

namespace
{
bool includeNotes = true, includeParameters = true, includeClips = true;
QString token = QUuid::createUuid().toString();
quint64 nextHandle = 0;
bool dispatchingSolo = false;

bool activeLadspaModel( AutomatableModel * model, LadspaControl * control )
{
	if( model == control->toggledModel() ) { return control->port()->data_type == TOGGLED; }
	if( model == control->knobModel() ) { return control->port()->data_type == INTEGER || control->port()->data_type == FLOATING; }
	if( model == control->tempoSyncKnobModel() ) { return control->port()->data_type == TIME; }
	// The remaining child is the channel-link model. Native LADSPA connects
	// it only on processor zero of a multi-processor effect.
	Effect * effect = control->parent() ? dynamic_cast<Effect *>( control->parent()->parent() ) : NULL;
	return effect && effect->processorCount() > 1 && control->port()->proc == 0;
}

// LMMS 1.2.2's solo slot toggles its previous model while processing the new
// one. Block recursive slot calls, then notify views of the resulting states.
void dispatchMixerSolo()
{
	if( dispatchingSolo ) { return; }
	QScopedValueRollback<bool> guard( dispatchingSolo, true );
	FxMixer * mixer = Engine::fxMixer();
	QVector<bool> blocked, before;
	for( int i = 0; i < mixer->numChannels(); ++i )
	{
		BoolModel & model = mixer->effectChannel( i )->m_soloModel;
		before.append( model.value() );
		blocked.append( model.blockSignals( true ) );
	}
	mixer->toggledSolo();
	for( int i = 0; i < mixer->numChannels(); ++i )
	{
		BoolModel & model = mixer->effectChannel( i )->m_soloModel;
		model.blockSignals( blocked[i] );
		if( before[i] != model.value() ) { model.dataChanged(); }
	}
}

TrackContentObject * clipByHandle( const QString & handle )
{
	if( !handle.startsWith( token + "/clip/" ) ) { return NULL; }
	const auto tracks = Engine::getSong()->tracks() + Engine::getBBTrackContainer()->tracks();
	for( Track * t : tracks ) { for( TrackContentObject * c : t->getTCOs() ) { if( objectHandle( c, "clip" ) == handle ) { return c; } } }
	return NULL;
}
}

JsonDetail::JsonDetail( const QJsonObject & args ) :
	m_notes( includeNotes, args["include_notes"].toBool( true ) ),
	m_parameters( includeParameters, args["include_parameters"].toBool( true ) ),
	m_clips( includeClips, args["include_clips"].toBool( true ) ) {}

QString projectToken() { return token; }
void resetProjectHandles() { token = QUuid::createUuid().toString(); }
QJsonObject handleContext()
{
	QJsonObject o;
	o["session_id"] = QString::number( QCoreApplication::applicationPid() );
	o["project_token"] = token;
	return o;
}
QString objectHandle( QObject * object, const QString & kind )
{
	const QString prefix = token + "/" + kind + "/";
	QString handle = object->property( "agentbridgeHandle" ).toString();
	if( !handle.startsWith( prefix ) )
	{
		handle = prefix + QString::number( ++nextHandle );
		object->setProperty( "agentbridgeHandle", handle );
	}
	return handle;
}

QString resolvedSamplePath( const QString & path )
{
	if( path.isEmpty() ) { return QString(); }
	QString expanded = SampleBuffer::tryToMakeAbsolute( path );
	if( expanded.startsWith( "data:/" ) )
	{
		expanded = QCoreApplication::applicationDirPath() + "/data/" + expanded.mid( 6 );
	}
	return QDir::cleanPath( QFileInfo( expanded ).absoluteFilePath() );
}

QJsonObject vstStatus( InstrumentTrack * track )
{
	QJsonObject status;
	Instrument * instrument = track->instrument();
	if( !instrument || QString::fromUtf8( instrument->descriptor()->name ) != "vestige" ) { return status; }
	QDomDocument doc;
	QDomElement root = doc.createElement( "state" );
	QDomElement state = instrument->saveState( doc, root );
	const QString path = state.attribute( "plugin" );
	status["plugin_loaded"] = !path.isEmpty();
	status["path"] = path.isEmpty() ? QString() : resolvedSamplePath( path );
	status["error"] = path.isEmpty() ? "VeSTige has no successfully loaded external plugin" : QString();
	status["parameter_count"] = models( instrument ).size();
	status["parameters_supported"] = !models( instrument ).isEmpty();
	status["vendor"] = QJsonValue();
	status["version"] = QJsonValue();
	status["metadata_limitation"] = "Stock VeSTige does not expose vendor/version through its public instrument API";
	return status;
}

void prepareMixerSolo()
{
	FxMixer * mixer = Engine::fxMixer();
	for( int i = 0; i < mixer->numChannels(); ++i )
	{
		BoolModel * model = &mixer->effectChannel( i )->m_soloModel;
		if( gui && gui->fxMixerView() )
		{
			QObject::disconnect( model, SIGNAL( dataChanged() ), gui->fxMixerView(), SLOT( toggledSolo() ) );
		}
		if( !model->property( "agentbridgeSoloHook" ).toBool() )
		{
			model->setProperty( "agentbridgeSoloHook", true );
			QObject::connect( model, &AutomatableModel::dataChanged, mixer, []() { dispatchMixerSolo(); }, Qt::DirectConnection );
		}
	}
}

bool setMixerSolo( int index, bool value )
{
	prepareMixerSolo();
	BoolModel & model = Engine::fxMixer()->effectChannel( index )->m_soloModel;
	if( model.value() == value ) { return false; }
	model.setValue( value );
	return true;
}

bool setModelValue( AutomatableModel * model, double value )
{
	if( model->value<float>() == float( value ) ) { return false; }
	for( int i = 0; i < Engine::fxMixer()->numChannels(); ++i )
	{
		if( model == &Engine::fxMixer()->effectChannel( i )->m_soloModel ) { return setMixerSolo( i, value != 0 ); }
	}
	const float before = model->value<float>();
	model->setValue( float( value ) );
	return before != model->value<float>();
}

TrackContentObject * duplicateClip( TrackContentObject * source, int position )
{
	TrackContentObject * copy;
	if( Pattern * p = dynamic_cast<Pattern *>( source ) ) { copy = new Pattern( *p ); }
	else if( AutomationPattern * p = dynamic_cast<AutomationPattern *>( source ) ) { copy = new AutomationPattern( *p ); }
	else
	{
		QDomDocument doc;
		QDomElement root = doc.createElement( "copy" );
		const QDomElement state = source->saveState( doc, root );
		copy = source->getTrack()->createTCO( MidiTime( position ) );
		copy->restoreState( state );
	}
	copy->setName( source->name() );
	copy->setMuted( source->isMuted() );
	copy->setSolo( source->isSolo() );
	copy->setAutoResize( source->getAutoResize() );
	copy->changeLength( source->length() );
	copy->movePosition( MidiTime( position ) );
	return copy;
}

Change::Change() { Engine::mixer()->requestChangeInModel(); }
Change::~Change() { Engine::mixer()->doneChangeInModel(); }
QJsonObject reply( bool ok, const QString & error )
{
	QJsonObject o;
	o["ok"] = ok;
	if( !error.isEmpty() ) { o["error"] = error; }
	return o;
}

TrackContainer * container( const QJsonObject & args )
{
	return args["container"].toString() == "bb" ? static_cast<TrackContainer *>( Engine::getBBTrackContainer() ) : Engine::getSong();
}

Track * track( const QJsonObject & args, QString & error )
{
	TrackContainer * tc = container( args );
	if( args.contains( "clip_handle" ) )
	{
		TrackContentObject * c = clipByHandle( args["clip_handle"].toString() );
		if( !c ) { error = "stale or unknown clip_handle; inspect the current project"; return NULL; }
		Track * t = c->getTrack();
		if( ( args.contains( "track_handle" ) && objectHandle( t, "track" ) != args["track_handle"].toString() ) ||
			( args.contains( "container" ) && tc != t->trackContainer() ) ||
			( args.contains( "track_index" ) && ( tc != t->trackContainer() || tc->tracks().indexOf( t ) != args["track_index"].toInt() ) ) )
		{ error = "clip_handle does not belong to the selected track/container"; return NULL; }
		return t;
	}
	if( args.contains( "track_handle" ) )
	{
		const auto tracks = Engine::getSong()->tracks() + Engine::getBBTrackContainer()->tracks();
		for( Track * t : tracks )
		{
			if( objectHandle( t, "track" ) != args["track_handle"].toString() ) { continue; }
			if( args.contains( "container" ) && t->trackContainer() != tc ) { error = "track_handle belongs to another container"; return NULL; }
			return t;
		}
		error = "stale or unknown track_handle; inspect the current project";
		return NULL;
	}
	const int index = args["track_index"].toInt( args["target_index"].toInt( -1 ) );
	if( !tc || index < 0 || index >= tc->tracks().size() ) { error = "track_index is outside the current container"; return NULL; }
	return tc->tracks()[index];
}

QString removeTrack( Track * t )
{
	if( !gui ) { delete t; return QString(); }
	TrackContainerView * view = t->trackContainer() == Engine::getSong() ?
		static_cast<TrackContainerView *>( gui->songEditor()->m_editor ) : gui->getBBEditor()->trackContainerView();
	QCoreApplication::sendPostedEvents( view, QEvent::MetaCall );
	const auto views = view->trackViews();
	for( TrackView * trackView : views )
	{
		if( trackView->getTrack() != t ) { continue; }
		return QMetaObject::invokeMethod( view, "deleteTrackView", Qt::DirectConnection, Q_ARG( TrackView *, trackView ) ) ?
			QString() : "native track removal slot is unavailable";
	}
	return "native track view is not ready; retry after LMMS updates";
}

InstrumentTrack * instrument( const QJsonObject & args, QString & error )
{
	Track * t = track( args, error );
	if( !t ) { return NULL; }
	if( BBTrack * bb = dynamic_cast<BBTrack *>( t ) )
	{
		Q_UNUSED( bb )
		const auto & tracks = Engine::getBBTrackContainer()->tracks();
		int index = args["bb_instrument_index"].toInt( -1 );
		if( index < 0 )
		{
			for( int i = 0; i < tracks.size(); ++i ) { if( dynamic_cast<InstrumentTrack *>( tracks[i] ) ) { index = i; break; } }
		}
		if( index < 0 || index >= tracks.size() ) { error = "BB track needs an inner instrument; use add_instrument_track with container=bb"; return NULL; }
		t = tracks[index];
	}
	InstrumentTrack * it = dynamic_cast<InstrumentTrack *>( t );
	if( !it ) { error = "track is not an instrument track"; }
	return it;
}

NoteTarget noteTarget( const QJsonObject & args, QString & error, bool allowMissing )
{
	NoteTarget target = { NULL, NULL, args["pattern_index"].toInt() };
	Track * outer = track( args, error );
	if( !outer ) { return target; }
	if( BBTrack * bb = dynamic_cast<BBTrack *>( outer ) )
	{
		if( args.contains( "pattern_index" ) && target.index != bb->index() ) { error = "pattern_index must match this BB track's column"; return target; }
		target.index = bb->index();
	}
	target.track = instrument( args, error );
	if( !target.track ) { return target; }
	if( args.contains( "clip_handle" ) )
	{
		target.pattern = dynamic_cast<Pattern *>( clipByHandle( args["clip_handle"].toString() ) );
		if( !target.pattern ) { error = "clip_handle is not a note pattern"; }
		else { target.index = target.track->getTCOs().indexOf( target.pattern ); }
		return target;
	}
	const auto & clips = target.track->getTCOs();
	if( target.index < 0 || target.index >= clips.size() )
	{
		if( !allowMissing || target.index != 0 || !clips.isEmpty() || target.track->trackContainer() != Engine::getSong() )
		{
			error = "pattern_index is outside the track; place_pattern first";
		}
		return target;
	}
	target.pattern = dynamic_cast<Pattern *>( clips[target.index] );
	if( !target.pattern ) { error = "clip is not a note pattern"; }
	return target;
}

Pattern * ensurePattern( NoteTarget & target )
{
	if( !target.pattern ) { target.pattern = static_cast<Pattern *>( target.track->createTCO( MidiTime( 0 ) ) ); }
	return target.pattern;
}

NoteData noteData( const QJsonObject & args, QString & error, const Note * defaults )
{
	NoteData n = { defaults ? defaults->key() : 48, defaults ? int( defaults->pos() ) : 0,
		defaults ? int( defaults->length() ) : 48, defaults ? int( defaults->getVolume() ) : 100,
		defaults ? int( defaults->getPanning() ) : 0 };
	const int pitchFields = int( args.contains( "key" ) ) + int( args.contains( "note_name" ) ) + int( args.contains( "lmms_key" ) );
	if( pitchFields > 1 ) { error = "provide only one of key, note_name, or lmms_key"; return n; }
	if( args.contains( "lmms_key" ) ) { n.key = args["lmms_key"].toInt(); }
	else if( pitchFields )
	{
		const QJsonValue value = args.contains( "note_name" ) ? args["note_name"] : args["key"];
		const int midi = value.isString() ? MusicTheory::midiKey( value.toString(), error ) : value.toInt();
		n.key = midi - 12;
	}
	n.pos = args["pos"].toInt( n.pos );
	n.length = args["length"].toInt( n.length );
	n.volume = args["volume"].toInt( n.volume );
	n.panning = args["panning"].toInt( n.panning );
	if( error.isEmpty() && ( n.key < 0 || n.key >= NumKeys ) ) { error = "LMMS 1.2.2 supports MIDI keys 12..119 (C0..B8); legacy LMMS keys are 0..107"; }
	if( error.isEmpty() && ( n.length == 0 || ( n.length < -1 && args.contains( "length" ) ) || qint64( n.pos ) + qMax( 1, n.length ) > MaxSongLength ) ) { error = "note length/position exceeds the song range; length=-1 is a step trigger"; }
	return n;
}

int ticks( const QJsonObject & args, const char * ticksName, const char * barsName, QString & error, int fallback )
{
	if( args.contains( ticksName ) && args.contains( barsName ) ) { error = QString( "provide %1 or %2, not both" ).arg( ticksName, barsName ); return 0; }
	const double value = args.contains( barsName ) ? args[barsName].toDouble() * MidiTime::ticksPerTact() : args[ticksName].toDouble( fallback );
	if( !std::isfinite( value ) || value < 0 || value > MaxSongLength ) { error = "timeline value is outside the song range"; return 0; }
	return int( std::floor( value + 0.5 ) );
}

TrackContentObject * clip( const QJsonObject & args, QString & error )
{
	Track * t = track( args, error );
	if( !t ) { return NULL; }
	if( args.contains( "clip_handle" ) ) { return clipByHandle( args["clip_handle"].toString() ); }
	int index = args["clip_index"].toInt( args["pattern_index"].toInt() );
	if( args.contains( "pos_bars" ) || args.contains( "old_pos_bars" ) )
	{
		if( args.contains( "clip_index" ) ) { error = "provide clip_index or a clip position, not both"; return NULL; }
		const char * field = args.contains( "old_pos_bars" ) ? "old_pos_bars" : "pos_bars";
		const int position = ticks( args, "clip_pos_ticks", field, error );
		if( !error.isEmpty() ) { return NULL; }
		index = -1;
		for( int i = 0; i < t->getTCOs().size(); ++i )
		{
			if( int( t->getTCOs()[i]->startPosition() ) == position )
			{
				if( index >= 0 ) { error = "multiple clips share that position; select by clip_index"; return NULL; }
				index = i;
			}
		}
	}
	if( index < 0 || index >= t->getTCOs().size() ) { error = "clip_index/position does not identify a current clip"; return NULL; }
	return t->getTCOs()[index];
}

QVector<AutomatableModel *> models( QObject * root )
{
	QVector<AutomatableModel *> found;
	if( !root ) { return found; }
	if( AutomatableModel * m = dynamic_cast<AutomatableModel *>( root ) ) { found.append( m ); }
	for( AutomatableModel * m : root->findChildren<AutomatableModel *>() ) { found.append( m ); }
	return found;
}

AutomatableModel * namedModel( QObject * root, const QString & context, const QString & name )
{
	const QString translated = QCoreApplication::translate( context.toLatin1().constData(), name.toLatin1().constData() );
	for( AutomatableModel * m : models( root ) ) { if( m->displayName() == name || m->displayName() == translated ) { return m; } }
	return NULL;
}

AutomatableModel * trackVolume( Track * t )
{
	if( InstrumentTrack * it = dynamic_cast<InstrumentTrack *>( t ) ) { return it->volumeModel(); }
	return dynamic_cast<SampleTrack *>( t ) ? namedModel( t, "SampleTrack", "Volume" ) : NULL;
}
AutomatableModel * trackPanning( Track * t )
{
	if( InstrumentTrack * it = dynamic_cast<InstrumentTrack *>( t ) ) { return it->panningModel(); }
	return dynamic_cast<SampleTrack *>( t ) ? namedModel( t, "SampleTrack", "Panning" ) : NULL;
}

QVector<AutomatableModel *> projectModels()
{
	QVector<AutomatableModel *> all = models( Engine::getSong() ) + models( Engine::getBBTrackContainer() ) + models( Engine::fxMixer() );
	const auto tracks = Engine::getSong()->tracks() + Engine::getBBTrackContainer()->tracks();
	for( Track * t : tracks )
	{
		all += models( t );
		if( InstrumentTrack * it = dynamic_cast<InstrumentTrack *>( t ) ) { all += models( it->instrument() ); all += models( it->audioPort()->effects() ); }
		if( SampleTrack * st = dynamic_cast<SampleTrack *>( t ) ) { all += models( st->audioPort()->effects() ); }
	}
	for( int i = 0; i < Engine::fxMixer()->numChannels(); ++i )
	{
		FxChannel * channel = Engine::fxMixer()->effectChannel( i );
		all += models( &channel->m_fxChain );
		all.append( &channel->m_volumeModel );
		all.append( &channel->m_muteModel );
		all.append( &channel->m_soloModel );
	}
	for( FxRoute * route : Engine::fxMixer()->m_fxRoutes ) { all.append( route->amount() ); }
	return all;
}

AutomatableModel * model( double id, QString & error )
{
	AutomatableModel * m = dynamic_cast<AutomatableModel *>( Engine::projectJournal()->journallingObject( jo_id_t( id ) ) );
	if( !m || !projectModels().contains( m ) ) { error = "model_id does not identify a current project parameter; inspect parameters again after loading/replacing plugins"; return NULL; }
	return m;
}

QString valueError( AutomatableModel * m, double value )
{
	if( !m ) { return "parameter is not available for this target"; }
	if( LadspaControl * control = dynamic_cast<LadspaControl *>( m->parent() ) )
	{
		if( control->port()->rate == CONTROL_RATE_OUTPUT || !activeLadspaModel( m, control ) ) { return "parameter is an inactive or read-only LADSPA control"; }
		if( m == control->knobModel() && control->port()->data_type == INTEGER && std::floor( value ) != value ) { return "parameter requires an integer value"; }
	}
	if( !std::isfinite( value ) || value < m->minValue<float>() || value > m->maxValue<float>() )
	{
		return QString( "%1 must be in %2..%3" ).arg( m->displayName() ).arg( m->minValue<float>() ).arg( m->maxValue<float>() );
	}
	if( m->hasStrictStepSize() && m->step<float>() == 1 && std::floor( value ) != value ) { return "parameter requires an integer value"; }
	return QString();
}

QJsonObject parameterJson( AutomatableModel * m )
{
	QJsonObject o;
	o["model_id"] = double( m->id() );
	o["name"] = m->displayName();
	o["full_name"] = m->fullDisplayName();
	o["value"] = m->value<float>();
	o["minimum"] = m->minValue<float>();
	o["maximum"] = m->maxValue<float>();
	o["step"] = m->step<float>();
	o["logarithmic"] = m->isScaleLogarithmic();
	o["automated"] = m->isAutomated();
	o["type"] = dynamic_cast<BoolModel *>( m ) ? "boolean" : dynamic_cast<IntModel *>( m ) ? "integer" : "number";
	o["writable"] = true;
	o["automation_capable"] = true;
	o["unit"] = "native";
	if( BoolModel * boolean = dynamic_cast<BoolModel *>( m ) ) { o["value"] = boolean->value(); o["unit"] = "none"; }
	QStringList segments;
	for( QObject * parent = m; parent; parent = parent->parent() )
	{
		int ordinal = 0;
		if( parent->parent() )
		{
			for( QObject * sibling : parent->parent()->children() )
			{
				if( sibling == parent ) { break; }
				if( QString::fromLatin1( sibling->metaObject()->className() ) == parent->metaObject()->className() ) { ++ordinal; }
			}
		}
		segments.prepend( QString::fromLatin1( parent->metaObject()->className() ) + "[" + QString::number( ordinal ) + "]" );
	}
	o["path"] = segments.join( "/" );
	if( m->parent() == Engine::getSong() )
	{
		if( m->displayName() == Song::tr( "Tempo" ) ) { o["unit"] = "bpm"; o["path"] = "song/tempo"; }
		if( m->displayName() == Song::tr( "Master volume" ) ) { o["unit"] = "percent"; o["path"] = "song/master_volume"; }
		if( m->displayName() == Song::tr( "Master pitch" ) ) { o["unit"] = "semitones"; o["path"] = "song/master_pitch"; }
	}
	if( ComboBoxModel * combo = dynamic_cast<ComboBoxModel *>( m ) )
	{
		QJsonArray choices;
		for( int i = 0; i < combo->size(); ++i ) { QJsonObject item; item["value"] = i; item["label"] = combo->itemText( i ); choices.append( item ); }
		o["type"] = "enum";
		o["choices"] = choices;
	}
	if( LadspaControl * control = dynamic_cast<LadspaControl *>( m->parent() ) )
	{
		const port_desc_t * port = control->port();
		o["name"] = port->name.isEmpty() ? QString( "Port %1" ).arg( port->port_id ) : port->name;
		o["port_index"] = port->port_id;
		o["processor_index"] = port->proc;
		const bool active = activeLadspaModel( m, control );
		o["active"] = active;
		o["writable"] = port->rate != CONTROL_RATE_OUTPUT && active;
		o["automation_capable"] = port->rate != CONTROL_RATE_OUTPUT && active;
		const QString role = m == control->toggledModel() ? "toggle" : m == control->knobModel() ? "value" : m == control->tempoSyncKnobModel() ? "time" : "channel_link";
		o["name"] = QString( "%1 (processor %2, port %3, %4%5)" ).arg( o["name"].toString() ).arg( port->proc ).arg( port->port_id ).arg( role ).arg( active ? "" : ", inactive" );
		o["path"] = o["path"].toString() + QString( "/processor[%1]/port[%2]" ).arg( port->proc ).arg( port->port_id );
		if( m == control->tempoSyncKnobModel() ) { o["unit"] = "milliseconds"; }
		if( m == control->knobModel() && port->data_type == INTEGER ) { o["type"] = "integer"; }
	}
	const auto tracks = Engine::getSong()->tracks() + Engine::getBBTrackContainer()->tracks();
	for( Track * t : tracks )
	{
		if( m == trackVolume( t ) ) { o["unit"] = "percent"; o["path"] = objectHandle( t, "track" ) + "/volume"; }
		if( m == trackPanning( t ) ) { o["unit"] = "percent_pan"; o["path"] = objectHandle( t, "track" ) + "/panning"; }
		if( InstrumentTrack * it = dynamic_cast<InstrumentTrack *>( t ) )
		{
			if( m == it->pitchModel() ) { o["unit"] = "cents"; o["path"] = objectHandle( t, "track" ) + "/pitch"; }
			if( m == it->baseNoteModel() ) { o["unit"] = "lmms_key"; o["path"] = objectHandle( t, "track" ) + "/base_note"; }
			if( m == it->pitchRangeModel() ) { o["unit"] = "semitones"; o["path"] = objectHandle( t, "track" ) + "/pitch_range"; }
			if( m == it->effectChannelModel() ) { o["unit"] = "channel_index"; o["path"] = objectHandle( t, "track" ) + "/fx_channel"; }
			if( it->instrument() && it->instrument()->nodeName() == "vestige" && m->parent() == it->instrument() )
			{
				o["unit"] = "normalized";
				o["name"] = "VST parameter " + m->displayName();
				o["vst_parameter_index"] = m->displayName().toInt();
				o["path"] = objectHandle( t, "track" ) + "/vst/parameter[" + m->displayName() + "]";
			}
		}
	}
	for( int i = 0; i < Engine::fxMixer()->numChannels(); ++i )
	{
		FxChannel * channel = Engine::fxMixer()->effectChannel( i );
		QString property;
		if( m == &channel->m_volumeModel ) { property = "volume"; o["unit"] = "linear_gain"; }
		if( m == &channel->m_muteModel ) { property = "muted"; }
		if( m == &channel->m_soloModel ) { property = "solo"; }
		if( !property.isEmpty() ) { o["name"] = channel->m_name + "/" + property; o["path"] = "mixer/channel[" + QString::number( i ) + "]/" + property; }
	}
	// Preserve the exact native value for edits/automation while giving clients
	// a readable float32 display without binary-conversion decimal noise.
	o["display_value"] = o["type"].toString() == "boolean" ? ( o["value"].toBool() ? "true" : "false" ) : QString::number( m->value<float>(), 'g', 7 );
	if( ComboBoxModel * combo = dynamic_cast<ComboBoxModel *>( m ) ) { o["display_value"] = combo->currentText(); }
	return o;
}
QJsonArray parametersJson( QObject * root )
{
	QJsonArray a;
	for( AutomatableModel * m : models( root ) ) { a.append( parameterJson( m ) ); }
	return a;
}

EffectChain * effectChain( const QJsonObject & args, QString & error )
{
	if( args["target_type"].toString( "track" ) == "mixer" )
	{
		const int index = args["target_index"].toInt( args["channel_num"].toInt() );
		if( index < 0 || index >= Engine::fxMixer()->numChannels() ) { error = "mixer channel is outside the current mixer"; return NULL; }
		return &Engine::fxMixer()->effectChannel( index )->m_fxChain;
	}
	Track * t = track( args, error );
	if( InstrumentTrack * it = dynamic_cast<InstrumentTrack *>( t ) ) { return it->audioPort()->effects(); }
	if( SampleTrack * st = dynamic_cast<SampleTrack *>( t ) ) { return st->audioPort()->effects(); }
	if( t ) { error = "effects require an instrument/sample track or mixer channel"; }
	return NULL;
}

bool chainEnabled( EffectChain * chain )
{
	QDomDocument doc;
	QDomElement root = doc.createElement( "fxchain" );
	chain->saveSettings( doc, root );
	return root.attribute( "enabled", "0" ).toInt() != 0;
}

namespace
{

bool sameElement( const QDomElement & a, const QDomElement & b )
{
	if( a.tagName() != b.tagName() || a.attributes().size() != b.attributes().size() ) { return false; }
	for( int i = 0; i < a.attributes().size(); ++i )
	{
		const QDomNode attribute = a.attributes().item( i );
		if( b.attribute( attribute.nodeName() ) != attribute.nodeValue() ) { return false; }
	}
	QDomNode left = a.firstChild(), right = b.firstChild();
	for( ; !left.isNull() && !right.isNull(); left = left.nextSibling(), right = right.nextSibling() )
	{
		if( left.isElement() != right.isElement() ) { return false; }
		if( left.isElement() ? !sameElement( left.toElement(), right.toElement() ) : left.nodeValue() != right.nodeValue() ) { return false; }
	}
	return left.isNull() && right.isNull();
}

QDomElement effectElement( Effect * effect, QDomDocument & doc )
{
	QDomElement root = doc.createElement( "root" );
	const QDomElement e = effect->saveState( doc, root );
	QDomElement element = e;
	element.setAttribute( "name", QString::fromUtf8( effect->descriptor()->name ) );
	QDomElement key = doc.createElement( "key" );
	for( auto it = effect->key().attributes.begin(); it != effect->key().attributes.end(); ++it )
	{
		QDomElement attribute = doc.createElement( "attribute" );
		attribute.setAttribute( "name", it.key() );
		attribute.setAttribute( "value", it.value() );
		key.appendChild( attribute );
	}
	element.appendChild( key );
	return element;
}

}

QVector<Effect *> effects( EffectChain * chain )
{
	QVector<Effect *> candidates, ordered;
	for( QObject * child : chain->children() ) { if( Effect * effect = dynamic_cast<Effect *>( child ) ) { candidates.append( effect ); } }
	QDomDocument doc;
	QDomElement root = doc.createElement( "fxchain" );
	chain->saveSettings( doc, root );
	for( QDomElement item = root.firstChildElement( "effect" ); !item.isNull(); item = item.nextSiblingElement( "effect" ) )
	{
		for( int i = 0; i < candidates.size(); ++i )
		{
			QDomDocument candidate;
			if( sameElement( item, effectElement( candidates[i], candidate ) ) ) { ordered.append( candidates.takeAt( i ) ); break; }
		}
	}
	return ordered;
}

QJsonObject effectJson( Effect * effect, int index )
{
	QJsonObject o;
	o["index"] = index;
	o["plugin"] = QString::fromUtf8( effect->descriptor()->name );
	o["name"] = effect->displayName();
	o["enabled"] = effect->isEnabled();
	o["wet"] = effect->wetLevel();
	if( includeParameters ) { o["parameters"] = parametersJson( effect ); }
	return o;
}
QJsonArray effectList( EffectChain * chain )
{
	QJsonArray a;
	const auto ordered = effects( chain );
	for( int i = 0; i < ordered.size(); ++i ) { a.append( effectJson( ordered[i], i ) ); }
	return a;
}

QJsonObject noteJson( const Note * note, int index, bool midi )
{
	QJsonObject o;
	o["index"] = index;
	o["key"] = note->key() + ( midi ? 12 : 0 );
	o["lmms_key"] = note->key();
	o["midi_key"] = note->key() + 12;
	o["note_name"] = MusicTheory::noteName( note->key() + 12 );
	o["pos"] = int( note->pos() );
	o["length"] = int( note->length() );
	o["len"] = int( note->length() );
	o["volume"] = int( note->getVolume() );
	o["vol"] = int( note->getVolume() );
	o["panning"] = int( note->getPanning() );
	o["pan"] = int( note->getPanning() );
	return o;
}

QJsonObject clipJson( TrackContentObject * tco, int index )
{
	QJsonObject o;
	o["index"] = index;
	o["clip_id"] = double( tco->id() );
	o["clip_handle"] = objectHandle( tco, "clip" );
	o["track_handle"] = objectHandle( tco->getTrack(), "track" );
	o["name"] = tco->name();
	o["start"] = int( tco->startPosition() );
	o["length"] = int( tco->length() );
	o["pos_bars"] = double( int( tco->startPosition() ) ) / MidiTime::ticksPerTact();
	o["length_bars"] = double( int( tco->length() ) ) / MidiTime::ticksPerTact();
	o["muted"] = tco->isMuted();
	o["auto_resize"] = tco->getAutoResize();
	o["name_persistent"] = !dynamic_cast<SampleTCO *>( tco );
	o["kind"] = "other";
	if( Pattern * p = dynamic_cast<Pattern *>( tco ) )
	{
		o["kind"] = "pattern";
		o["pattern_type"] = p->type() == Pattern::BeatPattern ? "beat" : "melody";
		QJsonArray notes;
		if( includeNotes ) { for( int i = 0; i < p->notes().size(); ++i ) { notes.append( noteJson( p->notes()[i], i ) ); } }
		o["note_count"] = p->notes().size();
		if( includeNotes ) { o["notes"] = notes; }
	}
	else if( SampleTCO * sample = dynamic_cast<SampleTCO *>( tco ) )
	{
		o["kind"] = "sample";
		o["file"] = sample->sampleBuffer()->audioFile();
		o["resolved_file_path"] = resolvedSamplePath( sample->sampleBuffer()->audioFile() );
		o["frames"] = double( sample->sampleBuffer()->frames() );
		if( !tco->name().isEmpty() ) { o["warnings"] = QJsonArray() << "LMMS 1.2.2 does not save sample clip names; this name lasts only in the current session"; }
	}
	else if( AutomationPattern * pattern = dynamic_cast<AutomationPattern *>( tco ) )
	{
		o["kind"] = "automation";
		o["name"] = pattern->name();
		o["progression"] = int( pattern->progressionType() );
		o["tension"] = pattern->getTension();
		QJsonArray targets, points;
		for( const auto & m : pattern->objects() ) { if( m ) { targets.append( parameterJson( m ) ); } }
		for( auto it = pattern->getTimeMap().begin(); it != pattern->getTimeMap().end(); ++it )
		{
			QJsonObject point;
			point["pos"] = it.key();
			point["value"] = pattern->firstObject()->scaledValue( it.value() );
			point["raw_value"] = it.value();
			points.append( point );
		}
		o["targets"] = targets;
		o["points"] = points;
	}
	else if( dynamic_cast<BBTCO *>( tco ) ) { o["kind"] = "bb"; }
	return o;
}

QJsonObject trackJson( Track * t, int index )
{
	QJsonObject o;
	o["index"] = index;
	o["track_handle"] = objectHandle( t, "track" );
	o["track_id"] = double( t->id() );
	o["name"] = t->name();
	o["type_code"] = int( t->type() );
	o["muted"] = t->isMuted();
	o["solo"] = t->isSolo();
	o["container"] = t->trackContainer() == Engine::getBBTrackContainer() ? "bb" : "song";
	const char * types[] = { "instrument", "bb", "sample", "event", "video", "automation", "hiddenAutomation" };
	o["type"] = int( t->type() ) >= 0 && int( t->type() ) < 7 ? types[int( t->type() )] : "unknown";
	if( AutomatableModel * m = trackVolume( t ) ) { o["volume"] = m->value<float>(); o["volume_model_id"] = double( m->id() ); }
	if( AutomatableModel * m = trackPanning( t ) ) { o["panning"] = m->value<float>(); o["panning_model_id"] = double( m->id() ); }
	if( InstrumentTrack * it = dynamic_cast<InstrumentTrack *>( t ) )
	{
		o["instrument"] = it->instrumentName();
		o["instrument_id"] = it->instrument() ? QString::fromUtf8( it->instrument()->descriptor()->name ) : QString();
		o["fx_channel"] = it->effectChannelModel()->value();
		o["base_note"] = it->baseNote() + 12;
		o["pitch"] = it->pitchModel()->value();
		o["pitch_range"] = it->pitchRangeModel()->value();
		const QJsonObject vst = vstStatus( it );
		if( !vst.isEmpty() ) { o["vst"] = vst; }
		if( it->instrument() && it->instrument()->nodeName() == "audiofileprocessor" )
		{
			QDomDocument doc; QDomElement root = doc.createElement( "state" );
			QDomElement state = it->instrument()->saveState( doc, root );
			o["file"] = state.attribute( "src" );
			o["resolved_file_path"] = resolvedSamplePath( state.attribute( "src" ) );
		}
	}
	if( BBTrack * bb = dynamic_cast<BBTrack *>( t ) ) { o["bb_index"] = bb->index(); }
	QJsonArray clips;
	if( includeClips )
	{
		for( int i = 0; i < t->getTCOs().size(); ++i ) { clips.append( clipJson( t->getTCOs()[i], i ) ); }
		o["tcos"] = clips;
	}
	o["clip_count"] = t->getTCOs().size();
	return o;
}

QJsonArray mixerJson()
{
	QJsonArray a;
	for( int i = 0; i < Engine::fxMixer()->numChannels(); ++i )
	{
		a.append( mixerChannelJson( i ) );
	}
	return a;
}

QJsonObject mixerChannelJson( int i )
{
		FxChannel * channel = Engine::fxMixer()->effectChannel( i );
		QJsonObject o;
		o["channel_num"] = i;
		o["name"] = channel->m_name;
		o["volume"] = channel->m_volumeModel.value();
		o["volume_model_id"] = double( channel->m_volumeModel.id() );
		o["muted"] = channel->m_muteModel.value();
		o["solo"] = channel->m_soloModel.value();
		QJsonArray sends;
		for( FxRoute * route : channel->m_sends )
		{
			QJsonObject send;
			send["target"] = int( route->receiverIndex() );
			send["amount"] = route->amount()->value();
			send["model_id"] = double( route->amount()->id() );
			sends.append( send );
		}
		o["sends"] = sends;
		o["effects"] = effectList( &channel->m_fxChain );
		return o;
}

void refreshMixer()
{
	prepareMixerSolo();
	if( gui && gui->fxMixerView() ) { gui->fxMixerView()->refreshDisplay(); }
	prepareMixerSolo();
}
void refreshPattern( Pattern * p, int minimumLength )
{
	p->rearrangeAllNotes();
	p->updateLength();
	if( int( p->length() ) < minimumLength ) { p->changeLength( MidiTime( minimumLength ) ); }
	p->dataChanged();
}

QString pluginError( const QString & name, Plugin::PluginTypes type )
{
	const auto info = pluginFactory->pluginInfo( name.toUtf8().constData() );
	return info.isNull() || !info.descriptor || info.descriptor->type != type ? "plugin is not available for this type: " + name : QString();
}
QJsonArray plugins( Plugin::PluginTypes type )
{
	QJsonArray a;
	for( const auto & info : pluginFactory->pluginInfos() )
	{
		if( !info.descriptor || info.descriptor->type != type ) { continue; }
		QJsonObject o;
		o["name"] = QString::fromUtf8( info.descriptor->name );
		o["display_name"] = QString::fromUtf8( info.descriptor->displayName );
		o["description"] = QString::fromUtf8( info.descriptor->description );
		o["file"] = info.file.absoluteFilePath();
		o["supported_files"] = QString::fromUtf8( info.descriptor->supportedFileTypes ? info.descriptor->supportedFileTypes : "" );
		o["requires_subplugin_key"] = info.descriptor->subPluginFeatures != NULL;
		a.append( o );
	}
	return a;
}

QString audioPath( const QString & path, QString & error )
{
	const QFileInfo file( path );
	if( !file.isAbsolute() || !file.isFile() || !file.isReadable() || path.contains( QChar( 0 ) ) ) { error = "audio file must be a readable absolute file path"; return QString(); }
	if( file.size() > 64 * 1024 * 1024 ) { error = "sample exceeds the 64 MiB limit"; return QString(); }
	if( file.suffix().toLower() != "ds" )
	{
		AudioCodec::Library library;
		const QJsonObject info = library.inspect( path, error );
		if( error.isEmpty() && ( info["seconds"].toDouble() > 300 || info["channels"].toInt() > 32 ||
			info["frames"].toDouble() * qMax( 2, info["channels"].toInt() ) * sizeof( float ) > 256 * 1024 * 1024 ) )
		{
			error = "sample exceeds the bridge's 300-second/256-MiB decoded-audio limit";
		}
		if( !error.isEmpty() ) { return QString(); }
	}
	return file.absoluteFilePath();
}

QString vstPath( const QString & path, QString & error )
{
	const QFileInfo info( path );
	QFile file( path );
	if( !info.isAbsolute() || info.suffix().toLower() != "dll" || !file.open( QIODevice::ReadOnly ) ) { error = "VST path must be a readable absolute .dll path"; return QString(); }
	const QByteArray header = file.read( 64 );
	if( header.size() != 64 || header.left( 2 ) != "MZ" ) { error = "VST file is not a Windows PE DLL"; return QString(); }
	const quint32 offset = qFromLittleEndian<quint32>( reinterpret_cast<const uchar *>( header.constData() + 60 ) );
	if( !file.seek( offset ) ) { error = "invalid VST PE header"; return QString(); }
	const QByteArray pe = file.read( 24 );
	if( pe.size() != 24 || pe.left( 4 ) != QByteArray( "PE\0\0", 4 ) ||
		qFromLittleEndian<quint16>( reinterpret_cast<const uchar *>( pe.constData() + 4 ) ) != 0x8664 ||
		!( qFromLittleEndian<quint16>( reinterpret_cast<const uchar *>( pe.constData() + 22 ) ) & 0x2000 ) )
	{
		error = "this bridge currently requires a Windows x64 VST2 DLL (not VST3)";
		return QString();
	}
	return info.absoluteFilePath();
}

}
