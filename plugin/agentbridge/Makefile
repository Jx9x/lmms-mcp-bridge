# agentbridge - LMMS Tool plugin. Native MinGW build, mirrors the flags
# LMMS 1.2.2 uses for its plugins (RelWithDebInfo).
#
# Paths are relative to this Makefile's directory; invoke via
#   mingw32-make -C plugin/agentbridge
# or scripts/build-plugin.sh (which puts tools/mingw64/bin on PATH first).

ROOT     := ../..
LMMS     := $(ROOT)/third_party/lmms-1.2.2
QT       := $(ROOT)/third_party/qtbase-opensource-src-5.4.2/include
SRC      := $(ROOT)/third_party/libsamplerate-0.1.8/src
SDK      := $(ROOT)/sdk
BUILD    := $(ROOT)/build
OBJDIR   := $(BUILD)/obj/agentbridge
UNSTRIP  := $(BUILD)/unstripped
PLUGINDIR:= $(BUILD)/lmms-plugins

TARGET   := agentbridge.dll

CPPFLAGS := -DPLUGIN_NAME=agentbridge -DQT_CORE_LIB -DQT_GUI_LIB \
	-DQT_WIDGETS_LIB -DQT_XML_LIB -DQT_NO_DEBUG -DNDEBUG \
	-I$(SDK)/include -I$(LMMS)/include -I$(LMMS)/src/gui -I$(SRC) \
	-I$(QT) -I$(QT)/QtCore -I$(QT)/QtGui -I$(QT)/QtWidgets -I$(QT)/QtXml

CXXFLAGS := -std=c++0x -fno-exceptions -O2 -g -Wall \
	-Werror=unused-function -Wno-sign-compare -Wno-strict-overflow \
	-Wno-array-bounds -Wno-attributes -MMD -MP

LDFLAGS  := -shared -static
LDLIBS   := -L$(SDK)/lib -llmms -lsamplerate -lQt5Widgets -lQt5Gui -lQt5Xml -lQt5Core

OBJS     := $(OBJDIR)/AgentBridge.o $(OBJDIR)/BridgeCommands.o \
	$(OBJDIR)/CompositionCommands.o $(OBJDIR)/CompositionTools.o \
	$(OBJDIR)/BridgeModel.o $(OBJDIR)/AudioCodecs.o $(OBJDIR)/BridgeRenderer.o \
	$(OBJDIR)/NativeAudioDevice.o $(OBJDIR)/PipeServer.o
DEPS     := $(OBJS:.o=.d)

all: $(PLUGINDIR)/$(TARGET)

$(UNSTRIP)/$(TARGET): $(OBJS)
	@mkdir -p $(UNSTRIP)
	g++ $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(PLUGINDIR)/$(TARGET): $(UNSTRIP)/$(TARGET)
	@mkdir -p $(PLUGINDIR)
	strip -o $@ $<

$(OBJDIR)/NativeAudioDevice.o: $(LMMS)/src/core/audio/AudioDevice.cpp
	@mkdir -p $(OBJDIR)
	g++ $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(OBJDIR)
	g++ $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

test: $(UNSTRIP)/command-schema-tests.exe
	$(UNSTRIP)/command-schema-tests.exe

$(UNSTRIP)/command-schema-tests.exe: CommandSchemaTests.cpp CompositionTools.cpp CommandSchema.h MusicTheory.h CompositionCommands.h
	@mkdir -p $(UNSTRIP)
	g++ $(CPPFLAGS) $(CXXFLAGS) -static -o $@ CommandSchemaTests.cpp CompositionTools.cpp -L$(SDK)/lib -lQt5Core

clean:
	rm -rf $(OBJDIR) $(UNSTRIP)/$(TARGET) $(PLUGINDIR)/$(TARGET)

-include $(DEPS)

.PHONY: all clean test
