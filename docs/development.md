# Development and build

Use this document for dependency setup, native builds, ABI compatibility,
and repository maintenance. Read [testing.md](testing.md) before running
tests that touch LMMS, and [bridge-reference.md](bridge-reference.md) when
changing native commands, models, or the MCP adapter.

## Dependencies and Python

Prerequisites are the installed LMMS 1.2.2 x64 / Qt 5.4.2, Git Bash,
Python 3.10 or newer, and Windows `tar.exe` with libarchive `.7z` support
(or `bsdtar` on PATH). Run commands from the repository root.

Create a virtual environment with an installed Python 3.10 or newer:

```powershell
py -3 -m venv .venv
& .\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

Virtual environments are local and ignored. If using a different environment,
substitute its interpreter in these commands:

```powershell
& .\.venv\Scripts\python.exe scripts\setup_dependencies.py
& .\.venv\Scripts\python.exe scripts\setup_dependencies.py --check
```

`scripts/dependencies.json` pins GCC 4.9.4 posix-seh, LMMS 1.2.2 at commit
`94363be152f526edba4e884264d891f1361cf54b`, Qt Base 5.4.2, and libsamplerate
0.1.8, with exact archive URLs and SHA-256 hashes. Setup verifies cached/new
archives and archive member paths before extraction. It checks required
inputs and compiler version in existing dependency directories, then leaves
them unchanged; it does not compare them byte-for-byte with upstream.
Incomplete directories and mismatched cached archives are refused rather
than deleted or replaced. Preserve local patches before moving them aside.

`--cache-dir <directory> --offline` uses an existing archive cache without
network access. Missing SDK import libraries are generated from tracked
`.def` files with the pinned x64 `dlltool`. Existing libraries are preserved
unless `--regenerate-sdk` is explicitly supplied; regeneration stages every
new library before replacing any destination. Setup does not change the
installed LMMS, settings, recovery projects, or running sessions.
Import libraries are generated locally and excluded from Git. Generation uses
relative output names so their symbols do not contain the checkout path.

## Build

From Git Bash:

```bash
./scripts/build-plugin.sh
```

The script puts `tools/mingw64/bin` on PATH and invokes
`mingw32-make -C plugin/agentbridge`. The stripped output is
`build/lmms-plugins/agentbridge.dll`; the debug-symbol copy is
`build/unstripped/agentbridge.dll`. The scan directory must contain only
the plugin DLL because LMMS loads every `*.dll` there. Expect zero warnings.
Use an isolated build with the test runner's `-PluginDirectory` when a
user session has the usual DLL loaded.

Do not run `scripts/build-plugin.sh clean` without confirmation. The
Makefile recipe removes the object directory and both DLL artifacts.

## Compatibility recipe

This SDK targets the installed LMMS 1.2.2 / Qt 5.4.2 ABI. Do not assume
newer LMMS APIs.

- Toolchain: `x86_64-4.9.4-release-posix-seh` MinGW-builds. The installed
  LMMS was built with GCC 4.9.3/4.9.4 posix-seh.
- Headers: LMMS 1.2.2 sources and Qt 5.4.2. `sdk/include` must be first
  on `-I` so its `lmmsconfig.h`, `lmmsversion.h`, `QtCore/qconfig.h`, and
  `QtCore/qfeatures.h` override source-tree headers. Stock qtbase lacks a
  generated `qconfig.h`.
- Import libraries: `sdk/lib/lib{lmms,Qt5Core,Qt5Gui,Qt5Widgets,Qt5Xml}.a`
  are generated with `gendef`/`dlltool` from installed binaries. Plugins
  link directly against `lmms.exe` exports.
- `Mixer.h` needs `third_party/libsamplerate-0.1.8/src/samplerate.h`.
  Rendering also links `sdk/lib/libsamplerate.a`, generated from installed
  `libsamplerate-0.dll` using `sdk/def/libsamplerate-0.def`.
- Stock `AudioDevice.cpp` is compiled unchanged as `NativeAudioDevice.o`
  because LMMS does not export its constructors. Encoding uses installed
  libsndfile/LAME APIs with Qt file I/O for Unicode paths. Do not copy
  encoder DLLs into the plugin scan directory.
- Flags: `-std=c++0x -fno-exceptions -O2 -g`; defines include
  `PLUGIN_NAME=agentbridge`, `QT_*_LIB`, `QT_NO_DEBUG`, and `NDEBUG`.
  Link with `-shared -static`; libstdc++, libgcc, and winpthread are static.
- Export exactly `agentbridge_plugin_descriptor` and `lmms_plugin_main`,
  matching the shape of shipped plugins.
- Discovery uses `LMMS_PLUGIN_DIR` to select `build/lmms-plugins`.
- Qt5 functor connections avoid Q_OBJECT/moc. `MainThreadDispatcher`
  marshals LMMS/GUI calls from the pipe thread through QEvents and
  `Request{line,response,QSemaphore}` held by `QSharedPointer`, with a
  30-second timeout.

## Tracked files, fixtures, and cleanup

Git includes bridge sources, the MCP server, the operation skill, documentation,
fixtures, and SDK overrides/definitions/tools. The root `.gitignore` explicitly
allows public top-level paths; add a new public path there when needed.
Downloaded compilers/sources, SDK import libraries, archive caches, builds,
runtime artifacts/backups, virtual environments, and local configuration are
ignored. A commit does not protect
ignored backups; preserve them separately before deleting ignored files.

Preview explicit cleanup targets. `git clean -X` can collapse a supplied
`runtime/test/...` path into the ignored `runtime/` parent, including backups
and retained evidence. The narrow preview verified for the disposable
checkout is:

```bash
git clean -ndx -- runtime/test/reproducibility-check/
```

Do not use an unscoped clean. Require confirmation before actual cleanup,
and retain `runtime/backup/` and the archived live-verification reports.
Private evidence indexes can be kept under ignored `runtime/maintenance/`.

`tests/fixtures/crunk-demo.mmp` is an unmodified copy of LMMS 1.2.2's
`data/projects/shorties/Crunk(Demo).mmp`. Preserve its upstream Artistic 2.0
notice in `tests/fixtures/LICENSES.TXT`.
