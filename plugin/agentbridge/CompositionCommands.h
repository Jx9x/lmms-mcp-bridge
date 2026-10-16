#ifndef AGENTBRIDGE_COMPOSITION_COMMANDS_H
#define AGENTBRIDGE_COMPOSITION_COMMANDS_H

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>

class BridgeRenderer;

class CompositionCommands : public QObject
{
public:
	CompositionCommands();
	virtual ~CompositionCommands();

	static QJsonArray tools();
	static QString validate( const QString & command, const QJsonObject & args );
	QJsonObject execute( const QString & command, const QJsonObject & args, bool & modified );
	bool isRendering() const;
	int playbackVolumeCap() const { return m_playbackVolumeCap; }

private:
	BridgeRenderer * m_renderer;
	int m_playbackVolumeCap;
};

#endif
