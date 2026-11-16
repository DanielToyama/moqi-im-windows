#include "KeyRepeatAccelerator.h"

#include <algorithm>

namespace Moqi {
namespace {

constexpr wchar_t kTimerWindowClass[] = L"MoqiKeyRepeatTimerWindow";
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerPeriodMs = 10;

} // namespace

KeyRepeatAccelerator::KeyRepeatAccelerator(Emitter emitter, bool useTimer)
    : emitter_(std::move(emitter)), useTimer_(useTimer) {}

KeyRepeatAccelerator::~KeyRepeatAccelerator() {
    stop();
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void KeyRepeatAccelerator::configure(bool enabled, int initialDelayMs,
                                     int intervalMs, int maxRepeats,
                                     int maxHoldMs) {
    enabled_ = enabled;
    initialDelayMs_ = (std::max)(50, initialDelayMs);
    intervalMs_ = (std::max)(10, intervalMs);
    maxRepeats_ = (std::max)(1, maxRepeats);
    maxHoldMs_ = (std::max)(initialDelayMs_ + intervalMs_, maxHoldMs);
    if (!enabled_) {
        stop();
    }
}

bool KeyRepeatAccelerator::IsInjectedEvent() {
    return ::GetMessageExtraInfo() ==
           static_cast<LPARAM>(kKeyRepeatInjectionMarker);
}

void KeyRepeatAccelerator::InjectKeyDown(WORD vk) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = 0;
    input.ki.dwExtraInfo = kKeyRepeatInjectionMarker;
    ::SendInput(1, &input, sizeof(input));
}

bool KeyRepeatAccelerator::IsEligibleKey(WORD vk, bool candidatesVisible) {
    // Letters are the primary target: while composing, every repeat extends
    // the pinyin buffer and refreshes the candidate list in one burst.
    if (vk >= L'A' && vk <= L'Z') {
        return true;
    }
    // Backspace deletes pinyin while composing and deletes document text when
    // there is no composition.  The backend returns false in the latter case,
    // so the host sees a normal Backspace.
    if (vk == VK_BACK) {
        return true;
    }
    // Everything else is only accelerated when the IME is not showing a
    // candidate list, so digits/space/punctuation can never accidentally
    // select candidates or trigger shortcuts.
    if (candidatesVisible) {
        return false;
    }
    if ((vk >= L'0' && vk <= L'9') || vk == VK_SPACE) {
        return true;
    }
    switch (vk) {
    case VK_OEM_1: // ;:
    case VK_OEM_2: // /?
    case VK_OEM_3: // `~
    case VK_OEM_4: // [{
    case VK_OEM_5: // \|
    case VK_OEM_6: // ]}
    case VK_OEM_7: // '"
    case VK_OEM_PLUS:
    case VK_OEM_COMMA:
    case VK_OEM_MINUS:
    case VK_OEM_PERIOD:
        return true;
    default:
        return false;
    }
}

bool KeyRepeatAccelerator::onKeyDown(WORD vk, unsigned repeatCount,
                                     bool ctrlAltWinDown,
                                     bool candidatesVisible, ULONGLONG now) {
    if (!enabled_) {
        return false;
    }
    if (ctrlAltWinDown) {
        stop();
        return false;
    }

    if (repeatCount > 1) {
        if (active_ && vk == key_) {
            // Consume the OS auto-repeat; the accelerator emits the burst at
            // its own (much faster) interval.  The first key-down was already
            // processed normally.
            return true;
        }
        return false;
    }

    if (active_) {
        if (vk == key_) {
            // Duplicate non-repeat key-down (e.g. a host that does not set
            // repeatCount).  Let it through but do not restart the timer.
            return false;
        }
        stop();
    }

    if (!IsEligibleKey(vk, candidatesVisible)) {
        return false;
    }

    active_ = true;
    key_ = vk;
    startTick_ = now;
    lastEmitTick_ = now;
    emitted_ = 0;
    startTimer();
    return false;
}

void KeyRepeatAccelerator::onKeyUp(WORD vk) {
    if (active_ && vk == key_) {
        stop();
    }
}

void KeyRepeatAccelerator::onFocusLost() {
    stop();
}

void KeyRepeatAccelerator::tick(ULONGLONG now) {
    if (!active_) {
        return;
    }
    if (now < startTick_) {
        startTick_ = now;
        lastEmitTick_ = now;
        return;
    }
    const ULONGLONG elapsed = now - startTick_;
    if (elapsed >= static_cast<ULONGLONG>(maxHoldMs_)) {
        stop();
        return;
    }
    if (elapsed < static_cast<ULONGLONG>(initialDelayMs_)) {
        return;
    }
    if (emitted_ >= maxRepeats_) {
        stop();
        return;
    }
    if (now - lastEmitTick_ < static_cast<ULONGLONG>(intervalMs_)) {
        return;
    }

    lastEmitTick_ = now;
    ++emitted_;
    if (emitter_) {
        emitter_(key_);
    }
}

void KeyRepeatAccelerator::stop() {
    stopTimer();
    active_ = false;
    key_ = 0;
    emitted_ = 0;
}

void KeyRepeatAccelerator::startTimer() {
    if (!useTimer_) {
        return;
    }
    if (!hwnd_) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &KeyRepeatAccelerator::TimerWndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = kTimerWindowClass;
        if (!::RegisterClassExW(&wc) &&
            ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return;
        }
        hwnd_ = ::CreateWindowExW(0, kTimerWindowClass, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr,
                                  ::GetModuleHandleW(nullptr), this);
        if (!hwnd_) {
            return;
        }
    }
    ::SetTimer(hwnd_, kTimerId, kTimerPeriodMs, nullptr);
}

void KeyRepeatAccelerator::stopTimer() {
    if (hwnd_ && useTimer_) {
        ::KillTimer(hwnd_, kTimerId);
    }
}

LRESULT CALLBACK KeyRepeatAccelerator::TimerWndProc(HWND hwnd, UINT msg,
                                                    WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<KeyRepeatAccelerator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<KeyRepeatAccelerator*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    return self ? self->timerWndProc(hwnd, msg, wp, lp)
                : ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT KeyRepeatAccelerator::timerWndProc(HWND hwnd, UINT msg, WPARAM wp,
                                           LPARAM lp) {
    if (msg == WM_TIMER && wp == kTimerId) {
        tick(::GetTickCount64());
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace Moqi
