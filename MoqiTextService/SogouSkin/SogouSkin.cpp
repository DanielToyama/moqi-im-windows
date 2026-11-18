#include "SogouSkin.h"

#include "third_party/miniz/miniz.h"

#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <sstream>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

namespace Moqi {
namespace SogouSkin {
namespace {

constexpr COLORREF kDefaultBorder = RGB(150, 150, 150);

ULONG_PTR g_gdiplusToken = 0;
bool g_gdiplusReady = false;

unsigned long long qpcMicros() {
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER value{};
        ::QueryPerformanceFrequency(&value);
        return value;
    }();
    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);
    if (frequency.QuadPart <= 0) {
        return 0;
    }
    return static_cast<unsigned long long>(
        (counter.QuadPart * 1000000ULL) / frequency.QuadPart);
}

bool ensureGdiplus() {
    if (g_gdiplusReady) {
        return true;
    }
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr) != Gdiplus::Ok) {
        return false;
    }
    g_gdiplusReady = true;
    return true;
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

std::wstring Trim(const std::wstring& value) {
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

bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>* out) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        ::CloseHandle(file);
        return false;
    }
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = out->empty() ||
                    (::ReadFile(file, out->data(), static_cast<DWORD>(out->size()), &read,
                                nullptr) &&
                     read == out->size());
    ::CloseHandle(file);
    return ok;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int count = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring result(count, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          result.data(), count);
    return result;
}

std::wstring DecodeIniText(const std::vector<unsigned char>& bytes) {
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        std::wstring result;
        result.resize((bytes.size() - 2) / 2);
        std::memcpy(result.data(), bytes.data() + 2, result.size() * sizeof(wchar_t));
        return result;
    }
    if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        std::wstring result;
        result.resize((bytes.size() - 2) / 2);
        for (size_t i = 0; i < result.size(); ++i) {
            result[i] = static_cast<wchar_t>((bytes[2 + i * 2] << 8) | bytes[3 + i * 2]);
        }
        return result;
    }
    // UTF-16LE without BOM: many NUL bytes in the high half.
    size_t zeros = 0;
    const size_t sample = std::min<size_t>(bytes.size(), 200);
    for (size_t i = 1; i < sample; i += 2) {
        if (bytes[i] == 0) {
            ++zeros;
        }
    }
    if (sample >= 8 && zeros * 2 >= sample / 2) {
        std::wstring result;
        result.resize(bytes.size() / 2);
        std::memcpy(result.data(), bytes.data(), result.size() * sizeof(wchar_t));
        return result;
    }
    // UTF-8, then ACP/GBK fallback.
    if (!bytes.empty()) {
        const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<int>(bytes.size()), nullptr, 0);
        if (count > 0) {
            std::wstring result(count, L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(bytes.data()),
                                  static_cast<int>(bytes.size()), result.data(), count);
            return result;
        }
        const int acpCount = ::MultiByteToWideChar(
            CP_ACP, 0, reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(bytes.size()), nullptr, 0);
        if (acpCount > 0) {
            std::wstring result(acpCount, L'\0');
            ::MultiByteToWideChar(CP_ACP, 0,
                                  reinterpret_cast<const char*>(bytes.data()),
                                  static_cast<int>(bytes.size()), result.data(), acpCount);
            return result;
        }
    }
    return {};
}

std::vector<std::wstring> SplitComma(const std::wstring& value) {
    std::vector<std::wstring> result;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t comma = value.find(L',', start);
        result.push_back(Trim(value.substr(start, comma == std::wstring::npos
                                                       ? std::wstring::npos
                                                       : comma - start)));
        if (comma == std::wstring::npos) {
            break;
        }
        start = comma + 1;
    }
    return result;
}

