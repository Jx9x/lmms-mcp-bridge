# Third-party notices

Original Agent Bridge code, MCP server, and agent skill:
Copyright (C) 2026 Jx9x. Licensed under the GNU General Public License,
version 2 or (at your option) any later version; see [LICENSE](LICENSE).

Third-party material retains its upstream copyrights and licenses. This file
identifies material included in this repository and the direct dependencies
used to build or run it. Native dependency versions, source downloads, and
checksums are recorded in [scripts/dependencies.json](scripts/dependencies.json).

## LMMS 1.2.2

- Upstream: [LMMS source at commit 94363be152f526edba4e884264d891f1361cf54b](https://github.com/LMMS/lmms/tree/94363be152f526edba4e884264d891f1361cf54b).
- License for the LMMS code used by this project: **GPL-2.0-or-later**;
  see the upstream [LICENSE.txt](https://github.com/LMMS/lmms/blob/94363be152f526edba4e884264d891f1361cf54b/LICENSE.txt)
  and individual source-file notices.
- Project attribution: Copyright (C) 2008-2020 LMMS Developers.
- `src/core/audio/AudioDevice.cpp` attribution:
  Copyright (c) 2004-2014 Tobias Doerffel
  `<tobydox/at/users.sourceforge.net>`.

The plugin compiles the upstream `AudioDevice.cpp` unchanged into
`agentbridge.dll` and uses LMMS headers and the installed LMMS executable's
exported interfaces. The full LMMS source is downloaded into the ignored
`third_party/lmms-1.2.2/` directory during dependency setup.

The tracked `sdk/include/lmmsconfig.h` and `sdk/include/lmmsversion.h` are
reconstructed configuration headers based on LMMS's `src/lmmsconfig.h.in`
and `src/lmmsversion.h.in`, configured for the supported Windows build.
The test project's separate license is described below.

## Qt Base 5.4.2

- Upstream: [Qt Base v5.4.2](https://github.com/qt/qtbase/tree/v5.4.2).
- The Qt headers and configuration code used here offer **LGPL-2.1** or
  **LGPL-3.0** licensing. The LGPL 2.1 option includes the additional
  permissions in the **Digia Qt LGPL Exception version 1.1**. See
  [LICENSE.LGPLv21](https://github.com/qt/qtbase/blob/v5.4.2/LICENSE.LGPLv21),
  [LICENSE.LGPLv3](https://github.com/qt/qtbase/blob/v5.4.2/LICENSE.LGPLv3),
  and [LGPL_EXCEPTION.txt](https://github.com/qt/qtbase/blob/v5.4.2/LGPL_EXCEPTION.txt).
  Upstream also offers commercial licensing under a separate agreement.
- Relevant upstream notices identify Copyright (C) 2014 Digia Plc and/or
  its subsidiary(-ies), Copyright (C) 2013 Intel Corporation in `configure`,
  and Copyright (C) 2014 Intel Corporation in `qglobal.h`.

The build uses QtCore, QtGui, QtWidgets, and QtXml headers and links to the
corresponding DLLs supplied by the installed LMMS. The tracked
`sdk/include/QtCore/qconfig.h` reconstructs the installed Qt configuration;
`sdk/include/QtCore/qfeatures.h` is generated from Qt's
`src/corelib/global/qfeatures.txt`. The local generator
`sdk/tools/gen_qfeatures.pl` follows the dependency-generation algorithm in
Qt's `qtbase.pro`.

Qt Base sources are downloaded into the ignored
`third_party/qtbase-opensource-src-5.4.2/` directory during dependency setup.

## libsamplerate 0.1.8 (Secret Rabbit Code)

- Source: [libsamplerate 0.1.8 archive pinned by this project](https://distfiles.macports.org/libsamplerate/libsamplerate-0.1.8.tar.gz).
- License: **GPL-2.0-or-later**, as stated in this version's
  `src/samplerate.h`; the archive includes the GPL v2 text in `COPYING`.
  A copy of the GPL v2 text is also provided in [LICENSE](LICENSE).
- Header attribution: Copyright (C) 2002-2011 Erik de Castro Lopo
  `<erikd@mega-nerd.com>`.

The build uses `src/samplerate.h` from the ignored
`third_party/libsamplerate-0.1.8/` directory and links to the
`libsamplerate-0.dll` supplied by the installed LMMS.

## SDK export definitions

The tracked export lists are generated from the binaries supplied by LMMS
1.2.2. They describe the interfaces used to generate local import libraries;
the generated `sdk/lib/*.a` files are ignored.

| Tracked export list | Upstream component | Upstream license described above |
| --- | --- | --- |
| `sdk/def/lmms.def` | LMMS 1.2.2 (`lmms.exe`) | GPL-2.0-or-later |
| `sdk/def/Qt5Core.def`, `Qt5Gui.def`, `Qt5Widgets.def`, `Qt5Xml.def` | Qt Base 5.4.2 | LGPL-2.1 with the Digia Qt LGPL Exception 1.1, or LGPL-3.0 |
| `sdk/def/libsamplerate-0.def` | libsamplerate 0.1.8 | GPL-2.0-or-later |

## LMMS test fixture

`tests/fixtures/crunk-demo.mmp` is an unmodified copy of
`data/projects/shorties/Crunk(Demo).mmp` from LMMS 1.2.2. It is licensed under
the **Artistic License 2.0**, as recorded in the upstream notice preserved
in [tests/fixtures/LICENSES.TXT](tests/fixtures/LICENSES.TXT).
The notice links to the [original LMMS Sharing Platform entry](https://lmms.io/lsp/?action=show&file=4093).
The license text is available from [The Perl Foundation](https://www.perlfoundation.org/artistic-license-20.html).

`tests/fixtures/crunk-groundtruth.json` contains regression reference data
generated from that project by `scripts/parse_groundtruth.py`.

## Build and runtime dependencies

The compiler, downloaded dependency trees, Python packages, and installed
LMMS libraries are external to the tracked source tree.

- **GCC 4.9.4 / MinGW-w64 toolchain:** the build statically links compiler
  runtime libraries. GCC's `libgcc` and `libstdc++` use GPL v3 or later with
  the [GCC Runtime Library Exception 3.1](https://www.gnu.org/licenses/gcc-exception-3.1.html).
  The downloaded toolchain supplies the full texts in
  `tools/mingw64/licenses/gcc/COPYING3` and `COPYING.RUNTIME`.
- **MinGW-w64 runtime:** includes material under several permissive and
  public-domain terms. Its collected notices are supplied in
  `tools/mingw64/licenses/mingw-w64/COPYING.MinGW-w64-runtime.txt`.
- **winpthreads:** Copyright (c) 2011 mingw-w64 project, under the MIT
  license, with portions Copyright (C) 2010 Lockless Inc. under BSD-style
  terms. The full notices are in
  `tools/mingw64/licenses/winpthreads/COPYING`.
- **libsndfile and LAME (GNU LGPL):** audio import/export uses DLLs supplied by the
  installed LMMS (`libsndfile-1.dll` and `libmp3lame-0.dll`). Their source
  and license notices are maintained by the [libsndfile](https://libsndfile.github.io/libsndfile/)
  and [LAME](https://lame.sourceforge.io/about.php) projects, respectively.

The direct Python dependencies are pinned in [requirements.txt](requirements.txt):

| Package | Version | License | Copyright notice |
| --- | --- | --- | --- |
| [MCP Python SDK](https://github.com/modelcontextprotocol/python-sdk) (`mcp`) | 1.27.1 | MIT | Copyright (c) 2024 Anthropic, PBC |
| [jsonschema](https://github.com/python-jsonschema/jsonschema) | 4.26.0 | MIT | Copyright (c) 2013 Julian Berman |

Their distributions include the full license texts in
`mcp-1.27.1.dist-info/licenses/LICENSE` and
`jsonschema-4.26.0.dist-info/licenses/COPYING`. Transitive Python dependencies
retain the license notices included in their own distributions.
