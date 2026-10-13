#pragma once
#include <algorithm>
#include <dwmapi.h>

namespace UI {
constexpr COLORREF Background = RGB(250, 250, 245);
constexpr COLORREF Surface = RGB(250, 250, 245);
constexpr COLORREF Border = RGB(224, 229, 218);
constexpr COLORREF Foreground = RGB(47, 60, 52);
constexpr COLORREF Muted = RGB(96, 112, 101);
constexpr COLORREF Accent = RGB(63, 87, 73);
constexpr COLORREF SoftGreen = RGB(236, 239, 231);
constexpr COLORREF Error = RGB(181, 71, 61);
constexpr WORD DefaultKey = MAKEWORD(VK_F12, HOTKEYF_CONTROL | HOTKEYF_SHIFT);
constexpr int Capture = 101, Reset = 102, Save = 103, Close = 104, Edit = 105, Desktop = 106;

struct State {
    HWND window = nullptr;
    HWND captureButton = nullptr, resetButton = nullptr, saveButton = nullptr;
    HWND closeButton = nullptr, editButton = nullptr, desktopButton = nullptr;
    HFONT body = nullptr, title = nullptr, keys = nullptr, smallFont = nullptr;
    HHOOK hook = nullptr;
    int dpi = 96;
    WORD selected = DefaultKey, saved = 0, pending = 0, beforeCapture = 0;
    bool recording = false, ctrl = false, alt = false, shift = false, win = false;
    bool leftWin = false, rightWin = false;
    bool complete = false, hasError = false, blocked = false, needsMigration = false;
    std::wstring feedback, destination, executable, iconDirectory;
    int Scale(int value) const { return MulDiv(value, dpi, 96); }
    RECT Rect(int x, int y, int w, int h) const {
        return RECT{Scale(x), Scale(y), Scale(x + w), Scale(y + h)};
    }
    ~State() {
        if (hook) UnhookWindowsHookEx(hook);
        for (HFONT font : {body, title, keys, smallFont}) if (font) DeleteObject(font);
    }
};

State* recordingState = nullptr;

std::wstring KeyName(WORD key) {
    BYTE code = LOBYTE(key);
    if (code >= VK_F1 && code <= VK_F12) return L"F" + std::to_wstring(code - VK_F1 + 1);
    return std::wstring(1, static_cast<wchar_t>(code));
}

std::vector<std::wstring> KeyParts(WORD key) {
    std::vector<std::wstring> parts;
    BYTE mods = HIBYTE(key);
    if (mods & HOTKEYF_CONTROL) parts.push_back(L"Ctrl");
    if (mods & HOTKEYF_ALT) parts.push_back(L"Alt");
    if (mods & HOTKEYF_SHIFT) parts.push_back(L"Shift");
    parts.push_back(KeyName(key));
    return parts;
}

void Refresh(State& state) {
    if (!state.window) return;
    ShowWindow(state.captureButton, state.complete ? SW_HIDE : SW_SHOW);
    ShowWindow(state.resetButton, state.complete ? SW_HIDE : SW_SHOW);
    ShowWindow(state.saveButton, state.complete ? SW_HIDE : SW_SHOW);
    ShowWindow(state.editButton, state.complete ? SW_SHOW : SW_HIDE);
    ShowWindow(state.desktopButton, state.blocked && !state.complete ? SW_SHOW : SW_HIDE);
    SetWindowTextW(state.captureButton, state.recording ? L"入力をキャンセル" : L"キーを変更");
    SetWindowTextW(state.saveButton, state.needsMigration ? L"設定を更新" : state.saved ? L"変更を保存" : L"ショートカットを作成");
    EnableWindow(state.captureButton, !state.blocked);
    EnableWindow(state.resetButton, !state.recording && !state.blocked);
    EnableWindow(state.saveButton, !state.recording && !state.blocked && ValidHotkey(state.selected) &&
        (!state.saved || state.selected != state.saved || state.needsMigration));
    InvalidateRect(state.window, nullptr, FALSE);
    for (HWND control : {state.captureButton, state.resetButton, state.saveButton, state.closeButton,
                         state.editButton, state.desktopButton}) InvalidateRect(control, nullptr, FALSE);
}

void EndCapture(State& state, bool accept) {
    if (accept && ValidHotkey(state.pending)) state.selected = state.pending;
    else state.selected = state.beforeCapture;
    state.recording = false;
    state.pending = 0;
    state.hasError = false;
    state.feedback.clear();
    if (state.hook) { UnhookWindowsHookEx(state.hook); state.hook = nullptr; }
    if (recordingState == &state) recordingState = nullptr;
    Refresh(state);
}

// Focused recording is the only time keys are intercepted. The hook is removed
// on completion, Escape, focus loss, cancellation, and window destruction.
void RecordKey(State& state, UINT code, bool down) {
    if (!state.recording) return;
    if (down && code == VK_ESCAPE) { EndCapture(state, false); return; }
    switch (code) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: state.ctrl = down; break;
    case VK_MENU: case VK_LMENU: case VK_RMENU: state.alt = down; break;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: state.shift = down; break;
    case VK_LWIN: case VK_RWIN:
        if (code == VK_LWIN) state.leftWin = down; else state.rightWin = down;
        state.win = state.leftWin || state.rightWin;
        if (down) {
            state.pending = 0;
            state.hasError = true;
            state.feedback = L"Windowsキーは使用できません。Ctrl または Alt と組み合わせてください。";
        }
        break;
    default:
        if (down) {
            BYTE modifiers = static_cast<BYTE>((state.ctrl ? HOTKEYF_CONTROL : 0) |
                (state.alt ? HOTKEYF_ALT : 0) | (state.shift ? HOTKEYF_SHIFT : 0));
            WORD candidate = MAKEWORD(static_cast<BYTE>(code), modifiers);
            if (!state.win && ValidHotkey(candidate)) {
                state.pending = candidate;
                state.selected = candidate;
                state.hasError = false;
                state.feedback = L"キーを離すと、この組み合わせを選択します。";
            } else {
                state.pending = 0;
                state.hasError = true;
                state.feedback = L"Ctrl または Alt と、F1～F12・英字・数字を組み合わせてください。";
            }
        }
        break;
    }
    if (!down && state.pending && !state.ctrl && !state.alt && !state.shift && !state.win)
        EndCapture(state, true);
    else Refresh(state);
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM message, LPARAM data) {
    auto state = recordingState;
    if (code == HC_ACTION && state && state->recording && GetForegroundWindow() == state->window) {
        auto event = reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
        bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        RecordKey(*state, event->vkCode, down);
        // Forward modifier releases, including keys already held before capture,
        // so Windows never retains a pressed modifier after recording ends.
        if (!down && (event->vkCode == VK_LCONTROL || event->vkCode == VK_RCONTROL ||
            event->vkCode == VK_LSHIFT || event->vkCode == VK_RSHIFT ||
            event->vkCode == VK_LMENU || event->vkCode == VK_RMENU ||
            event->vkCode == VK_LWIN || event->vkCode == VK_RWIN))
            return CallNextHookEx(nullptr, code, message, data);
        return 1;
    }
    return CallNextHookEx(nullptr, code, message, data);
}

