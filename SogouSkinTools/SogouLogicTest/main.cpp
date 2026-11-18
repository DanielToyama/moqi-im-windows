// Headless logic tests for the modules that do not need a live TSF context:
//   * KeyRepeatAccelerator state machine (press/delay/burst/release/focus)
//   * Sogou symbol table + menu command id mapping
//   * search-host process exclusion
//   * status-bar foreground ownership / single-window election
//   * u-mode / radical-lookup preedit formatting
//
// Build with the regular CMake configuration and run from a console; it
// creates no visible windows and returns non-zero on failure.

#include "KeyRepeatAccelerator.h"
#include "SogouSymbols.h"
#include "SogouSkin/SogouSkin.h"
#include "CompositionDisplay.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (condition) {
        std::printf("PASS %s\n", message);
    } else {
        std::printf("FAIL %s\n", message);
        ++g_failures;
    }
}

void Skip(const char* message) {
    std::printf("SKIP %s\n", message);
}

LRESULT CALLBACK TestWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

HWND CreateTestWindow(const wchar_t* title, HWND parent, DWORD style,
                      DWORD exStyle = 0) {
    static const wchar_t kClass[] = L"MoqiStatusBarLogicTestWindow";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &TestWindowProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        ::RegisterClassExW(&wc);
        registered = true;
    }
    return ::CreateWindowExW(exStyle, kClass, title, style, 0, 0, 120, 80,
                             parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

BOOL CALLBACK CollectMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto* monitors = reinterpret_cast<std::vector<HMONITOR>*>(param);
    if (monitors && monitor) {
        monitors->push_back(monitor);
    }
    return TRUE;
}

std::vector<HMONITOR> EnumerateMonitors() {
    std::vector<HMONITOR> monitors;
    ::EnumDisplayMonitors(nullptr, nullptr, &CollectMonitorProc,
                          reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

bool WriteSolidBmp(const std::wstring& path, int width, int height) {
    const int rowBytes = ((width * 3 + 3) / 4) * 4;
    const DWORD pixelBytes = static_cast<DWORD>(rowBytes * height);
    BITMAPFILEHEADER fileHeader = {};
    fileHeader.bfType = 0x4D42; // 'BM'
    fileHeader.bfOffBits =
        sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + pixelBytes;
    BITMAPINFOHEADER infoHeader = {};
    infoHeader.biSize = sizeof(infoHeader);
    infoHeader.biWidth = width;
    infoHeader.biHeight = height;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 24;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = pixelBytes;

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    file.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));
    std::vector<char> row(static_cast<size_t>(rowBytes), static_cast<char>(0x80));
    for (int y = 0; y < height; ++y) {
        file.write(row.data(), static_cast<std::streamsize>(row.size()));
    }
    return file.good();
}

std::wstring CreateTestSkinDirectoryWithIni(const std::string& iniText) {
    wchar_t tempPath[MAX_PATH] = {};
    if (::GetTempPathW(_countof(tempPath), tempPath) == 0) {
        return L"";
    }
    std::wstring dir = tempPath;
    dir += L"MoqiStatusBarTest_";
    dir += std::to_wstring(::GetCurrentProcessId());
    dir += L"_";
    static unsigned long sequence = 0;
    dir += std::to_wstring(::GetTickCount64());
    dir += L"_";
    dir += std::to_wstring(++sequence);
    if (!::CreateDirectoryW(dir.c_str(), nullptr)) {
        return L"";
    }
    if (!WriteSolidBmp(dir + L"\\scheme.bmp", 40, 24) ||
        !WriteSolidBmp(dir + L"\\bar.bmp", 40, 24)) {
        return L"";
    }
    std::ofstream ini(dir + L"\\skin.ini", std::ios::binary | std::ios::trunc);
    ini << iniText;
    if (!ini.good()) {
        return L"";
    }
    return dir;
}

std::wstring CreateTestSkinDirectory() {
    return CreateTestSkinDirectoryWithIni(
        "[Display]\n"
        "font_size=16\n"
        "[Scheme_H1]\n"
        "pic=scheme.bmp\n"
        "[StatusBar]\n"
        "pic=bar.bmp\n"
        "menu_display=1\n"
        "menu_pos=0,0\n"
        "menu=bar.bmp\n");
}

void RemoveTestSkinDirectory(const std::wstring& dir) {
    if (dir.empty()) {
        return;
    }
    ::DeleteFileW((dir + L"\\skin.ini").c_str());
    ::DeleteFileW((dir + L"\\scheme.bmp").c_str());
    ::DeleteFileW((dir + L"\\bar.bmp").c_str());
    ::RemoveDirectoryW(dir.c_str());
}

std::wstring GetEnvironmentString(const wchar_t* name) {
    wchar_t buffer[32768] = {};
    const DWORD length = ::GetEnvironmentVariableW(name, buffer,
                                                   _countof(buffer));
    return length > 0 && length < _countof(buffer) ? std::wstring(buffer)
                                                   : std::wstring();
}

// Creates an isolated %APPDATA% root with an empty \Moqi directory.  Callers
// must restore the previous APPDATA value and remove the directory.
std::wstring CreateTempAppDataRoot() {
    wchar_t tempPath[MAX_PATH] = {};
    if (::GetTempPathW(_countof(tempPath), tempPath) == 0) {
        return L"";
    }
    std::wstring root = tempPath;
    root += L"MoqiOptionsTest_";
    root += std::to_wstring(::GetCurrentProcessId());
    root += L"_";
    root += std::to_wstring(::GetTickCount64());
    // GetTickCount64 has coarse resolution; tests can create/remove several
    // isolated APPDATA roots within one tick on fast CI runners.  A static
    // sequence keeps the directory name unique in that case.
    static unsigned long sequence = 0;
    root += L"_";
    root += std::to_wstring(++sequence);
    if (!::CreateDirectoryW(root.c_str(), nullptr)) {
        return L"";
    }
    if (!::CreateDirectoryW((root + L"\\Moqi").c_str(), nullptr)) {
        ::RemoveDirectoryW(root.c_str());
        return L"";
    }
    return root;
}

void RemoveTempAppDataRoot(const std::wstring& root) {
    if (root.empty()) {
        return;
    }
    ::DeleteFileW((root + L"\\Moqi\\sogou_skin_options.ini").c_str());
    ::DeleteFileW((root + L"\\Moqi\\sogou_skin_overrides.ini").c_str());
    ::DeleteFileW((root + L"\\Moqi\\sogou_skin_layouts.ini").c_str());
    ::DeleteFileW((root + L"\\Moqi\\sogou_skin_path.txt").c_str());
    ::RemoveDirectoryW((root + L"\\Moqi").c_str());
    ::RemoveDirectoryW(root.c_str());
}

bool WriteOptionsFile(const std::wstring& root, const std::string& text) {
    std::ofstream file(root + L"\\Moqi\\sogou_skin_options.ini",
                       std::ios::binary | std::ios::trunc);
    file << text;
    return file.good();
}

bool WriteUtf8File(const std::wstring& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    return file.good();
}

void TestKeyRepeatStateMachine() {
    std::vector<WORD> emitted;
    Moqi::KeyRepeatAccelerator accelerator(
        [&](WORD vk) { emitted.push_back(vk); }, false);
    accelerator.configure(true, 420, 25, 60, 6000);

    // First press: processed normally, accelerator armed.
    Check(!accelerator.onKeyDown('A', 1, false, false, 1000),
          "first key down is not consumed");
    Check(accelerator.active(), "accelerator armed after first key down");
    Check(emitted.empty(), "no repeat before initial delay");

    accelerator.tick(1300);
    Check(emitted.empty(), "still no repeat inside 420ms delay");
    accelerator.tick(1421);
    Check(emitted.size() == 1 && emitted[0] == 'A',
          "first burst repeat at initial delay");
    accelerator.tick(1447);
    Check(emitted.size() == 2, "second burst repeat after interval");

    // OS auto-repeat while armed must be consumed and must not restart.
    Check(accelerator.onKeyDown('A', 3, false, false, 1450),
          "OS auto-repeat is consumed while accelerator is active");
    Check(accelerator.active(), "auto-repeat does not disarm accelerator");
    accelerator.tick(1472);
    Check(emitted.size() == 3, "burst continues after OS auto-repeat");

    // Physical release stops immediately.
    accelerator.onKeyUp('A');
    Check(!accelerator.active(), "key up stops accelerator");
    accelerator.tick(5000);
    Check(emitted.size() == 3, "no emits after key up");

    // Ctrl/Alt/Win must not arm.
    Check(!accelerator.onKeyDown('B', 1, true, false, 6000),
          "modified key is not consumed");
    Check(!accelerator.active(), "Ctrl/Alt/Win blocks acceleration");

    // Digits are blocked while a candidate list is visible.
    Check(!accelerator.onKeyDown('1', 1, false, true, 7000),
          "candidate selection digit is not accelerated");
    Check(!accelerator.active(), "digit does not arm with candidates visible");

    // Backspace is always eligible.
    Check(!accelerator.onKeyDown(VK_BACK, 1, false, true, 8000),
          "backspace first press is not consumed");
    Check(accelerator.active(), "backspace arms");
    accelerator.onFocusLost();
    Check(!accelerator.active(), "focus loss stops accelerator");

    // Max repeat cap.
    emitted.clear();
    accelerator.onKeyDown('C', 1, false, false, 10000);
    accelerator.tick(10420);
    for (int i = 0; i < 100; ++i) {
        accelerator.tick(10420 + 25ULL * static_cast<ULONGLONG>(i + 1));
    }
    Check(emitted.size() == 60, "repeat count is capped at maxRepeats");
    Check(!accelerator.active(), "accelerator disarms at maxRepeats");

    // Disabled option never arms.
    accelerator.configure(false);
    Check(!accelerator.onKeyDown('D', 1, false, false, 20000),
          "disabled accelerator does not consume");
    Check(!accelerator.active(), "disabled accelerator does not arm");
}

