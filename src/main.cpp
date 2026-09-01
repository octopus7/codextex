#include "app/Application.hpp"

#include <Windows.h>
#include <imgui_impl_win32.h>

#include <string>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    ImGui_ImplWin32_EnableDpiAwareness();
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    codextex::Application application;
    std::string error;
    if (!application.Initialize(instance, showCommand, error)) {
        const std::wstring message(error.begin(), error.end());
        MessageBoxW(nullptr, message.c_str(), L"CodexTex startup error", MB_ICONERROR | MB_OK);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 1;
    }
    const int result = application.Run();
    application.Shutdown();
    if (SUCCEEDED(comResult)) CoUninitialize();
    return result;
}
