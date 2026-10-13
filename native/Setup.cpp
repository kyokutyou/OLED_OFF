#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <iterator>
#include "resource.h"

using Microsoft::WRL::ComPtr;

struct Failure { std::wstring message; };

void Check(HRESULT result, const wchar_t* action) {
    if (FAILED(result)) {
        wchar_t code[24]{};
        swprintf_s(code, L" (0x%08lX)", static_cast<unsigned long>(result));
        throw Failure{std::wstring(action) + code};
    }
}

std::wstring ExecutablePath() {
    std::vector<wchar_t> buffer(32768);
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) throw Failure{L"EXEの場所を取得できません。"};
    return std::wstring(buffer.data(), length);
}

std::wstring ParentFolder(const std::wstring& path) {
    return path.substr(0, path.find_last_of(L"\\/"));
}

bool ValidHotkey(WORD hotkey) {
    BYTE key = LOBYTE(hotkey), modifiers = HIBYTE(hotkey);
    bool supported = (key >= VK_F1 && key <= VK_F12) ||
        (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9');
    return supported && (modifiers & (HOTKEYF_CONTROL | HOTKEYF_ALT)) &&
        !(modifiers & ~(HOTKEYF_CONTROL | HOTKEYF_ALT | HOTKEYF_SHIFT));
}

ComPtr<IShellLinkW> NewLink() {
    ComPtr<IShellLinkW> link;
    Check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(link.GetAddressOf())), L"ショートカットを作成できません。");
    return link;
}

// Only update this application's shortcut or its original PowerShell launcher.
bool OwnedLink(IShellLinkW* link, const std::wstring& executable) {
    std::vector<wchar_t> target(32768), arguments(32768);
    Check(link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH),
        L"既存の起動先を確認できません。");
    Check(link->GetArguments(arguments.data(), static_cast<int>(arguments.size())),
        L"既存の引数を確認できません。");
    if (_wcsicmp(target.data(), executable.c_str()) == 0 && std::wstring(arguments.data()) == L"--off")
        return true;
    wchar_t windows[32768]{};
    if (!GetWindowsDirectoryW(windows, ARRAYSIZE(windows))) return false;
    auto powershell = std::wstring(windows) + L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    auto original = L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
        ParentFolder(executable) + L"\\TurnOffMonitor.ps1\"";
    return _wcsicmp(target.data(), powershell.c_str()) == 0 && arguments.data() == original;
}

std::wstring ShortcutIconDirectory() {
    PWSTR local = nullptr;
    Check(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local), L"アイコンの保存先を取得できません。");
    std::wstring directory(local);
    CoTaskMemFree(local);
    return directory + L"\\OLED_OFF\\icons";
}

std::vector<char> ShortcutIconBytes() {
    auto module = GetModuleHandleW(nullptr);
    auto resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_SHORTCUT_ICON), RT_RCDATA);
    auto loaded = resource ? LoadResource(module, resource) : nullptr;
    auto data = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
    DWORD size = resource ? SizeofResource(module, resource) : 0;
    if (!data || !size) throw Failure{L"内蔵アイコンを読み込めません。"};
    return std::vector<char>(data, data + size);
}

// Change the location when the artwork changes, so Explorer cannot reuse the
// previous executable's cached icon. This hash identifies artwork, not security.
std::wstring IconFileName(const std::vector<char>& bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : bytes) { hash ^= byte; hash *= 1099511628211ULL; }
    wchar_t name[48]{};
    swprintf_s(name, L"oled-off-%016llx.ico", static_cast<unsigned long long>(hash));
    return name;
}

bool CurrentIconFile(const std::wstring& path, const std::vector<char>& bytes) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good()) return false;
    return std::vector<char>(std::istreambuf_iterator<char>(stream), {}) == bytes;
}

bool CurrentShortcutIcon(IShellLinkW* link, const std::wstring& directory) {
    auto bytes = ShortcutIconBytes();
    auto expected = directory + L"\\" + IconFileName(bytes);
    wchar_t path[32768]{};
    int index = -1;
    Check(link->GetIconLocation(path, ARRAYSIZE(path), &index), L"既存のアイコンを確認できません。");
    return index == 0 && _wcsicmp(path, expected.c_str()) == 0 && CurrentIconFile(expected, bytes);
}

std::wstring EnsureShortcutIcon(const std::wstring& directory) {
    auto bytes = ShortcutIconBytes();
    auto path = directory + L"\\" + IconFileName(bytes);
    if (CurrentIconFile(path, bytes)) return path;
    int result = SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS && result != ERROR_FILE_EXISTS)
        Check(HRESULT_FROM_WIN32(result), L"アイコンの保存先を作成できません。");
    auto temporary = path + L".tmp-" + std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) Check(HRESULT_FROM_WIN32(GetLastError()), L"アイコンを保存できません。");
    DWORD written = 0;
    BOOL success = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    DWORD error = success ? ERROR_WRITE_FAULT : GetLastError();
    CloseHandle(file);
    if (!success || written != bytes.size()) {
        DeleteFileW(temporary.c_str());
        Check(HRESULT_FROM_WIN32(error), L"アイコンを保存できません。");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = GetLastError();
        DeleteFileW(temporary.c_str());
        Check(HRESULT_FROM_WIN32(error), L"アイコンを保存できません。");
    }
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, path.c_str(), nullptr);
    return path;
}

