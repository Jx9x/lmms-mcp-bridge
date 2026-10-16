#ifndef AGENTBRIDGE_AUDIO_CODECS_H
#define AGENTBRIDGE_AUDIO_CODECS_H

#include <QtCore/QIODevice>
#include <QtCore/QLibrary>
#include <QtCore/QJsonObject>

namespace AudioCodec
{

typedef qint64 Count;
struct Info
{
	Count frames;
	int samplerate;
	int channels;
	int format;
	int sections;
	int seekable;
};

struct VirtualIO
{
	Count ( *length )( void * );
	Count ( *seek )( Count, int, void * );
	Count ( *read )( void *, Count, void * );
	Count ( *write )( const void *, Count, void * );
	Count ( *tell )( void * );
};

class Library
{
public:
	Library();
	QLibrary soundFile;
	QLibrary mp3;

	void * ( *open )( VirtualIO *, int, Info *, void * );
	Count ( *writeFrames )( void *, const float *, Count );
	int ( *close )( void * );
	const char * ( *error )( void * );
	int ( *checkFormat )( const Info * );

	bool available() const;
	static VirtualIO io();
	QJsonObject inspect( const QString & path, QString & errorText );
};

}

#endif
