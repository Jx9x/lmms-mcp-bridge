#ifndef AGENTBRIDGE_MODEL_H
#define AGENTBRIDGE_MODEL_H

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QVector>
#include <QtCore/QScopedValueRollback>

#include "Track.h"
#include "InstrumentTrack.h"
#include "Pattern.h"
#include "Effect.h"
#include "EffectChain.h"

namespace BridgeModel
{

// Serialization is scoped to one main-thread request. Native callers keep full
// detail by default; the MCP adapter requests compact objects explicitly.
class JsonDetail
{
public:
	JsonDetail( const QJsonObject & args );
private:
	QScopedValueRollback<bool> m_notes, m_parameters, m_clips;
};

QString projectToken();
void resetProjectHandles();
QString objectHandle( QObject * object, const QString & kind );
QJsonObject handleContext();
QString resolvedSamplePath( const QString & path );
QJsonObject vstStatus( InstrumentTrack * track );
void prepareMixerSolo();
bool setMixerSolo( int index, bool value );
bool setModelValue( AutomatableModel * model, double value );
TrackContentObject * duplicateClip( TrackContentObject * source, int position );

class Change
{
public:
	Change();
	~Change();
};

struct NoteTarget
{
	InstrumentTrack * track;
	Pattern * pattern;
	int index;
};

struct NoteData
{
	int key;
	int pos;
	int length;
	int volume;
	int panning;
};

QJsonObject reply( bool ok = true, const QString & error = QString() );
TrackContainer * container( const QJsonObject & args );
Track * track( const QJsonObject & args, QString & error );
QString removeTrack( Track * track );
InstrumentTrack * instrument( const QJsonObject & args, QString & error );
NoteTarget noteTarget( const QJsonObject & args, QString & error, bool allowMissing = false );
Pattern * ensurePattern( NoteTarget & target );
NoteData noteData( const QJsonObject & args, QString & error, const Note * defaults = NULL );
TrackContentObject * clip( const QJsonObject & args, QString & error );
int ticks( const QJsonObject & args, const char * ticksName, const char * barsName, QString & error, int fallback = 0 );

AutomatableModel * namedModel( QObject * root, const QString & context, const QString & name );
AutomatableModel * trackVolume( Track * track );
AutomatableModel * trackPanning( Track * track );
QVector<AutomatableModel *> models( QObject * root );
QVector<AutomatableModel *> projectModels();
AutomatableModel * model( double id, QString & error );
QString valueError( AutomatableModel * model, double value );
QJsonObject parameterJson( AutomatableModel * model );
QJsonArray parametersJson( QObject * root );
EffectChain * effectChain( const QJsonObject & args, QString & error );
bool chainEnabled( EffectChain * chain );
QVector<Effect *> effects( EffectChain * chain );
QJsonObject effectJson( Effect * effect, int index );
QJsonArray effectList( EffectChain * chain );
QJsonObject noteJson( const Note * note, int index, bool midi = false );
QJsonObject clipJson( TrackContentObject * clip, int index );
QJsonObject trackJson( Track * track, int index );
QJsonArray mixerJson();
QJsonObject mixerChannelJson( int index );
void refreshMixer();
void refreshPattern( Pattern * pattern, int minimumLength );
QString pluginError( const QString & name, Plugin::PluginTypes type );
QJsonArray plugins( Plugin::PluginTypes type );
QString audioPath( const QString & path, QString & error );
QString vstPath( const QString & path, QString & error );

}

#endif
