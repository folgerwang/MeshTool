cmake_minimum_required(VERSION 3.16)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
project(MeshTool LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Compile definitions
# ---------------------------------------------------------------------------
add_definitions(-D_HAS_STD_BYTE=0 -D_CRT_SECURE_NO_WARNINGS -DUNICODE=0 -D_MBCS)

# ---------------------------------------------------------------------------
# Static runtime (/MT or /MTd)
# ---------------------------------------------------------------------------
# CRT will be set per-target after add_executable

# ---------------------------------------------------------------------------
# Find packages
# ---------------------------------------------------------------------------
find_package(Vulkan REQUIRED)

# Build GLFW from source (in thirdparty/)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/glfw)

# OpenCV 3.4.1 -- prefer the ThirdParty copy
set(OpenCV_DIR "${CMAKE_SOURCE_DIR}/../../ThirdParty/opencv")
find_package(OpenCV 3.4.1 QUIET)
if(NOT OpenCV_FOUND)
    # Fallback: set include/lib paths manually
    set(OPENCV_INCLUDE "${CMAKE_SOURCE_DIR}/../../ThirdParty/opencv/include")
    set(OPENCV_LIB_DIR "${CMAKE_SOURCE_DIR}/../../ThirdParty/opencv/x64/vc15/lib")
endif()

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
# ThirdParty location - adjust if your layout differs
set(THIRDPARTY_ROOT "G:/work-c/ThirdParty")

include_directories(
    ${THIRDPARTY_ROOT}
    ${THIRDPARTY_ROOT}/opencv/include
    ${THIRDPARTY_ROOT}/boost
    ${THIRDPARTY_ROOT}/libkml
    ${THIRDPARTY_ROOT}/fbxsdk/include
    ${THIRDPARTY_ROOT}/geographiclib/include
    ${THIRDPARTY_ROOT}/gtest-1.7.0/include
    ${THIRDPARTY_ROOT}/libexpat/lib
    ${THIRDPARTY_ROOT}/uriparser-0.7.5/include
    ${THIRDPARTY_ROOT}/zlib-1.2.3/contrib
    ${THIRDPARTY_ROOT}/zlib-1.2.3
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/vulkan
    ${CMAKE_SOURCE_DIR}/glhook
    ${CMAKE_SOURCE_DIR}/hfa
    ${CMAKE_SOURCE_DIR}/port
    ${CMAKE_SOURCE_DIR}/thirdparty
    ${CMAKE_SOURCE_DIR}/thirdparty/imgui
    ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends
    ${CMAKE_SOURCE_DIR}/thirdparty/nfd/src/include
    ${CMAKE_SOURCE_DIR}/thirdparty/cpp-httplib
    ${CMAKE_SOURCE_DIR}/thirdparty/stb
    ${Vulkan_INCLUDE_DIRS}
)

# ---------------------------------------------------------------------------
# Library directories
# ---------------------------------------------------------------------------
link_directories(
    ${THIRDPARTY_ROOT}/opencv/x64/vc15/lib
    $<IF:$<CONFIG:Debug>,
        ${THIRDPARTY_ROOT}/fbxsdk/x64/debug,
        ${THIRDPARTY_ROOT}/fbxsdk/x64/release>
)

# ---------------------------------------------------------------------------
# Project sources (carried over from .pro, minus Qt-specific files)
# ---------------------------------------------------------------------------
set(MESHTOOL_SOURCES
    GlFunctionList.cpp
    GpaDumpAnalyzeTool.cpp
    CoreFile.cpp
    MeshData.cpp
    GpaMeshCreator.cpp
    KmlFileParser.cpp
    CoreGeographic.cpp
    MeshExport.cpp
    coretexture.cpp
    UsgsMeshCreator.cpp
    UsgsTextureDump.cpp
    debugout.cpp
    MeshImport.cpp
    earthmesh.cpp
    oglcamera.cpp
    LiveCaptureProcessor.cpp
    texture_util.cpp
    glhook/glhook_injector.cpp
)

# New Vulkan / GLFW sources
set(NEW_SOURCES
    src/app.cpp
    src/ui.cpp
    src/input.cpp
    src/file_downloader.cpp
    src/vulkan/vk_renderer.cpp
    src/vulkan/vk_buffer.cpp
    src/vulkan/vk_texture_manager.cpp
    src/vulkan/vk_pipeline.cpp
    src/vulkan/vk_mesh_renderer.cpp
)

# HFA sources
file(GLOB HFA_SOURCES "${CMAKE_SOURCE_DIR}/hfa/*.cpp")

# Port sources
file(GLOB PORT_SOURCES "${CMAKE_SOURCE_DIR}/port/*.cpp")

# ---------------------------------------------------------------------------
# Dear ImGui sources
# ---------------------------------------------------------------------------
file(GLOB IMGUI_SOURCES "${CMAKE_SOURCE_DIR}/thirdparty/imgui/*.cpp")
set(IMGUI_BACKEND_SOURCES
    ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends/imgui_impl_glfw.cpp
    ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends/imgui_impl_vulkan.cpp
)

# ---------------------------------------------------------------------------
# Native File Dialog (nfd)
# ---------------------------------------------------------------------------
file(GLOB NFD_SOURCES "${CMAKE_SOURCE_DIR}/thirdparty/nfd/src/*.c"
                      "${CMAKE_SOURCE_DIR}/thirdparty/nfd/src/*.cpp")

# ---------------------------------------------------------------------------
# ThirdParty glob sources (matching .pro)
# ---------------------------------------------------------------------------
file(GLOB GEOGRAPHICLIB_SOURCES "${THIRDPARTY_ROOT}/geographiclib/src/*.cpp")
file(GLOB LIBKML_DOM_SOURCES    "${THIRDPARTY_ROOT}/libkml/kml/dom/*.cc")
file(GLOB LIBKML_BASE_SOURCES   "${THIRDPARTY_ROOT}/libkml/kml/base/*.cc")
file(GLOB LIBKML_ENGINE_SOURCES "${THIRDPARTY_ROOT}/libkml/kml/engine/*.cc")
file(GLOB LIBEXPAT_SOURCES      "${THIRDPARTY_ROOT}/libexpat/lib/*.c")
file(GLOB URIPARSER_SOURCES     "${THIRDPARTY_ROOT}/uriparser-0.7.5/lib/*.c")
file(GLOB ZLIB_SOURCES          "${THIRDPARTY_ROOT}/zlib-1.2.11/*.c")
file(GLOB MINIZIP_SOURCES       "${THIRDPARTY_ROOT}/zlib-1.2.3/contrib/minizip/*.c")

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(${PROJECT_NAME}
    src/main.cpp
    ${MESHTOOL_SOURCES}
    ${NEW_SOURCES}
    ${HFA_SOURCES}
    ${PORT_SOURCES}
    ${IMGUI_SOURCES}
    ${IMGUI_BACKEND_SOURCES}
    ${NFD_SOURCES}
    ${GEOGRAPHICLIB_SOURCES}
    ${LIBKML_DOM_SOURCES}
    ${LIBKML_BASE_SOURCES}
    ${LIBKML_ENGINE_SOURCES}
    ${LIBEXPAT_SOURCES}
    ${URIPARSER_SOURCES}
    ${ZLIB_SOURCES}
    ${MINIZIP_SOURCES}
)

# Use dynamic CRT (/MD) to match FBX SDK's libfbxsdk-md.lib
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Debug>:/MDd>
        $<$<CONFIG:Release>:/MD>
    )
