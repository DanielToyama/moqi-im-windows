//
//	Copyright (C) 2013 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
//
//	This library is free software; you can redistribute it and/or
//	modify it under the terms of the GNU Library General Public
//	License as published by the Free Software Foundation; either
//	version 2 of the License, or (at your option) any later version.
//
//	This library is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//	Library General Public License for more details.
//
//	You should have received a copy of the GNU Library General Public
//	License along with this library; if not, write to the
//	Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
//	Boston, MA  02110-1301, USA.
//

#include "MoqiTextService.h"
#include <assert.h>
#include <string>
#include <algorithm>
#include <libIME2/src/DebugLogConfig.h>
#include <libIME2/src/DebugLogFile.h>
#include <libIME2/src/ComPtr.h>
#include <libIME2/src/Utils.h>
#include <libIME2/src/LangBarButton.h>
#include "MoqiImeModule.h"
#include "SogouSkin/SogouSkin.h"
#include "KeyRepeatAccelerator.h"
#include "SogouSymbols.h"
#include "resource.h"
#include <Shellapi.h>
#include <sys/stat.h>
#include <cwctype>
#include <fstream>
#include <sstream>

using namespace std;

namespace Moqi {

namespace {

std::wstring currentProcessPath();
std::wstring processBaseName(const std::wstring& imagePath);
std::wstring timestampNow();
std::wstring formatDebugLogLine(const std::wstring& message);
constexpr wchar_t kDefaultCommentFontFace[] = L"Consolas";
// The native candidate window previously inherited DEFAULT_GUI_FONT, which can
// resolve to SimSun on some Chinese installations.  Use the Windows UI sans
// face unless appearance_config.json sends candFontName explicitly.
constexpr wchar_t kDefaultCandidateFontFace[] = L"Microsoft YaHei UI";
constexpr ULONGLONG kCandidateWindowMoveThrottleMs = 50;

// Command IDs of the moqi-ime Rime backend.  They are stable public constants
// in input_methods/rime/rime.go and let the Sogou status bar drive the same
// state as the native language-bar buttons.
constexpr UINT kMoqiCommandAsciiMode = 2;
constexpr UINT kMoqiCommandFullShape = 3;
constexpr UINT kMoqiCommandAsciiPunct = 4;
constexpr UINT kMoqiCommandTraditionalization = 5;

const wchar_t* BackendButtonIdForSogouItem(const std::wstring& itemName) {
	if (itemName == L"cn_en") {
		return L"switch-lang";
	}
	if (itemName == L"quan_ban") {
		return L"switch-shape";
	}
	return nullptr;
}

void SaveSogouOptions(const SogouSkin::Options& options) {
	const std::wstring path = SogouSkin::OptionsPath();
	if (path.empty()) {
		return;
	}
	const std::wstring dir = SogouSkin::ConfigDirectory();
	if (!dir.empty()) {
		::CreateDirectoryW(dir.c_str(), nullptr);
	}
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file) {
		return;
	}
	std::string hidden;
	for (const auto& item : options.hiddenStatusItems) {
		if (!hidden.empty()) {
			hidden += ",";
		}
		for (wchar_t ch : item) {
			hidden += static_cast<char>(ch < 128 ? ch : '?');
		}
	}
	file << "[Options]\n";
	file << "statusbar=" << (options.statusbar ? 1 : 0) << "\n";
	file << "horizontal=" << (options.horizontal ? 1 : 0) << "\n";
	file << "key_repeat=" << (options.keyRepeat ? 1 : 0) << "\n";
	file << "statusbar_fullscreen_hide="
	     << (options.statusbarFullscreenHide ? 1 : 0) << "\n";
	file << "statusbar_fullscreen_scope="
	     << (options.statusbarFullscreenScope == SogouSkin::FullscreenScope::All
	             ? "all"
	             : "monitor")
	     << "\n";
	file << "statusbar_hide=" << hidden << "\n";
	if (options.statusbarX >= 0) {
		file << "statusbar_x=" << options.statusbarX << "\n";
	}
	if (options.statusbarY >= 0) {
		file << "statusbar_y=" << options.statusbarY << "\n";
	}
}

bool callClientFilterKeyDown(Client* client, Ime::KeyEvent& keyEvent, bool& sehCaught) {
	sehCaught = false;
	__try {
		return client->filterKeyDown(keyEvent);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		sehCaught = true;
		return false;
	}
}

bool callClientOnKeyDown(Client* client, Ime::KeyEvent& keyEvent, Ime::EditSession* session, bool& sehCaught) {
	sehCaught = false;
	__try {
		return client->onKeyDown(keyEvent, session);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		sehCaught = true;
		return false;
	}
}

bool callClientFilterKeyUp(Client* client, Ime::KeyEvent& keyEvent, bool& sehCaught) {
	sehCaught = false;
	__try {
		return client->filterKeyUp(keyEvent);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		sehCaught = true;
		return false;
	}
}

bool callClientOnKeyUp(Client* client, Ime::KeyEvent& keyEvent, Ime::EditSession* session, bool& sehCaught) {
	sehCaught = false;
	__try {
		return client->onKeyUp(keyEvent, session);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		sehCaught = true;
		return false;
	}
}

bool callClientOnPreservedKey(Client* client, const GUID& guid, Ime::EditSession* session, bool& sehCaught) {
	sehCaught = false;
	__try {
		return client->onPreservedKey(guid, session);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		sehCaught = true;
		return false;
	}
}

void appendCandidateWindowLog(const std::wstring& message) {
	if (!Ime::isTraceLoggingEnabled()) {
		return;
	}

	const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
	if (!localAppData || !*localAppData) {
		return;
	}

	std::wstring logDir = std::wstring(localAppData) + L"\\MoqiIM\\Log";
	std::wstring logPath = Ime::DebugLogFile::prepareDailyLogFilePath(
		logDir, L"candidate-window.log");
	if (logPath.empty()) {
		return;
	}

	std::wofstream stream(logPath, std::ios::app);
	if (!stream.is_open()) {
		return;
	}
	stream << formatDebugLogLine(message) << L"\n";
}

void appendTsfDebugLog(const std::wstring& message) {
	if (!Ime::isTraceLoggingEnabled()) {
		return;
	}

	const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
	if (!localAppData || !*localAppData) {
		return;
	}

	std::wstring logDir = std::wstring(localAppData) + L"\\MoqiIM\\Log";
	std::wstring logPath = Ime::DebugLogFile::prepareDailyLogFilePath(
		logDir, L"tsf-debug.log");
	if (logPath.empty()) {
		return;
	}

	std::wofstream stream(logPath, std::ios::app);
	if (!stream.is_open()) {
		return;
	}
	stream << message << L"\n";
}

std::wstring toLowerCopy(std::wstring value) {
	std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
		return static_cast<wchar_t>(std::towlower(ch));
	});
	return value;
}

std::wstring currentProcessPath() {
	std::wstring buffer(MAX_PATH, L'\0');
	DWORD len = ::GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
	if (len == 0) {
		return L"";
	}
	while (len >= buffer.size() - 1) {
		buffer.resize(buffer.size() * 2);
		len = ::GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
		if (len == 0) {
			return L"";
		}
	}
	buffer.resize(len);
	return buffer;
}

std::wstring processBaseName(const std::wstring& imagePath) {
	const size_t pos = imagePath.find_last_of(L"\\/");
	return pos == std::wstring::npos ? imagePath : imagePath.substr(pos + 1);
}

std::wstring currentProcessName() {
	const std::wstring exeName = processBaseName(currentProcessPath());
	return exeName.empty() ? L"<unknown>" : exeName;
}

std::wstring processPathForPid(DWORD pid) {
	if (pid == 0) {
		return L"";
	}

	HANDLE processHandle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!processHandle) {
		return L"";
	}

	std::wstring buffer(MAX_PATH, L'\0');
	DWORD len = static_cast<DWORD>(buffer.size());
	while (!::QueryFullProcessImageNameW(processHandle, 0, &buffer[0], &len)) {
		const DWORD error = ::GetLastError();
		if (error != ERROR_INSUFFICIENT_BUFFER) {
			::CloseHandle(processHandle);
			return L"";
		}
		buffer.resize(buffer.size() * 2);
		len = static_cast<DWORD>(buffer.size());
	}

	::CloseHandle(processHandle);
	buffer.resize(len);
	return buffer;
}

std::wstring boolText(bool value) {
	return value ? L"true" : L"false";
}

