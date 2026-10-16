/*
 * BridgeCommands.h - text-line command handlers and the ABI self-test.
 *                    Everything here runs on the LMMS main thread only.
 *
 * The header is QtCore-only so PipeServer (which contains no LMMS
 * headers) can hold a pointer to it.
 */

#ifndef AGENTBRIDGE_COMMANDS_H
#define AGENTBRIDGE_COMMANDS_H

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

class AgentBridge;
class CompositionCommands;

class BridgeCommands
{
public:
	BridgeCommands( AgentBridge * _plugin );
	~BridgeCommands();

	// handle one request line "<name> [arg]"; returns one compact JSON line
	QByteArray handle( const QByteArray & _line );

	// run the ABI self-test; returns {"passed","failed","checks":[...]}
	QJsonObject selfTest();

private:
	AgentBridge * m_plugin;
	CompositionCommands * m_composition;
	bool m_handling;
} ;

#endif
