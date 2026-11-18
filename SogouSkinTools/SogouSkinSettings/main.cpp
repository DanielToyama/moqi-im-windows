// Native Win32 settings UI for the Sogou skin compatibility layer.
//
// This is deliberately a standalone process: the TSF DLL only links the
// renderer, and a crash or a slow preview can never take the input service
// down.  It writes the same override INI files that the IME reads and then
// broadcasts WM_MOQI_SOGOU_SETTINGS_CHANGED for a live reload.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <gdiplus.h>

#include "SogouSkin.h"
#include "DpiSupport.h"

using namespace Moqi;

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {

constexpr wchar_t kMainClass[] = L"MoqiSogouSkinSettingsWindow";
constexpr wchar_t kPreviewClass[] = L"MoqiSogouSkinSettingsPreview";
constexpr int kClientWidth = 960;
constexpr int kClientHeight = 700;

bool g_offscreen = false; // --offscreen: used by the message-based UI tests.
std::wstring g_self_test_dir; // --self-test <dir>: headless dropdown checks.
UINT g_forced_dpi = 0; // --dpi <n>: test-only layout/rendering scale override.

enum ControlId {
    IDC_SKIN_COMBO = 1001,
    IDC_SKIN_BROWSE,
    IDC_FONT_SLIDER,
    IDC_FONT_VALUE,
    IDC_SCALE_SLIDER,
    IDC_SCALE_VALUE,
    IDC_LAYOUT_PREEDIT_X,
    IDC_LAYOUT_PREEDIT_Y,
    IDC_LAYOUT_CANDIDATE_X,
    IDC_LAYOUT_CANDIDATE_Y,
    IDC_LAYOUT_SPIN_PREEDIT_X,
    IDC_LAYOUT_SPIN_PREEDIT_Y,
    IDC_LAYOUT_SPIN_CANDIDATE_X,
    IDC_LAYOUT_SPIN_CANDIDATE_Y,
    IDC_COLOR_PINYIN,
    IDC_COLOR_FIRST,
    IDC_COLOR_TEXT,
    IDC_COLOR_COMMENT,
    IDC_HORIZONTAL,
    IDC_VERTICAL,
    IDC_STATUSBAR,
    IDC_STATUSBAR_FULLSCREEN,
    IDC_ITEM_LIST,
    IDC_APPLY,
    IDC_OK,
    IDC_CANCEL,
    IDC_RESET,
    IDC_STATUS_LABEL,
    IDC_FONT_FACE_LABEL,
    IDC_FONT_CH_COMBO,
    IDC_FONT_EN_COMBO,
};

constexpr int kColorCount = 4;

struct SkinEntry {
    std::wstring path;
    std::wstring label;
};

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) {
        return name;
    }
    const wchar_t last = dir.back();
    return dir + (last == L'\\' || last == L'/' ? L"" : L"\\") + name;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool SamePath(const std::wstring& left, const std::wstring& right) {
    // Recent-skin entries may use either separator style (and the settings
    // profile root can mix them); compare canonicalized separators so the
    // same file is never listed twice or copied onto itself.
    auto normalize = [](std::wstring value) {
        std::replace(value.begin(), value.end(), L'/', L'\\');
        return value;
    };
    return _wcsicmp(normalize(left).c_str(), normalize(right).c_str()) == 0;
}

std::wstring TrimText(const std::wstring& value) {
    size_t begin = 0;
    while (begin < value.size() && iswspace(value[begin])) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && iswspace(value[end - 1])) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::wstring ToLowerText(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

bool ReadAllBytes(const std::wstring& path, std::vector<unsigned char>* bytes) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size = {};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        ::CloseHandle(file);
        return false;
    }
    bytes->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok =
        bytes->empty() ||
        (::ReadFile(file, bytes->data(), static_cast<DWORD>(bytes->size()), &read,
                    nullptr) &&
         read == bytes->size());
    ::CloseHandle(file);
    return ok;
}

// Recent-skin list accepts UTF-16LE (with or without BOM) and UTF-8.
std::wstring DecodeTextBytes(const std::vector<unsigned char>& bytes) {
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        std::wstring text((bytes.size() - 2) / 2, L'\0');
        if (!text.empty()) {
            memcpy(text.data(), bytes.data() + 2, text.size() * sizeof(wchar_t));
        }
        return text;
    }
    size_t offset = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB &&
        bytes[2] == 0xBF) {
        offset = 3;
    }
    const char* data = reinterpret_cast<const char*>(bytes.data() + offset);
    const int length = static_cast<int>(bytes.size() - offset);
    if (length <= 0) {
        return {};
    }
    int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, length,
                                      nullptr, 0);
    UINT codePage = CP_UTF8;
    if (count <= 0) {
        codePage = CP_ACP;
        count = ::MultiByteToWideChar(codePage, 0, data, length, nullptr, 0);
    }
    if (count <= 0) {
        return {};
    }
    std::wstring text(count, L'\0');
    ::MultiByteToWideChar(codePage, 0, data, length, text.data(), count);
    return text;
}

std::wstring RecentSkinsPath() {
    const std::wstring dir = SogouSkin::ConfigDirectory();
    return dir.empty() ? std::wstring()
                       : JoinPath(dir, L"sogou_skin_recent.txt");
}

std::vector<std::wstring> LoadRecentSkins() {
    std::vector<std::wstring> result;
    const std::wstring path = RecentSkinsPath();
    if (path.empty()) {
        return result;
    }
    std::vector<unsigned char> bytes;
    if (!ReadAllBytes(path, &bytes)) {
        return result;
    }
    const std::wstring text = DecodeTextBytes(bytes);
    std::wstringstream stream(text);
    std::wstring line;
    while (std::getline(stream, line)) {
        line = TrimText(line);
        if (!line.empty() && line[0] == L'\xfeff') {
            line.erase(line.begin());
        }
        if (line.empty() || line[0] == L'#' || line[0] == L';') {
            continue;
        }
        if (!FileExists(line)) {
            continue; // drop deleted skins
        }
        bool duplicate = false;
        for (const auto& existing : result) {
            if (SamePath(existing, line)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            result.push_back(line);
            if (result.size() >= 10) {
                break;
            }
        }
    }
    return result;
}

std::wstring FormatColorLabel(COLORREF color) {
    wchar_t buffer[16] = {};
    swprintf_s(buffer, L"%02X%02X%02X", GetRValue(color), GetGValue(color),
               GetBValue(color));
    return buffer;
}

std::wstring FriendlyItemName(const std::wstring& name) {
    if (name == L"cn_en") return L"中英文 / 字母键";
    if (name == L"quan_ban") return L"全角 / 半角";
    if (name == L"biaodian") return L"中文 / 英文标点";
    if (name == L"fan_jian") return L"简体 / 繁体";
    if (name == L"quan_shuang") return L"全拼 / 双拼";
    if (name == L"softkeyboard") return L"软键盘";
    if (name == L"sogousearch") return L"搜索";
    if (name == L"menu") return L"菜单";
    if (name == L"passport") return L"账户";
    return name;
}

void AppendSkinPath(const std::wstring& path, std::vector<SkinEntry>* skins) {
    if (path.empty() || !FileExists(path)) {
        return;
    }
    for (const auto& entry : *skins) {
        if (SamePath(entry.path, path)) {
            return;
        }
    }
    skins->push_back({path, FileNameOf(path)});
}

void AppendSkinDir(const std::wstring& dir, std::vector<SkinEntry>* skins) {
    if (dir.empty() || !DirectoryExists(dir)) {
        return;
    }
    WIN32_FIND_DATAW data = {};
    const std::wstring pattern = JoinPath(dir, L"*.ssf");
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        AppendSkinPath(JoinPath(dir, data.cFileName), skins);
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
}

std::wstring DirectorySuffixLabel(const std::wstring& path) {
    std::wstring dir = path;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    dir.resize(slash);
    const size_t parentSlash = dir.find_last_of(L"\\/");
    const std::wstring leaf =
        parentSlash == std::wstring::npos ? dir : dir.substr(parentSlash + 1);
    if (parentSlash == std::wstring::npos) {
        return leaf;
    }
    std::wstring parent = dir.substr(0, parentSlash);
    const size_t grandSlash = parent.find_last_of(L"\\/");
    const std::wstring grand =
        grandSlash == std::wstring::npos ? parent : parent.substr(grandSlash + 1);
    return grand.empty() ? leaf : grand + L"\\" + leaf;
}

// Show just the file name in the common case.  If two skins share a file
// name, append a short parent-directory suffix so the user can tell them
// apart without showing the full path.
void UpdateSkinLabels(std::vector<SkinEntry>* skins) {
    std::map<std::wstring, int> counts;
    for (const auto& skin : *skins) {
        ++counts[ToLowerText(FileNameOf(skin.path))];
    }
    for (auto& skin : *skins) {
        const std::wstring name = FileNameOf(skin.path);
        if (counts[ToLowerText(name)] > 1) {
            const std::wstring suffix = DirectorySuffixLabel(skin.path);
            skin.label = suffix.empty() ? name : name + L"  (" + suffix + L")";
        } else {
            skin.label = name;
        }
    }
}

std::vector<SkinEntry> EnumerateSkins(const std::wstring& currentPath) {
    std::vector<SkinEntry> skins;

    // Most recently used skins first, then the currently installed one.
    for (const auto& recent : LoadRecentSkins()) {
        AppendSkinPath(recent, &skins);
    }
    if (!currentPath.empty()) {
        if (FileExists(currentPath)) {
            AppendSkinPath(currentPath, &skins);
        } else if (DirectoryExists(currentPath)) {
            // Extracted skin directory selected with "浏览..." / path file.
            skins.push_back({currentPath, FileNameOf(currentPath)});
        }
    }

    const std::wstring config = SogouSkin::ConfigDirectory();
    if (!config.empty()) {
        AppendSkinDir(config, &skins);
        AppendSkinDir(JoinPath(config, L"skins"), &skins);
    }

    // Users commonly download .ssf files and forget to move them.  Include
    // the usual download/desktop locations without requiring a browse dialog.
    wchar_t profile[32768] = {};
    const DWORD profileLength =
        ::GetEnvironmentVariableW(L"USERPROFILE", profile, _countof(profile));
    if (profileLength > 0 && profileLength < _countof(profile)) {
        const std::wstring userProfile(profile);
        AppendSkinDir(JoinPath(userProfile, L"Downloads"), &skins);
        AppendSkinDir(JoinPath(userProfile, L"Desktop"), &skins);
        AppendSkinDir(JoinPath(userProfile, L"OneDrive\\Desktop"), &skins);
    }

    const std::wstring programFilesX86 = [] {
        wchar_t buffer[32768] = {};
        const DWORD length =
            ::GetEnvironmentVariableW(L"ProgramFiles(x86)", buffer, _countof(buffer));
        return length > 0 && length < _countof(buffer) ? std::wstring(buffer)
                                                        : std::wstring();
    }();
    if (!programFilesX86.empty()) {
        AppendSkinDir(JoinPath(programFilesX86, L"SogouImeSkinEditor"), &skins);
    }

    UpdateSkinLabels(&skins);
    return skins;
}

bool WriteAllBytes(const std::wstring& path, const std::string& data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    return file.good();
}

std::string NarrowAscii(const std::wstring& text) {
    std::string result;
    result.reserve(text.size());
    for (wchar_t ch : text) {
        result.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    }
    return result;
}

bool GetPngEncoderClsid(CLSID* clsid) {
    UINT count = 0;
    UINT size = 0;
    Gdiplus::GetImageEncodersSize(&count, &size);
    if (size == 0) {
        return false;
    }
    std::vector<BYTE> buffer(size);
    auto* encoders =
        reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Gdiplus::GetImageEncoders(count, size, encoders);
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            *clsid = encoders[i].Clsid;
            return true;
        }
    }
    return false;
}

bool SaveBitmapPng(Gdiplus::Bitmap* bitmap, const std::wstring& path) {
    if (!bitmap) {
        return false;
    }
    CLSID png = {};
    return GetPngEncoderClsid(&png) &&
           bitmap->Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
}

bool CaptureWindowToPng(HWND hwnd, const std::wstring& path) {
    if (!hwnd) {
        return false;
    }
    RECT rect = {};
    ::GetWindowRect(hwnd, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return false;
    }
    HDC screen = ::GetDC(nullptr);
    HDC memory = ::CreateCompatibleDC(screen);
    HBITMAP bitmap = ::CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = ::SelectObject(memory, bitmap);
    const HBRUSH background = ::CreateSolidBrush(::GetSysColor(COLOR_WINDOW));
    RECT fillRect = {0, 0, width, height};
    ::FillRect(memory, &fillRect, background);
    ::DeleteObject(background);
    // PW_RENDERFULLCONTENT (0x2) is missing from older SDK headers even
    // though the OS has supported it since Windows 8.1.
    constexpr UINT kPrintWindowRenderFullContent = 0x00000002;
    ::PrintWindow(hwnd, memory, kPrintWindowRenderFullContent);
    // Standard child controls occasionally do not paint into a PrintWindow
    // bitmap when the window is positioned off-screen; WM_PRINT asks each
    // child to render itself as well.
    ::SendMessageW(hwnd, WM_PRINT, reinterpret_cast<WPARAM>(memory),
                   PRF_CLIENT | PRF_CHILDREN | PRF_NONCLIENT | PRF_ERASEBKGND);
    Gdiplus::Bitmap image(bitmap, nullptr);
    const bool saved = SaveBitmapPng(&image, path);
    ::SelectObject(memory, old);
    ::DeleteObject(bitmap);
    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    return saved;
}

