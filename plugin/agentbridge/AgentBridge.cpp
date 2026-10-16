/*
 * AgentBridge.cpp - plugin entry point, descriptor, icon, logger, view.
 */

#include "AgentBridge.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QFontDatabase>
#include <QtGui/QPainter>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include "embed.h"
#include "Engine.h"
#include "lmmsversion.h"
#include "Song.h"
#include "TrackContainer.h"
#include "Track.h"

#include "BridgeCommands.h"
#include "PipeServer.h"


namespace
{
	const char * const kPluginVersion = "0.4.0";
}


extern "C"
{

Plugin::Descriptor PLUGIN_EXPORT agentbridge_plugin_descriptor =
{
	STRINGIFY( PLUGIN_NAME ),
	"Agent Bridge",
	"Native music-production command bridge for agents",
	"Jx9x",
	0x0300,
	Plugin::Tool,
	new PluginPixmapLoader( "logo" ),
	NULL,
	NULL
} ;


// necessary for getting instance out of shared lib
Plugin * PLUGIN_EXPORT lmms_plugin_main( Model * _parent, void * _data )
{
	return new AgentBridge;
}

}




namespace agentbridge
{

QPixmap getIconPixmap( const char * _name, int _w, int _h )
{
	Q_UNUSED( _name )
	const int w = _w > 0 ? _w : 48;
	const int h = _h > 0 ? _h : 48;
	QPixmap pm( w, h );
	pm.fill( Qt::transparent );
	QPainter p( &pm );
	p.setRenderHint( QPainter::Antialiasing );
	p.setPen( Qt::NoPen );
	p.setBrush( QColor( 43, 88, 151 ) );
	p.drawRoundedRect( QRectF( 1, 1, w - 2, h - 2 ),
						w * 0.18, h * 0.18 );
	p.setPen( Qt::white );
	QFont f = p.font();
	f.setBold( true );
	f.setPixelSize( qMax( 8, (int) ( h * 0.42 ) ) );
	p.setFont( f );
	p.drawText( pm.rect(), Qt::AlignCenter, "AI" );
	p.end();
	return pm;
}

}




AgentBridge * AgentBridge::s_instance = NULL;


AgentBridge::AgentBridge() :
	ToolPlugin( &agentbridge_plugin_descriptor, NULL ),
	m_dispatcher( new MainThreadDispatcher ),
	m_commands( new BridgeCommands( this ) ),
	m_pipe( NULL ),
	m_view( NULL ),
	m_selfTestTimer( NULL ),
	m_selfTestTries( 0 )
{
	s_instance = this;

	openLog();
	log( QString( "agentbridge %1 starting" ).arg( kPluginVersion ) );
	log( QString( "qt headers %1, qt runtime %2, lmms headers %3" )
		.arg( QT_VERSION_STR, qVersion(), LMMS_VERSION ) );
	log( QString( "applicationFilePath=%1 pid=%2" )
		.arg( QCoreApplication::applicationFilePath() )
		.arg( QCoreApplication::applicationPid() ) );
	log( "pipe name: " + pipeName() );
	log( "log path: " + logPath() );

	m_pipe = new PipeServer( pipeName(), m_dispatcher, m_commands,
		[this] ( const QString & s ) { log( s ); },
		[this] ( const QString & s )
		{
			QPointer<AgentBridge> self( this );
			m_dispatcher->post( [self, s]()
			{
				if( self )
				{
					self->setStatusText( s );
				}
			} );
		} );
	m_pipe->start();

	Song * song = Engine::getSong();
	if( song )
	{
		m_connPlayback = QObject::connect( song,
			&Song::playbackStateChanged, this,
			[this]()
			{
				Song * s = Engine::getSong();
				log( QString( "event: playbackStateChanged playing=%1 paused=%2" )
					.arg( s && s->isPlaying() ? 1 : 0 )
					.arg( s && s->isPaused() ? 1 : 0 ) );
			} );
		m_connTempo = QObject::connect( song,
			&Song::tempoChanged, this,
			[this] ( bpm_t bpm )
			{
				log( QString( "event: tempoChanged bpm=%1" )
					.arg( (int) bpm ) );
			} );
		m_connProjectLoaded = QObject::connect( song,
			&Song::projectLoaded, this,
			[this]() { log( "event: projectLoaded" ); } );
		m_connTrackAdded = QObject::connect( song,
			&TrackContainer::trackAdded, this,
			[this] ( Track * t )
			{
				log( QString( "event: trackAdded name=%1 type=%2" )
					.arg( t ? t->name() : QString( "?" ) )
					.arg( t ? (int) t->type() : -1 ) );
			} );
	}

	// startup self-test: 3 s after construction, re-check every 1 s
	// while the project is still loading, max 30 tries
	m_selfTestTimer = new QTimer( this );
	m_selfTestTimer->setSingleShot( true );
	QObject::connect( m_selfTestTimer, &QTimer::timeout, this,
		[this]()
		{
			Song * s = Engine::getSong();
			if( s && s->isLoadingProject() && m_selfTestTries < 30 )
			{
				++m_selfTestTries;
				m_selfTestTimer->start( 1000 );
				return;
			}
			runSelfTestCheck();
		} );
	m_selfTestTimer->start( 3000 );
}


AgentBridge::~AgentBridge()
{
	log( "shutting down" );
	if( m_selfTestTimer )
	{
		m_selfTestTimer->stop();
	}
	if( m_pipe )
	{
		m_pipe->requestStop();
		m_pipe->wait( 5000 );
		delete m_pipe;
		m_pipe = NULL;
	}
	delete m_dispatcher;		// drops queued DispatchEvents un-run
	m_dispatcher = NULL;
	delete m_commands;
	m_commands = NULL;
	log( "plugin destroyed" );
	s_instance = NULL;
}


