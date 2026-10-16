# Bridge protocol and implementation

Use this reference when configuring the MCP server or changing the native
protocol, model integration, or MCP adapter. For live composition, use
[LMMS Operation Instructions](../.agents/skills/lmms-operation-instructions/SKILL.md).

## Discover commands

Send the native command `help`. Its `tools` response field is the
authoritative catalog of descriptions, JSON Schema arguments, and
read-only/destructive hints; `commands` lists native names and
`legacyCommands` lists text shortcuts. `help.tools` is a response-field
notation, not a valid command. MCP `tools/list` publishes the compact
agent-facing subset. Use discovery rather than a copied command inventory.

## Pipe protocol

The default pipe is `\\.\pipe\lmms-agent-bridge`, overridden with
`LMMS_AGENT_BRIDGE_PIPE`. Requests/replies are single UTF-8 lines. Supported
request forms include:

```text
tempo 128
set_tempo {"bpm":128}
{"cmd":"set_tempo","args":{"bpm":128},"id":"optional-correlation-id"}
```

Replies contain `ok`, `cmd`, `changed`, native `modified`, `windowTitle`,
`session_id`, and `project_token`; supplied IDs are echoed.

- `changed` means this call changed song data; `modified` is the current
  native dirty state. Saving clears `modified` without changing song data.
- Reads, rejected preflight, and no-op setters return `changed:false`
  without changing the dirty state. Unknown-tool rejection also returns
  `changed:false`.
- A native failure after partial mutation returns `changed:true`.
- An MCP transport failure after a native request starts returns
  `changed:null` and `outcome_indeterminate:true`. Inspect the project
  before retrying; do not automatically retry edits.

Requests are capped at 64 KiB and replies at 64 MiB. Check UTF-8 byte sizes
and split large note batches. The log defaults to
`runtime/logs/agentbridge.log`, overridden by `LMMS_AGENT_BRIDGE_LOG`.

For direct inspection from PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\bridge.ps1 ping song tracks selftest
```

The Python client accepts JSON arguments and command files. For example,
with the project's Python environment:

```powershell
& .\.venv\Scripts\python.exe scripts\bridge_client.py help
```

## MCP configuration and response adaptation

The server is stdio-only and never starts, closes, or replaces LMMS.
Configure the client with a Python interpreter containing the dependencies
in `requirements.txt`, and the absolute script path:

```text
C:/path/to/lmms-agent-bridge/scripts/lmms_mcp.py
```

The project has verified `mcp 1.27.1` and `jsonschema 4.26.0`. No global
MCP client configuration is installed by this repository. `--pipe` and
`--timeout` are optional server arguments. Opt into an optional tool with
`--include-tool get_project_xml`; repeat the flag for multiple tools.

The adapter discovers the full native catalog and exposes consolidated
controls while hiding replaced aliases and optional diagnostic/theory/Zyn
shortcuts. `scan_vst_directory` and `add_vst_track` remain available by
default. See `CONSOLIDATED_TOOLS`, `REPLACED_TOOLS`, and `OPTIONAL_TOOLS` in
`scripts/lmms_mcp.py` for the maintained selection.

MCP normalizes every returned note's `key` to MIDI and removes the
`midi_key`, `lmms_key`, `len`, `vol`, and `pan` aliases. Native nested
clip/pattern `key` values and legacy `tracks`/`get_tracks` retain native
LMMS numbering; `get_notes` and native note writes use MIDI. Keep these
representations distinct when changing the adapter or clients.

Native commands retain full-detail defaults for compatibility. MCP asks
for compact results, including `include_pattern:false` for insertion;
`full_detail:true` and explicit `include_*` controls are described in the
operation skill. Published schemas omit default `maxLength:32767`
metadata, but full native schemas and limits are still enforced.

Conditional schemas encode paired meter fields, seek-only positions,
instrument-only options, exclusive ticks/bars and pitch fields, and export
bit-depth enums. Calls are serialized through cancellation. Structured and
text JSON must agree and carry correct MCP error flags. Validation errors
should identify the failing field and constraint without echoing oversized
arguments, schemas, or a tool catalog. Live reports save the compact
catalog as `tools.json` and the native catalog as `native-tools.json`.

## Native model invariants

- Every project mutation must call `Song::setModified()`, including partial
  edits that subsequently fail. Preflight rejection must leave the song
  unchanged. New/load require `discard_unsaved:true` when dirty;
  save/output replacement must keep the documented overwrite guard.
  Native `project.new` creates default tracks; `create_project` is empty
  by default.
- Destroy an instrument's native view before its model. Use
  `BridgeModel::removeTrack` / native `deleteTrackView` through the Qt
  meta-object. Bare `delete track` crashes the LMMS 1.2.2 instrument-view
  destructor in a GUI session.
- Use the native `setSampleFile` Qt slot for sample setters absent from
  the import library. Do not replace them with file rewrites or GUI clicks.
- Seek through public PlayPos/setMilliSeconds APIs and notifications;
  `Song::setPlayPos` is private in 1.2.2.
- Project/preset/instrument-file/export paths and VST discovery directories
  require absolute local drive paths, excluding relative paths, UNC, and
  alternate streams. Direct `place_sample_clip`, `assign_sample_file`, and
  `add_vst_track` loaders require readable absolute paths but do not enforce
  the same local-drive restriction. Agent workflows should use local files.
- Preserve the 64-MiB project/preset input limit and compressed-project
  declared-size check. Sample metadata limits and export limits are in the
  operation skill's editing/export references.
- Render live models through the native mixer. Keep work in bounded
  main-thread timer slices and block edits/transport while rendering.
  Stage output beside its destination and commit only on success; restore
  device, quality, parameters, playhead, arrangement length, window state,
  and native dirty status on cleanup.

Read [development.md](development.md) for ABI constraints and
[testing.md](testing.md) before exercising these invariants in LMMS.