std::wstring timestampNow() {
	SYSTEMTIME st{};
	::GetLocalTime(&st);
	wchar_t buffer[64] = {0};
	_snwprintf_s(buffer, _countof(buffer), _TRUNCATE,
		L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
		st.wYear, st.wMonth, st.wDay,
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	return buffer;
}

ULONGLONG qpcMicros() {
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
	return static_cast<ULONGLONG>(
		(counter.QuadPart * 1000000ULL) / frequency.QuadPart);
}

std::wstring formatDebugLogLine(const std::wstring& message) {
	std::wostringstream line;
	line << L"[" << timestampNow() << L"]"
	     << L"[pid=" << ::GetCurrentProcessId() << L"]"
	     << L"[tid=" << ::GetCurrentThreadId() << L"]"
	     << L"[exe=" << currentProcessName() << L"] "
	     << message;
	return line.str();
}

void logDebug(const std::wstring& message) {
	if (!Ime::isTraceLoggingEnabled()) {
		return;
	}
	const std::wstring formatted = formatDebugLogLine(message);
	::OutputDebugStringW((formatted + L"\n").c_str());
	appendTsfDebugLog(formatted);
}

std::wstring guidToString(REFGUID guid) {
	LPOLESTR buffer = nullptr;
	if (FAILED(::StringFromCLSID(guid, &buffer))) {
		return L"<guid-convert-failed>";
	}
	std::wstring result{buffer};
	::CoTaskMemFree(buffer);
	return result;
}

std::wstring foregroundWindowSummary() {
	HWND hwnd = ::GetForegroundWindow();
	DWORD pid = 0;
	const DWORD tid = hwnd ? ::GetWindowThreadProcessId(hwnd, &pid) : 0;
	const std::wstring fgExe = processBaseName(processPathForPid(pid));
	std::wostringstream stream;
	stream << L"fg_hwnd=0x" << std::hex << reinterpret_cast<UINT_PTR>(hwnd) << std::dec
	       << L" fg_pid=" << pid
	       << L" fg_exe=" << (fgExe.empty() ? L"<unknown>" : fgExe)
	       << L" fg_tid=" << tid;
	return stream.str();
}

std::wstring keyEventSummary(const Ime::KeyEvent& keyEvent) {
	std::wostringstream stream;
	stream << L"vk=" << keyEvent.keyCode()
	       << L" char=" << keyEvent.charCode()
	       << L" scan=" << static_cast<unsigned int>(keyEvent.scanCode())
	       << L" ext=" << boolText(keyEvent.isExtended())
	       << L" repeat=" << keyEvent.repeatCount();
	return stream.str();
}

bool isOrdinaryPrintableKey(const Ime::KeyEvent& keyEvent) {
	return keyEvent.charCode() >= 0x20 &&
	       keyEvent.charCode() != 0x7f &&
	       !keyEvent.isKeyDown(VK_CONTROL) &&
	       !keyEvent.isKeyDown(VK_MENU);
}

bool hasKeyRepeatBlocker(const Ime::KeyEvent& keyEvent) {
	// Only plain typing is accelerated.  Ctrl/Alt/Win combinations, candidate
	// selection keys and navigation keys must keep their original behaviour.
	return keyEvent.isKeyDown(VK_CONTROL) ||
	       keyEvent.isKeyDown(VK_MENU) ||
	       keyEvent.isKeyDown(VK_LWIN) ||
	       keyEvent.isKeyDown(VK_RWIN);
}

bool shouldForceUiLessForProcess(const std::wstring& imagePath) {
	const std::wstring lowerPath = toLowerCopy(imagePath);
	const std::wstring lowerBaseName = processBaseName(lowerPath);
	if (lowerBaseName == L"war3.exe" || lowerBaseName == L"minecraft.windows.exe") {
		return true;
	}
	if ((lowerBaseName == L"javaw.exe" || lowerBaseName == L"java.exe") &&
		(lowerPath.find(L"minecraft") != std::wstring::npos ||
		 lowerPath.find(L".minecraft") != std::wstring::npos ||
		 lowerPath.find(L"mojang") != std::wstring::npos)) {
		return true;
	}
	return false;
}

bool shouldEnableDummyAnchorCompatForProcess(const std::wstring& imagePath) {
	const std::wstring lowerBaseName = processBaseName(toLowerCopy(imagePath));
	return lowerBaseName == L"steamwebhelper.exe" ||
	       lowerBaseName == L"steam.exe" ||
	       lowerBaseName == L"gameoverlayui.exe" ||
	       lowerBaseName == L"cs2.exe" ||
	       lowerBaseName == L"csgo.exe";
}

bool shouldDisableInlinePreeditForProcess(const std::wstring& imagePath) {
	const std::wstring lowerBaseName = processBaseName(toLowerCopy(imagePath));
	return lowerBaseName == L"cs2.exe" || lowerBaseName == L"csgo.exe";
}

bool shouldDisableTsfCandidateUiForProcess(const std::wstring& imagePath) {
	const std::wstring lowerBaseName = processBaseName(toLowerCopy(imagePath));
	return lowerBaseName == L"cs2.exe" || lowerBaseName == L"csgo.exe";
}

}

TextService::TextService(ImeModule* module):
	Ime::TextService(module),
	client_(nullptr),
	messageWindow_(nullptr),
	messageTimerId_(0),
	validCandidateListElementId_(false),
	candidateListElementId_(0),
	shouldShowCandidateWindowUI_(true),
	manualUiLessOverride_(false),
	autoUiLessOverride_(shouldForceUiLessForProcess(currentProcessPath())),
	autoDummyAnchorCompat_(shouldEnableDummyAnchorCompatForProcess(currentProcessPath())),
	autoInlinePreeditDisabled_(shouldDisableInlinePreeditForProcess(currentProcessPath())),
	autoDisableTsfCandidateUi_(shouldDisableTsfCandidateUiForProcess(currentProcessPath())),
	candidateWindow_(nullptr),
	showingCandidates_(false),
	pendingCandidateRecovery_(false),
	hasAppliedCandidateContent_(false),
	hasAppliedCandidateCursor_(false),
	appliedCandidateCursor_(0),
	hasLastCandidateWindowPos_(false),
	lastCandidateWindowPos_{0, 0},
	lastCandidateWindowMoveTick_(0),
	updateFont_(false),
	candPerRow_(1),
	candSpacing_(20),
	selKeys_(L"1234567890"),
	candUseCursor_(true),
	candFontName_(kDefaultCandidateFontFace),
	candCommentFontName_(kDefaultCommentFontFace),
	candFontSize_(16),
	candCommentFontSize_(14),
	candBackgroundColor_(RGB(255, 255, 255)),
	candHighlightColor_(RGB(198, 221, 249)),
	candTextColor_(RGB(0, 0, 0)),
	candHighlightTextColor_(RGB(0, 0, 0)),
	candCommentColor_(RGB(0, 0, 0)),
	candCommentHighlightColor_(RGB(0, 0, 0)),
	inlinePreedit_(true),
	autoPairQuotes_(false),
	suppressNextCompositionTerminatedNotification_(false),
	candidatePreeditCursor_(0),
	currentLangProfile_(GUID_NULL) {
	shouldShowCandidateWindowUI_ = !effectiveUiLess();

	keyRepeat_ = std::make_unique<KeyRepeatAccelerator>(
		[this](WORD vk) { injectSogouKeyRepeat(vk); });
	configureKeyRepeat();

	// font for candidate and mesasge windows
	font_ = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	LOGFONT lf;
	GetObject(font_, sizeof(lf), &lf);
	lf.lfHeight = candFontHeight(); // FIXME: make this configurable
	lf.lfWeight = FW_NORMAL;
	{
		const std::wstring& candidateFontName =
			candFontName_.empty() ? std::wstring(kDefaultCandidateFontFace) : candFontName_;
		wcsncpy_s(lf.lfFaceName, _countof(lf.lfFaceName), candidateFontName.c_str(),
		          _TRUNCATE);
	}
	font_ = CreateFontIndirect(&lf);
	lf.lfHeight = candCommentFontHeight();
	const std::wstring& commentFontName =
		candCommentFontName_.empty() ? std::wstring(kDefaultCommentFontFace) : candCommentFontName_;
	wcsncpy_s(lf.lfFaceName, _countof(lf.lfFaceName), commentFontName.c_str(), _TRUNCATE);
	commentFont_ = CreateFontIndirect(&lf);
}

TextService::~TextService(void) {
	if (client_) {
		closeClient();
	}

	stopKeyRepeat();
	keyRepeat_.reset();

	if(popupMenu_)
		::DestroyMenu(popupMenu_);

	destroyCandidateWindow();

	sogouStatusBar_.reset();

	if(messageWindow_)
		hideMessage();

	if(font_)
		::DeleteObject(font_);
	if(commentFont_)
		::DeleteObject(commentFont_);
}

void TextService::ensureSogouSkinMode() {
	if (sogouSkinChecked_) {
		return;
	}
	sogouSkinChecked_ = true;
	std::wstring error;
	sogouTheme_ = SogouSkin::LoadPreferredTheme(&error);
	sogouSkinMode_ = sogouTheme_ != nullptr;
}

void TextService::ensureSogouStatusBar() {
	ensureSogouSkinMode();
	if (!sogouSkinMode_ || !sogouTheme_ || sogouTheme_->statusBar().pic.empty()) {
		return;
	}
	const auto options = SogouSkin::LoadOptions();
	if (!options.statusbar) {
		if (sogouStatusBar_) {
			sogouStatusBar_->hide();
		}
		return;
	}
	if (sogouStatusBar_) {
		return;
	}
	auto bar = std::make_unique<SogouSkin::StatusBarWindow>(
		sogouTheme_, [this](const std::wstring& name) {
			handleSogouStatusBarClick(name);
		},
		[this](const std::wstring& name) {
			return sogouStatusBarItemState(name);
		});
	bar->setContextMenuHandler([this](POINT pt) {
		showSogouStatusBarMenu(pt);
	});
	bar->setSettingsChangedHandler([this]() {
		reloadSogouSkinFromFiles();
	});
	bar->setVisibilityChangedHandler([](bool visible) {
		logDebug(std::wstring(L"[SogouStatusBar] ") +
		         (visible ? L"shown" : L"hidden") + L" " +
		         foregroundWindowSummary());
	});
	if (bar->create()) {
		sogouStatusBar_ = std::move(bar);
	}
}