std::string Utf16LeBytes(const std::wstring& text) {
    std::string bytes;
    bytes.reserve(text.size() * 2 + 2);
    bytes.push_back(static_cast<char>(0xFF));
    bytes.push_back(static_cast<char>(0xFE));
    for (wchar_t ch : text) {
        bytes.push_back(static_cast<char>(ch & 0xFF));
        bytes.push_back(static_cast<char>((ch >> 8) & 0xFF));
    }
    return bytes;
}

int CALLBACK CollectFontFamilyProc(const LOGFONTW* logFont,
                                   const TEXTMETRICW* /*metrics*/, DWORD /*type*/,
                                   LPARAM param) {
    auto* names = reinterpret_cast<std::vector<std::wstring>*>(param);
    if (!logFont || !names) {
        return 1;
    }
    // '@' faces are vertical-writing variants of the same family; listing
    // both would only duplicate every Chinese font in the drop-down.
    if (logFont->lfFaceName[0] != L'\0' && logFont->lfFaceName[0] != L'@') {
        names->push_back(logFont->lfFaceName);
    }
    return 1;
}

void AppendUniqueFontName(std::vector<std::wstring>* names,
                          const std::wstring& name) {
    if (!names || name.empty() || name[0] == L'@') {
        return;
    }
    for (const auto& existing : *names) {
        if (_wcsicmp(existing.c_str(), name.c_str()) == 0) {
            return;
        }
    }
    names->push_back(name);
}

// GDI+ can read the family names from a font file even when GDI has not
// enumerated it (for example a font dropped into the per-user Fonts folder
// without the usual registry registration).
void AppendFontFamiliesFromFile(const std::wstring& path,
                                std::vector<std::wstring>* names) {
    Gdiplus::PrivateFontCollection collection;
    if (collection.AddFontFile(path.c_str()) != Gdiplus::Ok) {
        return;
    }
    const int count = collection.GetFamilyCount();
    if (count <= 0) {
        return;
    }
    std::vector<Gdiplus::FontFamily> families(static_cast<size_t>(count));
    int found = 0;
    if (collection.GetFamilies(count, families.data(), &found) != Gdiplus::Ok) {
        return;
    }
    for (int i = 0; i < found; ++i) {
        wchar_t name[LF_FACESIZE] = {};
        families[static_cast<size_t>(i)].GetFamilyName(name, LANG_NEUTRAL);
        AppendUniqueFontName(names, name);
    }
}

void ScanUserFontDirectory(const std::wstring& dir,
                           std::vector<std::wstring>* names) {
    if (dir.empty() || !DirectoryExists(dir)) {
        return;
    }
    WIN32_FIND_DATAW data = {};
    HANDLE find = ::FindFirstFileW(JoinPath(dir, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        std::wstring name = data.cFileName;
        const size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos) {
            continue;
        }
        const std::wstring ext = ToLowerText(name.substr(dot));
        if (ext != L".ttf" && ext != L".otf" && ext != L".ttc" &&
            ext != L".otc") {
            continue;
        }
        const std::wstring path = JoinPath(dir, name);
        // FR_PRIVATE makes the face available to this process for the
        // preview renderer even if it was copied in manually.  The OS drops
        // the registration when the settings process exits.
        ::AddFontResourceExW(path.c_str(), FR_PRIVATE, nullptr);
        AppendFontFamiliesFromFile(path, names);
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
}

std::vector<std::wstring> EnumerateInstalledFonts() {
    std::vector<std::wstring> found;
    wchar_t localAppData[32768] = {};
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData,
                                  _countof(localAppData)) > 0) {
        ScanUserFontDirectory(
            JoinPath(localAppData, L"Microsoft\\Windows\\Fonts"), &found);
    }

    HDC dc = ::GetDC(nullptr);
    if (dc) {
        LOGFONTW filter = {};
        filter.lfCharSet = DEFAULT_CHARSET;
        ::EnumFontFamiliesExW(dc, &filter, CollectFontFamilyProc,
                              reinterpret_cast<LPARAM>(&found), 0);
        ::ReleaseDC(nullptr, dc);
    }

    std::vector<std::wstring> unique;
    unique.reserve(found.size());
    for (const auto& name : found) {
        AppendUniqueFontName(&unique, name);
    }
    std::sort(unique.begin(), unique.end(),
              [](const std::wstring& left, const std::wstring& right) {
                  const int result =
                      ::CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE,
                                       left.c_str(), -1, right.c_str(), -1);
                  if (result == CSTR_LESS_THAN) {
                      return true;
                  }
                  if (result == CSTR_GREATER_THAN) {
                      return false;
                  }
                  return _wcsicmp(left.c_str(), right.c_str()) < 0;
              });
    return unique;
}

std::wstring ComboItemText(HWND combo, int index) {
    if (!combo || index < 0) {
        return {};
    }
    wchar_t buffer[256] = {};
    const LRESULT length = ::SendMessageW(
        combo, CB_GETLBTEXT, static_cast<WPARAM>(index),
        reinterpret_cast<LPARAM>(buffer));
    return length > 0 ? std::wstring(buffer, static_cast<size_t>(length))
                      : std::wstring();
}

void SaveRecentSkins(const std::vector<std::wstring>& paths) {
    const std::wstring path = RecentSkinsPath();
    if (path.empty()) {
        return;
    }
    const std::wstring config = SogouSkin::ConfigDirectory();
    if (!config.empty()) {
        ::CreateDirectoryW(config.c_str(), nullptr);
    }
    std::wstring text;
    int count = 0;
    for (const auto& item : paths) {
        if (count++ >= 10) {
            break;
        }
        text += item;
        text += L"\r\n";
    }
    WriteAllBytes(path, Utf16LeBytes(text));
}

void RememberRecentSkin(const std::wstring& path) {
    if (path.empty() || !FileExists(path)) {
        return;
    }
    std::vector<std::wstring> recent = LoadRecentSkins();
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [&](const std::wstring& item) {
                                    return SamePath(item, path);
                                }),
                 recent.end());
    recent.insert(recent.begin(), path);
    if (recent.size() > 10) {
        recent.resize(10);
    }
    SaveRecentSkins(recent);
}