bool ParseInt(const std::wstring& text, int* value) {
    if (text.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(text.c_str(), &end, 0);
    if (!end || *end != L'\0') {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

bool ParseLayout(const std::wstring& text, Layout* layout) {
    const auto parts = SplitComma(text);
    if (parts.size() != 3) {
        return false;
    }
    Layout result;
    if (!ParseInt(parts[0], &result.mode) || !ParseInt(parts[1], &result.fixed1) ||
        !ParseInt(parts[2], &result.fixed2)) {
        return false;
    }
    *layout = result;
    return true;
}

bool ParseMargin(const std::wstring& text, int out[4]) {
    const auto parts = SplitComma(text);
    if (parts.size() != 4) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (!ParseInt(parts[i], &out[i])) {
            return false;
        }
    }
    return true;
}

COLORREF ParseColor(const std::wstring& text, COLORREF fallback) {
    if (text.empty()) {
        return fallback;
    }
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(text.c_str(), &end, 0);
    if (!end || *end != L'\0') {
        return fallback;
    }
    return static_cast<COLORREF>(value & 0x00FFFFFF);
}

std::wstring GetString(const std::map<std::wstring, std::wstring>& section,
                       const wchar_t* key) {
    const auto it = section.find(ToLower(key));
    return it == section.end() ? std::wstring() : it->second;
}

int GetInt(const std::map<std::wstring, std::wstring>& section, const wchar_t* key,
           int fallback) {
    int value = fallback;
    const auto text = GetString(section, key);
    return ParseInt(text, &value) ? value : fallback;
}

bool GetBool(const std::map<std::wstring, std::wstring>& section, const wchar_t* key,
             bool fallback) {
    return GetInt(section, key, fallback ? 1 : 0) != 0;
}

using IniSectionMap = std::map<std::wstring, std::map<std::wstring, std::wstring>>;

void ParseIniSections(const std::wstring& text, IniSectionMap* sections) {
    std::wstring current;
    std::wstring normalized = text;
    if (!normalized.empty() && normalized[0] == 0xFEFF) {
        normalized.erase(normalized.begin());
    }
    std::wstringstream stream(normalized);
    std::wstring line;
    while (std::getline(stream, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') {
            continue;
        }
        if (line.front() == L'[' && line.back() == L']') {
            current = ToLower(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t equals = line.find(L'=');
        if (equals == std::wstring::npos) {
            continue;
        }
        const std::wstring key = ToLower(Trim(line.substr(0, equals)));
        const std::wstring value = Trim(line.substr(equals + 1));
        (*sections)[current][key] = value;
    }
}

std::wstring MoqiConfigDirectory() {
    const wchar_t* appData = _wgetenv(L"APPDATA");
    if (!appData || !*appData) {
        return {};
    }
    return std::wstring(appData) + L"\\Moqi";
}

std::wstring GetEnvValue(const wchar_t* name) {
    wchar_t buffer[32768] = {};
    const DWORD length = ::GetEnvironmentVariableW(name, buffer, _countof(buffer));
    return length > 0 && length < _countof(buffer) ? std::wstring(buffer) : std::wstring();
}

bool IsFile(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) {
        return name;
    }
    wchar_t last = dir.back();
    return dir + (last == L'\\' || last == L'/' ? L"" : L"\\") + name;
}

// Directories that may hold a skin font.  Sogou skins often rely on fonts
// installed per-user by the Sogou skin editor, not on machine-wide fonts.
std::vector<std::wstring> FontSearchDirs() {
    std::vector<std::wstring> dirs;
    const std::wstring localAppData = GetEnvValue(L"LOCALAPPDATA");
    if (!localAppData.empty()) {
        dirs.push_back(JoinPath(localAppData, L"Microsoft\\Windows\\Fonts"));
        dirs.push_back(JoinPath(localAppData, L"SogouPY"));
    }
    wchar_t windowsDir[MAX_PATH] = {};
    const UINT windowsLength = ::GetWindowsDirectoryW(windowsDir, MAX_PATH);
    if (windowsLength > 0 && windowsLength < MAX_PATH) {
        dirs.push_back(JoinPath(windowsDir, L"Fonts"));
    }
    const std::wstring programFilesX86 = GetEnvValue(L"ProgramFiles(x86)");
    if (!programFilesX86.empty()) {
        dirs.push_back(JoinPath(programFilesX86, L"SogouImeSkinEditor\\Fonts"));
    }
    const std::wstring programFiles = GetEnvValue(L"ProgramFiles");
    if (!programFiles.empty()) {
        dirs.push_back(JoinPath(programFiles, L"SogouInput"));
    }
    return dirs;
}

std::vector<std::wstring> FontFileCandidates(const std::wstring& family) {
    const std::wstring key = ToLower(family);
    if (key == L"宋体" || key == L"新宋体") {
        return {L"simsun.ttc", L"simsun.ttf"};
    }
    if (key == L"微软雅黑" || key == L"微软雅黑 ui") {
        return {L"msyh.ttc", L"msyh.ttf"};
    }
    if (key == L"黑体") {
        return {L"simhei.ttf"};
    }
    if (key == L"楷体") {
        return {L"simkai.ttf"};
    }
    if (key == L"仿宋") {
        return {L"simfang.ttf"};
    }
    if (key == L"arial") {
        return {L"arial.ttf"};
    }
    if (key == L"tahoma") {
        return {L"tahoma.ttf"};
    }
    if (key == L"segoe ui") {
        return {L"segoeui.ttf"};
    }
    return {family + L".ttf", family + L".otf", family + L".ttc", family + L".otc"};
}

std::wstring FindFontFile(const std::wstring& family) {
    if (family.empty()) {
        return {};
    }
    const auto candidates = FontFileCandidates(family);
    for (const auto& dir : FontSearchDirs()) {
        if (dir.empty()) {
            continue;
        }
        for (const auto& candidate : candidates) {
            const std::wstring path = JoinPath(dir, candidate);
            if (IsFile(path)) {
                return path;
            }
        }
    }
    return {};
}

// Builds a FontFamily from the actual font file.  GDI+ can resolve the
// localized family name for this OTF, but PrivateFontCollection makes the
// binding explicit and also covers fonts that are not installed globally.
std::unique_ptr<Gdiplus::FontFamily> LoadFontFamily(
    Gdiplus::PrivateFontCollection* collection, const std::wstring& face) {
    if (!collection || face.empty()) {
        return nullptr;
    }
    const std::wstring path = FindFontFile(face);
    if (!path.empty() && collection->AddFontFile(path.c_str()) == Gdiplus::Ok) {
        const int count = collection->GetFamilyCount();
        if (count > 0) {
            std::vector<Gdiplus::FontFamily> families(static_cast<size_t>(count));
            int found = 0;
            if (collection->GetFamilies(count, families.data(), &found) == Gdiplus::Ok) {
                const std::wstring wanted = ToLower(face);
                for (int i = 0; i < found; ++i) {
                    wchar_t name[LF_FACESIZE] = {};
                    families[static_cast<size_t>(i)].GetFamilyName(name, LANG_NEUTRAL);
                    if (ToLower(name) == wanted) {
                        return std::unique_ptr<Gdiplus::FontFamily>(
                            families[static_cast<size_t>(i)].Clone());
                    }
                }
                if (found > 0) {
                    return std::unique_ptr<Gdiplus::FontFamily>(
                        families[0].Clone());
                }
            }
        }
    }
    auto installed = std::make_unique<Gdiplus::FontFamily>(face.c_str());
    return installed->IsAvailable() ? std::move(installed) : nullptr;
}

// Applies an override INI on top of skin.ini.  Only known section/key pairs
// are consumed later; unknown keys are simply ignored, so upstream skin.ini
// additions keep working.
bool ReadOverrideSections(const std::wstring& path, IniSectionMap* sections) {
    if (path.empty()) {
        return false;
    }
    std::vector<unsigned char> data;
    if (!ReadFileBytes(path, &data)) {
        return false;
    }
    ParseIniSections(DecodeIniText(data), sections);
    return true;
}

void MergeOverrideSections(IniSectionMap* sections, const IniSectionMap& overrides) {
    for (const auto& [section, values] : overrides) {
        auto& target = (*sections)[section];
        for (const auto& [key, value] : values) {
            target[key] = value;
        }
    }
}

// The settings UI writes this marker once it has migrated a legacy override
// file.  From then on every layout key in the file is a deliberate user edit
// and must not be filtered again.
bool HasGlobalLayoutMarker(const IniSectionMap& sections) {
    const auto it = sections.find(L"moqi_overrides");
    if (it == sections.end()) {
        return false;
    }
    const auto& section = it->second;
    const int version = GetInt(section, L"version", 0);
    if (version >= 2) {
        return true;
    }
    return ToLower(GetString(section, L"layout_mode")) == L"global";
}

// Version 3 introduced independent [Display] font_size (actual text size) and
// scale (whole-skin percent).  Older files used font_size as a single
// font-and-window multiplier.  A hand-written file that already contains
// `scale` is treated as new-format even without the marker.
bool HasExplicitScale(const IniSectionMap& sections) {
    const auto it = sections.find(L"display");
    return it != sections.end() &&
           it->second.find(L"scale") != it->second.end();
}

bool HasNewFontScaleSemantics(const IniSectionMap& sections) {
    if (HasExplicitScale(sections)) {
        return true;
    }
    const auto it = sections.find(L"moqi_overrides");
    return it != sections.end() && GetInt(it->second, L"version", 0) >= 3;
}

int ScalePercentFromLegacyFontSize(int oldFontSize, int baseFontSize) {
    if (baseFontSize <= 0) {
        return 100;
    }
    const double ratio =
        static_cast<double>(std::max(1, oldFontSize)) / baseFontSize;
    return std::clamp(static_cast<int>(std::lround(ratio * 100.0)),
                      kMinScalePercent, kMaxScalePercent);
}

int ClampLayoutOffset(int value) {
    return std::clamp(value, -100, 100);
}

SkinLayout ClampLayout(SkinLayout layout) {
    layout.preeditX = ClampLayoutOffset(layout.preeditX);
    layout.preeditY = ClampLayoutOffset(layout.preeditY);
    layout.candidateX = ClampLayoutOffset(layout.candidateX);
    layout.candidateY = ClampLayoutOffset(layout.candidateY);
    return layout;
}

unsigned long long Fnv1a(const void* data, size_t size,
                         unsigned long long hash = 1469598103934665603ULL) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::wstring FormatSkinId(unsigned long long hash) {
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"skin_%016llx", hash);
    return buffer;
}

std::wstring SkinIdFromBytes(const std::vector<unsigned char>& bytes) {
    return FormatSkinId(Fnv1a(bytes.data(), bytes.size()));
}

void CollectSkinFiles(const std::wstring& root, const std::wstring& relative,
                      int depth,
                      std::vector<std::pair<std::wstring, unsigned long long>>* out) {
    if (!out || depth > 4) {
        return;
    }
    const std::wstring pattern =
        JoinPath(relative.empty() ? root : JoinPath(root, relative), L"*");
    WIN32_FIND_DATAW data = {};
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        const std::wstring childRelative =
            relative.empty() ? name : relative + L"\\" + name;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectSkinFiles(root, childRelative, depth + 1, out);
            continue;
        }
        if (relative.empty() && ToLower(name) == L"skin.ini") {
            continue;
        }
        std::wstring normalized = ToLower(childRelative);
        std::replace(normalized.begin(), normalized.end(), L'\\', L'/');
        const unsigned long long size =
            (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) |
            data.nFileSizeLow;
        out->emplace_back(normalized, size);
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
}

// The section id must identify one skin, not one filesystem location: the IME
// installs archives as %APPDATA%\Moqi\sogou_skin.ssf.  Archives therefore hash
// their bytes (the copied file is byte-identical), while extracted directories
// hash skin.ini plus the relative name/size inventory of their assets.
std::wstring SkinIdFromContent(
    const std::vector<unsigned char>& iniBytes,
    std::vector<std::pair<std::wstring, unsigned long long>> files) {
    unsigned long long hash = Fnv1a(iniBytes.data(), iniBytes.size());
    std::sort(files.begin(), files.end());
    for (const auto& [name, size] : files) {
        const std::wstring marker = name + L"\n";
        hash = Fnv1a(marker.data(), marker.size() * sizeof(wchar_t), hash);
        hash = Fnv1a(&size, sizeof(size), hash);
    }
    return FormatSkinId(hash);
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

bool WriteAllBytes(const std::wstring& path, const std::string& data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    return file.good();
}

std::wstring FormatIntList(const int* values, int count) {
    std::wstring result;
    for (int i = 0; i < count; ++i) {
        if (i) {
            result += L',';
        }
        result += std::to_wstring(values[i]);
    }
    return result;
}

std::wstring FormatLayoutValue(const Layout& layout) {
    const int values[] = {layout.mode, layout.fixed1, layout.fixed2};
    return FormatIntList(values, 3);
}

bool IntTokenEqualsScaled(const std::wstring& baseText, const std::wstring& overrideText,
                          double scale, bool keepValue) {
    int baseValue = 0;
    int overrideValue = 0;
    if (!ParseInt(baseText, &baseValue) || !ParseInt(overrideText, &overrideValue)) {
        return false;
    }
    if (keepValue) {
        return baseValue == overrideValue;
    }
    return overrideValue == static_cast<int>(std::lround(baseValue * scale));
}

// Compares a CSV override against the base value scaled the way the old
// settings UI wrote it.  keepFirst is true for layout triples (mode must not
// change).  colorFirst compares the first token as text (separator colour).
bool ValueMatchesScaled(const std::wstring& baseValue, const std::wstring& overrideValue,
                        double scale, bool keepFirst, bool colorFirst = false) {
    const auto baseParts = SplitComma(baseValue);
    const auto overrideParts = SplitComma(overrideValue);
    if (baseParts.size() != overrideParts.size() || baseParts.empty()) {
        return false;
    }
    size_t begin = 0;
    if (colorFirst) {
        if (ToLower(Trim(baseParts[0])) != ToLower(Trim(overrideParts[0]))) {
            return false;
        }
        begin = 1;
    }
    for (size_t i = begin; i < baseParts.size(); ++i) {
        if (!IntTokenEqualsScaled(baseParts[i], overrideParts[i], scale,
                                  keepFirst && i == 0)) {
            return false;
        }
    }
    return true;
}

bool BaseSchemeValue(const Scheme& scheme, const std::wstring& key,
                     std::wstring* value) {
    if (!scheme.split) {
        if (key == L"layout_horizontal") {
            *value = FormatLayoutValue(scheme.single.layoutH);
            return true;
        }
        if (key == L"layout_vertical") {
            *value = FormatLayoutValue(scheme.single.layoutV);
            return true;
        }
        if (key == L"pinyin_marge") {
            *value = FormatIntList(scheme.pinyinMargin, 4);
            return true;
        }
        if (key == L"zhongwen_marge") {
            *value = FormatIntList(scheme.zhongwenMargin, 4);
            return true;
        }
        if (key == L"separator") {
            if (!scheme.hasSeparator) {
                return false;
            }
            const int margins[] = {scheme.separatorLeft, scheme.separatorRight};
            wchar_t color[16] = {};
            swprintf_s(color, L"0x%06X",
                       static_cast<unsigned int>(scheme.separatorColor & 0x00FFFFFF));
            *value = std::wstring(color) + L"," + FormatIntList(margins, 2);
            return true;
        }
        if (key == L"anchor") {
            if (!scheme.hasAnchor) {
                return false;
            }
            const int anchor[] = {scheme.anchorX, scheme.anchorY};
            *value = FormatIntList(anchor, 2);
            return true;
        }
        return false;
    }

    if (key == L"pinyin_layout_horizontal") {
        *value = FormatLayoutValue(scheme.pinyin.layoutH);
        return true;
    }
    if (key == L"pinyin_layout_vertical") {
        *value = FormatLayoutValue(scheme.pinyin.layoutV);
        return true;
    }
    if (key == L"zhongwen_layout_horizontal") {
        *value = FormatLayoutValue(scheme.zhongwen.layoutH);
        return true;
    }
    if (key == L"zhongwen_layout_vertical") {
        *value = FormatLayoutValue(scheme.zhongwen.layoutV);
        return true;
    }
    if (key == L"pinyin_marge") {
        *value = FormatIntList(scheme.pinyin.margin, 4);
        return true;
    }
    if (key == L"zhongwen_marge") {
        *value = FormatIntList(scheme.zhongwen.margin, 4);
        return true;
    }
    return false;
}

// Removes [Scheme_*] layout/margin/separator/anchor keys and [Render] sizing
// keys that exactly match what the old settings UI generated by multiplying
// the skin's factory values with font_size / base_font_size.  Keys that differ
// (hand-edited advanced values) are kept.
int StripLegacyScaledLayoutKeys(
    IniSectionMap* sections,
    const std::map<std::wstring, Scheme>& baseSchemes,
    const RenderSettings& baseRender,
    const std::map<std::wstring, std::wstring>& baseStatusPositions,
    double scale) {
    int removed = 0;
    for (auto& [sectionName, values] : *sections) {
        if (sectionName.rfind(L"scheme_", 0) == 0) {
            const auto schemeIt = baseSchemes.find(sectionName);
            if (schemeIt == baseSchemes.end()) {
                continue;
            }
            const Scheme& scheme = schemeIt->second;
            for (auto it = values.begin(); it != values.end();) {
                std::wstring baseValue;
                if (!BaseSchemeValue(scheme, it->first, &baseValue)) {
                    ++it;
                    continue;
                }
                const bool keepFirst = it->first.find(L"layout_") != std::wstring::npos;
                const bool colorFirst = it->first == L"separator";
                if (ValueMatchesScaled(baseValue, it->second, scale, keepFirst,
                                       colorFirst)) {
                    it = values.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
            continue;
        }
        if (sectionName == L"statusbar") {
            for (auto it = values.begin(); it != values.end();) {
                if (it->first.size() > 4 &&
                    it->first.compare(it->first.size() - 4, 4, L"_pos") == 0) {
                    const auto baseIt = baseStatusPositions.find(it->first);
                    if (baseIt != baseStatusPositions.end() &&
                        ValueMatchesScaled(baseIt->second, it->second, scale,
                                           false)) {
                        it = values.erase(it);
                        ++removed;
                        continue;
                    }
                }
                ++it;
            }
            continue;
        }
        if (sectionName == L"render") {
            struct ScaledKey {
                const wchar_t* key;
                int baseValue;
                bool alwaysPresent;
            };
            const ScaledKey keys[] = {
                {L"item_spacing", baseRender.itemSpacing, true},
                {L"text_offset_y", baseRender.textOffsetY, true},
                {L"candidate_row_height", baseRender.candidateRowHeight,
                 baseRender.candidateRowHeight > 0},
            };
            for (const auto& entry : keys) {
                if (!entry.alwaysPresent) {
                    continue;
                }
                const auto it = values.find(entry.key);
                if (it == values.end()) {
                    continue;
                }
                int overrideValue = 0;
                if (ParseInt(it->second, &overrideValue) &&
                    overrideValue ==
                        static_cast<int>(std::lround(entry.baseValue * scale))) {
                    values.erase(it);
                    ++removed;
                }
            }
        }
    }
    return removed;
}

void ParseRenderSettings(const std::map<std::wstring, std::wstring>& section,
                         RenderSettings* render) {
    render->keyGap = std::max(0, GetInt(section, L"key_gap", render->keyGap));
    render->itemSpacing =
        std::max(0, GetInt(section, L"item_spacing", render->itemSpacing));
    render->pinyinBold = GetBool(section, L"pinyin_bold", render->pinyinBold);
    render->textOffsetY = GetInt(section, L"text_offset_y", render->textOffsetY);
    render->candidateRowHeight =
        std::max(0, GetInt(section, L"candidate_row_height",
                           render->candidateRowHeight));
    const std::wstring separator =
        ToLower(GetString(section, L"pinyin_separator"));
    if (separator == L"apostrophe" || separator == L"'") {
        render->pinyinSeparator = RenderSettings::PinyinSeparator::Apostrophe;
    } else if (separator == L"space" || separator == L"blank") {
        render->pinyinSeparator = RenderSettings::PinyinSeparator::Space;
    } else {
        render->pinyinSeparator = RenderSettings::PinyinSeparator::Auto;
    }
}

} // namespace

std::wstring ConfigDirectory() {
    return MoqiConfigDirectory();
}

std::wstring OverridesPath() {
    const std::wstring dir = MoqiConfigDirectory();
    return dir.empty() ? std::wstring() : JoinPath(dir, L"sogou_skin_overrides.ini");
}

std::wstring OptionsPath() {
    const std::wstring dir = MoqiConfigDirectory();
    return dir.empty() ? std::wstring() : JoinPath(dir, L"sogou_skin_options.ini");
}

std::wstring InstalledSkinPath() {
    const std::wstring dir = MoqiConfigDirectory();
    return dir.empty() ? std::wstring() : JoinPath(dir, L"sogou_skin.ssf");
}

std::wstring SkinLayoutsPath() {
    const std::wstring dir = MoqiConfigDirectory();
    return dir.empty() ? std::wstring()
                       : JoinPath(dir, L"sogou_skin_layouts.ini");
}

SkinLayout LoadSkinLayoutOffsets(const std::wstring& skinId) {
    SkinLayout layout;
    if (skinId.empty()) {
        return layout;
    }
    IniSectionMap sections;
    if (!ReadOverrideSections(SkinLayoutsPath(), &sections)) {
        return layout;
    }
    const auto it = sections.find(ToLower(skinId));
    if (it == sections.end()) {
        return layout;
    }
    const auto& section = it->second;
    layout.preeditX = GetInt(section, L"preedit_x", 0);
    layout.preeditY = GetInt(section, L"preedit_y", 0);
    layout.candidateX = GetInt(section, L"candidate_x", 0);
    layout.candidateY = GetInt(section, L"candidate_y", 0);
    return ClampLayout(layout);
}

std::wstring BuildSkinLayoutsText(const std::wstring& existingPath,
                                  const std::wstring& skinId,
                                  const SkinLayout& layout) {
    IniSectionMap sections;
    ReadOverrideSections(existingPath, &sections);
    const SkinLayout clamped = ClampLayout(layout);
    auto& marker = sections[L"moqi_layouts"];
    marker[L"version"] = L"1";
    if (!skinId.empty()) {
        auto& section = sections[ToLower(skinId)];
        section[L"preedit_x"] = std::to_wstring(clamped.preeditX);
        section[L"preedit_y"] = std::to_wstring(clamped.preeditY);
        section[L"candidate_x"] = std::to_wstring(clamped.candidateX);
        section[L"candidate_y"] = std::to_wstring(clamped.candidateY);
    }
    std::wostringstream out;
    for (const auto& [sectionName, values] : sections) {
        if (values.empty()) {
            continue;
        }
        out << L"[" << sectionName << L"]\n";
        for (const auto& [key, value] : values) {
            out << key << L"=" << value << L"\n";
        }
        out << L"\n";
    }
    return out.str();
}

bool SaveSkinLayoutOffsets(const std::wstring& skinId, const SkinLayout& layout) {
    const std::wstring path = SkinLayoutsPath();
    if (path.empty()) {
        return false;
    }
    const std::wstring config = MoqiConfigDirectory();
    if (!config.empty()) {
        ::CreateDirectoryW(config.c_str(), nullptr);
    }
    return WriteAllBytes(path, Utf16LeBytes(
                                  BuildSkinLayoutsText(path, skinId, layout)));
}

std::shared_ptr<Theme> Theme::Load(const std::wstring& path, std::wstring* error) {
    return LoadInternal(path, OverridesPath(), error);
}

std::shared_ptr<Theme> Theme::LoadWithOverrides(const std::wstring& path,
                                                const std::wstring& overridesPath,
                                                std::wstring* error) {
    return LoadInternal(path, overridesPath, error);
}

std::shared_ptr<Theme> LoadThemeWithOverrides(const std::wstring& path,
                                              const std::wstring& overridesPath,
                                              std::wstring* error) {
    return Theme::LoadWithOverrides(path, overridesPath, error);
}

std::shared_ptr<Theme> Theme::LoadInternal(const std::wstring& path,
                                           const std::wstring& overridesPath,
                                           std::wstring* error) {
    if (!ensureGdiplus()) {
        if (error) {
            *error = L"GDI+ 初始化失败";
        }
        return nullptr;
    }
    auto theme = std::shared_ptr<Theme>(new Theme());
    theme->gdiplusStarted_ = true;
    theme->overridesPath_ = overridesPath;

    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    bool ok = false;
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        ok = theme->loadFromDirectory(path, error);
    } else {
        std::vector<unsigned char> probe;
        if (ReadFileBytes(path, &probe) && probe.size() >= 4 && probe[0] == 'P' &&
            probe[1] == 'K') {
            ok = theme->loadFromArchive(path, error);
        } else {
            // A direct path to skin.ini.
            const size_t slash = path.find_last_of(L"\\/");
            theme->baseDir_ = slash == std::wstring::npos ? L"." : path.substr(0, slash);
            std::vector<unsigned char> data;
            if (ReadFileBytes(path, &data)) {
                std::vector<std::pair<std::wstring, unsigned long long>> files;
                CollectSkinFiles(theme->baseDir_, L"", 0, &files);
                theme->skinId_ =
                    SkinIdFromContent(data, std::move(files));
                ok = theme->loadIni(DecodeIniText(data), error);
            }
        }
    }
    theme->valid_ = ok;
    if (ok) {
        theme->loadLayoutOffsets();
    }
    if (!ok && error && error->empty()) {
        *error = L"未知加载错误";
    }
    return ok ? theme : nullptr;
}

Options LoadOptions() {
    Options options;
    const std::wstring path = OptionsPath();
    if (!path.empty()) {
        std::vector<unsigned char> data;
        if (ReadFileBytes(path, &data)) {
            std::wstring text = DecodeIniText(data);
            std::wstringstream stream(text);
            std::wstring line;
            while (std::getline(stream, line)) {
                line = Trim(line);
                if (line.empty() || line[0] == L';' || line[0] == L'#') {
                    continue;
                }
                const size_t equals = line.find(L'=');
                if (equals == std::wstring::npos) {
                    continue;
                }
                const std::wstring key = ToLower(Trim(line.substr(0, equals)));
                const std::wstring value = ToLower(Trim(line.substr(equals + 1)));
                if (key == L"statusbar") {
                    options.statusbar = value != L"0" && value != L"false" &&
                                        value != L"off";
                } else if (key == L"horizontal") {
                    options.horizontal = value == L"1" || value == L"true" ||
                                         value == L"on";
                } else if (key == L"key_repeat") {
                    options.keyRepeat = value != L"0" && value != L"false" &&
                                        value != L"off";
                } else if (key == L"statusbar_fullscreen_hide") {
                    options.statusbarFullscreenHide =
                        value != L"0" && value != L"false" && value != L"off";
                } else if (key == L"statusbar_fullscreen_scope") {
                    options.statusbarFullscreenScope =
                        value == L"all" ? FullscreenScope::All
                                        : FullscreenScope::Monitor;
                } else if (key == L"statusbar_hide") {
                    for (const auto& part : SplitComma(value)) {
                        const std::wstring name = ToLower(Trim(part));
                        if (!name.empty()) {
                            options.hiddenStatusItems.push_back(name);
                        }
                    }
                } else if (key == L"statusbar_x") {
                    ParseInt(value, &options.statusbarX);
                } else if (key == L"statusbar_y") {
                    ParseInt(value, &options.statusbarY);
                }
            }
        }
    }
    wchar_t value[16] = {};
    if (::GetEnvironmentVariableW(L"MOQI_SOGOU_STATUSBAR", value,
                                  _countof(value)) > 0) {
        options.statusbar = value[0] != L'0';
    }
    if (::GetEnvironmentVariableW(L"MOQI_SOGOU_STATUSBAR_FULLSCREEN", value,
                                  _countof(value)) > 0) {
        const std::wstring fullscreenValue = ToLower(value);
        options.statusbarFullscreenHide =
            fullscreenValue != L"0" && fullscreenValue != L"false" &&
            fullscreenValue != L"off";
    }
    if (::GetEnvironmentVariableW(L"MOQI_SOGOU_HORIZONTAL", value,
                                  _countof(value)) > 0) {
        options.horizontal = value[0] != L'0';
    }
    return options;
}

std::shared_ptr<Theme> LoadPreferredTheme(std::wstring* error, std::wstring* loadedPath) {
    std::vector<std::wstring> candidates;

    wchar_t envPath[32768] = {};
    const DWORD envLen = ::GetEnvironmentVariableW(L"MOQI_SOGOU_SKIN", envPath,
                                                   _countof(envPath));
    if (envLen > 0 && envLen < _countof(envPath)) {
        candidates.emplace_back(envPath);
    }

    wchar_t appData[MAX_PATH] = {};
    const DWORD appDataLen = ::GetEnvironmentVariableW(L"APPDATA", appData,
                                                       _countof(appData));
    if (appDataLen > 0 && appDataLen < _countof(appData)) {
        const std::wstring moqi = std::wstring(appData) + L"\\Moqi";
        std::vector<unsigned char> pathBytes;
        if (ReadFileBytes(moqi + L"\\sogou_skin_path.txt", &pathBytes)) {
            std::wstring line = DecodeIniText(pathBytes);
            const size_t newline = line.find_first_of(L"\r\n");
            if (newline != std::wstring::npos) {
                line.resize(newline);
            }
            while (!line.empty() &&
                   (line.back() == L' ' || line.back() == L'\t')) {
                line.pop_back();
            }
            while (!line.empty() &&
                   (line.front() == L' ' || line.front() == L'\t')) {
                line.erase(line.begin());
            }
            if (!line.empty()) {
                candidates.push_back(line);
            }
        }
        candidates.push_back(moqi + L"\\sogou_skin.ssf");
        candidates.push_back(moqi + L"\\sogou_skin");
        candidates.push_back(moqi + L"\\skins\\current");
    }

    std::wstring lastError;
    for (const auto& path : candidates) {
        if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        std::wstring pathError;
        auto theme = Theme::Load(path, &pathError);
        if (theme) {
            if (loadedPath) {
                *loadedPath = path;
            }
            return theme;
        }
        lastError = pathError;
    }
    if (error) {
        *error = lastError.empty() ? L"未找到 Sogou 皮肤路径" : lastError;
    }
    return nullptr;
}

Theme::~Theme() = default;

void Theme::setLayout(const SkinLayout& layout) {
    layout_ = ClampLayout(layout);
}

void Theme::loadLayoutOffsets() {
    layout_ = LoadSkinLayoutOffsets(skinId_);
}

bool Theme::loadFromDirectory(const std::wstring& dir, std::wstring* error) {
    baseDir_ = dir;
    const std::wstring iniPath = dir + L"\\skin.ini";
    std::ifstream in(iniPath, std::ios::binary);
    if (!in) {
        if (error) {
            *error = L"找不到 skin.ini: " + iniPath;
        }
        return false;
    }
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    std::vector<std::pair<std::wstring, unsigned long long>> files;
    CollectSkinFiles(dir, L"", 0, &files);
    skinId_ = SkinIdFromContent(data, std::move(files));
    const std::wstring text = DecodeIniText(data);
    return loadIni(text, error);
}

bool Theme::loadFromArchive(const std::wstring& file, std::wstring* error) {
    if (!ReadFileBytes(file, &zipBytes_)) {
        if (error) {
            *error = L"无法读取皮肤文件";
        }
        return false;
    }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipBytes_.data(), zipBytes_.size(), 0)) {
        if (error) {
            *error = L"不是有效的 .ssf/ZIP 文件";
        }
        return false;
    }
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    std::vector<unsigned char> iniData;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            continue;
        }
        const std::wstring name = ToLower(Utf8ToWide(stat.m_filename));
        std::vector<unsigned char> data(static_cast<size_t>(stat.m_uncomp_size));
        if (!data.empty() &&
            !mz_zip_reader_extract_to_mem(&zip, i, data.data(), data.size(), 0)) {
            continue;
        }
        if (name == L"skin.ini") {
            iniData = data;
        } else {
            zipEntries_[name] = std::move(data);
        }
    }
    mz_zip_reader_end(&zip);
    if (iniData.empty()) {
        if (error) {
            *error = L"皮肤包内没有 skin.ini";
        }
        return false;
    }
    skinId_ = SkinIdFromBytes(zipBytes_);
    return loadIni(DecodeIniText(iniData), error);
}

