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
int dispatchCount = 0;
HWND dispatchedWindow = nullptr;
void WINAPI MockDelay(DWORD milliseconds) { delayValue = milliseconds; }
bool MockNewProcessAge(ULONGLONG& milliseconds) { milliseconds = 0; return true; }
LRESULT WINAPI MockDispatch(HWND window, UINT message, WPARAM w, LPARAM l) {
    Require(window != HWND_BROADCAST, L"Monitor requests must not be left in other applications' message queues.");
    DWORD process = 0;
    Require(GetWindowThreadProcessId(window, &process) == GetCurrentThreadId() && process == GetCurrentProcessId(),
        L"Monitor request must execute on the calling process and thread.");
    Require(!IsWindowVisible(window) && message == WM_SYSCOMMAND && w == SC_MONITORPOWER && l == 2,
        L"Monitor command differs.");
    ++dispatchCount;
    dispatchedWindow = window;
    return mockSuccess ? 0 : 1;
}

int simulatedWinKey = VK_LWIN;
SHORT WINAPI MockHeldWin(int key) {
    return key == VK_LCONTROL || key == simulatedWinKey ? static_cast<SHORT>(0x8000) : 0;
}

std::vector<int> simulatedHeldModifiers;
SHORT WINAPI MockHeldModifiers(int key) {
    return std::find(simulatedHeldModifiers.begin(), simulatedHeldModifiers.end(), key) !=
        simulatedHeldModifiers.end() ? static_cast<SHORT>(0x8000) : 0;
}

