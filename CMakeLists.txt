cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Robotech Battlecry (Xbox) - Static Recompilation
#
# Scaffolded from xboxrecomp templates/new-game, matching other xboxrecomp projects /
# Redcard 2003 / NHL Hitz Pro layout on this machine (<tools>).
# ============================================================================

project(robotech_bc_recomp C)

# Toolkit libraries explicitly use /O2 even in Debug; MSVC's /RTC1 conflicts.
if(MSVC)
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Prefer the junction at tools/xboxrecomp (shared the build PC toolkit under <tools>\tools).
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)
include(patches/rbx_virtual_disk.cmake)
include(patches/rbx_present.cmake)

# Interactive pad input for this title requires bak OHCI. Shared toolkit
# ohci.c is frequently overwritten by XAPI_TD WIP (empty interrupt EDs /
# bad freelist). Compile a project-pinned copy and drop the live file.
set(_RBX_OHCI_BAK "${CMAKE_CURRENT_SOURCE_DIR}/patches/ohci.c.dx11bak")
set(_RBX_OHCI_PIN "${CMAKE_CURRENT_SOURCE_DIR}/patches/ohci_rbx_pinned.c")
if(EXISTS "${_RBX_OHCI_BAK}" AND TARGET xbox_usb)
    configure_file("${_RBX_OHCI_BAK}" "${_RBX_OHCI_PIN}" COPYONLY)
    get_target_property(_USB_SOURCES xbox_usb SOURCES)
    set(_USB_FILTERED "")
    foreach(_s IN LISTS _USB_SOURCES)
        get_filename_component(_sn "${_s}" NAME)
        if(NOT _sn STREQUAL "ohci.c")
            list(APPEND _USB_FILTERED "${_s}")
        endif()
    endforeach()
    set_property(TARGET xbox_usb PROPERTY SOURCES ${_USB_FILTERED})
    target_sources(xbox_usb PRIVATE "${_RBX_OHCI_PIN}")
    message(STATUS "Robotech: linking pinned bak OHCI (${_RBX_OHCI_PIN})")
endif()

# Project-pinned NV2A pushbuffer executor (vertex cache, hot-path trims;
# see the header of patches/nv2a_pb_exec_rbx.c). Re-sync it by hand when the
# shared toolkit's nv2a_pb_exec.c gains fixes this title needs.
set(_RBX_PBEXEC "${CMAKE_CURRENT_SOURCE_DIR}/patches/nv2a_pb_exec_rbx.c")
if(EXISTS "${_RBX_PBEXEC}" AND TARGET xbox_kernel)
    get_target_property(_K_SOURCES xbox_kernel SOURCES)
    set(_K_FILTERED "")
    foreach(_s IN LISTS _K_SOURCES)
        get_filename_component(_sn "${_s}" NAME)
        if(NOT _sn STREQUAL "nv2a_pb_exec.c" AND NOT _sn STREQUAL "nv2a_pb_scan.c")
            list(APPEND _K_FILTERED "${_s}")
        endif()
    endforeach()
    set_property(TARGET xbox_kernel PROPERTY SOURCES ${_K_FILTERED})
    # Pinned pushbuffer walk too (patches/nv2a_pb_scan_rbx.c): a mid-walk GET
    # rewrite must not drop the rest of a frame's commands.
    target_sources(xbox_kernel PRIVATE "${_RBX_PBEXEC}" "${CMAKE_CURRENT_SOURCE_DIR}/patches/nv2a_pb_scan_rbx.c")
    message(STATUS "Robotech: linking pinned NV2A executor (${_RBX_PBEXEC}) and pushbuffer walk")
endif()

# Project-pinned APU voice processor (see the header of
# patches/apu_vp_rbx.c): waits out a title's VOICE_LOCK instead of skipping
# the voice's frame, which garbled the Bink movie audio.
set(_RBX_APUVP "${CMAKE_CURRENT_SOURCE_DIR}/patches/apu_vp_rbx.c")
if(EXISTS "${_RBX_APUVP}" AND TARGET xbox_apu)
    get_target_property(_APU_SOURCES xbox_apu SOURCES)
    set(_APU_FILTERED "")
    foreach(_s IN LISTS _APU_SOURCES)
        get_filename_component(_sn "${_s}" NAME)
        if(NOT _sn STREQUAL "apu_vp.c")
            list(APPEND _APU_FILTERED "${_s}")
        endif()
    endforeach()
    set_property(TARGET xbox_apu PROPERTY SOURCES ${_APU_FILTERED})
    target_sources(xbox_apu PRIVATE "${_RBX_APUVP}")
    set(_RBX_HAVE_APUVP ON)
    message(STATUS "Robotech: linking pinned APU voice processor (${_RBX_APUVP})")
endif()

# Project-pinned APU DSP stage (see the header of patches/apu_dsp_rbx.c):
# folds the mixbins down to stereo by their DirectSound layout instead of
# summing every bin even/odd, which played reverb/FX sends dry and left
# speech quiet against positional effects.
set(_RBX_APUDSP "${CMAKE_CURRENT_SOURCE_DIR}/patches/apu_dsp_rbx.c")
if(EXISTS "${_RBX_APUDSP}" AND TARGET xbox_apu)
    get_target_property(_APU_SOURCES xbox_apu SOURCES)
    set(_APU_FILTERED "")
    foreach(_s IN LISTS _APU_SOURCES)
        get_filename_component(_sn "${_s}" NAME)
        if(NOT _sn STREQUAL "apu_dsp.c")
            list(APPEND _APU_FILTERED "${_s}")
        endif()
    endforeach()
    set_property(TARGET xbox_apu PROPERTY SOURCES ${_APU_FILTERED})
    target_sources(xbox_apu PRIVATE "${_RBX_APUDSP}")
    message(STATUS "Robotech: linking pinned APU DSP stage (${_RBX_APUDSP})")