void TestSymbolMenu() {
    const auto& categories = Moqi::SogouSymbolCategories();
    size_t total = 0;
    for (const auto& category : categories) {
        total += category.symbols.size();
        Check(!category.name.empty() && !category.symbols.empty(),
              "symbol category is non-empty");
    }
    Check(total == 19 + 10 + 9 + 13, "symbol table has expected item count");

    HMENU menu = Moqi::CreateSogouSymbolMenu();
    Check(menu != nullptr, "symbol popup created");
    if (menu) {
        Check(GetMenuItemCount(menu) == static_cast<int>(categories.size()),
              "symbol popup has one submenu per category");
        for (int i = 0; i < GetMenuItemCount(menu); ++i) {
            HMENU submenu = GetSubMenu(menu, i);
            Check(submenu != nullptr && GetMenuItemCount(submenu) ==
                                            static_cast<int>(categories[i].symbols.size()),
                  "symbol submenu item count matches table");
        }
        DestroyMenu(menu);
    }

    const std::wstring* first = Moqi::SogouSymbolForCommand(
        Moqi::kSogouSymbolCommandBase);
    Check(first && *first == L"，", "first symbol id maps to fullwidth comma");
    const std::wstring* last = Moqi::SogouSymbolForCommand(
        Moqi::kSogouSymbolCommandBase + static_cast<UINT>(total) - 1);
    Check(last && *last == L"□", "last symbol id maps to hollow square");
    Check(Moqi::SogouSymbolForCommand(1) == nullptr,
          "legacy menu id is not a symbol");
    Check(Moqi::SogouSymbolForCommand(Moqi::kSogouSymbolCommandBase +
                                      static_cast<UINT>(total)) == nullptr,
          "id past the symbol table is rejected");
}

void TestDefaultFonts() {
    Moqi::SogouSkin::Display display;
    Check(display.fontCh == L"微软雅黑",
          "default Sogou display font is Microsoft YaHei");
    Check(display.fontEn == L"Arial", "default English display font is Arial");
}

void TestKeyRepeatTimer() {
    std::vector<WORD> emitted;
    Moqi::KeyRepeatAccelerator accelerator(
        [&](WORD vk) { emitted.push_back(vk); }, true);
    accelerator.configure(true, 100, 20, 10, 2000);
    accelerator.onKeyDown('T', 1, false, false, ::GetTickCount64());

    const DWORD deadline = ::GetTickCount() + 600;
    while (::GetTickCount() < deadline) {
        MSG msg = {};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::Sleep(10);
    }
    Check(emitted.size() >= 2, "timer-driven accelerator emits repeats");
    accelerator.onKeyUp('T');
    const size_t countAtRelease = emitted.size();
    ::Sleep(100);
    MSG msg = {};
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    Check(emitted.size() == countAtRelease, "timer stops after key release");
}

void TestForegroundOwnership() {
    HWND parent = CreateTestWindow(L"Moqi status bar test parent", nullptr,
                                   WS_OVERLAPPEDWINDOW);
    HWND child = CreateTestWindow(L"Moqi status bar test child", parent,
                                  WS_CHILD);
    Check(parent != nullptr && child != nullptr, "test windows created");
    if (!parent || !child) {
        if (child) {
            ::DestroyWindow(child);
        }
        if (parent) {
            ::DestroyWindow(parent);
        }
        return;
    }

    const DWORD selfPid = ::GetCurrentProcessId();
    Check(Moqi::SogouSkin::WindowBelongsToProcess(parent, selfPid),
          "own top-level window belongs to this process");
    Check(Moqi::SogouSkin::WindowBelongsToProcess(child, selfPid),
          "own child window belongs to this process");
    Check(!Moqi::SogouSkin::WindowBelongsToProcess(parent, selfPid + 1),
          "own window does not belong to another process");
    Check(!Moqi::SogouSkin::WindowBelongsToProcess(nullptr, selfPid),
          "null window is never owned");
    Check(!Moqi::SogouSkin::WindowBelongsToProcess(parent, 0),
          "process id 0 never owns a window");
    Check(!Moqi::SogouSkin::IsForegroundProcess(0),
          "process id 0 is never the foreground process");

    const HWND shell = ::GetShellWindow();
    DWORD shellPid = 0;
    if (shell) {
        ::GetWindowThreadProcessId(shell, &shellPid);
    }
    if (shell && shellPid != 0 && shellPid != selfPid) {
        Check(Moqi::SogouSkin::WindowBelongsToProcess(shell, shellPid),
              "foreign shell window belongs to its own process");
        Check(!Moqi::SogouSkin::WindowBelongsToProcess(shell, selfPid),
              "foreign shell window does not belong to this process");
    } else {
        Skip("no distinct shell window for cross-process ownership check");
    }

    const HWND foreground = ::GetForegroundWindow();
    Check(Moqi::SogouSkin::IsForegroundProcess(selfPid) ==
              Moqi::SogouSkin::WindowBelongsToProcess(foreground, selfPid),
          "IsForegroundProcess agrees with direct ownership check");

    ::DestroyWindow(child);
    ::DestroyWindow(parent);
}

void TestStatusBarForegroundPolicy() {
    HWND ownWindow = CreateTestWindow(L"Moqi status bar policy owner", nullptr,
                                      WS_OVERLAPPEDWINDOW);
    HWND foreignWindow = ::GetShellWindow();
    DWORD foreignPid = ::GetCurrentProcessId() + 1;
    if (foreignWindow) {
        ::GetWindowThreadProcessId(foreignWindow, &foreignPid);
        if (foreignPid == ::GetCurrentProcessId()) {
            foreignWindow = nullptr;
            foreignPid = ::GetCurrentProcessId() + 1;
        }
    }
    Check(ownWindow != nullptr, "policy test window created");
    if (!ownWindow) {
        return;
    }

    using Policy = Moqi::SogouSkin::StatusBarForegroundPolicy;
    using Action = Policy::Action;

    Policy visible(::GetCurrentProcessId(), 1000);
    Check(visible.requestShow(ownWindow, 1000) == Action::Show,
          "show request for own foreground shows immediately");
    Check(!visible.pending(), "immediate show is not pending");
    Check(visible.evaluate(ownWindow, true, 1100) == Action::None,
          "visible bar stays visible while foreground is ours");
    Check(visible.evaluate(foreignWindow, true, 1200) == Action::Hide,
          "visible bar hides when foreground belongs elsewhere");
    Check(!visible.showRequested(), "foreground loss clears show request");

    Policy race(::GetCurrentProcessId(), 1000);
    Check(race.requestShow(foreignWindow, 1000) == Action::None,
          "show request during foreground transition stays hidden");
    Check(race.pending(), "foreground transition arms pending grace");
    Check(race.evaluate(ownWindow, false, 1200) == Action::Show,
          "pending show completes once foreground becomes ours");
    Check(!race.pending(), "pending grace ends after show");

    Policy expired(::GetCurrentProcessId(), 1000);
    expired.requestShow(foreignWindow, 1000);
    Check(expired.evaluate(foreignWindow, false, 1500) == Action::None,
          "pending show waits inside grace period");
    Check(expired.evaluate(foreignWindow, false, 2001) == Action::StopPolling,
          "pending show expires and stops timer");
    Check(!expired.showRequested(), "expired show request is cleared");

    Policy idle(::GetCurrentProcessId(), 1000);
    Check(idle.evaluate(ownWindow, false, 0) == Action::StopPolling,
          "hidden bar without request stops polling");
    Check(idle.evaluate(ownWindow, true, 0) == Action::None,
          "visible bar with owned foreground needs no action");

    // A bar that was shown successfully and then hidden by another status
    // bar in the same process must not auto-show and fight it every tick.
    Policy externallyHidden(::GetCurrentProcessId(), 1000);
    Check(externallyHidden.requestShow(ownWindow, 0) == Action::Show,
          "initial show succeeds");
    Check(externallyHidden.evaluate(ownWindow, false, 200) ==
              Action::StopPolling,
          "externally hidden shown bar stops instead of ping-ponging");

    ::DestroyWindow(ownWindow);
}