void VerifyKeyCapture() {
    // Releasing one side must not discard the other side of the same modifier.
    for (const auto& modifier : {std::pair<UINT, UINT>{VK_LCONTROL, VK_RCONTROL},
                                std::pair<UINT, UINT>{VK_LMENU, VK_RMENU},
                                std::pair<UINT, UINT>{VK_LSHIFT, VK_RSHIFT}}) {
        for (bool releaseLeft : {false, true}) {
            UI::State chord;
            chord.beforeCapture = chord.selected;
            chord.recording = true;
            if (modifier.first == VK_LSHIFT) UI::RecordKey(chord, VK_LCONTROL, true);
            UI::RecordKey(chord, modifier.first, true);
            UI::RecordKey(chord, modifier.second, true);
            UI::RecordKey(chord, releaseLeft ? modifier.first : modifier.second, false);
            UI::RecordKey(chord, VK_F10, true);
            UI::RecordKey(chord, VK_F10, false);
            Require(chord.recording, L"Capture ended while the other modifier was still held.");
            UI::RecordKey(chord, releaseLeft ? modifier.second : modifier.first, false);
            if (modifier.first == VK_LSHIFT) UI::RecordKey(chord, VK_LCONTROL, false);
            WORD expected = modifier.first == VK_LCONTROL ? MAKEWORD(VK_F10, HOTKEYF_CONTROL) :
                modifier.first == VK_LMENU ? MAKEWORD(VK_F10, HOTKEYF_ALT) :
                MAKEWORD(VK_F10, HOTKEYF_CONTROL | HOTKEYF_SHIFT);
            Require(!chord.recording && chord.selected == expected,
                L"A held left/right modifier was dropped from the selected shortcut.");
        }
    }
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

    for (const auto& modifier : {std::pair<UINT, UINT>{VK_LCONTROL, VK_RCONTROL},
                                std::pair<UINT, UINT>{VK_LMENU, VK_RMENU},
                                std::pair<UINT, UINT>{VK_LSHIFT, VK_RSHIFT}}) {
        for (bool releaseLeft : {false, true}) {
            simulatedHeldModifiers = {static_cast<int>(modifier.first), static_cast<int>(modifier.second)};
            if (modifier.first == VK_LSHIFT) simulatedHeldModifiers.push_back(VK_LCONTROL);
            UI::BeginCapture(state, MockHeldModifiers);
            UI::RecordKey(state, releaseLeft ? modifier.first : modifier.second, false);
            UI::RecordKey(state, VK_F10, true);
            UI::RecordKey(state, VK_F10, false);
            Require(state.recording, L"Pre-held opposite modifier was lost after one side was released.");
            UI::RecordKey(state, releaseLeft ? modifier.second : modifier.first, false);
            if (modifier.first == VK_LSHIFT) UI::RecordKey(state, VK_LCONTROL, false);
            WORD expected = modifier.first == VK_LCONTROL ? MAKEWORD(VK_F10, HOTKEYF_CONTROL) :
                modifier.first == VK_LMENU ? MAKEWORD(VK_F10, HOTKEYF_ALT) :
                MAKEWORD(VK_F10, HOTKEYF_CONTROL | HOTKEYF_SHIFT);
            Require(!state.recording && state.selected == expected && !state.hook && !UI::recordingState,
                L"A modifier held before capture was dropped or the completed capture leaked a hook.");
        }
    }

    const WORD beforeCancel = state.selected;
    simulatedHeldModifiers = {VK_LCONTROL, VK_RSHIFT};
    UI::BeginCapture(state, MockHeldModifiers);
    UI::RecordKey(state, VK_F12, true);
    UI::RecordKey(state, VK_ESCAPE, true);
    Require(state.selected == beforeCancel && !state.hook && !UI::recordingState,
        L"Cancelling a chord changed its saved selection or leaked a hook.");
    simulatedHeldModifiers.clear();
    UI::BeginCapture(state, MockHeldModifiers);
    UI::RecordKey(state, VK_RMENU, true);
    UI::RecordKey(state, VK_F10, true);
    UI::RecordKey(state, VK_F10, false);
    UI::RecordKey(state, VK_RMENU, false);
    Require(!state.recording && state.selected == MAKEWORD(VK_F10, HOTKEYF_ALT),
        L"A cancelled capture left modifiers in the next capture.");

    // Generic virtual-key events remain accepted by the recording interface.
    for (UINT modifier : {VK_CONTROL, VK_MENU, VK_SHIFT}) {
        UI::State generic;
        generic.beforeCapture = generic.selected;
        generic.recording = true;
        if (modifier == VK_SHIFT) UI::RecordKey(generic, VK_CONTROL, true);
        UI::RecordKey(generic, modifier, true);
        UI::RecordKey(generic, VK_F10, true);
        UI::RecordKey(generic, VK_F10, false);
        UI::RecordKey(generic, modifier, false);
        if (modifier == VK_SHIFT) UI::RecordKey(generic, VK_CONTROL, false);
        WORD expected = modifier == VK_CONTROL ? MAKEWORD(VK_F10, HOTKEYF_CONTROL) :
            modifier == VK_MENU ? MAKEWORD(VK_F10, HOTKEYF_ALT) :
            MAKEWORD(VK_F10, HOTKEYF_CONTROL | HOTKEYF_SHIFT);
        Require(!generic.recording && generic.selected == expected,
            L"Generic modifier virtual-key events were not accepted.");
    }
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

// Only the test executable exposes this mode. The injected dispatcher never
// forwards a system command, so process-lifetime tests cannot switch a display.
std::wstring lifecycleLog;
DWORD lifecycleDispatchDelay = 0, lifecycleStartDelay = 500;
HWND lifecycleWindow = nullptr;

void LifecycleEvent(const char* event) {
    std::ofstream stream(lifecycleLog, std::ios::app);
    Require(stream.good(), L"Cannot open lifecycle test log.");
    stream << event << '\n';
    stream.flush();
    Require(stream.good(), L"Cannot write lifecycle test log.");
}

void WINAPI LifecycleDelay(DWORD milliseconds) {
    Require(milliseconds == 500, L"Lifecycle request delay differs.");
    Sleep(lifecycleStartDelay);
}

LRESULT WINAPI LifecycleDispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    Require(message == WM_SYSCOMMAND && wParam == SC_MONITORPOWER && lParam == 2 &&
        IsWindow(window) && GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId(),
        L"Lifecycle request escaped its local window/thread.");
    lifecycleWindow = window;
    LifecycleEvent("dispatch-start");
    Sleep(lifecycleDispatchDelay);
    LifecycleEvent("dispatch-end");
    return 0;
}

