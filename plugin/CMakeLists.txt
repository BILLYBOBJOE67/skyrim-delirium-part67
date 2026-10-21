cmake_minimum_required(VERSION 3.24)
project(SkyrimDelirium VERSION 0.1.0 LANGUAGES CXX)

# C++23 + dynamic CRT to match CommonLibSSE-NG (vcpkg triplet x64-windows-static-md).
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# AE only (1.6.x). CommonLib checkout shared with SkyFluid; Delirium's C header from the Delirium checkout
# (the DLL is loaded at runtime, nothing links against it).
set(COMMONLIB_DIR "C:/path/to/CommonLibSSE-NG" CACHE PATH "CommonLibSSE-NG checkout")
set(DELIRIUM_DIR "C:/path/to/delirium-physics" CACHE PATH "Delirium checkout")
set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)
add_subdirectory(${COMMONLIB_DIR} commonlib EXCLUDE_FROM_ALL)

find_path(SIMPLEINI_INCLUDE_DIRS "SimpleIni.h")

# host: SKSE entry, hooks, keys, command file. core: the engine, loaded by the host from a temporary copy so it
# can be rebuilt and reloaded while the game runs (tools\hotreload.ps1).
add_library(SkyrimDelirium SHARED src/main.cpp)
add_library(SkyrimDeliriumCore SHARED src/core.cpp src/engine.cpp src/collision.cpp)
foreach(t SkyrimDelirium SkyrimDeliriumCore)
	target_include_directories(${t} PRIVATE src ${SIMPLEINI_INCLUDE_DIRS} "${DELIRIUM_DIR}/crates/delirium-sdk/include")
	target_precompile_headers(${t} PRIVATE src/PCH.h)
	target_link_libraries(${t} PRIVATE CommonLibSSE::CommonLibSSE)
	target_compile_definitions(${t} PRIVATE DELIRIUM_VERSION="${PROJECT_VERSION}" NOMINMAX WIN32_LEAN_AND_MEAN)
	target_compile_options(${t} PRIVATE /W4 /permissive- /Zc:preprocessor /utf-8 /wd4100 /wd4324)
	# the DLL names its PDB without a folder: debuggers find it beside the loaded copy, never lock the build's
	target_link_options(${t} PRIVATE /DEBUG:FULL /PDBALTPATH:%_PDB%)
endforeach()
