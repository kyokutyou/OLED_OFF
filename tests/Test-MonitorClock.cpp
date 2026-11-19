// Safe regression: substitute only the adjustable wall clock. The injected
// display routine records calls and never executes a display command.
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <cstdio>
#include <string>

static bool simulateRollback = false;
static unsigned dispatches = 0;

void WINAPI RegressionSystemTime(LPFILETIME value) {
    if (!simulateRollback) { GetSystemTimeAsFileTime(value); return; }
    FILETIME creation{}, exit{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    ULARGE_INTEGER adjusted{};
    adjusted.LowPart = creation.dwLowDateTime;
    adjusted.HighPart = creation.dwHighDateTime;
    adjusted.QuadPart += 100 * 10000ULL;
    value->dwLowDateTime = adjusted.LowPart;
    value->dwHighDateTime = adjusted.HighPart;
}

#define GetSystemTimeAsFileTime RegressionSystemTime
#define wWinMain UnusedRegressionWinMain
#include "../native/Setup.cpp"
#undef wWinMain
#undef GetSystemTimeAsFileTime

LRESULT WINAPI RegressionDispatch(HWND, UINT, WPARAM, LPARAM) {
    ++dispatches;
    return 0;
}

int main() {
    Sleep(2500);
    const auto mutex = L"Local\\OLED_OFF.ClockRegression." + std::to_wstring(GetCurrentProcessId());
    ULONGLONG age = 0;
    if (!MonitorOffProcessAge(age) || age < 1500 || TurnOffMonitor(RegressionDispatch, Sleep, mutex.c_str()) != 0)
        return 1;
    simulateRollback = true;
    if (!MonitorOffProcessAge(age)) return 1;
    int result = TurnOffMonitor(RegressionDispatch, Sleep, mutex.c_str());
    std::printf("Observed monotonic age=%llu ms; expired dispatches=%u; result=%d\n", age, dispatches, result);
    if (age < 1500 || dispatches != 0 || result != 0) {
        std::puts("FAIL: wall-clock rollback revived an expired request.");
        return 1;
    }
    std::puts("PASS: wall-clock rollback cannot revive expired requests.");
    return 0;
}
