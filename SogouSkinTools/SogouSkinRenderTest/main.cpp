// Headless render test: loads a Sogou skin and writes a PNG preview.
//
// Usage:
//   SogouSkinRenderTest <skin-dir-or-ssf> <H1|V1|H2|V2> <out.png>
//                       [preedit] [candidate|candidate|...]
//                       [key-gap] [item-spacing] [text-offset-y]
//                       [pinyin-bold 0|1] [cand-per-row] [white|transparent]
//                       [--dpi N]
//
// `--dpi` defaults to 0, which derives the effective DPI from the screen DC
// (96 in a DPI-unaware process).  Pass 120/144/168/192 to test the Windows
// 125/150/175/200% scale factors without changing the desktop.  It is a flag
// rather than a positional argument so it does not switch on the optional
// [Render] override profile used by the reference-render commands.
//
// It deliberately creates no visible window, so it is safe to run in the
// background while iterating.  Optional arguments let the audit harness feed
// the exact same sample text/candidates and [Render] settings as the Python
// reference renderer; metrics are printed as key=value lines.

#include "SogouSkin/SogouSkin.h"

#include <windows.h>
#include <gdiplus.h>

#include <cmath>
#include <codecvt>
#include <iostream>
#include <locale>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace {

int GetEncoderClsid(const WCHAR* format, CLSID* clsid) {
    UINT num = 0;
    UINT size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) {
        return -1;
    }
    std::vector<BYTE> buffer(size);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Gdiplus::GetImageEncoders(num, size, encoders);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(encoders[i].MimeType, format) == 0) {
            *clsid = encoders[i].Clsid;
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::wstring Arg(int argc, wchar_t** argv, int index, const wchar_t* fallback = L"") {
    return index < argc ? argv[index] : fallback;
}

int IntArg(int argc, wchar_t** argv, int index, int fallback) {
    if (index >= argc) {
        return fallback;
    }
    wchar_t* end = nullptr;
    const long value = std::wcstol(argv[index], &end, 0);
    return end && *end == L'\0' ? static_cast<int>(value) : fallback;
}

bool Contains(const std::wstring& text, const wchar_t* part) {
    return text.find(part) != std::wstring::npos;
}

std::vector<std::wstring> Split(const std::wstring& text, wchar_t sep) {
    std::vector<std::wstring> result;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t pos = text.find(sep, start);
        result.push_back(text.substr(
            start, pos == std::wstring::npos ? std::wstring::npos : pos - start));
        if (pos == std::wstring::npos) {
            break;
        }
        start = pos + 1;
    }
    return result;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int count = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(count > 0 ? count : 0, L'\0');
    if (count > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              result.data(), count);
    }
    return result;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int count = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0,
                                            nullptr, nullptr);
    std::string result(count > 0 ? count : 0, '\0');
    if (count > 0) {
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              result.data(), count, nullptr, nullptr);
    }
    return result;
}

void WriteFileUtf8(const std::wstring& path, const std::wstring& text) {
    std::string bytes = "\xEF\xBB\xBF" + WideToUtf8(text);
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    ::CloseHandle(file);
}

std::wstring MakeTempAppData() {
    wchar_t temp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, temp);
    std::wstring dir = std::wstring(temp) + L"SogouSkinRenderTest_" +
                       std::to_wstring(::GetCurrentProcessId());
    ::CreateDirectoryW(dir.c_str(), nullptr);
    ::CreateDirectoryW((dir + L"\\Moqi").c_str(), nullptr);
    return dir;
}

std::wstring FindUserFont(const wchar_t* file) {
    std::wcout << L"font.lookup.file.len=" << (file ? static_cast<int>(wcslen(file)) : -1)
               << L" first=" << (file && file[0] ? static_cast<int>(file[0]) : 0) << L"\n";
    wchar_t localAppData[32768] = {};
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, _countof(localAppData))) {
        const std::wstring path =
            std::wstring(localAppData) + L"\\Microsoft\\Windows\\Fonts\\" + file;
        if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return path;
        }
    }
    const std::wstring windows = std::wstring(L"C:\\Windows\\Fonts\\") + file;
    if (::GetFileAttributesW(windows.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return windows;
    }
    return {};
}

