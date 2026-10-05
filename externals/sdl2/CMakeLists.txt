# SDL's WinRT backend is C++/CX (/ZW), which MSVC rejects together with /std:c++20, so this
# directory builds as C++17.
if(WINDOWS_STORE)
    set(CMAKE_CXX_STANDARD 17)
endif()

# Configure static library build
set(SDL_SHARED OFF CACHE BOOL "")
set(SDL_STATIC ON CACHE BOOL "")

# Subsystems
set(SDL_ATOMIC ON CACHE BOOL "")
set(SDL_AUDIO ON CACHE BOOL "")
set(SDL_VIDEO ON CACHE BOOL "")
set(SDL_RENDER OFF CACHE BOOL "")
if(WINDOWS_STORE)
    # The WinRT video backend references D3D11_DXGIFormatToSDLPixelFormat, which is only compiled
    # with the D3D11 renderer. FORCE because the cache entry is set just above.
    set(SDL_RENDER ON CACHE BOOL "" FORCE)
endif()
set(SDL_EVENTS ON CACHE BOOL "")
set(SDL_JOYSTICK ON CACHE BOOL "")
set(SDL_HAPTIC OFF CACHE BOOL "")
set(SDL_HIDAPI ON CACHE BOOL "")
set(SDL_POWER OFF CACHE BOOL "")
set(SDL_THREADS ON CACHE BOOL "")
set(SDL_TIMERS ON CACHE BOOL "")
set(SDL_FILE ON CACHE BOOL "")
set(SDL_LOADSO ON CACHE BOOL "")
set(SDL_CPUINFO ON CACHE BOOL "")
set(SDL_FILESYSTEM OFF CACHE BOOL "")
set(SDL_DLOPEN ON CACHE BOOL "")
set(SDL_SENSOR OFF CACHE BOOL "")
set(SDL_LOCALE OFF CACHE BOOL "")

if(WINDOWS_STORE)
    # The WinRT fixes are applied to a build-directory copy so the submodule stays pristine.
    find_package(Git REQUIRED)
    set(SDL_UWP_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/SDL-uwp")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/SDL/" DESTINATION "${SDL_UWP_SOURCE_DIR}"
         PATTERN ".git" EXCLUDE)

    set(SDL_UWP_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/xbox-uwp.patch")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check --ignore-space-change --ignore-whitespace
                "${SDL_UWP_PATCH}"
        WORKING_DIRECTORY "${SDL_UWP_SOURCE_DIR}"
        RESULT_VARIABLE SDL_UWP_PATCH_NEEDED
        OUTPUT_QUIET ERROR_QUIET)
    if(SDL_UWP_PATCH_NEEDED EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply --ignore-space-change --ignore-whitespace
                    "${SDL_UWP_PATCH}"
            WORKING_DIRECTORY "${SDL_UWP_SOURCE_DIR}"
            RESULT_VARIABLE SDL_UWP_PATCH_RESULT)
        if(NOT SDL_UWP_PATCH_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to apply ${SDL_UWP_PATCH}")
        endif()
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --ignore-space-change
                    --ignore-whitespace "${SDL_UWP_PATCH}"
            WORKING_DIRECTORY "${SDL_UWP_SOURCE_DIR}"
            RESULT_VARIABLE SDL_UWP_PATCH_PRESENT
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT SDL_UWP_PATCH_PRESENT EQUAL 0)
            message(FATAL_ERROR "${SDL_UWP_PATCH} does not apply to the pinned SDL revision")
        endif()
    endif()

    add_subdirectory("${SDL_UWP_SOURCE_DIR}" SDL)
else()
    add_subdirectory(SDL)
endif()