void BeginCapture(State& state, decltype(&GetAsyncKeyState) readKey = &GetAsyncKeyState) {
    state.beforeCapture = state.selected;
    state.pending = 0;
    state.ctrl = (readKey(VK_CONTROL) & 0x8000) != 0;
    state.alt = (readKey(VK_MENU) & 0x8000) != 0;
    state.shift = (readKey(VK_SHIFT) & 0x8000) != 0;
    state.leftWin = (readKey(VK_LWIN) & 0x8000) != 0;
    state.rightWin = (readKey(VK_RWIN) & 0x8000) != 0;
    state.win = state.leftWin || state.rightWin;
    state.hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandleW(nullptr), 0);
    if (!state.hook) throw Failure{L"キー入力を開始できません。もう一度「キーを変更」を押してください。"};
    recordingState = &state;
    state.recording = true;
    state.hasError = false;
    state.feedback = L"登録したいキーを同時に押してください。Esc でキャンセルできます。";
    Refresh(state);
}

void LoadSettings(State& state) {
    DWORD attributes = GetFileAttributesW(state.destination.c_str());
    DWORD error = GetLastError();
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return;
        Check(HRESULT_FROM_WIN32(error), L"現在の設定を確認できません。");
    }
    auto link = NewLink();
    ComPtr<IPersistFile> file;
    Check(link.As(&file), L"現在の設定を確認できません。");
    Check(file->Load(state.destination.c_str(), STGM_READ), L"既存のショートカットを読み込めません。");
    if (!OwnedLink(link.Get(), state.executable)) {
        state.blocked = true;
        throw Failure{L"デスクトップに同名の別のショートカットがあります。名前を変更してから、このアプリを開き直してください。"};
    }
    WORD key = 0;
    Check(link->GetHotkey(&key), L"登録済みのキーを確認できません。");
    if (ValidHotkey(key)) state.selected = state.saved = key;
    wchar_t arguments[64]{};
    Check(link->GetArguments(arguments, ARRAYSIZE(arguments)), L"既存の起動方法を確認できません。");
    state.needsMigration = std::wstring(arguments) != L"--off" || !CurrentShortcutIcon(link.Get(), state.iconDirectory);
}

