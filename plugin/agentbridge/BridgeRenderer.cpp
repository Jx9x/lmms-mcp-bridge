#include "BridgeRenderer.h"
#include "AudioCodecs.h"
#include "BridgeModel.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <cmath>
#include <windows.h>

#include "AudioDevice.h"
#include "BBTrack.h"
#include "BBTrackContainer.h"
#include "Engine.h"
#include "ExportFilter.h"
#include "GuiApplication.h"
#include "MainWindow.h"
#include "Mixer.h"
#include "Song.h"

namespace
{

QString destinationError( const QString & path, bool overwrite )
{
	const QFileInfo file( path );
	if( !QDir( file.absolutePath() ).exists() ) { return "output directory does not exist"; }
	if( file.exists() && ( !file.isFile() || file.isSymLink() ) ) { return "output destination is not a regular file"; }
	return file.exists() && !overwrite ? "destination exists: set overwrite=true to replace it" : QString();
}

QString commitOutput( const QString & source, const QString & path, bool overwrite )
{
	const QString error = destinationError( path, overwrite );
	if( !error.isEmpty() ) { return error; }
	const QString from = QDir::toNativeSeparators( source ), to = QDir::toNativeSeparators( path );
	if( !MoveFileExW( reinterpret_cast<LPCWSTR>( from.utf16() ), reinterpret_cast<LPCWSTR>( to.utf16() ),
		MOVEFILE_WRITE_THROUGH | ( overwrite ? MOVEFILE_REPLACE_EXISTING : 0 ) ) )
	{
		return QString( "cannot commit output (Windows error %1); the previous destination was not replaced" ).arg( GetLastError() );
	}
	return QString();
}

class FileDevice : public AudioDevice
{
public:
	FileDevice( const QString & path, const QString & format, int rate, int depth, int bitrate, int quality ) :
		AudioDevice( 2, Engine::mixer() ), file( path ), format( format ), handle( NULL ), lame( NULL ),
		encode( NULL ), flush( NULL ), closeLame( NULL ), frames( 0 ), peak( 0 ), squares( 0 )
	{
		setSampleRate( rate );
		if( !file.open( QIODevice::WriteOnly ) ) { error = file.errorString(); return; }
		if( format == "mp3" )
		{
			auto init = reinterpret_cast<void * ( * )()>( codecs.mp3.resolve( "lame_init" ) );
			auto channels = reinterpret_cast<int ( * )( void *, int )>( codecs.mp3.resolve( "lame_set_num_channels" ) );
			auto sampleRate = reinterpret_cast<int ( * )( void *, int )>( codecs.mp3.resolve( "lame_set_in_samplerate" ) );
			auto setBitrate = reinterpret_cast<int ( * )( void *, int )>( codecs.mp3.resolve( "lame_set_brate" ) );
			auto setQuality = reinterpret_cast<int ( * )( void *, int )>( codecs.mp3.resolve( "lame_set_quality" ) );
			auto params = reinterpret_cast<int ( * )( void * )>( codecs.mp3.resolve( "lame_init_params" ) );
			encode = reinterpret_cast<decltype( encode )>( codecs.mp3.resolve( "lame_encode_buffer_interleaved_ieee_float" ) );
			flush = reinterpret_cast<decltype( flush )>( codecs.mp3.resolve( "lame_encode_flush" ) );
			closeLame = reinterpret_cast<decltype( closeLame )>( codecs.mp3.resolve( "lame_close" ) );
			if( !init || !channels || !sampleRate || !setBitrate || !setQuality || !params || !encode || !flush || !closeLame )
			{
				error = "installed MP3 encoder API is unavailable"; return;
			}
			lame = init();
			if( !lame || channels( lame, 2 ) < 0 || sampleRate( lame, rate ) < 0 || setBitrate( lame, bitrate ) < 0 ||
				setQuality( lame, quality ) < 0 || params( lame ) < 0 ) { error = "MP3 encoder rejected its settings"; }
		}
		else
		{
			if( !codecs.available() ) { error = "installed libsndfile encoder API is unavailable"; return; }
			AudioCodec::Info info = {};
			info.samplerate = rate;
			info.channels = 2;
			info.format = format == "ogg" ? 0x200060 : ( format == "flac" ? 0x170000 : 0x010000 ) | ( depth == 16 ? 2 : depth == 24 ? 3 : 6 );
			if( !codecs.checkFormat( &info ) ) { error = "installed encoder does not support this format/sample rate/bit depth"; return; }
			callbacks = AudioCodec::Library::io();
			handle = codecs.open( &callbacks, 0x20, &info, &file );
			if( !handle ) { error = QString::fromLocal8Bit( codecs.error( NULL ) ); }
		}
	}

