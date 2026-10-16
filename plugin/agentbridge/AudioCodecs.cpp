#include "AudioCodecs.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace AudioCodec
{
namespace
{

Count length( void * device ) { return static_cast<QIODevice *>( device )->size(); }
Count tell( void * device ) { return static_cast<QIODevice *>( device )->pos(); }
Count read( void * data, Count count, void * device ) { return static_cast<QIODevice *>( device )->read( static_cast<char *>( data ), count ); }
Count write( const void * data, Count count, void * device ) { return static_cast<QIODevice *>( device )->write( static_cast<const char *>( data ), count ); }
Count seek( Count offset, int origin, void * device )
{
	QIODevice * file = static_cast<QIODevice *>( device );
	const Count target = offset + ( origin == 1 ? file->pos() : origin == 2 ? file->size() : 0 );
	return file->seek( target ) ? file->pos() : -1;
}

}

Library::Library() :
	soundFile( QCoreApplication::applicationDirPath() + "/libsndfile-1.dll" ),
	mp3( QCoreApplication::applicationDirPath() + "/libmp3lame-0.dll" ),
	open( reinterpret_cast<decltype( open )>( soundFile.resolve( "sf_open_virtual" ) ) ),
	writeFrames( reinterpret_cast<decltype( writeFrames )>( soundFile.resolve( "sf_writef_float" ) ) ),
	close( reinterpret_cast<decltype( close )>( soundFile.resolve( "sf_close" ) ) ),
	error( reinterpret_cast<decltype( error )>( soundFile.resolve( "sf_strerror" ) ) ),
	checkFormat( reinterpret_cast<decltype( checkFormat )>( soundFile.resolve( "sf_format_check" ) ) )
{
}

bool Library::available() const { return open && writeFrames && close && error && checkFormat; }
VirtualIO Library::io() { return VirtualIO{ length, seek, read, write, tell }; }

QJsonObject Library::inspect( const QString & path, QString & errorText )
{
	QJsonObject o;
	if( !available() ) { errorText = "installed libsndfile API is unavailable"; return o; }
	QFile file( path );
	if( !file.open( QIODevice::ReadOnly ) ) { errorText = file.errorString(); return o; }
	Info info = {};
	VirtualIO callbacks = io();
	void * handle = open( &callbacks, 0x10, &info, &file );
	if( !handle ) { errorText = QString::fromLocal8Bit( error( NULL ) ); return o; }
	close( handle );
	if( info.frames <= 0 || info.samplerate <= 0 || info.channels <= 0 ) { errorText = "audio file has invalid frame metadata"; return o; }
	o["path"] = QFileInfo( path ).absoluteFilePath();
	o["frames"] = double( info.frames );
	o["sample_rate"] = info.samplerate;
	o["channels"] = info.channels;
	o["seconds"] = double( info.frames ) / info.samplerate;
	o["format_code"] = info.format;
	return o;
}

}