bool Theme::loadIni(const std::wstring& text, std::wstring* error) {
    if (text.empty()) {
        if (error) {
            *error = L"skin.ini 为空或编码不受支持";
        }
        return false;
    }
    IniSectionMap sections;
    ParseIniSections(text, &sections);

    // Capture the skin's own font size before overrides are merged.  All
    // layout values in skin.ini are authored in this coordinate system; the
    // renderer measures text with Display::fontSize and scales the finished
    // plan by Display::scalePercent (not by the font ratio).
    const auto baseDisplayIt = sections.find(L"display");
    baseFontSize_ = baseDisplayIt != sections.end()
                        ? std::max(1, GetInt(baseDisplayIt->second, L"font_size", 16))
                        : 16;

    // Parse the factory schemes and render defaults once so legacy overrides
    // written by the old settings UI can be recognised and dropped.
    std::map<std::wstring, Scheme> baseSchemes;
    for (const auto& [sectionName, section] : sections) {
        if (sectionName.rfind(L"scheme_", 0) != 0) {
            continue;
        }
        Scheme scheme;
        parseScheme(section, sectionName, &scheme);
        if (!scheme.single.pic.empty() || !scheme.pinyin.pic.empty()) {
            baseSchemes[sectionName] = std::move(scheme);
        }
    }
    RenderSettings baseRender;
    const auto baseRenderIt = sections.find(L"render");
    if (baseRenderIt != sections.end()) {
        ParseRenderSettings(baseRenderIt->second, &baseRender);
    }

    IniSectionMap overrides;
    const bool haveOverrides = ReadOverrideSections(overridesPath_, &overrides);
    bool legacyFontOverride = false;
    if (haveOverrides && !HasNewFontScaleSemantics(overrides)) {
        const auto overrideDisplay = overrides.find(L"display");
        legacyFontOverride =
            overrideDisplay != overrides.end() &&
            overrideDisplay->second.find(L"font_size") !=
                overrideDisplay->second.end();
    }
    if (haveOverrides && !HasGlobalLayoutMarker(overrides) &&
        !HasNewFontScaleSemantics(overrides)) {
        int overrideFontSize = baseFontSize_;
        const auto overrideDisplay = overrides.find(L"display");
        if (overrideDisplay != overrides.end()) {
            overrideFontSize =
                GetInt(overrideDisplay->second, L"font_size", baseFontSize_);
        }
        const double legacyScale =
            baseFontSize_ > 0
                ? static_cast<double>(std::max(1, overrideFontSize)) / baseFontSize_
                : 1.0;
        std::map<std::wstring, std::wstring> baseStatusPositions;
        const auto baseStatusIt = sections.find(L"statusbar");
        if (baseStatusIt != sections.end()) {
            for (const auto& [key, value] : baseStatusIt->second) {
                if (key.size() > 4 &&
                    key.compare(key.size() - 4, 4, L"_pos") == 0) {
                    baseStatusPositions[key] = value;
                }
            }
        }
        StripLegacyScaledLayoutKeys(&overrides, baseSchemes, baseRender,
                                    baseStatusPositions, legacyScale);
    }
    MergeOverrideSections(&sections, overrides);

    const auto displayIt = sections.find(L"display");
    if (displayIt != sections.end()) {
        const auto& section = displayIt->second;
        display_.fontSize = GetInt(section, L"font_size", baseFontSize_);
        const auto fontCh = GetString(section, L"font_ch");
        if (!fontCh.empty()) {
            display_.fontCh = fontCh;
        }
        const auto fontEn = GetString(section, L"font_en");
        if (!fontEn.empty()) {
            display_.fontEn = fontEn;
        }
        display_.pinyinColor = ParseColor(GetString(section, L"pinyin_color"), RGB(0, 0, 0));
        display_.firstColor =
            ParseColor(GetString(section, L"zhongwen_first_color"), RGB(0, 0, 0));
        display_.textColor = ParseColor(GetString(section, L"zhongwen_color"), RGB(0, 0, 0));
        display_.commentColor = ParseColor(GetString(section, L"comphint_color"), RGB(0, 0, 0));
        display_.useGdip = GetBool(section, L"use_gdip", true);
        display_.aero = GetBool(section, L"aero", false);
        display_.glow = GetBool(section, L"glow", false);
    }
    if (legacyFontOverride) {
        // The old UI used font_size as "font size AND whole-window scale".
        // Preserve the resulting window geometry by moving that ratio into
        // the new scale field and keeping the skin's native text size.
        display_.scalePercent =
            ScalePercentFromLegacyFontSize(display_.fontSize, baseFontSize_);
        display_.fontSize = std::max(1, baseFontSize_);
    } else {
        const int scalePercent =
            displayIt != sections.end()
                ? GetInt(displayIt->second, L"scale", 100)
                : 100;
        display_.scalePercent =
            std::clamp(scalePercent, kMinScalePercent, kMaxScalePercent);
    }

    const auto renderIt = sections.find(L"render");
    if (renderIt != sections.end()) {
        ParseRenderSettings(renderIt->second, &render_);
    }

    const auto statusIt = sections.find(L"statusbar");
    if (statusIt != sections.end()) {
        const auto& section = statusIt->second;
        statusBar_.pic = GetString(section, L"pic");
        statusBar_.mask = GetString(section, L"pic_mask");
        std::vector<std::wstring> names;
        for (const auto& [key, value] : section) {
            for (const wchar_t* suffix : {L"_display", L"_pos"}) {
                const size_t suffixLen = std::wcslen(suffix);
                if (key.size() > suffixLen &&
                    key.compare(key.size() - suffixLen, suffixLen, suffix) == 0) {
                    const std::wstring name = key.substr(0, key.size() - suffixLen);
                    if (std::find(names.begin(), names.end(), name) == names.end()) {
                        names.push_back(name);
                    }
                }
            }
        }
        auto splitList = [](const std::wstring& value) {
            std::vector<std::wstring> result;
            for (const auto& part : SplitComma(value)) {
                if (!part.empty()) {
                    result.push_back(part);
                }
            }
            return result;
        };
        for (const auto& name : names) {
            StatusItem item;
            item.name = name;
            item.display = GetInt(section, (name + L"_display").c_str(), 0) != 0;
            const auto pos = SplitComma(GetString(section, (name + L"_pos").c_str()));
            if (pos.size() >= 2) {
                ParseInt(pos[0], reinterpret_cast<int*>(&item.pos.x));
                ParseInt(pos[1], reinterpret_cast<int*>(&item.pos.y));
            }
            item.normal = splitList(GetString(section, name.c_str()));
            item.hover = splitList(GetString(section, (name + L"_hover").c_str()));
            item.down = splitList(GetString(section, (name + L"_down").c_str()));
            if (!item.normal.empty()) {
                statusBar_.items.push_back(std::move(item));
            }
        }
    }

    for (const auto& [sectionName, section] : sections) {
        if (sectionName.rfind(L"scheme_", 0) != 0) {
            continue;
        }
        Scheme scheme;
        scheme.name = sectionName;
        parseScheme(section, sectionName, &scheme);
        if (!scheme.single.pic.empty() || !scheme.pinyin.pic.empty()) {
            schemes_[sectionName] = std::move(scheme);
        }
    }
    if (schemes_.empty()) {
        if (error) {
            *error = L"skin.ini 中没有可用的 Scheme_H1/H2/V1/V2";
        }
        return false;
    }
    return true;
}