void TextService::refreshSogouStatusBar() {
	if (sogouStatusBar_) {
		// This runs on every key-up.  Only a state-image change needs a
		// layered-window repaint; colouring it every keystroke made SearchHost
		// and other lightweight TSF hosts visibly sluggish.
		sogouStatusBar_->refreshIfStateChanged();
	}
}

int TextService::sogouStatusBarItemState(const std::wstring& itemName) const {
	if (itemName == L"softkeyboard") {
		return isKeyboardOpened() ? 1 : 0;
	}
	if (!client_) {
		const auto it = sogouStatusBarLocalStates_.find(itemName);
		const int state = it == sogouStatusBarLocalStates_.end() ? 0 : it->second;
		// The skin's fan_jian images are ordered 简体, 繁体, while the
		// local/toggled state is the opposite visual order.
		return itemName == L"fan_jian" ? (state ? 0 : 1) : state;
	}
	LangBarButton* button = nullptr;
	if (const wchar_t* backendId = BackendButtonIdForSogouItem(itemName)) {
		button = client_->findButton(utf16ToUtf8(backendId));
	}
	if (!button && itemName == L"cn_en") {
		// Windows 8+/11 mode icon is the fallback when the backend chooses not
		// to create the separate switch-lang button.
		button = client_->findButton("windows-mode-icon");
	}
	if (!button) {
		const auto it = sogouStatusBarLocalStates_.find(itemName);
		const int state = it == sogouStatusBarLocalStates_.end() ? 0 : it->second;
		return itemName == L"fan_jian" ? (state ? 0 : 1) : state;
	}

	const std::wstring icon = toLowerCopy(button->iconFile());
	if (itemName == L"cn_en") {
		// moqi-ime publishes chi.ico / eng.ico through switch-lang, and
		// chi_*.ico / eng_*.ico through windows-mode-icon.
		if (icon.find(L"eng") != std::wstring::npos) {
			return 1;
		}
		if (icon.find(L"chi") != std::wstring::npos) {
			return 0;
		}
		const wchar_t* text = button->text();
		if (text) {
			const std::wstring value(text);
			if (value.find(L"A") != std::wstring::npos) {
				return 2;
			}
			if (value.find(L"英") != std::wstring::npos) {
				return 1;
			}
			if (value.find(L"中") != std::wstring::npos) {
				return 0;
			}
		}
		return button->toggled() ? 1 : 0;
	}
	if (itemName == L"quan_ban") {
		// switch-shape publishes full.ico / half.ico.
		if (icon.find(L"full") != std::wstring::npos) {
			return 0;
		}
		if (icon.find(L"half") != std::wstring::npos) {
			return 1;
		}
		return button->toggled() ? 1 : 0;
	}
	if (itemName == L"fan_jian") {
		return button->toggled() ? 0 : 1;
	}
	return button->toggled() ? 1 : 0;
}

void TextService::handleSogouStatusBarClick(const std::wstring& itemName) {
	if (itemName == L"menu") {
		openSogouSkinSettings();
		return;
	}
	if (itemName == L"softkeyboard") {
		::ShellExecuteW(
			nullptr, L"open",
			L"C:\\Program Files\\Common Files\\Microsoft Shared\\ink\\TabTip.exe",
			nullptr, nullptr, SW_SHOWNORMAL);
		return;
	}
	// These skin buttons do not have a stable moqi-ime command yet.  Do not
	// synthesize app shortcuts (Ctrl+Shift+S etc.), which can trigger unrelated
	// application commands such as Save As.
	if (itemName == L"quan_shuang" || itemName == L"sogousearch") {
		return;
	}
	if (client_) {
		UINT commandId = 0;
		if (itemName == L"cn_en") {
			commandId = kMoqiCommandAsciiMode;
		} else if (itemName == L"quan_ban") {
			commandId = kMoqiCommandFullShape;
		} else if (itemName == L"biaodian") {
			commandId = kMoqiCommandAsciiPunct;
		} else if (itemName == L"fan_jian") {
			commandId = kMoqiCommandTraditionalization;
		}
		if (commandId != 0) {
			if (commandId == kMoqiCommandAsciiPunct ||
			    commandId == kMoqiCommandTraditionalization) {
				// These options do not have a language-bar button whose state we
				// can read back, so keep a local visual toggle for the skin icon.
				sogouStatusBarLocalStates_[itemName] ^= 1;
			}
			client_->onCommand(commandId, COMMAND_LEFT_CLICK);
			refreshSogouStatusBar();
			return;
		}
		const std::string id = utf16ToUtf8(itemName.c_str());
		if (auto* button = client_->findButton(id)) {
			onCommand(button->commandId(), COMMAND_LEFT_CLICK);
			return;
		}
	}
	auto sendKey = [](WORD vk, bool up) {
		INPUT input = {};
		input.type = INPUT_KEYBOARD;
		input.ki.wVk = vk;
		input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
		::SendInput(1, &input, sizeof(input));
	};
	auto sendCombo = [&](std::initializer_list<WORD> modifiers, WORD key) {
		for (WORD modifier : modifiers) {
			sendKey(modifier, false);
		}
		sendKey(key, false);
		sendKey(key, true);
		for (auto it = modifiers.end(); it != modifiers.begin();) {
			--it;
			sendKey(*it, true);
		}
	};

	if (itemName == L"cn_en") {
		// Standard IME Chinese/English toggle.
		sendKey(VK_SHIFT, false);
		sendKey(VK_SHIFT, true);
	} else if (itemName == L"quan_ban") {
		sendCombo({VK_SHIFT}, VK_SPACE);
	} else if (itemName == L"biaodian") {
		sendCombo({VK_CONTROL}, VK_OEM_PERIOD);
	} else if (itemName == L"fan_jian") {
		sendCombo({VK_CONTROL, VK_SHIFT}, 'F');
	} else {
		return;
	}

	const int count = itemName == L"cn_en" ? 2 : 2;
	const int current = sogouStatusBarLocalStates_[itemName];
	sogouStatusBarLocalStates_[itemName] = (current + 1) % count;
	refreshSogouStatusBar();
}

void TextService::showSogouStatusBarMenu(POINT screenPoint) {
	if (!sogouStatusBar_) {
		return;
	}
	static const struct QuickItem {
		const wchar_t* name;
		const wchar_t* label;
	} quickItems[] = {
		{L"cn_en", L"中英文切换"},
		{L"quan_ban", L"全角 / 半角"},
		{L"biaodian", L"中文 / 英文标点"},
		{L"fan_jian", L"简体 / 繁体"},
		{L"softkeyboard", L"软键盘"},
	};
	const int quickCount = static_cast<int>(_countof(quickItems));
	const auto options = SogouSkin::LoadOptions();

	HMENU menu = ::CreatePopupMenu();
	if (!menu) {
		return;
	}
	for (int i = 0; i < quickCount; ++i) {
		const int state = sogouStatusBarItemState(quickItems[i].name);
		::AppendMenuW(menu, MF_STRING | (state != 0 ? MF_CHECKED : 0),
		              static_cast<UINT_PTR>(i + 1), quickItems[i].label);
	}
	::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	if (HMENU symbolMenu = CreateSogouSymbolMenu()) {
		::AppendMenuW(menu, MF_POPUP,
		              reinterpret_cast<UINT_PTR>(symbolMenu), L"符号输入");
	}
	::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	::AppendMenuW(menu, MF_STRING | (options.horizontal ? MF_CHECKED : 0), 100,
	              L"候选窗横排");
	::AppendMenuW(menu, MF_STRING | (options.statusbar ? MF_CHECKED : 0), 101,
	              L"显示皮肤状态栏");
	::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	::AppendMenuW(menu, MF_STRING, 110, L"皮肤设置...");
	::AppendMenuW(menu, MF_STRING, 111, L"重新加载皮肤");

	HWND owner = sogouStatusBar_->hwnd();
	const HWND previousForeground = ::GetForegroundWindow();
	::SetForegroundWindow(owner);
	const UINT command = ::TrackPopupMenuEx(
		menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
		screenPoint.x, screenPoint.y, owner, nullptr);
	::DestroyMenu(menu);
	::PostMessageW(owner, WM_NULL, 0, 0);
	if (previousForeground && previousForeground != owner) {
		::SetForegroundWindow(previousForeground);
	}

	if (command == 0) {
		return;
	}
	if (command <= static_cast<UINT>(quickCount)) {
		handleSogouStatusBarClick(quickItems[command - 1].name);
		return;
	}
	if (const std::wstring* symbol = SogouSymbolForCommand(command)) {
		insertSogouSymbol(*symbol);
		return;
	}
	if (command == 100) {
		SogouSkin::Options updated = options;
		updated.horizontal = !updated.horizontal;
		SaveSogouOptions(updated);
		reloadSogouSkinFromFiles();
	} else if (command == 101) {
		SogouSkin::Options updated = options;
		updated.statusbar = !updated.statusbar;
		SaveSogouOptions(updated);
		setSogouStatusBarVisible(updated.statusbar);
	} else if (command == 110) {
		openSogouSkinSettings();
	} else if (command == 111) {
		reloadSogouSkinFromFiles();
	}
}

