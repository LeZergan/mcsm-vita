cmake_minimum_required(VERSION 3.14)
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
set(CMAKE_ERROR_DEPRECATED OFF CACHE BOOL "" FORCE)

if (NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Please define VITASDK env var to point to your SDK path or set CMAKE_TOOLCHAIN_FILE.")
endif ()

if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(VITA_REAL_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Resolved VitaSDK toolchain file")
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/cmake/vita.toolchain.wrapper.cmake" CACHE PATH "Toolchain file")
endif ()

project(so_loader C CXX)

include("${VITASDK}/share/vita.cmake" REQUIRED)
find_package(Python3 COMPONENTS Interpreter QUIET)

find_program(NETCAT_EXE
             NAMES nc ncat
             HINTS
             "C:/Program Files (x86)/Nmap"
             "C:/Program Files/Nmap")
if (NOT NETCAT_EXE)
  set(NETCAT_EXE "nc" CACHE STRING "Path to netcat-compatible executable for send/reboot targets")
endif()

# Vita application configuration
set(VITA_APP_NAME "Minecraft: Story Mode")
set(VITA_TITLEID "MCSM00001")
set(VITA_VPKNAME "mcsm_diag")
set(VITA_VERSION "01.02")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

# Optional: used for "send" and "dump" targets
set(PSVITAIP "192.168.0.198" CACHE STRING "PSVita IP (for FTP access)")
set(PARSECOREPATH "$ENV{PARSECOREPATH}" CACHE STRING "vita-parse-core main.py")
if ("${PARSECOREPATH}" STREQUAL "")
  set(PARSECOREPATH "${CMAKE_SOURCE_DIR}/tools/vita-parse-core/main.py" CACHE STRING "vita-parse-core main.py" FORCE)
endif()
if (Python3_EXECUTABLE)
  set(PYTHON_FOR_DUMP "${Python3_EXECUTABLE}")
else()
  set(PYTHON_FOR_DUMP "python")
endif()

# Game-specific definitions
set(DATA_PATH "ux0:data/mcsm/" CACHE STRING "Path to data (with trailing /)")
set(LIB_MAIN_SO_PATH "${DATA_PATH}libmain.so" CACHE STRING "Path to libmain.so")
set(LIB_GAMEENGINE_SO_PATH "${DATA_PATH}libGameEngine.so" CACHE STRING "Path to libGameEngine.so")
set(LIB_SDL2_SO_PATH "${DATA_PATH}libSDL2.so" CACHE STRING "Path to libSDL2.so")
set(LIB_FMOD_SO_PATH "${DATA_PATH}libfmod.so" CACHE STRING "Path to libfmod.so")
set(LIB_FMODSTUDIO_SO_PATH "${DATA_PATH}libfmodstudio.so" CACHE STRING "Path to libfmodstudio.so")

add_definitions(
  -DDATA_PATH="${DATA_PATH}"
  -DLIB_MAIN_SO_PATH="${LIB_MAIN_SO_PATH}"
  -DLIB_GAMEENGINE_SO_PATH="${LIB_GAMEENGINE_SO_PATH}"
  -DLIB_SDL2_SO_PATH="${LIB_SDL2_SO_PATH}"
  -DLIB_FMOD_SO_PATH="${LIB_FMOD_SO_PATH}"
  -DLIB_FMODSTUDIO_SO_PATH="${LIB_FMODSTUDIO_SO_PATH}"
)

option(USE_SCELIBC_IO "Use SceLibcBridge for IO and some other std functions" OFF)
if (USE_SCELIBC_IO)
  add_definitions(-DUSE_SCELIBC_IO)
endif()

option(ENABLE_TELEMETRY_LOGGING "Enable verbose runtime telemetry in ux0:data/mcsm/loader.log" ON)
if (ENABLE_TELEMETRY_LOGGING)
  add_definitions(-DDEBUG_SOLOADER -DFALSOJNI_DEBUGLEVEL=2)
endif()

set(SHADER_FORMAT "GLSL" CACHE STRING "Preferred shader format (one of 'GLSL', 'CG', 'GXP')")
if (${SHADER_FORMAT} STREQUAL "GLSL")
  add_definitions(-DUSE_GLSL_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "CG")
  add_definitions(-DUSE_CG_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "GXP")
  add_definitions(-DUSE_GXP_SHADERS)
else()
  message(FATAL_ERROR "SHADER_FORMAT option can only be one of: 'GLSL', 'CG', 'GXP'")
endif()

set(valid_dump_formats GLSL CG)
option(DUMP_COMPILED_SHADERS "When using GLSL or CG shader format, cache compiled shaders on disk" OFF)
if (DUMP_COMPILED_SHADERS)
  if (SHADER_FORMAT IN_LIST valid_dump_formats)
    add_definitions(-DDUMP_COMPILED_SHADERS)
  else()
    message(WARNING "DUMP_COMPILED_SHADERS has no effect for the chosen shader format ${SHADER_FORMAT}")
  endif()
endif()

# Unique debug definition for loader's logging to not interfere with anything
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_definitions(-DDEBUG_SOLOADER)
  # Uncomment this for verbose FalsoJNI logging:
  #add_definitions(-DFALSOJNI_DEBUGLEVEL=0)
  # Uncomment this for OpenGL logging:
  #add_definitions(-DDEBUG_OPENGL)
endif()

# makes sincos, sincosf, etc. visible
add_definitions(-D_GNU_SOURCE -D__POSIX_VISIBLE=999999)

# NEON codegen (2026-07-17): the Vita CPU is a Cortex-A9 w/ NEON. -mfpu=neon +
# -mcpu/-mtune=cortex-a9 lets GCC (already -O3 -ffast-math) vectorize the hot
# per-load loops (box-filter downsample, 565/4444/BGRA converters, memcpy). The
# float ABI stays softfp so the boundary with VitaSDK stubs is unchanged (no new
# mismatch beyond the ones -Wl,--no-warn-mismatch already covers).
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -mfloat-abi=softfp -mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon -std=gnu11 -Wno-deprecated -fno-unwind-tables -fno-asynchronous-unwind-tables")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-q -mfloat-abi=softfp -mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon -std=gnu++20 -Wno-write-strings -Wno-psabi -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O1 -g3")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O1 -g3")
else ()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g0 -ffast-math")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g0 -ffast-math")
endif()