	virtual ~FileDevice() { finish( false ); }

	QString finish( bool complete )
	{
		if( handle )
		{
			if( codecs.close( handle ) && error.isEmpty() ) { error = "audio encoder could not finalize the file"; }
			handle = NULL;
		}
		if( lame )
		{
			if( complete && error.isEmpty() ) { encoded.resize( 8192 ); writeEncoded( flush( lame, reinterpret_cast<unsigned char *>( encoded.data() ), encoded.size() ) ); }
			closeLame( lame );
			lame = NULL;
		}
		if( file.isOpen() )
		{
			if( !file.flush() && error.isEmpty() ) { error = file.errorString(); }
			file.close();
		}
		return error;
	}

	QFile file;
	QString format, error;
	AudioCodec::Library codecs;
	AudioCodec::VirtualIO callbacks;
	void * handle;
	void * lame;
	int ( *encode )( void *, const float *, int, unsigned char *, int );
	int ( *flush )( void *, unsigned char *, int );
	int ( *closeLame )( void * );
	qint64 frames;
	double peak, squares;
	QVector<float> samples;
	QByteArray encoded;

protected:
	virtual void writeBuffer( const surroundSampleFrame * buffer, const fpp_t count, const float gain )
	{
		if( !error.isEmpty() ) { return; }
		samples.resize( count * 2 );
		for( fpp_t i = 0; i < count; ++i )
		{
			for( int channel = 0; channel < 2; ++channel )
			{
				const float value = buffer[i][channel] * gain;
				if( !std::isfinite( value ) ) { error = "instrument/effect produced nonfinite audio"; return; }
				const float clipped = Mixer::clip( value );
				samples[i * 2 + channel] = clipped;
				peak = qMax( peak, double( std::abs( clipped ) ) );
				squares += double( clipped ) * clipped;
			}
		}
		if( handle && codecs.writeFrames( handle, samples.constData(), count ) != count ) { error = "audio encoder did not write the complete buffer"; }
		if( lame )
		{
			encoded.resize( count * 2 + 7200 );
			writeEncoded( encode( lame, samples.constData(), count, reinterpret_cast<unsigned char *>( encoded.data() ), encoded.size() ) );
		}
		frames += count;
	}

private:
	void writeEncoded( int bytes )
	{
		if( bytes < 0 ) { error = "MP3 encoder failed"; }
		else if( bytes && file.write( encoded.constData(), bytes ) != bytes ) { error = file.errorString(); }
	}
};

QString midiSafetyError()
{
	const auto songTracks = Engine::getSong()->tracks();
	const auto bbTracks = Engine::getBBTrackContainer()->tracks();
	QVector<BBTrack *> columns;
	for( Track * t : songTracks ) { if( BBTrack * bb = dynamic_cast<BBTrack *>( t ) ) { columns.append( bb ); } }
	bool hasInstrument = false;
	for( Track * t : songTracks + bbTracks )
	{
		InstrumentTrack * it = dynamic_cast<InstrumentTrack *>( t );
		if( !it ) { continue; }
		hasInstrument = true;
		qint64 notes = 0;
		if( t->name().toUtf8().size() > 1024 ) { return "native MIDI exporter requires track names under 1024 UTF-8 bytes"; }
		if( t->trackContainer() == Engine::getSong() )
		{
			for( TrackContentObject * clip : t->getTCOs() ) { if( Pattern * p = dynamic_cast<Pattern *>( clip ) ) { notes += p->notes().size(); } }
		}
		else
		{
			if( t->getTCOs().size() != columns.size() ) { return "BB pattern columns do not match the song's BB tracks; native MIDI export would be unsafe"; }
			for( int column = 0; column < columns.size(); ++column )
			{
				Pattern * p = dynamic_cast<Pattern *>( t->getTCOs()[column] );
				if( !p ) { return "BB column is not a note pattern"; }
				QDomDocument doc;
				QDomElement root = doc.createElement( "root" );
				const int steps = p->saveState( doc, root ).attribute( "steps", "0" ).toInt();
				if( steps <= 0 || steps > MaxSongLength / 12 ) { return "BB pattern has no valid step length for native MIDI export"; }
				for( TrackContentObject * clip : columns[column]->getTCOs() )
				{
					notes += qint64( p->notes().size() ) * ( 2 + ( int( clip->length() ) + steps * 12 - 1 ) / ( steps * 12 ) );
				}
			}
		}
		if( notes > 2000 ) { return "native LMMS 1.2.2 MIDI exporter has a fixed buffer; export is limited to 2000 expanded notes per instrument track"; }
	}
	return hasInstrument ? QString() : "song has no native instrument tracks to export";
}

}

