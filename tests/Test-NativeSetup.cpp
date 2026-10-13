#include <iostream>
#include <fstream>
#include <iterator>
#include <gdiplus.h>

void Require(bool condition, const wchar_t* message) {
    if (!condition) throw Failure{message};
}

std::vector<char> Contents(const std::wstring& path) {
    std::ifstream stream(path, std::ios::binary);
    Require(stream.good(), L"Cannot read shortcut for verification.");
    return std::vector<char>(std::istreambuf_iterator<char>(stream), {});
}

ComPtr<IShellLinkW> LoadLink(const std::wstring& path) {
    auto link = NewLink();
    ComPtr<IPersistFile> file;
    Check(link.As(&file), L"Query persist interface");
    Check(file->Load(path.c_str(), STGM_READ), L"Load shortcut");
    return link;
}

void VerifyLink(const std::wstring& path, const std::wstring& executable, WORD expectedKey) {
    auto link = LoadLink(path);
    Require(OwnedLink(link.Get(), executable), L"Shortcut target or arguments differ.");
    WORD key = 0;
    Check(link->GetHotkey(&key), L"Read hotkey");
    Require(key == expectedKey, L"Saved hotkey differs.");
    wchar_t directory[32768]{};
    Check(link->GetWorkingDirectory(directory, ARRAYSIZE(directory)), L"Read working directory");
    Require(ParentFolder(executable) == directory, L"Working directory differs.");
    wchar_t iconPath[32768]{};
    int iconIndex = -1;
    Check(link->GetIconLocation(iconPath, ARRAYSIZE(iconPath), &iconIndex), L"Read shortcut icon");
    Require(std::wstring(iconPath) != executable && iconIndex == 0 &&
        std::wstring(iconPath).find(L".ico") != std::wstring::npos,
        L"Shortcut still reuses the old EXE icon cache location.");
    Require(CurrentIconFile(iconPath, ShortcutIconBytes()), L"Shortcut icon differs from the embedded artwork.");
    HICON extracted = nullptr;
    Require(ExtractIconExW(iconPath, 0, &extracted, nullptr, 1) == 1 && extracted,
        L"Shell could not extract the shortcut icon.");
    DestroyIcon(extracted);
}

DWORD delayValue = 0;
bool mockSuccess = true;
void WINAPI MockDelay(DWORD milliseconds) { delayValue = milliseconds; }
LRESULT WINAPI MockSend(HWND window, UINT message, WPARAM w, LPARAM l, UINT flags, UINT timeout, PDWORD_PTR result) {
    Require(window == HWND_BROADCAST && message == WM_SYSCOMMAND && w == SC_MONITORPOWER && l == 2 &&
        flags == SMTO_ABORTIFHUNG && timeout == 2000 && result != nullptr, L"Monitor command differs.");
    *result = 0;
    return mockSuccess ? 1 : 0;
}

int simulatedWinKey = VK_LWIN;
SHORT WINAPI MockHeldWin(int key) {
    return key == VK_CONTROL || key == simulatedWinKey ? static_cast<SHORT>(0x8000) : 0;
}

