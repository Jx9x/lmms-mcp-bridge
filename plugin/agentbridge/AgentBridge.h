/*
 * AgentBridge.h - LMMS Tool plugin exposing a text/JSON command bridge
 *                 over a named pipe, plus its ToolPluginView.
 */

#ifndef AGENT_BRIDGE_H
#define AGENT_BRIDGE_H

#include <QtCore/QFile>
#include <QtCore/QMetaObject>
#include <QtCore/QMutex>
#include <QtCore/QPointer>
#include <QtCore/QString>

#include "ToolPlugin.h"
#include "ToolPluginView.h"

class QLabel;
class QPlainTextEdit;
class QTimer;

class BridgeCommands;
class MainThreadDispatcher;
class PipeServer;

extern "C"
{
	extern Plugin::Descriptor agentbridge_plugin_descriptor;
}


namespace agentbridge
{
	QPixmap getIconPixmap( const char * _name, int _w, int _h );
}


class AgentBridgeView;


class AgentBridge : public ToolPlugin
{
public:
	AgentBridge();
	virtual ~AgentBridge();

	virtual PluginView * instantiateView( QWidget * _parent );

	virtual QString nodeName() const;

	virtual void saveSettings( QDomDocument & _doc,
								QDomElement & _element )
	{
		Q_UNUSED( _doc )
		Q_UNUSED( _element )
	}

	virtual void loadSettings( const QDomElement & _element )
	{
		Q_UNUSED( _element )
	}

	// thread-safe logging; callable from any thread
	void log( const QString & _msg );

	// main thread only
	void appendLogLine( const QString & _line );
	void setStatusText( const QString & _state );
	void showView();
	void requestSelfTest();

	QString pipeName() const;
	QString logPath() const;

	// self-test check 16: all ctor-made signal connections are valid
	bool signalConnectionsOk() const;
	QString signalConnectionsDetail() const;

	static AgentBridge * instance()
	{
		return s_instance;
	}

private:
	void openLog();
	void runSelfTestCheck();

	static AgentBridge * s_instance;

	QFile m_logFile;
	QMutex m_logMutex;
	MainThreadDispatcher * m_dispatcher;
	BridgeCommands * m_commands;
	PipeServer * m_pipe;
	QPointer<AgentBridgeView> m_view;
	QTimer * m_selfTestTimer;
	int m_selfTestTries;
	QMetaObject::Connection m_connPlayback;
	QMetaObject::Connection m_connTempo;
	QMetaObject::Connection m_connProjectLoaded;
	QMetaObject::Connection m_connTrackAdded;
} ;


class AgentBridgeView : public ToolPluginView
{
public:
	AgentBridgeView( ToolPlugin * _tool );
	virtual ~AgentBridgeView();

	void appendLogLine( const QString & _line );
	void setStatus( const QString & _pipeName, const QString & _state,
					const QString & _logPath );

private:
	QLabel * m_status;
	QPlainTextEdit * m_log;
} ;

#endif