class SettingsWindow {
public:
    bool create(HINSTANCE instance);
    int run();

private:
    static LRESULT CALLBACK staticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK staticPreviewProc(HWND hwnd, UINT msg, WPARAM wp,
                                              LPARAM lp);
    LRESULT wndProc(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT previewProc(UINT msg, WPARAM wp, LPARAM lp);

    void createControls();
    void destroyFonts();
    void layoutControls();
    void applyFonts();
    void updateDpiDependentMetrics();
    int dpiScale(int value) const;
    HFONT makeFont(int pointSize, bool bold = false) const;

    bool loadSkin(const std::wstring& path, bool useSavedOverrides,
                  std::wstring* error);
    void populateSkinCombo();
    void populateStatusItems();
    void populateFontCombo(HWND combo, const std::wstring& current);
    std::wstring selectedFont(HWND combo) const;
    std::wstring chooseDifferentFont(HWND combo, const wchar_t* preferred);
    void updateFontFaceLabel();
    void syncControlsFromState();
    void syncLayoutEdits();
    void syncLayoutSpins();
    void onLayoutEditChanged(int index);
    void onLayoutSpinDelta(int index, int delta, int position);
    void normalizeLayoutEdit(int index);
    void updateColorButton(int index);
    void chooseColor(int index);
    void schedulePreview();
    void refreshPreview();
    void paintPreview(HDC dc);
    void runSelfTest();
    bool captureWindowToPng(const std::wstring& path);
    bool savePreviewBitmap(const std::wstring& path);
    std::wstring previewHash() const;
    void setStatus(const std::wstring& text);
    void resetToDefaults();
    bool apply(bool closeAfter);
    SogouSkin::SettingsOverrides buildSettingsValues() const;
    std::wstring buildOverridesText(int* removedLegacyKeys = nullptr) const;
    bool saveOptions() const;

    void onCommand(WORD id, WORD notify);
    void onHScroll(HWND control, int code);
    void onListItemChanged(LPNMLISTVIEW info);
    void drawItem(LPDRAWITEMSTRUCT item);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND preview_ = nullptr;
    HWND groupPreview_ = nullptr;
    HWND groupSkin_ = nullptr;
    HWND groupFont_ = nullptr;
    HWND groupStatus_ = nullptr;
    HWND groupFineTune_ = nullptr;
    HWND skinCombo_ = nullptr;
    HWND browseButton_ = nullptr;
    HWND skinLabel_ = nullptr;
    HWND statusItemsLabel_ = nullptr;
    HWND fontSizeLabel_ = nullptr;
    HWND fontSlider_ = nullptr;
    HWND fontValue_ = nullptr;
    HWND scaleLabel_ = nullptr;
    HWND scaleSlider_ = nullptr;
    HWND scaleValue_ = nullptr;
    HWND layoutLabels_[4] = {};
    HWND layoutEdits_[4] = {};
    HWND layoutSpins_[4] = {};
    HWND fontFaceLabel_ = nullptr;
    HWND fontChLabel_ = nullptr;
    HWND fontEnLabel_ = nullptr;
    HWND fontChCombo_ = nullptr;
    HWND fontEnCombo_ = nullptr;
    HWND colorLabels_[kColorCount] = {};
    HWND colorButtons_[kColorCount] = {};
    HWND horizontalRadio_ = nullptr;
    HWND verticalRadio_ = nullptr;
    HWND statusbarCheck_ = nullptr;
    HWND statusbarFullscreenCheck_ = nullptr;
    HWND itemList_ = nullptr;
    HWND applyButton_ = nullptr;
    HWND okButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    HWND resetButton_ = nullptr;
    HWND statusLabel_ = nullptr;

    HFONT uiFont_ = nullptr;
    HFONT boldFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    UINT dpi_ = 96;

    std::vector<SkinEntry> skins_;
    int skinIndex_ = -1;
    std::wstring skinPath_;
    std::wstring initialSkinPath_;
    std::shared_ptr<SogouSkin::Theme> baseTheme_;
    std::shared_ptr<SogouSkin::Theme> effectiveTheme_;

    int baseFontSize_ = 24;
    int fontSize_ = 24;
    int scalePercent_ = 100;
    SogouSkin::SkinLayout layout_;
    std::wstring fontCh_;
    std::wstring fontEn_;
    std::vector<std::wstring> fonts_;
    COLORREF colors_[kColorCount] = {};
    bool horizontal_ = false;
    bool statusbar_ = true;
    bool statusbarFullscreenHide_ = true;

    std::vector<std::wstring> itemNames_;
    std::vector<bool> itemVisible_;
    std::shared_ptr<Gdiplus::Bitmap> previewBitmap_;
    int previewBitmapWidth_ = 0;
    int previewBitmapHeight_ = 0;
    std::wstring previewError_;

    UINT_PTR previewTimer_ = 0;
    bool updatingList_ = false;
    bool updatingControls_ = false;
    std::wstring tempOverridesPath_;
    COLORREF customColors_[16] = {};
};

int SettingsWindow::dpiScale(int value) const {
    return ::MulDiv(value, static_cast<int>(dpi_), 96);
}

HFONT SettingsWindow::makeFont(int pointSize, bool bold) const {
    const int height = -::MulDiv(pointSize, static_cast<int>(dpi_), 72);
    return ::CreateFontW(height, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE,
                         FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void SettingsWindow::destroyFonts() {
    if (uiFont_) ::DeleteObject(uiFont_);
    if (boldFont_) ::DeleteObject(boldFont_);
    if (smallFont_) ::DeleteObject(smallFont_);
    uiFont_ = nullptr;
    boldFont_ = nullptr;
    smallFont_ = nullptr;
}

void SettingsWindow::applyFonts() {
    destroyFonts();
    uiFont_ = makeFont(9);
    boldFont_ = makeFont(9, true);
    smallFont_ = makeFont(8);
    EnumChildWindows(hwnd_, [](HWND child, LPARAM param) -> BOOL {
        ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(uiFont_));
    if (fontFaceLabel_) {
        ::SendMessageW(fontFaceLabel_, WM_SETFONT,
                       reinterpret_cast<WPARAM>(smallFont_), TRUE);
    }
    if (statusLabel_) {
        ::SendMessageW(statusLabel_, WM_SETFONT,
                       reinterpret_cast<WPARAM>(smallFont_), TRUE);
    }
}

void SettingsWindow::updateDpiDependentMetrics() {
    if (fontChCombo_) {
        ::SendMessageW(fontChCombo_, CB_SETDROPPEDWIDTH, dpiScale(320), 0);
    }
    if (fontEnCombo_) {
        ::SendMessageW(fontEnCombo_, CB_SETDROPPEDWIDTH, dpiScale(320), 0);
    }
    if (itemList_) {
        ::SendMessageW(itemList_, LVM_SETCOLUMNWIDTH, 0, dpiScale(280));
    }
    for (HWND edit : layoutEdits_) {
        if (edit) {
            // The up-down is overlaid on the edit's right edge; keep the
            // right-aligned digits clear of the arrows at every DPI.
            ::SendMessageW(edit, EM_SETMARGINS, EC_RIGHTMARGIN,
                           MAKELPARAM(0, dpiScale(20)));
        }
    }
}

bool SettingsWindow::create(HINSTANCE instance) {
    instance_ = instance;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &SettingsWindow::staticWndProc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kMainClass;
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    WNDCLASSEXW pc = {};
    pc.cbSize = sizeof(pc);
    pc.lpfnWndProc = &SettingsWindow::staticPreviewProc;
    pc.hInstance = instance;
    pc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    pc.hbrBackground = nullptr;
    pc.lpszClassName = kPreviewClass;
    if (!::RegisterClassExW(&pc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    dpi_ = SogouSkin::SystemDpi();
    RECT desired = {0, 0, dpiScale(kClientWidth), dpiScale(kClientHeight)};
    SogouSkin::AdjustWindowRectForDpi(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0,
                                      dpi_);
    const int width = desired.right - desired.left;
    const int height = desired.bottom - desired.top;
    RECT work = {};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = std::max(0, static_cast<int>(work.left) +
                           (static_cast<int>(work.right) -
                            static_cast<int>(work.left) - width) /
                               2);
    int y = std::max(0, static_cast<int>(work.top) +
                           (static_cast<int>(work.bottom) -
                            static_cast<int>(work.top) - height) /
                               2);

    if (g_offscreen) {
        x = -32000;
        y = -32000;
    }

    hwnd_ = ::CreateWindowExW(
        g_offscreen ? WS_EX_TOOLWINDOW : 0, kMainClass,
        L"墨奇输入法 - 搜狗皮肤设置",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, x, y, width, height, nullptr, nullptr,
        instance, this);
    if (!hwnd_) {
        return false;
    }
    dpi_ = SogouSkin::WindowDpi(hwnd_);
    if (g_forced_dpi >= 96) {
        // Test-only: lay the same window out at an arbitrary scale so 100/125/
        // 150/175/200% can be verified without changing the desktop.
        dpi_ = g_forced_dpi;
        RECT forced = {0, 0, dpiScale(kClientWidth), dpiScale(kClientHeight)};
        SogouSkin::AdjustWindowRectForDpi(&forced, WS_OVERLAPPEDWINDOW, FALSE, 0,
                                          dpi_);
        ::SetWindowPos(hwnd_, nullptr, 0, 0, forced.right - forced.left,
                       forced.bottom - forced.top,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    createControls();
    fonts_ = EnumerateInstalledFonts();
    applyFonts();
    layoutControls();
    updateDpiDependentMetrics();

    // Find the skin currently used by the IME.
    std::wstring error;
    auto preferred = SogouSkin::LoadPreferredTheme(&error, &initialSkinPath_);
    if (!preferred || initialSkinPath_.empty()) {
        initialSkinPath_.clear();
    }
    skins_ = EnumerateSkins(initialSkinPath_);
    populateSkinCombo();
    // Re-apply geometry after the combo has items: some Windows versions
    // recompute a CBS_DROPDOWNLIST window height when strings are added.
    layoutControls();
    if (!skins_.empty()) {
        int initial = 0;
        for (int i = 0; i < static_cast<int>(skins_.size()); ++i) {
            if (!initialSkinPath_.empty() &&
                SamePath(skins_[i].path, initialSkinPath_)) {
                initial = i;
                break;
            }
        }
        skinIndex_ = initial;
        ::SendMessageW(skinCombo_, CB_SETCURSEL, initial, 0);
        if (!loadSkin(skins_[initial].path, true, &error)) {
            setStatus(L"无法加载皮肤: " + error);
        }
    } else {
        setStatus(L"没有找到 .ssf 皮肤，请点击“浏览...”选择。");
    }
    ::ShowWindow(hwnd_, SW_SHOW);
    ::UpdateWindow(hwnd_);
    ::SetTimer(hwnd_, 2, 50, nullptr); // initial preview after controls settle
    if (!g_self_test_dir.empty()) {
        ::SetTimer(hwnd_, 3, 150, nullptr);
    }
    return true;
}

void SettingsWindow::createControls() {
    const DWORD groupStyle = WS_CHILD | WS_VISIBLE | BS_GROUPBOX;
    const DWORD labelStyle = WS_CHILD | WS_VISIBLE | SS_LEFT;

    groupPreview_ = ::CreateWindowExW(0, L"BUTTON", L"皮肤预览", groupStyle, 0, 0,
                                       0, 0, hwnd_, nullptr, instance_, nullptr);
    preview_ = ::CreateWindowExW(
        WS_EX_CLIENTEDGE, kPreviewClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
        hwnd_, nullptr, instance_, this);

    groupSkin_ = ::CreateWindowExW(0, L"BUTTON", L"皮肤", groupStyle, 0, 0, 0, 0,
                                   hwnd_, nullptr, instance_, nullptr);
    skinLabel_ = ::CreateWindowExW(0, L"STATIC", L"选择皮肤文件：", labelStyle,
                                   0, 0, 0, 0, hwnd_, nullptr, instance_,
                                   nullptr);
    skinCombo_ = ::CreateWindowExW(
        0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST |
                                  CBS_NOINTEGRALHEIGHT | WS_VSCROLL,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_SKIN_COMBO), instance_,
        nullptr);
    ::SendMessageW(skinCombo_, CB_SETMINVISIBLE, 10, 0);
    browseButton_ = ::CreateWindowExW(
        0, L"BUTTON", L"浏览...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
        hwnd_, reinterpret_cast<HMENU>(IDC_SKIN_BROWSE), instance_, nullptr);

    groupFont_ = ::CreateWindowExW(0, L"BUTTON", L"字体与颜色", groupStyle, 0, 0,
                                   0, 0, hwnd_, nullptr, instance_, nullptr);
    fontChLabel_ = ::CreateWindowExW(0, L"STATIC", L"中文字体：", labelStyle, 0, 0,
                                     0, 0, hwnd_, nullptr, instance_, nullptr);
    fontChCombo_ = ::CreateWindowExW(
        0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST |
                                  CBS_NOINTEGRALHEIGHT | WS_VSCROLL,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_FONT_CH_COMBO), instance_,
        nullptr);
    // The height of a CBS_DROPDOWNLIST window includes the popup list.  A
    // 24px window (the historical bug) leaves no room for the list at all;
    // 220 logical px shows about ten entries and scrolls for the rest.
    ::SendMessageW(fontChCombo_, CB_SETMINVISIBLE, 20, 0);
    ::SendMessageW(fontChCombo_, CB_SETDROPPEDWIDTH, dpiScale(320), 0);

    fontEnLabel_ = ::CreateWindowExW(0, L"STATIC", L"英文字体：", labelStyle, 0, 0,
                                     0, 0, hwnd_, nullptr, instance_, nullptr);
    fontEnCombo_ = ::CreateWindowExW(
        0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST |
                                  CBS_NOINTEGRALHEIGHT | WS_VSCROLL,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_FONT_EN_COMBO), instance_,
        nullptr);
    ::SendMessageW(fontEnCombo_, CB_SETMINVISIBLE, 20, 0);
    ::SendMessageW(fontEnCombo_, CB_SETDROPPEDWIDTH, dpiScale(320), 0);

    fontSizeLabel_ = ::CreateWindowExW(0, L"STATIC", L"字体大小", labelStyle, 0, 0, 0,
                                       0, hwnd_, nullptr, instance_, nullptr);
    fontSlider_ = ::CreateWindowExW(
        0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_FONT_SLIDER), instance_,
        nullptr);
    ::SendMessageW(fontSlider_, TBM_SETRANGE, TRUE,
                   MAKELPARAM(SogouSkin::kMinFontSize, SogouSkin::kMaxFontSize));
    ::SendMessageW(fontSlider_, TBM_SETTICFREQ, 4, 0);
    fontValue_ = ::CreateWindowExW(0, L"STATIC", L"24", labelStyle | SS_CENTER,
                                   0, 0, 0, 0, hwnd_,
                                   reinterpret_cast<HMENU>(IDC_FONT_VALUE), instance_,
                                   nullptr);
    scaleLabel_ = ::CreateWindowExW(0, L"STATIC", L"缩放", labelStyle, 0, 0, 0,
                                    0, hwnd_, nullptr, instance_, nullptr);
    scaleSlider_ = ::CreateWindowExW(
        0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_SCALE_SLIDER), instance_,
        nullptr);
    ::SendMessageW(scaleSlider_, TBM_SETRANGE, TRUE,
                   MAKELPARAM(SogouSkin::kMinScalePercent,
                              SogouSkin::kMaxScalePercent));
    ::SendMessageW(scaleSlider_, TBM_SETTICFREQ, 25, 0);
    scaleValue_ = ::CreateWindowExW(0, L"STATIC", L"100%", labelStyle | SS_CENTER,
                                    0, 0, 0, 0, hwnd_,
                                    reinterpret_cast<HMENU>(IDC_SCALE_VALUE),
                                    instance_, nullptr);
    fontFaceLabel_ = ::CreateWindowExW(
        0, L"STATIC", L"", labelStyle | SS_ENDELLIPSIS, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(IDC_FONT_FACE_LABEL), instance_, nullptr);

    static const wchar_t* colorLabels[kColorCount] = {
        L"拼音颜色", L"首选候选", L"候选文字", L"注释颜色"};
    static const ControlId colorIds[kColorCount] = {
        IDC_COLOR_PINYIN, IDC_COLOR_FIRST, IDC_COLOR_TEXT, IDC_COLOR_COMMENT};
    for (int i = 0; i < kColorCount; ++i) {
        colorLabels_[i] = ::CreateWindowExW(0, L"STATIC", colorLabels[i],
                                            labelStyle, 0, 0, 0, 0, hwnd_,
                                            nullptr, instance_, nullptr);
        colorButtons_[i] = ::CreateWindowExW(
            0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0,
            hwnd_, reinterpret_cast<HMENU>(colorIds[i]), instance_, nullptr);
    }

    groupStatus_ = ::CreateWindowExW(0, L"BUTTON", L"候选与状态栏", groupStyle, 0,
                                     0, 0, 0, hwnd_, nullptr, instance_, nullptr);
    horizontalRadio_ = ::CreateWindowExW(
        0, L"BUTTON", L"候选窗横排", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON |
                                     WS_GROUP,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_HORIZONTAL), instance_,
        nullptr);
    verticalRadio_ = ::CreateWindowExW(
        0, L"BUTTON", L"候选窗竖排",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(IDC_VERTICAL), instance_, nullptr);
    statusbarCheck_ = ::CreateWindowExW(
        0, L"BUTTON", L"显示皮肤状态栏",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(IDC_STATUSBAR), instance_, nullptr);
    statusbarFullscreenCheck_ = ::CreateWindowExW(
        0, L"BUTTON", L"全屏时自动隐藏状态栏",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(IDC_STATUSBAR_FULLSCREEN), instance_, nullptr);
    statusItemsLabel_ = ::CreateWindowExW(
        0, L"STATIC", L"状态栏按钮（勾选后在皮肤上显示）：", labelStyle, 0, 0, 0,
        0, hwnd_, nullptr, instance_, nullptr);
    itemList_ = ::CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW,
        L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                 LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_ITEM_LIST), instance_,
        nullptr);
    ::SendMessageW(itemList_, LVM_SETEXTENDEDLISTVIEWSTYLE,
                   LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT,
                   LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    LVCOLUMNW column = {};
    column.mask = LVCF_WIDTH | LVCF_TEXT;
    column.cx = dpiScale(280);
    column.pszText = const_cast<wchar_t*>(L"状态栏按钮");
    ::SendMessageW(itemList_, LVM_INSERTCOLUMN, 0,
                   reinterpret_cast<LPARAM>(&column));

    groupFineTune_ = ::CreateWindowExW(0, L"BUTTON",
                                       L"文字位置微调（-100–100 设计像素）",
                                       groupStyle, 0, 0, 0, 0, hwnd_, nullptr,
                                       instance_, nullptr);
    static const wchar_t* layoutLabels[4] = {
        L"输入字 X", L"输入字 Y", L"候选词 X", L"候选词 Y"};
    static const ControlId layoutIds[4] = {
        IDC_LAYOUT_PREEDIT_X, IDC_LAYOUT_PREEDIT_Y,
        IDC_LAYOUT_CANDIDATE_X, IDC_LAYOUT_CANDIDATE_Y};
    static const ControlId layoutSpinIds[4] = {
        IDC_LAYOUT_SPIN_PREEDIT_X, IDC_LAYOUT_SPIN_PREEDIT_Y,
        IDC_LAYOUT_SPIN_CANDIDATE_X, IDC_LAYOUT_SPIN_CANDIDATE_Y};
    for (int i = 0; i < 4; ++i) {
        layoutLabels_[i] = ::CreateWindowExW(0, L"STATIC", layoutLabels[i],
                                             labelStyle, 0, 0, 0, 0, hwnd_,
                                             nullptr, instance_, nullptr);
        layoutEdits_[i] = ::CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"0",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_RIGHT,
            0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(layoutIds[i]), instance_,
            nullptr);
        ::SendMessageW(layoutEdits_[i], EM_SETLIMITTEXT, 4, 0);
        layoutSpins_[i] = ::CreateWindowExW(
            0, UPDOWN_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | UDS_ARROWKEYS | UDS_SETBUDDYINT |
                UDS_NOTHOUSANDS,
            0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(layoutSpinIds[i]), instance_, nullptr);
        ::SendMessageW(layoutSpins_[i], UDM_SETRANGE32,
                       static_cast<WPARAM>(-100), static_cast<LPARAM>(100));
        ::SendMessageW(layoutSpins_[i], UDM_SETBUDDY,
                       reinterpret_cast<WPARAM>(layoutEdits_[i]), 0);
        ::SendMessageW(layoutSpins_[i], UDM_SETPOS32, 0, 0);
    }

    applyButton_ = ::CreateWindowExW(0, L"BUTTON", L"应用",
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0,
                                     0, hwnd_, reinterpret_cast<HMENU>(IDC_APPLY),
                                     instance_, nullptr);
    okButton_ = ::CreateWindowExW(0, L"BUTTON", L"确定",
                                  WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0,
                                  0, hwnd_, reinterpret_cast<HMENU>(IDC_OK), instance_,
                                  nullptr);
    cancelButton_ = ::CreateWindowExW(
        0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
        hwnd_, reinterpret_cast<HMENU>(IDC_CANCEL), instance_, nullptr);
    resetButton_ = ::CreateWindowExW(
        0, L"BUTTON", L"恢复皮肤默认", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0,
        0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_RESET), instance_, nullptr);
    statusLabel_ = ::CreateWindowExW(
        0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0,
        0, hwnd_, reinterpret_cast<HMENU>(IDC_STATUS_LABEL), instance_, nullptr);
}