void VerifyKeyCapture() {
    UI::State state;
    state.beforeCapture = state.selected;
    state.recording = true;
    UI::RecordKey(state, VK_LCONTROL, true);
    UI::RecordKey(state, VK_LSHIFT, true);
    UI::RecordKey(state, VK_F10, true);
    UI::RecordKey(state, VK_F10, false);
    Require(state.recording, L"Recording ended before modifiers were released.");
    UI::RecordKey(state, VK_LSHIFT, false);
    UI::RecordKey(state, VK_LCONTROL, false);
    Require(!state.recording && state.selected == MAKEWORD(VK_F10, HOTKEYF_CONTROL | HOTKEYF_SHIFT),
        L"Ctrl+Shift+F10 was not selected on release.");
    state.recording = true; state.beforeCapture = state.selected;
    UI::RecordKey(state, VK_F13, true);
    Require(state.recording && state.hasError && state.pending == 0, L"Invalid chord accepted.");
    UI::RecordKey(state, VK_ESCAPE, true);
    Require(!state.recording && state.selected == MAKEWORD(VK_F10, HOTKEYF_CONTROL | HOTKEYF_SHIFT), L"Cancel changed the selected key.");
    state.recording = true; state.beforeCapture = state.selected;
    state.ctrl = state.alt = state.shift = state.win = false;
    UI::RecordKey(state, VK_LCONTROL, true);
    UI::RecordKey(state, VK_F12, true);
    Require(state.pending != 0, L"Valid candidate was not recorded.");
    UI::RecordKey(state, VK_LWIN, true);
    Require(state.pending == 0 && state.hasError, L"Adding Win retained a valid candidate.");
    UI::RecordKey(state, VK_F12, false);
    UI::RecordKey(state, VK_LCONTROL, false);
    UI::RecordKey(state, VK_LWIN, false);
    Require(state.recording, L"Unsupported Win chord was accepted.");
    UI::RecordKey(state, VK_ESCAPE, true);
    for (int winKey : {VK_LWIN, VK_RWIN}) {
        simulatedWinKey = winKey;
        UI::BeginCapture(state, MockHeldWin);
        Require(state.win && state.ctrl, L"Win held before capture was ignored.");
        UI::RecordKey(state, VK_F12, true);
        Require(state.pending == 0 && state.hasError, L"Pre-held Win chord was accepted.");
        UI::RecordKey(state, VK_ESCAPE, true);
        Require(state.hook == nullptr && UI::recordingState == nullptr, L"Win chord cancellation leaked the hook.");
    }
    state.recording = true; state.beforeCapture = state.selected;
    state.ctrl = state.alt = state.shift = state.win = state.leftWin = state.rightWin = false;
    UI::RecordKey(state, VK_LWIN, true);
    UI::RecordKey(state, VK_RWIN, true);
    UI::RecordKey(state, VK_LWIN, false);
    UI::RecordKey(state, VK_LCONTROL, true);
    UI::RecordKey(state, VK_F12, true);
    Require(state.win && state.pending == 0 && state.hasError, L"Second held Win key was ignored.");
    UI::RecordKey(state, VK_ESCAPE, true);
}

void VerifyIcon() {
    auto instance = GetModuleHandleW(nullptr);
    for (int size : {16, 20, 24, 32, 40, 48, 64, 128, 256}) {
        HICON icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_OLED_OFF), IMAGE_ICON, size, size, 0));
        Require(icon != nullptr, L"Embedded icon could not be loaded.");
        ICONINFO info{};
        Require(GetIconInfo(icon, &info) != FALSE, L"Read embedded icon");
        BITMAP bitmap{};
        GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap);
        Require(bitmap.bmWidth == size && bitmap.bmHeight == size, L"Icon dimensions differ.");
        DeleteObject(info.hbmColor); DeleteObject(info.hbmMask); DestroyIcon(icon);
    }
    HICON large = nullptr, smallIcon = nullptr;
    UINT extracted = ExtractIconExW(ExecutablePath().c_str(), 0, &large, &smallIcon, 1);
    Require(extracted > 0 && extracted != static_cast<UINT>(-1) && large && smallIcon,
        L"Shell could not extract the application icon.");
    DestroyIcon(large); DestroyIcon(smallIcon);
    std::wcout << L"PASS: embedded icons at nine sizes and shell icon extraction.\n";
}

void Preview(UI::State& state, const std::wstring& path) {
    RECT client{}; GetClientRect(state.window, &client);
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
    auto old = SelectObject(dc, bitmap);
    // Render the real paint and owner-draw paths in client coordinates. The
    // offscreen window's non-client origin must not offset child controls.
    UI::Paint(state, dc);
    for (HWND control : {state.captureButton, state.resetButton, state.saveButton,
                        state.closeButton, state.editButton, state.desktopButton}) {
        if (!(GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE)) continue;
        RECT rect{}; GetWindowRect(control, &rect);
        MapWindowPoints(HWND_DESKTOP, state.window, reinterpret_cast<POINT*>(&rect), 2);
        int previous = SaveDC(dc);
        SetViewportOrgEx(dc, rect.left, rect.top, nullptr);
        DRAWITEMSTRUCT draw{};
        draw.CtlID = static_cast<UINT>(GetDlgCtrlID(control));
        draw.hwndItem = control; draw.hDC = dc;
        draw.rcItem = RECT{0, 0, rect.right - rect.left, rect.bottom - rect.top};
        if (!IsWindowEnabled(control)) draw.itemState |= ODS_DISABLED;
        UI::DrawButton(state, draw);
        RestoreDC(dc, previous);
    }
    CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    {
        Gdiplus::Bitmap image(bitmap, nullptr);
        Require(image.Save(path.c_str(), &png) == Gdiplus::Ok, L"Save UI preview");
    }
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(nullptr, screen);
}