void Persist(State& state) {
    SaveShortcut(state.destination, state.executable, state.selected, state.iconDirectory);
    state.saved = state.selected;
    state.complete = true;
    state.needsMigration = false;
    state.hasError = false;
    state.feedback.clear();
    Refresh(state);
    SetFocus(state.closeButton);
}

void Fill(HDC dc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void Rounded(HDC dc, RECT rect, COLORREF fill, COLORREF stroke, int radius) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, stroke);
    auto oldBrush = SelectObject(dc, brush);
    auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
    DeleteObject(pen); DeleteObject(brush);
}

void Text(HDC dc, const State& state, const std::wstring& text, HFONT font, COLORREF color,
          int x, int y, int w, int h, UINT format = DT_LEFT | DT_TOP | DT_WORDBREAK) {
    auto previous = SelectObject(dc, font);
    SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
    RECT rect = state.Rect(x, y, w, h);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format | DT_NOPREFIX);
    SelectObject(dc, previous);
}

void PaintKeys(HDC dc, const State& state, int y) {
    auto parts = KeyParts(state.selected);
    int width = 0;
    for (auto& part : parts) width += part == L"Shift" ? 104 : 88;
    width += (static_cast<int>(parts.size()) - 1) * 28;
    int x = (680 - width) / 2;
    for (size_t index = 0; index < parts.size(); ++index) {
        int keyWidth = parts[index] == L"Shift" ? 104 : 88;
        Rounded(dc, state.Rect(x, y, keyWidth, 58), SoftGreen, state.recording ? Accent : Border, state.Scale(14));
        Text(dc, state, parts[index], state.keys, Foreground, x, y, keyWidth, 58, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        x += keyWidth;
        if (index + 1 < parts.size()) {
            Text(dc, state, L"+", state.body, Muted, x, y, 28, 58, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            x += 28;
        }
    }
}

void Paint(State& state, HDC dc) {
    RECT client{}; GetClientRect(state.window, &client);
    Fill(dc, client, Background);
    HICON appIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_OLED_OFF),
        IMAGE_ICON, state.Scale(42), state.Scale(42), 0));
    if (appIcon) {
        DrawIconEx(dc, state.Scale(32), state.Scale(30), appIcon, state.Scale(42), state.Scale(42), 0, nullptr, DI_NORMAL);
        DestroyIcon(appIcon);
    }
    Text(dc, state, L"OLED OFF", state.body, Foreground, 88, 35, 200, 30);
    Text(dc, state, state.blocked ? L"確認が必要" : state.saved ? L"設定済み" : L"未設定", state.smallFont,
        state.blocked ? Error : state.saved ? Accent : Muted,
        530, 38, 116, 24, DT_RIGHT | DT_SINGLELINE);
    if (state.complete)
        Text(dc, state, L"準備ができました。", state.title, Foreground, 32, 84, 616, 42);
    Text(dc, state, state.complete ? L"設定画面を閉じて、下のキーを押してみてください。" :
        L"好きなキーを選んで、デスクトップにショートカットを作成します。",
        state.body, Muted, 32, state.complete ? 134 : 106, 616, 32);
    Rounded(dc, state.Rect(32, 175, 616, 218), Surface, state.recording ? Accent : Border, state.Scale(20));
    Text(dc, state, state.complete ? L"登録したショートカット" : state.recording ? L"キーを入力中" : L"ショートカットキー",
        state.smallFont, state.recording || state.complete ? Accent : Muted, 56, 196, 568, 28);
    PaintKeys(dc, state, 241);
    if (state.complete) {
        Text(dc, state, L"デスクトップの「モニターを消灯」からも使えます。", state.body, Muted,
            56, 331, 568, 30, DT_CENTER | DT_SINGLELINE);
    }
    std::wstring feedback = state.feedback;
    if (feedback.empty()) feedback = state.complete ? L"マウスやキーボードを操作すると、画面が再び点灯します。" :
        state.needsMigration ? L"「設定を更新」で、現在のキーのままショートカットを更新できます。" :
        state.saved && state.selected == state.saved ? L"現在の設定です。「キーを変更」から別のキーを選べます。" :
        state.saved ? L"キーを選択しました。「変更を保存」で反映します。" :
        L"このキーでよければ、そのまま「ショートカットを作成」を押してください。";
    Text(dc, state, feedback, state.body, state.hasError ? Error : state.recording ? Accent : Muted,
        32, 415, state.blocked ? 426 : 616, 54);
    Text(dc, state, L"このEXEは同じ場所に置いてください。移動すると再設定が必要です。",
        state.smallFont, Muted, 32, 488, 616, 26);
    Fill(dc, state.Rect(32, 533, 616, 1), Border);
    Text(dc, state, L"常駐なし  /  PCのスリープ・ロックは行いません", state.smallFont, Muted,
        32, 615, 616, 26);
}

