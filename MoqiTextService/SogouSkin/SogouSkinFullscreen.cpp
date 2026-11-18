#include "SogouSkinFullscreen.h"
#include "SogouSkin.h"

#include <dwmapi.h>
#include <shellapi.h>

#include <cwchar>

namespace Moqi {
namespace SogouSkin {
namespace {

constexpr int kFullscreenTolerancePx = 2;
constexpr int kBusyFullscreenTolerancePx = 8;
constexpr ULONGLONG kVisibleSampleIntervalMs = 1000;
constexpr ULONGLONG kSuppressedSampleIntervalMs = 400;

// The status bar may live in a DPI-unaware or system-aware host process.
// GetWindowRect/GetMonitorInfo/DWM otherwise mix logical and physical pixels
// on mixed-DPI desktops, which breaks the frame-vs-monitor comparison.
class ScopedThreadDpiAwareness {
public:
    ScopedThreadDpiAwareness() {
        using SetThreadDpiAwarenessContextFn = HANDLE(WINAPI*)(HANDLE);
        static SetThreadDpiAwarenessContextFn setContext =
            reinterpret_cast<SetThreadDpiAwarenessContextFn>(
                ::GetProcAddress(::GetModuleHandleW(L"user32.dll"),
                                 "SetThreadDpiAwarenessContext"));
        if (setContext) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4.
            previous_ = setContext(
                reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
        }
    }

    ~ScopedThreadDpiAwareness() {
        if (!previous_) {
            return;
        }
        using SetThreadDpiAwarenessContextFn = HANDLE(WINAPI*)(HANDLE);
        static SetThreadDpiAwarenessContextFn setContext =
            reinterpret_cast<SetThreadDpiAwarenessContextFn>(
                ::GetProcAddress(::GetModuleHandleW(L"user32.dll"),
                                 "SetThreadDpiAwarenessContext"));
        if (setContext) {
            setContext(previous_);
        }
    }