void Theme::parseScheme(const std::map<std::wstring, std::wstring>& section,
                        const std::wstring& name, Scheme* scheme) {
    scheme->name = name;
    const auto pic = GetString(section, L"pic");
    if (!pic.empty()) {
        scheme->single.pic = pic;
        scheme->single.mask = GetString(section, L"pic_mask");
        ParseLayout(GetString(section, L"layout_horizontal"), &scheme->single.layoutH);
        ParseLayout(GetString(section, L"layout_vertical"), &scheme->single.layoutV);
        ParseMargin(GetString(section, L"pinyin_marge"), scheme->pinyinMargin);
        ParseMargin(GetString(section, L"zhongwen_marge"), scheme->zhongwenMargin);
        scheme->hasSeparator = !GetString(section, L"separator").empty();
        if (scheme->hasSeparator) {
            const auto parts = SplitComma(GetString(section, L"separator"));
            if (parts.size() >= 3) {
                scheme->separatorColor = ParseColor(parts[0], RGB(216, 216, 216));
                ParseInt(parts[1], &scheme->separatorLeft);
                ParseInt(parts[2], &scheme->separatorRight);
            }
        }
        const auto anchor = GetString(section, L"anchor");
        if (!anchor.empty()) {
            const auto parts = SplitComma(anchor);
            if (parts.size() >= 2) {
                scheme->hasAnchor = true;
                ParseInt(parts[0], &scheme->anchorX);
                ParseInt(parts[1], &scheme->anchorY);
            }
        }
    }

    const auto pinyinPic = GetString(section, L"pinyin_pic");
    if (!pinyinPic.empty()) {
        scheme->split = true;
        scheme->pinyin.pic = pinyinPic;
        scheme->pinyin.mask = GetString(section, L"pinyin_pic_mask");
        ParseLayout(GetString(section, L"pinyin_layout_horizontal"), &scheme->pinyin.layoutH);
        ParseLayout(GetString(section, L"pinyin_layout_vertical"), &scheme->pinyin.layoutV);
        ParseMargin(GetString(section, L"pinyin_marge"), scheme->pinyin.margin);
    }
    const auto zhongwenPic = GetString(section, L"zhongwen_pic");
    if (!zhongwenPic.empty()) {
        scheme->split = true;
        scheme->zhongwen.pic = zhongwenPic;
        scheme->zhongwen.mask = GetString(section, L"zhongwen_pic_mask");
        ParseLayout(GetString(section, L"zhongwen_layout_horizontal"),
                    &scheme->zhongwen.layoutH);
        ParseLayout(GetString(section, L"zhongwen_layout_vertical"),
                    &scheme->zhongwen.layoutV);
        ParseMargin(GetString(section, L"zhongwen_marge"), scheme->zhongwen.margin);
    }
}

