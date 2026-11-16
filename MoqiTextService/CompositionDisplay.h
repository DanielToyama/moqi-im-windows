//
// Candidate-window display helpers for Rime radical-lookup ("u mode").
//
// Rime's affix_segmentor with `prefix: u` consumes the leading `u` from the
// composition preedit as soon as the radical_lookup segment is recognized.
// The backend still receives/commits the exact typed keys (Enter commits the
// raw input), but the candidate window would otherwise show only `shui` and
// the user loses the visual cue that they are in radical-lookup mode.
//
// This header is intentionally header-only and independent of TSF so the
// formatting/tracking rules can be covered by SogouLogicTest.
//
#pragma once

#include <windows.h>

#include <string>

namespace Moqi {

inline constexpr wchar_t kSogouUModePreeditSuffix[] = L"[拼字]";

// Keeps the raw keystrokes of a u-mode composition.  Feed it once per
// key-down with the backend response flags.  The tracker only starts when a
// lowercase `u` is typed from an idle (non-composing) state, which avoids
// decorating ordinary compositions that happen to contain u.
class UModeInputTracker {
public:
    void reset() { raw_.clear(); }

    const std::wstring& rawInput() const { return raw_; }

    void onKeyEvent(wchar_t charCode, UINT keyCode, bool handled,
                    bool hasComposition, bool showCandidates, bool hasCommit,
                    bool idleBeforeKey) {
        if (hasCommit) {
            raw_.clear();
            return;
        }
        if (keyCode == VK_ESCAPE || keyCode == VK_RETURN) {
            raw_.clear();
            return;
        }
        if (keyCode == VK_BACK) {
            if (!raw_.empty()) {
                raw_.pop_back();
            }
            // Backspace can leave the backend in a u-mode state whose response
            // has neither a composition string nor candidates (Rime returns
            // only the affix tip after the radical part is deleted).  The
            // raw-key buffer is the authoritative u-mode record, so do not
            // clear a non-empty buffer based on those backend flags.
            return;
        }
        if (raw_.empty()) {
            // Start on the first `u` even when Rime's onKeyDown "handled"
            // flag is false: the filter probe already consumed the key, and
            // the onKeyDown reply can still carry the real composition and
            // candidates.  `idleBeforeKey` only means no TSF composition is
            // active; a recovering candidate window from the previous gesture
            // must not block a new u-mode gesture.
            if (charCode == L'u' && idleBeforeKey &&
                (hasComposition || showCandidates)) {
                raw_ = L"u";
            }
            return;
        }
        if (!handled && !hasComposition && !showCandidates) {
            // A printable key that the backend rejects and that produced no
            // composition/candidate state ends the gesture.
            if (charCode >= 0x20) {
                raw_.clear();
            }
            return;
        }
        if ((charCode >= L'a' && charCode <= L'z') || charCode == L'\'') {
            raw_.push_back(charCode);
            // A tracked letter for which the backend has neither a
            // composition nor candidates means Rime silently left u-mode.
            if (!hasComposition && !showCandidates) {
                raw_.clear();
            }
        } else if (charCode == L' ' || keyCode == VK_SPACE ||
                   (charCode >= 0x20 &&
                    !(keyCode >= '0' && keyCode <= '9'))) {
            // Space accepts a candidate / ends the composition.  Digits are
            // candidate-selection keys and do not change the raw input.
            raw_.clear();
        }
    }

private:
    std::wstring raw_;
};

// Builds the candidate-window preedit for a u-mode composition.  `rawInput`
// is UModeInputTracker::rawInput(), including the leading u; `composition` is
// the backend composition string (normally without the consumed u).  Returns
// `composition` unchanged for normal (non-u-mode) input.
inline std::wstring BuildSogouUModePreedit(const std::wstring& rawInput,
                                           const std::wstring& composition) {
    auto normalizeSeparators = [](std::wstring text) {
        std::wstring result;
        result.reserve(text.size());
        for (wchar_t ch : text) {
            if (ch == L' ' || ch == L'\t') {
                ch = L'\'';
            }
            // Do not emit duplicated apostrophes from Rime's pretty preedit
            // (e.g. "u's hui'shang" -> "u's'hui'shang").
            if (ch == L'\'' && !result.empty() && result.back() == L'\'') {
                continue;
            }
            result.push_back(ch);
        }
        return result;
    };
    auto withoutSeparators = [](const std::wstring& text) {
        std::wstring result;
        result.reserve(text.size());
        for (wchar_t ch : text) {
            if (ch != L'\'' && ch != L' ' && ch != L'\t') {
                result.push_back(ch);
            }
        }
        return result;
    };
    auto trimTrailingSeparators = [](std::wstring text) {
        while (!text.empty() &&
               (text.back() == L'\'' || text.back() == L' ' ||
                text.back() == L'\t')) {
            text.pop_back();
        }
        return text;
    };

    // Rime's affix segmentor can put its tip into the preedit, for example
    // "u  〔拆字〕shui" or "u  〔拆字〕".  The bracket annotation is display
    // chrome, not part of the typed code; remove it (and the spaces before it)
    // before matching the backend text against the raw keys.
    auto stripBackendTips = [](const std::wstring& text, bool& hadTip) {
        std::wstring result = text;
        for (;;) {
            const size_t open = result.find(L'〔');
            if (open == std::wstring::npos) {
                break;
            }
            const size_t close = result.find(L'〕', open + 1);
            size_t eraseStart = open;
            while (eraseStart > 0 &&
                   (result[eraseStart - 1] == L' ' ||
                    result[eraseStart - 1] == L'\t')) {
                --eraseStart;
            }
            hadTip = true;
            if (close == std::wstring::npos) {
                result.erase(eraseStart);
                break;
            }
            result.erase(eraseStart, close + 1 - eraseStart);
        }
        return result;
    };

    bool hadTip = false;
    const std::wstring backendText = stripBackendTips(composition, hadTip);

    if (rawInput.empty()) {
        if (!backendText.empty() &&
            (backendText[0] == L'u' || backendText[0] == L'U')) {
            return backendText + kSogouUModePreeditSuffix;
        }
        return composition;
    }
    if (rawInput[0] != L'u') {
        return composition;
    }

    // The very first `u` is not yet recognized by Rime's radical_lookup
    // (its recognizer requires "^u[a-z]+$"), but it is already the start of
    // the u-mode gesture from the user's point of view.
    if (rawInput.size() == 1) {
        return std::wstring(L"u") + kSogouUModePreeditSuffix;
    }

    const std::wstring body = rawInput.substr(1);
    for (wchar_t ch : body) {
        if ((ch >= L'a' && ch <= L'z') || ch == L'\'' || ch == L' ' ||
            ch == L'\t') {
            continue;
        }
        return composition;
    }

    const std::wstring rawKey = withoutSeparators(rawInput);
    const std::wstring rawBodyKey = withoutSeparators(body);

    // Ordinary pinyin that merely starts with u keeps the u in the backend
    // composition (`uan`, `un...`).  Do not decorate it as u-mode.  A lone
    // separator after the initial u (`u'`) has no letters yet, so it still
    // belongs to the u-mode gesture.
    if (!hadTip && !backendText.empty() && !rawBodyKey.empty() &&
        withoutSeparators(backendText) == rawKey &&
        (backendText[0] == L'u' || backendText[0] == L'U')) {
        return composition;
    }

    // Rime removes the consumed prefix from the radical-lookup content.  In
    // tip-only states the stripped backend text is still just "u"; treat that
    // as empty content.
    std::wstring backendBody = backendText;
    if (!backendBody.empty() &&
        (backendBody[0] == L'u' || backendBody[0] == L'U') &&
        (hadTip || rawBodyKey.empty())) {
        backendBody.erase(0, 1);
    }
    const std::wstring backendKey = withoutSeparators(backendBody);

    // Prefer the backend text when it represents exactly the same typed keys:
    // Rime inserts pretty syllable apostrophes (`shui'shang`) that the raw
    // tracker cannot know.  If the two disagree (Rime reordered the preedit,
    // an async update arrived, etc.) the raw keys are still the user's source
    // of truth and the window must never fall back to a bare backend string.
    std::wstring displayBody;
    if (!backendBody.empty() && !rawBodyKey.empty() &&
        backendKey == rawBodyKey) {
        displayBody = backendBody;
    } else {
        displayBody = body;
    }
    displayBody = trimTrailingSeparators(normalizeSeparators(displayBody));

    std::wstring result = L"u";
    if (!displayBody.empty() && displayBody[0] != L'\'') {
        result += L'\'';
    }
    result += displayBody;
    result += kSogouUModePreeditSuffix;
    return result;
}

} // namespace Moqi
