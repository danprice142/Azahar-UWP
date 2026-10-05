# Copyright Citra Emulator Project / Azahar Emulator Project
# Licensed under GPLv2 or any later version
# Refer to the license.txt file included.
#
# UWP warning and Win32-compatibility helpers.

set(UWP_COMPAT_HEADER "${CMAKE_CURRENT_LIST_DIR}/../src/uwp/compat/uwp_compat.h")
get_filename_component(UWP_COMPAT_HEADER "${UWP_COMPAT_HEADER}" ABSOLUTE)

set(_UWP_COMPILABLE_TYPES
    STATIC_LIBRARY SHARED_LIBRARY MODULE_LIBRARY OBJECT_LIBRARY EXECUTABLE)

# Returns TRUE in `out_var` if `target` exists and can carry compile options.
function(_uwp_target_is_compilable target out_var)
    set(${out_var} FALSE PARENT_SCOPE)
    if(NOT TARGET ${target})
        return()
    endif()
    # ALIAS and IMPORTED targets cannot be modified.
    get_target_property(_aliased ${target} ALIASED_TARGET)
    if(_aliased)
        return()
    endif()
    get_target_property(_imported ${target} IMPORTED)
    if(_imported)
        return()
    endif()
    get_target_property(_type ${target} TYPE)
    if(NOT _type IN_LIST _UWP_COMPILABLE_TYPES)
        return()
    endif()
    set(${out_var} TRUE PARENT_SCOPE)
endfunction()

function(uwp_relax_warnings_in_target target)
    _uwp_target_is_compilable(${target} _ok)
    if(NOT _ok)
        return()
    endif()

    # cryptopp and dynarmic build .asm sources, and ml64.exe rejects cl-only switches.
    target_compile_options(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:C,CXX>:/WX->"
        "$<$<COMPILE_LANGUAGE:C,CXX>:/wd4996>"  # declared deprecated (secure-CRT nags)
        "$<$<COMPILE_LANGUAGE:C,CXX>:/wd4146>"  # unary minus applied to unsigned type
    )
    target_compile_definitions(${target} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_DEPRECATE
        _WINSOCK_DEPRECATED_NO_WARNINGS
        # cryptopp only enables its OS RNG for a non-desktop WINAPI_FAMILY when _WIN32_WINNT is at
        # least 0x0A00.
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
    )
    # WINAPI_FAMILY is already WINAPI_FAMILY_APP under the WindowsStore toolset.
endfunction()

# Recursively relax warnings for every target defined at or below `dir`.
function(uwp_relax_warnings_in_directory dir)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        uwp_relax_warnings_in_target(${_t})
    endforeach()

    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        uwp_relax_warnings_in_directory("${_sub}")
    endforeach()
endfunction()

# Force-include the Win32 shim into a whole target.
function(uwp_apply_compat_to_target target)
    _uwp_target_is_compilable(${target} _ok)
    if(NOT _ok)
        message(STATUS "UWP compat: skipping '${target}' (not a compilable target)")
        return()
    endif()
    target_compile_options(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:C,CXX>:/FI${UWP_COMPAT_HEADER}>"
    )
endfunction()

# Force-include the Win32 shim into individual source files. `target` is required because
# SOURCE properties are scoped to the directory that sets them.
function(uwp_apply_compat_to_sources target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "UWP compat: no such target '${target}'")
    endif()
    foreach(_src IN LISTS ARGN)
        get_filename_component(_abs "${_src}" ABSOLUTE)
        if(NOT EXISTS "${_abs}")
            message(FATAL_ERROR "UWP compat: no such source file '${_abs}'")
        endif()
        set_property(SOURCE "${_abs}" TARGET_DIRECTORY ${target}
            APPEND PROPERTY COMPILE_OPTIONS "/FI${UWP_COMPAT_HEADER}")
    endforeach()
endfunction()

# Force-include a system header into a target (used to give cryptopp <winapifamily.h>).
function(uwp_force_include_header target header)
    _uwp_target_is_compilable(${target} _ok)
    if(NOT _ok)
        message(STATUS "UWP compat: skipping '${target}' (not a compilable target)")
        return()
    endif()
    target_compile_options(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:C,CXX>:/FI${header}>"
    )
endfunction()