void TextService::insertSogouSymbol(const std::wstring& text) {
	if (text.empty()) {
		return;
	}
	stopKeyRepeat();
	auto context = currentContext();
	if (!context) {
		logDebug(L"[SogouSymbol] no current context");
		return;
	}

	// A symbol is a standalone commit; do not insert it into the middle of an
	// active pinyin composition.  Clear the composition range first, then end
	// it, so clicking a symbol never commits the raw pinyin string.
	if (isComposing()) {
		auto clearSession = Ime::ComPtr<Ime::EditSession>::make(
			context, [this](Ime::EditSession* editSession, TfEditCookie) {
				setCompositionString(editSession, L"", 0);
			});
		if (clearSession) {
			HRESULT clearResult = E_FAIL;
			context->RequestEditSession(clientId(), clearSession,
			                            TF_ES_SYNC | TF_ES_READWRITE,
			                            &clearResult);
		}
		endComposition(context);
	}

	HRESULT sessionResult = E_FAIL;
	auto session = Ime::ComPtr<Ime::EditSession>::make(
		context,
		[context, text](Ime::EditSession* editSession, TfEditCookie cookie) {
			auto insertAtSelection =
				Ime::ComPtr<ITfInsertAtSelection>::queryFrom(context);
			if (!insertAtSelection) {
				return;
			}
			Ime::ComPtr<ITfRange> range;
			insertAtSelection->InsertTextAtSelection(
				cookie, 0, text.c_str(), static_cast<LONG>(text.size()), &range);
		});
	if (session) {
		context->RequestEditSession(clientId(), session,
		                            TF_ES_SYNC | TF_ES_READWRITE,
		                            &sessionResult);
	}
	if (FAILED(sessionResult)) {
		logDebug(L"[SogouSymbol] InsertTextAtSelection failed hr=" +
		         std::to_wstring(static_cast<long>(sessionResult)));
	} else {
		logDebug(L"[SogouSymbol] inserted codepoints=" +
		         std::to_wstring(text.size()));
	}
}

void TextService::reloadSogouSkinFromFiles() {
	std::wstring error;
	auto theme = SogouSkin::LoadPreferredTheme(&error);
	if (!theme) {
		sogouSkinMode_ = false;
		sogouSkinChecked_ = true;
		sogouTheme_.reset();
		if (sogouStatusBar_) {
			sogouStatusBar_->hide();
		}
		if (candidateWindow_) {
			candidateWindow_->reloadSogouSkin();
		}
		logDebug(L"[SogouSkin] reload failed: " + error);
		return;
	}
	sogouTheme_ = theme;
	sogouSkinMode_ = true;
	sogouSkinChecked_ = true;
	if (!theme->statusBar().pic.empty()) {
		const bool statusbarEnabled = SogouSkin::LoadOptions().statusbar;
		if (sogouStatusBar_) {
			sogouStatusBar_->setTheme(theme);
			if (statusbarEnabled) {
				sogouStatusBar_->show();
			} else {
				sogouStatusBar_->hide();
			}
		} else {
			ensureSogouStatusBar();
			// create() intentionally leaves the window hidden; only this
			// process may show it, and show() re-validates the foreground.
			if (sogouStatusBar_ && statusbarEnabled) {
				sogouStatusBar_->show();
			}
		}
	} else if (sogouStatusBar_) {
		sogouStatusBar_->hide();
	}
	if (candidateWindow_) {
		candidateWindow_->reloadSogouSkin();
	}
	refreshSogouStatusBar();
	logDebug(L"[SogouSkin] reloaded");
}

void TextService::openSogouSkinSettings() {
	std::wstring dir;
	if (imeModule()) {
		auto* module = static_cast<Moqi::ImeModule*>(imeModule().operator->());
		if (module) {
			dir = module->programDir();
		}
	}
	std::wstring exe = dir.empty() ? std::wstring() : dir + L"\\SogouSkinSettings.exe";
	if (!exe.empty() &&
	    ::GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
		::ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr,
		                dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
		return;
	}
	::MessageBoxW(nullptr,
	              L"未找到 SogouSkinSettings.exe。\n请重新运行部署脚本，或从构建目录手动启动设置程序。",
	              L"墨奇输入法 - 搜狗皮肤", MB_OK | MB_ICONWARNING);
}

void TextService::setSogouStatusBarVisible(bool visible) {
	if (visible) {
		ensureSogouStatusBar();
		const auto options = SogouSkin::LoadOptions();
		if (sogouStatusBar_ && options.statusbar) {
			sogouStatusBar_->show();
		} else if (SogouSkin::IsForegroundProcess(::GetCurrentProcessId())) {
			// This TSF host has no skin status bar of its own (skin has no
			// [StatusBar], the option is off, or this process cannot render
			// one).  Clear the previous application's bar immediately instead
			// of waiting for its 150ms foreground guard.  UWP and other hosts
			// without a TSF service are still covered by that guard.
			SogouSkin::HideOtherStatusBarWindows(nullptr);
		}
	}
	else if (sogouStatusBar_) {
		sogouStatusBar_->hide();
	}
}

void TextService::configureKeyRepeat() {
	if (!keyRepeat_) {
		return;
	}
	const auto options = SogouSkin::LoadOptions();
	keyRepeat_->configure(options.keyRepeat);
}

void TextService::stopKeyRepeat() {
	if (keyRepeat_) {
		keyRepeat_->onFocusLost();
	}
	keyRepeatForeground_ = nullptr;
	suppressRepeatKeyDown_ = false;
	suppressedRepeatKey_ = 0;
}

void TextService::injectSogouKeyRepeat(WORD vk) {
	if (!keyRepeat_ || !keyRepeat_->enabled() || !client_) {
		stopKeyRepeat();
		return;
	}
	// SendInput targets the foreground queue.  Abort the moment focus moves
	// so repeats can never leak into another application.
	const HWND foreground = ::GetForegroundWindow();
	if (!foreground || foreground != keyRepeatForeground_) {
		stopKeyRepeat();
		return;
	}
	if (!currentContext()) {
		stopKeyRepeat();
		return;
	}
	KeyRepeatAccelerator::InjectKeyDown(vk);
}

// virtual
void TextService::onActivate() {
	std::wostringstream log;
	log << L"[onActivate] exe=" << processBaseName(currentProcessPath())
	    << L" flags=0x" << std::hex << activateFlags() << std::dec
	    << L" keyboard_open=" << boolText(isKeyboardOpened())
	    << L" is_ui_less=" << boolText(isUiLess())
	    << L" effective_ui_less=" << boolText(effectiveUiLess())
	    << L" auto_ui_less=" << boolText(autoUiLessOverride_)
	    << L" inline_preedit=" << boolText(inlinePreedit_)
	    << L" effective_inline_preedit=" << boolText(effectiveInlinePreedit())
	    << L" auto_inline_preedit_disabled=" << boolText(autoInlinePreeditDisabled_)
	    << L" auto_disable_tsf_candidate_ui=" << boolText(autoDisableTsfCandidateUi_)
	    << L" dummy_anchor_compat=" << boolText(autoDummyAnchorCompat_)
	    << L" manual_ui_less=" << boolText(manualUiLessOverride_)
	    << L" " << foregroundWindowSummary();
	logDebug(log.str());
	configureKeyRepeat();
	ensureSogouSkinMode();
	ensureClientForCurrentProfile(L"onActivate");
	setSogouStatusBarVisible(true);
}

// virtual
void TextService::onDeactivate() {
	logDebug(L"[onDeactivate] exe=" + processBaseName(currentProcessPath()) + L" " + foregroundWindowSummary());
	stopKeyRepeat();
	// Low-latency hide: only when the foreground window has actually moved to
	// another process.  If TSF deactivates this thread while the application
	// is still foreground (status-bar clicks, focus moves inside the app),
	// the check keeps the bar visible; the 150ms timer remains the fallback
	// for hosts that do not deliver deactivate/kill-focus at all.
	if (sogouStatusBar_) {
		sogouStatusBar_->checkForegroundNow();
	}
	if(client_) {
		closeClient();
	}
}

// virtual
void TextService::onFocus() {
}

// virtual
void TextService::onSetFocus() {
	setSogouStatusBarVisible(true);
	std::wostringstream log;
	log << L"[onSetFocus] exe=" << processBaseName(currentProcessPath())
		<< L" showing_candidates=" << boolText(showingCandidates_)
		<< L" pending_candidate_recovery=" << boolText(pendingCandidateRecovery_)
		<< L" " << foregroundWindowSummary();
	logDebug(log.str());
}

