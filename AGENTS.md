# Agent Bridge for LMMS 1.2.2

This project controls stock Windows x64 LMMS 1.2.2 through a native
`agentbridge` Tool plugin and an MCP stdio server. Commands edit the open
LMMS models, including unsaved work. The server does not start or close LMMS.

## Read for your task

Read only the documents relevant to the work. Any agent operating the live
session, including during development, must read the operation skill first.

| Task | Instructions |
| --- | --- |
| Inspect, compose, arrange, mix, audition, or export music | [LMMS Operation Instructions](.agents/skills/lmms-operation-instructions/SKILL.md) |
| Set up dependencies, build the plugin, or maintain the repository | [Development and build](docs/development.md) |
| Configure MCP or change commands, native models, schemas, or transport | [Bridge reference](docs/bridge-reference.md) |
| Run offline checks or protected live tests | [Testing](docs/testing.md) |

The operation skill links to editing and export details as needed. Development
and testing documents contain their own specialized constraints; read them
before the corresponding work.

## Shared safety

- Never write under `C:\Program Files\LMMS`.
- Keep one LMMS instance. Use `scripts/start-lmms-with-bridge.ps1` when a
  bridge-enabled launch is needed; it refuses a second instance.
- Protect unsaved songs. Never kill LMMS or discard edits to make a task
  proceed. Close only through `CloseMainWindow()` after saving and confirming
  that the window title has no `*`.
- Default to 40 percent master volume unless the user
  specifies another level; the operation skill gives
  the checks required after connecting/new/load and before playback.
- Use the native bridge for supported live edits. After a timeout or pipe
  disconnect, inspect the session before retrying an edit.
- Preserve ignored backups and retained evidence, especially `runtime/backup/`.
  Preview explicit cleanup targets and obtain confirmation before deletion;
  see the development document for the `git clean` parent-directory hazard.

## Brief project map

```text
.agents/skills/       LMMS operation skill and its editing/export references
docs/                development, protocol, and testing guides
plugin/agentbridge/  native bridge, model/command integration, renderer, schema tests
scripts/             dependency setup, build/launch/client/MCP tools, regression suites
tests/fixtures/      demo project, expected ground truth, upstream licensing notice
sdk/                 ABI headers, definitions, support tools; ignored import libraries
third_party/, tools/ ignored upstream sources and pinned MinGW toolchain
build/               ignored outputs: lmms-plugins/ contains only agentbridge.dll
runtime/             ignored logs, test artifacts, settings/recovery backups
```
