#-------------------------------------------------
#
# Project created by QtCreator 2018-07-09T13:50:58
#
#-------------------------------------------------

QT       += core gui opengl widgets webenginewidgets

TARGET = MeshTool
TEMPLATE = app

# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS
DEFINES += _HAS_STD_BYTE=0

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

INCLUDEPATH += $$PWD/../../ThirdParty/
INCLUDEPATH += $$PWD/../../ThirdParty/opencv/include
INCLUDEPATH += $$PWD/../../ThirdParty/boost
INCLUDEPATH += $$PWD/../../ThirdParty/libkml
INCLUDEPATH += $$PWD/../../ThirdParty/fbxsdk/include
INCLUDEPATH += $$PWD/../../ThirdParty/geographiclib/include
INCLUDEPATH += $$PWD/../../ThirdParty/gtest-1.7.0/include/
INCLUDEPATH += $$PWD/../../ThirdParty/libexpat/lib/
INCLUDEPATH += $$PWD/../../ThirdParty/uriparser-0.7.5/include/
INCLUDEPATH += $$PWD/../../ThirdParty/zlib-1.2.3/contrib/
INCLUDEPATH += $$PWD/../../ThirdParty/zlib-1.2.3/
INCLUDEPATH += $$PWD/include
INCLUDEPATH += $$PWD/hfa
INCLUDEPATH += $$PWD/port

QMAKE_LIBDIR += $$PWD/../../ThirdParty/opencv/x64/vc15/lib

CONFIG(debug, debug|release) {
        QMAKE_LIBDIR += $$PWD/../../ThirdParty/fbxsdk/x64/debug
        LIBS += opencv_world341d.lib
    }
    else {
        QMAKE_LIBDIR += $$PWD/../../ThirdParty/fbxsdk/x64/release
        LIBS += opencv_world341.lib
    }

LIBS += libfbxsdk-md.lib zlib-md.lib libxml2-md.lib opengl32.lib glu32.lib Advapi32.lib

QMAKE_LFLAGS_WINDOWS += -Wl,--stack,100000000
QMAKE_LFLAGS_WINDOWS += -Wl,--heap,100000000

CONFIG += -static -static-runtime

SOURCES += \
        main.cpp \
        mainwindow.cpp \
    oglwidget.cpp \
    GlFunctionList.cpp \
    GpaDumpAnalyzeTool.cpp \
    CoreFile.cpp \
    MeshData.cpp \
    GpaMeshCreator.cpp \
    KmlFileParser.cpp \
    CoreGeographic.cpp \
    MeshExport.cpp \
    coretexture.cpp \
    hfa/hfaband.cpp \
    hfa/hfacompress.cpp \
    hfa/hfadictionary.cpp \
    hfa/hfaentry.cpp \
    hfa/hfafield.cpp \
    hfa/hfaopen.cpp \
    hfa/hfatype.cpp \
    port/cpl_conv.cpp \
    port/cpl_csv.cpp \
    port/cpl_dir.cpp \
    port/cpl_error.cpp \
    port/cpl_findfile.cpp \
    port/cpl_multiproc.cpp \
    port/cpl_path.cpp \
    port/cpl_string.cpp \
    port/cpl_vsi_mem.cpp \
    port/cpl_vsil.cpp \
    port/cpl_vsil_win32.cpp \
    port/cpl_vsisimple.cpp \
    UsgsMeshCreator.cpp \
    UsgsTextureDump.cpp \
    oglshaders.cpp \
    debugout.cpp \
    oglmesh.cpp \
    MeshImport.cpp \
    earthmesh.cpp \
    oglcamera.cpp \
    imagedownload.cpp

HEADERS += \
    GpaDumpAnalyzeTool.h \
    hfa/hfa.h \
    hfa/hfa_p.h \
    port/cpl_config.h \
    port/cpl_config.h.vc \
    port/cpl_conv.h \
    port/cpl_csv.h \
    port/cpl_error.h \
    port/cpl_multiproc.h \
    port/cpl_port.h \
    port/cpl_string.h \
    port/cpl_vsi.h \
    port/cpl_vsi_private.h \
    include/meshtexture.h \
    include/base.h \
    include/corebounds.h \
    include/corefile.h \
    include/coregeographic.h \
    include/coremath.h \
    include/corematrix.h \
    include/coreprimitive.h \
    include/corequaternion.h \
    include/coretexture.h \
    include/corevector.h \
    include/glfunctionlist.h \
    include/kmlfileparser.h \
    include/mainwindow.h \
    include/meshdata.h \
    include/meshtexture.h \
    include/oglwidget.h \
    include/worlddata.h \
    include/oglshaders.h \
    include/debugout.h \
    include/oglcamera.h \
    include/imagedownload.h

SOURCES += $$files($$PWD/../../ThirdParty/geographiclib/src/*.cpp)
SOURCES += $$files($$PWD/../../ThirdParty/libkml/kml/dom/*.cc)
SOURCES += $$files($$PWD/../../ThirdParty/libkml/kml/base/*.cc)
SOURCES += $$files($$PWD/../../ThirdParty/libkml/kml/engine/*.cc)
SOURCES += $$files($$PWD/../../ThirdParty/libexpat/lib/*.c)
SOURCES += $$files($$PWD/../../ThirdParty/uriparser-0.7.5/lib/*.c)
SOURCES += $$files($$PWD/../../ThirdParty/zlib-1.2.11/*.c)
SOURCES += $$files($$PWD/../../ThirdParty/zlib-1.2.3/contrib/minizip/*.c)

FORMS += \
        mainwindow.ui

DISTFILES +=

RESOURCES += \
    resources.qrc

CONFIG += c++17