void VerifyUI(const std::wstring& folder, const std::wstring& executable) {
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput startup;
    Require(Gdiplus::GdiplusStartup(&token, &startup, nullptr) == Gdiplus::Ok, L"Initialize preview renderer");
    {
        UI::State state;
        state.executable = executable;
        state.iconDirectory = folder + L"\\icons";
        state.destination = folder + L"\\ui shortcut.lnk";
        UI::CreateSetupWindow(state, GetModuleHandleW(nullptr));
        Require(state.selected == UI::DefaultKey && IsWindowEnabled(state.saveButton), L"Initial UI state differs.");
        Preview(state, folder + L"\\initial.png");
        SendMessageW(state.window, WM_COMMAND, UI::Capture, 0);
        Require(state.recording && !IsWindowEnabled(state.saveButton), L"Capture does not disable save.");
        Preview(state, folder + L"\\recording.png");
        UI::RecordKey(state, VK_LCONTROL, true);
        UI::RecordKey(state, VK_LSHIFT, true);
        UI::RecordKey(state, VK_F10, true);
        UI::RecordKey(state, VK_F10, false);
        UI::RecordKey(state, VK_LSHIFT, false);
        UI::RecordKey(state, VK_LCONTROL, false);
        Require(!state.recording && state.hook == nullptr && UI::recordingState == nullptr, L"Hook remained after recording.");
        SendMessageW(state.window, WM_COMMAND, UI::Save, 0);
        Require(state.complete && state.saved == state.selected, L"Save did not show completion.");
        VerifyLink(state.destination, executable, MAKEWORD(VK_F10, HOTKEYF_CONTROL | HOTKEYF_SHIFT));
        Preview(state, folder + L"\\complete.png");
        SendMessageW(state.window, WM_COMMAND, UI::Edit, 0);
        Require(!state.complete && !IsWindowEnabled(state.saveButton), L"Unchanged settings allow redundant save.");
        SendMessageW(state.window, WM_COMMAND, UI::Capture, 0);
        SendMessageW(state.window, WM_ACTIVATE, WA_INACTIVE, 0);
        Require(!state.recording && state.hook == nullptr, L"Focus loss left recording active.");
        SendMessageW(state.window, WM_COMMAND, UI::Reset, 0);
        Require(state.selected == UI::DefaultKey && IsWindowEnabled(state.saveButton), L"Reset failed.");
        auto realDestination = state.destination;
        state.destination = folder + L"\\missing folder\\shortcut.lnk";
        SendMessageW(state.window, WM_COMMAND, UI::Save, 0);
        Require(state.hasError && !state.complete && !state.feedback.empty(), L"Save failure was not shown inline.");
        state.destination = realDestination;
        SendMessageW(state.window, WM_COMMAND, UI::Capture, 0);
        SendMessageW(state.window, WM_COMMAND, UI::Capture, 0);
        Require(!state.recording && state.selected == UI::DefaultKey, L"Cancel button changed selection.");
        SendMessageW(state.window, WM_COMMAND, UI::Capture, 0);
        DestroyWindow(state.window);
        Require(!state.recording && state.hook == nullptr && UI::recordingState == nullptr, L"Closing left recording active.");
    }
    for (int dpi : {96, 144, 192}) {
        UI::State state;
        state.dpi = dpi;
        state.executable = executable;
        state.iconDirectory = folder + L"\\icons";
        state.destination = folder + L"\\ui shortcut.lnk";
        UI::CreateSetupWindow(state, GetModuleHandleW(nullptr));
        Require(state.selected == MAKEWORD(VK_F10, HOTKEYF_CONTROL | HOTKEYF_SHIFT), L"Saved settings were not restored.");
        Preview(state, folder + L"\\existing-" + std::to_wstring(dpi) + L".png");
        DestroyWindow(state.window);
    }
    {
        auto conflict = folder + L"\\conflict.lnk";
        auto link = NewLink();
        Check(link->SetPath(executable.c_str()), L"Conflict target");
        Check(link->SetArguments(L"--unrelated"), L"Conflict arguments");
        ComPtr<IPersistFile> file; Check(link.As(&file), L"Conflict persist");
        Check(file->Save(conflict.c_str(), TRUE), L"Save conflict");
        UI::State state;
        state.executable = executable; state.destination = conflict;
        state.iconDirectory = folder + L"\\icons";
        UI::CreateSetupWindow(state, GetModuleHandleW(nullptr));
        Require(state.blocked && state.hasError && !IsWindowEnabled(state.saveButton), L"Conflict was not explained inline.");
        Preview(state, folder + L"\\conflict.png");
        DestroyWindow(state.window);
    }
    {
        auto destination = folder + L"\\ui shortcut.lnk";
        auto link = LoadLink(destination);
        Check(link->SetIconLocation(executable.c_str(), 0), L"Restore old icon reference");
        ComPtr<IPersistFile> file; Check(link.As(&file), L"Persist old icon reference");
        Check(file->Save(destination.c_str(), TRUE), L"Save old icon reference");
        file.Reset(); link.Reset();
        UI::State state;
        state.executable = executable; state.destination = destination;
        state.iconDirectory = folder + L"\\icons";
        UI::CreateSetupWindow(state, GetModuleHandleW(nullptr));
        WORD previousKey = state.saved;
        Require(state.needsMigration && IsWindowEnabled(state.saveButton),
            L"Old icon reference cannot be updated without changing the key.");
        SendMessageW(state.window, WM_COMMAND, UI::Save, 0);
        Require(state.complete && !state.needsMigration && state.saved == previousKey,
            L"Icon update changed the key or failed.");
        VerifyLink(destination, executable, previousKey);
        DestroyWindow(state.window);
    }
    Gdiplus::GdiplusShutdown(token);
    std::wcout << L"PASS: UI create/edit/save/completion, cancel/focus cleanup, existing settings, conflict, and 100/150/200% previews.\n";
    std::wcout << L"UI previews: " << folder << L"\n";
}

