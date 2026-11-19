#pragma once
#include <windows.h>
#include <realtimeapiset.h>
#include <cstddef>
#include <cstring>
#include <vector>

// All durations and request identities use the same boot-relative clock.
// UTC is used only as an opaque process identity supplied by the PS launcher.
namespace MonitorClock {
constexpr ULONGLONG TicksPerMillisecond = 10000;

inline ULONGLONG Now() {
    ULONGLONG ticks = 0;
    QueryInterruptTimePrecise(&ticks);
    return ticks;
}

struct Creation {
    ULONGLONG interruptTime = 0;
    ULONGLONG fileTime = 0;
};

inline bool ReadCreation(HANDLE process, Creation& creation) {
    // Microsoft documents class 64, but requires dynamic linking and permits
    // changes to this interface. Reject the request if its layout is unusable.
    struct Telemetry {
        ULONG headerSize, processId;
        ULONGLONG startKey, createTime, createInterruptTime, createUnbiasedTime;
        ULONGLONG sequenceNumber, sessionCreateTime;
        ULONG sessionId, bootId, imageChecksum, imageTimestamp;
        ULONG sidOffset, imageOffset, packageOffset, appOffset, commandOffset;
    };
    static_assert(sizeof(Telemetry) == 96 && offsetof(Telemetry, createInterruptTime) == 24,
        "Process telemetry layout differs.");
    using QueryProcess = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    auto module = GetModuleHandleW(L"ntdll.dll");
    auto address = module ? GetProcAddress(module, "NtQueryInformationProcess") : nullptr;
    QueryProcess query = nullptr;
    static_assert(sizeof(query) == sizeof(address), "Function pointer size differs.");
    std::memcpy(&query, &address, sizeof(query));
    const DWORD processId = GetProcessId(process);
    if (!query || !processId) return false;

    constexpr ULONG maximum = 64 * 1024;
    std::vector<unsigned char> buffer(4096);
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        ULONG returned = 0;
        LONG status = query(process, 64, buffer.data(), static_cast<ULONG>(buffer.size()), &returned);
        if (status != 0) {
            if (returned <= buffer.size() || returned > maximum) return false;
            buffer.resize(returned);
            continue;
        }
        if (returned < sizeof(Telemetry) || returned > buffer.size()) return false;
        Telemetry header{};
        std::memcpy(&header, buffer.data(), sizeof(header));
        if (header.headerSize < sizeof(Telemetry) || header.headerSize > returned ||
            header.processId != processId || !header.createTime || !header.createInterruptTime ||
            header.createInterruptTime > Now()) return false;
        creation.interruptTime = header.createInterruptTime;
        creation.fileTime = header.createTime;
        return true;
    }
    return false;
}
} // namespace MonitorClock
