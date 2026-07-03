# CMakeLists.txt - builds the two RPC executables and the MIDL stubs.
#
#   Client:  WinApiDemo.exe  (windowed)  = WinApiDemo.cpp + Timesvc_c.c
#   Server:  TimeServer.exe  (console)   = time.c        + Timesvc_s.c
#
# Build (from a Developer Command Prompt so midl/cl are on PATH):
#   cmake -S . -B build -A x64        # or: -A Win32
#   cmake --build build --config Debug
# Run (from build\Debug):
#   TimeServer.exe                    # leave this running
#   WinApiDemo.exe

cmake_minimum_required(VERSION 3.20)
project(WinApiDemo LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Requires Windows with the MSVC toolchain (RPC/MIDL/GDI).")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)            # __has_include guard in WinApiDemo.cpp
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# MIDL: generate Timesvc.h / _c.c / _s.c from the interface + acf.
# ---------------------------------------------------------------------------
find_program(MIDL_EXECUTABLE NAMES midl REQUIRED)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MIDL_ENV x64)
else()
    set(MIDL_ENV win32)
endif()

set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_DIR})

set(MIDL_H ${GEN_DIR}/Timesvc.h)
set(MIDL_C ${GEN_DIR}/Timesvc_c.c)
set(MIDL_S ${GEN_DIR}/Timesvc_s.c)

add_custom_command(
    OUTPUT  ${MIDL_H} ${MIDL_C} ${MIDL_S}
    COMMAND ${MIDL_EXECUTABLE} /nologo /env ${MIDL_ENV}
            /h Timesvc.h /cstub Timesvc_c.c /sstub Timesvc_s.c
            /out ${GEN_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/Timesvc.idl
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Timesvc.idl
            ${CMAKE_CURRENT_SOURCE_DIR}/Timesvc.acf
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}   # so midl finds Timesvc.acf
    COMMENT "MIDL: compiling Timesvc.idl (${MIDL_ENV})"
    VERBATIM
)
add_custom_target(rpc_stubs DEPENDS ${MIDL_H} ${MIDL_C} ${MIDL_S})

# ---------------------------------------------------------------------------
# Server: TimeServer.exe  (links the server stub Timesvc_s.c)
# ---------------------------------------------------------------------------
add_executable(TimeServer time.c ${MIDL_S} ${MIDL_H})
add_dependencies(TimeServer rpc_stubs)
target_include_directories(TimeServer PRIVATE ${GEN_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(TimeServer PRIVATE rpcrt4 user32)

# ---------------------------------------------------------------------------
# Client: WinApiDemo.exe  (WIN32 => /SUBSYSTEM:WINDOWS; links client stub)
# ---------------------------------------------------------------------------
add_executable(WinApiDemo WIN32 WinApiDemo.cpp ${MIDL_C} ${MIDL_H})
add_dependencies(WinApiDemo rpc_stubs)
target_include_directories(WinApiDemo PRIVATE ${GEN_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(WinApiDemo PRIVATE rpcrt4 user32 gdi32)