struct BridgeRenderer::State
{
	explicit State( QObject * owner ) : timer( owner ), device( NULL ), temporary( NULL ), original( NULL ),
		quality( Mixer::qualitySettings::Mode_Draft ), milliseconds( 0 ), endTick( 0 ), tailFrames( -1 ), overwrite( false ), windowEnabled( true ), nextId( 0 )
	{
		job["state"] = "idle";
		QObject::connect( &timer, &QTimer::timeout, owner, [this]() { step(); } );
	}

	QTimer timer;
	FileDevice * device;
	QTemporaryDir * temporary;
	AudioDevice * original;
	Mixer::qualitySettings quality;
	Song::PlayPos position;
	int milliseconds, endTick;
	qint64 tailFrames;
	bool overwrite, windowEnabled;
	int nextId;
	QJsonObject job;
	QVector<QPair<QPointer<AutomatableModel>, float> > values;

	void step()
	{
		QElapsedTimer slice;
		slice.start();
		Song * song = Engine::getSong();
		while( device && slice.elapsed() < 15 )
		{
			if( !song->isExporting() ) { finish( "cancelled" ); return; }
			const qint64 before = device->frames;
			device->processNextBuffer();
			job["frames"] = double( device->frames );
			job["progress"] = qMin( 0.99, double( song->getPlayPos( Song::Mode_PlaySong ).getTicks() ) / qMax( 1, endTick ) );
			if( !device->error.isEmpty() ) { finish( "failed", device->error ); return; }
			if( device->frames == before ) { finish( "failed", "native mixer returned no audio frames" ); return; }
			if( device->frames > qint64( device->sampleRate() ) * 3600 || device->file.size() > 2LL * 1024 * 1024 * 1024 )
			{
				finish( "failed", "render exceeds the one-hour/2-GiB output limit" ); return;
			}
			if( tailFrames >= 0 )
			{
				tailFrames -= device->frames - before;
				if( tailFrames <= 0 ) { finish( "succeeded" ); return; }
			}
			else if( song->getPlayPos( Song::Mode_PlaySong ).getTicks() >= endTick )
			{
				tailFrames = qint64( std::ceil( job["tail_seconds"].toDouble() * device->sampleRate() ) );
				if( !tailFrames ) { finish( "succeeded" ); return; }
			}
		}
	}

	void finish( QString state, QString error = QString() )
	{
		if( !device ) { return; }
		timer.stop();
		const QString staged = device->file.fileName();
		const QString encoderError = device->finish( state == "succeeded" );
		if( error.isEmpty() && !encoderError.isEmpty() ) { error = encoderError; state = "failed"; }
		job["frames"] = double( device->frames );
		job["duration_seconds"] = double( device->frames ) / device->sampleRate();
		job["peak"] = device->peak;
		job["rms"] = device->frames ? std::sqrt( device->squares / ( 2 * device->frames ) ) : 0;
		Mixer * mixer = Engine::mixer();
		mixer->setAudioDevice( original, quality, true, false );
		device = NULL;
		Song * song = Engine::getSong();
		song->stopExport();
		for( const auto & saved : values )
		{
			if( !saved.first ) { continue; }
			saved.first->saveJournallingState( false );
			saved.first->setValue( saved.second );
			saved.first->restoreJournallingState();
		}
		values.clear();
		song->updateLength();
		song->getPlayPos( Song::Mode_PlaySong ) = position;
		song->setMilliSeconds( milliseconds );
		emit song->playbackStateChanged();
		mixer->setAudioDevice( original, quality, true, true );
		mixer->restoreAudioDevice();
		original = NULL;
		if( gui && gui->mainWindow() ) { gui->mainWindow()->setEnabled( windowEnabled ); }
		if( state == "succeeded" )
		{
			error = commitOutput( staged, job["path"].toString(), overwrite );
			if( !error.isEmpty() ) { state = "failed"; }
			else { job["bytes"] = double( QFileInfo( job["path"].toString() ).size() ); job["progress"] = 1; }
		}
		job["state"] = state;
		if( !error.isEmpty() ) { job["error"] = error; }
		delete temporary;
		temporary = NULL;
	}
};