std::wstring BuildSettingsOverridesText(const Theme& baseTheme,
                                        const std::wstring& existingPath,
                                        const SettingsOverrides& values,
                                        int* removedLegacyKeys) {
    IniSectionMap sections;
    ReadOverrideSections(existingPath, &sections);

    int removed = 0;
    if (!HasGlobalLayoutMarker(sections) &&
        !HasNewFontScaleSemantics(sections)) {
        int oldFontSize = baseTheme.baseFontSize();
        const auto displayIt = sections.find(L"display");
        if (displayIt != sections.end()) {
            oldFontSize = GetInt(displayIt->second, L"font_size", oldFontSize);
        }
        const double legacyscale =
            baseTheme.baseFontSize() > 0
                ? static_cast<double>(std::max(1, oldFontSize)) /
                      baseTheme.baseFontSize()
                : 1.0;
        std::map<std::wstring, Scheme> baseSchemes;
        for (const auto& name : baseTheme.schemeNames()) {
            if (const Scheme* scheme = baseTheme.scheme(name)) {
                baseSchemes[name] = *scheme;
            }
        }
        std::map<std::wstring, std::wstring> baseStatusPositions;
        for (const auto& item : baseTheme.statusBar().items) {
            baseStatusPositions[ToLower(item.name) + L"_pos"] =
                std::to_wstring(item.pos.x) + L"," +
                std::to_wstring(item.pos.y);
        }
        removed = StripLegacyScaledLayoutKeys(&sections, baseSchemes,
                                              baseTheme.render(),
                                              baseStatusPositions,
                                              legacyscale);
    }

    auto& display = sections[L"display"];
    display[L"font_size"] = std::to_wstring(std::max(1, values.fontSize));
    display[L"scale"] = std::to_wstring(
        std::clamp(values.scalePercent, kMinScalePercent, kMaxScalePercent));
    // Font faces are also UTF-16 in the INI (the settings UI writes the whole
    // file as UTF-16LE), so localized names such as 荆南麦圆体 round-trip
    // without loss.  An empty value leaves a hand-written override untouched.
    if (!values.fontCh.empty()) {
        display[L"font_ch"] = values.fontCh;
    }
    if (!values.fontEn.empty()) {
        display[L"font_en"] = values.fontEn;
    }
    auto formatColor = [](COLORREF color) {
        wchar_t buffer[16] = {};
        swprintf_s(buffer, L"0x%06X",
                   static_cast<unsigned int>(color & 0x00FFFFFF));
        return std::wstring(buffer);
    };
    display[L"pinyin_color"] = formatColor(values.pinyinColor);
    display[L"zhongwen_first_color"] = formatColor(values.firstColor);
    display[L"zhongwen_color"] = formatColor(values.textColor);
    display[L"comphint_color"] = formatColor(values.commentColor);

    auto& statusbar = sections[L"statusbar"];
    for (const auto& [name, visible] : values.statusItems) {
        statusbar[ToLower(name) + L"_display"] = visible ? L"1" : L"0";
    }

    auto& marker = sections[L"moqi_overrides"];
    marker[L"version"] = L"3";
    marker[L"layout_mode"] = L"global";

    std::wostringstream out;
    for (const auto& [sectionName, sectionValues] : sections) {
        if (sectionValues.empty()) {
            continue;
        }
        out << L"[" << sectionName << L"]\n";
        for (const auto& [key, value] : sectionValues) {
            out << key << L"=" << value << L"\n";
        }
        out << L"\n";
    }
    if (removedLegacyKeys) {
        *removedLegacyKeys = removed;
    }
    return out.str();
}

const Scheme* Theme::scheme(const std::wstring& name) const {
    const auto it = schemes_.find(ToLower(name));
    return it == schemes_.end() ? nullptr : &it->second;}

std::vector<std::wstring> Theme::schemeNames() const {
    std::vector<std::wstring> names;
    names.reserve(schemes_.size());
    for (const auto& [name, scheme] : schemes_) {
        names.push_back(name);
    }
    return names;
}

const Scheme* Theme::chooseScheme(bool horizontal) const {
    const wchar_t* first = horizontal ? L"scheme_h1" : L"scheme_v1";
    const wchar_t* split = horizontal ? L"scheme_h2" : L"scheme_v2";
    if (const auto* scheme = this->scheme(first); scheme && !scheme->single.pic.empty()) {
        return scheme;
    }
    if (const auto* scheme = this->scheme(split);
        scheme && (!scheme->pinyin.pic.empty() || !scheme->zhongwen.pic.empty())) {
        return scheme;
    }
    if (const auto* scheme = this->scheme(horizontal ? L"scheme_h2" : L"scheme_v2")) {
        return scheme;
    }
    return nullptr;
}

std::shared_ptr<Gdiplus::Bitmap> Theme::image(const std::wstring& name) const {
    if (name.empty()) {
        return nullptr;
    }
    const std::wstring key = ToLower(name);
    const auto cached = imageCache_.find(key);
    if (cached != imageCache_.end()) {
        return cached->second;
    }

    std::shared_ptr<Gdiplus::Bitmap> bitmap;
    if (!zipEntries_.empty()) {
        const auto it = zipEntries_.find(key);
        if (it != zipEntries_.end() && !it->second.empty()) {
            IStream* stream = ::SHCreateMemStream(it->second.data(),
                                                  static_cast<UINT>(it->second.size()));
            if (stream) {
                bitmap = std::make_shared<Gdiplus::Bitmap>(stream, FALSE);
                stream->Release();
            }
        }
    } else if (!baseDir_.empty()) {
        std::wstring path = baseDir_ + L"\\" + name;
        std::replace(path.begin(), path.end(), L'/', L'\\');
        bitmap = std::make_shared<Gdiplus::Bitmap>(path.c_str(), FALSE);
    }
    if (bitmap && bitmap->GetLastStatus() != Gdiplus::Ok) {
        bitmap.reset();
    }
    imageCache_[key] = bitmap;
    return bitmap;
}

std::shared_ptr<Gdiplus::Bitmap> Theme::mask(const std::wstring& name) const {
    if (name.empty()) {
        return nullptr;
    }
    const std::wstring key = ToLower(name);
    const auto cached = maskCache_.find(key);
    if (cached != maskCache_.end()) {
        return cached->second;
    }
    auto bitmap = image(name);
    maskCache_[key] = bitmap;
    return bitmap;
}

Renderer::Renderer(std::shared_ptr<Theme> theme, int dpi) : theme_(std::move(theme)), dpi_(dpi) {
    if (theme_) {
        fontPixelSize_ = std::max(1, theme_->display().fontSize);
        scalePercent_ = std::clamp(theme_->display().scalePercent,
                                   kMinScalePercent, kMaxScalePercent);
    }
    updateScale();
}

Renderer::~Renderer() = default;

