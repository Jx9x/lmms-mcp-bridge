# Connect to the LMMS bridge

The native plugin and Python MCP server must be installed on the Windows
computer running LMMS 1.2.2 x64. The skill provides operating guidance and can
be installed separately from the bridge project.

Use the connected MCP tools when available. The stdio server does not launch
LMMS. If a launch or connection repair is needed, locate the bridge project
from the configured MCP server script path or the user's installation location.
Do not infer its location from the installed skill directory.

From the bridge project's root, the launcher is:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\start-lmms-with-bridge.ps1
```

It accepts `-LmmsExe <absolute-lmms.exe>`, `-PluginDirectory <absolute-directory>`,
and `-Project <absolute-song.mmp>`. The defaults are the standard LMMS installation
and the project's `build/lmms-plugins/` directory. Verify that no LMMS process is
running before launching; preserve unsaved songs and recovery data.

For MCP configuration, use the absolute Python interpreter path from an
environment containing the project's `requirements.txt` dependencies. Pass the
absolute path to `scripts/lmms_mcp.py` as its script argument. Keep
`scripts/bridge_client.py` beside the server. Optional server arguments are
`--pipe <name>` and `--timeout <seconds>`.

The default local named pipe is `\\.\pipe\lmms-agent-bridge`. A custom name must
match the plugin's `LMMS_AGENT_BRIDGE_PIPE` environment variable. The launcher
and server must run as the intended local user with compatible privileges.

For a direct diagnostic from the bridge project root, use its Python environment:

```powershell
& .\.venv\Scripts\python.exe scripts\bridge_client.py help
```

Direct diagnostics do not replace supported MCP operations. Inspect the current
project before retrying any edit whose result was lost to a timeout or disconnect.
