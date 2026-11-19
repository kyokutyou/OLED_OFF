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
#include <cerrno>
#include "resource.h"
#include "MonitorClock.h"
#include "MonitorRequest.h"

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

constexpr const wchar_t* MonitorOffMutexName = MonitorRequest::MutexName;
constexpr ULONGLONG MonitorOffExpiry = 1500;
constexpr DWORD MonitorOffTimeout = 3000;

bool MonitorOffProcessAge(ULONGLONG& milliseconds) {
    MonitorClock::Creation creation;
    if (!MonitorClock::ReadCreation(GetCurrentProcess(), creation)) return false;
    auto now = MonitorClock::Now();
    if (now < creation.interruptTime) return false;
    milliseconds = (now - creation.interruptTime) / MonitorClock::TicksPerMillisecond;
    return true;
}

// The watchdog belongs to this one-shot process. It never retries or starts a
// helper process. A blocked power call must not leave an application running.
struct MonitorOffDeadline {
    ULONGLONG started;
    HANDLE stop = nullptr, thread = nullptr;
    explicit MonitorOffDeadline(ULONGLONG start) : started(start) {
        stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stop) thread = CreateThread(nullptr, 0, Watch, this, 0, nullptr);
    }
    static DWORD WINAPI Watch(void* parameter) {
        auto& deadline = *static_cast<MonitorOffDeadline*>(parameter);
        for (;;) {
            auto now = MonitorClock::Now();
            auto elapsed = now >= deadline.started ? now - deadline.started : ~0ULL;
            if (elapsed >= MonitorOffTimeout * MonitorClock::TicksPerMillisecond)
                TerminateProcess(GetCurrentProcess(), 1);
            // Recheck the biased clock after resume; a relative Windows wait
            // alone does not include time spent in system sleep.
            DWORD status = WaitForSingleObject(deadline.stop, 25);
            if (status == WAIT_OBJECT_0) return 0;
            if (status != WAIT_TIMEOUT) { TerminateProcess(GetCurrentProcess(), 1); return 1; }
        }
    }
    ~MonitorOffDeadline() {
        if (thread) {
            SetEvent(stop);
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
        if (stop) CloseHandle(stop);
    }
};

std::wstring MonitorOffStatePath(const wchar_t* mutexName, const MonitorRequest::Identity& identity) {
    std::wstring path;
    if (std::wstring(mutexName) == MonitorOffMutexName) {
        if (!MonitorRequest::DefaultStatePath(identity, path)) return {};
    } else {
        // Explicit non-production coordination names are used by isolated test
        // processes. Their files stay beside the test executable.
        std::uint64_t hash = 14695981039346656037ULL;
        for (const wchar_t* part = mutexName; *part; ++part) { hash ^= *part; hash *= 1099511628211ULL; }
        path = ParentFolder(ExecutablePath()) + L"\\request-state-" + std::to_wstring(hash) + L".bin";
    }
    return path;
}

struct MonitorOffWindow {
    HWND handle = nullptr;
    ~MonitorOffWindow() { if (handle) DestroyWindow(handle); }
};

int ExecuteMonitorOff(ULONGLONG started, HANDLE origin,
                      decltype(&DefWindowProcW) dispatch, decltype(&Sleep) delay,
                      const wchar_t* mutexName) {
    auto fresh = [&]() {
        auto now = MonitorClock::Now();
        return now >= started && now - started < MonitorOffExpiry * MonitorClock::TicksPerMillisecond &&
            WaitForSingleObject(origin, 0) == WAIT_TIMEOUT;
    };
    if (!fresh()) return 0;
    MonitorOffDeadline deadline(started);
    if (!deadline.thread) return 1;
    MonitorRequest::Identity identity;
    if (!MonitorRequest::CurrentIdentity(identity)) return 1;
    auto statePath = MonitorOffStatePath(mutexName, identity);
    if (statePath.empty()) return 1;
    MonitorRequest::Coordinator coordinator(mutexName, statePath, identity);
    auto decision = coordinator.Begin(started, MonitorClock::Now());
    if (decision != MonitorRequest::Decision::Accepted)
        return decision == MonitorRequest::Decision::Dropped ? 0 : 1;
    delay(500);
    if (!fresh()) return 0;

    // Process the system command synchronously on our own thread/window. No
    // other application's message queue can retain a request after we exit.
    MonitorOffWindow window;
    window.handle = CreateWindowExW(0, L"STATIC", L"OLED_OFF power request", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window.handle) return 1;
    if (!fresh()) return 0;
    LRESULT result = dispatch(window.handle, WM_SYSCOMMAND, SC_MONITORPOWER, 2);
    bool completed = coordinator.Complete(MonitorClock::Now());
    // WM_SYSCOMMAND returns zero when handled; this is not a physical display
    // state measurement. There is deliberately no retry on an error.
    return result == 0 && completed ? 0 : 1;
}