endif()

file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/rbx_d3d11.c
    src/rbx_settings.c
    src/rbx_menu.c
    src/rbx_ui.c
    src/rbx_keymap.c
    src/rbx_sdl_window.c
)

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
    src/robotech_bc.rc
)
if(_RBX_HAVE_APUVP)
    # rbx_d3d11.c reads the pinned voice processor's lock-wait counters.
    target_compile_definitions(${PROJECT_NAME} PRIVATE RBX_HAVE_APU_VP=1)
endif()

get_filename_component(_RBX_TOOLS_DIR "${XBOXRECOMP_DIR}" DIRECTORY)
set(RBX_SDL3_ROOT "${_RBX_TOOLS_DIR}/SDL3-3.4.20" CACHE PATH "SDL3 Windows development package")
if(NOT EXISTS "${RBX_SDL3_ROOT}/lib/x64/SDL3.lib" OR
   NOT EXISTS "${RBX_SDL3_ROOT}/lib/x64/SDL3.dll")
    message(FATAL_ERROR "Set RBX_SDL3_ROOT to the SDL3 Windows x64 development package")
endif()
get_target_property(_RBX_INPUT_SOURCES xbox_input SOURCES)
list(FILTER _RBX_INPUT_SOURCES EXCLUDE REGEX "(^|[/\\\\])xinput_device\\.c$")
set_property(TARGET xbox_input PROPERTY SOURCES ${_RBX_INPUT_SOURCES})
target_sources(xbox_input PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/patches/rbx_sdl_input.c")
target_include_directories(xbox_input PUBLIC "${RBX_SDL3_ROOT}/include")
target_link_libraries(xbox_input PUBLIC "${RBX_SDL3_ROOT}/lib/x64/SDL3.lib")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RBX_SDL3_ROOT}/lib/x64/SDL3.dll"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>")

# -DRBX_DEBUG_UNLOCK_ALL_LEVELS=ON (build.ps1 -UnlockAllLevels): debug/test
# builds only. Every story mission can be picked without the cheat code or a
# save; nothing is written to the profile. RECOMP_UNLOCK_LEVELS=0 turns it off
# for one run. package_release.py refuses to package such a build.
option(RBX_DEBUG_UNLOCK_ALL_LEVELS "Debug only: unlock every level in mission select" OFF)
if(RBX_DEBUG_UNLOCK_ALL_LEVELS)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RBX_DEBUG_UNLOCK_ALL_LEVELS)
    message(WARNING "Robotech: RBX_DEBUG_UNLOCK_ALL_LEVELS is ON (debug build, do not ship)")
endif()

# -DRBX_ABI_CHECK=ON (use a separate build dir): every lifted direct call checks
# that ebx/esi/edi survive it; src/recomp_manual.c logs offenders as [ABI].
option(RBX_ABI_CHECK "Check callee-saved registers across lifted calls" OFF)
if(RBX_ABI_CHECK)
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_DEFINITIONS RECOMP_ABI_CHECK)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RBX_ABI_CHECK)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
    ${XBOXRECOMP_DIR}/src
    ${XBOXRECOMP_DIR}/src/kernel
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    d3dcompiler
    windowscodecs
    ole32
    dxgi
    dxguid
    winmm
    dbghelp
    gdi32
)

# Config.exe: the settings window outside the game. It edits the same
# settings.ini as the F1 menu and draws with the same src/rbx_ui.c.
add_executable(rbx_config WIN32 src/config/rbx_config.c src/rbx_ui.c src/rbx_menu.c src/rbx_keymap.c
    src/config/rbx_config.rc)
target_include_directories(rbx_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(rbx_config PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
set_target_properties(rbx_config PROPERTIES OUTPUT_NAME "Config")
target_link_libraries(rbx_config PRIVATE user32 gdi32 shell32)
add_dependencies(${PROJECT_NAME} rbx_config)

# rbx_import.exe: Setup's disc importer (installer/robotech.iss runs it from its temp folder, so the C runtime is
# linked in). Built once installer/manifest.h exists (python installer/gen_manifest.py <your disc image>).
# rbx_texture_download.exe: the hi-res texture pack's resumable downloader. Both Setups run it from their temp folder
# (installer/texture_download_code.iss), so the C runtime is linked in.
add_executable(rbx_texture_download installer/texture_download.c)
target_compile_definitions(rbx_texture_download PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(rbx_texture_download PRIVATE winhttp bcrypt shell32)
set_target_properties(rbx_texture_download PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/installer/manifest.h")
    add_executable(rbx_import installer/rbx_import.c)
    target_compile_definitions(rbx_import PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(rbx_import PRIVATE bcrypt shell32)
    set_target_properties(rbx_import PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # The same importer is the game's own --prepare step for RecompLauncher (recomplauncher.json).
    target_sources(${PROJECT_NAME} PRIVATE src/rbx_prepare_import.c)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RBX_HAVE_IMPORT)
    target_link_libraries(${PROJECT_NAME} PRIVATE bcrypt shell32)
endif()

if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE)
    # /FS: parallel CL.EXE in one vcxproj share one PDB (avoids C1041).
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /FS)
endif()
