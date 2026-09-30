# ---------------------------------------------------------------------------
# ThirdParty.cmake -- builds / locates every dependency from <repo>/thirdparty
#
# thirdparty/ is populated by setup.bat (it is gitignored). Nothing here reads
# from outside the repository.
#
# Creates these targets:
#   mt_zlib, mt_expat, mt_uriparser, mt_geographic, mt_kml   (static libs)
#   mt_glm, mt_stb                                           (header-only)
#   glfw                                                     (from GLFW's CMake)
# ---------------------------------------------------------------------------

set(TP "${CMAKE_SOURCE_DIR}/thirdparty")

# ---- sanity check: did the user run setup.bat? -----------------------------
set(_mt_required
    "glfw/CMakeLists.txt"
    "imgui/imgui.cpp"
    "nfd/src/nfd_common.c"
    "cpp-httplib/httplib.h"
    "zlib/zlib.h"
    "libexpat/expat/lib/xmlparse.c"
    "uriparser/include/uriparser/Uri.h"
    "geographiclib/include/GeographicLib/UTMUPS.hpp"
    "libkml/src/kml/dom.h"
    "boost/boost/scoped_ptr.hpp"
    "glm/glm/glm.hpp"
    "stb/stb_image_write.h")
foreach(_f ${_mt_required})
    if(NOT EXISTS "${TP}/${_f}")
        message(FATAL_ERROR "Missing thirdparty/${_f}\n"
                            "Run setup.bat in the repository root first.")
    endif()
endforeach()

set(MT_GENERATED_INCLUDE "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MT_GENERATED_INCLUDE}")

# Third-party code is old and noisy -- silence warnings for it.
if(MSVC)
    set(_mt_tp_flags /w)
else()
    set(_mt_tp_flags -w)
endif()