// virtual
void TextService::onKillFocus() {
	stopKeyRepeat();
	if (sogouStatusBar_) {
		sogouStatusBar_->checkForegroundNow();
	}
	std::wostringstream log;
	log << L"[onKillFocus] exe=" << processBaseName(currentProcessPath())
		<< L" showing_candidates=" << boolText(showingCandidates_)
		<< L" pending_candidate_recovery=" << boolText(pendingCandidateRecovery_)
		<< L" " << foregroundWindowSummary();
	logDebug(log.str());
}

// virtual
void TextService::onSetThreadFocus() {
	setSogouStatusBarVisible(true);
	std::wostringstream log;
	log << L"[onSetThreadFocus] exe=" << processBaseName(currentProcessPath())
	    << L" keyboard_open=" << boolText(isKeyboardOpened())
	    << L" effective_ui_less=" << boolText(effectiveUiLess())
	    << L" showing_candidates=" << boolText(showingCandidates_)
	    << L" pending_candidate_recovery=" << boolText(pendingCandidateRecovery_)
	    << L" " << foregroundWindowSummary();
	logDebug(log.str());
	ensureClientForCurrentProfile(L"onSetThreadFocus");
}

// virtual
void TextService::onKillThreadFocus() {
	stopKeyRepeat();
	if (sogouStatusBar_) {
		sogouStatusBar_->checkForegroundNow();
	}
	std::wostringstream log;
	log << L"[onKillThreadFocus] exe=" << processBaseName(currentProcessPath())
		<< L" showing_candidates=" << boolText(showingCandidates_)
		<< L" pending_candidate_recovery=" << boolText(pendingCandidateRecovery_)
		<< L" " << foregroundWindowSummary();
	logDebug(log.str());
}

// virtual
bool TextService::filterKeyDown(Ime::KeyEvent& keyEvent) {
	logDebug(L"[filterKeyDown] " + keyEventSummary(keyEvent) +
		L" client=" + boolText(client_ != nullptr) +
		L" keyboard_open=" + boolText(isKeyboardOpened()) +
		L" effective_ui_less=" + boolText(effectiveUiLess()));
	// if (keyEvent.isKeyToggled(VK_CAPITAL))
	//	return true;
	if (!client_ && !ensureClientForCurrentProfile(L"filterKeyDown")) {
		if (isOrdinaryPrintableKey(keyEvent)) {
			logDebug(L"[filterKeyDown] consumed printable key while client is not ready " + keyEventSummary(keyEvent));
			return true;
		}
		return false;
	}
	{
		const WORD key = static_cast<WORD>(keyEvent.keyCode());
		if (key == VK_SHIFT) {
			if (!shiftTapCandidate_ && !isComposing() &&
			    !hasKeyRepeatBlocker(keyEvent)) {
				shiftTapCandidate_ = true;
				shiftTapUsed_ = false;
				shiftTapStartTick_ = ::GetTickCount64();
			}
		} else if (shiftTapCandidate_) {
			shiftTapUsed_ = true;
		}
	}
	if (keyRepeat_ && keyRepeat_->enabled()) {
		const WORD key = static_cast<WORD>(keyEvent.keyCode());
		// Never re-arm on our own SendInput events.  This check is
		// deliberately independent of active(): if the user releases the
		// physical key before the injected key-down reaches the queue, a new
		// accelerator must not start without a matching key-up.
		const bool synthetic = KeyRepeatAccelerator::IsInjectedEvent();
		if (!synthetic) {
			const bool wasActive = keyRepeat_->active();
			const bool consume = keyRepeat_->onKeyDown(
				key,
				keyEvent.repeatCount(),
				hasKeyRepeatBlocker(keyEvent),
				showingCandidates(),
				::GetTickCount64());
			if (!wasActive && keyRepeat_->active()) {
				keyRepeatForeground_ = ::GetForegroundWindow();
			}
			if (consume) {
				// The accelerator emits the burst itself; swallow the OS
				// repeat so the two rates do not add up.  onKeyDown() must
				// also skip the backend for this same event.
				suppressRepeatKeyDown_ = true;
				suppressedRepeatKey_ = key;
				return true;
			}
		}
	}
	try {
		bool sehCaught = false;
		const bool result = callClientFilterKeyDown(client_.get(), keyEvent, sehCaught);
		if (sehCaught) {
			logDebug(L"[filterKeyDown] caught SEH exception from client");
			closeClient();
		}
		return result;
	}
	catch (...) {
		logDebug(L"[filterKeyDown] caught exception from client");
		closeClient();
		return false;
	}
}

// virtual
bool TextService::onKeyDown(Ime::KeyEvent& keyEvent, Ime::EditSession* session) {
	logDebug(L"[onKeyDown] " + keyEventSummary(keyEvent) +
		L" client=" + boolText(client_ != nullptr));
	if (suppressRepeatKeyDown_ &&
	    static_cast<WORD>(keyEvent.keyCode()) == suppressedRepeatKey_) {
		suppressRepeatKeyDown_ = false;
		return true;
	}
	suppressRepeatKeyDown_ = false;
	//if (keyEvent.isKeyToggled(VK_CAPITAL))
	//	return true;
	if (!client_)
		return false;
	try {
		bool sehCaught = false;
		const bool result = callClientOnKeyDown(client_.get(), keyEvent, session, sehCaught);
		if (sehCaught) {
			logDebug(L"[onKeyDown] caught SEH exception from client");
			closeClient();
		}
		return result;
	}
	catch (...) {
		logDebug(L"[onKeyDown] caught exception from client");
		closeClient();
		return false;
	}
}

// virtual
bool TextService::filterKeyUp(Ime::KeyEvent& keyEvent) {
	logDebug(L"[filterKeyUp] " + keyEventSummary(keyEvent) +
		L" client=" + boolText(client_ != nullptr));
	if (keyRepeat_) {
		keyRepeat_->onKeyUp(static_cast<WORD>(keyEvent.keyCode()));
		if (suppressedRepeatKey_ ==
		    static_cast<WORD>(keyEvent.keyCode())) {
			suppressRepeatKeyDown_ = false;
			suppressedRepeatKey_ = 0;
		}
	}
	if(!client_)
		return false;
	try {
		bool sehCaught = false;
		std::wstring shiftIconBefore;
		LangBarButton* shiftButton = nullptr;
		const bool shiftTap = keyEvent.keyCode() == VK_SHIFT &&
		                      shiftTapCandidate_ && !shiftTapUsed_;
		if (shiftTap) {
			shiftButton = client_->findButton("switch-lang");
			if (shiftButton) {
				shiftIconBefore = shiftButton->iconFile();
			}
		}
		const bool result = callClientFilterKeyUp(client_.get(), keyEvent, sehCaught);
		if (sehCaught) {
			logDebug(L"[filterKeyUp] caught SEH exception from client");
			closeClient();
		} else if (shiftTap && shiftButton && client_ &&
		           !shiftTapUsed_ &&
		           ::GetTickCount64() - shiftTapStartTick_ < 500) {
			// Rime's ascii_composer handles Shift while composing.  A bare
			// Shift tap in an empty buffer can be a no-op in some schemas;
			// if the backend did not change the switch-lang icon, toggle it
			// through the same command used by the language-bar button.
			if (shiftButton->iconFile() == shiftIconBefore) {
				client_->onCommand(kMoqiCommandAsciiMode, COMMAND_LEFT_CLICK);
			}
		}
		shiftTapCandidate_ = false;
		shiftTapUsed_ = false;
		return result;
	}
	catch (...) {
		logDebug(L"[filterKeyUp] caught exception from client");
		closeClient();
		return false;
	}
}

// virtual
bool TextService::onKeyUp(Ime::KeyEvent& keyEvent, Ime::EditSession* session) {
	logDebug(L"[onKeyUp] " + keyEventSummary(keyEvent) +
		L" client=" + boolText(client_ != nullptr));
	if (keyRepeat_) {
		keyRepeat_->onKeyUp(static_cast<WORD>(keyEvent.keyCode()));
		suppressRepeatKeyDown_ = false;
		suppressedRepeatKey_ = 0;
	}
	if(!client_)
		return false;
	try {
		bool sehCaught = false;
		const bool result = callClientOnKeyUp(client_.get(), keyEvent, session, sehCaught);
		if (sehCaught) {
			logDebug(L"[onKeyUp] caught SEH exception from client");
			closeClient();
		}
		return result;
	}
	catch (...) {
		logDebug(L"[onKeyUp] caught exception from client");
		closeClient();
		return false;
	}
}

bool TextService::highlightCandidate(int index) {
	if (!client_)
		return false;
	return client_->highlightCandidate(index);
}

bool TextService::selectCandidate(int index) {
	if (!client_)
		return false;
	return client_->selectCandidate(index);
}

bool TextService::changeCandidatePage(bool backward) {
	if (!client_)
		return false;
	return client_->changePage(backward);
}

