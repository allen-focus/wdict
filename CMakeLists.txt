# TODO:
#   LTO & PGO  (Release only, opt-in — not yet enabled)
#
#   LTO (Link Time Optimization)
#     Lets the linker optimize across translation units: better inlining,
#     dead code elimination, constant propagation across .c files.
#     MSVC: adds /GL (compile) + /LTCG (link). Expect 5-15% runtime gain,
#     5-10x slower links. Enable with one line:
#       set_target_properties(${PROJECT_NAME} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
#
#   PGO (Profile Guided Optimization)
#     Uses real execution data to guide inlining, branch layout, and register
#     allocation — typically 10-15% on top of LTO. Three-step process:
#       1. Instrumented build  /GL /LTCG:PGI  — produces a slower binary that
#          writes .pgc profile files as it runs
#       2. Run a representative workload  (bad training data = wrong optimizations)
#       3. Optimized build     /GL /LTCG:PGO  — compiler uses the .pgc data
#     Not worth doing manually; automate it in CI before enabling.

cmake_minimum_required(VERSION 3.16)

set(CMAKE_CONFIGURATION_TYPES "Debug;Profile;Release" CACHE STRING "" FORCE)

project(wdict C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#
# Compiler / platform guard
#

if(NOT MSVC)
  message(FATAL_ERROR "This project currently targets MSVC only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Only x64 builds are supported.")
endif()

#
# Build configurations
#
#   Debug   – fast iteration, debugger-friendly   (/Od /Zi /RTC1)
#   Profile – optimised + Tracy                   (/O2 /Zi + TRACY_ENABLE)
#   Release – ship build, zero overhead           (/O2 /GS- + linker opts)
#

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_GENERATOR MATCHES "Visual Studio")
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

#
# Sources
#

file(GLOB C_SOURCES CONFIGURE_DEPENDS "*.c")
add_executable(${PROJECT_NAME} ${C_SOURCES})

#
# Precompiled header  (CMake 3.16+)
#

target_precompile_headers(${PROJECT_NAME} PRIVATE pch.h)

#
# Compile options
#

target_compile_options(${PROJECT_NAME} PRIVATE
    /W3 /WX /MP /utf-8
    /D_CRT_SECURE_NO_WARNINGS
    $<$<CONFIG:Debug>:/Od /Zi /RTC1>
    $<$<CONFIG:Profile>:/O2 /Zi /DNDEBUG /DTRACY_ENABLE /DTRACY_SUBSYSTEMS=0x7FF>
    $<$<CONFIG:Release>:/O2 /GS- /DNDEBUG>
)

#
# Linker options
#

target_link_options(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:/SUBSYSTEM:WINDOWS>
    $<$<CONFIG:Profile>:/SUBSYSTEM:WINDOWS>
    $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS /FIXED /DYNAMICBASE:NO /INCREMENTAL:NO /OPT:ICF /OPT:REF>
)

#
# Tracy  (Profile only)
#
# TracyClient.cpp is a C++ TU compiled as a separate static library so it
# doesn't infect the C sources with C++ compiler flags.
#
# It lives in its own subdirectory so that CMAKE_CXX_FLAGS_DEBUG can be
# overridden in that scope without affecting the rest of the build.
# (set_target_properties COMPILE_OPTIONS cannot remove flags that come from
#  CMAKE_CXX_FLAGS_DEBUG — those are injected at a lower layer.)
#

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tracy/public/TracyClient.cpp")
  add_subdirectory(thirdparty/tracy)

  target_link_libraries(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Profile>:tracy_client>
    )
endif()

#
# zstd decompression  (embedded — dictionary is stored compressed in the .exe)
#

file(GLOB ZSTD_SOURCES
    "thirdparty/zstd/common/*.c"
    "thirdparty/zstd/decompress/*.c")