void MeasureWithFamily(Gdiplus::Graphics& graphics, const Gdiplus::FontFamily* family,
                       const wchar_t* label, const std::wstring& sample) {
    if (!family) {
        return;
    }
    wchar_t name[LF_FACESIZE] = {};
    family->GetFamilyName(name, LANG_NEUTRAL);
    Gdiplus::Font font(family, 24.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::StringFormat format;
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap |
                          Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    Gdiplus::RectF plain;
    graphics.MeasureString(sample.c_str(), static_cast<INT>(sample.size()), &font,
                           Gdiplus::PointF(0, 0), &format, &plain);
    Gdiplus::StringFormat typo = Gdiplus::StringFormat::GenericTypographic();
    typo.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap |
                        Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    Gdiplus::RectF typographic;
    graphics.MeasureString(sample.c_str(), static_cast<INT>(sample.size()), &font,
                           Gdiplus::PointF(0, 0), &typo, &typographic);
    std::wcout << L"measure." << label
               << L" status=" << static_cast<int>(font.GetLastStatus())
               << L" height=" << font.GetHeight(&graphics)
               << L" plain=" << plain.Width << L" typo=" << typographic.Width << L"\n";
}

void PrintFontDebug() {
    Gdiplus::FontFamily direct(L"荆南麦圆体");
    std::wcout << L"font.direct.available=" << (direct.IsAvailable() ? 1 : 0) << L"\n";
    const std::wstring fontPath = FindUserFont(L"荆南麦圆体.otf");
    std::wcout << L"font.path.present=" << (fontPath.empty() ? 0 : 1) << L"\n";
    Gdiplus::PrivateFontCollection collection;
    const Gdiplus::Status added =
        fontPath.empty() ? Gdiplus::FileNotFound
                         : collection.AddFontFile(fontPath.c_str());
    std::wcout << L"font.private.add=" << static_cast<int>(added) << L"\n";
    const int count = collection.GetFamilyCount();
    std::wcout << L"font.private.count=" << count << L"\n";
    std::vector<Gdiplus::FontFamily> families(static_cast<size_t>(std::max(0, count)));
    int found = 0;
    if (count > 0) {
        collection.GetFamilies(count, families.data(), &found);
    }
    std::wcout << L"font.private.found=" << found << L"\n";

    Gdiplus::Bitmap scratch(1, 1, PixelFormat32bppARGB);
    Gdiplus::Graphics graphics(&scratch);
    const std::wstring sample = L"shì shì shāng shì shì shì shāng";
    MeasureWithFamily(graphics, &direct, L"direct", sample);
    for (int i = 0; i < found; ++i) {
        const std::wstring label = L"private" + std::to_wstring(i);
        MeasureWithFamily(graphics, &families[static_cast<size_t>(i)], label.c_str(),
                          sample);
    }
    MeasureWithFamily(graphics, Gdiplus::FontFamily::GenericSansSerif(), L"sans",
                      sample);

    if (found > 0) {
        Gdiplus::Font font(&families[0], 24.0f, Gdiplus::FontStyleRegular,
                           Gdiplus::UnitPixel);
        Gdiplus::StringFormat typo = Gdiplus::StringFormat::GenericTypographic();
        typo.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap |
                            Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
        const wchar_t* samples[] = {L"1.", L"2.", L"3.", L"4.", L"5.", L"6.",
                                    L"你好", L"(*´▽`*)ノ"};
        for (const wchar_t* text : samples) {
            Gdiplus::RectF bounds;
            graphics.MeasureString(text, -1, &font, Gdiplus::PointF(0, 0), &typo,
                                   &bounds);
            std::wcout << L"sample.width=" << bounds.Width << L"\n";
        }
        const std::wstring chars = L"shìā你1.2.3.(*´▽`*)ノ";
        for (wchar_t ch : chars) {
            const std::wstring one(1, ch);
            Gdiplus::RectF bounds;
            graphics.MeasureString(one.c_str(), 1, &font, Gdiplus::PointF(0, 0),
                                   &typo, &bounds);
            std::wcout << L"char.width=" << static_cast<int>(ch) << L":"
                       << bounds.Width << L"\n";
        }
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wcout.imbue(
        std::locale(std::locale::classic(), new std::codecvt_utf8<wchar_t>));
    // Separate --dpi from the positional arguments so the historical argument
    // order and the [Render] override path stay byte-identical at 100%.
    std::vector<wchar_t*> positional;
    positional.push_back(argv[0]);
    int dpiArg = 0;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--dpi") == 0 && i + 1 < argc) {
            dpiArg = IntArg(argc, argv, i + 1, 0);
            ++i;
        } else {
            positional.push_back(argv[i]);
        }
    }
    argc = static_cast<int>(positional.size());
    argv = positional.data();
    if (argc < 4) {
        std::wcerr << L"usage: SogouSkinRenderTest <skin-dir-or-ssf> <H1|V1|H2|V2> <out.png>\n"
                      L"       [preedit] [candidate|candidate|...] [key-gap] [item-spacing]\n"
                      L"       [text-offset-y] [pinyin-bold] [cand-per-row] [white|transparent]\n"
                      L"       [--dpi 120|144|168|192]\n";
        return 2;
    }
    const std::wstring skinPath = argv[1];
    const std::wstring schemeName = argv[2];
    const std::wstring outPath = argv[3];
    const std::wstring preedit = Arg(argc, argv, 4, L"搜狗pinyin");
    const std::wstring candidateArg = Arg(argc, argv, 5);
    const int keyGap = IntArg(argc, argv, 6, 1);
    const int itemSpacing = IntArg(argc, argv, 7, 11);
    const int textOffsetY = IntArg(argc, argv, 8, 0);
    const int pinyinBold = IntArg(argc, argv, 9, 1);
    const int candPerRowArg = IntArg(argc, argv, 10, -1);
    const std::wstring background = Arg(argc, argv, 11, L"white");

    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        std::wcerr << L"GDI+ startup failed\n";
        return 1;
    }
    PrintFontDebug();

    // Optional [Render] overrides for the headless test: point APPDATA at a
    // throw-away profile so the normal Theme/overrides path is exercised.
    wchar_t oldAppData[32768] = {};
    const bool hadAppData =
        ::GetEnvironmentVariableW(L"APPDATA", oldAppData, _countof(oldAppData)) > 0;
    std::wstring tempAppData;
    if (argc > 6) {
        tempAppData = MakeTempAppData();
        std::wstring ini = L"[Render]\n";
        ini += L"key_gap=" + std::to_wstring(keyGap) + L"\n";
        ini += L"item_spacing=" + std::to_wstring(itemSpacing) + L"\n";
        ini += L"text_offset_y=" + std::to_wstring(textOffsetY) + L"\n";
        ini += L"pinyin_bold=" + std::to_wstring(pinyinBold) + L"\n";
        WriteFileUtf8(tempAppData + L"\\Moqi\\sogou_skin_overrides.ini", ini);
        _wputenv_s(L"APPDATA", tempAppData.c_str());
    }

    std::wstring error;
    auto theme = Moqi::SogouSkin::Theme::Load(skinPath, &error);
    if (!theme) {
        std::wcerr << L"load failed: " << error << L"\n";
        Gdiplus::GdiplusShutdown(token);
        return 1;
    }
    std::wcout << L"font.base=" << theme->baseFontSize()
               << L" font.effective=" << theme->display().fontSize
               << L" render.item_spacing=" << theme->render().itemSpacing << L"\n";
    std::wcout << L"overrides.path=" << Moqi::SogouSkin::OverridesPath() << L"\n";

    if (schemeName == L"StatusBar" || schemeName == L"STATUS") {
        const auto& barInfo = theme->statusBar();
        auto bar = theme->image(barInfo.pic);
        if (!bar) {
            std::wcerr << L"statusbar image missing\n";
            Gdiplus::GdiplusShutdown(token);
            return 1;
        }
        const int baseWidth = static_cast<int>(bar->GetWidth());
        const int baseHeight = static_cast<int>(bar->GetHeight());
        int statusDpi = dpiArg;
        if (statusDpi <= 0) {
            HDC screen = ::GetDC(nullptr);
            statusDpi = screen ? ::GetDeviceCaps(screen, LOGPIXELSX) : 96;
            if (screen) {
                ::ReleaseDC(nullptr, screen);
            }
        }
        if (statusDpi <= 0) {
            statusDpi = 96;
        }
        const int width = ::MulDiv(baseWidth, statusDpi, 96);
        const int height = ::MulDiv(baseHeight, statusDpi, 96);
        Gdiplus::Bitmap target(width, height, PixelFormat32bppPARGB);
        Gdiplus::Graphics graphics(&target);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.DrawImage(bar.get(), 0, 0, width, height);
        const auto options = Moqi::SogouSkin::LoadOptions();
        int drawn = 0;
        for (const auto& item : barInfo.items) {
            bool hidden = false;
            for (const auto& name : options.hiddenStatusItems) {
                if (_wcsicmp(name.c_str(), item.name.c_str()) == 0) {
                    hidden = true;
                }
            }
            if (!item.display || hidden || item.normal.empty()) {
                continue;
            }
            auto image = theme->image(item.normal[0]);
            if (!image) {
                continue;
            }
            graphics.DrawImage(image.get(), ::MulDiv(item.pos.x, statusDpi, 96),
                               ::MulDiv(item.pos.y, statusDpi, 96),
                               ::MulDiv(static_cast<int>(image->GetWidth()), statusDpi, 96),
                               ::MulDiv(static_cast<int>(image->GetHeight()), statusDpi, 96));
            ++drawn;
            std::wcout << L"statusbar.item=" << item.name << L" pos=" << item.pos.x
                       << L"," << item.pos.y << L" size=" << image->GetWidth() << L"x"
                       << image->GetHeight() << L"\n";
        }
        std::wcout << L"statusbar.size=" << width << L"x" << height
                   << L" items=" << drawn << L"\n";
        CLSID png = {};
        if (GetEncoderClsid(L"image/png", &png) < 0 ||
            target.Save(outPath.c_str(), &png, nullptr) != Gdiplus::Ok) {
            std::wcerr << L"save failed: " << outPath << L"\n";
            Gdiplus::GdiplusShutdown(token);
            return 1;
        }
        std::wcout << L"rendered StatusBar -> " << outPath << L"\n";
        Gdiplus::GdiplusShutdown(token);
        return 0;
    }

    Moqi::SogouSkin::RenderState state;
    state.preedit = preedit;
    if (candidateArg.empty()) {
        state.candidates = {L"拼音", L"品", L"频", L"拼", L"货",
                            L"聘", L"嫔", L"姘", L"颦"};
    } else {
        state.candidates = Split(candidateArg, L'|');
    }
    state.selKeys = L"1234567890";
    state.currentSel = 0;
    state.candPerRow = candPerRowArg > 0
                           ? candPerRowArg
                           : (Contains(schemeName, L"H") ? 3 : 1);

    Moqi::SogouSkin::Renderer renderer(theme, dpiArg);
    HDC screen = ::GetDC(nullptr);
    const auto layout = renderer.measure(state, screen);
    ::ReleaseDC(nullptr, screen);
    if (layout.width <= 0 || layout.height <= 0) {
        std::wcerr << L"layout failed (no usable scheme?)\n";
        Gdiplus::GdiplusShutdown(token);
        return 1;
    }

    std::wcout << L"scheme=" << schemeName << L" size=" << layout.width << L"x"
               << layout.height << L" scale=" << layout.scale << L"\n";
    std::wcout << L"preedit.rect=" << layout.preeditRect.left << L","
               << layout.preeditRect.top << L"," << layout.preeditRect.right << L","
               << layout.preeditRect.bottom << L"\n";
    std::wcout << L"anchor=" << layout.anchor.x << L"," << layout.anchor.y << L"\n";
    for (size_t i = 0; i < layout.candidateRects.size(); ++i) {
        const RECT& rect = layout.candidateRects[i];
        std::wcout << L"candidate[" << i << L"].rect=" << rect.left << L","
                   << rect.top << L"," << rect.right << L"," << rect.bottom << L"\n";
    }

    std::shared_ptr<Gdiplus::Bitmap> target;
    if (background == L"transparent") {
        target = renderer.renderToBitmap(state, nullptr, nullptr);
        if (!target) {
            std::wcerr << L"bitmap render failed\n";
            Gdiplus::GdiplusShutdown(token);
            return 1;
        }
    } else {
        target = std::make_shared<Gdiplus::Bitmap>(layout.width, layout.height,
                                                  PixelFormat32bppARGB);
        Gdiplus::Graphics graphics(target.get());
        graphics.Clear(Gdiplus::Color(255, 255, 255, 255));
        HDC hdc = graphics.GetHDC();
        RECT rc = {0, 0, layout.width, layout.height};
        renderer.paint(hdc, rc, state);
        graphics.ReleaseHDC(hdc);
    }

    CLSID png = {};
    if (GetEncoderClsid(L"image/png", &png) < 0 ||
        target->Save(outPath.c_str(), &png, nullptr) != Gdiplus::Ok) {
        std::wcerr << L"save failed: " << outPath << L"\n";
        Gdiplus::GdiplusShutdown(token);
        return 1;
    }
    std::wcout << L"rendered " << schemeName << L" -> " << outPath
               << L" (" << layout.width << L"x" << layout.height << L")\n";

    if (!tempAppData.empty()) {
        _wputenv_s(L"APPDATA", hadAppData ? oldAppData : L"");
        ::DeleteFileW((tempAppData + L"\\Moqi\\sogou_skin_overrides.ini").c_str());
        ::RemoveDirectoryW((tempAppData + L"\\Moqi").c_str());
        ::RemoveDirectoryW(tempAppData.c_str());
    }
    Gdiplus::GdiplusShutdown(token);
    return 0;
}