int MonitorLifecycleChild(const std::wstring& mode, const wchar_t* path, const wchar_t* mutexName) {
    if (mode != L"normal" && mode != L"hold" && mode != L"stale" && mode != L"late-start" &&
        mode != L"delayed-overlap" && mode != L"blocked") return 2;
    lifecycleLog = path;
    lifecycleDispatchDelay = mode == L"hold" ? 1800 : mode == L"blocked" ? 10000 : 0;
    lifecycleStartDelay = mode == L"stale" ? 1600 : 500;
    try {
        LifecycleEvent("child-start");
        if (mode == L"late-start") Sleep(1600);
        if (mode == L"delayed-overlap") Sleep(750);
        int result = TurnOffMonitor(LifecycleDispatch, LifecycleDelay, mutexName);
        LifecycleEvent(lifecycleWindow ? IsWindow(lifecycleWindow) ? "window-alive" : "window-destroyed" : "window-none");
        LifecycleEvent(result == 0 ? "completed-ok" : "completed-error");
        return result;
    } catch (const Failure& error) {
        std::wcerr << error.message << L"\n";
        return 1;
    }
}

int wmain(int count, wchar_t** arguments) {
    if (count != 1) {
        if (count >= 2 && std::wstring(arguments[1]) == L"--off") {
            wchar_t output[32768]{};
            DWORD length = GetEnvironmentVariableW(L"OLED_OFF_TEST_LOG", output, ARRAYSIZE(output));
            if (!length || length >= ARRAYSIZE(output)) return 2;
            lifecycleLog = output;
            auto testMutex = L"Local\\OLED_OFF.LauncherTest." + lifecycleLog;
            // Backslashes are not valid inside a named kernel object's name.
            for (auto& character : testMutex) if (character == L'\\' || character == L'/') character = L'_';
            testMutex = L"Local\\" + testMutex;
            try {
                LifecycleEvent("helper-start");
                int result = RunOffCommand(count, arguments, LifecycleDispatch, testMutex.c_str());
                LifecycleEvent(result == 0 ? "completed-ok" : "completed-error");
                return result;
            } catch (...) { return 1; }
        }
        if (count == 5 && std::wstring(arguments[1]) == L"--monitor-lifecycle")
            return MonitorLifecycleChild(arguments[2], arguments[3], arguments[4]);
        return 2;
    }
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
        auto testMutex = L"Local\\OLED_OFF.Unit." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(MonitorClock::Now());
        Require(TurnOffMonitor(MockDispatch, MockDelay, testMutex.c_str(), MockNewProcessAge) == 0 && delayValue == 500 &&
            dispatchCount == 1 && !IsWindow(dispatchedWindow), L"Successful monitor request differs or its window remained.");
        mockSuccess = false;
        auto failedMutex = testMutex + L".failure";
        Require(TurnOffMonitor(MockDispatch, MockDelay, failedMutex.c_str(), MockNewProcessAge) == 1 && dispatchCount == 2 &&
            !IsWindow(dispatchedWindow), L"Failed monitor request exit code differs or it retried.");
        std::wcout << L"PASS: native key capture, F12/F10/custom shortcut save and update, invalid input, overwrite protection, legacy migration, monitor command and exit codes.\n";
        std::wcout << L"Monitor-off API was substituted; desktop shortcuts were not changed.\n";
    } catch (const Failure& error) {
        std::wcerr << error.message << L"\n";
        exitCode = 1;
    }
    CoUninitialize();
    return exitCode;
}
