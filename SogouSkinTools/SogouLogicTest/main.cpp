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
#include <cstdio>
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

HWND CreateTestWindow(const wchar_t* title, HWND parent, DWORD style) {
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
    return ::CreateWindowExW(0, kClass, title, style, 0, 0, 120, 80,
                             parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
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

std::wstring CreateTestSkinDirectory() {
    wchar_t tempPath[MAX_PATH] = {};
    if (::GetTempPathW(_countof(tempPath), tempPath) == 0) {
        return L"";
    }
    std::wstring dir = tempPath;
    dir += L"MoqiStatusBarTest_";
    dir += std::to_wstring(::GetCurrentProcessId());
    dir += L"_";
    dir += std::to_wstring(::GetTickCount64());
    if (!::CreateDirectoryW(dir.c_str(), nullptr)) {
        return L"";
    }
    if (!WriteSolidBmp(dir + L"\\scheme.bmp", 40, 24) ||
        !WriteSolidBmp(dir + L"\\bar.bmp", 40, 24)) {
        return L"";
    }
    std::ofstream ini(dir + L"\\skin.ini", std::ios::binary | std::ios::trunc);
    ini << "[Display]\n"
           "font_size=16\n"
           "[Scheme_H1]\n"
           "pic=scheme.bmp\n"
           "[StatusBar]\n"
           "pic=bar.bmp\n"
           "menu_display=1\n"
           "menu_pos=0,0\n"
           "menu=bar.bmp\n";
    if (!ini.good()) {
        return L"";
    }
    return dir;
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
    TestStatusBarWindowLifecycle();
    TestSogouUModeDisplay();
    std::printf("failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