HWND Button(State& state, const wchar_t* text, int id, int x, int y, int w, int h) {
    HWND button = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        state.Scale(x), state.Scale(y), state.Scale(w), state.Scale(h), state.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (!button) throw Failure{L"ボタンを作成できません。"};
    SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(state.body), FALSE);
    return button;
}

void DrawButton(State& state, DRAWITEMSTRUCT& draw) {
    bool primary = draw.CtlID == Save || (draw.CtlID == Close && state.complete);
    bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    COLORREF fill = primary ? (disabled ? SoftGreen : pressed ? RGB(49, 68, 57) : Accent) : Surface;
    COLORREF text = disabled ? Muted : primary ? Surface : Foreground;
    Fill(draw.hDC, draw.rcItem, draw.CtlID == Capture || draw.CtlID == Reset ? Surface : Background);
    Rounded(draw.hDC, draw.rcItem, fill, primary ? fill : Border, state.Scale(12));
    wchar_t label[128]{}; GetWindowTextW(draw.hwndItem, label, ARRAYSIZE(label));
    auto previous = SelectObject(draw.hDC, state.body);
    SetBkMode(draw.hDC, TRANSPARENT); SetTextColor(draw.hDC, text);
    DrawTextW(draw.hDC, label, -1, &draw.rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(draw.hDC, previous);
    if (draw.itemState & ODS_FOCUS) {
        RECT focus = draw.rcItem;
        InflateRect(&focus, -state.Scale(4), -state.Scale(4));
        DrawFocusRect(draw.hDC, &focus);
    }
}

HFONT Font(const State& state, int size, int weight) {
    return CreateFontW(-state.Scale(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    try {
        switch (message) {
        case WM_CREATE:
            state->body = Font(*state, 16, FW_NORMAL);
            state->title = Font(*state, 30, FW_BOLD);
            state->keys = Font(*state, 23, FW_SEMIBOLD);
            state->smallFont = Font(*state, 13, FW_NORMAL);
            state->captureButton = Button(*state, L"キーを変更", Capture, 186, 327, 168, 42);
            state->resetButton = Button(*state, L"既定に戻す", Reset, 366, 327, 128, 42);
            state->closeButton = Button(*state, L"閉じる", Close, 32, 554, 128, 44);
            state->saveButton = Button(*state, L"ショートカットを作成", Save, 404, 554, 244, 44);
            state->editButton = Button(*state, L"設定を変更", Edit, 404, 554, 244, 44);
            state->desktopButton = Button(*state, L"デスクトップを開く", Desktop, 462, 415, 186, 42);
            try { LoadSettings(*state); }
            catch (const Failure& error) { state->hasError = state->blocked = true; state->feedback = error.message; }
            Refresh(*state);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{}; GetClientRect(window, &client);
            HDC memory = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
            auto old = SelectObject(memory, bitmap);
            Paint(*state, memory);
            BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
            SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_PRINTCLIENT: Paint(*state, reinterpret_cast<HDC>(wParam)); return 0;
        case WM_DRAWITEM: DrawButton(*state, *reinterpret_cast<DRAWITEMSTRUCT*>(lParam)); return TRUE;
        case WM_COMMAND:
            if (HIWORD(wParam) != BN_CLICKED) return 0;
            switch (LOWORD(wParam)) {
            case Capture: if (state->recording) EndCapture(*state, false); else BeginCapture(*state); break;
            case Reset:
                state->selected = DefaultKey; state->hasError = false; state->feedback.clear(); Refresh(*state); break;
            case Save:
                if (!state->recording && !state->blocked && IsWindowEnabled(state->saveButton)) Persist(*state);
                break;
            case Close: case IDCANCEL: DestroyWindow(window); break;
            case Edit: state->complete = false; Refresh(*state); SetFocus(state->captureButton); break;
            case Desktop: ShellExecuteW(window, L"open", ParentFolder(state->destination).c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && state->recording) EndCapture(*state, false);
            return 0;
        case WM_CLOSE: DestroyWindow(window); return 0;
        case WM_DESTROY:
            if (state->recording) EndCapture(*state, false);
            PostQuitMessage(0); return 0;
        }
    } catch (const Failure& error) {
        if (message == WM_CREATE) return -1;
        state->hasError = true; state->feedback = error.message; Refresh(*state);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HWND CreateSetupWindow(State& state, HINSTANCE instance) {
    WNDCLASSW type{};
    type.lpfnWndProc = WindowProc;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_OLED_OFF));
    type.lpszClassName = L"OledOffSetupV2";
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw Failure{L"設定画面を登録できません。"};
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT bounds{0, 0, state.Scale(680), state.Scale(658)};
    AdjustWindowRectEx(&bounds, style, FALSE, WS_EX_CONTROLPARENT);
    int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, type.lpszClassName, L"OLED OFF", style,
        work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
        width, height, nullptr, nullptr, instance, &state);
    if (!window) throw Failure{L"設定画面を開けません。"};
    BOOL dark = FALSE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    return window;
}
} // namespace UI

int ShowSetup(HINSTANCE instance, int show) {
    UI::State state;
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    state.dpi = (std::min)(static_cast<int>(GetDpiForSystem()),
        (std::min)(MulDiv(work.right - work.left, 96, 720), MulDiv(work.bottom - work.top, 96, 708)));
    state.destination = DesktopShortcut();
    state.executable = ExecutablePath();
    state.iconDirectory = ShortcutIconDirectory();
    HWND window = UI::CreateSetupWindow(state, instance);
    ShowWindow(window, show);
    SetFocus(state.captureButton);
    MSG message{};
    BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    return result == -1 ? 1 : static_cast<int>(message.wParam);
}
