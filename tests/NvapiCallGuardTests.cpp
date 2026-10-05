#include "include/NvapiCallGuard.h"
#include "ReflexController.h"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace Magpie;
using namespace std::chrono_literals;

static void Require(bool value, const char* message) {
	if (!value) throw std::runtime_error(message);
}

static void TestNativeException() {
	NvapiCallGuard guard;
	DWORD seh = 0;
	unsigned entered = 0;
	const int result = guard.Invoke([&] {
		++entered;
		RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
		return 0;
	}, -1, &seh);
	Require(result == -1 && seh == EXCEPTION_ACCESS_VIOLATION && guard.IsFaulted(),
		"real Windows exception must become a driver failure");
	Require(guard.FaultCode() == seh && guard.FaultAddress() && guard.FaultThread() == GetCurrentThreadId(),
		"fault must retain native exception code, address and calling thread");
	for (int retry = 0; retry < 100; ++retry) {
		Require(guard.Invoke([&] { ++entered; return 0; }, -1, &seh) == -1 && seh == 0,
			"faulted driver must reject further native calls without another exception");
	}
	Require(entered == 1, "faulted callback must never be re-entered");
}

static void TestNoGlobalCallLock() {
	NvapiCallGuard guard;
	std::promise<void> entered, release;
	auto ready = entered.get_future();
	auto wake = release.get_future().share();
	auto sleeper = std::async(std::launch::async, [&] {
		DWORD seh = 0;
		return guard.Invoke([&] { entered.set_value(); wake.wait_for(3s); return 7; }, -1, &seh);
	});
	Require(ready.wait_for(1s) == std::future_status::ready, "native Sleep did not start");
	auto present = std::async(std::launch::async, [&] {
		DWORD seh = 0;
		return guard.Invoke([] { return 9; }, -1, &seh);
	});
	const bool progressed = present.wait_for(1s) == std::future_status::ready;
	release.set_value();
	Require(sleeper.get() == 7 && present.get() == 9 && progressed,
		"a sleeping native call must not hold a lock needed by Present");
}

class FaultingDriver final : public ReflexDriver {
public:
	NvapiCallGuard guard;
	unsigned nativeCalls = 0, failures = 0;
	ReflexConfigurationResult Configure(ReflexSettings value) noexcept override {
		DWORD seh = 0;
		const int status = guard.Invoke([&] { ++nativeCalls; return 0; }, -1, &seh);
		return {status,0,status == 0,value.lowLatency};
	}
	int Sleep() noexcept override {
		DWORD seh = 0;
		return guard.Invoke([&] {
			++nativeCalls; RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr); return 0;
		}, -1, &seh);
	}
	int Marker(ReflexMarker, uint64_t) noexcept override { return 0; }
	int RegisterGenerationQueue(ID3D12CommandQueue*) noexcept override { return 0; }
	int Generation(ID3D12CommandQueue*, uint64_t, uint64_t, bool) noexcept override { return 0; }
	int FrontendRender(uint64_t, uint64_t, bool) noexcept override { return 0; }
	int Present(uint64_t, uint64_t, bool, bool) noexcept override { return 0; }
	void ReportFailure(const char*, int) noexcept override { ++failures; }
};

static void TestFailedCleanupNeverClaimsCapCleared() {
	ReflexController reflex;
	auto fake = std::make_unique<FaultingDriver>();
	auto* driver = fake.get();
	reflex.Initialize(std::move(fake), {.minimumIntervalUs = 14706});
	Require(!reflex.BeginCapture(), "native Sleep exception must reject the capture");
	Require(driver->nativeCalls == 2 && driver->failures == 2,
		"cleanup must report failure without re-entering the faulted native driver");
	Require(reflex.CaptureBlocked() && !reflex.CanUseAsync() &&
		reflex.PacingState() == ReflexPacingState::CleanupFailed,
		"uncertain driver cap must block unsafe fallback after native exception");
	reflex.Stop();
	Require(driver->nativeCalls == 2, "stop must not retry a faulted native runtime");
}

int main() {
	try {
		TestNativeException();
		TestNoGlobalCallLock();
		TestFailedCleanupNeverClaimsCapCleared();
		std::cout << "PASS: real Windows SEH containment, native fault latch, concurrent Sleep/Present and cap cleanup safety\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