PluginView * AgentBridge::instantiateView( QWidget * _parent )
{
	Q_UNUSED( _parent )
	m_view = new AgentBridgeView( this );
	return m_view;
}


QString AgentBridge::nodeName() const
{
	return agentbridge_plugin_descriptor.name;
}


void AgentBridge::openLog()
{
	QString path = QString::fromLocal8Bit(
						qgetenv( "LMMS_AGENT_BRIDGE_LOG" ) );
	if( path.isEmpty() )
	{
		path = QDir::tempPath() + "/lmms-agent-bridge.log";
	}
	QDir().mkpath( QFileInfo( path ).absolutePath() );
	m_logFile.setFileName( path );
	m_logFile.open( QIODevice::WriteOnly | QIODevice::Append
											| QIODevice::Text );
}


QString AgentBridge::pipeName() const
{
	QString name = QString::fromLocal8Bit(
						qgetenv( "LMMS_AGENT_BRIDGE_PIPE" ) );
	if( name.isEmpty() )
	{
		name = "lmms-agent-bridge";
	}
	return "\\\\.\\pipe\\" + name;
}


QString AgentBridge::logPath() const
{
	return m_logFile.fileName();
}


bool AgentBridge::signalConnectionsOk() const
{
	return m_connPlayback && m_connTempo &&
			m_connProjectLoaded && m_connTrackAdded;
}


QString AgentBridge::signalConnectionsDetail() const
{
	return QString( "playbackStateChanged=%1 tempoChanged=%2 projectLoaded=%3 trackAdded=%4" )
		.arg( m_connPlayback ? 1 : 0 )
		.arg( m_connTempo ? 1 : 0 )
		.arg( m_connProjectLoaded ? 1 : 0 )
		.arg( m_connTrackAdded ? 1 : 0 );
}


void AgentBridge::log( const QString & _msg )
{
	const bool onMain = QCoreApplication::instance() &&
		QThread::currentThread() ==
						QCoreApplication::instance()->thread();
	const QString line = QDateTime::currentDateTime()
		.toString( "yyyy-MM-ddTHH:mm:ss.zzz" ) +
		( onMain ? " [main] " : " [pipe] " ) + _msg;

	QMutexLocker lock( &m_logMutex );
	if( m_logFile.isOpen() )
	{
		m_logFile.write( line.toUtf8() );
		m_logFile.write( "\n" );
		m_logFile.flush();
	}
	lock.unlock();

	if( onMain )
	{
		appendLogLine( line );
	}
	else if( m_dispatcher )
	{
		QPointer<AgentBridge> self( this );
		m_dispatcher->post( [self, line]()
		{
			if( self )
			{
				self->appendLogLine( line );
			}
		} );
	}
}


void AgentBridge::appendLogLine( const QString & _line )
{
	if( m_view )
	{
		m_view->appendLogLine( _line );
	}
}


void AgentBridge::setStatusText( const QString & _state )
{
	if( m_view )
	{
		m_view->setStatus( pipeName(), _state, logPath() );
	}
}


void AgentBridge::showView()
{
	if( m_view )
	{
		m_view->show();
		if( m_view->parentWidget() )
		{
			m_view->parentWidget()->show();
		}
		m_view->setFocus();
	}
}


void AgentBridge::requestSelfTest()
{
	runSelfTestCheck();
}


void AgentBridge::runSelfTestCheck()
{
	if( !m_commands )
	{
		return;
	}
	QJsonObject report = m_commands->selfTest();
	QByteArray json = QJsonDocument( report )
						.toJson( QJsonDocument::Compact );
	log( QString( "self-test: passed=%1 failed=%2" )
		.arg( report["passed"].toInt() )
		.arg( report["failed"].toInt() ) );
	log( "self-test report: " + QString::fromUtf8( json ) );
}




AgentBridgeView::AgentBridgeView( ToolPlugin * _tool ) :
	ToolPluginView( _tool ),
	m_status( NULL ),
	m_log( NULL )
{
	AgentBridge * bridge = static_cast<AgentBridge *>( _tool );

	QVBoxLayout * layout = new QVBoxLayout( this );

	m_status = new QLabel( this );
	m_status->setTextInteractionFlags( Qt::TextSelectableByMouse );
	setStatus( bridge ? bridge->pipeName() : QString( "?" ),
				"starting",
				bridge ? bridge->logPath() : QString( "?" ) );

	m_log = new QPlainTextEdit( this );
	m_log->setReadOnly( true );
	m_log->setMaximumBlockCount( 2000 );
	m_log->setFont( QFontDatabase::systemFont(
						QFontDatabase::FixedFont ) );

	QPushButton * selfTestBtn = new QPushButton( "Run self-test", this );
	QObject::connect( selfTestBtn, &QPushButton::clicked, this,
		[bridge]()
		{
			if( bridge )
			{
				bridge->requestSelfTest();
			}
		} );

	layout->addWidget( m_status );
	layout->addWidget( m_log, 1 );
	layout->addWidget( selfTestBtn );

	hide();
	if( parentWidget() )
	{
		parentWidget()->hide();
		parentWidget()->resize( 720, 480 );
	}
}


AgentBridgeView::~AgentBridgeView()
{
}


void AgentBridgeView::appendLogLine( const QString & _line )
{
	m_log->appendPlainText( _line );
}


void AgentBridgeView::setStatus( const QString & _pipeName,
								const QString & _state,
								const QString & _logPath )
{
	m_status->setText( QString( "pipe: %1   state: %2   log: %3" )
		.arg( _pipeName, _state, _logPath ) );
}