int TurnOffMonitor(decltype(&DefWindowProcW) dispatch = &DefWindowProcW,
                   decltype(&Sleep) delay = &Sleep,
                   const wchar_t* mutexName = MonitorOffMutexName,
                   decltype(&MonitorOffProcessAge) readAge = &MonitorOffProcessAge) {
    ULONGLONG started = 0;
    if (readAge == &MonitorOffProcessAge) {
        MonitorClock::Creation creation;
        if (!MonitorClock::ReadCreation(GetCurrentProcess(), creation)) return 1;
        started = creation.interruptTime;
    } else {
        // A deterministic request-age seam for unit tests; CLI paths always
        // use OS process metadata directly and never round creation time.
        const auto sampled = MonitorClock::Now();
        ULONGLONG age = 0;
        if (!readAge(age)) return 1;
        if (age >= MonitorOffExpiry || age * MonitorClock::TicksPerMillisecond > sampled) return 0;
        started = sampled - age * MonitorClock::TicksPerMillisecond;
    }
    return ExecuteMonitorOff(started, GetCurrentProcess(), dispatch, delay, mutexName);
}

bool MonitorOffUnsigned(const wchar_t* text, ULONGLONG& value) {
    if (!text || !*text) return false;
    for (const wchar_t* part = text; *part; ++part) if (*part < L'0' || *part > L'9') return false;
    errno = 0;
    wchar_t* end = nullptr;
    value = wcstoull(text, &end, 10);
    return errno != ERANGE && end && *end == 0;
}

int RunOffCommand(int count, wchar_t** arguments,
                  decltype(&DefWindowProcW) dispatch = &DefWindowProcW,
                  const wchar_t* mutexName = MonitorOffMutexName) {
    if (count == 2 && std::wstring(arguments[1]) == L"--off") return TurnOffMonitor(dispatch, &Sleep, mutexName);
    if (count != 6 || std::wstring(arguments[1]) != L"--off" ||
        std::wstring(arguments[2]) != L"--request-process" || std::wstring(arguments[4]) != L"--request-created") return 1;
    ULONGLONG processId = 0, expectedCreation = 0;
    if (!MonitorOffUnsigned(arguments[3], processId) || !processId || processId > MAXDWORD ||
        !MonitorOffUnsigned(arguments[5], expectedCreation) || !expectedCreation) return 1;
    HANDLE origin = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(processId));
    if (!origin) return 1;
    int result = 1;
    try {
        MonitorClock::Creation creation;
        if (MonitorClock::ReadCreation(origin, creation) && creation.fileTime == expectedCreation)
            result = ExecuteMonitorOff(creation.interruptTime, origin, dispatch, &Sleep, mutexName);
    } catch (...) { CloseHandle(origin); throw; }
    CloseHandle(origin);
    return result;
}

#include "SetupUI.h"

#ifndef OLED_OFF_TEST
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int count = 0;
    PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return 1;
    bool off = count >= 2 && std::wstring(arguments[1]) == L"--off";
    bool setup = count == 1;
    bool repair = count == 2 && std::wstring(arguments[1]) == L"--repair-shortcut";
    if (off) {
        int result = 1;
        try { result = RunOffCommand(count, arguments); } catch (...) { result = 1; }
        LocalFree(arguments);
        return result;
    }
    LocalFree(arguments);
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