# ---------------------------------------------------------------------------
# zlib + minizip (zip writer; libkml brings its own unzip)
# ---------------------------------------------------------------------------
add_library(mt_zlib STATIC
    ${TP}/zlib/adler32.c   ${TP}/zlib/compress.c ${TP}/zlib/crc32.c
    ${TP}/zlib/deflate.c   ${TP}/zlib/gzclose.c  ${TP}/zlib/gzlib.c
    ${TP}/zlib/gzread.c    ${TP}/zlib/gzwrite.c  ${TP}/zlib/infback.c
    ${TP}/zlib/inffast.c   ${TP}/zlib/inflate.c  ${TP}/zlib/inftrees.c
    ${TP}/zlib/trees.c     ${TP}/zlib/uncompr.c  ${TP}/zlib/zutil.c
    ${TP}/zlib/contrib/minizip/zip.c
    ${TP}/zlib/contrib/minizip/ioapi.c
)
target_include_directories(mt_zlib PUBLIC ${TP}/zlib ${TP}/zlib/contrib)
target_compile_definitions(mt_zlib PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
target_compile_options(mt_zlib PRIVATE ${_mt_tp_flags})

# ---------------------------------------------------------------------------
# expat (static; config header generated here instead of running its CMake)
# ---------------------------------------------------------------------------
file(WRITE "${MT_GENERATED_INCLUDE}/expat_config.h.tmp" [=[
#ifndef EXPAT_CONFIG_H
#define EXPAT_CONFIG_H 1
#define BYTEORDER 1234
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_FCNTL_H 1
#define STDC_HEADERS 1
#define PACKAGE "expat"
#define PACKAGE_NAME "expat"
#define PACKAGE_VERSION "2.6.2"
#define XML_CONTEXT_BYTES 1024
#define XML_DTD 1
#define XML_GE 1
#define XML_NS 1
#endif
]=])
configure_file("${MT_GENERATED_INCLUDE}/expat_config.h.tmp"
               "${MT_GENERATED_INCLUDE}/expat_config.h" COPYONLY)

add_library(mt_expat STATIC
    ${TP}/libexpat/expat/lib/xmlparse.c
    ${TP}/libexpat/expat/lib/xmlrole.c
    ${TP}/libexpat/expat/lib/xmltok.c
)
target_include_directories(mt_expat
    PUBLIC  ${TP}/libexpat/expat/lib
    PRIVATE ${MT_GENERATED_INCLUDE})
target_compile_definitions(mt_expat PUBLIC XML_STATIC PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(mt_expat PRIVATE ${_mt_tp_flags})

# ---------------------------------------------------------------------------
# uriparser 0.7.5
# ---------------------------------------------------------------------------
file(GLOB _uri_src "${TP}/uriparser/lib/*.c")
add_library(mt_uriparser STATIC ${_uri_src})
target_include_directories(mt_uriparser PUBLIC ${TP}/uriparser/include)
target_compile_definitions(mt_uriparser PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(mt_uriparser PRIVATE ${_mt_tp_flags})

# ---------------------------------------------------------------------------
# GeographicLib (Config.h normally produced by its CMake; generate it here)
# ---------------------------------------------------------------------------
file(MAKE_DIRECTORY "${MT_GENERATED_INCLUDE}/GeographicLib")
set(PROJECT_VERSION        "2.3")
set(PROJECT_VERSION_MAJOR  2)
set(PROJECT_VERSION_MINOR  3)
set(PROJECT_VERSION_PATCH  0)
set(GEOGRAPHICLIB_DATA     "")
set(GEOGRAPHICLIB_HAVE_LONG_DOUBLE 0)
set(GEOGRAPHICLIB_WORDS_BIGENDIAN  0)
set(GEOGRAPHICLIB_PRECISION        2)
set(GEOGRAPHICLIB_LIB_TYPE_VAL     0)
configure_file("${TP}/geographiclib/include/GeographicLib/Config.h.in"
               "${MT_GENERATED_INCLUDE}/GeographicLib/Config.h")

file(GLOB _geo_src "${TP}/geographiclib/src/*.cpp")
add_library(mt_geographic STATIC ${_geo_src})
target_include_directories(mt_geographic PUBLIC
    ${MT_GENERATED_INCLUDE}
    ${TP}/geographiclib/include)
target_compile_definitions(mt_geographic PUBLIC GEOGRAPHICLIB_SHARED_LIB=0)
target_compile_options(mt_geographic PRIVATE ${_mt_tp_flags})

# ---------------------------------------------------------------------------
# libkml (base + dom + engine), needs boost headers (scoped_ptr/intrusive_ptr)
# ---------------------------------------------------------------------------
set(_kml "${TP}/libkml/src/kml")
file(GLOB _kml_base   "${_kml}/base/*.cc")
file(GLOB _kml_dom    "${_kml}/dom/*.cc")
file(GLOB _kml_engine "${_kml}/engine/*.cc")
list(FILTER _kml_base   EXCLUDE REGEX "_test\\.cc$")
list(FILTER _kml_dom    EXCLUDE REGEX "_test\\.cc$")
list(FILTER _kml_engine EXCLUDE REGEX "_test\\.cc$")
if(WIN32)
    list(FILTER _kml_base EXCLUDE REGEX "/file_posix\\.cc$")
    list(APPEND _kml_base "${_kml}/base/contrib/strptime.c")
    # MSVC only declares the underscored name
    set_source_files_properties("${_kml}/base/contrib/strptime.c"
        PROPERTIES COMPILE_DEFINITIONS "tzname=_tzname")
    # file_win32.cc is written against the wide-char Win32 API
    set_source_files_properties("${_kml}/base/file_win32.cc"
        PROPERTIES COMPILE_DEFINITIONS "UNICODE;_UNICODE")
else()
    list(FILTER _kml_base EXCLUDE REGEX "/file_win32\\.cc$")
endif()
list(APPEND _kml_base
    "${_kml}/base/contrib/minizip/unzip.c"
    "${_kml}/base/contrib/minizip/iomem_simple.c")

add_library(mt_kml STATIC ${_kml_base} ${_kml_dom} ${_kml_engine})
target_include_directories(mt_kml PUBLIC
    ${TP}/libkml/src
    ${TP}/boost)
target_link_libraries(mt_kml PUBLIC mt_expat mt_uriparser mt_zlib)
target_compile_definitions(mt_kml PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(mt_kml PRIVATE ${_mt_tp_flags})

# ---------------------------------------------------------------------------
# GLFW (its own CMake)
# ---------------------------------------------------------------------------
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory(${TP}/glfw ${CMAKE_BINARY_DIR}/glfw EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Header-only: glm (math), stb (image load / write)
# ---------------------------------------------------------------------------
add_library(mt_glm INTERFACE)
target_include_directories(mt_glm INTERFACE ${TP}/glm)
target_compile_definitions(mt_glm INTERFACE GLM_FORCE_SILENT_WARNINGS)

add_library(mt_stb INTERFACE)
target_include_directories(mt_stb INTERFACE ${TP}/stb)
