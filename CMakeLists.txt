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

# The Android engine modules and this loader use the softfp ABI. A second, regular
# (hard-float) VitaSDK is commonly installed on development machines and used to be
# silently accepted because the linker mismatch warning was suppressed below. That
# produces a binary whose calls can disagree about where float arguments live.
# Reject that toolchain before compiling and print the exact vitaGL archive identity
# so release builds are attributable to a concrete dependency.
set(MCSM_VITAGL_ARCHIVE "${VITASDK}/arm-vita-eabi/lib/libvitaGL.a")
find_program(MCSM_READELF
             NAMES arm-vita-eabi-readelf
             HINTS "${VITASDK}/bin"
             NO_DEFAULT_PATH)
if (NOT EXISTS "${MCSM_VITAGL_ARCHIVE}")
  message(FATAL_ERROR "vitaGL archive not found: ${MCSM_VITAGL_ARCHIVE}")
endif()
if (NOT MCSM_READELF)
  message(FATAL_ERROR "arm-vita-eabi-readelf not found under ${VITASDK}/bin")
endif()
execute_process(COMMAND "${MCSM_READELF}" -A "${MCSM_VITAGL_ARCHIVE}"
                RESULT_VARIABLE MCSM_VITAGL_ATTR_RC
                OUTPUT_VARIABLE MCSM_VITAGL_ATTRIBUTES
                ERROR_VARIABLE MCSM_VITAGL_ATTR_ERROR)
if (NOT MCSM_VITAGL_ATTR_RC EQUAL 0)
  message(FATAL_ERROR "Could not inspect vitaGL ABI: ${MCSM_VITAGL_ATTR_ERROR}")
endif()
if (MCSM_VITAGL_ATTRIBUTES MATCHES "Tag_ABI_VFP_args:[ \t]*VFP registers")
  message(FATAL_ERROR
          "Hard-float vitaGL detected at ${MCSM_VITAGL_ARCHIVE}. "
          "MCSM requires the softfp VitaSDK; refusing an ABI-mixed build.")
endif()
file(SHA256 "${MCSM_VITAGL_ARCHIVE}" MCSM_VITAGL_SHA256)
set(MCSM_KNOWN_GOOD_VITAGL_SHA256
    "ea9110d0941eb961e09da90f4be034f0aafeaae83d8ace03b69a889fe0f5c913")
if (MCSM_VITAGL_SHA256 STREQUAL MCSM_KNOWN_GOOD_VITAGL_SHA256)
  message(STATUS "vitaGL: known-good rollback (${MCSM_VITAGL_SHA256})")
else()
  message(WARNING
          "vitaGL differs from the device-tested rollback: ${MCSM_VITAGL_SHA256}. "
          "Rendering/orientation/alpha must be hardware-validated before release.")
endif()

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
set(VITA_VPKNAME "MCSM-1.10")
set(VITA_VERSION "01.10")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

# ☠ THE NP COMMUNICATION ID HAS EXACTLY ONE DEFINITION, AND THIS IS IT.
# It is NOT the title id. Three places have to agree or there are simply no trophies:
#   1. the folder the pack is packaged into (sce_sys/trophy/<id>_00/TROPHY.TRP)
#   2. the id the loader passes to sceNpTrophyCreateContext  (source/utils/trophies.c)
#   3. the <npcommid> inside the pack itself                 (tools/trophies.def)
# They drifted apart once already -- packaged as MCSM00002_00, requested as MCSM00001,
# declared as MCSM00001_00 -- which silently disabled trophies on every fresh install
# and was invisible until a device log showed the context failing to open. (1) and (2)
# are now both derived from this variable; (3) is a data file, so tools/trophies.def
# still states it and must be kept in step by hand when rebuilding the pack.
#
# BACK TO MCSM00001 (2026-07-31). This was MCSM00002 for a while because MCSM00001's
# registration had been poisoned on the test console -- deleting a set's conf/data
# folders leaves its row in the trophy database behind, and the half-state that
# produces is not repairable in place (TRPUSER.DAT is PFS-sealed). Moving to an id the
# system had never seen was the workaround, and it cost a permanent ghost entry in the
# trophy list for every id abandoned that way.
#
# Burning a fresh id per accident is not the answer -- each one abandons a permanent
# ghost entry in the trophy list and fixes nothing. Clear the old registration instead
# and keep the id. What is known, all observed on device 2026-07-31:
#   - Deleting a set's three folders ALONE leaves its title row in trophy_local.db
#     with no trophy rows, and the game then boots to NP-10229-4 "data corrupted".
#   - Trophy Manager (ONElua) deletes the folders AND trophy_local.db, and the system
#     rebuilds that database from the folders that remain. That does fully remove the
#     set -- verified: no MCSM rows left anywhere, retail sets intact.
#
# ☠ BUT THAT IS NOT SUFFICIENT, AND THIS IS UNRESOLVED. After a complete clean-up
# (no MCSM trophy folders, no MCSM rows in trophy_local.db, no MCSM reference left in
# ur0:shell/db/app.db, app correctly registered with 29 tbl_appinfo rows), a build
# carrying sce_sys/trophy STILL fails to launch with NP-6182-7 "could not prepare the
# application to start", while the byte-identical build without the trophy folder
# launches and runs fine. The TRP itself was verified sound (v2, correct header SHA-1,
# valid SFM XML, right npcommid). So the remaining fault is system-side state on that
# one console that has not been found yet -- trophies are reported working on other
# consoles with the same VPK. Do not read the id choice below as "the trophy problem
# is solved"; it is not.
#
# If this ever needs to move again, all three places must move together, and
# tools/trophies.def is the one that will not follow automatically.
# ★ TEST BUILDS USE MCSM00003, SHIPPING BUILDS USE MCSM00001 (rule set 2026-07-31).
# Anything that reaches a tester must be MCSM00001 -- that is the id their console will
# carry forever, so it has to be the real one. Local experiments use MCSM00003 instead,
# so a poisoned or half-registered set on the dev console can never contaminate the id
# players get. MCSM00002 is burned (an abandoned experiment already ghosted on the dev
# console); do not reuse it.
#
# Override per build, do not edit this line:
#     cmake -B build_test -DMCSM_NP_COMM_ID=MCSM00003 ...
# The pack's INTERNAL id must match, so a matching TROPHY.TRP has to be supplied for
# whichever id is selected -- `python tools/trp_tool.py setcommid <trp> <old> <new>`
# rewrites an existing pack in place (same-length ids, digest recomputed).
set(MCSM_NP_COMM_ID "MCSM00001" CACHE STRING "NP communication id (MCSM00001 = ship, MCSM00003 = test)")

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
  # Same value that names the sce_sys/trophy/<id>_00/ folder below, so the loader
  # cannot ask for an id the package does not contain. See MCSM_NP_COMM_ID above.
  -DMCSM_NP_COMM_ID="${MCSM_NP_COMM_ID}"
)

