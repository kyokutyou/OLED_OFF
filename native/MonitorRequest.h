#pragma once
#include <windows.h>
#include <shlobj.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

// A file remembers completed requests without leaving a resident process. All
// timestamps are boot-relative interrupt time in 100 ns units, never UTC.
namespace MonitorRequest {
constexpr ULONGLONG ReservationTicks = 3000ULL * 10000;
constexpr wchar_t MutexName[] = L"Local\\OLED_OFF.MonitorOff.v3";

struct Identity {
    GUID boot{};
    DWORD session = 0;
};

inline bool SameBoot(const GUID& left, const GUID& right) {
    return std::memcmp(&left, &right, sizeof(GUID)) == 0;
}

inline bool CurrentIdentity(Identity& identity) {
    // The native query is dynamically resolved and failure is closed. There
    // is deliberately no estimate based on an adjustable wall clock.
    using QuerySystem = LONG(NTAPI*)(ULONG, void*, ULONG, ULONG*);
    auto module = GetModuleHandleW(L"ntdll.dll");
    auto address = module ? GetProcAddress(module, "NtQuerySystemInformation") : nullptr;
    QuerySystem query = nullptr;
    static_assert(sizeof(query) == sizeof(address), "Function pointer size differs.");
    std::memcpy(&query, &address, sizeof(query));
    if (!query) return false;
    struct BootEnvironment {
        GUID identifier;
        ULONG firmwareType;
        ULONG reserved;
        ULONGLONG flags;
    } environment{};
    static_assert(sizeof(BootEnvironment) == 32, "Boot information layout differs.");
    ULONG returned = 0;
    if (query(90, &environment, sizeof(environment), &returned) < 0 || returned < sizeof(GUID)) return false;
    GUID empty{};
    if (SameBoot(environment.identifier, empty) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &identity.session)) return false;
    identity.boot = environment.identifier;
    return true;
}

inline bool DefaultStatePath(const Identity& identity, std::wstring& path) {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return false;
    std::wstring directory = std::wstring(local) + L"\\OLED_OFF";
    CoTaskMemFree(local);
    int created = SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS && created != ERROR_FILE_EXISTS) return false;
    // Local mutexes are session scoped, so separate files must be as well.
    path = directory + L"\\request-state-v3-" + std::to_wstring(identity.session) + L".bin";
    return true;
}

enum class Decision { Accepted, Dropped, Error };

class Coordinator {
    struct Record {
        std::uint32_t magic = 0x4F4C4544;
        std::uint32_t version = 3;
        GUID boot{};
        DWORD session = 0;
        DWORD reserved = 0;
        ULONGLONG rejectThrough = 0;
        std::uint64_t checksum = 0;
    } record_;
    static_assert(sizeof(Record) == 48 && offsetof(Record, checksum) == 40,
        "Request state layout differs.");
    HANDLE mutex_ = nullptr;
    DWORD lockStatus_ = WAIT_FAILED;
    std::wstring path_;
    Identity identity_;
    ULONGLONG began_ = 0;
    bool accepted_ = false;

    static std::uint64_t Checksum(const Record& record) {
        auto bytes = reinterpret_cast<const unsigned char*>(&record);
        std::uint64_t hash = 14695981039346656037ULL;
        for (std::size_t index = 0; index < offsetof(Record, checksum); ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    bool Read(bool& exists) {
        exists = false;
        HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        exists = true;
        LARGE_INTEGER size{};
        DWORD read = 0;
        bool valid = GetFileSizeEx(file, &size) && size.QuadPart == sizeof(Record) &&
            ReadFile(file, &record_, sizeof(record_), &read, nullptr) && read == sizeof(record_);
        CloseHandle(file);
        return valid && record_.magic == 0x4F4C4544 && record_.version == 3 && record_.reserved == 0 &&
            record_.checksum == Checksum(record_);
    }

    bool Store() {
        record_.checksum = Checksum(record_);
        auto temporary = path_ + L".tmp-" + std::to_wstring(GetCurrentProcessId());
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        bool saved = WriteFile(file, &record_, sizeof(record_), &written, nullptr) &&
            written == sizeof(record_) && FlushFileBuffers(file);
        CloseHandle(file);
        if (saved) saved = MoveFileExW(temporary.c_str(), path_.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!saved) DeleteFileW(temporary.c_str());
        return saved;
    }

    bool OwnsLock() const { return lockStatus_ == WAIT_OBJECT_0 || lockStatus_ == WAIT_ABANDONED; }

public:
    Coordinator(const wchar_t* mutexName, const std::wstring& statePath, const Identity& identity)
        : mutex_(CreateMutexW(nullptr, FALSE, mutexName)),
          lockStatus_(mutex_ ? WaitForSingleObject(mutex_, 0) : WAIT_FAILED),
          path_(statePath), identity_(identity) {}
    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;
    ~Coordinator() {
        // No automatic completion: termination or an error leaves the durable
        // reservation in place, so a delayed sibling cannot become a retry.
        if (OwnsLock()) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }

    Decision Begin(ULONGLONG requestCreated, ULONGLONG now) {
        if (!OwnsLock()) return lockStatus_ == WAIT_TIMEOUT ? Decision::Dropped : Decision::Error;
        if (accepted_ || requestCreated > now || now > (std::numeric_limits<ULONGLONG>::max)() - ReservationTicks)
            return Decision::Error;
        bool exists = false;
        if (!Read(exists)) return Decision::Error;
        if (exists && SameBoot(record_.boot, identity_.boot)) {
            if (record_.session != identity_.session) return Decision::Error;
            // A persisted counter can belong to an earlier interrupt-time
            // epoch even if Windows retains the boot identity. Such a value
            // cannot be produced by a concurrent valid three-second lease.
            // Rebase safely, discard this request, and retain a full quiet
            // interval so pre-recovery requests never become a delayed off.
            if (record_.rejectThrough > now + ReservationTicks) {
                record_.rejectThrough = now + ReservationTicks;
                if (!Store()) return Decision::Error;
                return Decision::Dropped;
            }
            if (requestCreated <= record_.rejectThrough) return Decision::Dropped;
        }
        record_ = Record{};
        record_.boot = identity_.boot;
        record_.session = identity_.session;
        record_.rejectThrough = now + ReservationTicks;
        if (!Store()) return Decision::Error;
        began_ = now;
        accepted_ = true;
        return Decision::Accepted;
    }

    bool Complete(ULONGLONG now) {
        if (!OwnsLock() || !accepted_ || now < began_) return false;
        // Keep the full reservation even after normal completion. Shrinking
        // it before flush/unlock/process exit would open another delayed-
        // sibling race. There is no disk I/O after the display command.
        accepted_ = false;
        return true;
    }
};
} // namespace MonitorRequest