// virtual
bool TextService::onPreservedKey(const GUID& guid) {
	if(!client_)
		return false;
	// some preserved keys registered in ctor are pressed
	return client_->onPreservedKey(guid);
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext* pContext, REFGUID rguid, BOOL* pfEaten) {
	if (pfEaten == nullptr) {
		return S_OK;
	}
	*pfEaten = FALSE;
	if (!client_ || pContext == nullptr || isKeyboardDisabled(pContext) || !isKeyboardOpened()) {
		return S_OK;
	}

	HRESULT sessionResult = E_FAIL;
	bool sehCaught = false;
	auto session = Ime::ComPtr<Ime::EditSession>::make(
		pContext,
		[&](Ime::EditSession* session, TfEditCookie cookie) {
			*pfEaten = callClientOnPreservedKey(client_.get(), rguid, session, sehCaught);
		}
	);
	pContext->RequestEditSession(clientId(), session, TF_ES_SYNC | TF_ES_READWRITE, &sessionResult);
	if (sehCaught) {
		logDebug(L"[onPreservedKey] SEH caught while handling preserved key");
		*pfEaten = FALSE;
	}
	if (FAILED(sessionResult)) {
		logDebug(L"[onPreservedKey] RequestEditSession failed hr=" + std::to_wstring(static_cast<long>(sessionResult)));
		*pfEaten = onPreservedKey(rguid);
	}
	return S_OK;
}


// virtual
bool TextService::onCommand(UINT id, CommandType type) {
	if(!client_)
		return false;
	return client_->onCommand(id, type);
}


// called when a language bar button needs a menu
// virtual
bool TextService::onMenu(LangBarButton* btn, ITfMenu* pMenu) {
	if (client_ != nullptr) {
		return client_->onMenu(btn, pMenu);
	}
	return false;
}

// called when a language bar button needs a menu
// virtual
HMENU TextService::onMenu(LangBarButton* btn) {
	if (client_ != nullptr) {
		return client_->onMenu(btn);
	}
	return NULL;
}


// virtual
void TextService::onCompartmentChanged(const GUID& key) {
	Ime::TextService::onCompartmentChanged(key);
	if(client_)
		client_->onCompartmentChanged(key);
}

// called when the keyboard is opened or closed
// virtual
void TextService::onKeyboardStatusChanged(bool opened) {
	Ime::TextService::onKeyboardStatusChanged(opened);
	if(client_)
		client_->onKeyboardStatusChanged(opened);
	refreshSogouStatusBar();
	if(opened) { // keyboard is opened
	}
	else { // keyboard is closed
		if(isComposing()) {
			// end current composition if needed
			if(auto context = currentContext()) {
				endComposition(context);
			}
		}
		if(showingCandidates()) // disable candidate window if it's opened
			hideCandidates();
		hideMessage(); // hide message window, if there's any
	}
}

// called just before current composition is terminated for doing cleanup.
// if forced is true, the composition is terminated by others, such as
// the input focus is grabbed by another application.
// if forced is false, the composition is terminated gracefully by endComposition().
// virtual
void TextService::onCompositionTerminated(bool forced) {
	if (forced) {
		// Focus was taken away by another app/context; never keep injecting.
		stopKeyRepeat();
	}
	// we do special handling here for forced composition termination.
	if (!forced && suppressNextCompositionTerminatedNotification_) {
		suppressNextCompositionTerminatedNotification_ = false;
		return;
	}
	if(forced) {
		const bool shouldPreserveCandidateRecovery =
			showingCandidates_ || !candidatePreedit_.empty() || !candidates_.empty();
		std::wostringstream log;
		log << L"[onCompositionTerminated] forced=true"
			<< L" showing_candidates=" << boolText(showingCandidates_)
			<< L" candidate_count=" << candidates_.size()
			<< L" preedit_len=" << candidatePreedit_.length()
			<< L" preserve_candidate_recovery=" << boolText(shouldPreserveCandidateRecovery);
		logDebug(log.str());
		// we're still editing our composition and have something in the preedit buffer.
		// however, some other applications grabs the focus and force us to terminate
		// our composition.
		if (showingCandidates()) // disable candidate window if it's opened
			hideCandidates(shouldPreserveCandidateRecovery);
		hideMessage(); // hide message window, if there's any
	}
	else {
		logDebug(L"[onCompositionTerminated] forced=false");
	}
	if(client_)
		client_->onCompositionTerminated(forced);
}

void TextService::onLangProfileActivated(REFIID lang) {
	logDebug(L"[onLangProfileActivated] profile=" + guidToString(lang) +
		L" exe=" + processBaseName(currentProcessPath()) +
		L" " + foregroundWindowSummary());
	currentLangProfile_ = lang;
	// Sometimes, Windows does not deactivate the old language profile before
	// activating the new one. So here we do it by ourselves.
	// If a new profile is activated, but there is an old one remaining active,
	// deactive it first.
	if (client_ != nullptr)
		closeClient();

	// create a new client connection to the input method server for the language profile
	try {
		client_ = std::make_unique<Client>(this, lang);
		client_->onActivate();
	}
	catch (...) {
		logDebug(L"[onLangProfileActivated] caught exception while creating client");
		closeClient();
	}
}

void TextService::onLangProfileDeactivated(REFIID lang) {
	logDebug(L"[onLangProfileDeactivated] profile=" + guidToString(lang) +
		L" exe=" + processBaseName(currentProcessPath()) +
		L" " + foregroundWindowSummary());
	closeClient();
	if (::IsEqualGUID(currentLangProfile_, lang)) {
		currentLangProfile_ = GUID_NULL;
	}
}

void TextService::onLayoutChange(ITfContext* context, TfLayoutCode code, ITfContextView* view) {
	if (code != TF_LC_CHANGE || !context || !hasCandidateWindow()) {
		return;
	}

	auto editSession = Ime::ComPtr<Ime::EditSession>::make(
		Ime::ComPtr<ITfContext>(context),
		[this](Ime::EditSession* session, TfEditCookie cookie) {
			updateCandidatesWindow(session);
			updateMessageWindow(session);
		}
	);

	HRESULT sessionResult = E_FAIL;
	context->RequestEditSession(clientId(), editSession, TF_ES_ASYNCDONTCARE | TF_ES_READ, &sessionResult);

	std::wostringstream log;
	log << L"[onLayoutChange] code=" << code
		<< L" requested=" << boolText(SUCCEEDED(sessionResult))
		<< L" has_candidate_window=" << boolText(hasCandidateWindow());
	logDebug(log.str());
}

bool TextService::ensureCandidateWindowValid(const wchar_t* reason) {
	if (!candidateWindow_) {
		return false;
	}
	if (candidateWindow_->isWindow()) {
		return true;
	}

	std::wostringstream log;
	log << L"[TextService::" << reason << L"] candidate window hwnd invalid; recreating";
	appendCandidateWindowLog(log.str());

	if (validCandidateListElementId_) {
		auto elementMgr = Ime::ComPtr<ITfUIElementMgr>::queryFrom(threadMgr());
		if (elementMgr) {
			elementMgr->EndUIElement(candidateListElementId_);
		}
		candidateListElementId_ = 0;
		validCandidateListElementId_ = false;
	}

	candidateWindow_ = nullptr;
	invalidateCandidateUiCache();
	return false;
}

void TextService::createCandidateWindow(Ime::EditSession* session) {
	if (!tsfCandidateUiEnabled()) {
		appendCandidateWindowLog(L"[TextService::createCandidateWindow] skipped by process policy");
		return;
	}
	ensureCandidateWindowValid(L"createCandidateWindow");
	if (!candidateWindow_) {
		appendCandidateWindowLog(L"[TextService::createCandidateWindow] creating");
		shouldShowCandidateWindowUI_ = !effectiveUiLess();
		candidateWindow_ = new Moqi::CandidateWindow(this, session); // assigning to smart ptr also inrease ref count
		candidateWindow_->Release();  // decrease ref count caused by new

		candidateWindow_->setFont(font_);
		candidateWindow_->setCommentFont(commentFont_);
		candidateWindow_->setBackgroundColor(candBackgroundColor_);
		candidateWindow_->setHighlightColor(candHighlightColor_);
		candidateWindow_->setTextColor(candTextColor_);
		candidateWindow_->setHighlightTextColor(candHighlightTextColor_);
		candidateWindow_->setPreeditText(effectiveInlinePreedit() ? L"" : candidatePreedit_);
		candidateWindow_->setPreeditCursor(effectiveInlinePreedit() ? 0 : candidatePreeditCursor_);
		auto elementMgr = Ime::ComPtr<ITfUIElementMgr>::queryFrom(threadMgr());
		if (elementMgr) {
			BOOL pbShow = shouldShowCandidateWindowUI_ ? TRUE : FALSE;
			if (validCandidateListElementId_ =
				(elementMgr->BeginUIElement(candidateWindow_, &pbShow, &candidateListElementId_) == S_OK)) {
				shouldShowCandidateWindowUI_ = !effectiveUiLess() && pbShow != FALSE;
				std::wostringstream log;
				log << L"[TextService::createCandidateWindow] BeginUIElement success pbShow=" << pbShow
					<< L" elementId=" << candidateListElementId_;
				appendCandidateWindowLog(log.str());
			}
			else {
				appendCandidateWindowLog(L"[TextService::createCandidateWindow] BeginUIElement failed");
			}
		}
		else {
			appendCandidateWindowLog(L"[TextService::createCandidateWindow] elementMgr unavailable");
		}
		candidateWindow_->Show(shouldShowCandidateWindowUI_ ? TRUE : FALSE);
		if (!shouldShowCandidateWindowUI_) {
			appendCandidateWindowLog(L"[TextService::createCandidateWindow] candidate window suppressed by UI-less host");
		}
	}
}

