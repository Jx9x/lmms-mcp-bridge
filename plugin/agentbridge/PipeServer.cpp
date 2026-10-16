/*
 * PipeServer.cpp - overlapped byte-mode named-pipe server on a dedicated
 *                  QThread. Each request line is marshalled to the LMMS main
 *                  thread via a DispatchEvent + QSemaphore handshake.
 *
 * No LMMS headers in this TU. windows.h is confined here.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601
#include <windows.h>

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

#include "PipeServer.h"
#include "BridgeCommands.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>


namespace
{
	const int kBufferSize = 8192;
	const int kMaxLine = 64 * 1024;
	const int kDispatchTimeoutMs = 30000;

	QString winError( DWORD _err, const QString & _what )
	{
		return QString( "%1 failed: GetLastError=%2" ).arg( _what ).arg( _err );
	}
}


QEvent::Type MainThreadDispatcher::s_type = QEvent::None;


MainThreadDispatcher::MainThreadDispatcher()
{
	// register the event type eagerly: this ctor runs on the main
	// thread before the pipe thread starts, so the lazy init in
	// eventType() never races between the two threads
	eventType();
}


MainThreadDispatcher::~MainThreadDispatcher()
{
}


QEvent::Type MainThreadDispatcher::eventType()
{
	if( s_type == QEvent::None )
	{
		s_type = (QEvent::Type) QEvent::registerEventType();
	}
	return s_type;
}


void MainThreadDispatcher::post( const std::function<void()> & _fn )
{
	QCoreApplication::postEvent( this,
						new DispatchEvent( eventType(), _fn ) );
}


void MainThreadDispatcher::customEvent( QEvent * _e )
{
	if( _e->type() == eventType() )
	{
		static_cast<DispatchEvent *>( _e )->run();
	}
}




PipeServer::PipeServer( const QString & _pipeName,
						MainThreadDispatcher * _dispatcher,
						BridgeCommands * _commands,
						const std::function<void( const QString & )> & _logFn,
						const std::function<void( const QString & )> & _statusFn,
						QObject * _parent ) :
	QThread( _parent ),
	m_pipeName( _pipeName ),
	m_dispatcher( _dispatcher ),
	m_commands( _commands ),
	m_logFn( _logFn ),
	m_statusFn( _statusFn ),
	m_stopRequested( false ),
	m_pipe( NULL ),
	m_stopEvent( NULL ),
	m_ioEvent( NULL )
{
	m_stopEvent = (void *) CreateEventW( NULL, TRUE, FALSE, NULL );
	m_ioEvent = (void *) CreateEventW( NULL, TRUE, FALSE, NULL );
}


PipeServer::~PipeServer()
{
	if( isRunning() )
	{
		requestStop();
		wait( 5000 );
	}
	if( m_stopEvent )
	{
		CloseHandle( (HANDLE) m_stopEvent );
		m_stopEvent = NULL;
	}
	if( m_ioEvent )
	{
		CloseHandle( (HANDLE) m_ioEvent );
		m_ioEvent = NULL;
	}
}


void PipeServer::requestStop()
{
	m_stopRequested = true;
	if( m_stopEvent )
	{
		SetEvent( (HANDLE) m_stopEvent );
	}
}


void PipeServer::log( const QString & _msg )
{
	if( m_logFn )
	{
		m_logFn( _msg );
	}
}


void PipeServer::status( const QString & _msg )
{
	if( m_statusFn )
	{
		m_statusFn( _msg );
	}
}


// Waits on {stopEvent, ioEvent} after an overlapped call returned
// ERROR_IO_PENDING. Returns WAIT_OBJECT_0 (stop), WAIT_OBJECT_0+1 (I/O
// done) or a WAIT_FAILED-style value.
static DWORD waitIo( HANDLE _stopEvent, HANDLE _ioEvent )
{
	HANDLE hs[2] = { _stopEvent, _ioEvent };
	return WaitForMultipleObjects( 2, hs, FALSE, INFINITE );
}


// Overlapped read of one chunk. Returns false when the pipe broke or an
// error occurred; _stop is set when requestStop() was signalled (the
// pending read is cancelled and drained before returning).
bool PipeServer::readChunk( QByteArray & _chunk, bool & _stop )
{
	_stop = false;
	char buf[kBufferSize];
	OVERLAPPED ov;
	memset( &ov, 0, sizeof( ov ) );
	ov.hEvent = (HANDLE) m_ioEvent;
	ResetEvent( (HANDLE) m_ioEvent );

	DWORD nread = 0;
	BOOL ok = ReadFile( (HANDLE) m_pipe, buf, sizeof( buf ), &nread, &ov );
	DWORD err = ok ? ERROR_SUCCESS : GetLastError();

	if( !ok && err == ERROR_IO_PENDING )
	{
		DWORD w = waitIo( (HANDLE) m_stopEvent, (HANDLE) m_ioEvent );
		if( w == WAIT_OBJECT_0 )
		{
			CancelIoEx( (HANDLE) m_pipe, &ov );
			DWORD t = 0;
			GetOverlappedResult( (HANDLE) m_pipe, &ov, &t, TRUE );
			_stop = true;
			return false;
		}
		if( w != WAIT_OBJECT_0 + 1 )
		{
			log( winError( w, "WaitForMultipleObjects(read)" ) );
			return false;
		}
		ok = GetOverlappedResult( (HANDLE) m_pipe, &ov, &nread, FALSE );
		err = ok ? ERROR_SUCCESS : GetLastError();
	}

	if( !ok )
	{
		if( err != ERROR_BROKEN_PIPE && err != ERROR_NO_DATA )
		{
			log( winError( err, "ReadFile" ) );
		}
		return false;
	}

	_chunk.append( buf, (int) nread );
	return true;
}


// Overlapped write of one response line.
bool PipeServer::writeLine( const QByteArray & _line, bool & _stop )
{
	_stop = false;
	OVERLAPPED ov;
	memset( &ov, 0, sizeof( ov ) );
	ov.hEvent = (HANDLE) m_ioEvent;
	ResetEvent( (HANDLE) m_ioEvent );

	DWORD nwritten = 0;
	BOOL ok = WriteFile( (HANDLE) m_pipe, _line.constData(),
						_line.size(), &nwritten, &ov );
	DWORD err = ok ? ERROR_SUCCESS : GetLastError();

	if( !ok && err == ERROR_IO_PENDING )
	{
		DWORD w = waitIo( (HANDLE) m_stopEvent, (HANDLE) m_ioEvent );
		if( w == WAIT_OBJECT_0 )
		{
			CancelIoEx( (HANDLE) m_pipe, &ov );
			DWORD t = 0;
			GetOverlappedResult( (HANDLE) m_pipe, &ov, &t, TRUE );
			_stop = true;
			return false;
		}
		if( w != WAIT_OBJECT_0 + 1 )
		{
			log( winError( w, "WaitForMultipleObjects(write)" ) );
			return false;
		}
		ok = GetOverlappedResult( (HANDLE) m_pipe, &ov, &nwritten, FALSE );
		err = ok ? ERROR_SUCCESS : GetLastError();
	}

	if( !ok )
	{
		if( err != ERROR_BROKEN_PIPE && err != ERROR_NO_DATA )
		{
			log( winError( err, "WriteFile" ) );
		}
		return false;
	}
	return true;
}


// Post the request to the main thread and wait up to 30 s for the
// response. On timeout the pending Request is kept alive by the posted
// lambda's QSharedPointer capture and its late result is discarded.
bool PipeServer::dispatchLine( const QByteArray & _line,
								QByteArray & _response )
{
	QSharedPointer<Request> req( new Request );
	req->line = _line;
	BridgeCommands * commands = m_commands;
	m_dispatcher->post( [req, commands]()
	{
		req->response = commands->handle( req->line );
		req->done.release();
	} );

	const int tries = kDispatchTimeoutMs / 50;
	for( int i = 0; i < tries && !m_stopRequested; ++i )
	{
		if( req->done.tryAcquire( 1, 50 ) )
		{
			_response = req->response;
			return true;
		}
	}

	QJsonObject err;
	err["ok"] = false;
	err["cmd"] = QString::fromUtf8( _line.left( 200 ) );
	err["error"] = QString( "timeout waiting for LMMS main thread; command outcome may be indeterminate, so inspect the project before retrying an edit" );
	_response = QJsonDocument( err ).toJson( QJsonDocument::Compact );
	return false;
}


void PipeServer::run()
{
	m_pipe = (void *) CreateNamedPipeW(
		(LPCWSTR) m_pipeName.utf16(),
		PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
			| PIPE_REJECT_REMOTE_CLIENTS,
		1, 65536, 65536, 0, NULL );

	if( m_pipe == NULL || m_pipe == (void *) INVALID_HANDLE_VALUE )
	{
		DWORD err = GetLastError();
		m_pipe = NULL;
		log( winError( err, "CreateNamedPipeW(" + m_pipeName + ")" ) );
		status( "error: CreateNamedPipeW failed" );
		return;
	}

	log( "pipe server listening on " + m_pipeName );
	status( "listening" );

	while( !m_stopRequested )
	{
		OVERLAPPED cov;
		memset( &cov, 0, sizeof( cov ) );
		cov.hEvent = (HANDLE) m_ioEvent;
		ResetEvent( (HANDLE) m_ioEvent );

		BOOL conn = ConnectNamedPipe( (HANDLE) m_pipe, &cov );
		if( !conn )
		{
			DWORD err = GetLastError();
			if( err == ERROR_IO_PENDING )
			{
				DWORD w = waitIo( (HANDLE) m_stopEvent,
									(HANDLE) m_ioEvent );
				if( w == WAIT_OBJECT_0 )
				{
					CancelIoEx( (HANDLE) m_pipe, &cov );
					DWORD t = 0;
					GetOverlappedResult( (HANDLE) m_pipe, &cov, &t,
																TRUE );
					break;
				}
				if( w != WAIT_OBJECT_0 + 1 )
				{
					log( winError( w,
							"WaitForMultipleObjects(connect)" ) );
					break;
				}
				DWORD t = 0;
				GetOverlappedResult( (HANDLE) m_pipe, &cov, &t,
																FALSE );
			}
			else if( err == ERROR_PIPE_CONNECTED )
			{
				// client was already waiting; proceed
			}
			else
			{
				log( winError( err, "ConnectNamedPipe" ) );
				break;
			}
		}
		if( m_stopRequested )
		{
			break;
		}

		log( "client connected" );
		status( "client connected" );

		QByteArray pending;
		bool stop = false;
		bool dead = false;

		while( !dead && !stop )
		{
			QByteArray chunk;
			if( !readChunk( chunk, stop ) )
			{
				break;
			}
			pending.append( chunk );

			for(;;)
			{
				int nl = pending.indexOf( '\n' );
				if( nl < 0 )
				{
					break;
				}
				QByteArray line = pending.left( nl );
				pending.remove( 0, nl + 1 );
				if( line.endsWith( '\r' ) )
				{
					line.chop( 1 );
				}
				if( line.isEmpty() )
				{
					continue;
				}

				QByteArray resp;
				dispatchLine( line, resp );
				resp.append( '\n' );
				if( !writeLine( resp, stop ) )
				{
					dead = true;
					break;
				}
			}

			if( !dead && pending.size() > kMaxLine )
			{
				QJsonObject err;
				err["ok"] = false;
				err["cmd"] = QString();
				err["error"] = QString( "line too long" );
				QByteArray resp = QJsonDocument( err ).toJson(
											QJsonDocument::Compact );
				resp.append( '\n' );
				writeLine( resp, stop );
				log( "client sent overlong line, disconnecting" );
				break;
			}
		}

		DisconnectNamedPipe( (HANDLE) m_pipe );
		log( "client disconnected" );
		status( "listening" );

		if( stop )
		{
			break;
		}
	}

	CloseHandle( (HANDLE) m_pipe );
	m_pipe = NULL;
	log( "pipe server stopped" );
	status( "stopped" );
}
