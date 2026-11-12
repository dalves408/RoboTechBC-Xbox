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
    message(STATUS "Robotech: linking pinned APU voice processor (${_RBX_APUVP})")
endif()

file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/rbx_d3d11.c
)

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

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
    $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>
    winmm
    dbghelp
)

if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE)
    # /FS: parallel CL.EXE in one vcxproj share one PDB (avoids C1041).
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /FS)
endif()