    ScopedThreadDpiAwareness(const ScopedThreadDpiAwareness&) = delete;
    ScopedThreadDpiAwareness& operator=(const ScopedThreadDpiAwareness&) = delete;

private:
    HANDLE previous_ = nullptr;
};

bool WindowClassIs(HWND hwnd, const wchar_t* name) {
    wchar_t className[64] = {};
    return hwnd && name &&
           ::GetClassNameW(hwnd, className, _countof(className)) > 0 &&
           _wcsicmp(className, name) == 0;
}

bool IsShellWindow(HWND hwnd) {
    if (!hwnd) {
        return false;
    }
    if (hwnd == ::GetShellWindow() || hwnd == ::GetDesktopWindow()) {
        return true;
    }
    const HWND root = ::GetAncestor(hwnd, GA_ROOT);
    if (root && (root == ::GetShellWindow() || root == ::GetDesktopWindow())) {
        return true;
    }
    return WindowClassIs(hwnd, L"Progman") || WindowClassIs(hwnd, L"WorkerW") ||
           WindowClassIs(hwnd, L"Shell_TrayWnd") ||
           WindowClassIs(hwnd, L"Shell_SecondaryTrayWnd");
}

bool IsStatusBarWindow(HWND hwnd) {
    return WindowClassIs(hwnd, kStatusBarWindowClass);
}

bool IsWindowCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    const HRESULT hr = ::DwmGetWindowAttribute(
        hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return SUCCEEDED(hr) && cloaked != 0;
}

} // namespace

bool CoversMonitorRect(const RECT& frame, const RECT& monitor, int tolerancePx) {
    const int tolerance = tolerancePx < 0 ? 0 : tolerancePx;
    if (frame.right <= frame.left || frame.bottom <= frame.top ||
        monitor.right <= monitor.left || monitor.bottom <= monitor.top) {
        return false;
    }
    // Overhang is intentional: an Eyefinity/Surround window covers each
    // monitor it contains even though its frame extends past that monitor.
    return frame.left <= monitor.left + tolerance &&
           frame.top <= monitor.top + tolerance &&
           frame.right >= monitor.right - tolerance &&
           frame.bottom >= monitor.bottom - tolerance;
}

bool IsFullscreenWindowForMetrics(const FullscreenMetrics& metrics) {
    if (!metrics.visible || metrics.iconic || metrics.cloaked ||
        metrics.shellWindow) {
        return false;
    }

    // Exclusive-mode D3D is a strong signal, but QUNS can briefly keep
    // reporting it after the game loses the foreground.  Confirm that the
    // current foreground window is actually fullscreen-sized and is not a
    // normal maximized framed window before trusting it.
    if (metrics.exclusiveD3DFullscreen) {
        if (!metrics.frameValid) {
            return true;
        }
        if (!CoversMonitorRect(metrics.frame, metrics.monitor,
                               kBusyFullscreenTolerancePx)) {
            return false;
        }
        return !(metrics.maximized && metrics.hasCaption);
    }

    if (!metrics.frameValid || !metrics.monitorValid) {
        return false;
    }

    // QUNS_BUSY is a useful confirmation for borderless fullscreen windows,
    // but it also covers presentation settings; the frame still has to cover
    // the monitor.  The larger tolerance only absorbs DPI/rounding noise.
    const int tolerance = metrics.shellBusy ? kBusyFullscreenTolerancePx
                                            : kFullscreenTolerancePx;
    if (!CoversMonitorRect(metrics.frame, metrics.monitor, tolerance)) {
        return false;
    }

    // Critical false-positive guard: with an auto-hidden taskbar a normal
    // maximized framed window also covers the whole monitor.
    if (metrics.maximized && metrics.hasCaption) {
        return false;
    }

    return true;
}

bool SampleForegroundFullscreen(HWND foreground, HWND statusBar,
                                FullscreenScope scope,
                                FullscreenSample* sample) {
    if (!statusBar || !sample) {
        return false;
    }
    *sample = FullscreenSample{};
    sample->foreground = foreground;

    ScopedThreadDpiAwareness dpiAwareness;

    if (!foreground || !::IsWindow(foreground) ||
        !::IsWindowVisible(foreground) || ::IsIconic(foreground)) {
        return true;
    }
    if (IsShellWindow(foreground) || IsStatusBarWindow(foreground) ||
        IsWindowCloaked(foreground)) {
        return true;
    }

    QUERY_USER_NOTIFICATION_STATE notificationState =
        QUNS_ACCEPTS_NOTIFICATIONS;
    const HRESULT queryResult =
        ::SHQueryUserNotificationState(&notificationState);
    if (FAILED(queryResult)) {
        notificationState = QUNS_ACCEPTS_NOTIFICATIONS;
    }
    sample->notificationState = static_cast<int>(notificationState);
    sample->shellBusy = notificationState == QUNS_BUSY;
    sample->exclusiveD3DFullscreen =
        notificationState == QUNS_RUNNING_D3D_FULL_SCREEN;

    RECT frame = {};
    bool frameValid = false;
    const HRESULT dwmResult =
        ::DwmGetWindowAttribute(foreground, DWMWA_EXTENDED_FRAME_BOUNDS, &frame,
                                sizeof(frame));
    if (SUCCEEDED(dwmResult) && !::IsRectEmpty(&frame)) {
        frameValid = true;
    } else if (::GetWindowRect(foreground, &frame)) {
        frameValid = !::IsRectEmpty(&frame);
    }

    const HMONITOR monitor =
        ::MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!monitor || !::GetMonitorInfoW(monitor, &monitorInfo)) {
        return true;
    }

    const LONG_PTR style = ::GetWindowLongPtrW(foreground, GWL_STYLE);
    const LONG_PTR exStyle = ::GetWindowLongPtrW(foreground, GWL_EXSTYLE);

    FullscreenMetrics metrics = {};
    metrics.frame = frame;
    metrics.monitor = monitorInfo.rcMonitor;
    metrics.work = monitorInfo.rcWork;
    metrics.frameValid = frameValid;
    metrics.monitorValid = true;
    metrics.visible = true;
    metrics.maximized =
        ::IsZoomed(foreground) != FALSE || (style & WS_MAXIMIZE) != 0;
    metrics.hasCaption = (style & WS_CAPTION) == WS_CAPTION;
    metrics.hasThickFrame = (style & WS_THICKFRAME) != 0;
    metrics.popup = (style & WS_POPUP) != 0;
    metrics.topmost = (exStyle & WS_EX_TOPMOST) != 0;
    metrics.shellBusy = sample->shellBusy;
    metrics.exclusiveD3DFullscreen = sample->exclusiveD3DFullscreen;