BridgeRenderer::BridgeRenderer( QObject * parent ) : QObject( parent ), m_state( new State( this ) ) {}
BridgeRenderer::~BridgeRenderer() { m_state->finish( "cancelled" ); delete m_state; }
bool BridgeRenderer::busy() const { return m_state->device != NULL; }
QJsonObject BridgeRenderer::formats()
{
	AudioCodec::Library codecs;
	QJsonObject formats;
	AudioCodec::Info info = {};
	info.samplerate = 44100;
	info.channels = 2;
	const QMap<QString, int> codes = { { "wav", 0x010002 }, { "ogg", 0x200060 }, { "flac", 0x170002 } };
	for( auto it = codes.begin(); it != codes.end(); ++it )
	{
		info.format = it.value();
		formats[it.key()] = codecs.available() && codecs.checkFormat( &info );
	}
	bool mp3 = true;
	for( const char * symbol : { "lame_init", "lame_set_num_channels", "lame_set_in_samplerate", "lame_set_brate", "lame_set_quality", "lame_init_params", "lame_encode_buffer_interleaved_ieee_float", "lame_encode_flush", "lame_close" } )
	{
		mp3 = codecs.mp3.resolve( symbol ) && mp3;
	}
	formats["mp3"] = mp3;
	return formats;
}
QJsonObject BridgeRenderer::status() const { QJsonObject o = BridgeModel::reply(); o["job"] = m_state->job; return o; }
QJsonObject BridgeRenderer::cancel() { if( !busy() ) { return BridgeModel::reply( false, "no bridge render is active" ); } m_state->finish( "cancelled" ); return status(); }

QJsonObject BridgeRenderer::start( const QJsonObject & args )
{
	State & s = *m_state;
	Song * song = Engine::getSong();
	if( busy() || song->isExporting() ) { return BridgeModel::reply( false, "a render is already active" ); }
	if( !song->isStopped() ) { return BridgeModel::reply( false, "stop playback before rendering" ); }
	const QString path = args["path"].toString();
	const bool overwrite = args["overwrite"].toBool();
	QString error = destinationError( path, overwrite );
	if( !error.isEmpty() ) { return BridgeModel::reply( false, error ); }
	const QString format = args["format"].toString( QFileInfo( path ).suffix().toLower() );
	if( !QStringList{ "wav", "ogg", "flac", "mp3" }.contains( format ) || QFileInfo( path ).suffix().toLower() != format )
	{
		return BridgeModel::reply( false, "path extension must match wav, ogg, flac, or mp3 format" );
	}
	const int rate = args["sample_rate"].toInt( 44100 ), depth = args["bit_depth"].toInt( 16 );
	if( depth != 16 && depth != 24 && depth != 32 ) { return BridgeModel::reply( false, "bit_depth must be 16, 24, or 32" ); }
	if( format == "flac" && depth == 32 ) { return BridgeModel::reply( false, "FLAC supports 16/24-bit output, not 32-bit float" ); }
	if( format == "mp3" && !QSet<int>{ 22050, 24000, 32000, 44100, 48000 }.contains( rate ) ) { return BridgeModel::reply( false, "MP3 sample_rate must be 22050, 24000, 32000, 44100, or 48000" ); }
	song->updateLength();
	if( song->length() <= 0 ) { return BridgeModel::reply( false, "song has no arrangement clips to render" ); }
	s.temporary = new QTemporaryDir( QFileInfo( path ).absolutePath() + "/.lmms-agent-render-XXXXXX" );
	if( !s.temporary->isValid() ) { delete s.temporary; s.temporary = NULL; return BridgeModel::reply( false, "cannot create temporary output in the destination directory" ); }
	s.device = new FileDevice( s.temporary->path() + "/audio." + format, format, rate, depth, args["bitrate"].toInt( 192 ), args["quality"].toInt( 2 ) );
	if( !s.device->error.isEmpty() )
	{
		error = s.device->error;
		delete s.device; s.device = NULL;
		delete s.temporary; s.temporary = NULL;
		return BridgeModel::reply( false, error );
	}
	s.job = QJsonObject{ { "job_id", ++s.nextId }, { "state", "running" }, { "path", path }, { "format", format },
		{ "sample_rate", rate }, { "channels", 2 }, { "frames", 0 }, { "progress", 0 }, { "tail_seconds", args["tail_seconds"].toDouble( 2 ) } };
	s.overwrite = overwrite;
	s.tailFrames = -1;
	s.position = song->getPlayPos( Song::Mode_PlaySong );
	s.milliseconds = song->getMilliseconds();
	QSet<AutomatableModel *> seen;
	for( AutomatableModel * m : BridgeModel::projectModels() )
	{
		if( seen.contains( m ) ) { continue; }
		seen.insert( m );
		float value = m->value<float>();
		if( m->controllerConnection() || m->hasLinkedModels() )
		{
			QDomDocument doc;
			QDomElement root = doc.createElement( "parameter" );
			m->saveSettings( doc, root, "value" );
			value = ( root.hasAttribute( "value" ) ? root.attribute( "value" ) : root.firstChildElement( "value" ).attribute( "value" ) ).toFloat();
		}
		s.values.append( qMakePair( QPointer<AutomatableModel>( m ), value ) );
	}
	Mixer * mixer = Engine::mixer();
	s.original = mixer->audioDev();
	s.quality = mixer->currentQualitySettings();
	mixer->storeAudioDevice();
	mixer->setAudioDevice( s.device, Mixer::qualitySettings( Mixer::qualitySettings::Interpolation_SincBest, Mixer::qualitySettings::Oversampling_None ), false, false );
	if( gui && gui->mainWindow() ) { s.windowEnabled = gui->mainWindow()->isEnabled(); gui->mainWindow()->setEnabled( false ); }
	song->setRenderBetweenMarkers( false );
	song->setExportLoop( true );
	song->startExport();
	song->updateLength();
	s.endTick = song->getExportEndpoints().second.getTicks();
	mixer->nextBuffer();
	s.timer.start( 1 );
	return status();
}

