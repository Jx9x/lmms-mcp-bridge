/*
 * qconfig.h reconstructed for the Qt 5.4.2 DLLs shipped with LMMS 1.2.2
 * (C:\Program Files\LMMS\Qt5*.dll).
 *
 * Qt5Core.dll reports: "Qt 5.4.2 (x86_64-little_endian-llp64 shared (dynamic)
 * release build; by GCC 4.9.3 20150410 (prerelease))", qt_prfxpath=/opt/mingw64,
 * i.e. the Unix configure script cross-compiling with -xplatform win32-g++.
 * This mirrors what qtbase-5.4.2/configure writes for that case (see the
 * "part of configuration information goes into qconfig.h" section):
 *  - "full" feature set: every feature-conditional virtual of QWidget/QWindow
 *    (wheelEvent, tabletEvent, contextMenuEvent, actionEvent, drag&drop),
 *    QMainWindow::createPopupMenu, QAbstractScrollArea, QObjectUserData,
 *    session management and accessibility are exported by the shipped DLLs.
 *  - QT_LARGEFILE_SUPPORT is not written for MinGW targets.
 *  - QT_POINTER_SIZE comes from the ptrsize test (8 on x86_64).
 *  - Unix-only QCONFIG_FLAGS (QT_NO_DBUS, QT_NO_ICONV, QT_NO_GLIB, ...) are
 *    omitted: none is referenced by a public QtCore/QtGui/QtWidgets/QtXml
 *    header except QT_NO_SESSIONMANAGER (non-virtual API only, and session
 *    management is present in Qt5Gui.dll), and none is a dependency in
 *    qfeatures.txt, so they cannot change class layouts or vtables.
 */

/* Everything */
/* Compile time features */
#define QT_POINTER_SIZE 8

// Compiler sub-arch support
#define QT_COMPILER_SUPPORTS_SSE2 1
#define QT_COMPILER_SUPPORTS_SSE3 1
#define QT_COMPILER_SUPPORTS_SSSE3 1
#define QT_COMPILER_SUPPORTS_SSE4_1 1
#define QT_COMPILER_SUPPORTS_SSE4_2 1
#define QT_COMPILER_SUPPORTS_AVX 1
#define QT_COMPILER_SUPPORTS_AVX2 1

#define QT_QPA_DEFAULT_PLATFORM_NAME "windows"
