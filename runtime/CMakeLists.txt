cmake_minimum_required(VERSION 3.20)
project(psxrecomp-runtime C CXX)
include(CTest)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

# ---- Native/oracle runtime targets ----

set(PSXRECOMP_ROOT "${CMAKE_SOURCE_DIR}/..")
include("${CMAKE_SOURCE_DIR}/runtime.cmake")

psxrecomp_add_runtime_target(psx-runtime
    DEBUG_PORT 4370
    WINDOW_TITLE "psxrecomp - BIOS"
    DEFAULT_BIOS_PATH "${PSXRECOMP_ROOT}/bios/SCPH1001.BIN"
    EXE_NAME "PSXRecomp"
)

if(BUILD_TESTING)
    add_executable(launcher_device_roundtrip_test
        tests/test_launcher_device_roundtrip.cpp)
    target_include_directories(launcher_device_roundtrip_test PRIVATE include)
    add_test(NAME launcher_device_roundtrip_test
             COMMAND launcher_device_roundtrip_test)

    add_executable(psx_cyc_batch_test tests/test_psx_cyc_batch.c)
    target_include_directories(psx_cyc_batch_test PRIVATE include)
    target_compile_definitions(psx_cyc_batch_test PRIVATE
        PSX_ENABLE_BLOCK_CYCLES=1)
    add_test(NAME psx_cyc_batch_test COMMAND psx_cyc_batch_test)

    add_executable(psx_icache_fastpath_test
        tests/test_psx_icache_fastpath.c
        src/psx_icache.c
    )
    target_include_directories(psx_icache_fastpath_test PRIVATE include)
    # PSX_OVERLAY_DLL_BUILD is the seam psx_cycles.h already provides for a TU
    # that supplies its own psx_advance_cycles (overlay DLLs forward it through
    # a callback shim). Without it the header inlines the production definition
    # and this test's counting stub is a redefinition — the whole runtime test
    # suite stopped building at "redefinition of 'psx_advance_cycles'", which
    # also blocked every test registered after it.
    target_compile_definitions(psx_icache_fastpath_test PRIVATE
        PSX_ENABLE_BLOCK_CYCLES=1
        PSX_OVERLAY_DLL_BUILD=1)
    add_test(NAME psx_icache_fastpath_test COMMAND psx_icache_fastpath_test)

    add_executable(gte_register_access_test
        tests/test_gte_register_access.cpp
        src/gte.cpp
    )
    target_include_directories(gte_register_access_test PRIVATE include)
    target_compile_definitions(gte_register_access_test PRIVATE
        PSX_NO_DEBUG_TOOLS=1
        PSX_GTE_REGISTER_TEST=1
        PSX_ENABLE_BLOCK_CYCLES=1)
    add_test(NAME gte_register_access_test COMMAND gte_register_access_test)

    add_executable(psx_instr_cost_test tests/test_psx_instr_cost.cpp)
    target_include_directories(psx_instr_cost_test PRIVATE include)
    add_test(NAME psx_instr_cost_test COMMAND psx_instr_cost_test)

    add_executable(ws_aspect_cone_math_test
        tests/test_ws_aspect_cone_math.c
        src/ws_aspect_cone_math.c)
    target_include_directories(ws_aspect_cone_math_test PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(ws_aspect_cone_math_test PRIVATE m)
    endif()
    add_test(NAME ws_aspect_cone_math_test COMMAND ws_aspect_cone_math_test)

    add_executable(ws_ui_group_test
        tests/test_ws_ui_group.c
        src/ws_ui_group.c)
    target_include_directories(ws_ui_group_test PRIVATE include)
    add_test(NAME ws_ui_group_test COMMAND ws_ui_group_test)

    add_executable(overlay_capture_retry_test
        tests/test_overlay_capture_retry.cpp
        src/overlay_capture.c)
    target_include_directories(overlay_capture_retry_test PRIVATE
        include ${PSX_SDL_INCLUDE_DIRS})
    target_compile_definitions(overlay_capture_retry_test PRIVATE
        SDL_MAIN_HANDLED PSX_OVERLAY_CAPTURE_TEST=1
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>)
    target_link_libraries(overlay_capture_retry_test PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME overlay_capture_retry_test COMMAND overlay_capture_retry_test)

    add_executable(overlay_path_canon_test
        tests/test_overlay_path_canon.c
        src/overlay_path_canon.c)
    target_include_directories(overlay_path_canon_test PRIVATE include)
    add_test(NAME overlay_path_canon_test COMMAND overlay_path_canon_test)

    # The player-facing "skip the BIOS" flag must resolve to the same boot
    # behaviour on retail SCPH-1001 and on the bundled OpenBIOS, whose
    # kernel-call HLE is (correctly) refused for want of a DeliverEvent anchor.
    add_executable(bios_hle_plan_test
        tests/test_bios_hle_plan.c
        src/bios_hle_plan.c)
    target_include_directories(bios_hle_plan_test PRIVATE include)
    add_test(NAME bios_hle_plan_test COMMAND bios_hle_plan_test)

    # Either member of a disc dump (.cue or .bin) is a valid pick: both mount
    # the same disc and identify off the same data track.
    add_executable(disc_path_resolve_test
        tests/test_disc_path_resolve.cpp
        src/disc_path.cpp
        src/cue_sheet.cpp
        src/iso_reader.cpp)
    target_include_directories(disc_path_resolve_test PRIVATE include)
    add_test(NAME disc_path_resolve_test COMMAND disc_path_resolve_test)

    # Both BIOS filename conventions (bare vs region-qualified) resolve, in
    # both directions, so a config and a dump folder may disagree.
    add_executable(bios_rom_alias_test tests/test_bios_rom_alias.cpp)
    target_include_directories(bios_rom_alias_test PRIVATE
        ${PSXRECOMP_ROOT}/recompiler/include)
    add_test(NAME bios_rom_alias_test COMMAND bios_rom_alias_test)

    # Multi-file cue TOC mapping (data track + CD-DA in separate bins).
    add_executable(iso_reader_cdda_test
        tests/test_iso_reader_cdda.cpp
        src/iso_reader.cpp
        src/cue_sheet.cpp)
    target_include_directories(iso_reader_cdda_test PRIVATE include)
    add_test(NAME iso_reader_cdda_test COMMAND iso_reader_cdda_test)

    add_executable(mod_packages_test
        tests/test_mod_packages.cpp
        src/mod_packages.cpp
        src/crc32.c
        src/psx_sha256.c)
    target_include_directories(mod_packages_test PRIVATE
        include
        ../recompiler/lib/toml11)
    add_test(NAME mod_packages_test COMMAND mod_packages_test)

    add_executable(mod_runtime_test
        tests/test_mod_runtime.cpp
        src/mod_runtime.cpp
        src/mod_packages.cpp
        src/disc_path.cpp
        src/cue_sheet.cpp
        src/crc32.c
        src/psx_sha256.c)
    target_include_directories(mod_runtime_test PRIVATE
        include
        ../recompiler/lib/toml11)
    add_test(NAME mod_runtime_test COMMAND mod_runtime_test)

    if(WIN32)
        add_executable(autocompile_publication_test
            tests/test_autocompile_publication.c
            src/autocompile.c)
        target_include_directories(autocompile_publication_test PRIVATE include)
        target_compile_definitions(autocompile_publication_test PRIVATE
            PSX_AUTOCOMPILE_TEST=1)
        add_test(NAME autocompile_publication_test
                 COMMAND autocompile_publication_test)
    endif()
endif()

if(NOT MSVC)
    psxrecomp_add_runtime_target(psx-oracle
        ORACLE
        DEBUG_PORT 4371
        WINDOW_TITLE "psxrecomp - Oracle"
        DEFAULT_BIOS_PATH "${PSXRECOMP_ROOT}/bios/SCPH1001.BIN"
    )
endif()

# ---- psx-beetle target (Beetle PSX libretro standalone, MinGW) ----
#
# Standalone binary that runs Beetle PSX (mednafen-psx libretro core) with
# a debug server exposing the SAME wire-protocol commands as psx-runtime,
# on a different port (4380). No recomp code linked.
#
# Requires beetle-psx built via:
#   cd beetle-psx && make platform=mingw_x86_64 STATIC_LINKING=1 HAVE_LIGHTREC=0

set(BEETLE_LIB "${CMAKE_SOURCE_DIR}/../beetle-psx/libmednafen_psx.a")

if(NOT MSVC AND EXISTS "${BEETLE_LIB}")
    message(STATUS "psx-beetle: found ${BEETLE_LIB}")

    set(BEETLE_DIR "${CMAKE_SOURCE_DIR}/../beetle-psx")
    set(LRC "${BEETLE_DIR}/libretro-common")
    set(LIBRETRO_COMMON_SOURCES
        ${LRC}/streams/file_stream.c
        ${LRC}/streams/file_stream_transforms.c
        ${LRC}/vfs/vfs_implementation.c
        ${LRC}/file/file_path.c
        ${LRC}/compat/compat_strl.c
        ${LRC}/compat/fopen_utf8.c
        ${LRC}/compat/compat_posix_string.c
        ${LRC}/string/stdstring.c
        ${LRC}/lists/string_list.c
        ${LRC}/encodings/encoding_utf.c
        ${LRC}/time/rtime.c
        ${LRC}/rthreads/rthreads.c
        ${LRC}/memmap/memalign.c
        ${LRC}/hash/rhash.c
    )

    add_executable(psx-beetle
        src/beetle_main.cpp
        src/beetle_libretro.cpp
        src/beetle_debug_server.c
        src/frame_pacing.c
        src/parity_trace.c
        src/device_trace.c
        src/audio_trace.c
        ${LIBRETRO_COMMON_SOURCES}
    )

    target_include_directories(psx-beetle PRIVATE
        include
        ${PSX_SDL_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/../beetle-psx/libretro-common/include
        ${CMAKE_SOURCE_DIR}/../beetle-psx
    )

    target_compile_definitions(psx-beetle PRIVATE
        PSX_BEETLE_BUILD=1
        DEFAULT_DEBUG_PORT=4380
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>
    )
    if(NOT PSX_DEBUG_TOOLS)
        target_compile_definitions(psx-beetle PRIVATE PSX_NO_DEBUG_TOOLS=1)
    endif()

    target_link_libraries(psx-beetle PRIVATE
        "${BEETLE_LIB}"
        ${PSX_SDL_LIBRARIES}
    )

    if(WIN32 OR MINGW)
        target_link_libraries(psx-beetle PRIVATE ws2_32 -lpthread)
    endif()

    if(MINGW)
        target_link_options(psx-beetle PRIVATE -Wl,--stack,67108864)
    endif()
elseif(NOT MSVC)
    message(STATUS "psx-beetle: libmednafen_psx.a not found - skipping psx-beetle")
endif()