endif()

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
if(OpenCV_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${OpenCV_LIBS})
else()
    target_link_directories(${PROJECT_NAME} PRIVATE ${OPENCV_LIB_DIR})
    target_link_libraries(${PROJECT_NAME} PRIVATE
        $<IF:$<CONFIG:Debug>,opencv_world341d,opencv_world341>
    )
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    Vulkan::Vulkan
    glfw
    libfbxsdk-md.lib
    zlib-md.lib
    libxml2-md.lib
    Advapi32.lib
    RPCRT4.lib
)

# ---------------------------------------------------------------------------
# Windows-specific linker flags
# ---------------------------------------------------------------------------
if(WIN32)
    target_link_options(${PROJECT_NAME} PRIVATE /STACK:100000000)
endif()

# ---------------------------------------------------------------------------
# SPIR-V shader compilation
# ---------------------------------------------------------------------------
file(GLOB SHADER_SOURCES
    "${CMAKE_SOURCE_DIR}/shaders/*.vert"
    "${CMAKE_SOURCE_DIR}/shaders/*.frag"
    "${CMAKE_SOURCE_DIR}/shaders/*.comp"
    "${CMAKE_SOURCE_DIR}/shaders/*.geom"
    "${CMAKE_SOURCE_DIR}/shaders/*.tesc"
    "${CMAKE_SOURCE_DIR}/shaders/*.tese"
)