list(FILTER ZSTD_SOURCES EXCLUDE REGEX "\\.S$")
add_library(zstd STATIC ${ZSTD_SOURCES})
target_include_directories(zstd PUBLIC "thirdparty/zstd")
target_compile_options(zstd PRIVATE /W0)
target_link_libraries(${PROJECT_NAME} PRIVATE zstd)

#
# Manifest  (embedded, optional)
#

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/main.manifest")
  target_link_options(${PROJECT_NAME} PRIVATE
        /MANIFEST:EMBED
        /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/main.manifest
    )
endif()

#
# Shaders  (fxc baked into headers, not linked as resources)
#

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/shader.hlsl")
  set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
  set(FXC_FLAGS /nologo /O3 /WX /Zpc /Ges /Qstrip_reflect /Qstrip_debug /Qstrip_priv)
  set(SHADER_STAMP "${SHADER_DIR}/shader_compile.stamp")

  add_custom_command(
        OUTPUT
            "${SHADER_DIR}/d3d11_vshader.h"
            "${SHADER_DIR}/d3d11_pshader.h"
            "${SHADER_DIR}/d3d11_pshader_comp.h"
            "${SHADER_STAMP}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_DIR}"
        COMMAND fxc.exe ${FXC_FLAGS} /T vs_5_0 /E vs
                /Fh "${SHADER_DIR}/d3d11_vshader.h" /Vn d3d11_vshader
                "${CMAKE_CURRENT_SOURCE_DIR}/shader.hlsl"
        COMMAND fxc.exe ${FXC_FLAGS} /T ps_5_0 /E ps
                /Fh "${SHADER_DIR}/d3d11_pshader.h" /Vn d3d11_pshader
                "${CMAKE_CURRENT_SOURCE_DIR}/shader.hlsl"
        COMMAND fxc.exe ${FXC_FLAGS} /T ps_5_0 /E ps /D COMPOSITION_PATH=1
                /Fh "${SHADER_DIR}/d3d11_pshader_comp.h" /Vn d3d11_pshader_comp
                "${CMAKE_CURRENT_SOURCE_DIR}/shader.hlsl"
        COMMAND ${CMAKE_COMMAND} -E touch "${SHADER_STAMP}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shader.hlsl"
        COMMENT "Compiling HLSL shaders"
        VERBATIM
    )

  add_custom_target(shaders DEPENDS "${SHADER_STAMP}")
  foreach(source ${C_SOURCES})
    set_source_files_properties(${source} PROPERTIES OBJECT_DEPENDS "${SHADER_STAMP}")
  endforeach()

  add_dependencies(${PROJECT_NAME} shaders)
endif()

#
# Resource
#

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resource.rc")
  set(RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")
  set(RC_OUTPUT "${RESOURCE_DIR}/resource.res")
  set(RESOURCE_STAMP "${RESOURCE_DIR}/resource_compile.stamp")

  add_custom_command(
        OUTPUT "${RC_OUTPUT}" "${RESOURCE_STAMP}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RESOURCE_DIR}"
        COMMAND rc /nologo /fo "${RC_OUTPUT}" "${CMAKE_CURRENT_SOURCE_DIR}/resource.rc"
        COMMAND ${CMAKE_COMMAND} -E touch "${RESOURCE_STAMP}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resource.rc"
                "${CMAKE_CURRENT_SOURCE_DIR}/data/dict.bin.zstd"
        COMMENT "Compiling resource file"
        VERBATIM
    )

  add_custom_target(resource_compile DEPENDS "${RESOURCE_STAMP}")
  add_dependencies(${PROJECT_NAME} resource_compile)

  target_link_options(${PROJECT_NAME} PRIVATE "${RC_OUTPUT}")
endif()

#
# Output directory  (mirrors the original build/ layout)
#

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_CURRENT_SOURCE_DIR}/build"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}/build"
    RUNTIME_OUTPUT_DIRECTORY_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/build"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/build"
)

#
# Message
#

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Build successfully, see build\\${PROJECT_NAME}.exe"
    COMMENT ""
    VERBATIM
)
