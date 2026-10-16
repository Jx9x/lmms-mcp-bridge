#ifndef AGENTBRIDGE_RENDERER_H
#define AGENTBRIDGE_RENDERER_H

#include <QtCore/QJsonObject>
#include <QtCore/QObject>

class BridgeRenderer : public QObject
{
public:
	explicit BridgeRenderer( QObject * parent );
	virtual ~BridgeRenderer();
	bool busy() const;
	static QJsonObject formats();
	QJsonObject start( const QJsonObject & args );
	QJsonObject status() const;
	QJsonObject cancel();
	QJsonObject exportMidi( const QJsonObject & args );

private:
	struct State;
	State * m_state;
};

#endif