# Some shipped Vita stubs are assembled without VFP ABI attributes; suppress linker
# ABI mismatch diagnostics so hard-float third-party libs can still link.
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--no-warn-mismatch")

add_executable(${CMAKE_PROJECT_NAME}
               source/main.c
               source/dynlib.c
               source/java.c
               source/patch.c
               source/reimpl/bits/_ctype.c
               source/reimpl/controls.c
               source/reimpl/controls.h
                source/reimpl/opensl_audio.c
                source/reimpl/errno.c
               source/reimpl/io.c
               source/reimpl/log.c
               source/reimpl/mem.c
               source/reimpl/miniz_zlib.c
               source/reimpl/pthr.c
               source/reimpl/sys.c
               source/reimpl/time64.c
               source/reimpl/asset_manager.cpp
               source/utils/config.c
               source/utils/dialog.c
               source/utils/glutil.c
               source/utils/init.c
               source/utils/launch_state.c
               source/utils/logger.c
               source/utils/telemetry.c
               source/utils/utils.c
               lib/falso_jni/FalsoJNI.c
               lib/falso_jni/FalsoJNI_ImplBridge.c
               lib/falso_jni/FalsoJNI_Logger.c
               lib/falso_jni/converter.c
               lib/sha1/sha1.c
               lib/fios/fios.c
               lib/so_util/so_util.c)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
               source/reimpl/egl.c
               source/utils/loading_screen.c)   # loading screen during scene loads