void TextService::destroyCandidateWindow() {
	if (validCandidateListElementId_) {
		auto elementMgr = Ime::ComPtr<ITfUIElementMgr>::queryFrom(threadMgr());
		if (elementMgr) {
			elementMgr->EndUIElement(candidateListElementId_);
		}
		candidateListElementId_ = 0;
		validCandidateListElementId_ = false;
	}
	if (candidateWindow_) {
		candidateWindow_->setPreeditText(L"");
		candidateWindow_->Show(FALSE);
		candidateWindow_ = nullptr;
		appendCandidateWindowLog(L"[TextService::destroyCandidateWindow] destroyed");
	}
	showingCandidates_ = false;
	pendingCandidateRecovery_ = false;
	candidatePreedit_.clear();
	candidatePreeditCursor_ = 0;
	invalidateCandidateUiCache();
}

void TextService::updateCandidates(Ime::EditSession* session) {
	const ULONGLONG fnStart = qpcMicros();
	if (!tsfCandidateUiEnabled()) {
		appendCandidateWindowLog(L"[TextService::updateCandidates] skipped by process policy");
		return;
	}
	createCandidateWindow(session);
	if (!candidateWindow_) {
		return;
	}
	candidateWindow_->syncOwner(session);
	const ULONGLONG ownerEnd = qpcMicros();

	applyCandidateAppearanceNow();

	const std::wstring renderedPreedit = effectiveInlinePreedit() ? L"" : candidatePreedit_;
	const bool contentChanged = !isCandidateContentApplied(renderedPreedit);
	const ULONGLONG contentStart = qpcMicros();
	if (contentChanged) {
		candidateWindow_->beginUpdate();
		candidateWindow_->clear();
		candidateWindow_->setUseCursor(candUseCursor_);
		candidateWindow_->setCandPerRow(candPerRow_);
		candidateWindow_->setCandSpacing(candSpacing_);
		candidateWindow_->setBackgroundColor(candBackgroundColor_);
		candidateWindow_->setHighlightColor(candHighlightColor_);
		candidateWindow_->setTextColor(candTextColor_);
		candidateWindow_->setHighlightTextColor(candHighlightTextColor_);
		candidateWindow_->setCommentColor(candCommentColor_);
		candidateWindow_->setCommentHighlightColor(candCommentHighlightColor_);
		candidateWindow_->setPreeditText(renderedPreedit);
		candidateWindow_->setPreeditCursor(effectiveInlinePreedit() ? 0 : candidatePreeditCursor_);

		// the items in the candidate list should not exist the
		// number of available keys used to select them.
		assert(candidates_.size() <= selKeys_.size());
		for (int i = 0; i < candidates_.size(); ++i) {
			candidateWindow_->add(candidates_[i], selKeys_[i]);
		}
		// endUpdate() performs the single layout pass for this response.
		candidateWindow_->recalculateSize();
		candidateWindow_->endUpdate();
		candidateWindow_->refresh();
		markCandidateContentApplied(renderedPreedit);
		hasAppliedCandidateCursor_ = false;
	}
	const ULONGLONG contentEnd = qpcMicros();

	const ULONGLONG moveStart = qpcMicros();
	moveCandidateWindowToInputRect(session, L"updateCandidates", true);
	const ULONGLONG moveEnd = qpcMicros();

	ULONGLONG showEnd = moveEnd;
	if (showingCandidates_) {
		candidateWindow_->Show(shouldShowCandidateWindowUI_ ? TRUE : FALSE);
		std::wostringstream log;
		log << L"[TextService::updateCandidates] ensured visibility should_show_ui="
			<< boolText(shouldShowCandidateWindowUI_);
		appendCandidateWindowLog(log.str());
	}
	showEnd = qpcMicros();

	ULONGLONG uiElementEnd = showEnd;
	if (contentChanged && validCandidateListElementId_) {
		auto elementMgr = Ime::ComPtr<ITfUIElementMgr>::queryFrom(threadMgr());
		if (elementMgr) {
			elementMgr->UpdateUIElement(candidateListElementId_);
		}
	}
	uiElementEnd = qpcMicros();
	if (Ime::isTraceLoggingEnabled()) {
		std::wostringstream timing;
		timing << L"[TextService::updateCandidates.detail] owner_us="
		       << (ownerEnd - fnStart)
		       << L" content_us=" << (contentEnd - contentStart)
		       << L" move_us=" << (moveEnd - moveStart)
		       << L" show_us=" << (showEnd - moveEnd)
		       << L" uielement_us=" << (uiElementEnd - showEnd)
		       << L" total_us=" << (uiElementEnd - fnStart)
		       << L" changed=" << boolText(contentChanged);
		appendCandidateWindowLog(timing.str());
	}
}

void TextService::updateCandidatesWindow(Ime::EditSession* session) {
    if (!tsfCandidateUiEnabled()) {
        return;
    }
    ensureCandidateWindowValid(L"updateCandidatesWindow");
    if (candidateWindow_) {
        candidateWindow_->syncOwner(session);
		moveCandidateWindowToInputRect(session, L"updateCandidatesWindow", true);
    }
}

void TextService::refreshCandidates() {
	if (!tsfCandidateUiEnabled()) {
		return;
	}
	if (validCandidateListElementId_) {
		auto elementMgr = Ime::ComPtr<ITfUIElementMgr>::queryFrom(threadMgr());
		if (elementMgr) {
			elementMgr->UpdateUIElement(candidateListElementId_);
		}
	}
}

bool TextService::setCandidateCursor(int cursor) {
	if (!tsfCandidateUiEnabled()) {
		return false;
	}
	if (candidateWindow_) {
		if (hasAppliedCandidateCursor_ && appliedCandidateCursor_ == cursor) {
			return false;
		}
		candidateWindow_->setCurrentSel(cursor);
		appliedCandidateCursor_ = cursor;
		hasAppliedCandidateCursor_ = true;
		return true;
	}
	return false;
}

void TextService::invalidateCandidateUiCache() {
	appliedCandidates_.clear();
	appliedSelKeys_.clear();
	appliedCandidatePreedit_.clear();
	hasAppliedCandidateContent_ = false;
	hasAppliedCandidateCursor_ = false;
	appliedCandidateCursor_ = 0;
	hasLastCandidateWindowPos_ = false;
	lastCandidateWindowPos_ = {0, 0};
	lastCandidateWindowMoveTick_ = 0;
}

bool TextService::isCandidateContentApplied(const std::wstring& renderedPreedit) const {
	return hasAppliedCandidateContent_ &&
		appliedCandidatePreedit_ == renderedPreedit &&
		appliedCandidates_ == candidates_ &&
		appliedSelKeys_ == selKeys_;
}

void TextService::markCandidateContentApplied(const std::wstring& renderedPreedit) {
	appliedCandidatePreedit_ = renderedPreedit;
	appliedCandidates_ = candidates_;
	appliedSelKeys_ = selKeys_;
	hasAppliedCandidateContent_ = true;
}

bool TextService::moveCandidateWindowToInputRect(Ime::EditSession* session, const wchar_t* reason, bool throttleSamePosition) {
	if (!candidateWindow_) {
		return false;
	}

	RECT textRect;
	const ULONGLONG inputStart = qpcMicros();
	// get the position of composition area from TSF
	if (!inputRect(session, &textRect)) {
		std::wstring tag = L"[TextService::";
		tag += reason;
		tag += L"] inputRect unavailable";
		appendCandidateWindowLog(tag);
		return false;
	}
	const ULONGLONG inputEnd = qpcMicros();

	const POINT nextPos{textRect.left, textRect.bottom};
	const ULONGLONG now = ::GetTickCount64();
	if (throttleSamePosition && hasLastCandidateWindowPos_ &&
		lastCandidateWindowPos_.x == nextPos.x &&
		lastCandidateWindowPos_.y == nextPos.y &&
		now - lastCandidateWindowMoveTick_ < kCandidateWindowMoveThrottleMs) {
		return false;
	}

	// FIXME: where should we put the candidate window?
	candidateWindow_->move(nextPos.x, nextPos.y);
	const ULONGLONG moveEnd = qpcMicros();
	hasLastCandidateWindowPos_ = true;
	lastCandidateWindowPos_ = nextPos;
	lastCandidateWindowMoveTick_ = now;

	std::wostringstream log;
	log << L"[TextService::" << reason << L"] moved left=" << nextPos.x
		<< L" bottom=" << nextPos.y;
	appendCandidateWindowLog(log.str());
	if (Ime::isTraceLoggingEnabled()) {
		std::wostringstream timing;
		timing << L"[TextService::" << reason << L".timing] input_us="
		       << (inputEnd - inputStart)
		       << L" move_us=" << (moveEnd - inputEnd)
		       << L" total_us=" << (moveEnd - inputStart);
		appendCandidateWindowLog(timing.str());
	}
	return true;
}

