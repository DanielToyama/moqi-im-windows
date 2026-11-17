#include "SogouSkin.h"
#include "DpiSupport.h"

#include <windowsx.h>

#include <algorithm>

namespace Moqi {
namespace SogouSkin {
namespace {

constexpr UINT_PTR kStateTimerId = 1;
constexpr UINT kStateTimerIntervalMs = 150;

bool WindowProcessId(HWND hwnd, DWORD* processId) {
    if (!hwnd || !processId) {
        return false;
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
        return false;
    }
    *processId = pid;
    return true;
}

struct ChildProcessSearch {
    DWORD processId = 0;
    bool found = false;
};

BOOL CALLBACK FindChildProcessProc(HWND hwnd, LPARAM param) {
    auto* search = reinterpret_cast<ChildProcessSearch*>(param);
    DWORD pid = 0;
    if (WindowProcessId(hwnd, &pid) && pid == search->processId) {
        search->found = true;
        return FALSE;
    }
    return TRUE;
}

// Direct window + parent/owner/root checks.  Separated from the descendant
// scan so the latter stays a last-resort fallback.
bool WindowOrAncestorsBelongToProcess(HWND hwnd, DWORD processId) {
    if (!hwnd || processId == 0) {
        return false;
    }

    DWORD pid = 0;
    if (WindowProcessId(hwnd, &pid) && pid == processId) {
        return true;
    }

    // GetParent() returns the owner for top-level popups; checking GW_OWNER
    // as well covers windows whose parent relationship is not exposed through
    // GetParent.
    HWND current = hwnd;
    for (int depth = 0; current && depth < 32; ++depth) {
        if (WindowProcessId(current, &pid) && pid == processId) {
            return true;
        }
        HWND next = ::GetParent(current);
        if (!next) {
            next = ::GetWindow(current, GW_OWNER);
        }
        if (!next || next == current) {
            break;
        }
        current = next;
    }

    HWND root = ::GetAncestor(hwnd, GA_ROOT);
    if (root && root != hwnd && WindowProcessId(root, &pid) && pid == processId) {
        return true;
    }
    HWND rootOwner = ::GetAncestor(hwnd, GA_ROOTOWNER);
    if (rootOwner && rootOwner != hwnd && WindowProcessId(rootOwner, &pid) &&
        pid == processId) {
        return true;
    }
    return false;
}

bool IsHiddenItem(const Options& options, const std::wstring& name) {
    for (const auto& hidden : options.hiddenStatusItems) {
        if (_wcsicmp(hidden.c_str(), name.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

bool WindowBelongsToProcess(HWND hwnd, DWORD processId) {
    if (WindowOrAncestorsBelongToProcess(hwnd, processId)) {
        return true;
    }
    if (!hwnd || processId == 0) {
        return false;
    }

    // If GetForegroundWindow() is the host application's top-level window
    // while TSF runs in a child process (browser renderers, WebView2, some
    // Electron layouts), the keyboard focus sits on a child HWND owned by
    // that process.  Checking the focused child first is more precise than
    // scanning every descendant, especially with multiple renderer children.
    const DWORD threadId = ::GetWindowThreadProcessId(hwnd, nullptr);
    if (threadId != 0) {
        GUITHREADINFO info = {};
        info.cbSize = sizeof(info);
        if (::GetGUIThreadInfo(threadId, &info) && info.hwndFocus &&
            info.hwndFocus != hwnd &&
            WindowOrAncestorsBelongToProcess(info.hwndFocus, processId)) {
            return true;
        }
    }

    // Browser/Electron/WebView2 hosts can put the actual TSF-enabled
    // content window in a child process.  EnumChildWindows walks the whole
    // descendant tree, so a foreground host window is considered ours when
    // any hosted child belongs to this process.  Only run this relatively
    // expensive check when the direct/ancestor/focus checks did not match;
    // this is the path that runs at most once before a stale bar hides
    // itself.
    ChildProcessSearch search;
    search.processId = processId;
    ::EnumChildWindows(hwnd, &FindChildProcessProc,
                       reinterpret_cast<LPARAM>(&search));
    return search.found;
}

bool IsForegroundProcess(DWORD processId) {
    return WindowBelongsToProcess(::GetForegroundWindow(), processId);
}

void HideOtherStatusBarWindows(HWND self, const wchar_t* className) {
    if (!className || !*className) {
        return;
    }
    struct HideContext {
        HWND self;
        const wchar_t* className;
    } context = {self, className};
    ::EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            const auto* context = reinterpret_cast<const HideContext*>(param);
            if (hwnd == context->self) {
                return TRUE;
            }
            wchar_t windowClass[64] = {};
            if (::GetClassNameW(hwnd, windowClass, _countof(windowClass)) > 0 &&
                _wcsicmp(windowClass, context->className) == 0 &&
                ::IsWindowVisible(hwnd)) {
                // SetWindowPos works across process boundaries for these
                // non-activating layered tool windows; ShowWindow can fail
                // when the owner runs at a different integrity level.
                ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                                   SWP_NOACTIVATE | SWP_HIDEWINDOW);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));
}

StatusBarForegroundPolicy::StatusBarForegroundPolicy(DWORD ownerProcessId,
                                                     ULONGLONG pendingGraceMs)
    : ownerProcessId_(ownerProcessId), pendingGraceMs_(pendingGraceMs) {}

StatusBarForegroundPolicy::Action StatusBarForegroundPolicy::requestShow(
    HWND foreground, ULONGLONG now) {
    showRequested_ = true;
    if (WindowBelongsToProcess(foreground, ownerProcessId_)) {
        pendingDeadline_ = 0;
        return Action::Show;
    }
    // TSF can deliver OnSetFocus/OnActivate just before Windows finishes the
    // foreground transition.  Poll briefly instead of either showing a stale
    // window or losing the focus event entirely.
    pendingDeadline_ = now + pendingGraceMs_;
    return Action::None;
}

StatusBarForegroundPolicy::Action StatusBarForegroundPolicy::evaluate(
    HWND foreground, bool visible, ULONGLONG now) {
    if (visible) {
        if (WindowBelongsToProcess(foreground, ownerProcessId_)) {
            return Action::None;
        }
        showRequested_ = false;
        pendingDeadline_ = 0;
        return Action::Hide;
    }
    if (!showRequested_ || pendingDeadline_ == 0) {
        // A bar that was successfully shown keeps pendingDeadline_ == 0.
        // If another status bar subsequently hides it (same-process TSF
        // threads), do not auto-show again: that would make two windows
        // fight and ping-pong every timer tick.  Re-showing after a focus
        // loss is driven by the TSF OnSetFocus/OnActivate callback instead.
        showRequested_ = false;
        pendingDeadline_ = 0;
        return Action::StopPolling;
    }
    if (WindowBelongsToProcess(foreground, ownerProcessId_)) {
        pendingDeadline_ = 0;
        return Action::Show;
    }
    if (pendingDeadline_ != 0 && now >= pendingDeadline_) {
        showRequested_ = false;
        pendingDeadline_ = 0;
        return Action::StopPolling;
    }
    return Action::None;
}

void StatusBarForegroundPolicy::clear() {
    showRequested_ = false;
    pendingDeadline_ = 0;
}

StatusBarWindow::StatusBarWindow(std::shared_ptr<Theme> theme,
                                 ItemClickHandler onItemClick,
                                 ItemStateProvider stateProvider,
                                 std::wstring windowClassName)
    : theme_(std::move(theme)),
      windowClassName_(std::move(windowClassName)),
      onItemClick_(std::move(onItemClick)),
      stateProvider_(std::move(stateProvider)) {}

StatusBarWindow::~StatusBarWindow() {
    if (hwnd_) {
        stopStateTimer();
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool StatusBarWindow::create() {
    if (!theme_ || theme_->statusBar().pic.empty() || windowClassName_.empty()) {
        return false;
    }
    auto bar = theme_->image(theme_->statusBar().pic);
    if (!bar) {
        return false;
    }
    baseWidth_ = static_cast<int>(bar->GetWidth());
    baseHeight_ = static_cast<int>(bar->GetHeight());
    if (baseWidth_ <= 0 || baseHeight_ <= 0) {
        return false;
    }
    width_ = baseWidth_;
    height_ = baseHeight_;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &StatusBarWindow::staticWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = windowClassName_.c_str();
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    hwnd_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        windowClassName_.c_str(), L"Moqi Sogou Skin", WS_POPUP,
        0, 0, width_, height_, nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        return false;
    }
    dpi_ = static_cast<int>(WindowDpi(hwnd_));
    updateSizeForDpi();
    // Create hidden.  The owner (TextService) calls show() only after its TSF
    // focus event proves that this process owns the foreground; the timer then
    // keeps validating that ownership.
    positionAtCorner();
    refresh();
    return true;
}

void StatusBarWindow::show() {
    if (!hwnd_) {
        return;
    }
    const auto action = foregroundPolicy_.requestShow(
        ::GetForegroundWindow(), ::GetTickCount64());
    if (action == StatusBarForegroundPolicy::Action::Show) {
        showNow();
        return;
    }
    // The foreground transition may not be visible to GetForegroundWindow()
    // yet.  Stay hidden and let the timer finish the election shortly after.
    if (::IsWindowVisible(hwnd_)) {
        ::ShowWindow(hwnd_, SW_HIDE);
        if (onVisibilityChanged_) {
            onVisibilityChanged_(false);
        }
    }
    ensureStateTimer();
}

void StatusBarWindow::hide() {
    foregroundPolicy_.clear();
    stopStateTimer();
    if (hwnd_) {
        const bool wasVisible = ::IsWindowVisible(hwnd_) != FALSE;
        ::ShowWindow(hwnd_, SW_HIDE);
        if (wasVisible && onVisibilityChanged_) {
            onVisibilityChanged_(false);
        }
    }
}

void StatusBarWindow::checkForegroundNow() {
    if (!hwnd_) {
        return;
    }
    const auto action = foregroundPolicy_.evaluate(
        ::GetForegroundWindow(), isVisible(), ::GetTickCount64());
    if (action == StatusBarForegroundPolicy::Action::Hide) {
        hide();
    } else if (action == StatusBarForegroundPolicy::Action::Show) {
        showNow();
    } else if (action == StatusBarForegroundPolicy::Action::StopPolling &&
               !isVisible()) {
        stopStateTimer();
    }
}

void StatusBarWindow::showNow() {
    if (!hwnd_) {
        return;
    }
    HideOtherStatusBarWindows(hwnd_, windowClassName_.c_str());
    const bool wasVisible = ::IsWindowVisible(hwnd_) != FALSE;
    if (!wasVisible) {
        refresh();
        ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
    ::SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ensureStateTimer();
    if (!wasVisible && onVisibilityChanged_) {
        onVisibilityChanged_(true);
    }
}

void StatusBarWindow::ensureStateTimer() {
    if (hwnd_ && !stateTimer_) {
        stateTimer_ = ::SetTimer(hwnd_, kStateTimerId, kStateTimerIntervalMs, nullptr);
    }
}

void StatusBarWindow::stopStateTimer() {
    if (hwnd_ && stateTimer_) {
        ::KillTimer(hwnd_, kStateTimerId);
    }
    stateTimer_ = 0;
}

void StatusBarWindow::refresh() {
    if (!hwnd_ || !theme_) {
        return;
    }
    auto bar = theme_->image(theme_->statusBar().pic);
    if (!bar) {
        return;
    }
    auto bitmap = std::make_shared<Gdiplus::Bitmap>(
        width_, height_, PixelFormat32bppPARGB);
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
        return;
    }
    Gdiplus::Graphics graphics(bitmap.get());
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.DrawImage(bar.get(), 0, 0, width_, height_);
    const auto options = LoadOptions();
    const auto& items = theme_->statusBar().items;
    lastItemStates_.assign(items.size(), -1);
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const auto& item = items[i];
        if (!item.display || IsHiddenItem(options, item.name)) {
            continue;
        }
        const bool pressed = pressedIndex_ == i;
        const bool hovered = hoverIndex_ == i;
        const auto& names = pressed && !item.down.empty()
                                ? item.down
                                : (hovered && !item.hover.empty() ? item.hover : item.normal);
        if (names.empty()) {
            continue;
        }
        int stateIndex = 0;
        if (stateProvider_) {
            stateIndex = stateProvider_(item.name);
        }
        if (stateIndex < 0 || stateIndex >= static_cast<int>(names.size())) {
            stateIndex = 0;
        }
        lastItemStates_[i] = stateIndex;
        if (auto image = theme_->image(names[stateIndex])) {
            graphics.DrawImage(image.get(), scaleValue(item.pos.x),
                               scaleValue(item.pos.y),
                               scaleValue(static_cast<int>(image->GetWidth())),
                               scaleValue(static_cast<int>(image->GetHeight())));
        }
    }
    SogouSkin::UpdateLayeredWindowBitmap(hwnd_, bitmap.get());
}

bool StatusBarWindow::refreshIfStateChanged() {
    if (!hwnd_) {
        return false;
    }
    std::vector<int> states;
    if (!collectItemStates(&states) || states == lastItemStates_) {
        return false;
    }
    lastItemStates_ = std::move(states);
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

bool StatusBarWindow::collectItemStates(std::vector<int>* states) const {
    if (!theme_ || !stateProvider_) {
        return false;
    }
    const auto options = LoadOptions();
    const auto& items = theme_->statusBar().items;
    states->assign(items.size(), -1);
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const auto& item = items[i];
        if (!item.display || IsHiddenItem(options, item.name) || item.normal.empty()) {
            continue;
        }
        int stateIndex = stateProvider_(item.name);
        if (stateIndex < 0 || stateIndex >= static_cast<int>(item.normal.size())) {
            stateIndex = 0;
        }
        (*states)[i] = stateIndex;
    }
    return true;
}

bool StatusBarWindow::setTheme(std::shared_ptr<Theme> theme) {
    if (!theme) {
        return false;
    }
    auto bar = theme->image(theme->statusBar().pic);
    if (!bar) {
        return false;
    }
    const int newWidth = static_cast<int>(bar->GetWidth());
    const int newHeight = static_cast<int>(bar->GetHeight());
    if (newWidth <= 0 || newHeight <= 0) {
        return false;
    }
    theme_ = std::move(theme);
    if (newWidth != baseWidth_ || newHeight != baseHeight_) {
        baseWidth_ = newWidth;
        baseHeight_ = newHeight;
        updateSizeForDpi();
    }
    hoverIndex_ = -1;
    pressedIndex_ = -1;
    refresh();
    return true;
}

void StatusBarWindow::updateSizeForDpi() {
    width_ = scaleValue(baseWidth_);
    height_ = scaleValue(baseHeight_);
    if (width_ <= 0 || height_ <= 0) {
        return;
    }
    if (hwnd_) {
        ::SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, width_, height_,
                       SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
    }
}

int StatusBarWindow::scaleValue(int value) const {
    return ::MulDiv(value, dpi_, 96);
}

void StatusBarWindow::positionAtCorner() {
    if (!hwnd_) {
        return;
    }
    RECT work = {};
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    HMONITOR monitor = ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
    if (monitor && ::GetMonitorInfoW(monitor, &monitorInfo)) {
        work = monitorInfo.rcWork;
    } else {
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    }
    const auto options = LoadOptions();
    const int margin = scaleValue(20);
    const int x = options.statusbarX >= 0
                      ? options.statusbarX
                      : std::max(0, static_cast<int>(work.right) - width_ - margin);
    const int y = options.statusbarY >= 0
                      ? options.statusbarY
                      : std::max(0, static_cast<int>(work.bottom) - height_ - margin);
    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_,
                   SWP_NOACTIVATE);
}

int StatusBarWindow::hitTest(POINT pt) const {
    if (!theme_) {
        return -1;
    }
    const auto options = LoadOptions();
    const auto& items = theme_->statusBar().items;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const auto& item = items[i];
        if (!item.display || item.normal.empty() ||
            IsHiddenItem(options, item.name)) {
            continue;
        }
        auto image = theme_->image(item.normal[0]);
        if (!image) {
            continue;
        }
        RECT rect = {scaleValue(item.pos.x), scaleValue(item.pos.y),
                     scaleValue(item.pos.x + static_cast<int>(image->GetWidth())),
                     scaleValue(item.pos.y + static_cast<int>(image->GetHeight()))};
        if (::PtInRect(&rect, pt)) {
            return i;
        }
    }
    return -1;
}

void StatusBarWindow::paint() {
    PAINTSTRUCT ps = {};
    ::BeginPaint(hwnd_, &ps);
    refresh();
    ::EndPaint(hwnd_, &ps);
}

LRESULT CALLBACK StatusBarWindow::staticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    StatusBarWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<StatusBarWindow*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<StatusBarWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->wndProc(msg, wp, lp) : ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT StatusBarWindow::wndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOQI_SOGOU_SETTINGS_CHANGED:
        if (onSettingsChanged_) {
            onSettingsChanged_();
        }
        return 0;
    case WM_TIMER:
        if (wp == kStateTimerId) {
            // The foreground check is the authoritative single-instance
            // election.  A TSF host may be deactivated without another host
            // creating its own bar (UWP, Electron subprocesses, unusual TSF
            // lifecycles), so hiding only when a new bar appears leaves the
            // old application's window on screen.
            const auto action = foregroundPolicy_.evaluate(
                ::GetForegroundWindow(), isVisible(), ::GetTickCount64());
            if (action == StatusBarForegroundPolicy::Action::Hide) {
                hide();
                return 0;
            }
            if (action == StatusBarForegroundPolicy::Action::StopPolling) {
                stopStateTimer();
                return 0;
            }
            if (action == StatusBarForegroundPolicy::Action::Show) {
                showNow();
            }
            if (!isVisible()) {
                // A pending show keeps polling for its short grace period;
                // otherwise a hidden window has no reason to keep a timer.
                if (!foregroundPolicy_.showRequested()) {
                    stopStateTimer();
                }
                return 0;
            }
            // Key shortcuts can toggle Chinese/English, full/half width, etc.
            // without any RPC notification.  Poll the button state so the
            // skin icon follows immediately even when the backend does not
            // send a changeButton message.
            std::vector<int> states;
            if (collectItemStates(&states) && states != lastItemStates_) {
                lastItemStates_ = std::move(states);
                ::InvalidateRect(hwnd_, nullptr, TRUE);
            }
        }
        return 0;
    case WM_PAINT:
        paint();
        return 0;
    case WM_DPICHANGED: {
        const UINT newDpi = HIWORD(wp);
        if (newDpi > 0) {
            dpi_ = static_cast<int>(newDpi);
            updateSizeForDpi();
            positionAtCorner();
            refresh();
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return TRUE;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_RBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ::ClientToScreen(hwnd_, &pt);
        if (onContextMenu_) {
            onContextMenu_(pt);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd_;
            if (::TrackMouseEvent(&tme)) {
                trackingMouse_ = true;
            }
        }
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int hit = hitTest(pt);
        if (hit != hoverIndex_) {
            hoverIndex_ = hit;
            ::InvalidateRect(hwnd_, nullptr, TRUE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        if (hoverIndex_ != -1) {
            hoverIndex_ = -1;
            ::InvalidateRect(hwnd_, nullptr, TRUE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int hit = hitTest(pt);
        if (hit >= 0) {
            pressedIndex_ = hit;
            ::SetCapture(hwnd_);
            ::InvalidateRect(hwnd_, nullptr, TRUE);
        } else {
            pressedIndex_ = -1;
            ::ReleaseCapture();
            ::SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (::GetCapture() == hwnd_) {
            ::ReleaseCapture();
        }
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int hit = hitTest(pt);
        if (pressedIndex_ >= 0 && hit == pressedIndex_ && onItemClick_) {
            const auto& items = theme_->statusBar().items;
            if (pressedIndex_ < static_cast<int>(items.size())) {
                onItemClick_(items[pressedIndex_].name);
            }
        }
        pressedIndex_ = -1;
        ::InvalidateRect(hwnd_, nullptr, TRUE);
        return 0;
    }
    default:
        return ::DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

} // namespace SogouSkin
} // namespace Moqi
