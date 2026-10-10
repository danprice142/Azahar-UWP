# Copyright Citra Emulator Project / Azahar Emulator Project
# Licensed under GPLv2 or any later version
# Refer to the license.txt file included.

add_executable(citra_uwp WIN32
    citra_uwp.cpp
    remote_library_server.cpp
    remote_library_server.h
    emulation_thread.cpp
    emulation_thread.h
    emu_window_uwp.cpp
    emu_window_uwp.h
    keyboard_input.cpp
    keyboard_input.h
    live_camera.cpp
    live_camera.h
    sdl_main_winrt.cpp
    still_image_camera.cpp
    still_image_camera.h
    storage_paths.cpp
    storage_paths.h
    screenscraper.cpp
    screenscraper.h
    uwp_config.cpp
    uwp_config.h
    uwp_gl.cpp
    uwp_gl.h
    uwp_settings.cpp
    uwp_settings.h
    ui/configure_dialog.cpp
    ui/configure_dialog.h
    ui/dialogs.cpp
    ui/dialogs.h
    ui/file_browser.cpp
    ui/file_browser.h
    ui/game_list.cpp
    ui/game_list.h
    ui/imgui_manager.cpp
    ui/imgui_manager.h
    ui/main_menu.cpp
    ui/main_menu.h
    ui/mii_selector.cpp
    ui/mii_selector.h
    ui/multiplayer.cpp
    ui/multiplayer.h
    ui/popups.cpp
    ui/popups.h
    ui/software_keyboard.cpp
    ui/software_keyboard.h
    ui/virtual_keyboard.cpp
    ui/virtual_keyboard.h
    ui/widgets.cpp
    ui/widgets.h
)

set(AZAHAR_SCREENSCRAPER_DEV_ID "" CACHE STRING
    "ScreenScraper developer ID embedded in the UWP package")
set(AZAHAR_SCREENSCRAPER_DEV_PASSWORD "" CACHE STRING
    "ScreenScraper developer password embedded in the UWP package")
mark_as_advanced(AZAHAR_SCREENSCRAPER_DEV_ID AZAHAR_SCREENSCRAPER_DEV_PASSWORD)
include("${CMAKE_CURRENT_SOURCE_DIR}/screenscraper_credentials.cmake" OPTIONAL)
if(NOT AZAHAR_SCREENSCRAPER_DEV_ID OR NOT AZAHAR_SCREENSCRAPER_DEV_PASSWORD)
    message(WARNING "citra_uwp: ScreenScraper support will be disabled because developer "
                    "credentials were not supplied at package-build time")