QJsonObject BridgeRenderer::exportMidi( const QJsonObject & args )
{
	const QString path = args["path"].toString();
	if( QFileInfo( path ).suffix().toLower() != "mid" && QFileInfo( path ).suffix().toLower() != "midi" ) { return BridgeModel::reply( false, "MIDI output must end in .mid or .midi" ); }
	QString error = destinationError( path, args["overwrite"].toBool() );
	if( error.isEmpty() ) { error = midiSafetyError(); }
	if( !error.isEmpty() ) { return BridgeModel::reply( false, error ); }
	QTemporaryDir temporary( QFileInfo( path ).absolutePath() + "/.lmms-agent-midi-XXXXXX" );
	if( !temporary.isValid() ) { return BridgeModel::reply( false, "cannot create temporary MIDI output in the destination directory" ); }
	const QString staged = temporary.path() + "/song.mid";
	Plugin * plugin = Plugin::instantiate( "midiexport", NULL, NULL );
	ExportFilter * filter = dynamic_cast<ExportFilter *>( plugin );
	if( !filter ) { delete plugin; return BridgeModel::reply( false, "native midiexport plugin is unavailable" ); }
	Song * song = Engine::getSong();
	const bool exported = filter->tryExport( song->tracks(), Engine::getBBTrackContainer()->tracks(), song->getTempo(), song->masterPitch(), staged );
	delete filter;
	QFile file( staged );
	if( !exported || !file.open( QIODevice::ReadOnly ) || file.read( 4 ) != "MThd" || file.size() < 14 ) { return BridgeModel::reply( false, "native MIDI export did not produce a valid header" ); }
	file.close();
	error = commitOutput( staged, path, args["overwrite"].toBool() );
	if( !error.isEmpty() ) { return BridgeModel::reply( false, error ); }
	QJsonObject o = BridgeModel::reply();
	o["path"] = path;
	o["bytes"] = double( QFileInfo( path ).size() );
	o["limitations"] = "native LMMS 1.2.2 MIDI semantics: notes and static tempo/pitch/track volume; no audio, effects, parameter automation, mute/solo, or MIDI program selection";
	return o;
}