void TestStatusBarFullscreenOptions() {
    const std::wstring oldAppData = GetEnvironmentString(L"APPDATA");
    const std::wstring oldEnv =
        GetEnvironmentString(L"MOQI_SOGOU_STATUSBAR_FULLSCREEN");
    const std::wstring root = CreateTempAppDataRoot();
    Check(!root.empty(), "isolated APPDATA for fullscreen options");
    if (root.empty()) {
        return;
    }
    _wputenv_s(L"APPDATA", root.c_str());

    const auto defaults = Moqi::SogouSkin::LoadOptions();
    Check(defaults.statusbarFullscreenHide,
          "fullscreen hide defaults to enabled");
    Check(defaults.statusbarFullscreenScope ==
              Moqi::SogouSkin::FullscreenScope::Monitor,
          "fullscreen scope defaults to monitor");

    Check(WriteOptionsFile(
              root,
              "[Options]\nstatusbar_fullscreen_hide=0\n"
              "statusbar_fullscreen_scope=all\n"),
          "fullscreen options file written");
    const auto configured = Moqi::SogouSkin::LoadOptions();
    Check(!configured.statusbarFullscreenHide,
          "statusbar_fullscreen_hide=0 disables hiding");
    Check(configured.statusbarFullscreenScope ==
              Moqi::SogouSkin::FullscreenScope::All,
          "statusbar_fullscreen_scope=all parsed");

    Check(WriteOptionsFile(
              root,
              "[Options]\nstatusbar_fullscreen_hide=off\n"
              "statusbar_fullscreen_scope=unknown\n"),
          "false/unknown fullscreen options written");
    const auto falseAndUnknown = Moqi::SogouSkin::LoadOptions();
    Check(!falseAndUnknown.statusbarFullscreenHide,
          "statusbar_fullscreen_hide=off disables hiding");
    Check(falseAndUnknown.statusbarFullscreenScope ==
              Moqi::SogouSkin::FullscreenScope::Monitor,
          "unknown fullscreen scope falls back to monitor");

    _wputenv_s(L"MOQI_SOGOU_STATUSBAR_FULLSCREEN", L"1");
    const auto envEnabled = Moqi::SogouSkin::LoadOptions();
    Check(envEnabled.statusbarFullscreenHide,
          "environment override re-enables fullscreen hiding");
    _wputenv_s(L"MOQI_SOGOU_STATUSBAR_FULLSCREEN", L"0");
    const auto envDisabled = Moqi::SogouSkin::LoadOptions();
    Check(!envDisabled.statusbarFullscreenHide,
          "environment override disables fullscreen hiding");

    _wputenv_s(L"MOQI_SOGOU_STATUSBAR_FULLSCREEN",
               oldEnv.empty() ? L"" : oldEnv.c_str());
    _wputenv_s(L"APPDATA",
               oldAppData.empty() ? L"" : oldAppData.c_str());
    RemoveTempAppDataRoot(root);
}

Moqi::SogouSkin::FullscreenMetrics MakeFullscreenMetrics() {
    Moqi::SogouSkin::FullscreenMetrics metrics;
    metrics.visible = true;
    metrics.frameValid = true;
    metrics.monitorValid = true;
    metrics.monitor = RECT{0, 0, 1920, 1080};
    metrics.work = RECT{0, 0, 1920, 1032};
    metrics.frame = metrics.monitor;
    return metrics;
}

void TestStatusBarFullscreenClassifier() {
    using Moqi::SogouSkin::FullscreenMetrics;
    using Moqi::SogouSkin::IsFullscreenWindowForMetrics;

    Check(Moqi::SogouSkin::CoversMonitorRect(
              RECT{0, 0, 1920, 1080}, RECT{0, 0, 1920, 1080}, 2),
          "exact monitor coverage is fullscreen geometry");
    Check(Moqi::SogouSkin::CoversMonitorRect(
              RECT{-2, -2, 1922, 1082}, RECT{0, 0, 1920, 1080}, 2),
          "small overhang still covers the monitor");
    Check(!Moqi::SogouSkin::CoversMonitorRect(
              RECT{0, 0, 1920, 1032}, RECT{0, 0, 1920, 1080}, 2),
          "work area does not cover the monitor");

    FullscreenMetrics normal = MakeFullscreenMetrics();
    normal.frame = RECT{100, 100, 1280, 820};
    Check(!IsFullscreenWindowForMetrics(normal),
          "ordinary window is not fullscreen");

    FullscreenMetrics maximized = MakeFullscreenMetrics();
    maximized.frame = maximized.work;
    maximized.maximized = true;
    maximized.hasCaption = true;
    maximized.hasThickFrame = true;
    Check(!IsFullscreenWindowForMetrics(maximized),
          "maximized framed window is not fullscreen");

    FullscreenMetrics maximizedAutoHide = MakeFullscreenMetrics();
    maximizedAutoHide.work = maximizedAutoHide.monitor;
    maximizedAutoHide.frame = maximizedAutoHide.monitor;
    maximizedAutoHide.maximized = true;
    maximizedAutoHide.hasCaption = true;
    maximizedAutoHide.hasThickFrame = true;
    Check(!IsFullscreenWindowForMetrics(maximizedAutoHide),
          "maximized framed window with auto-hide taskbar is not fullscreen");

    FullscreenMetrics f11 = MakeFullscreenMetrics();
    f11.hasCaption = true;
    f11.hasThickFrame = true;
    Check(IsFullscreenWindowForMetrics(f11),
          "borderless F11-style window is fullscreen");

    FullscreenMetrics borderlessGame = MakeFullscreenMetrics();
    borderlessGame.popup = false; // WinForms games may not set WS_POPUP
    Check(IsFullscreenWindowForMetrics(borderlessGame),
          "borderless fullscreen game is fullscreen");

    FullscreenMetrics borderlessMaximized = MakeFullscreenMetrics();
    borderlessMaximized.maximized = true;
    Check(IsFullscreenWindowForMetrics(borderlessMaximized),
          "borderless maximized game is fullscreen");

    FullscreenMetrics exclusive = MakeFullscreenMetrics();
    exclusive.exclusiveD3DFullscreen = true;
    Check(IsFullscreenWindowForMetrics(exclusive),
          "exclusive D3D state with fullscreen geometry is fullscreen");

    FullscreenMetrics exclusiveSmall = MakeFullscreenMetrics();
    exclusiveSmall.frame = RECT{100, 100, 800, 600};
    exclusiveSmall.exclusiveD3DFullscreen = true;
    Check(!IsFullscreenWindowForMetrics(exclusiveSmall),
          "stale exclusive D3D state does not hide over a small window");

    FullscreenMetrics busyWindowed = MakeFullscreenMetrics();
    busyWindowed.frame = RECT{100, 100, 1280, 820};
    busyWindowed.shellBusy = true;
    Check(!IsFullscreenWindowForMetrics(busyWindowed),
          "QUNS_BUSY with a small window is not fullscreen");

    FullscreenMetrics busyPresentationMaximized = MakeFullscreenMetrics();
    busyPresentationMaximized.frame = busyPresentationMaximized.monitor;
    busyPresentationMaximized.maximized = true;
    busyPresentationMaximized.hasCaption = true;
    busyPresentationMaximized.shellBusy = true;
    Check(!IsFullscreenWindowForMetrics(busyPresentationMaximized),
          "QUNS_BUSY does not override the maximized-window guard");

    FullscreenMetrics busyTolerance = MakeFullscreenMetrics();
    busyTolerance.frame = RECT{8, 8, 1912, 1072};
    busyTolerance.shellBusy = true;
    Check(IsFullscreenWindowForMetrics(busyTolerance),
          "QUNS_BUSY tolerates small DPI/rounding gaps");

    FullscreenMetrics inset = MakeFullscreenMetrics();
    inset.frame = RECT{10, 10, 1910, 1070};
    Check(!IsFullscreenWindowForMetrics(inset),
          "a clearly inset window is not fullscreen");

    FullscreenMetrics shell = MakeFullscreenMetrics();
    shell.shellWindow = true;
    Check(!IsFullscreenWindowForMetrics(shell),
          "desktop shell is never fullscreen");

    FullscreenMetrics cloaked = MakeFullscreenMetrics();
    cloaked.cloaked = true;
    Check(!IsFullscreenWindowForMetrics(cloaked),
          "cloaked UWP window is not fullscreen");

    FullscreenMetrics iconic = MakeFullscreenMetrics();
    iconic.iconic = true;
    Check(!IsFullscreenWindowForMetrics(iconic),
          "minimized window is not fullscreen");

    FullscreenMetrics secondary = MakeFullscreenMetrics();
    secondary.monitor = RECT{1920, 0, 3840, 1080};
    secondary.work = RECT{1920, 0, 3840, 1032};
    secondary.frame = secondary.monitor;
    Check(IsFullscreenWindowForMetrics(secondary),
          "secondary-monitor fullscreen uses the same geometry rules");
}

void TestStatusBarFullscreenDebounce() {
    Moqi::SogouSkin::FullscreenSuppressionDebouncer debouncer(400);
    Check(!debouncer.active(), "fullscreen debouncer starts inactive");
    Check(debouncer.update(true, 1000),
          "fullscreen enters immediately");
    Check(debouncer.update(false, 1100),
          "fullscreen exit waits inside the hysteresis window");
    Check(debouncer.update(false, 1499),
          "fullscreen exit is still waiting before the deadline");
    Check(!debouncer.update(false, 1500),
          "fullscreen exit releases after the hysteresis window");
    debouncer.update(true, 2000);
    debouncer.reset();
    Check(!debouncer.active(), "fullscreen debouncer reset clears state");
}

