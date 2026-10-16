/*
 * PipeServer.h - Win32 named-pipe server running on its own QThread plus the
 *                main-thread dispatch machinery used to marshal requests onto
 *                the LMMS (GUI) thread.
 *
 * This translation unit is intentionally free of LMMS headers: everything it
 * needs from the plugin arrives as a QObject* plus std::function callbacks.
 * windows.h is only included by the .cpp.
 */

#ifndef AGENTBRIDGE_PIPE_SERVER_H
#define AGENTBRIDGE_PIPE_SERVER_H

#include <functional>

#include <QtCore/QByteArray>
#include <QtCore/QEvent>
#include <QtCore/QObject>
#include <QtCore/QSemaphore>
#include <QtCore/QSharedPointer>
#include <QtCore/QString>
#include <QtCore/QThread>

class BridgeCommands;

// Event carrying a callable to be executed on the dispatcher's thread.
class DispatchEvent : public QEvent
{
public:
	DispatchEvent( QEvent::Type _type, const std::function<void()> & _fn ) :
		QEvent( _type ),
		m_fn( _fn )
	{
	}

	void run()
	{
		m_fn();
	}

private:
	std::function<void()> m_fn;
} ;


// QObject living on the main thread (created in the plugin ctor).
// customEvent() executes posted DispatchEvents there; every LMMS and
// Qt-GUI access happens on that thread.
class MainThreadDispatcher : public QObject
{
public:
	MainThreadDispatcher();
	virtual ~MainThreadDispatcher();

	static QEvent::Type eventType();
	void post( const std::function<void()> & _fn );

protected:
	virtual void customEvent( QEvent * _e );

private:
	static QEvent::Type s_type;
} ;


// One pending request exchanged between the pipe thread and the main
// thread. Held by QSharedPointer so a late main-thread result cannot
// touch freed memory after the pipe thread has given up.
struct Request
{
	QByteArray line;
	QByteArray response;
	QSemaphore done;
} ;


class PipeServer : public QThread
{
public:
	PipeServer( const QString & _pipeName,
				MainThreadDispatcher * _dispatcher,
				BridgeCommands * _commands,
				const std::function<void( const QString & )> & _logFn,
				const std::function<void( const QString & )> & _statusFn,
				QObject * _parent = NULL );
	virtual ~PipeServer();

	void requestStop();
	QString pipeName() const
	{
		return m_pipeName;
	}

protected:
	virtual void run();

private:
	void log( const QString & _msg );
	void status( const QString & _msg );
	bool readChunk( QByteArray & _chunk, bool & _stop );
	bool writeLine( const QByteArray & _line, bool & _stop );
	bool dispatchLine( const QByteArray & _line, QByteArray & _response );

	QString m_pipeName;
	MainThreadDispatcher * m_dispatcher;
	BridgeCommands * m_commands;
	std::function<void( const QString & )> m_logFn;
	std::function<void( const QString & )> m_statusFn;
	volatile bool m_stopRequested;
	void * m_pipe;			// HANDLE, kept opaque so windows.h stays in the .cpp
	void * m_stopEvent;		// HANDLE
	void * m_ioEvent;		// HANDLE
} ;

#endif
