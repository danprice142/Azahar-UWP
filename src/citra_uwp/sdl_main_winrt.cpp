// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// WinRT entry point (SDL2main does not build the WinRT variant). Compiled with /ZW so the
// build emits the Windows Metadata file the package needs; see CMakeLists.txt.

#include <SDL_main.h>
#include <wrl.h>

#ifndef __cplusplus_winrt
#error This file must be compiled with /ZW; without it the app has no .winmd and will not activate.
#endif

#ifdef _MSC_VER
// SDL initialises the apartment itself; suppress the threading-model warning for WinMain.
#pragma warning(disable : 4447)
// Ensure the Windows Runtime initialisation entry point gets linked in.
#pragma comment(lib, "runtimeobject.lib")
#endif

int CALLBACK WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return SDL_WinRTRunApp(SDL_main, NULL);
}