endif()
configure_file(screenscraper_credentials.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/screenscraper_credentials.h" @ONLY)
target_include_directories(citra_uwp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

create_target_directory_groups(citra_uwp)

# The WinRT entry point must be C++/CX (/ZW) so the build emits the Windows Metadata file.
# MSVC rejects /ZW with /std:c++20 and /permissive-, so this unit uses C++17 and /permissive.
set_source_files_properties(sdl_main_winrt.cpp PROPERTIES
    COMPILE_OPTIONS "/ZW;/std:c++17;/EHsc;/permissive"
    SKIP_PRECOMPILE_HEADERS ON
)

target_link_libraries(citra_uwp PRIVATE
    citra_common
    citra_core
    cryptopp
    video_core
    audio_core
    network
    input_common
    glad
    imgui
    inih
    json-headers
    lodepng
)

target_link_libraries(citra_uwp PRIVATE WindowsApp.lib OneCore.lib)
target_link_libraries(citra_uwp PRIVATE mfplat.lib mfreadwrite.lib mfuuid.lib propsys.lib
                                        windowscodecs.lib)

target_link_libraries(citra_uwp PRIVATE SDL2-static)

if (ENABLE_WEB_SERVICE)
    target_link_libraries(citra_uwp PRIVATE web_service)
endif()

# mesa-uwp provides OpenGL through its d3d12 driver; the prebuilt archive is extracted here.
set(MESA_UWP_ARCHIVE "${PROJECT_SOURCE_DIR}/externals/uwp/mesa-uwp-26.1.3.zip")
set(MESA_UWP_EXTRACT_DIR "${CMAKE_BINARY_DIR}/externals/uwp/mesa")
set(MESA_UWP_ROOT "${MESA_UWP_EXTRACT_DIR}/26.1.3")
set(MESA_UWP_ARCHIVE_STAMP "${MESA_UWP_EXTRACT_DIR}/archive.sha256")
if(NOT EXISTS "${MESA_UWP_ARCHIVE}")
    message(FATAL_ERROR "citra_uwp: missing mesa archive '${MESA_UWP_ARCHIVE}'")
endif()
file(SHA256 "${MESA_UWP_ARCHIVE}" MESA_UWP_ARCHIVE_SHA256)
set(MESA_UWP_EXTRACT_REQUIRED TRUE)
if(EXISTS "${MESA_UWP_ARCHIVE_STAMP}")
    file(READ "${MESA_UWP_ARCHIVE_STAMP}" MESA_UWP_EXTRACTED_SHA256)
    string(STRIP "${MESA_UWP_EXTRACTED_SHA256}" MESA_UWP_EXTRACTED_SHA256)
    if(MESA_UWP_EXTRACTED_SHA256 STREQUAL MESA_UWP_ARCHIVE_SHA256 AND
       EXISTS "${MESA_UWP_ROOT}/bin/libgallium_wgl.dll" AND
       EXISTS "${MESA_UWP_ROOT}/lib/libgallium_wgl.lib")
        set(MESA_UWP_EXTRACT_REQUIRED FALSE)
    endif()
endif()
if(MESA_UWP_EXTRACT_REQUIRED)
    file(MAKE_DIRECTORY "${MESA_UWP_EXTRACT_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${MESA_UWP_ARCHIVE}" DESTINATION "${MESA_UWP_EXTRACT_DIR}")
    file(WRITE "${MESA_UWP_ARCHIVE_STAMP}" "${MESA_UWP_ARCHIVE_SHA256}\n")
endif()

# The windowing entry points are exported by libgallium_wgl itself, so both libraries are needed.
target_link_libraries(citra_uwp PRIVATE
    "${MESA_UWP_ROOT}/lib/opengl32.lib"
    "${MESA_UWP_ROOT}/lib/libgallium_wgl.lib"
)

# FFmpeg is loaded at runtime by common/dynamic_library/ffmpeg.cpp; no import libraries.
set(FFMPEG_UWP_ROOT "${PROJECT_SOURCE_DIR}/externals/uwp/ffmpeg/6.0")

# z-1.dll is mesa's zlib. dxil.dll is loaded by name by mesa's d3d12 driver to sign shaders and
# comes from the Windows SDK's D3D redistributable; the app CRT ships in Microsoft.VCLibs.
find_file(CITRA_UWP_DXIL_DLL
    NAMES dxil.dll
    PATHS
        "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/Redist/D3D/x64"
        "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    NO_DEFAULT_PATH
)
if(NOT CITRA_UWP_DXIL_DLL)
    message(FATAL_ERROR "citra_uwp: could not locate dxil.dll in the Windows SDK; mesa's d3d12 "
                        "driver needs it to sign shaders at runtime.")
endif()

set(CITRA_UWP_RUNTIME_DLLS
    "${MESA_UWP_ROOT}/bin/opengl32.dll"
    "${MESA_UWP_ROOT}/bin/libgallium_wgl.dll"
    "${MESA_UWP_ROOT}/bin/z-1.dll"
    "${CITRA_UWP_DXIL_DLL}"
    "${FFMPEG_UWP_ROOT}/bin/avcodec-60.dll"
    "${FFMPEG_UWP_ROOT}/bin/avfilter-9.dll"
    "${FFMPEG_UWP_ROOT}/bin/avformat-60.dll"
    "${FFMPEG_UWP_ROOT}/bin/avutil-58.dll"
    "${FFMPEG_UWP_ROOT}/bin/swresample-4.dll"
    "${FFMPEG_UWP_ROOT}/bin/swscale-7.dll"
)
foreach(_dll IN LISTS CITRA_UWP_RUNTIME_DLLS)
    if(NOT EXISTS "${_dll}")
        message(FATAL_ERROR "citra_uwp: missing required runtime DLL '${_dll}'")
    endif()
endforeach()

set(CITRA_UWP_COMPATIBILITY_LIST
    "${PROJECT_SOURCE_DIR}/dist/compatibility_list/compatibility_list.json")
set(CITRA_UWP_FFMPEG_NOTICES
    "${FFMPEG_UWP_ROOT}/LICENSE.txt"
    "${FFMPEG_UWP_ROOT}/BUILD_INFO.txt"
)
set(CITRA_UWP_MESA_NOTICES
    "${PROJECT_SOURCE_DIR}/externals/uwp/mesa/BUILD_INFO.txt"
    "${PROJECT_SOURCE_DIR}/externals/uwp/mesa/LICENSE.rst"
    "${PROJECT_SOURCE_DIR}/externals/uwp/mesa/ZLIB-LICENSE.txt"
    "${PROJECT_SOURCE_DIR}/externals/uwp/mesa/licenses/Apache-2.0"
    "${PROJECT_SOURCE_DIR}/externals/uwp/mesa/licenses/BSL-1.0"
    "${PROJECT_SOURCE_DIR}/externals/uwp/mesa/licenses/GPL-1.0-or-later"
    "${PROJECT_SOURCE_DIR}/externals/uwp/mesa/licenses/GPL-2.0-only"
    "${PROJECT_SOURCE_DIR}/externals/uwp/mesa/licenses/MIT"
    "${PROJECT_SOURCE_DIR}/externals/uwp/mesa/licenses/SGI-B-2.0"
    "${PROJECT_SOURCE_DIR}/externals/uwp/mesa/licenses/exceptions/Linux-Syscall-Note"
)
target_sources(citra_uwp PRIVATE
    ${CITRA_UWP_RUNTIME_DLLS}
    "${CITRA_UWP_COMPATIBILITY_LIST}"
    ${CITRA_UWP_FFMPEG_NOTICES}
    ${CITRA_UWP_MESA_NOTICES}
)
set_source_files_properties(
    ${CITRA_UWP_RUNTIME_DLLS}
    "${CITRA_UWP_COMPATIBILITY_LIST}"
    PROPERTIES
        VS_DEPLOYMENT_CONTENT 1
        VS_DEPLOYMENT_LOCATION "."
)
set_source_files_properties(
    ${CITRA_UWP_FFMPEG_NOTICES}
    PROPERTIES
        VS_DEPLOYMENT_CONTENT 1
        VS_DEPLOYMENT_LOCATION "ThirdPartyNotices"
)
set_source_files_properties(
    ${CITRA_UWP_MESA_NOTICES}
    PROPERTIES
        VS_DEPLOYMENT_CONTENT 1
        VS_DEPLOYMENT_LOCATION "ThirdPartyNotices/Mesa"
)

add_custom_command(TARGET citra_uwp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CITRA_UWP_RUNTIME_DLLS} "$<TARGET_FILE_DIR:citra_uwp>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CITRA_UWP_COMPATIBILITY_LIST}"
        "$<TARGET_FILE_DIR:citra_uwp>/compatibility_list.json"
    COMMENT "Staging UWP runtime DLLs and compatibility metadata next to citra_uwp"
    VERBATIM
)

set_target_properties(citra_uwp PROPERTIES
    VS_GLOBAL_ROOTNAMESPACE citra_uwp
    # Certification is not required for a devkit sideload and the check is slow.
    VS_GLOBAL_AppxPackageSigningEnabled true
    VS_GLOBAL_AppxBundle Never
    VS_GLOBAL_AppxPackageOutput "${CMAKE_BINARY_DIR}/AppPackages/citra_uwp.msix"
)

# The Visual Studio generator writes % as %%, so the MSVC /PDBALTPATH:%_PDB% flag embeds a literal
# %citra_uwp.pdb% and debuggers never find the PDB. The last /PDBALTPATH on the command line wins.
target_link_options(citra_uwp PRIVATE "$<$<CONFIG:Release>:/PDBALTPATH:citra_uwp.pdb>")

if (CITRA_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(citra_uwp PRIVATE precompiled_headers.h)
endif()