void SettingsWindow::layoutControls() {
    auto rect = [&](int x, int y, int w, int h) {
        // Scale the final edges, not a rounded "1 px" unit.  At 150% a
        // dpiScale(1) prefix rounds to 2 and overflows the window by 33%.
        return RECT{dpiScale(x), dpiScale(y), dpiScale(x + w), dpiScale(y + h)};
    };
    auto move = [](HWND control, const RECT& r) {
        if (control) {
            ::MoveWindow(control, r.left, r.top, r.right - r.left,
                         r.bottom - r.top, TRUE);
        }
    };

    move(groupPreview_, rect(14, 14, 570, 570));
    move(groupSkin_, rect(600, 14, 346, 100));
    move(groupFont_, rect(600, 122, 346, 216));
    move(groupStatus_, rect(600, 346, 346, 250));

    move(preview_, rect(24, 44, 550, 520));
    // For CBS_DROPDOWNLIST the window height includes the drop-down list.
    // 220 logical px leaves enough room for 10 items (CB_SETMINVISIBLE=10);
    // the old 24 px clipped the list to almost nothing.
    move(skinLabel_, rect(614, 38, 248, 18));
    move(skinCombo_, rect(614, 58, 248, 220));
    move(browseButton_, rect(868, 58, 64, 24));
    move(fontChLabel_, rect(614, 140, 64, 20));
    move(fontChCombo_, rect(680, 136, 252, 220));
    move(fontEnLabel_, rect(614, 168, 64, 20));
    move(fontEnCombo_, rect(680, 164, 252, 220));
    move(scaleLabel_, rect(614, 198, 40, 20));
    move(scaleSlider_, rect(664, 194, 172, 28));
    move(scaleValue_, rect(842, 197, 42, 22));
    move(fontSizeLabel_, rect(614, 226, 50, 20));
    move(fontSlider_, rect(664, 224, 172, 28));
    move(fontValue_, rect(842, 227, 42, 22));
    move(fontFaceLabel_, rect(614, 258, 318, 16));

    static const ControlId colorIds[kColorCount] = {
        IDC_COLOR_PINYIN, IDC_COLOR_FIRST, IDC_COLOR_TEXT, IDC_COLOR_COMMENT};
    for (int i = 0; i < kColorCount; ++i) {
        // A 2x2 grid keeps the four colour rows inside the existing group
        // box now that the two font drop-downs and two sliders occupy the
        // first rows.
        const int column = i % 2;
        const int row = i / 2;
        const int x = 614 + column * 158;
        const int y = 282 + row * 30;
        move(colorLabels_[i], rect(x, y, 60, 24));
        move(colorButtons_[i], rect(x + 64, y, 80, 24));
    }

    move(horizontalRadio_, rect(614, 368, 110, 22));
    move(verticalRadio_, rect(730, 368, 110, 22));
    move(statusbarCheck_, rect(614, 396, 160, 22));
    move(statusbarFullscreenCheck_, rect(780, 396, 166, 22));
    move(statusItemsLabel_, rect(614, 422, 318, 18));
    move(itemList_, rect(614, 446, 318, 138));

    move(groupFineTune_, rect(14, 588, 570, 46));
    {
        const int labelX[4] = {26, 152, 278, 404};
        const int labelW = 58;
        const int editW = 64;
        const int spinW = 16;
        for (int i = 0; i < 4; ++i) {
            const int editX = labelX[i] + labelW;
            move(layoutLabels_[i], rect(labelX[i], 606, labelW, 20));
            move(layoutEdits_[i], rect(editX, 604, editW, 23));
            move(layoutSpins_[i],
                 rect(editX + editW - spinW, 604, spinW, 23));
        }
    }

    move(applyButton_, rect(600, 640, 78, 28));
    move(okButton_, rect(686, 640, 78, 28));
    move(cancelButton_, rect(772, 640, 78, 28));
    move(resetButton_, rect(858, 640, 88, 28));
    move(statusLabel_, rect(14, 638, 570, 22));
}

bool SettingsWindow::loadSkin(const std::wstring& path, bool useSavedOverrides,
                              std::wstring* error) {
    std::wstring loadError;
    auto base = SogouSkin::LoadThemeWithOverrides(path, L"", &loadError);
    if (!base) {
        if (error) *error = loadError.empty() ? L"无法读取皮肤" : loadError;
        return false;
    }
    std::shared_ptr<SogouSkin::Theme> effective = base;
    if (useSavedOverrides) {
        const std::wstring saved = SogouSkin::OverridesPath();
        if (!saved.empty() && FileExists(saved)) {
            std::wstring effectiveError;
            auto loaded = SogouSkin::LoadThemeWithOverrides(path, saved,
                                                            &effectiveError);
            if (loaded) {
                effective = loaded;
            }
        }
    }

    skinPath_ = path;
    baseTheme_ = base;
    effectiveTheme_ = effective;
    baseFontSize_ = std::max(1, base->display().fontSize);
    fontSize_ = std::max(1, effective->display().fontSize);
    scalePercent_ = std::clamp(effective->display().scalePercent,
                               SogouSkin::kMinScalePercent,
                               SogouSkin::kMaxScalePercent);
    layout_ = effective->layout();
    fontCh_ = effective->display().fontCh;
    fontEn_ = effective->display().fontEn;
    colors_[0] = effective->display().pinyinColor;
    colors_[1] = effective->display().firstColor;
    colors_[2] = effective->display().textColor;
    colors_[3] = effective->display().commentColor;

    const auto options = SogouSkin::LoadOptions();
    horizontal_ = options.horizontal;
    statusbar_ = options.statusbar;
    statusbarFullscreenHide_ = options.statusbarFullscreenHide;

    itemNames_.clear();
    itemVisible_.clear();
    const auto& baseItems = base->statusBar().items;
    for (const auto& baseItem : baseItems) {
        itemNames_.push_back(baseItem.name);
        bool visible = baseItem.display;
        for (const auto& effectiveItem : effective->statusBar().items) {
            if (_wcsicmp(effectiveItem.name.c_str(), baseItem.name.c_str()) == 0) {
                visible = effectiveItem.display;
                break;
            }
        }
        for (const auto& hidden : options.hiddenStatusItems) {
            if (_wcsicmp(hidden.c_str(), baseItem.name.c_str()) == 0) {
                visible = false;
                break;
            }
        }
        itemVisible_.push_back(visible);
    }

    syncControlsFromState();
    populateStatusItems();
    schedulePreview();
    return true;
}

void SettingsWindow::populateSkinCombo() {
    ::SendMessageW(skinCombo_, CB_RESETCONTENT, 0, 0);
    for (const auto& skin : skins_) {
        ::SendMessageW(skinCombo_, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(skin.label.c_str()));
    }
}

void SettingsWindow::populateStatusItems() {
    updatingList_ = true;
    ::SendMessageW(itemList_, LVM_DELETEALLITEMS, 0, 0);
    for (int i = 0; i < static_cast<int>(itemNames_.size()); ++i) {
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = const_cast<wchar_t*>(FriendlyItemName(itemNames_[i]).c_str());
        ::SendMessageW(itemList_, LVM_INSERTITEM, 0,
                       reinterpret_cast<LPARAM>(&item));
        LVITEMW state = {};
        state.mask = LVIF_STATE;
        state.state = INDEXTOSTATEIMAGEMASK(itemVisible_[i] ? 2 : 1);
        state.stateMask = LVIS_STATEIMAGEMASK;
        ::SendMessageW(itemList_, LVM_SETITEMSTATE, i,
                       reinterpret_cast<LPARAM>(&state));
    }
    updatingList_ = false;
}

void SettingsWindow::populateFontCombo(HWND combo, const std::wstring& current) {
    if (!combo) {
        return;
    }
    ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int selected = -1;
    for (size_t i = 0; i < fonts_.size(); ++i) {
        const int index = static_cast<int>(::SendMessageW(
            combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(fonts_[i].c_str())));
        if (index >= 0 && !current.empty() &&
            _wcsicmp(fonts_[i].c_str(), current.c_str()) == 0) {
            selected = index;
        }
    }
    if (selected < 0 && !current.empty()) {
        // The skin may reference a font that is not installed on this
        // machine (or is installed only as a private skin-editor font).
        // Keep the name selectable/visible instead of silently showing the
        // first font in the list.
        selected = static_cast<int>(::SendMessageW(
            combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(current.c_str())));
        AppendUniqueFontName(&fonts_, current);
    }
    if (selected < 0 && ::SendMessageW(combo, CB_GETCOUNT, 0, 0) > 0) {
        selected = 0;
    }
    ::SendMessageW(combo, CB_SETCURSEL,
                   selected < 0 ? static_cast<WPARAM>(-1)
                                : static_cast<WPARAM>(selected),
                   0);
}

std::wstring SettingsWindow::selectedFont(HWND combo) const {
    if (!combo) {
        return {};
    }
    const int index =
        static_cast<int>(::SendMessageW(combo, CB_GETCURSEL, 0, 0));
    return ComboItemText(combo, index);
}

std::wstring SettingsWindow::chooseDifferentFont(HWND combo,
                                                 const wchar_t* preferred) {
    if (!combo) {
        return {};
    }
    const std::wstring current = selectedFont(combo);
    int chosen = -1;
    if (preferred && *preferred) {
        const int index = static_cast<int>(::SendMessageW(
            combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
            reinterpret_cast<LPARAM>(preferred)));
        if (index >= 0) {
            const std::wstring candidate = ComboItemText(combo, index);
            if (_wcsicmp(candidate.c_str(), current.c_str()) != 0) {
                chosen = index;
            }
        }
    }
    if (chosen < 0) {
        const int count =
            static_cast<int>(::SendMessageW(combo, CB_GETCOUNT, 0, 0));
        for (int i = 0; i < count; ++i) {
            const std::wstring candidate = ComboItemText(combo, i);
            if (_wcsicmp(candidate.c_str(), current.c_str()) != 0) {
                chosen = i;
                break;
            }
        }
    }
    if (chosen < 0) {
        return current;
    }
    ::SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(chosen), 0);
    ::SendMessageW(hwnd_, WM_COMMAND,
                   MAKEWPARAM(static_cast<WORD>(::GetDlgCtrlID(combo)),
                              CBN_SELCHANGE),
                   reinterpret_cast<LPARAM>(combo));
    return selectedFont(combo);
}

void SettingsWindow::updateFontFaceLabel() {
    if (!fontFaceLabel_) {
        return;
    }
    const std::wstring ch = fontCh_.empty() ? L"皮肤默认" : fontCh_;
    const std::wstring en = fontEn_.empty() ? L"皮肤默认" : fontEn_;
    const std::wstring text =
        L"当前字体：中文 " + ch + L" / 英文 " + en +
        L"（字体大小与整窗缩放相互独立）";
    ::SetWindowTextW(fontFaceLabel_, text.c_str());
}

