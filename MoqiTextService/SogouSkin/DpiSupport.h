//
// Per-monitor DPI helpers shared by the Sogou skin renderer/status bar and the
// standalone settings UI.  Everything is resolved dynamically so the binaries
// still load on Windows 7, where the DPI-aware APIs below do not exist.
//
#pragma once

#include <windows.h>

namespace Moqi {
namespace SogouSkin {

// Monitor/effective DPI of a window.  For a DPI-unaware window this returns 96
// (Windows bitmap-scales the window), for a system-aware window the system DPI,
// and for a per-monitor-v2 window the DPI of the monitor it is on.
inline UINT WindowDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (getDpiForWindow && hwnd) {
        const UINT dpi = getDpiForWindow(hwnd);
        if (dpi > 0) {
            return dpi;
        }
    }
    HDC dc = hwnd ? ::GetDC(hwnd) : ::GetDC(nullptr);
    UINT dpi = dc ? static_cast<UINT>(::GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc) {
        ::ReleaseDC(hwnd, dc);
    }
    return dpi > 0 ? dpi : 96;
}

inline UINT SystemDpi() {
    using GetDpiForSystemFn = UINT(WINAPI*)();
    static GetDpiForSystemFn getDpiForSystem = reinterpret_cast<GetDpiForSystemFn>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
    if (getDpiForSystem) {
        const UINT dpi = getDpiForSystem();
        if (dpi > 0) {
            return dpi;
        }
    }
    return WindowDpi(nullptr);
}

// Opt the current process into per-monitor-v2 DPI awareness.  Must run before
// the first window is created.  Returns true when the process was made at
// least per-monitor aware.
inline bool EnablePerMonitorDpiAwareness() {
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
    static SetProcessDpiAwarenessContextFn setContext =
        reinterpret_cast<SetProcessDpiAwarenessContextFn>(::GetProcAddress(
            ::GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4.
    if (setContext && setContext(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) {
        return true;
    }

    HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
    if (shcore) {
        using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);
        auto setAwareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
            ::GetProcAddress(shcore, "SetProcessDpiAwareness"));
        // PROCESS_PER_MONITOR_DPI_AWARE == 2.
        if (setAwareness && SUCCEEDED(setAwareness(2))) {
            ::FreeLibrary(shcore);
            return true;
        }
        ::FreeLibrary(shcore);
    }
    return ::SetProcessDPIAware() != FALSE;
}

// AdjustWindowRectEx() is not DPI aware.  Prefer the per-DPI variant so the
// non-client area is correct at 125/150/175/200%.
inline void AdjustWindowRectForDpi(RECT* rect, DWORD style, BOOL menu,
                                   DWORD exStyle, UINT dpi) {
    using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static AdjustWindowRectExForDpiFn adjustForDpi =
        reinterpret_cast<AdjustWindowRectExForDpiFn>(::GetProcAddress(
            ::GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
    if (adjustForDpi && dpi > 0) {
        if (adjustForDpi(rect, style, menu, exStyle, dpi)) {
            return;
        }
    }
    ::AdjustWindowRectEx(rect, style, menu, exStyle);
}

} // namespace SogouSkin
} // namespace Moqi