void Renderer::updateScale() const {
    const double dpiScale = static_cast<double>(dpi_ > 0 ? dpi_ : 96) / 96.0;
    scale_ = (static_cast<double>(scalePercent_) / 100.0) * dpiScale;
}

void Renderer::setDpi(int dpi) {
    if (dpi <= 0) {
        dpi = 96;
    }
    if (dpi_ == dpi) {
        return;
    }
    dpi_ = dpi;
    updateScale();
    // Cached background bitmaps were composed at the previous scale.
    backgroundCache_.clear();
}

int Renderer::scaleValue(int value) const {
    return static_cast<int>(std::lround(value * scale_));
}

RECT Renderer::scaleRect(const RECT& rect) const {
    return RECT{scaleValue(rect.left), scaleValue(rect.top),
                scaleValue(rect.right), scaleValue(rect.bottom)};
}

void Renderer::ensureFonts(HDC hdc) const {
    if (fontsReady_) {
        return;
    }
    if (dpi_ <= 0) {
        dpi_ = hdc ? ::GetDeviceCaps(hdc, LOGPIXELSX) : 96;
        if (dpi_ <= 0) {
            dpi_ = 96;
        }
        updateScale();
        backgroundCache_.clear();
    }
    const auto& display = theme_->display();
    ownedFamilyCh_ = LoadFontFamily(&privateFonts_, display.fontCh);
    familyCh_ = ownedFamilyCh_ ? ownedFamilyCh_.get()
                               : Gdiplus::FontFamily::GenericSansSerif();
    if (ToLower(display.fontEn) == ToLower(display.fontCh)) {
        familyEn_ = familyCh_;
    } else {
        ownedFamilyEn_ = LoadFontFamily(&privateFonts_, display.fontEn);
        familyEn_ = ownedFamilyEn_ ? ownedFamilyEn_.get()
                                   : Gdiplus::FontFamily::GenericSansSerif();
    }
    fontsReady_ = true;
}

const Gdiplus::FontFamily* Renderer::familyFor(bool ascii) const {
    return ascii ? familyEn_ : familyCh_;
}

int Renderer::fontPixelSize() const {
    // Design-space font size.  The complete plan is scaled by scale_ at paint
    // time, so applying the monitor DPI here as well would double-scale text
    // while leaving skin bitmaps at 1x.
    return std::max(1, fontPixelSize_);
}

std::wstring Renderer::faceFor(bool ascii) const {
    return ascii ? theme_->display().fontEn : theme_->display().fontCh;
}

std::wstring displayPreedit(const std::wstring& text,
                            RenderSettings::PinyinSeparator separator) {
    if (separator == RenderSettings::PinyinSeparator::Space) {
        std::wstring result = text;
        for (wchar_t& ch : result) {
            if (ch == L'\'') {
                ch = L' ';
            }
        }
        return result;
    }
    // Auto: Sogou shows plain pinyin with apostrophes (ni'hao), but when the
    // preedit contains tone-marked vowels it keeps spaces (shì shì shāng).
    if (separator == RenderSettings::PinyinSeparator::Auto) {
        for (wchar_t ch : text) {
            if (ch >= 128) {
                return text;
            }
        }
    }
    std::wstring result = text;
    for (wchar_t& ch : result) {
        if (ch == L' ' || ch == L'\t') {
            ch = L'\'';
        }
    }
    return result;
}

namespace {

void SetTypographicFlags(Gdiplus::StringFormat* format) {
    format->SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap |
                           Gdiplus::StringFormatFlagsMeasureTrailingSpaces |
                           Gdiplus::StringFormatFlagsNoClip);
}

float MeasureRunGdiplus(Gdiplus::Graphics& graphics,
                        const Gdiplus::FontFamily* family, float fontSize,
                        const std::wstring& run) {
    if (run.empty() || !family) {
        return 0.0f;
    }
    Gdiplus::Font font(family, fontSize, Gdiplus::FontStyleRegular,
                       Gdiplus::UnitPixel);
    if (font.GetLastStatus() != Gdiplus::Ok) {
        return 0.0f;
    }
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    SetTypographicFlags(&format);
    Gdiplus::RectF bounds;
    if (graphics.MeasureString(run.c_str(), static_cast<INT>(run.size()), &font,
                               Gdiplus::PointF(0.0f, 0.0f), &format,
                               &bounds) != Gdiplus::Ok) {
        return 0.0f;
    }
    return bounds.Width;
}

} // namespace

int Renderer::measureTextGdiplus(Gdiplus::Graphics& graphics,
                                 const std::wstring& text) const {
    if (text.empty()) {
        return 0;
    }
    const float fontSize = static_cast<float>(fontPixelSize());
    int width = 0;
    for (size_t i = 0; i < text.size();) {
        size_t length = 1;
        if (IS_HIGH_SURROGATE(text[i]) && i + 1 < text.size() &&
            IS_LOW_SURROGATE(text[i + 1])) {
            length = 2;
        }
        const std::wstring character = text.substr(i, length);
        const bool ascii = length == 1 && text[i] < 128;
        std::wstring cacheKey = ascii ? L"A" : L"C";
        cacheKey += character;
        const auto cached = glyphWidthCache_.find(cacheKey);
        if (cached != glyphWidthCache_.end()) {
            width += cached->second;
            i += length;
            continue;
        }
        const int charWidth = static_cast<int>(
            std::lround(MeasureRunGdiplus(graphics, familyFor(ascii), fontSize,
                                          character)));
        // Pillow/FreeType hints U+00B4 (acute accent) one pixel narrower than
        // GDI+ at skin sizes; keep the reference layout byte-aligned for the
        // kaomoji candidates used by Dania.
        const int adjustedWidth = (length == 1 && text[i] == 0x00B4)
                                      ? std::max(0, charWidth - 1)
                                      : charWidth;
        glyphWidthCache_[cacheKey] = adjustedWidth;
        width += adjustedWidth;
        i += length;
    }
    return width;
}

int Renderer::drawTextRuns(Gdiplus::Graphics& graphics, int x, int y,
                           const std::wstring& text, COLORREF color,
                           bool bold) const {
    if (text.empty()) {
        return 0;
    }
    const float fontSize = static_cast<float>(fontPixelSize());
    Gdiplus::SolidBrush brush(Gdiplus::Color(
        255, GetRValue(color), GetGValue(color), GetBValue(color)));
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    SetTypographicFlags(&format);
    const int startX = x;

    auto drawRun = [&](const std::wstring& run, bool ascii) {
        const Gdiplus::FontFamily* family = familyFor(ascii);
        if (!family) {
            return;
        }
        Gdiplus::Font font(family, fontSize, Gdiplus::FontStyleRegular,
                           Gdiplus::UnitPixel);
        if (font.GetLastStatus() != Gdiplus::Ok) {
            return;
        }
        auto drawAt = [&](int dx, int dy) {
            graphics.DrawString(run.c_str(), static_cast<INT>(run.size()), &font,
                                Gdiplus::PointF(static_cast<float>(x + dx),
                                                static_cast<float>(y + dy)),
                                &format, &brush);
        };
        // Pillow's stroke_width=1 bold is equivalent to drawing the regular
        // glyphs at the four cardinal neighbours; this keeps the advance the
        // same as the regular text instead of using GDI+'s wider synthetic bold.
        drawAt(0, 0);
        if (bold) {
            drawAt(-1, 0);
            drawAt(1, 0);
            drawAt(0, -1);
            drawAt(0, 1);
        }
        x += measureTextGdiplus(graphics, run);
    };

    bool runAscii = text.front() < 128;
    size_t start = 0;
    for (size_t i = 1; i <= text.size(); ++i) {
        const bool ascii = i < text.size() && text[i] < 128;
        if (i == text.size() || ascii != runAscii) {
            drawRun(text.substr(start, i - start), runAscii);
            start = i;
            runAscii = ascii;
        }
    }
    return x - startX;
}

int Renderer::drawMixedTextGdiplus(Gdiplus::Graphics& graphics, int x, int y,
                                   const std::wstring& text, COLORREF color,
                                   bool bold) const {
    return drawTextRuns(graphics, x, y, text, color, bold);
}

void Renderer::drawSliced(Gdiplus::Graphics& graphics, Gdiplus::Bitmap* bitmap,
                          const RECT& dest, const Layout& h, const Layout& v) const {
    if (!bitmap) {
        return;
    }
    const int sw = static_cast<int>(bitmap->GetWidth());
    const int sh = static_cast<int>(bitmap->GetHeight());
    const int dw = dest.right - dest.left;
    const int dh = dest.bottom - dest.top;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return;
    }
    int left = std::clamp(h.fixed1, 0, sw);
    int right = std::clamp(h.fixed2, 0, sw - left);
    int top = std::clamp(v.fixed1, 0, sh);
    int bottom = std::clamp(v.fixed2, 0, sh - top);
    const int midW = sw - left - right;
    const int midH = sh - top - bottom;

    struct Band {
        int src;
        int srcLen;
        int dst;
        int dstLen;
        bool tile = false;
    };
    std::vector<Band> xBands;
    if (dw == sw) {
        xBands.push_back({0, sw, 0, sw, false});
    } else {
        if (left > 0) {
            xBands.push_back({0, left, 0, left, false});
        }
        if (midW > 0) {
            const int middleWidth = std::max(1, dw - left - right);
            xBands.push_back({left, midW, left, middleWidth, h.mode == 1});
        }
        if (right > 0) {
            xBands.push_back({sw - right, right, dw - right, right, false});
        }
    }
    std::vector<Band> yBands;
    if (v.mode == 2 || dh == sh) {
        yBands.push_back({0, sh, 0, sh, false});
    } else {
        if (top > 0) {
            yBands.push_back({0, top, 0, top, false});
        }
        if (midH > 0) {
            const int middleHeight = std::max(1, dh - top - bottom);
            yBands.push_back({top, midH, top, middleHeight, v.mode == 1});
        }
        if (bottom > 0) {
            yBands.push_back({sh - bottom, bottom, dh - bottom, bottom, false});
        }
    }

    auto drawTiles = [&](const Band& xb, const Band& yb) {
        int dy = yb.dst;
        int remainingY = yb.dstLen;
        do {
            const int srcH = yb.tile ? std::min(yb.srcLen, remainingY) : yb.srcLen;
            const int dstH = yb.tile ? srcH : yb.dstLen;
            if (srcH <= 0 || dstH <= 0) {
                break;
            }
            int dx = xb.dst;
            int remainingX = xb.dstLen;
            do {
                const int srcW = xb.tile ? std::min(xb.srcLen, remainingX) : xb.srcLen;
                const int dstW = xb.tile ? srcW : xb.dstLen;
                if (srcW <= 0 || dstW <= 0) {
                    break;
                }
                Gdiplus::Rect dstRect(dest.left + dx, dest.top + dy, dstW, dstH);
                graphics.DrawImage(bitmap, dstRect, xb.src, yb.src, srcW, srcH,
                                   Gdiplus::UnitPixel);
                ++lastRenderTiming_.imageDraws;
                dx += srcW;
                remainingX -= srcW;
            } while (xb.tile && remainingX > 0);
            dy += srcH;
            remainingY -= srcH;
        } while (yb.tile && remainingY > 0);
    };

    for (const auto& yb : yBands) {
        for (const auto& xb : xBands) {
            drawTiles(xb, yb);
        }
    }
}

