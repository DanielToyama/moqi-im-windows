#include "SogouSymbols.h"

namespace Moqi {
namespace {

const std::vector<SogouSymbolCategory> kCategories = {
    {L"常用标点",
     {L"，", L"。", L"、", L"？", L"！", L"：", L"；", L"“", L"”", L"‘",
      L"’", L"（", L"）", L"【", L"】", L"《", L"》", L"…", L"—"}},
    {L"数学符号",
     {L"＋", L"－", L"×", L"÷", L"＝", L"≠", L"≤", L"≥", L"±", L"∞"}},
    {L"货币/单位",
     {L"￥", L"＄", L"€", L"£", L"％", L"‰", L"℃", L"℉", L"°"}},
    {L"特殊符号",
     {L"©", L"®", L"™", L"←", L"→", L"↑", L"↓", L"★", L"☆", L"●", L"○",
      L"■", L"□"}},
};

} // namespace

const std::vector<SogouSymbolCategory>& SogouSymbolCategories() {
    return kCategories;
}

HMENU CreateSogouSymbolMenu() {
    HMENU root = ::CreatePopupMenu();
    if (!root) {
        return nullptr;
    }
    UINT commandId = kSogouSymbolCommandBase;
    for (const auto& category : kCategories) {
        HMENU submenu = ::CreatePopupMenu();
        if (!submenu) {
            commandId += static_cast<UINT>(category.symbols.size());
            continue;
        }
        for (const auto& symbol : category.symbols) {
            ::AppendMenuW(submenu, MF_STRING, commandId, symbol.c_str());
            ++commandId;
        }
        ::AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu),
                      category.name.c_str());
    }
    return root;
}

const std::wstring* SogouSymbolForCommand(UINT commandId) {
    if (commandId < kSogouSymbolCommandBase) {
        return nullptr;
    }
    UINT id = kSogouSymbolCommandBase;
    for (const auto& category : kCategories) {
        for (const auto& symbol : category.symbols) {
            if (id == commandId) {
                return &symbol;
            }
            ++id;
        }
    }
    return nullptr;
}

} // namespace Moqi