void SettingsWindow::syncControlsFromState() {
    updatingControls_ = true;
    ::SendMessageW(fontSlider_, TBM_SETPOS, TRUE, fontSize_);
    wchar_t sizeText[16] = {};
    swprintf_s(sizeText, L"%d", fontSize_);
    ::SetWindowTextW(fontValue_, sizeText);
    ::SendMessageW(scaleSlider_, TBM_SETPOS, TRUE, scalePercent_);
    wchar_t scaleText[16] = {};
    swprintf_s(scaleText, L"%d%%", scalePercent_);
    ::SetWindowTextW(scaleValue_, scaleText);
    syncLayoutEdits();
    populateFontCombo(fontChCombo_, fontCh_);
    populateFontCombo(fontEnCombo_, fontEn_);
    updateFontFaceLabel();
    for (int i = 0; i < kColorCount; ++i) {
        updateColorButton(i);
    }
    ::SendMessageW(horizontalRadio_, BM_SETCHECK,
                   horizontal_ ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(verticalRadio_, BM_SETCHECK,
                   horizontal_ ? BST_UNCHECKED : BST_CHECKED, 0);
    ::SendMessageW(statusbarCheck_, BM_SETCHECK,
                   statusbar_ ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(statusbarFullscreenCheck_, BM_SETCHECK,
                   statusbarFullscreenHide_ ? BST_CHECKED : BST_UNCHECKED, 0);
    updatingControls_ = false;
}

void SettingsWindow::syncLayoutEdits() {
    const int values[4] = {layout_.preeditX, layout_.preeditY,
                           layout_.candidateX, layout_.candidateY};
    for (int i = 0; i < 4; ++i) {
        wchar_t text[16] = {};
        swprintf_s(text, L"%d", values[i]);
        ::SetWindowTextW(layoutEdits_[i], text);
    }
    syncLayoutSpins();
}

void SettingsWindow::syncLayoutSpins() {
    const int values[4] = {layout_.preeditX, layout_.preeditY,
                           layout_.candidateX, layout_.candidateY};
    for (int i = 0; i < 4; ++i) {
        if (layoutSpins_[i]) {
            ::SendMessageW(layoutSpins_[i], UDM_SETPOS32, 0, values[i]);
        }
    }
}

void SettingsWindow::onLayoutSpinDelta(int index, int delta, int position) {
    if (updatingControls_ || index < 0 || index >= 4) {
        return;
    }
    const int clamped = std::clamp(position + delta, -100, 100);
    int* target = nullptr;
    switch (index) {
    case 0: target = &layout_.preeditX; break;
    case 1: target = &layout_.preeditY; break;
    case 2: target = &layout_.candidateX; break;
    default: target = &layout_.candidateY; break;
    }
    if (*target == clamped) {
        return;
    }
    *target = clamped;
    updatingControls_ = true;
    wchar_t text[16] = {};
    swprintf_s(text, L"%d", clamped);
    ::SetWindowTextW(layoutEdits_[index], text);
    ::SendMessageW(layoutSpins_[index], UDM_SETPOS32, 0, clamped);
    updatingControls_ = false;
    schedulePreview();
}

void SettingsWindow::onLayoutEditChanged(int index) {
    if (updatingControls_ || index < 0 || index >= 4) {
        return;
    }
    wchar_t text[16] = {};
    ::GetWindowTextW(layoutEdits_[index], text, _countof(text));
    wchar_t* end = nullptr;
    const long value = std::wcstol(text, &end, 10);
    if (!end || end == text || *end != L'\0') {
        return; // intermediate "-" / empty while the user is typing
    }
    const int clamped = std::clamp(static_cast<int>(value), -100, 100);
    int* target = nullptr;
    switch (index) {
    case 0: target = &layout_.preeditX; break;
    case 1: target = &layout_.preeditY; break;
    case 2: target = &layout_.candidateX; break;
    default: target = &layout_.candidateY; break;
    }
    if (*target != clamped) {
        *target = clamped;
        schedulePreview();
    }
}

void SettingsWindow::normalizeLayoutEdit(int index) {
    if (index < 0 || index >= 4) {
        return;
    }
    // Reformat the field so "-", empty and out-of-range input snaps back to
    // the stored value when focus leaves.
    updatingControls_ = true;
    const int values[4] = {layout_.preeditX, layout_.preeditY,
                           layout_.candidateX, layout_.candidateY};
    wchar_t text[16] = {};
    swprintf_s(text, L"%d", values[index]);
    ::SetWindowTextW(layoutEdits_[index], text);
    updatingControls_ = false;
}

void SettingsWindow::updateColorButton(int index) {
    if (index < 0 || index >= kColorCount || !colorButtons_[index]) {
        return;
    }
    const std::wstring text = FormatColorLabel(colors_[index]);
    ::SetWindowTextW(colorButtons_[index], text.c_str());
    ::InvalidateRect(colorButtons_[index], nullptr, TRUE);
}

void SettingsWindow::chooseColor(int index) {
    if (index < 0 || index >= kColorCount) {
        return;
    }
    CHOOSECOLORW cc = {};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = hwnd_;
    cc.rgbResult = colors_[index];
    cc.lpCustColors = customColors_;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (::ChooseColorW(&cc)) {
        colors_[index] = cc.rgbResult;
        updateColorButton(index);
        schedulePreview();
    }
}

void SettingsWindow::schedulePreview() {
    if (previewTimer_) {
        ::KillTimer(hwnd_, previewTimer_);
    }
    previewTimer_ = ::SetTimer(hwnd_, 1, 120, nullptr);
}

SogouSkin::SettingsOverrides SettingsWindow::buildSettingsValues() const {
    SogouSkin::SettingsOverrides values;
    values.fontSize = fontSize_;
    values.scalePercent = scalePercent_;
    values.fontCh = fontCh_;
    values.fontEn = fontEn_;
    values.pinyinColor = colors_[0];
    values.firstColor = colors_[1];
    values.textColor = colors_[2];
    values.commentColor = colors_[3];
    values.statusItems.reserve(itemNames_.size());
    for (int i = 0; i < static_cast<int>(itemNames_.size()); ++i) {
        values.statusItems.emplace_back(itemNames_[i], itemVisible_[i]);
    }
    return values;
}

std::wstring SettingsWindow::buildOverridesText(int* removedLegacyKeys) const {
    if (!baseTheme_) {
        return {};
    }
    // Font size, global scale and per-skin text nudges are the geometric
    // values written by this UI.  BuildSettingsOverridesText migrates the old
    // combined font_size semantics and layout keys generated by the old
    // per-metric UI, while preserving hand-edited advanced keys.
    return SogouSkin::BuildSettingsOverridesText(
        *baseTheme_, SogouSkin::OverridesPath(), buildSettingsValues(),
        removedLegacyKeys);
}

void SettingsWindow::refreshPreview() {
    previewTimer_ = 0;
    previewError_.clear();
    previewBitmap_.reset();
    previewBitmapWidth_ = 0;
    previewBitmapHeight_ = 0;
    if (!baseTheme_ || skinPath_.empty()) {
        ::InvalidateRect(preview_, nullptr, TRUE);
        return;
    }

    wchar_t tempDir[MAX_PATH] = {};
    wchar_t tempFile[MAX_PATH] = {};
    ::GetTempPathW(_countof(tempDir), tempDir);
    swprintf_s(tempFile, L"%smoqi_sogou_preview_%lu.ini", tempDir,
               static_cast<unsigned long>(::GetCurrentProcessId()));
    tempOverridesPath_ = tempFile;

    // Rebuild a clean override file: keep manual advanced keys, drop legacy
    // per-metric font scaling, then apply the values owned by this UI.
    const std::wstring overrides = buildOverridesText();
    if (!WriteAllBytes(tempOverridesPath_, Utf16LeBytes(overrides))) {
        previewError_ = L"无法写入预览配置";
        ::InvalidateRect(preview_, nullptr, TRUE);
        return;
    }

    std::wstring error;
    auto theme = SogouSkin::LoadThemeWithOverrides(skinPath_, tempOverridesPath_,
                                                   &error);
    if (!theme) {
        previewError_ = L"预览失败: " + error;
        ::InvalidateRect(preview_, nullptr, TRUE);
        return;
    }
    // Layout nudges are per-skin values stored outside the override INI.  The
    // preview uses the unsaved in-memory values so the sliders are live.
    theme->setLayout(layout_);
    SogouSkin::Renderer renderer(theme);
    renderer.setDpi(static_cast<int>(dpi_));
    SogouSkin::RenderState state;
    state.preedit = L"moqi shurufa";
    state.candidates = {L"墨奇输入法", L"墨奇", L"默契", L"默祈", L"摩奇"};
    state.selKeys = L"12345";
    state.candPerRow = horizontal_ ? 9 : 1;
    state.currentSel = 0;
    state.useCursor = true;
    int width = 0;
    int height = 0;
    auto bitmap = renderer.renderToBitmap(state, &width, &height);
    if (!bitmap) {
        previewError_ = L"当前皮肤无法渲染预览";
        ::InvalidateRect(preview_, nullptr, TRUE);
        return;
    }
    previewBitmap_ = bitmap;
    previewBitmapWidth_ = width;
    previewBitmapHeight_ = height;
    ::InvalidateRect(preview_, nullptr, TRUE);
}

void SettingsWindow::paintPreview(HDC dc) {
    RECT client = {};
    ::GetClientRect(preview_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

    // Checkerboard makes PNG transparency visible.
    const int cell = std::max(8, dpiScale(10));
    for (int y = 0; y < height; y += cell) {
        for (int x = 0; x < width; x += cell) {
            const bool dark = ((x / cell) + (y / cell)) % 2 != 0;
            Gdiplus::SolidBrush brush(Gdiplus::Color(
                255, dark ? 224 : 240, dark ? 224 : 240, dark ? 224 : 240));
            graphics.FillRectangle(&brush, x, y, cell, cell);
        }
    }

    if (!previewBitmap_) {
        if (!previewError_.empty()) {
            Gdiplus::SolidBrush brush(Gdiplus::Color(255, 120, 60, 60));
            Gdiplus::Font font(L"Microsoft YaHei UI", static_cast<float>(dpiScale(10)),
                               Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            graphics.DrawString(previewError_.c_str(), -1, &font,
                                Gdiplus::PointF(16.0f, 16.0f), &brush);
        }
        return;
    }

    const int margin = dpiScale(12);
    const double scale = std::min(
        1.0, std::min(static_cast<double>(width - margin * 2) / previewBitmapWidth_,
                      static_cast<double>(height - margin * 2) / previewBitmapHeight_));
    const int drawWidth =
        std::max(1, static_cast<int>(std::lround(previewBitmapWidth_ * scale)));
    const int drawHeight =
        std::max(1, static_cast<int>(std::lround(previewBitmapHeight_ * scale)));
    const int x = (width - drawWidth) / 2;
    const int y = (height - drawHeight) / 2;
    graphics.DrawImage(previewBitmap_.get(), x, y, drawWidth, drawHeight);

    wchar_t sizeText[64] = {};
    swprintf_s(sizeText, L"%d × %d px", previewBitmapWidth_, previewBitmapHeight_);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 90, 90, 90));
    Gdiplus::Font font(L"Microsoft YaHei UI", static_cast<float>(dpiScale(9)),
                       Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    graphics.DrawString(sizeText, -1, &font, Gdiplus::PointF(8.0f, 6.0f),
                        &textBrush);
}

void SettingsWindow::setStatus(const std::wstring& text) {
    if (statusLabel_) {
        ::SetWindowTextW(statusLabel_, text.c_str());
    }
}

std::wstring SettingsWindow::previewHash() const {
    if (!previewBitmap_) {
        return L"none";
    }
    const int width = static_cast<int>(previewBitmap_->GetWidth());
    const int height = static_cast<int>(previewBitmap_->GetHeight());
    if (width <= 0 || height <= 0) {
        return L"empty";
    }
    Gdiplus::BitmapData data = {};
    Gdiplus::Rect rect(0, 0, width, height);
    if (previewBitmap_->LockBits(&rect, Gdiplus::ImageLockModeRead,
                                 PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
        return L"lock-failed";
    }
    unsigned long long hash = 1469598103934665603ULL;
    const auto* base = static_cast<const unsigned char*>(data.Scan0);
    for (int y = 0; y < height; ++y) {
        const unsigned char* row = base + static_cast<ptrdiff_t>(y) * data.Stride;
        for (int x = 0; x < width * 4; ++x) {
            hash ^= row[x];
            hash *= 1099511628211ULL;
        }
    }
    previewBitmap_->UnlockBits(&data);
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%016llx", hash);
    return buffer;
}

bool SettingsWindow::savePreviewBitmap(const std::wstring& path) {
    return SaveBitmapPng(previewBitmap_.get(), path);
}

bool SettingsWindow::captureWindowToPng(const std::wstring& path) {
    return CaptureWindowToPng(hwnd_, path);
}

void SettingsWindow::runSelfTest() {
    const std::wstring dir = g_self_test_dir;
    if (dir.empty()) {
        return;
    }
    ::CreateDirectoryW(dir.c_str(), nullptr);

    std::wostringstream report;
    report << L"combo_count=" << skins_.size() << L"\n";
    RECT comboRect = {};
    ::GetWindowRect(skinCombo_, &comboRect);
    report << L"combo_window_height=" << (comboRect.bottom - comboRect.top)
           << L"\n";
    report << L"combo_min_visible="
           << ::SendMessageW(skinCombo_, CB_GETMINVISIBLE, 0, 0) << L"\n";
    COMBOBOXINFO comboInfo = {};
    comboInfo.cbSize = sizeof(comboInfo);
    int dropdownHeight = 0;
    if (::GetComboBoxInfo(skinCombo_, &comboInfo) && comboInfo.hwndList) {
        ::SendMessageW(skinCombo_, CB_SHOWDROPDOWN, TRUE, 0);
        RECT listRect = {};
        ::GetWindowRect(comboInfo.hwndList, &listRect);
        dropdownHeight = listRect.bottom - listRect.top;
        ::SendMessageW(skinCombo_, CB_SHOWDROPDOWN, FALSE, 0);
    }
    report << L"dropdown_list_height=" << dropdownHeight << L"\n";

    std::vector<std::wstring> signatures;
    for (int i = 0; i < static_cast<int>(skins_.size()); ++i) {
        ::SendMessageW(skinCombo_, CB_SETCURSEL, i, 0);
        ::SendMessageW(hwnd_, WM_COMMAND,
                       MAKEWPARAM(IDC_SKIN_COMBO, CBN_SELCHANGE),
                       reinterpret_cast<LPARAM>(skinCombo_));
        if (previewTimer_) {
            ::KillTimer(hwnd_, previewTimer_);
            previewTimer_ = 0;
        }
        refreshPreview();

        wchar_t label[512] = {};
        ::SendMessageW(skinCombo_, CB_GETLBTEXT, i,
                       reinterpret_cast<LPARAM>(label));
        const std::wstring hash = previewHash();
        report << L"item=" << i << L" label=" << label
               << L" path=" << skins_[i].path
               << L" font_ch=" << selectedFont(fontChCombo_)
               << L" font_en=" << selectedFont(fontEnCombo_)
               << L" preview=" << previewBitmapWidth_ << L"x"
               << previewBitmapHeight_ << L" hash=" << hash << L"\n";

        savePreviewBitmap(dir + L"\\settings_preview_" +
                          std::to_wstring(i) + L".png");
        captureWindowToPng(dir + L"\\settings_window_" + std::to_wstring(i) +
                           L".png");

        signatures.push_back(std::to_wstring(previewBitmapWidth_) + L"x" +
                             std::to_wstring(previewBitmapHeight_) + L"/" + hash);
    }

    // Capacity probe: temporarily append dummy strings so the drop-down has
    // more than 8 entries, then measure the real popup list height.  This
    // verifies the combo geometry even when the isolated profile only
    // contains 2-3 test skins.
    int itemHeight = static_cast<int>(
        ::SendMessageW(skinCombo_, CB_GETITEMHEIGHT, 0, 0));
    if (itemHeight <= 0) {
        itemHeight = 16;
    }
    for (int i = 0; i < 8; ++i) {
        ::SendMessageW(skinCombo_, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(L"__capacity_probe__"));
    }
    int capacityHeight = 0;
    COMBOBOXINFO capacityInfo = {};
    capacityInfo.cbSize = sizeof(capacityInfo);
    if (::GetComboBoxInfo(skinCombo_, &capacityInfo) && capacityInfo.hwndList) {
        ::SendMessageW(skinCombo_, CB_SHOWDROPDOWN, TRUE, 0);
        RECT listRect = {};
        ::GetWindowRect(capacityInfo.hwndList, &listRect);
        capacityHeight = listRect.bottom - listRect.top;
        CaptureWindowToPng(capacityInfo.hwndList,
                           dir + L"\\settings_dropdown_open.png");
        ::SendMessageW(skinCombo_, CB_SHOWDROPDOWN, FALSE, 0);
    }
    report << L"capacity_item_height=" << itemHeight << L"\n";
    report << L"capacity_list_height=" << capacityHeight << L"\n";
    report << L"capacity_items_visible="
           << (itemHeight > 0 ? capacityHeight / itemHeight : 0) << L"\n";
    const bool capacityOk =
        capacityHeight >= itemHeight * 8;
    // Restore the real skin list.
    ::SendMessageW(skinCombo_, CB_RESETCONTENT, 0, 0);
    populateSkinCombo();
    if (!skins_.empty()) {
        ::SendMessageW(skinCombo_, CB_SETCURSEL, 0, 0);
    }

    // Exercise the same path as "浏览... + 应用": applying the first skin must
    // refresh sogou_skin_recent.txt with that path.
    bool recentOk = false;
    if (!skins_.empty()) {
        std::wstring applyError;
        if (loadSkin(skins_[0].path, false, &applyError)) {
            skinPath_ = skins_[0].path;
            const bool applied = apply(false);
            std::vector<unsigned char> recentBytes;
            const std::wstring recentText =
                ReadAllBytes(RecentSkinsPath(), &recentBytes)
                    ? DecodeTextBytes(recentBytes)
                    : std::wstring();
            recentOk = applied &&
                       recentText.find(skins_[0].path) != std::wstring::npos;
        }
    }

    // ---- Font controls -------------------------------------------------
    // Reload the first skin so the font test starts from factory faces, then
    // exercise the exact same state -> preview -> apply path as the UI.
    bool fontLoaded = false;
    if (!skins_.empty()) {
        std::wstring fontLoadError;
        fontLoaded = loadSkin(skins_[0].path, false, &fontLoadError);
    }
    if (previewTimer_) {
        ::KillTimer(hwnd_, previewTimer_);
        previewTimer_ = 0;
    }
    refreshPreview();
    const std::wstring fontBeforeHash = previewHash();
    const int fontBeforeWidth = previewBitmapWidth_;
    const int fontBeforeHeight = previewBitmapHeight_;
    savePreviewBitmap(dir + L"\\settings_font_before.png");

    const int fontChCount =
        static_cast<int>(::SendMessageW(fontChCombo_, CB_GETCOUNT, 0, 0));
    const int fontEnCount =
        static_cast<int>(::SendMessageW(fontEnCombo_, CB_GETCOUNT, 0, 0));
    const std::wstring fontChBefore = selectedFont(fontChCombo_);
    const std::wstring fontEnBefore = selectedFont(fontEnCombo_);
    report << L"font_ch_count=" << fontChCount << L"\n";
    report << L"font_en_count=" << fontEnCount << L"\n";
    report << L"font_ch_before=" << fontChBefore << L"\n";
    report << L"font_en_before=" << fontEnBefore << L"\n";

    int fontItemHeight = static_cast<int>(
        ::SendMessageW(fontChCombo_, CB_GETITEMHEIGHT, 0, 0));
    if (fontItemHeight <= 0) {
        fontItemHeight = 16;
    }
    int fontDropdownHeight = 0;
    COMBOBOXINFO fontComboInfo = {};
    fontComboInfo.cbSize = sizeof(fontComboInfo);
    if (::GetComboBoxInfo(fontChCombo_, &fontComboInfo) &&
        fontComboInfo.hwndList) {
        ::SendMessageW(fontChCombo_, CB_SHOWDROPDOWN, TRUE, 0);
        RECT listRect = {};
        ::GetWindowRect(fontComboInfo.hwndList, &listRect);
        fontDropdownHeight = listRect.bottom - listRect.top;
        CaptureWindowToPng(fontComboInfo.hwndList,
                           dir + L"\\settings_font_dropdown_open.png");
        ::SendMessageW(fontChCombo_, CB_SHOWDROPDOWN, FALSE, 0);
    }
    const bool fontDropdownOk =
        fontDropdownHeight >= fontItemHeight * 8;
    report << L"font_item_height=" << fontItemHeight << L"\n";
    report << L"font_dropdown_list_height=" << fontDropdownHeight << L"\n";
    report << L"font_items_visible="
           << (fontItemHeight > 0 ? fontDropdownHeight / fontItemHeight : 0)
           << L"\n";
    report << L"font_dropdown_ok=" << (fontDropdownOk ? 1 : 0) << L"\n";

    // GetComboBoxInfo().rcItem is the closed item rectangle; the window
    // rectangle intentionally includes the popup and would produce false
    // overlaps with the controls below.
    auto windowRect = [](HWND control, RECT* rect) -> bool {
        return control && ::GetWindowRect(control, rect) != FALSE;
    };
    auto comboItemRect = [this](HWND combo, RECT* rect) -> bool {
        RECT window = {};
        if (!combo || !::GetWindowRect(combo, &window)) {
            return false;
        }
        int itemHeight = static_cast<int>(
            ::SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0));
        if (itemHeight <= 0) {
            itemHeight = 16;
        }
        // The combo window height includes the popup list.  Only the closed
        // item area participates in layout-overlap checks.
        *rect = window;
        rect->bottom = rect->top + itemHeight + dpiScale(8);
        return true;
    };
    auto intersects = [](const RECT& left, const RECT& right) {
        return left.left < right.right && right.left < left.right &&
               left.top < right.bottom && right.top < left.bottom;
    };
    RECT chItemRect = {};
    RECT enItemRect = {};
    RECT sliderRect = {};
    RECT valueRect = {};
    RECT scaleSliderRect = {};
    RECT scaleValueRect = {};
    RECT faceRect = {};
    bool layoutOverlapOk =
        comboItemRect(fontChCombo_, &chItemRect) &&
        comboItemRect(fontEnCombo_, &enItemRect) &&
        windowRect(fontSlider_, &sliderRect) &&
        windowRect(fontValue_, &valueRect) &&
        windowRect(scaleSlider_, &scaleSliderRect) &&
        windowRect(scaleValue_, &scaleValueRect) &&
        windowRect(fontFaceLabel_, &faceRect);
    if (layoutOverlapOk) {
        layoutOverlapOk =
            !intersects(chItemRect, enItemRect) &&
            !intersects(chItemRect, sliderRect) &&
            !intersects(chItemRect, valueRect) &&
            !intersects(chItemRect, scaleSliderRect) &&
            !intersects(chItemRect, scaleValueRect) &&
            !intersects(chItemRect, faceRect) &&
            !intersects(enItemRect, sliderRect) &&
            !intersects(enItemRect, valueRect) &&
            !intersects(enItemRect, scaleSliderRect) &&
            !intersects(enItemRect, scaleValueRect) &&
            !intersects(enItemRect, faceRect) &&
            !intersects(sliderRect, scaleSliderRect) &&
            !intersects(valueRect, scaleValueRect);
        for (int i = 0; i < kColorCount && layoutOverlapOk; ++i) {
            RECT labelRect = {};
            RECT buttonRect = {};
            if (!windowRect(colorLabels_[i], &labelRect) ||
                !windowRect(colorButtons_[i], &buttonRect)) {
                layoutOverlapOk = false;
                break;
            }
            layoutOverlapOk =
                !intersects(chItemRect, labelRect) &&
                !intersects(chItemRect, buttonRect) &&
                !intersects(enItemRect, labelRect) &&
                !intersects(enItemRect, buttonRect) &&
                !intersects(sliderRect, labelRect) &&
                !intersects(sliderRect, buttonRect) &&
                !intersects(scaleSliderRect, labelRect) &&
                !intersects(scaleSliderRect, buttonRect);
        }
        // The four nudge fields and their up-down controls must fit side by
        // side inside the fine-tune group without overlapping each other.
        for (int i = 0; i < 4 && layoutOverlapOk; ++i) {
            RECT editRect = {};
            if (!windowRect(layoutEdits_[i], &editRect)) {
                layoutOverlapOk = false;
                break;
            }
            for (int j = 0; j < i; ++j) {
                RECT otherRect = {};
                if (!windowRect(layoutEdits_[j], &otherRect) ||
                    intersects(otherRect, editRect)) {
                    layoutOverlapOk = false;
                    break;
                }
            }
        }
    }

    bool spinOk = true;
    for (int i = 0; i < 4; ++i) {
        if (!layoutSpins_[i] || !layoutEdits_[i] ||
            !::IsWindowVisible(layoutSpins_[i])) {
            spinOk = false;
            break;
        }
        const HWND buddy = reinterpret_cast<HWND>(
            ::SendMessageW(layoutSpins_[i], UDM_GETBUDDY, 0, 0));
        int low = 0;
        int high = 0;
        ::SendMessageW(layoutSpins_[i], UDM_GETRANGE32,
                       reinterpret_cast<WPARAM>(&low),
                       reinterpret_cast<LPARAM>(&high));
        if (buddy != layoutEdits_[i] || low != -100 || high != 100) {
            spinOk = false;
            break;
        }
        RECT editRect = {};
        RECT spinRect = {};
        RECT spinLabelRect = {};
        if (!windowRect(layoutEdits_[i], &editRect) ||
            !windowRect(layoutSpins_[i], &spinRect) ||
            !windowRect(layoutLabels_[i], &spinLabelRect)) {
            spinOk = false;
            break;
        }
        // UDS_ALIGNRIGHT is intentionally not used: the spin sits on the
        // edit's right edge (inside its bounds) so it cannot collide with the
        // neighbouring field.
        if (spinRect.left < editRect.left ||
            spinRect.right > editRect.right ||
            spinRect.top < editRect.top - dpiScale(1) ||
            spinRect.bottom > editRect.bottom + dpiScale(1)) {
            spinOk = false;
            break;
        }
        for (int j = 0; j < i; ++j) {
            RECT otherEdit = {};
            RECT otherLabel = {};
            RECT otherSpin = {};
            if (!windowRect(layoutEdits_[j], &otherEdit) ||
                !windowRect(layoutLabels_[j], &otherLabel) ||
                !windowRect(layoutSpins_[j], &otherSpin) ||
                intersects(spinRect, otherEdit) ||
                intersects(spinRect, otherLabel) ||
                intersects(spinRect, otherSpin) ||
                intersects(spinLabelRect, otherEdit) ||
                intersects(spinLabelRect, otherSpin)) {
                spinOk = false;
                break;
            }
        }
    }
    report << L"layout_spin_ok=" << (spinOk ? 1 : 0) << L"\n";
    report << L"font_layout_overlap_ok=" << (layoutOverlapOk ? 1 : 0) << L"\n";

    std::wstring fontSelectedCh;
    std::wstring fontSelectedEn;
    if (fontLoaded) {
        fontSelectedCh = chooseDifferentFont(fontChCombo_, L"微软雅黑");
        fontSelectedEn = chooseDifferentFont(fontEnCombo_, L"Arial");
    }
    if (previewTimer_) {
        ::KillTimer(hwnd_, previewTimer_);
        previewTimer_ = 0;
    }
    refreshPreview();
    const std::wstring fontAfterHash = previewHash();
    const int fontAfterWidth = previewBitmapWidth_;
    const int fontAfterHeight = previewBitmapHeight_;
    const bool fontPreviewChanged =
        fontLoaded && fontAfterHash != L"none" && fontAfterHash != L"empty" &&
        fontAfterHash != fontBeforeHash;
    savePreviewBitmap(dir + L"\\settings_font_after.png");
    captureWindowToPng(dir + L"\\settings_window_font.png");
    report << L"font_selected_ch=" << fontSelectedCh << L"\n";
    report << L"font_selected_en=" << fontSelectedEn << L"\n";
    report << L"font_preview_before=" << fontBeforeWidth << L"x"
           << fontBeforeHeight << L"/" << fontBeforeHash << L"\n";
    report << L"font_preview_after=" << fontAfterWidth << L"x"
           << fontAfterHeight << L"/" << fontAfterHash << L"\n";
    report << L"font_preview_changed=" << (fontPreviewChanged ? 1 : 0)
           << L"\n";

    bool fontApplyOk = false;
    bool fontReloadOk = false;
    if (fontLoaded) {
        const bool applied = apply(false);
        std::vector<unsigned char> overrideBytes;
        const std::wstring overrideText =
            ReadAllBytes(SogouSkin::OverridesPath(), &overrideBytes)
                ? DecodeTextBytes(overrideBytes)
                : std::wstring();
        fontApplyOk =
            applied && !fontCh_.empty() && !fontEn_.empty() &&
            overrideText.find(L"font_ch=" + fontCh_) != std::wstring::npos &&
            overrideText.find(L"font_en=" + fontEn_) != std::wstring::npos;
        std::wstring reloadError;
        auto reloaded = SogouSkin::LoadThemeWithOverrides(
            skins_[0].path, SogouSkin::OverridesPath(), &reloadError);
        fontReloadOk =
            reloaded &&
            _wcsicmp(reloaded->display().fontCh.c_str(), fontCh_.c_str()) == 0 &&
            _wcsicmp(reloaded->display().fontEn.c_str(), fontEn_.c_str()) == 0;
    }
    report << L"font_apply_ok=" << (fontApplyOk ? 1 : 0) << L"\n";
    report << L"font_reload_ok=" << (fontReloadOk ? 1 : 0) << L"\n";

    // ---- Independent font size / global scale / per-skin nudges --------
    bool fontSizeIndependentOk = false;
    bool scaleIndependentOk = false;
    bool spinPreviewOk = false;
    bool layoutApplyOk = false;
    bool layoutReloadOk = false;
    bool layoutIsolationOk = false;
    if (fontLoaded && !skins_.empty()) {
        std::wstring stateError;
        loadSkin(skins_[0].path, true, &stateError);
        // Start from zero nudges so a previous self-test run (which saves its
        // values) cannot make the "preview changed" comparison vacuous.
        layout_ = {};
        scalePercent_ = 100;
        syncLayoutEdits();
        if (previewTimer_) {
            ::KillTimer(hwnd_, 1);
            previewTimer_ = 0;
        }
        refreshPreview();
        const int basePreviewWidth = previewBitmapWidth_;
        const int basePreviewHeight = previewBitmapHeight_;
        const std::wstring basePreviewHash = previewHash();
        const int skinBaseFont = baseFontSize_;
        const int targetFont =
            std::clamp(skinBaseFont + 6, SogouSkin::kMinFontSize,
                       SogouSkin::kMaxFontSize);

        auto moveSlider = [&](HWND slider, int value) {
            ::SendMessageW(slider, TBM_SETPOS, TRUE, value);
            ::SendMessageW(hwnd_, WM_HSCROLL,
                           MAKEWPARAM(SB_THUMBPOSITION, value),
                           reinterpret_cast<LPARAM>(slider));
        };

        moveSlider(fontSlider_, targetFont);
        if (previewTimer_) {
            ::KillTimer(hwnd_, 1);
            previewTimer_ = 0;
        }
        refreshPreview();
        const std::wstring fontHash = previewHash();
        fontSizeIndependentOk =
            fontSize_ == targetFont && scalePercent_ == 100 &&
            fontHash != basePreviewHash && previewBitmapWidth_ > 0 &&
            previewBitmapHeight_ > 0;
        report << L"independent_font_size=" << fontSize_
               << L" scale=" << scalePercent_
               << L" preview=" << previewBitmapWidth_ << L"x"
               << previewBitmapHeight_ << L"\n";

        moveSlider(fontSlider_, skinBaseFont);
        moveSlider(scaleSlider_, 150);
        if (previewTimer_) {
            ::KillTimer(hwnd_, 1);
            previewTimer_ = 0;
        }
        refreshPreview();
        const int scaledWidth = previewBitmapWidth_;
        const int scaledHeight = previewBitmapHeight_;
        scaleIndependentOk =
            fontSize_ == skinBaseFont && scalePercent_ == 150 &&
            scaledWidth >= basePreviewWidth * 3 / 2 - 2 &&
            scaledHeight >= basePreviewHeight * 3 / 2 - 2;
        report << L"independent_scale=" << scalePercent_
               << L" preview=" << scaledWidth << L"x" << scaledHeight
               << L" base=" << basePreviewWidth << L"x" << basePreviewHeight
               << L"\n";

        moveSlider(scaleSlider_, 100);
        ::SetWindowTextW(layoutEdits_[0], L"20");
        ::SendMessageW(hwnd_, WM_COMMAND,
                       MAKEWPARAM(IDC_LAYOUT_PREEDIT_X, EN_CHANGE),
                       reinterpret_cast<LPARAM>(layoutEdits_[0]));
        ::SetWindowTextW(layoutEdits_[3], L"-15");
        ::SendMessageW(hwnd_, WM_COMMAND,
                       MAKEWPARAM(IDC_LAYOUT_CANDIDATE_Y, EN_CHANGE),
                       reinterpret_cast<LPARAM>(layoutEdits_[3]));
        if (previewTimer_) {
            ::KillTimer(hwnd_, 1);
            previewTimer_ = 0;
        }
        refreshPreview();
        const std::wstring layoutHash = previewHash();
        const bool layoutPreviewOk =
            layout_.preeditX == 20 && layout_.candidateY == -15 &&
            layoutHash != basePreviewHash;

        // Drive the Candidate X spinner exactly like an arrow click/keypress:
        // UDN_DELTAPOS is what the control sends before applying iDelta.
        const int spinBefore = layout_.candidateX;
        NMUPDOWN spinUpdate = {};
        spinUpdate.hdr.hwndFrom = layoutSpins_[2];
        spinUpdate.hdr.idFrom = IDC_LAYOUT_SPIN_CANDIDATE_X;
        spinUpdate.hdr.code = UDN_DELTAPOS;
        spinUpdate.iPos = spinBefore;
        spinUpdate.iDelta = 1;
        ::SendMessageW(hwnd_, WM_NOTIFY, spinUpdate.hdr.idFrom,
                       reinterpret_cast<LPARAM>(&spinUpdate));
        ::SendMessageW(layoutSpins_[2], UDM_SETPOS32, 0, layout_.candidateX);
        if (previewTimer_) {
            ::KillTimer(hwnd_, 1);
            previewTimer_ = 0;
        }
        refreshPreview();
        spinPreviewOk = layout_.candidateX == spinBefore + 1 &&
                        previewHash() != layoutHash;
        report << L"layout_spin_preview=" << (spinPreviewOk ? 1 : 0)
               << L" candidate_x=" << layout_.candidateX << L"\n";

        const bool layoutApplied = apply(false);
        std::vector<unsigned char> layoutBytes;
        const std::wstring layoutText =
            ReadAllBytes(SogouSkin::SkinLayoutsPath(), &layoutBytes)
                ? DecodeTextBytes(layoutBytes)
                : std::wstring();
        const bool layoutTextOk =
            layoutText.find(L"preedit_x=20") != std::wstring::npos &&
            layoutText.find(L"candidate_y=-15") != std::wstring::npos &&
            layoutText.find(L"candidate_x=1") != std::wstring::npos;
        layoutApplyOk =
            layoutApplied && layoutPreviewOk && layoutTextOk;
        wchar_t applyStatus[256] = {};
        ::GetWindowTextW(statusLabel_, applyStatus, _countof(applyStatus));
        report << L"layout_applied=" << (layoutApplied ? 1 : 0)
               << L" layout_preview_ok=" << (layoutPreviewOk ? 1 : 0)
               << L" layout_text_ok=" << (layoutTextOk ? 1 : 0)
               << L" apply_status=" << applyStatus << L"\n";

        std::wstring reloadError;
        auto reloaded = SogouSkin::LoadThemeWithOverrides(
            skins_[0].path, SogouSkin::OverridesPath(), &reloadError);
        layoutReloadOk =
            reloaded && reloaded->display().scalePercent == 100 &&
            reloaded->layout().preeditX == 20 &&
            reloaded->layout().candidateX == 1 &&
            reloaded->layout().candidateY == -15;
        report << L"layout_apply_ok=" << (layoutApplyOk ? 1 : 0) << L"\n";
        report << L"layout_reload_ok=" << (layoutReloadOk ? 1 : 0) << L"\n";

        // Pick a *different* skin id, not merely the second path or a
        // different preview: the installed copy and the original .ssf can be
        // byte-identical and therefore intentionally share the same per-skin
        // layout section.
        int otherIndex = -1;
        const std::wstring firstSkinId =
            baseTheme_ ? baseTheme_->skinId() : std::wstring();
        for (int i = 1; i < static_cast<int>(skins_.size()); ++i) {
            std::wstring otherSkinError;
            auto otherSkin =
                SogouSkin::Theme::Load(skins_[i].path, &otherSkinError);
            if (otherSkin && !otherSkin->skinId().empty() &&
                otherSkin->skinId() != firstSkinId) {
                otherIndex = i;
                break;
            }
        }
        if (otherIndex >= 0) {
            std::wstring otherError;
            const bool otherLoaded =
                loadSkin(skins_[otherIndex].path, false, &otherError);
            layoutIsolationOk =
                otherLoaded && layout_.preeditX == 0 && layout_.preeditY == 0 &&
                layout_.candidateX == 0 && layout_.candidateY == 0;
            if (otherLoaded) {
                loadSkin(skins_[0].path, true, &otherError);
                layoutIsolationOk =
                    layoutIsolationOk && layout_.preeditX == 20 &&
                    layout_.candidateX == 1 && layout_.candidateY == -15;
            }
        }
        report << L"layout_isolation_ok=" << (layoutIsolationOk ? 1 : 0)
               << L"\n";
    }
    report << L"font_size_independent_ok="
           << (fontSizeIndependentOk ? 1 : 0) << L"\n";
    report << L"scale_independent_ok=" << (scaleIndependentOk ? 1 : 0)
           << L"\n";

    size_t distinct = 0;
    for (size_t i = 0; i < signatures.size(); ++i) {
        if (std::find(signatures.begin(), signatures.begin() + i,
                      signatures[i]) == signatures.begin() + i) {
            ++distinct;
        }
    }
    const bool pass =
        skins_.size() >= 2 && distinct >= 2 && capacityOk && recentOk &&
        fontLoaded && fontPreviewChanged && fontApplyOk && fontReloadOk &&
        fontDropdownOk && layoutOverlapOk && spinOk &&
        fontSizeIndependentOk && scaleIndependentOk && spinPreviewOk &&
        layoutApplyOk && layoutReloadOk && layoutIsolationOk;
    report << L"distinct_previews=" << distinct << L"\n";
    report << L"capacity_ok=" << (capacityOk ? 1 : 0) << L"\n";
    report << L"recent_apply_ok=" << (recentOk ? 1 : 0) << L"\n";
    report << L"font_test_ok="
           << ((fontLoaded && fontPreviewChanged && fontApplyOk &&
                fontReloadOk && fontDropdownOk && layoutOverlapOk && spinOk)
                   ? 1
                   : 0)
           << L"\n";
    report << L"result=" << (pass ? L"pass" : L"fail") << L"\n";
    WriteAllBytes(dir + L"\\self-test.log", Utf16LeBytes(report.str()));

    ::DestroyWindow(hwnd_);
    ::PostQuitMessage(pass ? 0 : 1);
}

void SettingsWindow::resetToDefaults() {
    if (!baseTheme_) {
        return;
    }
    fontSize_ = baseFontSize_;
    scalePercent_ = 100;
    layout_ = {};
    fontCh_ = baseTheme_->display().fontCh;
    fontEn_ = baseTheme_->display().fontEn;
    statusbarFullscreenHide_ = true;
    colors_[0] = baseTheme_->display().pinyinColor;
    colors_[1] = baseTheme_->display().firstColor;
    colors_[2] = baseTheme_->display().textColor;
    colors_[3] = baseTheme_->display().commentColor;
    for (int i = 0; i < static_cast<int>(itemNames_.size()); ++i) {
        bool visible = false;
        for (const auto& item : baseTheme_->statusBar().items) {
            if (_wcsicmp(item.name.c_str(), itemNames_[i].c_str()) == 0) {
                visible = item.display;
                break;
            }
        }
        itemVisible_[i] = visible;
    }
    syncControlsFromState();
    populateStatusItems();
    schedulePreview();
    setStatus(L"已恢复皮肤默认值（尚未写入）。");
}

bool SettingsWindow::saveOptions() const {
    const std::wstring path = SogouSkin::OptionsPath();
    if (path.empty()) {
        return false;
    }
    const std::wstring config = SogouSkin::ConfigDirectory();
    if (!config.empty()) {
        ::CreateDirectoryW(config.c_str(), nullptr);
    }
    std::wostringstream out;
    const auto current = SogouSkin::LoadOptions();
    out << L"[Options]\n";
    out << L"statusbar=" << (statusbar_ ? 1 : 0) << L"\n";
    out << L"horizontal=" << (horizontal_ ? 1 : 0) << L"\n";
    out << L"key_repeat=" << (current.keyRepeat ? 1 : 0) << L"\n";
    out << L"statusbar_fullscreen_hide="
        << (statusbarFullscreenHide_ ? 1 : 0) << L"\n";
    out << L"statusbar_fullscreen_scope="
        << (current.statusbarFullscreenScope ==
                    SogouSkin::FullscreenScope::All
                ? L"all"
                : L"monitor")
        << L"\n";
    out << L"statusbar_hide=\n";
    if (current.statusbarX >= 0) {
        out << L"statusbar_x=" << current.statusbarX << L"\n";
    }
    if (current.statusbarY >= 0) {
        out << L"statusbar_y=" << current.statusbarY << L"\n";
    }
    return WriteAllBytes(path, NarrowAscii(out.str()));
}

bool SettingsWindow::apply(bool closeAfter) {
    if (skinPath_.empty()) {
        setStatus(L"请先选择一个 .ssf 皮肤。");
        return false;
    }
    const std::wstring installed = SogouSkin::InstalledSkinPath();
    if (installed.empty()) {
        setStatus(L"无法定位 %APPDATA%\\Moqi。");
        return false;
    }
    const std::wstring config = SogouSkin::ConfigDirectory();
    ::CreateDirectoryW(config.c_str(), nullptr);
    if (DirectoryExists(skinPath_)) {
        // Extracted skin directory: keep a pointer instead of copying a file.
        const std::wstring pointer = JoinPath(config, L"sogou_skin_path.txt");
        WriteAllBytes(pointer, Utf16LeBytes(skinPath_));
    } else {
        // The IME always looks at sogou_skin_path.txt first.  Remove a stale
        // pointer so the freshly selected archive is the one that loads.
        const std::wstring pathFile = JoinPath(config, L"sogou_skin_path.txt");
        if (FileExists(pathFile)) {
            ::DeleteFileW(pathFile.c_str());
        }
        if (!SamePath(skinPath_, installed)) {
            if (!::CopyFileW(skinPath_.c_str(), installed.c_str(), FALSE)) {
                setStatus(L"复制皮肤失败: " + skinPath_);
                return false;
            }
        }
    }
    initialSkinPath_ = installed;
    RememberRecentSkin(skinPath_);

    int removedLegacyKeys = 0;
    const std::wstring overrides = buildOverridesText(&removedLegacyKeys);
    const std::wstring existing = SogouSkin::OverridesPath();
    if (existing.empty() || overrides.empty() ||
        !WriteAllBytes(existing, Utf16LeBytes(overrides))) {
        setStatus(L"写入皮肤覆盖配置失败。");
        return false;
    }
    if (baseTheme_ &&
        !SogouSkin::SaveSkinLayoutOffsets(baseTheme_->skinId(), layout_)) {
        setStatus(L"写入皮肤文字微调配置失败。");
        return false;
    }
    if (!saveOptions()) {
        setStatus(L"写入显示选项失败。");
        return false;
    }

    ::PostMessageW(HWND_BROADCAST, SogouSkin::WM_MOQI_SOGOU_SETTINGS_CHANGED, 0, 0);
    if (removedLegacyKeys > 0) {
        setStatus(L"已应用字体大小/缩放/文字微调；已迁移 " +
                  std::to_wstring(removedLegacyKeys) + L" 个旧版布局键。");
    } else {
        setStatus(L"已应用字体大小/缩放/文字微调，输入法会立即刷新。");
    }
    if (closeAfter) {
        ::DestroyWindow(hwnd_);
    }
    return true;
}

void SettingsWindow::onListItemChanged(LPNMLISTVIEW info) {
    if (updatingList_ || !info) {
        return;
    }
    if ((info->uChanged & LVIF_STATE) == 0) {
        return;
    }
    const UINT newState =
        (info->uNewState & LVIS_STATEIMAGEMASK) >> 12;
    if (info->iItem >= 0 && info->iItem < static_cast<int>(itemVisible_.size())) {
        const bool checked = newState == 2;
        if (itemVisible_[info->iItem] != checked) {
            itemVisible_[info->iItem] = checked;
            schedulePreview();
        }
    }
}

void SettingsWindow::drawItem(LPDRAWITEMSTRUCT item) {
    if (!item) {
        return;
    }
    int index = -1;
    static const ControlId ids[kColorCount] = {
        IDC_COLOR_PINYIN, IDC_COLOR_FIRST, IDC_COLOR_TEXT, IDC_COLOR_COMMENT};
    for (int i = 0; i < kColorCount; ++i) {
        if (item->CtlID == ids[i]) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        return;
    }
    const COLORREF color = colors_[index];
    HBRUSH brush = ::CreateSolidBrush(color);
    ::FillRect(item->hDC, &item->rcItem, brush);
    ::DeleteObject(brush);
    const int luminance = (GetRValue(color) * 299 + GetGValue(color) * 587 +
                           GetBValue(color) * 114) /
                          1000;
    ::SetBkMode(item->hDC, TRANSPARENT);
    ::SetTextColor(item->hDC, luminance > 140 ? RGB(0, 0, 0) : RGB(255, 255, 255));
    wchar_t text[32] = {};
    ::GetWindowTextW(item->hwndItem, text, _countof(text));
    RECT textRect = item->rcItem;
    ::DrawTextW(item->hDC, text, -1, &textRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (item->itemState & ODS_FOCUS) {
        ::DrawFocusRect(item->hDC, &item->rcItem);
    }
}

void SettingsWindow::onHScroll(HWND control, int code) {
    if (control == fontSlider_) {
        const int position =
            static_cast<int>(::SendMessageW(fontSlider_, TBM_GETPOS, 0, 0));
        if (position != fontSize_) {
            fontSize_ = position;
            wchar_t text[16] = {};
            swprintf_s(text, L"%d", fontSize_);
            ::SetWindowTextW(fontValue_, text);
            schedulePreview();
        }
    } else if (control == scaleSlider_) {
        const int position =
            static_cast<int>(::SendMessageW(scaleSlider_, TBM_GETPOS, 0, 0));
        if (position != scalePercent_) {
            scalePercent_ = position;
            wchar_t text[16] = {};
            swprintf_s(text, L"%d%%", scalePercent_);
            ::SetWindowTextW(scaleValue_, text);
            schedulePreview();
        }
    }
}

void SettingsWindow::onCommand(WORD id, WORD notify) {
    if (id == IDC_SKIN_BROWSE && notify == BN_CLICKED) {
        wchar_t file[32768] = {};
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"搜狗皮肤 (*.ssf)\0*.ssf\0所有文件 (*.*)\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = _countof(file);
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (::GetOpenFileNameW(&ofn)) {
            std::wstring path = file;
            bool found = false;
            for (int i = 0; i < static_cast<int>(skins_.size()); ++i) {
                if (SamePath(skins_[i].path, path)) {
                    skinIndex_ = i;
                    found = true;
                    break;
                }
            }
            if (!found) {
                skins_.push_back({path, FileNameOf(path)});
                UpdateSkinLabels(&skins_);
                skinIndex_ = static_cast<int>(skins_.size()) - 1;
                populateSkinCombo();
            }
            ::SendMessageW(skinCombo_, CB_SETCURSEL, skinIndex_, 0);
            std::wstring error;
            if (!loadSkin(path, false, &error)) {
                setStatus(L"加载皮肤失败: " + error);
            } else {
                RememberRecentSkin(path);
                setStatus(L"已载入 " + FileNameOf(path) + L"，点击“应用”生效。");
            }
        }
        return;
    }
    if (id == IDC_SKIN_COMBO && notify == CBN_SELCHANGE) {
        const int selected =
            static_cast<int>(::SendMessageW(skinCombo_, CB_GETCURSEL, 0, 0));
        if (selected >= 0 && selected < static_cast<int>(skins_.size())) {
            skinIndex_ = selected;
            std::wstring error;
            const bool useSaved = !initialSkinPath_.empty() &&
                                  SamePath(skins_[selected].path, initialSkinPath_);
            if (!loadSkin(skins_[selected].path, useSaved, &error)) {
                setStatus(L"加载皮肤失败: " + error);
            }
        }
        return;
    }
    if (id == IDC_FONT_CH_COMBO && notify == CBN_SELCHANGE) {
        const std::wstring selected = selectedFont(fontChCombo_);
        if (!selected.empty() &&
            _wcsicmp(selected.c_str(), fontCh_.c_str()) != 0) {
            fontCh_ = selected;
            updateFontFaceLabel();
            schedulePreview();
        }
        return;
    }
    if (id == IDC_FONT_EN_COMBO && notify == CBN_SELCHANGE) {
        const std::wstring selected = selectedFont(fontEnCombo_);
        if (!selected.empty() &&
            _wcsicmp(selected.c_str(), fontEn_.c_str()) != 0) {
            fontEn_ = selected;
            updateFontFaceLabel();
            schedulePreview();
        }
        return;
    }
    if (id >= IDC_LAYOUT_PREEDIT_X && id <= IDC_LAYOUT_CANDIDATE_Y) {
        const int index = static_cast<int>(id - IDC_LAYOUT_PREEDIT_X);
        if (notify == EN_CHANGE) {
            onLayoutEditChanged(index);
        } else if (notify == EN_KILLFOCUS) {
            normalizeLayoutEdit(index);
        }
        return;
    }
    if (id >= IDC_COLOR_PINYIN && id <= IDC_COLOR_COMMENT && notify == BN_CLICKED) {
        chooseColor(id - IDC_COLOR_PINYIN);
        return;
    }
    if (id == IDC_HORIZONTAL && notify == BN_CLICKED) {
        horizontal_ = true;
        schedulePreview();
        return;
    }
    if (id == IDC_VERTICAL && notify == BN_CLICKED) {
        horizontal_ = false;
        schedulePreview();
        return;
    }
    if (id == IDC_STATUSBAR && notify == BN_CLICKED) {
        statusbar_ = ::SendMessageW(statusbarCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        return;
    }
    if (id == IDC_STATUSBAR_FULLSCREEN && notify == BN_CLICKED) {
        statusbarFullscreenHide_ =
            ::SendMessageW(statusbarFullscreenCheck_, BM_GETCHECK, 0, 0) ==
            BST_CHECKED;
        return;
    }
    if (id == IDC_RESET && notify == BN_CLICKED) {
        resetToDefaults();
        return;
    }
    if (id == IDC_APPLY && notify == BN_CLICKED) {
        apply(false);
        return;
    }
    if (id == IDC_OK && notify == BN_CLICKED) {
        apply(true);
        return;
    }
    if (id == IDC_CANCEL && notify == BN_CLICKED) {
        ::DestroyWindow(hwnd_);
        return;
    }
}

LRESULT SettingsWindow::wndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        onCommand(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_HSCROLL:
        onHScroll(reinterpret_cast<HWND>(lp), LOWORD(wp));
        return 0;
    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lp);
        if (header && header->idFrom == IDC_ITEM_LIST &&
            header->code == LVN_ITEMCHANGED) {
            onListItemChanged(reinterpret_cast<NMLISTVIEW*>(lp));
        } else if (header && header->code == UDN_DELTAPOS) {
            static const ControlId spinIds[4] = {
                IDC_LAYOUT_SPIN_PREEDIT_X, IDC_LAYOUT_SPIN_PREEDIT_Y,
                IDC_LAYOUT_SPIN_CANDIDATE_X, IDC_LAYOUT_SPIN_CANDIDATE_Y};
            for (int i = 0; i < 4; ++i) {
                if (header->idFrom == spinIds[i]) {
                    const auto* update = reinterpret_cast<NMUPDOWN*>(lp);
                    onLayoutSpinDelta(i, update->iDelta, update->iPos);
                    break;
                }
            }
        }
        return 0;
    }
    case WM_DRAWITEM:
        drawItem(reinterpret_cast<LPDRAWITEMSTRUCT>(lp));
        return TRUE;
    case WM_TIMER:
        if (wp == 1) {
            if (previewTimer_) {
                ::KillTimer(hwnd_, previewTimer_);
                previewTimer_ = 0;
            }
            refreshPreview();
            return 0;
        }
        if (wp == 2) {
            ::KillTimer(hwnd_, 2);
            refreshPreview();
            return 0;
        }
        if (wp == 3) {
            ::KillTimer(hwnd_, 3);
            runSelfTest();
            return 0;
        }
        break;
    case WM_SIZE:
        layoutControls();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        RECT desired = {0, 0, dpiScale(kClientWidth), dpiScale(kClientHeight)};
        SogouSkin::AdjustWindowRectForDpi(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0,
                                          dpi_);
        info->ptMinTrackSize.x = desired.right - desired.left;
        info->ptMinTrackSize.y = desired.bottom - desired.top;
        return 0;
    }
    case WM_DPICHANGED: {
        const UINT newDpi = HIWORD(wp);
        if (newDpi > 0) {
            dpi_ = newDpi;
        }
        const RECT* suggested = reinterpret_cast<const RECT*>(lp);
        if (suggested) {
            ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left,
                           suggested->bottom - suggested->top,
                           SWP_NOACTIVATE | SWP_NOZORDER);
        }
        applyFonts();
        layoutControls();
        updateDpiDependentMetrics();
        ::InvalidateRect(hwnd_, nullptr, TRUE);
        refreshPreview();
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        ::SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_WINDOW));
    case WM_DESTROY:
        if (previewTimer_) {
            ::KillTimer(hwnd_, previewTimer_);
        }
        ::KillTimer(hwnd_, 2);
        if (!tempOverridesPath_.empty()) {
            ::DeleteFileW(tempOverridesPath_.c_str());
        }
        destroyFonts();
        ::PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT SettingsWindow::previewProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC dc = ::BeginPaint(preview_, &ps);
        RECT client = {};
        ::GetClientRect(preview_, &client);
        paintPreview(dc);
        ::EndPaint(preview_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PRINT:
    case WM_PRINTCLIENT: {
        paintPreview(reinterpret_cast<HDC>(wp));
        return 0;
    }
    default:
        return ::DefWindowProcW(preview_, msg, wp, lp);
    }
}