option(USE_SCELIBC_IO "Use SceLibcBridge for IO and some other std functions" OFF)
set(MCSM_FIOS_SOURCE "")
set(MCSM_FIOS_STUB "")
if (USE_SCELIBC_IO)
  add_definitions(-DUSE_SCELIBC_IO)
  set(MCSM_FIOS_SOURCE lib/fios/fios.c)
  set(MCSM_FIOS_STUB SceFios2_stub)
endif()

option(ENABLE_TELEMETRY_LOGGING "Enable verbose runtime telemetry in ux0:data/mcsm/loader.log" OFF)
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
# float ABI stays softfp so the boundary with VitaSDK stubs is unchanged. The
# archive check above now rejects a hard-float vitaGL instead of hiding a mismatch.
# WARNINGS ARE ON (2026-07-30). This project pokes engine structs at raw byte
# offsets, hand-builds ARM trampolines and passes floats through function pointers
# whose prototypes it has to get exactly right -- compiler diagnostics are the
# cheapest safety net available here, and there were none at all: no -Wall, no
# -Wextra, so the standing "zero warnings" claim was vacuous. Turning them on
# surfaced 7 real items on the first build, including a dead throttle variable that
# had silently disabled a 30s log throttle.
# Suppressions are per-reason, never blanket, so the remaining ~40 checks stay
# meaningful (-Wformat alone caught a real crash in file_load's OOM path):
#   -Wno-unused-parameter    the reimpl/ layer implements fixed POSIX/Android/FMOD
#                            signatures; unimplemented parameters are the point.
#   -Wno-sign-compare        all 9 instances are inside vendored
#                            lib/falso_jni/FalsoJNI_ImplBridge.h, not our code.
#   -Wno-unknown-pragmas     vendored FalsoJNI_Logger.h carries clang/IDE pragmas.
# (-Wno-format-truncation used to be here as well, for ONE snprintf in
# reimpl/time64.c. Suppressing it for the whole build to silence a single known-safe
# call is exactly the blanket suppression the miniz note below argues against, and it
# would hide a genuine truncation anywhere in the ~30 other files that format into
# fixed buffers -- several of which build device-log paths. Scoped to that file.)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -mfloat-abi=softfp -mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unknown-pragmas -Wno-deprecated -fno-unwind-tables -fno-asynchronous-unwind-tables")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-q -mfloat-abi=softfp -mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon -std=gnu++20 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unknown-pragmas -Wno-deprecated -Wno-write-strings -Wno-psabi -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O1 -g3")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O1 -g3")
else ()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g0 -ffast-math -DNDEBUG -ffunction-sections -fdata-sections")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g0 -ffast-math -DNDEBUG -ffunction-sections -fdata-sections")
endif()

# Dead loader/FIOS compatibility paths account for hundreds of KiB of BSS when
# USE_SCELIBC_IO is off. Per-function/data sections let the linker discard them.
# Keep ABI mismatch diagnostics enabled: suppressing them hid a hard-float vitaGL.
if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
endif()