int wmain() {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    int exitCode = 0;
    try {
        VerifyKeyCapture();
        VerifyIcon();
        auto executable = ExecutablePath();
        auto project = ParentFolder(ParentFolder(executable));
        auto artifacts = project + L"\\tests\\artifacts";
        CreateDirectoryW(artifacts.c_str(), nullptr);
        auto folder = artifacts + L"\\native setup " + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        Require(CreateDirectoryW(folder.c_str(), nullptr) != FALSE, L"Create test directory");
        VerifyUI(folder, executable);
        auto iconDirectory = folder + L"\\icons";
        auto artwork = ShortcutIconBytes();
        auto changed = artwork; changed.back() ^= 1;
        Require(IconFileName(artwork) != IconFileName(changed), L"Changed artwork reused the cached icon name.");
        auto iconPath = EnsureShortcutIcon(iconDirectory);
        Require(iconPath == EnsureShortcutIcon(iconDirectory) && CurrentIconFile(iconPath, artwork),
            L"Repeated setup did not reuse the current icon.");
        {
            std::ofstream corrupt(iconPath, std::ios::binary | std::ios::trunc);
            corrupt << "broken";
        }
        Require(!CurrentIconFile(iconPath, artwork), L"Corrupted icon was not detected.");
        Require(EnsureShortcutIcon(iconDirectory) == iconPath && CurrentIconFile(iconPath, artwork),
            L"Corrupted icon was not repaired.");
        Require(DeleteFileW(iconPath.c_str()) != FALSE, L"Remove test icon");
        EnsureShortcutIcon(iconDirectory);
        Require(CurrentIconFile(iconPath, artwork), L"Missing icon was not restored.");
        auto shortcut = folder + L"\\モニターを消灯.lnk";
        WORD f12 = MAKEWORD(VK_F12, HOTKEYF_CONTROL | HOTKEYF_SHIFT);
        WORD f10 = MAKEWORD(VK_F10, HOTKEYF_CONTROL | HOTKEYF_SHIFT);
        WORD letter = MAKEWORD('O', HOTKEYF_CONTROL | HOTKEYF_ALT);
        for (WORD key : {f12, f10, letter}) {
            SaveShortcut(shortcut, executable, key, iconDirectory);
            VerifyLink(shortcut, executable, key);
        }
        for (WORD key : {static_cast<WORD>(0), MAKEWORD(VK_F10, 0), MAKEWORD('O', HOTKEYF_SHIFT),
                        MAKEWORD(VK_F13, HOTKEYF_CONTROL)}) {
            auto before = Contents(shortcut);
            bool rejected = false;
            try { SaveShortcut(shortcut, executable, key, iconDirectory); } catch (const Failure&) { rejected = true; }
            Require(rejected && before == Contents(shortcut), L"Invalid key changed the shortcut.");
        }
        auto link = LoadLink(shortcut);
        Check(link->SetArguments(L"--unrelated"), L"Set unrelated command");
        ComPtr<IPersistFile> file;
        Check(link.As(&file), L"Query persist interface");
        Check(file->Save(shortcut.c_str(), TRUE), L"Save unrelated command");
        file.Reset(); link.Reset();
        auto before = Contents(shortcut);
        bool rejected = false;
        try { SaveShortcut(shortcut, executable, f10, iconDirectory); } catch (const Failure&) { rejected = true; }
        Require(rejected && before == Contents(shortcut), L"Unrelated shortcut was overwritten.");
        rejected = false;
        try { RepairShortcut(shortcut, executable, iconDirectory); } catch (const Failure&) { rejected = true; }
        Require(rejected && before == Contents(shortcut), L"Repair overwrote an unrelated shortcut.");

        // Migrate only the previous launcher from the same application directory.
        link = NewLink();
        wchar_t windows[32768]{};
        Require(GetWindowsDirectoryW(windows, ARRAYSIZE(windows)) != 0, L"Windows folder");
        auto powershell = std::wstring(windows) + L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
        auto original = L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
            ParentFolder(executable) + L"\\TurnOffMonitor.ps1\"";
        Check(link->SetPath(powershell.c_str()), L"Set old target");
        Check(link->SetArguments(original.c_str()), L"Set old arguments");
        Check(link.As(&file), L"Query persist interface");
        Check(file->Save(shortcut.c_str(), TRUE), L"Save old launcher");
        file.Reset(); link.Reset();
        SaveShortcut(shortcut, executable, f10, iconDirectory);
        VerifyLink(shortcut, executable, f10);
        link = LoadLink(shortcut);
        Check(link->SetIconLocation(executable.c_str(), 0), L"Set old icon");
        Check(link.As(&file), L"Persist old icon");
        Check(file->Save(shortcut.c_str(), TRUE), L"Save old icon");
        file.Reset(); link.Reset();
        RepairShortcut(shortcut, executable, iconDirectory);
        VerifyLink(shortcut, executable, f10);
        std::wcout << L"PASS: artwork-specific icon path, shell ICO extraction, missing/corrupt icon repair, and old-icon update preserving hotkey.\n";
        Require(TurnOffMonitor(MockSend, MockDelay) == 0 && delayValue == 500, L"Successful monitor request differs.");
        mockSuccess = false;
        Require(TurnOffMonitor(MockSend, MockDelay) == 1, L"Failed monitor request exit code differs.");
        std::wcout << L"PASS: native key capture, F12/F10/custom shortcut save and update, invalid input, overwrite protection, legacy migration, monitor command and exit codes.\n";
        std::wcout << L"Monitor-off API was substituted; desktop shortcuts were not changed.\n";
    } catch (const Failure& error) {
        std::wcerr << error.message << L"\n";
        exitCode = 1;
    }
    CoUninitialize();
    return exitCode;
}
