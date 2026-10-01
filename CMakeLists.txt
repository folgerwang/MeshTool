cmake_minimum_required(VERSION 3.16)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
project(MeshTool LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Compile definitions
# ---------------------------------------------------------------------------
add_definitions(-D_HAS_STD_BYTE=0 -D_CRT_SECURE_NO_WARNINGS -DUNICODE=0 -D_MBCS)

# Dynamic CRT (/MD, /MDd) everywhere
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# ---------------------------------------------------------------------------
# Dependencies -- everything comes from ./thirdparty (run setup.bat first)
# ---------------------------------------------------------------------------
find_package(Vulkan REQUIRED)
include(${CMAKE_SOURCE_DIR}/cmake/ThirdParty.cmake)

# ---------------------------------------------------------------------------
# Project sources
# ---------------------------------------------------------------------------
set(MESHTOOL_SOURCES
    GlFunctionList.cpp
    GpaDumpAnalyzeTool.cpp
    corefile.cpp
    meshdata.cpp
    GpaMeshCreator.cpp
    kmlfileparser.cpp
    coregeographic.cpp
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

set(NEW_SOURCES
    src/app.cpp
    src/ui.cpp
    src/input.cpp
    src/viewcamera.cpp
    src/scenefile.cpp
    src/geoview.cpp
    src/segment/segmenter.cpp
    src/segment/qwen_client.cpp
    src/file_downloader.cpp
    src/vulkan/vk_renderer.cpp
    src/vulkan/vk_buffer.cpp
    src/vulkan/vk_texture_manager.cpp
    src/vulkan/vk_pipeline.cpp
    src/vulkan/vk_mesh_renderer.cpp
)

file(GLOB HFA_SOURCES  "${CMAKE_SOURCE_DIR}/hfa/*.cpp")
file(GLOB PORT_SOURCES "${CMAKE_SOURCE_DIR}/port/*.cpp")

# Dear ImGui (+ GLFW / Vulkan backends)
file(GLOB IMGUI_SOURCES "${TP}/imgui/*.cpp")
set(IMGUI_BACKEND_SOURCES
    ${TP}/imgui/backends/imgui_impl_glfw.cpp
    ${TP}/imgui/backends/imgui_impl_vulkan.cpp
)

# Native File Dialog -- Windows backend only
set(NFD_SOURCES
    ${TP}/nfd/src/nfd_common.c
    ${TP}/nfd/src/nfd_win.cpp
)

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
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/segment
    ${CMAKE_SOURCE_DIR}/src/vulkan
    ${CMAKE_SOURCE_DIR}/glhook
    ${CMAKE_SOURCE_DIR}/hfa
    ${CMAKE_SOURCE_DIR}/port
    ${TP}/imgui
    ${TP}/imgui/backends
    ${TP}/nfd/src/include
    ${TP}/cpp-httplib
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Vulkan::Vulkan
    glfw
    mt_kml
    mt_geographic
    mt_zlib
    mt_glm
    mt_stb
    Advapi32.lib
    RPCRT4.lib
)

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
    # Put the .spv files next to the exe as well, so it runs from any working dir
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${SPIRV_OUTPUT_DIR} $<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders
        COMMENT "Copying SPIR-V shaders next to MeshTool.exe")
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
    glhook/glhook_panel.cpp
    glhook/glhook_exports.def
)
# Built into <config>/proxy/: next to MeshTool.exe it would be loaded by MeshTool
# itself (as its opengl32.dll) and run the capture hook in the wrong process.
set_target_properties(GLHookDLL PROPERTIES OUTPUT_NAME "opengl32" PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/proxy")
target_include_directories(GLHookDLL PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/glhook
)
target_link_libraries(GLHookDLL PRIVATE kernel32 user32)
target_compile_definitions(GLHookDLL PRIVATE GLHOOK_EXPORTS _CRT_SECURE_NO_WARNINGS)

# Injectable hook DLL (loaded via CreateRemoteThread injection, not proxy)
add_library(GLHookInject SHARED
    glhook/glhook_injectable.cpp
    glhook/glhook_injector.cpp
    glhook/glhook_panel.cpp
)
set_target_properties(GLHookInject PROPERTIES OUTPUT_NAME "meshtool_hook" PREFIX "")
target_include_directories(GLHookInject PRIVATE ${CMAKE_SOURCE_DIR}/glhook)
target_link_libraries(GLHookInject PRIVATE kernel32 user32)
target_compile_definitions(GLHookInject PRIVATE _CRT_SECURE_NO_WARNINGS)

# Minimal test proxy (no hooks, just forwarding - for debugging)
add_library(GLHookTest SHARED glhook/glhook_test_minimal.cpp glhook/glhook_exports.def)
set_target_properties(GLHookTest PROPERTIES OUTPUT_NAME "opengl32_test" PREFIX "")
target_link_libraries(GLHookTest PRIVATE kernel32 user32)
target_compile_definitions(GLHookTest PRIVATE _CRT_SECURE_NO_WARNINGS)

# Stand-alone injector for testing meshtool_hook.dll
add_executable(test_inject test_inject.cpp glhook/glhook_injector.cpp)
target_include_directories(test_inject PRIVATE glhook)
target_link_libraries(test_inject PRIVATE kernel32 user32)
target_compile_definitions(test_inject PRIVATE _CRT_SECURE_NO_WARNINGS)
