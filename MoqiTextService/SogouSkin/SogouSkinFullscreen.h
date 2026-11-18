//
// Fullscreen detection for the Sogou-skin status bar.
//
// The status bar is only ever shown while the TSF owner process owns the
// foreground window, so "should this bar be hidden because the user is in a
// fullscreen app" is a separate question from "is the foreground ours".
// This file keeps the geometry/style classifier pure (unit-testable) and the
// Win32 sampling/debouncing separate.
//
#pragma once

#include <windows.h>
#include <shellapi.h>

namespace Moqi {
namespace SogouSkin {

// Monitor means: hide only when the fullscreen window covers the monitor the
// status bar lives on.  All means: hide on every monitor.
enum class FullscreenScope {
    Monitor,
    All,
};

// Snapshot of the foreground window used by the pure classifier.  All RECTs
// are in physical screen pixels; callers that run in a DPI-unaware host should
// take the snapshot through SampleForegroundFullscreen(), which temporarily
// switches the calling thread to per-monitor-v2 awareness.
struct FullscreenMetrics {
    RECT frame = {};   // DWMWA_EXTENDED_FRAME_BOUNDS, or GetWindowRect fallback
    RECT monitor = {}; // rcMonitor of the foreground window's monitor
    RECT work = {};    // rcWork of that monitor
    bool frameValid = false;
    bool monitorValid = false;
    bool visible = false;
    bool iconic = false;
    bool cloaked = false;
    bool shellWindow = false;
    bool maximized = false;
    bool hasCaption = false;
    bool hasThickFrame = false;
    bool popup = false;
    bool topmost = false;
    bool shellBusy = false;             // QUNS_BUSY
    bool exclusiveD3DFullscreen = false; // QUNS_RUNNING_D3D_FULL_SCREEN
};

// True when `frame` covers `monitor` within `tolerancePx` on every edge.
// Overhang is allowed so a window spanning multiple monitors (Surround) still
// covers each monitor it contains.
bool CoversMonitorRect(const RECT& frame, const RECT& monitor, int tolerancePx);

// Pure classifier.  The important negative case is a normal maximized framed
// window while the taskbar is auto-hidden: its DWM frame equals rcMonitor, so
// geometry alone would call it fullscreen.  IsZoomed + WS_CAPTION excludes it.
bool IsFullscreenWindowForMetrics(const FullscreenMetrics& metrics);

struct FullscreenSample {
    bool fullscreen = false;
    bool coversBarMonitor = false;
    bool frameValid = false;
    bool monitorValid = false;
    bool exclusiveD3DFullscreen = false;
    bool shellBusy = false;
    int notificationState = static_cast<int>(QUNS_ACCEPTS_NOTIFICATIONS);
    HWND foreground = nullptr;
    HMONITOR monitor = nullptr;
    RECT frame = {};
    RECT monitorRect = {};
    RECT workRect = {};
};

// Samples the foreground window and fills `sample`.  Returns false only for
// invalid arguments (e.g. no status bar window); a non-fullscreen foreground
// is a successful sample with sample.fullscreen == false.
bool SampleForegroundFullscreen(HWND foreground, HWND statusBar,
                                FullscreenScope scope,
                                FullscreenSample* sample);

// Time-based exit hysteresis.  Entering suppression is immediate; leaving it
// requires `exitDelayMs` of continuous non-fullscreen samples, because
// QUNS_BUSY can linger briefly after a fullscreen window closes.
class FullscreenSuppressionDebouncer {
public:
    explicit FullscreenSuppressionDebouncer(ULONGLONG exitDelayMs = 400);

    bool update(bool candidate, ULONGLONG now);
    void reset();
    bool active() const { return active_; }

private:
    ULONGLONG exitDelayMs_;
    bool active_ = false;
    bool clearPending_ = false;
    ULONGLONG clearSince_ = 0;
};

// Owns the sampling cache and the suppression hysteresis for one status bar.
class StatusBarFullscreenDetector {
public:
    void setOptions(bool enabled, FullscreenScope scope);
    void invalidate();
    bool update(HWND foreground, HWND statusBar, ULONGLONG now, bool force);
    bool active() const { return debouncer_.active(); }
    const FullscreenSample& lastSample() const { return lastSample_; }

private:
    bool enabled_ = true;
    FullscreenScope scope_ = FullscreenScope::Monitor;
    FullscreenSuppressionDebouncer debouncer_;
    HWND lastForeground_ = nullptr;
    ULONGLONG lastSampleAt_ = 0;
    FullscreenSample lastSample_ = {};
};

} // namespace SogouSkin
} // namespace Moqi
