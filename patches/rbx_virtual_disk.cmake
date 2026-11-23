# Windows raw Xbox partition devices use process memory. Keep the shared
# toolkit unchanged and compile its file, path and bridge code with our adapters.
if(WIN32)
    get_target_property(_kernel_sources xbox_kernel SOURCES)
    foreach(_unit IN ITEMS path file bridge)
        list(FILTER _kernel_sources EXCLUDE REGEX "(^|/)kernel_${_unit}[.]c$")
        set(_wrapper "${CMAKE_CURRENT_BINARY_DIR}/rbx_kernel_${_unit}.c")
        configure_file("patches/rbx_kernel_${_unit}.c.in" "${_wrapper}" @ONLY)
        list(APPEND _kernel_sources "${_wrapper}")
    endforeach()
    set_property(TARGET xbox_kernel PROPERTY SOURCES ${_kernel_sources})
    target_include_directories(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/patches"
        "${XBOXRECOMP_DIR}/src/kernel")
    target_sources(xbox_kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/patches/rbx_virtual_disk.c")
endif()