void TestStatusBarFullscreenPolicy() {
    HWND ownWindow = CreateTestWindow(L"Moqi fullscreen policy owner",
                                      nullptr, WS_OVERLAPPEDWINDOW);
    HWND foreignWindow = ::GetShellWindow();
    if (foreignWindow) {
        DWORD foreignPid = 0;
        ::GetWindowThreadProcessId(foreignWindow, &foreignPid);
        if (foreignPid == ::GetCurrentProcessId()) {
            foreignWindow = nullptr;
        }
    }
    Check(ownWindow != nullptr, "fullscreen policy test window created");
    if (!ownWindow) {
        return;
    }

    using Policy = Moqi::SogouSkin::StatusBarForegroundPolicy;
    using Action = Policy::Action;

    Policy suppressedShow(::GetCurrentProcessId(), 1000);
    Check(suppressedShow.requestShow(ownWindow, true, 1000) == Action::None,
          "fullscreen show request stays hidden");
    Check(suppressedShow.showRequested(),
          "fullscreen show request is retained");
    Check(suppressedShow.evaluate(ownWindow, true, false, 1400) ==
              Action::None,
          "fullscreen suppression keeps polling");
    Check(suppressedShow.evaluate(ownWindow, false, false, 1800) ==
              Action::Show,
          "leaving fullscreen shows the retained bar");

    Policy visibleSuppressed(::GetCurrentProcessId(), 1000);
    Check(visibleSuppressed.requestShow(ownWindow, false, 1000) ==
              Action::Show,
          "ordinary show request shows immediately");
    Check(visibleSuppressed.evaluate(ownWindow, true, true, 1100) ==
              Action::Suppress,
          "visible bar is suppressed in fullscreen");
    Check(visibleSuppressed.evaluate(ownWindow, true, false, 1200) ==
              Action::None,
          "hidden suppressed bar keeps waiting");
    Check(visibleSuppressed.evaluate(ownWindow, false, false, 1600) ==
              Action::Show,
          "hidden suppressed bar returns after fullscreen");

    Policy foregroundLoss(::GetCurrentProcessId(), 1000);
    foregroundLoss.requestShow(ownWindow, true, 1000);
    foregroundLoss.evaluate(ownWindow, true, true, 1100);
    if (foreignWindow) {
        Check(foregroundLoss.evaluate(foreignWindow, true, true, 1200) ==
                  Action::Hide,
              "foreground loss hides a suppressed bar");
        Check(!foregroundLoss.showRequested(),
              "foreground loss clears the retained request");

        Policy pendingFullscreen(::GetCurrentProcessId(), 1000);
        Check(pendingFullscreen.requestShow(foreignWindow, false, 1000) ==
                  Action::None,
              "pending show during a foreground transition stays hidden");
        Check(pendingFullscreen.evaluate(ownWindow, true, false, 1200) ==
                  Action::None,
              "pending show does not flash when it lands on fullscreen");
        Check(pendingFullscreen.showRequested(),
              "pending fullscreen show keeps the request");
        Check(pendingFullscreen.evaluate(ownWindow, false, false, 1600) ==
                  Action::Show,
              "pending show returns after leaving fullscreen");
    } else {
        Skip("no foreign window for suppressed foreground-loss check");
    }

    Policy explicitHide(::GetCurrentProcessId(), 1000);
    explicitHide.requestShow(ownWindow, false, 1000);
    explicitHide.evaluate(ownWindow, true, true, 1100);
    explicitHide.clear();
    Check(!explicitHide.showRequested(),
          "explicit hide clears a suppressed request");
    Check(explicitHide.evaluate(ownWindow, false, false, 1200) ==
              Action::StopPolling,
          "explicit hide stops polling");
    Check(explicitHide.requestShow(ownWindow, false, 1300) == Action::Show,
          "show after explicit hide still works");

    ::DestroyWindow(ownWindow);
}

void PumpMessagesFor(DWORD milliseconds) {
    const ULONGLONG deadline = ::GetTickCount64() + milliseconds;
    while (::GetTickCount64() < deadline) {
        MSG msg = {};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::Sleep(10);
    }
}

void TestStatusBarWindowLifecycle() {
    const std::wstring skinDir = CreateTestSkinDirectory();
    Check(!skinDir.empty(), "minimal status-bar skin created");
    if (skinDir.empty()) {
        return;
    }
    {
        std::wstring error;
        auto theme = Moqi::SogouSkin::Theme::LoadWithOverrides(
            skinDir, L"", &error);
        Check(theme != nullptr, "minimal status-bar theme loads");
        if (theme) {
            Moqi::SogouSkin::StatusBarWindow bar(
                theme, {}, {}, L"MoqiSogouSkinStatusBarTest");
            Check(bar.create(), "status bar window created");
            Check(!bar.isVisible(), "status bar starts hidden");
            Check(!bar.isStateTimerActive(), "status bar starts without timer");

            bar.show();
            const bool foregroundOwned =
                Moqi::SogouSkin::IsForegroundProcess(::GetCurrentProcessId());
            Check(bar.isVisible() == foregroundOwned,
                  "show displays only when foreground is owned");
            Check(bar.isStateTimerActive(),
                  "show arms status timer or foreground grace timer");
            if (!foregroundOwned) {
                PumpMessagesFor(400);
                Check(bar.isStateTimerActive(),
                      "background show keeps polling during grace period");
                Check(!bar.isVisible(),
                      "background show never displays a stale window");
                PumpMessagesFor(1200);
                Check(!bar.isStateTimerActive(),
                      "background grace expires and stops timer");
            }
            bar.hide();
            Check(!bar.isVisible(), "hide leaves status bar hidden");
            Check(!bar.isStateTimerActive(), "hide stops status timer");

            // A hidden window must be able to come back after a later focus
            // event; hide() must not leave a dead election state.
            bar.show();
            Check(bar.isStateTimerActive(),
                  "show after hide re-arms status timer");
            Check(bar.isVisible() == foregroundOwned,
                  "show after hide respects foreground ownership");
            bar.hide();
        }
    }
    RemoveTestSkinDirectory(skinDir);
}

bool ForceForeground(HWND hwnd) {
    ::ShowWindow(hwnd, SW_SHOW);
    ::BringWindowToTop(hwnd);
    HWND foreground = ::GetForegroundWindow();
    const DWORD foregroundThread =
        foreground ? ::GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD selfThread = ::GetCurrentThreadId();
    bool attached = false;
    if (foregroundThread && foregroundThread != selfThread) {
        attached = ::AttachThreadInput(foregroundThread, selfThread, TRUE) != FALSE;
    }
    ::SetForegroundWindow(hwnd);
    ::SetFocus(hwnd);
    if (attached) {
        ::AttachThreadInput(foregroundThread, selfThread, FALSE);
    }
    PumpMessagesFor(250);
    return ::GetForegroundWindow() == hwnd;
}

int RunWindowHelper(const std::wstring& readyEventName) {
    HWND hwnd = CreateTestWindow(L"Moqi status bar foreground helper", nullptr,
                                 WS_OVERLAPPEDWINDOW);
    if (!hwnd) {
        return 1;
    }
    ::SetWindowPos(hwnd, nullptr, 120, 120, 320, 180,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    HANDLE ready = ::OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                readyEventName.c_str());
    if (ready) {
        ::SetEvent(ready);
        ::CloseHandle(ready);
    }
    const std::wstring stopName = readyEventName + L"_stop";
    HANDLE stop = ::OpenEventW(SYNCHRONIZE, FALSE, stopName.c_str());
    for (;;) {
        if (stop && ::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
            break;
        }
        MSG msg = {};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                goto done;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::Sleep(10);
    }
done:
    if (stop) {
        ::CloseHandle(stop);
    }
    ::DestroyWindow(hwnd);
    return 0;
}