void Renderer::drawComponent(Gdiplus::Graphics& graphics, const Component& component,
                             const RECT& dest) const {
    auto bitmap = theme_->image(component.pic);
    if (!bitmap) {
        return;
    }
    drawSliced(graphics, bitmap.get(), dest, component.layoutH, component.layoutV);
}

int Renderer::itemWidth(HDC hdc, const std::wstring& text,
                        const std::wstring& key) const {
    Gdiplus::Graphics graphics(hdc);
    return measureTextGdiplus(graphics, key) + theme_->render().keyGap +
           measureTextGdiplus(graphics, text);
}

std::wstring Renderer::selKeyFor(size_t index, const std::wstring& keys) const {
    wchar_t key = static_cast<wchar_t>(L'1' + index);
    if (index >= 9) {
        key = L'0';
    }
    if (index < keys.size()) {
        key = keys[index];
    }
    return std::wstring(1, key) + L".";
}

Renderer::Plan Renderer::buildPlan(HDC hdc, const RenderState& state) const {
    Plan plan;
    if (!theme_ || !hdc) {
        return plan;
    }
    ensureFonts(hdc);
    plan.scheme = theme_->chooseScheme(state.candPerRow > 1);
    if (!plan.scheme) {
        return plan;
    }
    if (plan.scheme->hasAnchor) {
        plan.anchor = {plan.scheme->anchorX, plan.scheme->anchorY};
    }
    const auto& render = theme_->render();
    const int fontPixel = fontPixelSize();
    const int baseFont = theme_ ? std::max(1, theme_->baseFontSize()) : fontPixel;
    // A larger configured font needs room for the glyphs, but the skin's
    // 9-slice/margins must not be multiplied.  Only enlarge the affected
    // dimension when the user actually exceeds the skin's native font size;
    // the native-size path stays byte-identical to the historical output.
    const bool enlargedFont = fontPixel > baseFont;
    const std::wstring preeditText =
        displayPreedit(state.preedit, render.pinyinSeparator);
    Gdiplus::Graphics measureGraphics(hdc);
    const int preeditW = measureTextGdiplus(measureGraphics, preeditText);
    std::vector<int> widths;
    widths.reserve(state.candidates.size());
    for (size_t i = 0; i < state.candidates.size(); ++i) {
        widths.push_back(itemWidth(hdc, state.candidates[i],
                                   selKeyFor(i, state.selKeys)));
    }
    int rowW = 0;
    for (size_t i = 0; i < widths.size(); ++i) {
        rowW += widths[i] + (i ? render.itemSpacing : 0);
    }
    auto rowHeight = [&](int automatic) {
        return render.candidateRowHeight > 0 ? render.candidateRowHeight : automatic;
    };
    plan.split = plan.scheme->split;
    plan.hasSeparator = plan.scheme->hasSeparator;
    plan.horizontal = ToLower(plan.scheme->name).find(L"_h") != std::wstring::npos;
    const auto& py = plan.scheme->pinyinMargin;
    const auto& zw = plan.scheme->zhongwenMargin;

    if (plan.split) {
        auto pyBitmap = theme_->image(plan.scheme->pinyin.pic);
        auto zwBitmap = theme_->image(plan.scheme->zhongwen.pic);
        const int pyBgW = pyBitmap ? static_cast<int>(pyBitmap->GetWidth()) : 0;
        const int pyBgH = pyBitmap ? static_cast<int>(pyBitmap->GetHeight()) : 0;
        const int zwBgW = zwBitmap ? static_cast<int>(zwBitmap->GetWidth()) : 0;
        const int zwBgH = zwBitmap ? static_cast<int>(zwBitmap->GetHeight()) : 0;
        const int pyW = std::max(pyBgW, preeditW + py[2] + py[3]);
        int pyH = std::max(1, pyBgH);
        if (enlargedFont) {
            pyH = std::max(pyH, py[0] + fontPixel + py[1]);
        }
        const int rowH = rowHeight(fontPixel + kRowPadding);
        int zwW = 0;
        int zwH = std::max(1, zwBgH);
        int textTop = zw[0] - 1;
        int maxItemW = 0;
        for (int width : widths) {
            maxItemW = std::max(maxItemW, width);
        }
        if (plan.horizontal) {
            // H2: candidates share the zhongwen slice's original height.
            zwW = std::max(zwBgW, rowW + zw[2] + zw[3] + kPageArrowArea);
            if (enlargedFont) {
                zwH = std::max(zwH, textTop + fontPixel + zw[1]);
            }
        } else {
            // V2: one blank row height before the first candidate, exactly as
            // the Python reference (and official Sogou) lays out V1/V2.
            textTop = zw[0] + rowH;
            zwW = std::max(zwBgW, maxItemW + zw[2] + zw[3]);
            zwH = std::max(zwBgH, textTop +
                                       (std::max<int>(1, widths.size()) - 1) * rowH +
                                       fontPixel + zw[1]);
        }
        plan.width = std::max(pyW, zwW);
        const int splitGap = plan.horizontal ? kSplitGap : 0;
        plan.height = pyH + splitGap + zwH;
        plan.primaryRect = {0, 0, pyW, pyH};
        plan.secondaryRect = {0, pyH + splitGap, zwW, pyH + splitGap + zwH};
        plan.preeditRect = {py[2], py[0], py[2] + preeditW, py[0] + fontPixel};
        int x = zw[2];
        for (size_t i = 0; i < widths.size(); ++i) {
            RECT item = {x, pyH + splitGap + textTop, x + widths[i],
                         pyH + splitGap + textTop + fontPixel};
            if (!plan.horizontal) {
                item.top = pyH + textTop + static_cast<int>(i) * rowH;
                item.bottom = item.top + rowH;
            }
            plan.candidateRects.push_back(item);
            x += widths[i] + render.itemSpacing;
        }
        if (plan.horizontal) {
            plan.arrowX = zwW - 6;
            plan.arrowY = pyH + splitGap + zw[0] + fontPixel / 2 + 1;
        } else {
            plan.arrowX = zw[2] + 20;
            plan.arrowY = plan.height - 8;
        }
        plan.arrowsHorizontal = plan.horizontal;
        applyTextOffsets(&plan);
        return plan;
    }

    auto bgBitmap = theme_->image(plan.scheme->single.pic);
    const int bgW = bgBitmap ? static_cast<int>(bgBitmap->GetWidth()) : 0;
    const int bgH = bgBitmap ? static_cast<int>(bgBitmap->GetHeight()) : 0;
    const int sepY = py[0] + fontPixel + 2 + py[1];
    plan.sepY = sepY;
    if (state.candPerRow > 1) {
        plan.width = std::max(bgW, std::max(preeditW + py[2] + py[3],
                                             rowW + zw[2] + zw[3] + kPageArrowArea));
        const int y = sepY + zw[0] - 1;
        plan.height = std::max(1, bgH);
        if (enlargedFont) {
            plan.height = std::max(plan.height, y + fontPixel + zw[1]);
        }
        plan.primaryRect = {0, 0, plan.width, plan.height};
        plan.preeditRect = {py[2], py[0], py[2] + preeditW, py[0] + fontPixel};
        int x = zw[2];
        for (size_t i = 0; i < widths.size(); ++i) {
            RECT item = {x, y, x + widths[i], y + fontPixel};
            plan.candidateRects.push_back(item);
            x += widths[i] + render.itemSpacing;
        }
        plan.arrowX = plan.width - 6;
        plan.arrowY = y + fontPixel / 2 + 2;
        plan.arrowsHorizontal = true;
    } else {
        const int rowH = rowHeight(fontPixel + kRowPadding);
        const int textTop = sepY + zw[0] + rowH;
        int maxItemW = 0;
        for (int width : widths) {
            maxItemW = std::max(maxItemW, width);
        }
        plan.width = std::max(bgW, std::max(preeditW + py[2] + py[3],
                                             maxItemW + zw[2] + zw[3]));
        plan.height = std::max(bgH, textTop +
                                     (std::max<int>(1, widths.size()) - 1) * rowH +
                                     fontPixel + zw[1]);
        plan.primaryRect = {0, 0, plan.width, plan.height};
        plan.preeditRect = {py[2], py[0], py[2] + preeditW, py[0] + fontPixel};
        for (size_t i = 0; i < widths.size(); ++i) {
            RECT item = {zw[2], textTop + static_cast<int>(i) * rowH,
                         zw[2] + widths[i], textTop + static_cast<int>(i) * rowH + rowH};
            plan.candidateRects.push_back(item);
        }
        plan.arrowX = zw[2] + 20;
        plan.arrowY = plan.height - 8;
        plan.arrowsHorizontal = false;
    }
    applyTextOffsets(&plan);
    return plan;
}

void Renderer::applyTextOffsets(Plan* plan) const {
    if (!plan || !theme_) {
        return;
    }
    const SkinLayout& layout = theme_->layout();
    if (layout.preeditX != 0 || layout.preeditY != 0) {
        plan->preeditRect.left += layout.preeditX;
        plan->preeditRect.right += layout.preeditX;
        plan->preeditRect.top += layout.preeditY;
        plan->preeditRect.bottom += layout.preeditY;
    }
    if (layout.candidateX != 0 || layout.candidateY != 0) {
        for (RECT& rect : plan->candidateRects) {
            rect.left += layout.candidateX;
            rect.right += layout.candidateX;
            rect.top += layout.candidateY;
            rect.bottom += layout.candidateY;
        }
    }
}

LayoutResult Renderer::measure(const RenderState& state, HDC hdc) const {
    LayoutResult result;
    const Plan plan = buildPlan(hdc, state);
    result.width = scaleValue(plan.width);
    result.height = scaleValue(plan.height);
    result.scale = scale_;
    result.preeditRect = scaleRect(plan.preeditRect);
    result.anchor = {scaleValue(plan.anchor.x), scaleValue(plan.anchor.y)};
    result.candidateRects.reserve(plan.candidateRects.size());
    for (const RECT& rect : plan.candidateRects) {
        result.candidateRects.push_back(scaleRect(rect));
    }
    return result;
}