    sample->frame = frame;
    sample->frameValid = frameValid;
    sample->monitorValid = true;
    sample->monitor = monitor;
    sample->monitorRect = monitorInfo.rcMonitor;
    sample->workRect = monitorInfo.rcWork;

    if (!IsFullscreenWindowForMetrics(metrics)) {
        return true;
    }
    sample->fullscreen = true;

    MONITORINFO barInfo = {};
    barInfo.cbSize = sizeof(barInfo);
    const HMONITOR barMonitor =
        ::MonitorFromWindow(statusBar, MONITOR_DEFAULTTONEAREST);
    if (!barMonitor || !::GetMonitorInfoW(barMonitor, &barInfo)) {
        sample->coversBarMonitor = scope == FullscreenScope::All;
        return true;
    }

    sample->coversBarMonitor =
        scope == FullscreenScope::All ||
        (frameValid &&
         CoversMonitorRect(frame, barInfo.rcMonitor,
                           sample->shellBusy ? kBusyFullscreenTolerancePx
                                             : kFullscreenTolerancePx)) ||
        (!frameValid && monitor == barMonitor);
    return true;
}

FullscreenSuppressionDebouncer::FullscreenSuppressionDebouncer(
    ULONGLONG exitDelayMs)
    : exitDelayMs_(exitDelayMs) {}

bool FullscreenSuppressionDebouncer::update(bool candidate, ULONGLONG now) {
    if (candidate) {
        active_ = true;
        clearPending_ = false;
        return true;
    }
    if (!active_) {
        return false;
    }
    if (!clearPending_) {
        clearPending_ = true;
        clearSince_ = now;
    } else if (now - clearSince_ >= exitDelayMs_) {
        active_ = false;
        clearPending_ = false;
    }
    return active_;
}

void FullscreenSuppressionDebouncer::reset() {
    active_ = false;
    clearPending_ = false;
    clearSince_ = 0;
}

void StatusBarFullscreenDetector::setOptions(bool enabled,
                                             FullscreenScope scope) {
    if (enabled != enabled_ || scope != scope_) {
        invalidate();
        // A scope/enabled change invalidates the previous decision; do not
        // carry the exit hysteresis across it.
        debouncer_.reset();
    }
    enabled_ = enabled;
    scope_ = scope;
    if (!enabled_) {
        debouncer_.reset();
    }
}

void StatusBarFullscreenDetector::invalidate() {
    lastForeground_ = nullptr;
    lastSampleAt_ = 0;
}

bool StatusBarFullscreenDetector::update(HWND foreground, HWND statusBar,
                                         ULONGLONG now, bool force) {
    if (!enabled_) {
        debouncer_.reset();
        lastSample_ = FullscreenSample{};
        return false;
    }

    const ULONGLONG sampleInterval =
        debouncer_.active() ? kSuppressedSampleIntervalMs
                            : kVisibleSampleIntervalMs;
    if (!force && foreground == lastForeground_ && lastSampleAt_ != 0 &&
        now - lastSampleAt_ < sampleInterval) {
        return debouncer_.active();
    }

    // The exit hysteresis only exists for state changes of the same
    // foreground window (Alt+Enter, QUNS_BUSY lingering).  A new foreground
    // window must be judged immediately, otherwise returning from a game to a
    // normal app would keep the bar hidden for an extra 400ms.
    if (foreground != lastForeground_) {
        debouncer_.reset();
    }

    FullscreenSample sample;
    if (!SampleForegroundFullscreen(foreground, statusBar, scope_, &sample)) {
        sample = FullscreenSample{};
    }
    lastSample_ = sample;
    lastForeground_ = foreground;
    lastSampleAt_ = now;

    const bool candidate = sample.fullscreen && sample.coversBarMonitor;
    return debouncer_.update(candidate, now);
}

} // namespace SogouSkin
} // namespace Moqi
