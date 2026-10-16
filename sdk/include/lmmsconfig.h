/*
 * lmmsconfig.h reconstructed for the installed LMMS 1.2.2 win64 build
 * (C:\Program Files\LMMS). Generated from src/lmmsconfig.h.in of tag v1.2.2.
 *
 * Evidence for the feature macros:
 *  - LMMS_BUILD_WIN32/WIN64, LMMS_HOST_X86_64: lmms.exe reports "win32 x86_64";
 *    cmake/modules/DetectMachine.cmake sets these for WIN64 builds.
 *  - LMMS_HAVE_STK / LMMS_HAVE_FLUIDSYNTH (change ConfigManager's layout):
 *    ~/.lmmsrc.xml contains "stkdir" and "defaultsf2", which ConfigManager.cpp
 *    only writes under these macros; malletsstk.dll and sf2player.dll exist.
 *  - LMMS_HAVE_SDL / PORTAUDIO / MP3LAME / OGGVORBIS: SDL.dll,
 *    libportaudio-2.dll, libmp3lame-0.dll, libvorbis*.dll are shipped.
 *  - LMMS_HAVE_SOUNDIO, ALSA, JACK, PULSEAUDIO, SNDIO, OSS: forced off for
 *    WIN32 in CMakeLists.txt (or no headers on MinGW).
 *  - LMMS_HAVE_VST: never set by the 1.2.2 CMake scripts (they use
 *    LMMS_SUPPORT_VST instead).
 *  - LMMS_HAVE_*_H: headers present in a mingw-w64 + winpthreads sysroot.
 */

/* #undef LMMS_BUILD_LINUX */
#define LMMS_BUILD_WIN32
#define LMMS_BUILD_WIN64
/* #undef LMMS_BUILD_APPLE */
/* #undef LMMS_BUILD_OPENBSD */
/* #undef LMMS_BUILD_FREEBSD */
/* #undef LMMS_BUILD_HAIKU */

/* #undef LMMS_HOST_X86 */
#define LMMS_HOST_X86_64

/* #undef LMMS_HAVE_ALSA */
#define LMMS_HAVE_FLUIDSYNTH
/* #undef LMMS_HAVE_JACK */
/* #undef LMMS_HAVE_WEAKJACK */
#define LMMS_HAVE_MP3LAME
#define LMMS_HAVE_OGGVORBIS
/* #undef LMMS_HAVE_OSS */
/* #undef LMMS_HAVE_SNDIO */
#define LMMS_HAVE_PORTAUDIO
/* #undef LMMS_HAVE_SOUNDIO */
/* #undef LMMS_HAVE_PULSEAUDIO */
#define LMMS_HAVE_SDL
#define LMMS_HAVE_STK
/* #undef LMMS_HAVE_VST */

#define LMMS_HAVE_STDINT_H
#define LMMS_HAVE_STDLIB_H
#define LMMS_HAVE_PTHREAD_H
#define LMMS_HAVE_UNISTD_H
#define LMMS_HAVE_SYS_TYPES_H
/* #undef LMMS_HAVE_SYS_IPC_H */
#define LMMS_HAVE_SEMAPHORE_H
/* #undef LMMS_HAVE_SYS_SHM_H */
#define LMMS_HAVE_SYS_TIME_H
#define LMMS_HAVE_SCHED_H
/* #undef LMMS_HAVE_SYS_SOUNDCARD_H */
/* #undef LMMS_HAVE_SOUNDCARD_H */
#define LMMS_HAVE_FCNTL_H
/* #undef LMMS_HAVE_SYS_IOCTL_H */
#define LMMS_HAVE_CTYPE_H
#define LMMS_HAVE_STRING_H
#define LMMS_HAVE_PROCESS_H
#define LMMS_HAVE_LOCALE_H

/* defines for libsamplerate */


/* Target processor clips on negative float to int conversion. */
/* #undef CPU_CLIPS_NEGATIVE */
#ifndef CPU_CLIPS_NEGATIVE
#define CPU_CLIPS_NEGATIVE 0
#endif

/* Target processor clips on positive float to int conversion. */
/* #undef CPU_CLIPS_POSITIVE */
#ifndef CPU_CLIPS_POSITIVE
#define CPU_CLIPS_POSITIVE 0
#endif

/* Define if you have C99's lrint function. */
/* #undef HAVE_LRINT */

/* Define if you have C99's lrintf function. */
/* #undef HAVE_LRINTF */
