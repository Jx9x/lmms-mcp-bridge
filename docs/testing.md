# Verification and protected live testing

Use this document when testing the bridge. These session replacement and
restoration steps apply to test runs; a composition session should keep
using the running bridge. Read the
[operation skill](../.agents/skills/lmms-operation-instructions/SKILL.md)
before live edits or playback, and [development.md](development.md) for
dependency setup and ABI constraints.

## Build and offline checks

From the repository root in Git Bash, using the environment created in
[development.md](development.md):

```bash
./scripts/build-plugin.sh
PATH="$PATH:/c/Program Files/LMMS" ./scripts/build-plugin.sh test
.venv/Scripts/python.exe -m unittest discover -s scripts -p 'test_*.py' -v
```

Require a warning-free build. The native test executable belongs in
`build/unstripped/`, never the plugin scan directory. It needs the installed
Qt DLL directory on PATH for that command only. Confirm that the plugin
exports only its two LMMS entry symbols and that `build/lmms-plugins/`
contains only `agentbridge.dll`. Do not run `clean` without confirmation.

## Prefer the protected live runner

From PowerShell:

```powershell
$testPython = (Resolve-Path '.\.venv\Scripts\python.exe').Path
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-live-tests.ps1 -Mode features -Python $testPython
```

The runner refuses multiple LMMS processes and accepts only the expected
clean normal session. By default that is `Untitled - LMMS 1.2.2`.
For a clean named session, supply `-RestoreProject <absolute-saved-path>`;
the runner requires its exact expected title, hash-checks the saved file,
and reopens it in normal LMMS after restoring backups. Do not automatically
replace a different named or recovery session.

`-PluginDirectory <absolute-directory>` selects an isolated build without
replacing a DLL loaded in the user's usual session. The runner:

1. Backs up `~/.lmmsrc.xml` and `Documents/lmms/recover.mmp.bak`, and records
   the installed LMMS file listing.
2. Closes the eligible clean normal session with `CloseMainWindow()` and
   refuses startup if `Documents/lmms/recover.mmp` remains.
3. Launches a visible bridge-enabled instance through
   `start-lmms-with-bridge.ps1`, using a disposable fixture copy at 8% master
   volume for automated playback.
4. Runs the protocol, composition, render, close-prompt, and MCP suites.
5. Saves dirty test edits to a protected output before closing normally.
   If saving/shutdown fails, leaves LMMS and backups intact rather than
   killing it or discarding edits.
6. Restores and SHA256-verifies configuration/recovery backups, compares the
   installed listing, checks any saved user-project hash, and reopens the
   original normal session without bridge environment variables. A separate
   bridge `ping` must then fail.

The installation under `C:\Program Files\LMMS` must remain unchanged.
LMMS may rewrite equivalent configuration attribute order after startup.
Autosave can refresh `recover.mmp.bak` even when stopped and clean, so exact
persistent recovery-backup restoration requires a safe session close;
retain the protected originals and restoration report.

## Manual smoke test

Use this only when the protected suite does not cover the investigation.

1. Back up configuration and any recovery backup; record the installed
   listing. Check that there is at most one LMMS instance and no unsaved
   user work. Close only an eligible clean session through
   `CloseMainWindow()`. A clean exit deletes `recover.mmp`; do not delete
   recovery data to make startup proceed.
2. Launch with `start-lmms-with-bridge.ps1 -Project <absolute-demo-copy>`
   and a visible window. Poll `bridge.ps1 ping` for readiness, then inspect
   the song and set master volume to 40 unless the user requested another
   level. Reapply after every new/load and confirm before playback.
3. Run native `selftest`, `song`, and `tracks`; exercise tempo, play/stop,
   `show`, and rejected commands such as `bogus`, `tempo abc`, and `tempo 5`.
   Confirm edits mark the song modified and new/load refuse dirty work.
4. Compare tracks with fixture ground truth using `scripts/compare.py`:
   names, types, order, clip positions/lengths/kinds, and all note fields.
   The reported instrument is its display name while the file stores the
   internal key. Legacy note keys also differ from MCP MIDI keys.
5. Check the native save prompt and Cancel preserving edits. Save test
   work to a fresh destination, confirm `modified:false` and no title `*`,
   then close through `CloseMainWindow()`. On failure leave LMMS/backups
   intact. Restore and hash-verify configuration/recovery backups, compare
   the installed listing, reopen normal LMMS without bridge environment
   variables, and confirm bridge `ping` fails.

## Suites and reports

`test_bridge_live.py` drives `test_protocol_live.py`,
`test_composition_live.py`, `test_render_live.py`, `test_mcp_live.py`,
`test_review_fixes_live.py`, and `test_mcp_errors_live.py`. GUI messages are
used for the close-prompt regression; composition and MCP control use the
native pipe/models.

Look in each `runtime/test/commands-*/` report directory for
`responses.jsonl`, compact/native catalogs, `close-prompt.json`, saved
projects, audio/MIDI exports, render/MCP/review reports, installed-file
listings, and `restoration.txt`. Judge each run from its actual reports;
old passing counts do not establish a new build's correctness.

Reports and any local evidence indexes are private artifacts excluded from Git.
Preserve retained reports and `runtime/backup/` during cleanup.