set(SPIRV_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SPIRV_OUTPUT_DIR})

foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPIRV_OUTPUT "${SPIRV_OUTPUT_DIR}/${SHADER_NAME}.spv")

    add_custom_command(
        OUTPUT ${SPIRV_OUTPUT}
        COMMAND Vulkan::glslc ${SHADER} -o ${SPIRV_OUTPUT}
        DEPENDS ${SHADER}
        COMMENT "Compiling SPIR-V: ${SHADER_NAME}"
    )
    list(APPEND SPIRV_OUTPUTS ${SPIRV_OUTPUT})
endforeach()

if(SPIRV_OUTPUTS)
    add_custom_target(compile_shaders ALL DEPENDS ${SPIRV_OUTPUTS})
    add_dependencies(${PROJECT_NAME} compile_shaders)
endif()

# ---------------------------------------------------------------------------
# GLHookDLL - Proxy opengl32.dll for intercepting Google Earth's GL calls
# ---------------------------------------------------------------------------
add_library(GLHookDLL SHARED
    glhook/glhook_main.cpp
    glhook/glhook_interceptors.cpp
    glhook/glhook_ipc_writer.cpp
    glhook/glhook_state.cpp
    glhook/glhook_overlay.cpp
)

set_target_properties(GLHookDLL PROPERTIES
    OUTPUT_NAME "opengl32"
    PREFIX ""
)

target_include_directories(GLHookDLL PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/glhook
)

target_sources(GLHookDLL PRIVATE glhook/glhook_exports.def)
target_link_libraries(GLHookDLL PRIVATE kernel32 user32)

target_compile_definitions(GLHookDLL PRIVATE GLHOOK_EXPORTS _CRT_SECURE_NO_WARNINGS)

# Injectable hook DLL (loaded via CreateRemoteThread injection, not proxy)
add_library(GLHookInject SHARED
    glhook/glhook_injectable.cpp
    glhook/glhook_injector.cpp
)
set_target_properties(GLHookInject PROPERTIES OUTPUT_NAME "meshtool_hook" PREFIX "")
target_link_libraries(GLHookInject PRIVATE kernel32 user32)
target_compile_definitions(GLHookInject PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(GLHookInject PRIVATE $<$<CONFIG:Debug>:/MDd> $<$<CONFIG:Release>:/MD>)
endif()

# Minimal test proxy (no hooks, just forwarding - for debugging)
add_library(GLHookTest SHARED glhook/glhook_test_minimal.cpp)
set_target_properties(GLHookTest PROPERTIES OUTPUT_NAME "opengl32_test" PREFIX "")
target_sources(GLHookTest PRIVATE glhook/glhook_exports.def)
target_link_libraries(GLHookTest PRIVATE kernel32 user32)
target_compile_definitions(GLHookTest PRIVATE _CRT_SECURE_NO_WARNINGS)

# Match MeshTool's CRT linkage
if(MSVC)
    target_compile_options(GLHookDLL PRIVATE
        $<$<CONFIG:Debug>:/MDd>
        $<$<CONFIG:Release>:/MD>
    )
endif()

add_executable(test_inject test_inject.cpp glhook/glhook_injector.cpp)
target_include_directories(test_inject PRIVATE glhook)
target_link_libraries(test_inject PRIVATE kernel32 user32)
target_compile_definitions(test_inject PRIVATE _CRT_SECURE_NO_WARNINGS)
