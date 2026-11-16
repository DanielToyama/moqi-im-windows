//
// Sogou skin compatibility layer for Moqi.
//
// This module is intentionally self-contained: it only reads a Sogou skin
// (extracted directory or .ssf ZIP) and paints an already-computed candidate
// window into an HDC.  It does not touch input state, protocols, or the TSF
// service; CandidateWindow can simply ignore it when no skin is configured.
//
#pragma once

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Moqi {
namespace SogouSkin {

struct Layout {
    int mode = 0;
    int fixed1 = 0;
    int fixed2 = 0;
};

struct Component {
    std::wstring pic;
    std::wstring mask;
    Layout layoutH;
    Layout layoutV;
    int margin[4] = {0, 0, 0, 0}; // top, bottom, left, right
};

struct Scheme {
    std::wstring name;
    bool split = false;
    Component single;
    Component pinyin;
    Component zhongwen;
    int pinyinMargin[4] = {0, 0, 0, 0};   // top, bottom/sep-gap, left, right
    int zhongwenMargin[4] = {0, 0, 0, 0};
    bool hasSeparator = false;
    COLORREF separatorColor = RGB(216, 216, 216);
    int separatorLeft = 1;
    int separatorRight = 1;
    bool hasAnchor = false;
    int anchorX = 0;
    int anchorY = 0;
};

struct Display {
    // Skins that do not specify [Display] font_ch used to fall back to SimSun
    // (宋体), which looks dated and has poor ClearType coverage.  A sans-serif
    // Chinese face matches the rest of the Windows 10/11 UI.  A skin's own
    // font_ch/font_en values still win (see loadIni()).
    std::wstring fontCh = L"微软雅黑";
    std::wstring fontEn = L"Arial";
    int fontSize = 16;
    COLORREF pinyinColor = RGB(0, 0, 0);
    COLORREF firstColor = RGB(0, 0, 0);
    COLORREF textColor = RGB(0, 0, 0);
    COLORREF commentColor = RGB(0, 0, 0);
    bool useGdip = true;
    bool aero = false;
    bool glow = false;
};

// Optional [Render] overrides (usually from
// %APPDATA%\Moqi\sogou_skin_overrides.ini).  The defaults preserve the
// historical hard-coded behaviour when no override file exists.
struct RenderSettings {
    enum class PinyinSeparator {
        Auto,       // tone-marked preedit keeps spaces, plain pinyin uses '
        Apostrophe, // always use '
        Space,      // always use spaces
    };

    int keyGap = 1;
    int itemSpacing = 11;
    bool pinyinBold = true;
    int textOffsetY = 0;
    int candidateRowHeight = 0; // 0 = automatic (font height + padding)
    PinyinSeparator pinyinSeparator = PinyinSeparator::Auto;
};

struct RenderState {
    std::wstring preedit;
    std::vector<std::wstring> candidates;
    std::wstring selKeys;
    int currentSel = 0;
    int candPerRow = 1;
    bool useCursor = true;
    COLORREF backgroundColor = RGB(255, 255, 255);
    COLORREF highlightColor = RGB(198, 221, 249);
    COLORREF highlightTextColor = RGB(0, 0, 0);
};

struct StatusItem {
    std::wstring name;
    bool display = true;
    POINT pos = {0, 0};
    std::vector<std::wstring> normal;
    std::vector<std::wstring> hover;
    std::vector<std::wstring> down;
};

struct StatusBar {
    std::wstring pic;
    std::wstring mask;
    std::vector<StatusItem> items;
};

struct LayoutResult {
    int width = 0;
    int height = 0;
    double scale = 1.0;
    std::vector<RECT> candidateRects;
    RECT preeditRect = {0, 0, 0, 0};
    // Skin anchor point, already multiplied by the global scale.
    POINT anchor = {0, 0};
};

class Theme;

// Values owned by the native settings UI.  BuildSettingsOverridesText merges
// them with any manual advanced keys already present in the override file and
// strips layout values produced by the old per-metric font scaling.
struct SettingsOverrides {
    int fontSize = 16;
    // Empty means "keep the skin's own font_ch/font_en".
    std::wstring fontCh;
    std::wstring fontEn;
    COLORREF pinyinColor = RGB(0, 0, 0);
    COLORREF firstColor = RGB(0, 0, 0);
    COLORREF textColor = RGB(0, 0, 0);
    COLORREF commentColor = RGB(0, 0, 0);
    std::vector<std::pair<std::wstring, bool>> statusItems;
};

// Rewrites the override INI for the settings UI.  Existing files written by
// the old UI (no [MoqiOverrides] marker) are migrated by dropping layout keys
// that exactly match base value * font-size ratio.  User-edited advanced keys
// are preserved.  Returns the complete UTF-16 text (without BOM).
std::wstring BuildSettingsOverridesText(const Theme& baseTheme,
                                        const std::wstring& existingPath,
                                        const SettingsOverrides& values,
                                        int* removedLegacyKeys = nullptr);

struct Options {
    bool statusbar = true;
    bool horizontal = false;
    // Long-press key auto-repeat accelerator (letters/backspace).  Read from
    // %APPDATA%\Moqi\sogou_skin_options.ini key_repeat=0/1.
    bool keyRepeat = true;
    std::vector<std::wstring> hiddenStatusItems;
    int statusbarX = -1;
    int statusbarY = -1;
};

Options LoadOptions();

// Paths shared by the IME and the standalone settings UI.
std::wstring ConfigDirectory();
std::wstring OverridesPath();
std::wstring OptionsPath();
std::wstring InstalledSkinPath();

// Broadcast after the settings UI writes skin files.  Every live status bar
// window receives it and asks its TextService to reload the skin in place.
constexpr UINT WM_MOQI_SOGOU_SETTINGS_CHANGED = WM_APP + 0x4D51;

// Default top-level window class shared by every TSF host process.  Status
// bar election and cross-process hiding rely on this name staying identical.
inline constexpr wchar_t kStatusBarWindowClass[] = L"MoqiSogouSkinStatusBar";

// Hides every status bar top-level window of `className` except `self`.
// Passing nullptr for `self` hides all of them.  This is a best-effort
// cross-process supplement: each status bar also verifies foreground
// ownership on its own timer, which remains authoritative when
// ShowWindow/SetWindowPos is denied by integrity level boundaries.
void HideOtherStatusBarWindows(
    HWND self, const wchar_t* className = kStatusBarWindowClass);

// Returns true when `hwnd` belongs to `processId`, either directly, through
// its parent/owner chain, or because a descendant window is hosted by that
// process (WebView2/Electron-style cross-process hosting).  The descendant
// check is what keeps the status bar visible when TSF runs in a child process
// while GetForegroundWindow() returns the host application's top-level window.
bool WindowBelongsToProcess(HWND hwnd, DWORD processId);

// Convenience wrapper around WindowBelongsToProcess(GetForegroundWindow()).
// Returns false when there is no foreground window (desktop, lock screen,
// secure desktop).
bool IsForegroundProcess(DWORD processId);

// Pure state machine behind StatusBarWindow's foreground check.  Exposed so
// the show/hide/pending transitions can be tested deterministically with
// synthetic windows and timestamps, without a visible status bar.
class StatusBarForegroundPolicy {
public:
    enum class Action {
        None,        // no visibility change
        Show,        // make the window visible
        Hide,        // hide it and stop the timer
        StopPolling, // stay hidden and stop the timer
    };

    explicit StatusBarForegroundPolicy(
        DWORD ownerProcessId = ::GetCurrentProcessId(),
        ULONGLONG pendingGraceMs = 1000);

    DWORD ownerProcessId() const { return ownerProcessId_; }
    bool showRequested() const { return showRequested_; }
    bool pending() const { return showRequested_ && pendingDeadline_ != 0; }

    // Records an explicit show request (TSF OnSetFocus/OnActivate).  If the
    // foreground window already belongs to this process the caller should show
    // immediately; otherwise the request stays pending for a short grace
    // period because TSF can deliver focus before SetForegroundWindow settles.
    Action requestShow(HWND foreground, ULONGLONG now);

    // Called from the status bar timer.  `visible` is the current window
    // visibility.  Returns Hide as soon as the foreground belongs to another
    // process, and StopPolling once a pending show has expired.
    Action evaluate(HWND foreground, bool visible, ULONGLONG now);

    void clear();

private:
    DWORD ownerProcessId_;
    ULONGLONG pendingGraceMs_;
    bool showRequested_ = false;
    ULONGLONG pendingDeadline_ = 0;
};

class Theme;

std::shared_ptr<Theme> LoadPreferredTheme(std::wstring* error,
                                          std::wstring* loadedPath = nullptr);

// Loads a skin while applying `overridesPath` instead of the default
// %APPDATA%\Moqi\sogou_skin_overrides.ini.  An empty path disables overrides,
// which the settings UI uses to obtain the skin's factory defaults.
std::shared_ptr<Theme> LoadThemeWithOverrides(const std::wstring& path,
                                              const std::wstring& overridesPath,
                                              std::wstring* error);

bool UpdateLayeredWindowBitmap(HWND hwnd, Gdiplus::Bitmap* bitmap);

class Theme {
public:
    static std::shared_ptr<Theme> Load(const std::wstring& path, std::wstring* error);
    static std::shared_ptr<Theme> LoadWithOverrides(const std::wstring& path,
                                                   const std::wstring& overridesPath,
                                                   std::wstring* error);
    ~Theme();

    bool valid() const { return valid_; }
    bool isArchive() const { return !zipBytes_.empty(); }
    const Display& display() const { return display_; }
    // [Display] font_size from the skin itself, before override merging.
    int baseFontSize() const { return baseFontSize_; }
    const RenderSettings& render() const { return render_; }
    const Scheme* scheme(const std::wstring& name) const;
    const Scheme* chooseScheme(bool horizontal) const;
    const StatusBar& statusBar() const { return statusBar_; }
    std::vector<std::wstring> schemeNames() const;

    std::shared_ptr<Gdiplus::Bitmap> image(const std::wstring& name) const;
    std::shared_ptr<Gdiplus::Bitmap> mask(const std::wstring& name) const;

private:
    Theme() = default;
    static std::shared_ptr<Theme> LoadInternal(const std::wstring& path,
                                               const std::wstring& overridesPath,
                                               std::wstring* error);
    bool loadFromDirectory(const std::wstring& dir, std::wstring* error);
    bool loadFromArchive(const std::wstring& file, std::wstring* error);
    bool loadIni(const std::wstring& text, std::wstring* error);
    void parseScheme(const std::map<std::wstring, std::wstring>& section,
                     const std::wstring& name, Scheme* scheme);

    bool valid_ = false;
    bool gdiplusStarted_ = false;
    int baseFontSize_ = 16;
    Display display_;
    RenderSettings render_;
    std::map<std::wstring, Scheme> schemes_;
    StatusBar statusBar_;
    std::wstring baseDir_;
    std::wstring overridesPath_;
    std::vector<unsigned char> zipBytes_;
    std::map<std::wstring, std::vector<unsigned char>> zipEntries_;
    mutable std::map<std::wstring, std::shared_ptr<Gdiplus::Bitmap>> imageCache_;
    mutable std::map<std::wstring, std::shared_ptr<Gdiplus::Bitmap>> maskCache_;
};

class StatusBarWindow {
public:
    using ItemClickHandler = std::function<void(const std::wstring& itemName)>;
    using ItemStateProvider = std::function<int(const std::wstring& itemName)>;
    using ContextMenuHandler = std::function<void(POINT screenPoint)>;
    using SettingsChangedHandler = std::function<void()>;
    using VisibilityChangedHandler = std::function<void(bool visible)>;

    explicit StatusBarWindow(std::shared_ptr<Theme> theme,
                             ItemClickHandler onItemClick = {},
                             ItemStateProvider stateProvider = {},
                             std::wstring windowClassName = kStatusBarWindowClass);
    void setItemClickHandler(ItemClickHandler handler) { onItemClick_ = std::move(handler); }
    void setItemStateProvider(ItemStateProvider provider) { stateProvider_ = std::move(provider); }
    void setContextMenuHandler(ContextMenuHandler handler) { onContextMenu_ = std::move(handler); }
    void setSettingsChangedHandler(SettingsChangedHandler handler) {
        onSettingsChanged_ = std::move(handler);
    }
    void setVisibilityChangedHandler(VisibilityChangedHandler handler) {
        onVisibilityChanged_ = std::move(handler);
    }
    ~StatusBarWindow();

    bool create();
    void show();
    void hide();
    // Applies the foreground rule immediately (used by TSF focus-loss
    // callbacks for low-latency hiding; the same check runs on the timer).
    void checkForegroundNow();
    void refresh();
    // Repaints only when a visible item's state image changes.  Used on
    // every key-up so normal typing never redraws a layered status bar.
    bool refreshIfStateChanged();
    bool setTheme(std::shared_ptr<Theme> theme);
    HWND hwnd() const { return hwnd_; }
    // True while the window exists and is visible.  Exposed for tests and for
    // diagnostics; production callers use show()/hide().
    bool isVisible() const { return hwnd_ && ::IsWindowVisible(hwnd_) != FALSE; }
    bool isStateTimerActive() const { return stateTimer_ != 0; }

private:
    static LRESULT CALLBACK staticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT wndProc(UINT msg, WPARAM wp, LPARAM lp);
    void paint();
    int hitTest(POINT pt) const;
    void positionAtCorner();
    bool collectItemStates(std::vector<int>* states) const;
    void showNow();
    void ensureStateTimer();
    void stopStateTimer();

    std::shared_ptr<Theme> theme_;
    std::wstring windowClassName_;
    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int hoverIndex_ = -1;
    int pressedIndex_ = -1;
    bool trackingMouse_ = false;
    UINT_PTR stateTimer_ = 0;
    StatusBarForegroundPolicy foregroundPolicy_;
    std::vector<int> lastItemStates_;
    ItemClickHandler onItemClick_;
    ItemStateProvider stateProvider_;
    ContextMenuHandler onContextMenu_;
    SettingsChangedHandler onSettingsChanged_;
    VisibilityChangedHandler onVisibilityChanged_;
};

class Renderer {
public:
    explicit Renderer(std::shared_ptr<Theme> theme);
    ~Renderer();

    bool valid() const { return theme_ && theme_->valid(); }
    const Theme& theme() const { return *theme_; }
    // effectiveFontSize / Theme::baseFontSize, clamped to 0.5..4.0.
    double scale() const { return scale_; }

    LayoutResult measure(const RenderState& state, HDC hdc) const;
    void paint(HDC hdc, const RECT& client, const RenderState& state) const;
    std::shared_ptr<Gdiplus::Bitmap> renderToBitmap(const RenderState& state,
                                                    int* outWidth = nullptr,
                                                    int* outHeight = nullptr) const;

    struct RenderTiming {
        unsigned long long buildUs = 0;
        unsigned long long bitmapUs = 0;
        unsigned long long paintUs = 0;
        unsigned long long backgroundUs = 0;
        unsigned long long textUs = 0;
        unsigned long long arrowUs = 0;
        unsigned int imageDraws = 0;
        unsigned long long totalUs = 0;
    };
    const RenderTiming& lastRenderTiming() const { return lastRenderTiming_; }

    // Applies a window region from the background image alpha where possible.
    void applyWindowRegion(HWND hwnd, const RECT& client) const;

private:
    static constexpr int kPageArrowArea = 30;
    static constexpr int kSplitGap = 5;
    static constexpr int kRowPadding = 4;

    struct Plan {
        const Scheme* scheme = nullptr;
        bool split = false;
        bool horizontal = true;
        int width = 0;
        int height = 0;
        RECT primaryRect = {0, 0, 0, 0};
        RECT secondaryRect = {0, 0, 0, 0};
        std::vector<RECT> candidateRects;
        RECT preeditRect = {0, 0, 0, 0};
        POINT anchor = {0, 0};
        int sepY = 0;
        bool hasSeparator = false;
        int arrowX = 0;
        int arrowY = 0;
        bool arrowsHorizontal = true;
    };

    Plan buildPlan(HDC hdc, const RenderState& state) const;
    void paintPlan(HDC hdc, const RECT& client, const RenderState& state,
                   const Plan& plan) const;
    void paintPlanGdiplus(Gdiplus::Graphics& graphics, const RenderState& state,
                          const Plan& plan) const;
    void paintBackground(Gdiplus::Graphics& graphics, const Plan& plan) const;
    void paintForeground(Gdiplus::Graphics& graphics, const RenderState& state,
                         const Plan& plan) const;
    std::shared_ptr<Gdiplus::Bitmap> backgroundBitmap(const Plan& plan,
                                                      int targetWidth,
                                                      int targetHeight) const;
    int drawMixedTextGdiplus(Gdiplus::Graphics& graphics, int x, int y,
                             const std::wstring& text, COLORREF color,
                             bool bold = false) const;
    std::wstring faceFor(bool ascii) const;
    void ensureFonts(HDC hdc) const;
    const Gdiplus::FontFamily* familyFor(bool ascii) const;
    int fontPixelSize() const;
    int measureTextGdiplus(Gdiplus::Graphics& graphics, const std::wstring& text) const;
    int drawTextRuns(Gdiplus::Graphics& graphics, int x, int y,
                     const std::wstring& text, COLORREF color, bool bold) const;
    void drawSliced(Gdiplus::Graphics& g, Gdiplus::Bitmap* bitmap,
                    const RECT& dest, const Layout& h, const Layout& v) const;
    void drawComponent(Gdiplus::Graphics& g, const Component& component,
                       const RECT& dest) const;
    int itemWidth(HDC hdc, const std::wstring& text, const std::wstring& key) const;
    std::wstring selKeyFor(size_t index, const std::wstring& keys) const;
    int scaleValue(int value) const;
    RECT scaleRect(const RECT& rect) const;

    std::shared_ptr<Theme> theme_;
    double scale_ = 1.0;
    mutable int dpi_ = 96;
    mutable bool fontsReady_ = false;
    mutable Gdiplus::PrivateFontCollection privateFonts_;
    mutable std::unique_ptr<Gdiplus::FontFamily> ownedFamilyCh_;
    mutable std::unique_ptr<Gdiplus::FontFamily> ownedFamilyEn_;
    mutable const Gdiplus::FontFamily* familyCh_ = nullptr;
    mutable const Gdiplus::FontFamily* familyEn_ = nullptr;
    mutable std::map<std::wstring, int> glyphWidthCache_;
    mutable std::map<std::wstring, std::shared_ptr<Gdiplus::Bitmap>> backgroundCache_;
    mutable RenderTiming lastRenderTiming_;
};

} // namespace SogouSkin
} // namespace Moqi