void SaveShortcut(const std::wstring& destination, const std::wstring& executable, WORD hotkey,
                  const std::wstring& iconDirectory) {
    if (!ValidHotkey(hotkey)) throw Failure{L"Ctrl または Alt と、F1～F12・英字・数字を組み合わせてください。"};
    auto link = NewLink();
    ComPtr<IPersistFile> file;
    Check(link.As(&file), L"ショートカットを保存できません。");
    DWORD attributes = GetFileAttributesW(destination.c_str());
    DWORD lookupError = GetLastError();
    bool exists = attributes != INVALID_FILE_ATTRIBUTES;
    if (!exists && lookupError != ERROR_FILE_NOT_FOUND && lookupError != ERROR_PATH_NOT_FOUND)
        Check(HRESULT_FROM_WIN32(lookupError), L"保存先を確認できません。");
    if (exists) {
        Check(file->Load(destination.c_str(), STGM_READ), L"既存のショートカットを確認できません。");
        if (!OwnedLink(link.Get(), executable))
            throw Failure{L"同名の別のショートカットがあります。既存の「モニターを消灯」を移動または名前変更してから再実行してください。"};
    }
    Check(link->SetPath(executable.c_str()), L"起動先を設定できません。");
    Check(link->SetArguments(L"--off"), L"起動引数を設定できません。");
    Check(link->SetWorkingDirectory(ParentFolder(executable).c_str()), L"作業フォルダーを設定できません。");
    Check(link->SetDescription(L"OLED_OFF: モニターを消灯"), L"説明を設定できません。");
    auto icon = EnsureShortcutIcon(iconDirectory);
    Check(link->SetIconLocation(icon.c_str(), 0), L"アイコンを設定できません。");
    Check(link->SetShowCmd(SW_SHOWNORMAL), L"表示方法を設定できません。");
    Check(link->SetHotkey(hotkey), L"キーを設定できません。");
    Check(file->Save(destination.c_str(), TRUE), L"ショートカットを保存できません。");
    SHChangeNotify(exists ? SHCNE_UPDATEITEM : SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSH, destination.c_str(), nullptr);
}

std::wstring DesktopShortcut() {
    PWSTR desktop = nullptr;
    Check(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop), L"デスクトップの場所を取得できません。");
    std::wstring path(desktop);
    CoTaskMemFree(desktop);
    return path + L"\\モニターを消灯.lnk";
}

void RepairShortcut(const std::wstring& destination, const std::wstring& executable,
                    const std::wstring& iconDirectory) {
    auto link = NewLink();
    ComPtr<IPersistFile> file;
    Check(link.As(&file), L"既存のショートカットを確認できません。");
    Check(file->Load(destination.c_str(), STGM_READ), L"既存のショートカットを読み込めません。");
    if (!OwnedLink(link.Get(), executable)) throw Failure{L"このアプリのショートカットではありません。"};
    WORD key = 0;
    Check(link->GetHotkey(&key), L"登録済みのキーを確認できません。");
    SaveShortcut(destination, executable, key, iconDirectory);
}

int TurnOffMonitor(decltype(&SendMessageTimeoutW) send = &SendMessageTimeoutW,
                   decltype(&Sleep) delay = &Sleep) {
    delay(500);
    DWORD_PTR result = 0;
    SetLastError(0);
    return send(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2,
        SMTO_ABORTIFHUNG, 2000, &result) ? 0 : 1;
}

#include "SetupUI.h"

#ifndef OLED_OFF_TEST
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int count = 0;
    PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return 1;
    bool off = count == 2 && std::wstring(arguments[1]) == L"--off";
    bool setup = count == 1;
    bool repair = count == 2 && std::wstring(arguments[1]) == L"--repair-shortcut";
    LocalFree(arguments);
    if (off) return TurnOffMonitor();
    if (!setup && !repair) return 1;
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    int result = 1;
    try {
        if (repair) { RepairShortcut(DesktopShortcut(), ExecutablePath(), ShortcutIconDirectory()); result = 0; }
        else result = ShowSetup(instance, show);
    }
    catch (const Failure& error) {
        if (!repair) MessageBoxW(nullptr, error.message.c_str(), L"OLED_OFF - エラー", MB_OK | MB_ICONERROR);
    }
    CoUninitialize();
    return result;
}
#else
#include "../tests/Test-NativeSetup.cpp"
#endif
