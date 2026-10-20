#    Compilation instructions:
#    https://flow5.tech/docs/flow5_doc/Source/Compilation.html
#
#    flow5-mcp: Model Context Protocol server for flow5
#    Build after XFoil-lib, flow5-lib and flow5-io-lib


QT -= gui
QT += core

TEMPLATE = app
TARGET = flow5-mcp

CONFIG += console c++20
CONFIG -= app_bundle


INCLUDEPATH += $$PWD/../XFoil-lib/
INCLUDEPATH += $$PWD/../flow5-lib/
INCLUDEPATH += $$PWD/../flow5-lib/api
INCLUDEPATH += $$PWD/../flow5-io-lib/
INCLUDEPATH += $$PWD/../flow5-io-lib/api

LIBS += -L../flow5-io-lib -lflow5-io-lib
LIBS += -L../flow5-lib -lflow5-lib


OBJECTS_DIR = ./objects
MOC_DIR     = ./moc
RCC_DIR     = ./rcc


linux-g++ {
    DEFINES += LINUX_OS

    LIBS += -L../XFoil-lib -lXFoil

    INCLUDEPATH += /usr/local/include/opencascade/  #make install location
    INCLUDEPATH += /usr/include/opencascade/        #fedora install location
    LIBS += -L/usr/local/lib/
    LIBS += -L/usr/lib64/

    LIBS += -lgmsh

    # find the flow5 libraries next to the executable or in the build tree
    QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN\'
    QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../flow5-lib\'
    QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../flow5-io-lib\'
    QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../XFoil-lib\'

    isEmpty(PREFIX){
        PREFIX = /usr/local
    }
    target.path = $$PREFIX/bin
    INSTALLS += target
}


win32-msvc {
    DEFINES += WIN_OS
    CONFIG -= debug_and_release debug_and_release_target

    LIBS += -L../XFoil-lib -lXFoil1

    INCLUDEPATH += D:/bin/build/OCCT/inc

    INCLUDEPATH += D:/bin/gmsh-4.14.1-Windows64-sdk/include/
    LIBS += -L"D:/bin/gmsh-4.14.1-Windows64-sdk/lib"
    LIBS += -lgmsh.dll  # the file name is gmsh.dll.lib

    QMAKE_CXXFLAGS += -utf-8
}


macx {
    DEFINES += MAC_OS
    QMAKE_MACOSX_DEPLOYMENT_TARGET = 13.3   # needed for c++20 / std::format
    QMAKE_MAC_SDK = macosx
    QMAKE_APPLE_DEVICE_ARCHS = x86_64 arm64

    LIBS += -L../XFoil-lib -lXFoil

    INCLUDEPATH += /usr/local/include/opencascade
    INCLUDEPATH += /usr/local/include
    LIBS += -L/usr/local/lib
    LIBS += -lgmsh

    QMAKE_LFLAGS += -Wl,-rpath,@executable_path/../Frameworks
}


SOURCES += \
    main.cpp \
    mcp/mcpserver.cpp \
    tools/common.cpp \
    tools/foiltools.cpp \
    tools/planetools.cpp \
    tools/projecttools.cpp \
    tools/resources.cpp

HEADERS += \
    mcp/mcpserver.h \
    tools/flow5tools.h

DISTFILES += \
    README.md \
    CMakeLists.txt \
    test/mcp_smoke_test.py