// show candidate list window
void TextService::showCandidates(Ime::EditSession* session) {
	if (!tsfCandidateUiEnabled()) {
		showingCandidates_ = true;
		pendingCandidateRecovery_ = false;
		appendCandidateWindowLog(L"[TextService::showCandidates] skipped by process policy");
		return;
	}
	// NOTE: in Windows 8 store apps, candidate window should be owned by
	// composition window, which can be returned by TextService::compositionWindow().
	// Otherwise, the candidate window cannot be shown.
	// Ime::CandidateWindow handles this internally. If you create your own
	// candidate window, you need to call TextService::isImmersive() to check
	// if we're in a Windows store app. If isImmersive() returns true,
	// The candidate window created should be a child window of the composition window.
	// Please see Ime::CandidateWindow::CandidateWindow() for an example.
	createCandidateWindow(session);
	if (candidateWindow_) {
		candidateWindow_->syncOwner(session);
		candidateWindow_->Show(shouldShowCandidateWindowUI_ ? TRUE : FALSE);
	}
	showingCandidates_ = true;
	pendingCandidateRecovery_ = false;
	appendCandidateWindowLog(L"[TextService::showCandidates] shown");
}

// hide candidate list window
void TextService::hideCandidates(bool preserveRecoveryState) {
	if (!tsfCandidateUiEnabled()) {
		showingCandidates_ = false;
		pendingCandidateRecovery_ = preserveRecoveryState;
		appendCandidateWindowLog(L"[TextService::hideCandidates] skipped by process policy");
		return;
	}
	if (ensureCandidateWindowValid(L"hideCandidates")) {
		candidateWindow_->setPreeditText(L"");
		candidateWindow_->Show(FALSE);
		candidateWindow_->clear();
	}
	invalidateCandidateUiCache();
	showingCandidates_ = false;
	pendingCandidateRecovery_ = preserveRecoveryState;
	std::wostringstream log;
	log << L"[TextService::hideCandidates] hidden preserve_recovery="
		<< boolText(preserveRecoveryState);
	appendCandidateWindowLog(log.str());
}

// message window
void TextService::showMessage(Ime::EditSession* session, std::wstring message, int duration) {
	if (effectiveUiLess()) {
		hideMessage();
		return;
	}

	// remove previous message if there's any
	hideMessage();
	// FIXME: reuse the window whenever possible
	messageWindow_ = make_unique<Ime::MessageWindow>(this, session);
	messageWindow_->setFont(font_);
	messageWindow_->setText(message);
	
	int x = 0, y = 0;
	if(isComposing()) {
		RECT rc;
		if(inputRect(session, &rc)) {
			x = rc.left;
			y = rc.bottom;
		}
	}
	messageWindow_->move(x, y);
	messageWindow_->show();

	messageTimerId_ = ::SetTimer(messageWindow_->hwnd(), 1, duration * 1000, (TIMERPROC)TextService::onMessageTimeout);
}

void TextService::updateMessageWindow(Ime::EditSession* session) {
    if (messageWindow_) {
        RECT textRect;
        // get the position of composition area from TSF
        if (inputRect(session, &textRect)) {
            // FIXME: where should we put the message window?
            messageWindow_->move(textRect.left, textRect.bottom);
        }
    }
}

void TextService::hideMessage() {
	if(messageTimerId_) {
		::KillTimer(messageWindow_->hwnd(), messageTimerId_);
		messageTimerId_ = 0;
	}
	if(messageWindow_) {
		messageWindow_ = nullptr;
	}
}

// called when the message window timeout
void TextService::onMessageTimeout() {
	hideMessage();
}

// static
void CALLBACK TextService::onMessageTimeout(HWND hwnd, UINT msg, UINT_PTR id, DWORD time) {
	Ime::MessageWindow* messageWindow = (Ime::MessageWindow*)Ime::Window::fromHwnd(hwnd);
	assert(messageWindow);
	if(messageWindow) {
		TextService* pThis = (Moqi::TextService*)messageWindow->textService();
		pThis->onMessageTimeout();
	}
}


void TextService::updateLangButtons() {
}

int TextService::candFontHeight() {
	int candFontHeight_ = -candFontSize_;
	HDC hdc = GetDC(NULL);
	if (hdc)
	{
		candFontHeight_ = -MulDiv(candFontSize_, GetDeviceCaps(hdc, LOGPIXELSY), 72);
		ReleaseDC(NULL, hdc);
	}
	return candFontHeight_;
}

int TextService::candCommentFontHeight() {
	int candFontHeight_ = -candCommentFontSize_;
	HDC hdc = GetDC(NULL);
	if (hdc)
	{
		candFontHeight_ = -MulDiv(candCommentFontSize_, GetDeviceCaps(hdc, LOGPIXELSY), 72);
		ReleaseDC(NULL, hdc);
	}
	return candFontHeight_;
}

void TextService::applyCandidateAppearanceNow() {
	if (!updateFont_) {
		return;
	}

	HFONT baseFont = font_;
	if (!baseFont) {
		baseFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	}

	LOGFONT lf{};
	GetObject(baseFont, sizeof(lf), &lf);
	if (font_) {
		::DeleteObject(font_);
		font_ = nullptr;
	}
	if (commentFont_) {
		::DeleteObject(commentFont_);
		commentFont_ = nullptr;
	}

	lf.lfHeight = candFontHeight();
	lf.lfWeight = FW_NORMAL;
	if (!candFontName_.empty()) {
		wcsncpy_s(lf.lfFaceName, _countof(lf.lfFaceName), candFontName_.c_str(), _TRUNCATE);
	}
	font_ = CreateFontIndirect(&lf);

	lf.lfHeight = candCommentFontHeight();
	const std::wstring& commentFontName =
		candCommentFontName_.empty() ? std::wstring(kDefaultCommentFontFace) : candCommentFontName_;
	wcsncpy_s(lf.lfFaceName, _countof(lf.lfFaceName), commentFontName.c_str(), _TRUNCATE);
	commentFont_ = CreateFontIndirect(&lf);
	updateFont_ = false;

	if (messageWindow_) {
		messageWindow_->setFont(font_);
	}
	if (candidateWindow_) {
		candidateWindow_->setFont(font_);
		candidateWindow_->setCommentFont(commentFont_);
	}
	refreshCandidateAppearance();
}

void TextService::refreshCandidateAppearance() {
	if (!candidateWindow_) {
		return;
	}
	candidateWindow_->setUseCursor(candUseCursor_);
	candidateWindow_->setCandPerRow(candPerRow_);
	candidateWindow_->setCandSpacing(candSpacing_);
	candidateWindow_->setBackgroundColor(candBackgroundColor_);
	candidateWindow_->setHighlightColor(candHighlightColor_);
	candidateWindow_->setTextColor(candTextColor_);
	candidateWindow_->setHighlightTextColor(candHighlightTextColor_);
	candidateWindow_->setPreeditText(effectiveInlinePreedit() ? L"" : candidatePreedit_);
	candidateWindow_->setPreeditCursor(effectiveInlinePreedit() ? 0 : candidatePreeditCursor_);
	candidateWindow_->recalculateSize();
	candidateWindow_->refresh();
	refreshCandidates();
}

void TextService::applyUiLessOverrideState() {
	shouldShowCandidateWindowUI_ = !effectiveUiLess();
	if (!tsfCandidateUiEnabled()) {
		destroyCandidateWindow();
		hideMessage();
		return;
	}
	if (candidateWindow_) {
		candidateWindow_->setPreeditText(effectiveInlinePreedit() ? L"" : candidatePreedit_);
		candidateWindow_->setPreeditCursor(effectiveInlinePreedit() ? 0 : candidatePreeditCursor_);
		candidateWindow_->Show(shouldShowCandidateWindowUI_ ? TRUE : FALSE);
	}
	if (effectiveUiLess()) {
		hideMessage();
	}
	refreshCandidates();
}

bool TextService::ensureClientForCurrentProfile(const wchar_t* reason) {
	if (client_) {
		return true;
	}
	if (::IsEqualGUID(currentLangProfile_, GUID_NULL)) {
		logDebug(std::wstring(L"[ensureClient] no current profile reason=") + (reason ? reason : L"<null>"));
		return false;
	}
	try {
		logDebug(std::wstring(L"[ensureClient] creating lazy client reason=") + (reason ? reason : L"<null>") +
			L" profile=" + guidToString(currentLangProfile_));
		client_ = std::make_unique<Client>(this, currentLangProfile_);
		client_->onActivate();
		return client_ != nullptr;
	}
	catch (...) {
		logDebug(std::wstring(L"[ensureClient] caught exception reason=") + (reason ? reason : L"<null>"));
		closeClient();
		return false;
	}
}

void TextService::closeClient() {
	stopKeyRepeat();
	// deactive currently active language profile
	if (client_) {
		// disconnect from the server
		client_->onDeactivate();
		client_ = nullptr;
		// detroy UI resources
		hideMessage();
		destroyCandidateWindow();
	}
}

} // namespace Moqi