LRESULT CALLBACK SettingsWindow::staticWndProc(HWND hwnd, UINT msg, WPARAM wp,
                                               LPARAM lp) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<SettingsWindow*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->wndProc(msg, wp, lp)
                : ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK SettingsWindow::staticPreviewProc(HWND hwnd, UINT msg, WPARAM wp,
                                                   LPARAM lp) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) {
            self->preview_ = hwnd;
        }
    } else {
        self = reinterpret_cast<SettingsWindow*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->previewProc(msg, wp, lp)
                : ::DefWindowProcW(hwnd, msg, wp, lp);
}

int SettingsWindow::run() {
    MSG msg = {};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--offscreen") == 0) {
                g_offscreen = true;
            } else if (_wcsicmp(argv[i], L"--self-test") == 0 && i + 1 < argc) {
                g_self_test_dir = argv[++i];
                g_offscreen = true;
            } else if (_wcsicmp(argv[i], L"--dpi") == 0 && i + 1 < argc) {
                const int dpi = _wtoi(argv[++i]);
                if (dpi >= 96 && dpi <= 480) {
                    g_forced_dpi = static_cast<UINT>(dpi);
                }
            }
        }
        ::LocalFree(argv);
    }
    // Must be set before any window is created.  The old SetProcessDPIAware()
    // produced a system-aware process with a 96-DPI window layout, so at 150%
    // the controls were laid out at 2x inside a 1x window and clipped.
    SogouSkin::EnablePerMonitorDpiAwareness();
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES |
                     ICC_BAR_CLASSES | ICC_UPDOWN_CLASS;
    ::InitCommonControlsEx(&controls);

    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput gdiplusInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

    SettingsWindow window;
    int result = 1;
    if (window.create(instance)) {
        result = window.run();
    } else {
        ::MessageBoxW(nullptr, L"无法创建设置窗口。", L"墨奇输入法 - 搜狗皮肤",
                      MB_OK | MB_ICONERROR);
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    ::CoUninitialize();
    return result;
}