// Optional interactive smoke test: actually creates a visible status bar and
// switches the foreground between this process and a helper process.  Run
// with SogouLogicTest.exe --statusbar-smoke.  The default headless test run
// does not touch the foreground.
int RunStatusBarSmokeTest() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("Moqi status bar foreground smoke test\n");
    const std::wstring skinDir = CreateTestSkinDirectory();
    if (skinDir.empty()) {
        Check(false, "minimal status-bar skin created");
        return 1;
    }
    HWND host = CreateTestWindow(L"Moqi status bar smoke host", nullptr,
                                 WS_OVERLAPPEDWINDOW);
    if (!host) {
        Check(false, "smoke host window created");
        RemoveTestSkinDirectory(skinDir);
        return 1;
    }
    {
        std::wstring error;
        auto theme = Moqi::SogouSkin::Theme::LoadWithOverrides(
            skinDir, L"", &error);
        Check(theme != nullptr, "smoke theme loads");
        if (theme) {
            Moqi::SogouSkin::StatusBarWindow bar(
                theme, {}, {}, L"MoqiSogouSkinStatusBarTest");
            Check(bar.create(), "smoke status bar created");
            if (!ForceForeground(host)) {
                Skip("could not acquire foreground for smoke test");
            } else {
                bar.show();
                PumpMessagesFor(200);
                Check(bar.isVisible(),
                      "status bar is visible while its process is foreground");
                Check(bar.isStateTimerActive(),
                      "visible status bar keeps its timer");

                const std::wstring eventName =
                    L"Local\\MoqiStatusBarSmoke_" +
                    std::to_wstring(::GetCurrentProcessId());
                const std::wstring stopName = eventName + L"_stop";
                HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE,
                                              eventName.c_str());
                HANDLE stop = ::CreateEventW(nullptr, TRUE, FALSE,
                                             stopName.c_str());
                wchar_t modulePath[MAX_PATH] = {};
                ::GetModuleFileNameW(nullptr, modulePath, _countof(modulePath));
                std::wstring commandLine = L"\"" + std::wstring(modulePath) +
                                           L"\" --window-helper \"" +
                                           eventName + L"\"";
                STARTUPINFOW startup = {};
                startup.cb = sizeof(startup);
                PROCESS_INFORMATION process = {};
                const BOOL started = ::CreateProcessW(
                    nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                    CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
                HWND helper = nullptr;
                if (started && ready &&
                    ::WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0) {
                    helper = ::FindWindowW(
                        nullptr, L"Moqi status bar foreground helper");
                }
                if (helper) {
                    DWORD helperPid = 0;
                    ::GetWindowThreadProcessId(helper, &helperPid);
                    // Browser/WebView2-style cross-process hosting: a child
                    // window owned by this process inside the foreground
                    // host window must make the host count as ours.
                    HWND hostedChild = ::CreateWindowExW(
                        0, L"STATIC", L"", WS_CHILD, 0, 0, 10, 10, helper,
                        nullptr, ::GetModuleHandleW(nullptr), nullptr);
                    if (hostedChild) {
                        Check(Moqi::SogouSkin::WindowBelongsToProcess(
                                  helper, ::GetCurrentProcessId()),
                              "host window with foreign hosted child counts as ours");
                        ::DestroyWindow(hostedChild);
                    } else {
                        Skip("cross-process child window unavailable");
                    }
                    ::AllowSetForegroundWindow(helperPid);
                    ::SetForegroundWindow(helper);
                    PumpMessagesFor(600);
                    if (::GetForegroundWindow() == helper) {
                        Check(!bar.isVisible(),
                              "status bar hides after foreground switch");
                        Check(!bar.isStateTimerActive(),
                              "foreground switch stops status timer");
                        if (ForceForeground(host)) {
                            bar.show();
                            PumpMessagesFor(200);
                            Check(bar.isVisible(),
                                  "status bar returns after refocus");
                        } else {
                            Skip("could not restore foreground for refocus check");
                        }
                    } else {
                        Skip("could not switch foreground to helper process");
                    }
                } else {
                    Skip("foreground helper process/window unavailable");
                }
                if (started) {
                    if (stop) {
                        ::SetEvent(stop);
                    }
                    if (::WaitForSingleObject(process.hProcess, 3000) !=
                        WAIT_OBJECT_0) {
                        ::TerminateProcess(process.hProcess, 0);
                    }
                    ::CloseHandle(process.hThread);
                    ::CloseHandle(process.hProcess);
                }
                if (ready) {
                    ::CloseHandle(ready);
                }
                if (stop) {
                    ::CloseHandle(stop);
                }
                bar.hide();
            }
        }
    }
    ::DestroyWindow(host);
    RemoveTestSkinDirectory(skinDir);
    std::printf("failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

int RunStatusBarFullscreenSmokeTest() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("Moqi status bar fullscreen smoke test\n");
    const std::wstring oldAppData = GetEnvironmentString(L"APPDATA");
    const std::wstring configRoot = CreateTempAppDataRoot();
    Check(!configRoot.empty(), "fullscreen smoke isolated APPDATA created");
    if (configRoot.empty()) {
        return 1;
    }
    _wputenv_s(L"APPDATA", configRoot.c_str());
    Check(WriteOptionsFile(configRoot,
                           "[Options]\nstatusbar=1\n"
                           "statusbar_fullscreen_hide=1\n"
                           "statusbar_fullscreen_scope=monitor\n"),
          "fullscreen smoke options written");

    const std::wstring skinDir = CreateTestSkinDirectory();
    Check(!skinDir.empty(), "fullscreen smoke skin created");
    if (skinDir.empty()) {
        _wputenv_s(L"APPDATA",
                   oldAppData.empty() ? L"" : oldAppData.c_str());
        RemoveTempAppDataRoot(configRoot);
        return 1;
    }

    HWND host = CreateTestWindow(L"Moqi fullscreen smoke host", nullptr,
                                 WS_POPUP, WS_EX_TOPMOST);
    Check(host != nullptr, "fullscreen smoke popup created");
    if (!host) {
        RemoveTestSkinDirectory(skinDir);
        _wputenv_s(L"APPDATA",
                   oldAppData.empty() ? L"" : oldAppData.c_str());
        RemoveTempAppDataRoot(configRoot);
        return 1;
    }

    POINT origin = {0, 0};
    HMONITOR primaryMonitor =
        ::MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    const bool haveMonitor =
        primaryMonitor && ::GetMonitorInfoW(primaryMonitor, &monitorInfo);
    Check(haveMonitor, "fullscreen smoke primary monitor resolved");
    if (haveMonitor) {
        ::SetWindowPos(host, HWND_TOPMOST, monitorInfo.rcMonitor.left,
                       monitorInfo.rcMonitor.top,
                       monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                       monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                       SWP_SHOWWINDOW);
    }

    {
        std::wstring error;
        auto theme = Moqi::SogouSkin::Theme::LoadWithOverrides(skinDir, L"",
                                                               &error);
        Check(theme != nullptr, "fullscreen smoke theme loads");
        if (theme && haveMonitor) {
            Moqi::SogouSkin::StatusBarWindow bar(
                theme, {}, {}, L"MoqiSogouSkinStatusBarTest");
            Check(bar.create(), "fullscreen smoke status bar created");
            if (!ForceForeground(host)) {
                Skip("could not acquire foreground for fullscreen smoke");
            } else {
                bar.show();
                PumpMessagesFor(600);
                Check(!bar.isVisible(),
                      "status bar stays hidden over a fullscreen popup");
                Check(bar.isStateTimerActive(),
                      "fullscreen suppression keeps polling");

                ::SetWindowPos(host, HWND_TOPMOST, monitorInfo.rcMonitor.left + 80,
                               monitorInfo.rcMonitor.top + 80, 800, 600,
                               SWP_SHOWWINDOW);
                PumpMessagesFor(1600);
                Check(bar.isVisible(),
                      "status bar returns after leaving fullscreen");

                // Disabling the option must restore the old behavior even
                // while the foreground window is fullscreen.
                Check(WriteOptionsFile(configRoot,
                                       "[Options]\nstatusbar=1\n"
                                       "statusbar_fullscreen_hide=0\n"
                                       "statusbar_fullscreen_scope=monitor\n"),
                      "fullscreen smoke disable option written");
                ::SetWindowPos(host, HWND_TOPMOST,
                               monitorInfo.rcMonitor.left,
                               monitorInfo.rcMonitor.top,
                               monitorInfo.rcMonitor.right -
                                   monitorInfo.rcMonitor.left,
                               monitorInfo.rcMonitor.bottom -
                                   monitorInfo.rcMonitor.top,
                               SWP_SHOWWINDOW);
                bar.show();
                PumpMessagesFor(700);
                Check(bar.isVisible(),
                      "statusbar_fullscreen_hide=0 keeps the bar visible");

                Check(WriteOptionsFile(configRoot,
                                       "[Options]\nstatusbar=1\n"
                                       "statusbar_fullscreen_hide=1\n"
                                       "statusbar_fullscreen_scope=all\n"),
                      "fullscreen smoke scope=all option written");
                bar.show();
                PumpMessagesFor(600);
                Check(!bar.isVisible(),
                      "statusbar_fullscreen_scope=all hides on any monitor");

                // Multi-monitor: a fullscreen window on the other monitor
                // must not hide a bar that lives on a different monitor when
                // scope=monitor; scope=all must hide it.
                const auto monitors = EnumerateMonitors();
                const HMONITOR barMonitor = ::MonitorFromWindow(
                    bar.hwnd(), MONITOR_DEFAULTTONEAREST);
                HMONITOR otherMonitor = nullptr;
                for (HMONITOR monitor : monitors) {
                    if (monitor != barMonitor) {
                        otherMonitor = monitor;
                        break;
                    }
                }
                if (otherMonitor) {
                    MONITORINFO otherInfo = {};
                    otherInfo.cbSize = sizeof(otherInfo);
                    if (::GetMonitorInfoW(otherMonitor, &otherInfo)) {
                        ::SetWindowPos(
                            host, HWND_TOPMOST, otherInfo.rcMonitor.left,
                            otherInfo.rcMonitor.top,
                            otherInfo.rcMonitor.right -
                                otherInfo.rcMonitor.left,
                            otherInfo.rcMonitor.bottom -
                                otherInfo.rcMonitor.top,
                            SWP_SHOWWINDOW);
                        if (ForceForeground(host)) {
                            Check(WriteOptionsFile(
                                      configRoot,
                                      "[Options]\nstatusbar=1\n"
                                      "statusbar_fullscreen_hide=1\n"
                                      "statusbar_fullscreen_scope=monitor\n"),
                                  "multi-monitor scope=monitor option written");
                            bar.show();
                            PumpMessagesFor(700);
                            Check(bar.isVisible(),
                                  "bar stays visible when fullscreen is on "
                                  "another monitor");

                            Check(WriteOptionsFile(
                                      configRoot,
                                      "[Options]\nstatusbar=1\n"
                                      "statusbar_fullscreen_hide=1\n"
                                      "statusbar_fullscreen_scope=all\n"),
                                  "multi-monitor scope=all option written");
                            bar.show();
                            PumpMessagesFor(700);
                            Check(!bar.isVisible(),
                                  "scope=all hides the bar for another-monitor "
                                  "fullscreen");

                            ::SetWindowPos(host, HWND_TOPMOST,
                                           otherInfo.rcMonitor.left + 80,
                                           otherInfo.rcMonitor.top + 80, 800,
                                           600, SWP_SHOWWINDOW);
                            PumpMessagesFor(1600);
                            Check(bar.isVisible(),
                                  "bar returns after other-monitor fullscreen "
                                  "ends");
                        } else {
                            Skip("could not acquire foreground on second monitor");
                        }
                    } else {
                        Skip("second monitor info unavailable");
                    }
                } else {
                    Skip("second monitor unavailable for scope test");
                }

                // Regression guard for the taskbar-auto-hide case: a normal
                // maximized framed window covers the whole monitor there but
                // must keep the status bar visible.
                HWND maximizedHost = CreateTestWindow(
                    L"Moqi fullscreen smoke maximized host", nullptr,
                    WS_OVERLAPPEDWINDOW);
                if (maximizedHost) {
                    ::ShowWindow(maximizedHost, SW_MAXIMIZE);
                    if (ForceForeground(maximizedHost)) {
                        Check(WriteOptionsFile(
                                  configRoot,
                                  "[Options]\nstatusbar=1\n"
                                  "statusbar_fullscreen_hide=1\n"
                                  "statusbar_fullscreen_scope=monitor\n"),
                              "maximized-window option written");
                        bar.show();
                        PumpMessagesFor(500);
                        Check(bar.isVisible(),
                              "maximized framed window is not treated as "
                              "fullscreen");
                    } else {
                        Skip("could not foreground the maximized test window");
                    }
                    ::DestroyWindow(maximizedHost);
                } else {
                    Skip("maximized test window unavailable");
                }
            }
            bar.hide();
        }
    }

    ::DestroyWindow(host);
    RemoveTestSkinDirectory(skinDir);
    _wputenv_s(L"APPDATA", oldAppData.empty() ? L"" : oldAppData.c_str());
    RemoveTempAppDataRoot(configRoot);
    std::printf("failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

void TestFontScaleAndLegacyMigration() {
    const std::wstring oldAppData = GetEnvironmentString(L"APPDATA");
    const std::wstring root = CreateTempAppDataRoot();
    Check(!root.empty(), "font/scale migration isolated APPDATA created");
    if (root.empty()) {
        return;
    }
    _wputenv_s(L"APPDATA", root.c_str());
    const std::wstring skinDir = CreateTestSkinDirectory();
    Check(!skinDir.empty(), "font/scale migration skin created");
    if (skinDir.empty()) {
        _wputenv_s(L"APPDATA", oldAppData.empty() ? L"" : oldAppData.c_str());
        RemoveTempAppDataRoot(root);
        return;
    }
    const std::wstring overrides = root + L"\\Moqi\\sogou_skin_overrides.ini";

    std::wstring error;
    auto base = Moqi::SogouSkin::Theme::LoadWithOverrides(skinDir, L"", &error);
    Check(base != nullptr, "factory theme loads");
    if (base) {
        Check(base->display().fontSize == 16,
              "factory font_size remains the design text size");
        Check(base->display().scalePercent == 100,
              "factory whole-skin scale defaults to 100%");
        Moqi::SogouSkin::Renderer renderer(base, 96);
        Check(renderer.designFontPixelSize() == 16 &&
                  std::abs(renderer.scale() - 1.0) < 1e-9,
              "factory 100% renderer stays at 1.0");
    }

    // Legacy file without a marker: font_size was both text size and global
    // scale.  24 / 16 must become scale=150 with the native 16 px font.
    Check(WriteUtf8File(overrides, "[Display]\nfont_size=24\n"),
          "legacy override written");
    auto legacy = Moqi::SogouSkin::Theme::LoadWithOverrides(skinDir, overrides, &error);
    Check(legacy != nullptr, "legacy override theme loads");
    if (legacy) {
        Check(legacy->display().fontSize == 16,
              "legacy font_size migrates text size back to skin base");
        Check(legacy->display().scalePercent == 150,
              "legacy font_size 24/16 migrates to scale 150%");
        Moqi::SogouSkin::Renderer renderer(legacy, 96);
        Check(std::abs(renderer.scale() - 1.5) < 1e-9,
              "legacy migration preserves the old window scale");
    }

    // The previous settings UI wrote version=2 with the same combined
    // semantics; it must migrate too.
    Check(WriteUtf8File(
              overrides,
              "[Display]\nfont_size=24\n"
              "[MoqiOverrides]\nversion=2\nlayout_mode=global\n"),
          "version 2 override written");
    auto version2 = Moqi::SogouSkin::Theme::LoadWithOverrides(skinDir, overrides, &error);
    if (version2) {
        Check(version2->display().fontSize == 16 &&
                  version2->display().scalePercent == 150,
              "version 2 file receives the same legacy migration");
    } else {
        Check(false, "version 2 override theme loads");
    }

    // New semantics: independent actual font size and global scale.
    Check(WriteUtf8File(
              overrides,
              "[Display]\nfont_size=20\nscale=80\n"
              "[MoqiOverrides]\nversion=3\n"),
          "new-format override written");
    auto modern = Moqi::SogouSkin::Theme::LoadWithOverrides(skinDir, overrides, &error);
    if (modern) {
        Check(modern->display().fontSize == 20,
              "new font_size is the actual text size");
        Check(modern->display().scalePercent == 80,
              "new scale is the whole-skin percentage");
        Moqi::SogouSkin::Renderer renderer(modern, 96);
        Check(renderer.designFontPixelSize() == 20 &&
                  std::abs(renderer.scale() - 0.8) < 1e-9,
              "new font size does not multiply the whole-window scale");
    } else {
        Check(false, "new-format override theme loads");
    }

    if (base) {
        Moqi::SogouSkin::SettingsOverrides values;
        values.fontSize = 18;
        values.scalePercent = 130;
        const std::wstring text =
            Moqi::SogouSkin::BuildSettingsOverridesText(*base, overrides, values);
        Check(text.find(L"font_size=18") != std::wstring::npos,
              "settings saves the independent font size");
        Check(text.find(L"scale=130") != std::wstring::npos,
              "settings saves the global scale percentage");
        Check(text.find(L"version=3") != std::wstring::npos,
              "settings marks the file as version 3");

        // Saving over an old version=2 file must preserve the migrated window
        // scale instead of writing the raw old font size as the new font.
        Check(WriteUtf8File(
                  overrides,
                  "[Display]\nfont_size=24\n"
                  "[MoqiOverrides]\nversion=2\nlayout_mode=global\n"),
              "version 2 file restored for save migration");
        auto migrated = Moqi::SogouSkin::Theme::LoadWithOverrides(
            skinDir, overrides, &error);
        if (migrated) {
            Moqi::SogouSkin::SettingsOverrides migratedValues;
            migratedValues.fontSize = migrated->display().fontSize;
            migratedValues.scalePercent = migrated->display().scalePercent;
            const std::wstring migratedText =
                Moqi::SogouSkin::BuildSettingsOverridesText(
                    *base, overrides, migratedValues);
            Check(migratedText.find(L"font_size=16") != std::wstring::npos &&
                      migratedText.find(L"scale=150") != std::wstring::npos,
                  "saving an old file writes migrated font size and scale");
        } else {
            Check(false, "version 2 save-migration theme loads");
        }
    }

    _wputenv_s(L"APPDATA", oldAppData.empty() ? L"" : oldAppData.c_str());
    RemoveTestSkinDirectory(skinDir);
    RemoveTempAppDataRoot(root);
}

void TestSkinLayoutStorage() {
    const std::wstring oldAppData = GetEnvironmentString(L"APPDATA");
    const std::wstring root = CreateTempAppDataRoot();
    Check(!root.empty(), "layout storage isolated APPDATA created");
    if (root.empty()) {
        return;
    }
    _wputenv_s(L"APPDATA", root.c_str());
    const std::wstring skinA = CreateTestSkinDirectoryWithIni(
        "[Display]\nfont_size=16\n[Scheme_H1]\npic=scheme.bmp\n");
    const std::wstring skinB = CreateTestSkinDirectoryWithIni(
        "[Display]\nfont_size=20\n[Scheme_H1]\npic=scheme.bmp\n");
    Check(!skinA.empty() && !skinB.empty(), "two layout test skins created");
    if (skinA.empty() || skinB.empty()) {
        if (!skinA.empty()) RemoveTestSkinDirectory(skinA);
        if (!skinB.empty()) RemoveTestSkinDirectory(skinB);
        _wputenv_s(L"APPDATA", oldAppData.empty() ? L"" : oldAppData.c_str());
        RemoveTempAppDataRoot(root);
        return;
    }

    std::wstring errorA;
    std::wstring errorB;
    auto themeA = Moqi::SogouSkin::Theme::LoadWithOverrides(skinA, L"", &errorA);
    auto themeB = Moqi::SogouSkin::Theme::LoadWithOverrides(skinB, L"", &errorB);
    Check(themeA && themeB, "layout test themes load");
    if (themeA && themeB) {
        Check(!themeA->skinId().empty() && themeA->skinId() != themeB->skinId(),
              "different skins get different stable section ids");
        Check(themeA->layout().preeditX == 0 && themeA->layout().candidateY == 0,
              "per-skin layout defaults to zero");

        Moqi::SogouSkin::SkinLayout a;
        a.preeditX = 10;
        a.preeditY = -5;
        a.candidateX = 20;
        a.candidateY = -15;
        Check(Moqi::SogouSkin::SaveSkinLayoutOffsets(themeA->skinId(), a),
              "skin A layout saved");
        Moqi::SogouSkin::SkinLayout b;
        b.preeditX = -1;
        b.preeditY = 2;
        b.candidateX = -3;
        b.candidateY = 4;
        Check(Moqi::SogouSkin::SaveSkinLayoutOffsets(themeB->skinId(), b),
              "skin B layout saved");

        auto reloadA = Moqi::SogouSkin::Theme::LoadWithOverrides(skinA, L"", &errorA);
        auto reloadB = Moqi::SogouSkin::Theme::LoadWithOverrides(skinB, L"", &errorB);
        Check(reloadA && reloadA->layout().preeditX == 10 &&
                  reloadA->layout().preeditY == -5 &&
                  reloadA->layout().candidateX == 20 &&
                  reloadA->layout().candidateY == -15,
              "skin A reloads its own offsets");
        Check(reloadB && reloadB->layout().preeditX == -1 &&
                  reloadB->layout().preeditY == 2 &&
                  reloadB->layout().candidateX == -3 &&
                  reloadB->layout().candidateY == 4,
              "skin B does not inherit skin A offsets");

        Moqi::SogouSkin::SkinLayout clamped;
        clamped.preeditX = 200;
        clamped.preeditY = -300;
        clamped.candidateX = 101;
        clamped.candidateY = -101;
        Check(Moqi::SogouSkin::SaveSkinLayoutOffsets(themeA->skinId(), clamped),
              "out-of-range layout saved");
        const auto clampedLoad =
            Moqi::SogouSkin::LoadSkinLayoutOffsets(themeA->skinId());
        Check(clampedLoad.preeditX == 100 && clampedLoad.preeditY == -100 &&
                  clampedLoad.candidateX == 100 &&
                  clampedLoad.candidateY == -100,
              "layout offsets clamp to -100..100 design pixels");

        const std::wstring customPath = root + L"\\custom_layouts.ini";
        Check(WriteUtf8File(customPath, "[Custom]\nkeep_me=1\n"),
              "custom layout section written");
        const std::wstring merged = Moqi::SogouSkin::BuildSkinLayoutsText(
            customPath, themeA->skinId(), a);
        Check(merged.find(L"[custom]") != std::wstring::npos &&
                  merged.find(L"keep_me=1") != std::wstring::npos,
              "layout writer preserves unrelated sections");
        Check(merged.find(L"preedit_x=10") != std::wstring::npos,
              "layout writer emits the current skin section");
    }

    RemoveTestSkinDirectory(skinA);
    RemoveTestSkinDirectory(skinB);
    _wputenv_s(L"APPDATA", oldAppData.empty() ? L"" : oldAppData.c_str());
    RemoveTempAppDataRoot(root);
}

void TestRendererFontScaleAndOffsets() {
    const std::wstring oldAppData = GetEnvironmentString(L"APPDATA");
    const std::wstring root = CreateTempAppDataRoot();
    Check(!root.empty(), "renderer offset test APPDATA created");
    if (root.empty()) {
        return;
    }
    _wputenv_s(L"APPDATA", root.c_str());
    const std::wstring skinDir = CreateTestSkinDirectoryWithIni(
        "[Display]\nfont_size=16\n"
        "[Scheme_V1]\n"
        "pic=scheme.bmp\n"
        "pinyin_marge=4,2,6,8\n"
        "zhongwen_marge=5,3,7,9\n"
        "[Render]\nitem_spacing=3\n");
    Check(!skinDir.empty(), "renderer offset skin created");
    if (skinDir.empty()) {
        _wputenv_s(L"APPDATA", oldAppData.empty() ? L"" : oldAppData.c_str());
        RemoveTempAppDataRoot(root);
        return;
    }

    std::wstring error;
    auto theme = Moqi::SogouSkin::Theme::LoadWithOverrides(skinDir, L"", &error);
    Check(theme != nullptr, "renderer offset theme loads");
    if (theme) {
        Moqi::SogouSkin::RenderState state;
        state.preedit = L"moqi";
        state.candidates = {L"墨奇", L"默契"};
        state.selKeys = L"12";
        state.candPerRow = 1;
        HDC dc = ::GetDC(nullptr);
        Moqi::SogouSkin::Renderer renderer(theme, 96);
        const auto baseline = renderer.measure(state, dc);

        Moqi::SogouSkin::SkinLayout nudges;
        nudges.preeditX = 10;
        nudges.preeditY = 5;
        nudges.candidateX = 20;
        nudges.candidateY = 7;
        theme->setLayout(nudges);
        const auto shifted = renderer.measure(state, dc);
        Check(shifted.width == baseline.width && shifted.height == baseline.height,
              "nudges do not resize or move the background plan");
        Check(shifted.preeditRect.left == baseline.preeditRect.left + 10 &&
                  shifted.preeditRect.top == baseline.preeditRect.top + 5,
              "preedit X/Y nudges apply in design pixels");
        Check(shifted.candidateRects.size() == baseline.candidateRects.size() &&
                  shifted.candidateRects[0].left ==
                      baseline.candidateRects[0].left + 20 &&
                  shifted.candidateRects[0].top ==
                      baseline.candidateRects[0].top + 7,
              "candidate X/Y nudges apply to the click rects");
        Check(baseline.preeditRect.left == 6,
              "font size does not multiply the skin's left margin");

        Moqi::SogouSkin::Renderer hidpi(theme, 144);
        const auto scaled = hidpi.measure(state, dc);
        Check(scaled.preeditRect.left == 24 && scaled.preeditRect.top == 14,
              "nudges scale correctly with DPI");
        ::ReleaseDC(nullptr, dc);

        Moqi::SogouSkin::SettingsOverrides values;
        values.fontSize = 24;
        values.scalePercent = 100;
        const std::wstring text = Moqi::SogouSkin::BuildSettingsOverridesText(
            *theme, L"", values);
        Check(text.find(L"font_size=24") != std::wstring::npos &&
                  text.find(L"scale=100") != std::wstring::npos,
              "settings writer keeps the 100% scale with a larger font");
        const std::wstring overridePath =
            root + L"\\Moqi\\sogou_skin_overrides.ini";
        Check(WriteUtf8File(overridePath, "[Display]\nfont_size=24\nscale=100\n"
                                         "[MoqiOverrides]\nversion=3\n"),
              "large-font override written");
        auto largeTheme = Moqi::SogouSkin::Theme::LoadWithOverrides(
            skinDir, overridePath, &error);
        if (largeTheme) {
            Moqi::SogouSkin::Renderer largeRenderer(largeTheme, 96);
            Check(largeRenderer.designFontPixelSize() == 24 &&
                      std::abs(largeRenderer.scale() - 1.0) < 1e-9,
                  "large font keeps the global scale at 100%");
            HDC largeDc = ::GetDC(nullptr);
            const auto largeLayout = largeRenderer.measure(state, largeDc);
            ::ReleaseDC(nullptr, largeDc);
            Check(largeLayout.preeditRect.left == 6,
                  "large font does not multiply the 9-slice margin");
            Check(largeLayout.height >= baseline.height,
                  "large font may grow the layout for measured text");
        } else {
            Check(false, "large-font theme loads");
        }
    }

    RemoveTestSkinDirectory(skinDir);
    _wputenv_s(L"APPDATA", oldAppData.empty() ? L"" : oldAppData.c_str());
    RemoveTempAppDataRoot(root);
}

void TestSogouUModeDisplay() {
    using Moqi::BuildSogouUModePreedit;
    Check(BuildSogouUModePreedit(L"u'shui'shang", L"shui'shang") ==
              L"u'shui'shang[拼字]",
          "u-mode keeps typed apostrophes and appends [拼字]");
    Check(BuildSogouUModePreedit(L"ushui'shang", L"shui'shang") ==
              L"u'shui'shang[拼字]",
          "u-mode inserts the leading separator");
    Check(BuildSogouUModePreedit(L"ushui", L"shui") == L"u'shui[拼字]",
          "u-mode restores the consumed u");
    Check(BuildSogouUModePreedit(L"ushuishang", L"shui'shang") ==
              L"u'shui'shang[拼字]",
          "u-mode uses backend syllable separators");
    Check(BuildSogouUModePreedit(L"u", L"u") == L"u[拼字]",
          "u-mode shows the prefix before the first lookup");
    Check(BuildSogouUModePreedit(L"u'", L"u") == L"u[拼字]",
          "u-mode keeps the hint after a lone leading apostrophe");
    Check(BuildSogouUModePreedit(L"ushui", L"shui'shang") == L"u'shui[拼字]",
          "u-mode never appends backend-only text to the raw input");
    Check(BuildSogouUModePreedit(L"ushui'shang", L"shangshui") ==
              L"u'shui'shang[拼字]",
          "u-mode keeps the raw keys when the backend reorders them");
    Check(BuildSogouUModePreedit(L"ushui's", L"sshui") ==
              L"u'shui's[拼字]",
          "u-mode keeps a partially typed trailing syllable");
    Check(BuildSogouUModePreedit(L"ushui'", L"u  〔拆字〕shui") ==
              L"u'shui[拼字]",
          "u-mode trims a trailing separator while the next key arrives");
    Check(BuildSogouUModePreedit(L"us", L"u  〔拆字〕") == L"u's[拼字]",
          "u-mode strips Rime's affix tip from the backend preedit");
    Check(BuildSogouUModePreedit(L"u", L"u  〔拆字〕") == L"u[拼字]",
          "u-mode keeps a preedit-only tip state visible");
    Check(BuildSogouUModePreedit(L"us", L"") == L"u's[拼字]",
          "u-mode preedit survives an empty backend composition");
    Check(BuildSogouUModePreedit(L"", L"nihao") == L"nihao",
          "normal pinyin preedit is unchanged");
    Check(BuildSogouUModePreedit(L"", L"u") == L"u[拼字]",
          "backend-only u-mode preedit still gets the hint");
    Check(BuildSogouUModePreedit(L"uan", L"uan") == L"uan",
          "ordinary pinyin starting with u is not decorated");
    Check(BuildSogouUModePreedit(L"ushui", L"ushui") == L"ushui",
          "ordinary pinyin that keeps the full u is not decorated");

    // Enter on a u-mode gesture commits the complete raw input.  Rime only
    // strips the leading u from the composition preedit, never from the
    // commit string, so ui/uxxx/u'shui must keep every typed character.
    using Moqi::ApplyUModeEnterCommit;
    Check(ApplyUModeEnterCommit(L"u", L"u") == L"u",
          "u-mode Enter commits a lone u");
    Check(ApplyUModeEnterCommit(L"uu", L"uu") == L"uu",
          "u-mode Enter commits a typed uu sequence");
    Check(ApplyUModeEnterCommit(L"uuu", L"uuu") == L"uuu",
          "u-mode Enter commits a typed uuu sequence");
    Check(ApplyUModeEnterCommit(L"ui", L"ui") == L"ui",
          "u-mode Enter keeps u on a suffixed lookup");
    Check(ApplyUModeEnterCommit(L"uxxx", L"uxxx") == L"uxxx",
          "u-mode Enter keeps u on a long suffixed lookup");
    Check(ApplyUModeEnterCommit(L"u'shui", L"u'shui") == L"u'shui",
          "u-mode Enter keeps typed apostrophes and the leading u");
    Check(ApplyUModeEnterCommit(L"ushui", L"ushui") == L"ushui",
          "u-mode Enter commits ushui verbatim");
    Check(ApplyUModeEnterCommit(L"uu", L"u") == L"uu",
          "u-mode Enter restores a stripped uu prefix");
    Check(ApplyUModeEnterCommit(L"ui", L"i") == L"ui",
          "u-mode Enter restores u if a backend strips it");
    Check(ApplyUModeEnterCommit(L"uxxx", L"xxx") == L"uxxx",
          "u-mode Enter restores u for a stripped long lookup");
    Check(ApplyUModeEnterCommit(L"u'shui", L"shui") == L"u'shui",
          "u-mode Enter restores the complete raw lookup text");
    Check(ApplyUModeEnterCommit(L"u'", L"u") == L"u",
          "u-mode Enter keeps u when only separators follow it");
    Check(ApplyUModeEnterCommit(L"us", L"𢌽") == L"𢌽",
          "u-mode Chinese candidate commits are untouched");
    Check(ApplyUModeEnterCommit(L"", L"u") == L"u",
          "Enter without a tracked gesture is untouched");
    Check(ApplyUModeEnterCommit(L"u", L"") == L"",
          "empty Enter commit stays empty");

    using Moqi::ShouldKeepPreeditWindow;
    Check(ShouldKeepPreeditWindow(true, true, false),
          "zero-candidate composition keeps an external preedit window");
    Check(!ShouldKeepPreeditWindow(true, false, false),
          "inline preedit does not force an empty candidate window");
    Check(!ShouldKeepPreeditWindow(false, true, false),
          "finished composition still hides the candidate window");
    Check(ShouldKeepPreeditWindow(false, true, true),
          "u-mode tip-only state keeps its preedit window");

    Moqi::UModeInputTracker tracker;
    tracker.onKeyEvent(L'u', 'U', true, true, true, false, true);
    Check(tracker.rawInput() == L"u", "u-mode tracker starts from idle u");
    tracker.onKeyEvent(L's', 'S', true, true, true, false, false);
    tracker.onKeyEvent(L'h', 'H', true, true, true, false, false);
    tracker.onKeyEvent(L'u', 'U', true, true, true, false, false);
    tracker.onKeyEvent(L'i', 'I', true, true, true, false, false);
    Check(tracker.rawInput() == L"ushui", "u-mode tracker follows letters");
    tracker.onKeyEvent(0, VK_BACK, true, true, true, false, false);
    Check(tracker.rawInput() == L"ushu", "u-mode tracker handles backspace");
    tracker.onKeyEvent(L'\'', VK_OEM_7, true, true, true, false, false);
    Check(tracker.rawInput() == L"ushu'", "u-mode tracker keeps apostrophes");
    tracker.onKeyEvent(0, VK_RETURN, true, false, false, true, false);
    Check(tracker.rawInput().empty(), "u-mode tracker clears on commit/enter");

    Moqi::UModeInputTracker tipOnly;
    tipOnly.onKeyEvent(L'u', 'U', true, true, true, false, true);
    tipOnly.onKeyEvent(L's', 'S', true, true, true, false, false);
    tipOnly.onKeyEvent(0, VK_BACK, true, false, false, false, false);
    Check(tipOnly.rawInput() == L"u",
          "u-mode tracker keeps u across a tip-only backspace response");

    Moqi::UModeInputTracker selection;
    selection.onKeyEvent(L'u', 'U', true, true, true, false, true);
    selection.onKeyEvent(L's', 'S', true, true, true, false, false);
    selection.onKeyEvent(L'1', '1', true, false, false, false, false);
    Check(selection.rawInput() == L"us",
          "u-mode selection keys do not clear a tracked gesture");

    Moqi::UModeInputTracker notIdle;
    notIdle.onKeyEvent(L'u', 'U', true, true, true, false, false);
    Check(notIdle.rawInput().empty(),
          "u-mode tracker does not start inside another composition");

    Moqi::UModeInputTracker ordinary;
    ordinary.onKeyEvent(L'n', 'N', true, true, true, false, true);
    ordinary.onKeyEvent(L'u', 'U', true, true, true, false, false);
    Check(ordinary.rawInput().empty(),
          "ordinary pinyin containing u is not decorated");
}

void TestWebView2HostIfPresent() {
    HWND host = ::FindWindowW(L"MoqiWebView2TestHost", nullptr);
    if (!host) {
        Skip("MoqiWebView2TestHost is not running");
        return;
    }
    DWORD hostPid = 0;
    ::GetWindowThreadProcessId(host, &hostPid);
    Check(Moqi::SogouSkin::WindowBelongsToProcess(host, hostPid),
          "WebView2 host belongs to its own process");

    std::vector<DWORD> childPids;
    struct ChildSearch {
        DWORD hostPid;
        std::vector<DWORD>* pids;
    } search = {hostPid, &childPids};
    ::EnumChildWindows(
        host,
        [](HWND child, LPARAM param) -> BOOL {
            auto* state = reinterpret_cast<ChildSearch*>(param);
            DWORD pid = 0;
            ::GetWindowThreadProcessId(child, &pid);
            if (pid != 0 && pid != state->hostPid &&
                std::find(state->pids->begin(), state->pids->end(), pid) ==
                    state->pids->end()) {
                state->pids->push_back(pid);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (childPids.empty()) {
        Skip("WebView2 host has no cross-process child window");
        return;
    }
    for (DWORD childPid : childPids) {
        Check(Moqi::SogouSkin::WindowBelongsToProcess(host, childPid),
              "WebView2 host is recognized for its renderer child process");
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && _wcsicmp(argv[1], L"--window-helper") == 0) {
        return RunWindowHelper(argc > 2 ? argv[2] : L"");
    }
    if (argc > 1 && _wcsicmp(argv[1], L"--statusbar-smoke") == 0) {
        return RunStatusBarSmokeTest();
    }
    if (argc > 1 && _wcsicmp(argv[1], L"--statusbar-fullscreen-smoke") == 0) {
        return RunStatusBarFullscreenSmokeTest();
    }
    if (argc > 1 && _wcsicmp(argv[1], L"--webview-check") == 0) {
        std::printf("Moqi WebView2 window ownership check\n");
        TestWebView2HostIfPresent();
        std::printf("failures=%d\n", g_failures);
        return g_failures == 0 ? 0 : 1;
    }
    std::printf("Moqi Sogou logic tests\n");
    TestKeyRepeatStateMachine();
    TestKeyRepeatTimer();
    TestSymbolMenu();
    TestDefaultFonts();
    TestForegroundOwnership();
    TestStatusBarForegroundPolicy();
    TestStatusBarFullscreenOptions();
    TestStatusBarFullscreenClassifier();
    TestStatusBarFullscreenDebounce();
    TestStatusBarFullscreenPolicy();
    TestStatusBarWindowLifecycle();
    TestFontScaleAndLegacyMigration();
    TestSkinLayoutStorage();
    TestRendererFontScaleAndOffsets();
    TestSogouUModeDisplay();
    std::printf("failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