add_executable(${CMAKE_PROJECT_NAME}
               source/main.c
               source/dynlib.c
               source/java.c
               source/patch.c
               source/reimpl/bits/_ctype.c
               source/reimpl/controls.c
               source/reimpl/controls.h
                    source/reimpl/fmod_native_probe.c
                    source/reimpl/fmod_output_vita.c
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
               source/utils/trophies.c
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
                ${MCSM_FIOS_SOURCE}
                lib/so_util/so_util.c)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
               source/reimpl/egl.c)

# miniz_zlib.c #includes the vendored third_party/miniz sources, whose narrow-type
# comparisons trip -Wtype-limits. Scoped to THIS translation unit rather than added
# to CMAKE_C_FLAGS, so -Wtype-limits keeps working on code we actually own -- the
# whole point of turning warnings on is that a clean build makes new ones visible.
set_source_files_properties(source/reimpl/miniz_zlib.c
                            PROPERTIES COMPILE_OPTIONS "-Wno-type-limits")

# time64.c's asctime_r writes into the 26-byte buffer the POSIX contract mandates,
# so GCC's truncation warning is advisory there and nowhere else. Same reasoning as
# miniz above: scope it, do not disable the check for the whole build.
set_source_files_properties(source/reimpl/time64.c
                            PROPERTIES COMPILE_OPTIONS "-Wno-format-truncation")

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
                      # Vita trophies. VitaSDK ships this stub but NO header, so the
                      # API is declared by hand in source/utils/trophies.c.
                       SceNpTrophy_stub
                       ${MCSM_FIOS_STUB}
                       taihen_stub
                      ${LIBC_BRIDGE_STUB}
                      )

# vitaGL GLSL path: runtime shader compilation via vitaShaRK (libvitashark) + the
# SceShaccCg compiler stubs. SceShaccCgExt hooks to load the runtime compiler module,
# so it needs taihen AFTER it in link order (single-pass linker). mathneon resolves
# vitaGL's NEON math.
# ☠ DO NOT ADD ScePerf_stub BACK. A previous pass linked it to read the Cortex-A9
# hardware performance counters and left a comment here calling it "the profiler we
# can actually use". It is not: scePerfArmPmon* are kernelgroup{ScePerf} in
# psp2/perf.h, so the stub LINKS but the user-mode call is invalid and crashed before
# the first frame. Reading the PMU needs a taihen kernel plugin. The full write-up is
# above g_chore_us in source/patch.c; wall-clock phase timing is what we have.
target_link_libraries(${CMAKE_PROJECT_NAME} vitashark SceShaccCg_stub SceShaccCgExt taihen_stub mathneon)

if (EXISTS "${CMAKE_SOURCE_DIR}/extras/livearea")
  set(VITA_LIVEAREA_DIR "${CMAKE_SOURCE_DIR}/extras/livearea" CACHE PATH "LiveArea content directory")
endif()

vita_create_self(eboot.bin ${CMAKE_PROJECT_NAME} UNSAFE)

# Trophy pack. The system resolves it by NP communication id, not by title id.
# Both this path and trophies.c come from MCSM_NP_COMM_ID at the top of this
# file -- see the note there for why that matters. Requires the NoTrpDrm plugin
# on the device to be readable.
#
# OPTIONAL, because the pack's icons are the retail game's own art and are not
# published with the source. Without it the loader builds and runs exactly as
# before; UnlockAchievement simply has no pack to unlock against. Build one with
# tools/make_trophy_pack.py. Checked at configure time, so re-run cmake after
# adding it.
set(MCSM_TROPHY_FILE "")
if (EXISTS "${CMAKE_SOURCE_DIR}/extras/trophy/TROPHY.TRP")
  set(MCSM_TROPHY_FILE FILE ${CMAKE_SOURCE_DIR}/extras/trophy/TROPHY.TRP
                            sce_sys/trophy/${MCSM_NP_COMM_ID}_00/TROPHY.TRP)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE MCSM_HAVE_TROPHY_PACK=1)
else()
  # ☠ The runtime MUST be told, not left to discover it. sceNpTrophyCreateContext
  # SUCCEEDS even when no pack is present, so without this flag the loader concludes
  # "not registered yet" and runs sceNpTrophySetupDialogInit -- the SYSTEM trophy
  # setup dialog -- which then cannot complete, spins the full 3600-frame guard and
  # puts an NP error dialog on screen on EVERY boot. Device-observed 2026-07-31:
  #   TROPHY: set not registered yet (rc=0x80551612 count=0) -- running setup once
  #   TROPHY: setup dialog did not finish in 3600 frames -- continuing anyway
  # Deleting the pack from the VPK is therefore NOT sufficient to disable trophies;
  # the code path has to be switched off at the same time, from the same fact.
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE MCSM_HAVE_TROPHY_PACK=0)
  message(STATUS "No extras/trophy/TROPHY.TRP -- building with trophies DISABLED "
                 "(no context, no setup dialog). See tools/make_trophy_pack.py.")
endif()

vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
                VERSION ${VITA_VERSION}
                NAME ${VITA_APP_NAME}
                FILE ${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png sce_sys/icon0.png
                ${MCSM_TROPHY_FILE}
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