void Renderer::paintPlan(HDC hdc, const RECT& client, const RenderState& state,
                         const Plan& plan) const {
    if (!plan.scheme || !hdc) {
        return;
    }
    Gdiplus::Graphics graphics(hdc);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    Gdiplus::SolidBrush background(
        Gdiplus::Color(255, GetRValue(state.backgroundColor),
                       GetGValue(state.backgroundColor), GetBValue(state.backgroundColor)));
    graphics.FillRectangle(&background, 0, 0, client.right - client.left,
                           client.bottom - client.top);
    // The plan is expressed in the skin's base-font coordinate system.
    // Scaling the graphics context scales background slices, text, separator
    // and arrows together; 9-slice source rectangles stay in base pixels.
    graphics.ScaleTransform(static_cast<Gdiplus::REAL>(scale_),
                            static_cast<Gdiplus::REAL>(scale_));
    paintPlanGdiplus(graphics, state, plan);
}

void Renderer::paint(HDC hdc, const RECT& client, const RenderState& state) const {
    const Plan plan = buildPlan(hdc, state);
    paintPlan(hdc, client, state, plan);
}

void Renderer::paintBackground(Gdiplus::Graphics& graphics, const Plan& plan) const {
    if (!plan.scheme) {
        return;
    }
    const unsigned long long backgroundStart = qpcMicros();
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
    if (plan.split) {
        drawComponent(graphics, plan.scheme->pinyin, plan.primaryRect);
        drawComponent(graphics, plan.scheme->zhongwen, plan.secondaryRect);
    } else {
        drawComponent(graphics, plan.scheme->single, plan.primaryRect);
        if (plan.hasSeparator) {
            const int x1 = plan.scheme->separatorLeft;
            const int x2 = plan.width - plan.scheme->separatorRight;
            Gdiplus::Pen pen(
                Gdiplus::Color(255, GetRValue(plan.scheme->separatorColor),
                               GetGValue(plan.scheme->separatorColor),
                               GetBValue(plan.scheme->separatorColor)),
                1.0f);
            graphics.DrawLine(&pen, x1, plan.sepY, x2, plan.sepY);
        }
    }
    lastRenderTiming_.backgroundUs += qpcMicros() - backgroundStart;
}

void Renderer::paintForeground(Gdiplus::Graphics& graphics, const RenderState& state,
                               const Plan& plan) const {
    if (!plan.scheme) {
        return;
    }
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    const auto& render = theme_->render();
    const unsigned long long textStart = qpcMicros();
    if (!state.preedit.empty()) {
        drawMixedTextGdiplus(
            graphics, plan.preeditRect.left,
            plan.preeditRect.top + render.textOffsetY,
            displayPreedit(state.preedit, render.pinyinSeparator),
            theme_->display().pinyinColor, render.pinyinBold);
    }

    for (size_t i = 0; i < state.candidates.size() && i < plan.candidateRects.size(); ++i) {
        const RECT& rect = plan.candidateRects[i];
        const std::wstring key = selKeyFor(i, state.selKeys);
        const COLORREF color =
            (state.useCursor && static_cast<int>(i) == state.currentSel)
                ? theme_->display().firstColor
                : theme_->display().textColor;
        const int textY = rect.top + render.textOffsetY;
        const int keyWidth = measureTextGdiplus(graphics, key);
        drawMixedTextGdiplus(graphics, rect.left, textY, key, color, false);
        drawMixedTextGdiplus(graphics, rect.left + keyWidth + render.keyGap, textY,
                             state.candidates[i], color, false);
    }
    lastRenderTiming_.textUs = qpcMicros() - textStart;

    const unsigned long long arrowStart = qpcMicros();
    if (!plan.candidateRects.empty()) {
        const int offset =
            (!plan.split && plan.arrowsHorizontal) ? render.textOffsetY : 0;
        const int centerY = plan.arrowY + offset;
        const int right = plan.arrowX;
        const int leftX = right - 28;
        const int rightX = right - 12;
        Gdiplus::SolidBrush arrowBrush(Gdiplus::Color(255, 96, 96, 96));
        Gdiplus::Point leftPoints[3] = {
            Gdiplus::Point(leftX + 5, centerY - 5),
            Gdiplus::Point(leftX + 5, centerY + 5),
            Gdiplus::Point(leftX, centerY)};
        Gdiplus::Point rightPoints[3] = {
            Gdiplus::Point(rightX, centerY - 5),
            Gdiplus::Point(rightX, centerY + 5),
            Gdiplus::Point(rightX + 5, centerY)};
        graphics.FillPolygon(&arrowBrush, leftPoints, 3);
        graphics.FillPolygon(&arrowBrush, rightPoints, 3);
    }
    lastRenderTiming_.arrowUs = qpcMicros() - arrowStart;
}

void Renderer::paintPlanGdiplus(Gdiplus::Graphics& graphics, const RenderState& state,
                                const Plan& plan) const {
    paintBackground(graphics, plan);
    paintForeground(graphics, state, plan);
}

std::shared_ptr<Gdiplus::Bitmap> Renderer::backgroundBitmap(const Plan& plan,
                                                           int targetWidth,
                                                           int targetHeight) const {
    if (!plan.scheme || targetWidth <= 0 || targetHeight <= 0) {
        return nullptr;
    }
    std::wstring key = plan.scheme->name;
    key += L"|";
    key += std::to_wstring(targetWidth);
    key += L"x";
    key += std::to_wstring(targetHeight);
    key += L"|s";
    key += std::to_wstring(static_cast<long long>(std::lround(scale_ * 1000.0)));
    const auto cached = backgroundCache_.find(key);
    if (cached != backgroundCache_.end()) {
        return cached->second;
    }
    // A skin background is static for a given scheme and size.  Composing the
    // 9-slice slices on every keystroke costs several milliseconds; caching the
    // composed pixels makes subsequent renders a single 1:1 blit.
    if (backgroundCache_.size() >= 16) {
        backgroundCache_.clear();
    }
    auto bitmap = std::make_shared<Gdiplus::Bitmap>(
        targetWidth, targetHeight, PixelFormat32bppPARGB);
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
        return nullptr;
    }
    Gdiplus::Graphics graphics(bitmap.get());
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
    graphics.ScaleTransform(static_cast<Gdiplus::REAL>(scale_),
                            static_cast<Gdiplus::REAL>(scale_));
    paintBackground(graphics, plan);
    backgroundCache_[key] = bitmap;
    return bitmap;
}

std::shared_ptr<Gdiplus::Bitmap> Renderer::renderToBitmap(const RenderState& state,
                                                         int* outWidth,
                                                         int* outHeight) const {
    const unsigned long long totalStart = qpcMicros();
    HDC screen = ::GetDC(nullptr);
    const Plan plan = buildPlan(screen, state);
    if (screen) {
        ::ReleaseDC(nullptr, screen);
    }
    lastRenderTiming_ = {};
    lastRenderTiming_.buildUs = qpcMicros() - totalStart;
    if (plan.width <= 0 || plan.height <= 0) {
        return nullptr;
    }
    const int targetWidth = scaleValue(plan.width);
    const int targetHeight = scaleValue(plan.height);
    if (targetWidth <= 0 || targetHeight <= 0) {
        return nullptr;
    }
    const unsigned long long bitmapStart = qpcMicros();
    auto bitmap = std::make_shared<Gdiplus::Bitmap>(
        targetWidth, targetHeight, PixelFormat32bppPARGB);
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
        return nullptr;
    }
    Gdiplus::Graphics graphics(bitmap.get());
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
    lastRenderTiming_.bitmapUs = qpcMicros() - bitmapStart;

    const unsigned long long paintStart = qpcMicros();
    auto background = backgroundBitmap(plan, targetWidth, targetHeight);
    if (background) {
        const unsigned long long drawStart = qpcMicros();
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.DrawImage(background.get(), 0, 0, targetWidth, targetHeight);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        ++lastRenderTiming_.imageDraws;
        lastRenderTiming_.backgroundUs += qpcMicros() - drawStart;
    }
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
    graphics.ScaleTransform(static_cast<Gdiplus::REAL>(scale_),
                            static_cast<Gdiplus::REAL>(scale_));
    paintForeground(graphics, state, plan);
    lastRenderTiming_.paintUs = qpcMicros() - paintStart;
    lastRenderTiming_.totalUs = qpcMicros() - totalStart;
    if (outWidth) {
        *outWidth = targetWidth;
    }
    if (outHeight) {
        *outHeight = targetHeight;
    }
    return bitmap;
}

bool UpdateLayeredWindowBitmap(HWND hwnd, Gdiplus::Bitmap* bitmap) {
    if (!hwnd || !bitmap) {
        return false;
    }
    const int width = static_cast<int>(bitmap->GetWidth());
    const int height = static_cast<int>(bitmap->GetHeight());
    if (width <= 0 || height <= 0) {
        return false;
    }
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = ::CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        return false;
    }
    Gdiplus::BitmapData data = {};
    Gdiplus::Rect rect(0, 0, width, height);
    bool ok = bitmap->LockBits(&rect, Gdiplus::ImageLockModeRead,
                               PixelFormat32bppPARGB, &data) == Gdiplus::Ok;
    if (ok) {
        const int rowBytes = width * 4;
        for (int y = 0; y < height; ++y) {
            std::memcpy(static_cast<unsigned char*>(bits) + y * rowBytes,
                        static_cast<unsigned char*>(data.Scan0) + y * data.Stride,
                        rowBytes);
        }
        bitmap->UnlockBits(&data);
    }
    if (!ok) {
        ::DeleteObject(dib);
        return false;
    }
    HDC screen = ::GetDC(nullptr);
    HDC memdc = ::CreateCompatibleDC(screen);
    HGDIOBJ old = ::SelectObject(memdc, dib);
    RECT wr = {};
    ::GetWindowRect(hwnd, &wr);
    POINT dst = {wr.left, wr.top};
    SIZE size = {width, height};
    POINT src = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const BOOL result = ::UpdateLayeredWindow(hwnd, screen, &dst, &size, memdc, &src, 0,
                                               &blend, ULW_ALPHA);
    ::SelectObject(memdc, old);
    ::DeleteDC(memdc);
    if (screen) {
        ::ReleaseDC(nullptr, screen);
    }
    ::DeleteObject(dib);
    return result != FALSE;
}

void Renderer::applyWindowRegion(HWND hwnd, const RECT& client) const {
    if (!hwnd) {
        return;
    }
    HRGN region = ::CreateRectRgn(client.left, client.top, client.right, client.bottom);
    ::SetWindowRgn(hwnd, region, TRUE);
}

} // namespace SogouSkin
} // namespace Moqi
