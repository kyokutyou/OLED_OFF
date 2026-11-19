#define UNICODE
#define _UNICODE
#include "../native/MonitorRequest.h"
#include <iostream>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

void Require(bool condition, const char* message) {
    if (!condition) throw message;
}

int wmain(int count, wchar_t** arguments) {
    if (count != 2) return 2;
    std::wstring folder = arguments[1];
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return 2;
    const auto mutex = L"Local\\OLED_OFF.Coordinator.Tests." + std::to_wstring(GetCurrentProcessId());
    MonitorRequest::Identity identity{{1, 2, 3, {4, 5, 6, 7, 8, 9, 10, 11}}, 7};
    const ULONGLONG second = 10000000;
    try {
        MonitorRequest::Identity actual{};
        Require(MonitorRequest::CurrentIdentity(actual), "Cannot get current boot/session identity.");
        const auto normal = folder + L"\\normal.bin";
        {
            MonitorRequest::Coordinator first(mutex.c_str(), normal, identity);
            Require(first.Begin(100 * second, 100 * second) == MonitorRequest::Decision::Accepted,
                "First request was not accepted.");
            Require(first.Complete(101 * second), "Could not complete first request.");
        }
        {
            MonitorRequest::Coordinator delayed(mutex.c_str(), normal, identity);
            Require(delayed.Begin(100 * second + second / 2, 102 * second) == MonitorRequest::Decision::Dropped,
                "A sibling created before first completion was accepted later.");
        }
        {
            MonitorRequest::Coordinator cooldown(mutex.c_str(), normal, identity);
            Require(cooldown.Begin(101 * second + second / 2, 102 * second) == MonitorRequest::Decision::Dropped,
                "Completion shortened the reservation and allowed a late sibling.");
        }
        {
            MonitorRequest::Coordinator fresh(mutex.c_str(), normal, identity);
            Require(fresh.Begin(104 * second, 104 * second) == MonitorRequest::Decision::Accepted,
                "New request after the reservation was not accepted.");
            Require(fresh.Complete(104 * second + second / 2), "Could not complete new request.");
        }
        std::cout << "PASS: delayed overlapping requests and cooldown requests are discarded; later explicit requests work.\n";

        const auto crashed = folder + L"\\crashed.bin";
        {
            MonitorRequest::Coordinator unfinished(mutex.c_str(), crashed, identity);
            Require(unfinished.Begin(200 * second, 200 * second) == MonitorRequest::Decision::Accepted,
                "Crash fixture was not accepted.");
        }
        {
            MonitorRequest::Coordinator delayed(mutex.c_str(), crashed, identity);
            Require(delayed.Begin(201 * second, 205 * second) == MonitorRequest::Decision::Dropped,
                "Incomplete request lost its reservation.");
        }
        {
            MonitorRequest::Coordinator fresh(mutex.c_str(), crashed, identity);
            Require(fresh.Begin(204 * second, 205 * second) == MonitorRequest::Decision::Accepted,
                "New request after crash reservation could not proceed.");
            Require(fresh.Complete(205 * second), "Could not complete crash recovery request.");
        }
        std::cout << "PASS: unfinished requests keep a durable three-second rejection interval.\n";

        const auto contested = folder + L"\\contested.bin";
        {
            MonitorRequest::Coordinator active(mutex.c_str(), contested, identity);
            Require(active.Begin(300 * second, 300 * second) == MonitorRequest::Decision::Accepted,
                "Lock fixture was not accepted.");
            MonitorRequest::Decision concurrent = MonitorRequest::Decision::Error;
            std::thread contender([&]() {
                MonitorRequest::Coordinator other(mutex.c_str(), contested, identity);
                concurrent = other.Begin(300 * second, 300 * second);
            });
            contender.join();
            Require(concurrent == MonitorRequest::Decision::Dropped, "Concurrent request waited or was accepted.");
            Require(active.Complete(301 * second), "Could not complete lock fixture.");
        }
        std::cout << "PASS: mutex contention never queues requests.\n";

        {
            std::ofstream broken(normal, std::ios::binary | std::ios::trunc);
            broken << "broken";
        }
        {
            MonitorRequest::Coordinator corrupt(mutex.c_str(), normal, identity);
            Require(corrupt.Begin(400 * second, 400 * second) == MonitorRequest::Decision::Error,
                "Corrupt history was treated as an empty state.");
        }
        {
            MonitorRequest::Coordinator inaccessible(mutex.c_str(), folder + L"\\missing\\state.bin", identity);
            Require(inaccessible.Begin(400 * second, 400 * second) == MonitorRequest::Decision::Error,
                "Failed history write allowed a request.");
        }
        std::cout << "PASS: corrupt or unwritable history fails closed.\n";

        const auto checksumPath = folder + L"\\checksum.bin";
        {
            MonitorRequest::Coordinator valid(mutex.c_str(), checksumPath, identity);
            Require(valid.Begin(500 * second, 500 * second) == MonitorRequest::Decision::Accepted,
                "Checksum fixture was not accepted.");
            Require(valid.Complete(501 * second), "Could not complete checksum fixture.");
        }
        std::vector<char> bytes;
        {
            std::ifstream input(checksumPath, std::ios::binary);
            bytes = std::vector<char>(std::istreambuf_iterator<char>(input), {});
        }
        Require(!bytes.empty(), "Checksum fixture is empty.");
        bytes[bytes.size() / 2] ^= 1;
        {
            std::ofstream output(checksumPath, std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        {
            MonitorRequest::Coordinator corrupt(mutex.c_str(), checksumPath, identity);
            Require(corrupt.Begin(504 * second, 504 * second) == MonitorRequest::Decision::Error,
                "Changed history bytes passed integrity validation.");
        }
        std::cout << "PASS: same-size history corruption fails integrity validation.\n";

        auto rebooted = identity;
        rebooted.boot.Data1 = 99;
        {
            MonitorRequest::Coordinator newBoot(mutex.c_str(), crashed, rebooted);
            Require(newBoot.Begin(second, second) == MonitorRequest::Decision::Accepted,
                "History from a prior boot blocked a new boot.");
            Require(newBoot.Complete(2 * second), "Could not complete new boot request.");
        }
        std::cout << "PASS: boot identity prevents earlier-boot uptime from blocking new requests.\n";
        auto wrongSession = rebooted;
        ++wrongSession.session;
        {
            MonitorRequest::Coordinator wrong(mutex.c_str(), crashed, wrongSession);
            Require(wrong.Begin(5 * second, 5 * second) == MonitorRequest::Decision::Error,
                "A session-mismatched history file was accepted.");
        }
        std::cout << "PASS: session mismatches fail closed.\n";

        const auto resetClock = folder + L"\\reset-clock.bin";
        {
            MonitorRequest::Coordinator previous(mutex.c_str(), resetClock, identity);
            Require(previous.Begin(1000 * second, 1000 * second) == MonitorRequest::Decision::Accepted,
                "Counter-reset fixture was not accepted.");
            Require(previous.Complete(1001 * second), "Counter-reset fixture did not complete.");
        }
        {
            MonitorRequest::Coordinator reset(mutex.c_str(), resetClock, identity);
            Require(reset.Begin(10 * second, 10 * second) == MonitorRequest::Decision::Dropped,
                "Counter reset should discard the triggering request.");
        }
        {
            MonitorRequest::Coordinator old(mutex.c_str(), resetClock, identity);
            Require(old.Begin(11 * second, 14 * second) == MonitorRequest::Decision::Dropped,
                "Recovery revived a request from the reset interval.");
        }
        {
            MonitorRequest::Coordinator recovered(mutex.c_str(), resetClock, identity);
            Require(recovered.Begin(14 * second, 14 * second) == MonitorRequest::Decision::Accepted,
                "A valid future history blocked all new requests after the counter reset.");
        }
        std::cout << "PASS: inconsistent future history is safely rebased without reviving old requests.\n";
        return 0;
    } catch (const char* message) {
        std::cerr << message << '\n';
        return 1;
    }
}