set(LIBC_BRIDGE_STUB "")
if (USE_SCELIBC_IO)
  add_subdirectory(lib/libc_bridge)
  add_dependencies(${CMAKE_PROJECT_NAME} SceLibcBridge)
  set(LIBC_BRIDGE_STUB "${CMAKE_BINARY_DIR}/lib/libc_bridge/stubs/libSceLibcBridge_stub.a")
endif()

target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM
                           PUBLIC ${CMAKE_SOURCE_DIR}/lib
                           PUBLIC ${CMAKE_SOURCE_DIR}/lib/kubridge
                           )

target_include_directories(${CMAKE_PROJECT_NAME}
                           PUBLIC ${CMAKE_SOURCE_DIR}/source
                           )

target_link_libraries(${CMAKE_PROJECT_NAME}
                    -Wl,--whole-archive pthread -Wl,--no-whole-archive
                    vitaGL
                    OpenSLES
                    m
                    stdc++
                    ${CMAKE_SOURCE_DIR}/lib/kubridge/libkubridge_stub.a
                    )

target_link_libraries(${CMAKE_PROJECT_NAME}
                      SceSysmem_stub
                      SceKernelThreadMgr_stub
                      SceKernelModulemgr_stub
                      SceKernelDmacMgr_stub
                      SceSysmodule_stub
                      SceUlt_stub
                      SceIofilemgr_stub
                      SceAppUtil_stub
                      SceAppMgr_stub
                      SceCtrl_stub
                      SceHid_stub
                      SceTouch_stub
                      SceMotion_stub
                      SceDisplay_stub
                      SceGxm_stub
                      SceNet_stub
                      SceNetCtl_stub
                      SceAudio_stub
                      SceAudioIn_stub
                      ScePower_stub
                      SceRtc_stub
                      SceSblSsMgr_stub
                      SceVshBridge_stub
                      SceCommonDialog_stub
                      SceFios2_stub
                      taihen_stub
                      ${LIBC_BRIDGE_STUB}
                      )

# vitaGL GLSL path: runtime shader compilation via vitaShaRK (libvitashark) + the
# SceShaccCg compiler stubs. SceShaccCgExt hooks to load the runtime compiler module,
# so it needs taihen AFTER it in link order (single-pass linker). mathneon resolves
# vitaGL's NEON math.
target_link_libraries(${CMAKE_PROJECT_NAME} vitashark SceShaccCg_stub SceShaccCgExt taihen_stub mathneon)

if (EXISTS "${CMAKE_SOURCE_DIR}/extras/livearea")
  set(VITA_LIVEAREA_DIR "${CMAKE_SOURCE_DIR}/extras/livearea" CACHE PATH "LiveArea content directory")
endif()

vita_create_self(eboot.bin ${CMAKE_PROJECT_NAME} UNSAFE)

vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
                VERSION ${VITA_VERSION}
                NAME ${VITA_APP_NAME}
                FILE ${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png sce_sys/icon0.png
                FILE ${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png sce_sys/pic0.png
                FILE ${CMAKE_SOURCE_DIR}/extras/livearea/bg0.png sce_sys/livearea/contents/bg0.png
                FILE ${CMAKE_SOURCE_DIR}/extras/livearea/startup.png sce_sys/livearea/contents/startup.png
                FILE ${CMAKE_SOURCE_DIR}/extras/livearea/template.xml sce_sys/livearea/contents/template.xml
                )

set(SYNC_DIR "ux0:data/mcsm/")

add_custom_target(send
  COMMAND cmake -P "${CMAKE_SOURCE_DIR}/cmake/sync_vpk.cmake"
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  COMMENT "Pushing ${VITA_VPKNAME}.vpk and log script to the device..."
)

add_custom_target(dump
  COMMAND ${PYTHON_FOR_DUMP} ${PARSECOREPATH} ${PSVITAIP}
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  COMMENT "Dumping crash core from ${PSVITAIP}..."
)

add_custom_target(log
  COMMAND ${PYTHON_FOR_DUMP} ${PARSECOREPATH} ${PSVITAIP} --get-loader-log
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  COMMENT "Grabbing loader.log from ${PSVITAIP}..."
)
