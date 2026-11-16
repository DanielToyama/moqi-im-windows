//
// Sogou-style long-press key repeat accelerator.
//
// Windows sends auto-repeat WM_KEYDOWN messages at the user's configured
// repeat rate.  Many users keep that rate low, so holding a letter or
// Backspace feels sluggish compared with Sogou's "one char, short pause, then
// a burst" behaviour.
//
// This module is a small, testable state machine.  TextService feeds it the
// first key down/up events; after `initialDelayMs` it emits synthetic key
// downs through SendInput until the key is released, focus is lost, or
// `maxRepeats` is reached.  Injected events carry `kKeyRepeatInjectionMarker`
// in dwExtraInfo so the TextService can tell them apart from real key presses
// (GetMessageExtraInfo() is checked while handling the TSF key event).
//
#pragma once

#include <windows.h>

#include <functional>

namespace Moqi {

// 'MQRK' - private marker stored in INPUT.ki.dwExtraInfo.  The TSF key sink
// runs on the thread that owns the focused message queue, so GetMessageExtraInfo
// still reports the marker when our own filterKeyDown sees the injected event.
constexpr ULONG_PTR kKeyRepeatInjectionMarker = 0x4D51524B;

class KeyRepeatAccelerator {
public:
    using Emitter = std::function<void(WORD)>;

    explicit KeyRepeatAccelerator(Emitter emitter = {}, bool useTimer = true);
    ~KeyRepeatAccelerator();

    KeyRepeatAccelerator(const KeyRepeatAccelerator&) = delete;
    KeyRepeatAccelerator& operator=(const KeyRepeatAccelerator&) = delete;

    void configure(bool enabled, int initialDelayMs = 420, int intervalMs = 25,
                   int maxRepeats = 60, int maxHoldMs = 6000);

    bool enabled() const { return enabled_; }
    bool active() const { return active_; }
    WORD activeKey() const { return key_; }
    int emittedCount() const { return emitted_; }

    // Returns true when the current physical key-down must be consumed (it is
    // an OS auto-repeat while the accelerator is already emitting).  The first
    // key-down and all injected key-downs return false so they are processed
    // normally by the IME backend.
    bool onKeyDown(WORD vk, unsigned repeatCount, bool ctrlAltWinDown,
                   bool candidatesVisible, ULONGLONG now);

    void onKeyUp(WORD vk);
    void onFocusLost();

    // Advances the state machine.  Public so the logic test can drive time
    // deterministically without a message loop; the internal timer calls it.
    void tick(ULONGLONG now);

    static bool IsInjectedEvent();
    // Injects one key-down carrying kKeyRepeatInjectionMarker.  We do not
    // synthesize a key-up: the user's real release still produces the physical
    // key-up, which avoids confusing an injected key-up with the release that
    // must stop the accelerator.  The marker lets TextService skip
    // re-arming while still forwarding the injected key to the backend.
    static void InjectKeyDown(WORD vk);

private:
    void startTimer();
    void stopTimer();
    void stop();
    static bool IsEligibleKey(WORD vk, bool candidatesVisible);

    static LRESULT CALLBACK TimerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT timerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    Emitter emitter_;
    bool useTimer_ = true;
    bool enabled_ = true;
    int initialDelayMs_ = 420;
    int intervalMs_ = 25;
    int maxRepeats_ = 60;
    int maxHoldMs_ = 6000;

    bool active_ = false;
    WORD key_ = 0;
    ULONGLONG startTick_ = 0;
    ULONGLONG lastEmitTick_ = 0;
    int emitted_ = 0;
    HWND hwnd_ = nullptr;
};

} // namespace Moqi
