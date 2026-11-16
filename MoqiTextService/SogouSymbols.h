//
// Common symbol palette shared by the status-bar context menu.
//
// The symbol table and menu builder are kept separate from TextService so the
// menu/ID mapping can be verified by a headless test even though the actual
// TSF insertion needs a live input context.
//
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace Moqi {

// Menu IDs 1..5 and 100/101/110/111 are already used by the status-bar menu.
// Keep symbol commands in a clearly separated range.
constexpr UINT kSogouSymbolCommandBase = 2000;

struct SogouSymbolCategory {
    std::wstring name;
    std::vector<std::wstring> symbols;
};

const std::vector<SogouSymbolCategory>& SogouSymbolCategories();

// Builds the "符号输入" popup with one submenu per category.  The caller owns
// the returned HMENU and must DestroyMenu() it.
HMENU CreateSogouSymbolMenu();

// Resolves a menu command id back to its character.  Returns nullptr for ids
// outside the symbol range.
const std::wstring* SogouSymbolForCommand(UINT commandId);

} // namespace Moqi
