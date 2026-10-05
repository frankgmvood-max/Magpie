#include "NoFocusLossController.h"
#include <memory>
#include <mutex>
#include <CommCtrl.h>

#if defined(_M_X64) || defined(_M_IX86)
#include "third_party/minhook/include/MinHook.h"
#endif

namespace Magpie {
namespace {

struct Session {
	Session(const NoFocusLossController* controller, HWND outputWindow, HWND sourceWindow,
		DWORD sourcePid, NoFocusLossMode requestedMode) noexcept :
		owner(controller), output(outputWindow), source(sourceWindow), sourceProcess(sourcePid), mode(requestedMode) {}
	const NoFocusLossController* owner;
	HWND output;
	HWND source;
	DWORD sourceProcess;
	NoFocusLossMode mode;
	mutable std::atomic<uint64_t> calls{0};
	mutable std::atomic<uint64_t> spoofed{0};
	mutable std::atomic<uint64_t> suppressedMessages{0};
};
std::atomic<std::shared_ptr<const Session>> currentSession;
#if defined(_M_X64) || defined(_M_IX86)
std::mutex hookMutex;
bool hookCreated = false;
constexpr UINT_PTR subclassId = 0x4E464C;
#endif

bool IsLive(const std::shared_ptr<const Session>& session) noexcept {
	if (!session) return false;
	DWORD outputProcess = 0;
	DWORD sourceProcess = 0;
	GetWindowThreadProcessId(session->output, &outputProcess);
	GetWindowThreadProcessId(session->source, &sourceProcess);
	return outputProcess == GetCurrentProcessId() &&
		sourceProcess != 0 && sourceProcess == session->sourceProcess &&
		IsWindowVisible(session->output) && IsWindowVisible(session->source) &&
		!IsIconic(session->output) && !IsIconic(session->source);
}

bool IsEligible(const std::shared_ptr<const Session>& session, HWND foreground) noexcept {
	if (!IsLive(session)) return false;
	DWORD foregroundProcess = 0;
	if (foreground) GetWindowThreadProcessId(foreground, &foregroundProcess);
	return ShouldSpoofForeground(session->mode, true, foregroundProcess != 0 &&
		foregroundProcess == session->sourceProcess, foreground == session->output);
}

bool IsDeactivation(UINT message, WPARAM wParam) noexcept {
	switch (message) {
	case WM_NCACTIVATE: case WM_ACTIVATEAPP: case WM_IME_SETCONTEXT:
		return !wParam;
	case WM_ACTIVATE:
		return LOWORD(wParam) == WA_INACTIVE;
	case WM_KILLFOCUS:
		return true;
	default:
		return false;
	}
}

bool ShouldSuppress(const std::shared_ptr<const Session>& session, UINT message, WPARAM wParam) noexcept {
	if (!IsDeactivation(message, wParam) || !session || session->mode == NoFocusLossMode::ForegroundOnly ||
		!IsEligible(session, GetActualForegroundWindow())) return false;
	session->suppressedMessages.fetch_add(1, std::memory_order_relaxed);
	return true;
}

#if defined(_M_X64) || defined(_M_IX86)
HWND WINAPI ForegroundDetour() noexcept {
	const HWND actual = GetActualForegroundWindow();
	const DWORD lastError = GetLastError();
	const auto session = currentSession.load(std::memory_order_acquire);
	if (session) session->calls.fetch_add(1, std::memory_order_relaxed);
	const HWND result = IsEligible(session, actual) &&
		currentSession.load(std::memory_order_acquire) == session ? session->output : actual;
	if (session && result == session->output && result != actual) session->spoofed.fetch_add(1, std::memory_order_relaxed);
	SetLastError(lastError);
	return result;
}

LRESULT CALLBACK OutputSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR id, DWORD_PTR) noexcept {
	if (message == WM_NCDESTROY) {
		RemoveWindowSubclass(hwnd, &OutputSubclass, id);
	} else if (IsDeactivation(message, wParam)) {
		const auto session = currentSession.load(std::memory_order_acquire);
		if (session && session->output == hwnd && ShouldSuppress(session, message, wParam)) return 0;
	}
	return DefSubclassProc(hwnd, message, wParam, lParam);
}
#endif

}

bool NoFocusLossController::Start(HWND output, HWND source, NoFocusLossSettings settings) noexcept {
	Stop();
	_lastError = 0;
	if (!settings.enabled) return false;
	DWORD sourceProcess = 0;
	DWORD outputProcess = 0;
	GetWindowThreadProcessId(source, &sourceProcess);
	GetWindowThreadProcessId(output, &outputProcess);
	if (!sourceProcess || outputProcess != GetCurrentProcessId() || output == source ||
		!IsWindow(source) || !IsWindow(output)) {
		_lastError = -1001;
		return false;
	}
	settings.mode = SanitizeNoFocusLossMode(uint32_t(settings.mode));
	if (settings.mode == NoFocusLossMode::ClassicCompatibility &&
		GetWindowThreadProcessId(output, nullptr) != GetCurrentThreadId()) {
		_lastError = -1006;
		return false;
	}
#if defined(_M_X64) || defined(_M_IX86)
	try {
		const std::lock_guard lock(hookMutex);
		if (currentSession.load(std::memory_order_acquire)) {
			_lastError = -1003;
			return false;
		}
		if (!hookCreated) {
			MH_STATUS status = MH_Initialize();
			if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
				_lastError = int(status);
				return false;
			}
			NoFocusLossDetail::ForegroundGetter original = nullptr;
			status = MH_CreateHook(reinterpret_cast<void*>(&::GetForegroundWindow),
				reinterpret_cast<void*>(&ForegroundDetour), reinterpret_cast<void**>(&original));
			if (status != MH_OK) { _lastError = int(status); return false; }
			NoFocusLossDetail::genuineForegroundGetter.store(original, std::memory_order_release);
			hookCreated = true;
		}
		const auto session = std::make_shared<const Session>(this, output, source, sourceProcess, settings.mode);
		const MH_STATUS status = MH_EnableHook(reinterpret_cast<void*>(&::GetForegroundWindow));
		if (status != MH_OK && status != MH_ERROR_ENABLED) {
			_lastError = int(status);
			return false;
		}
		if (settings.mode == NoFocusLossMode::ClassicCompatibility) {
			if (!SetWindowSubclass(output, &OutputSubclass, subclassId, 0)) {
				MH_DisableHook(reinterpret_cast<void*>(&::GetForegroundWindow));
				_lastError = -1005;
				return false;
			}
			_subclassWindow = output;
		}
		currentSession.store(session, std::memory_order_release);
		_armed = true;
		return true;
	} catch (...) {
		_lastError = -1004;
		return false;
	}
#else
	_lastError = -1002;
	return false;
#endif
}

void NoFocusLossController::Stop() noexcept {
	if (!_armed) return;
	// In-flight detours validate this snapshot again before returning a fake
	// HWND. Its trampoline stays allocated even after MH_DisableHook.
	currentSession.store(nullptr, std::memory_order_release);
	_armed = false;
#if defined(_M_X64) || defined(_M_IX86)
	if (_subclassWindow) {
		// The callback uses the published session, never a raw controller pointer.
		// If another subclass outlives us, our transparent callback stays safe.
		RemoveWindowSubclass(_subclassWindow, &OutputSubclass, subclassId);
		_subclassWindow = nullptr;
	}
	const std::lock_guard lock(hookMutex);
	const MH_STATUS status = MH_DisableHook(reinterpret_cast<void*>(&::GetForegroundWindow));
	if (status != MH_OK && status != MH_ERROR_DISABLED) _lastError = int(status);
#endif
}

bool NoFocusLossController::IsSpoofing() const noexcept {
	const auto session = currentSession.load(std::memory_order_acquire);
	return session && session->owner == this && IsEligible(session, GetActualForegroundWindow());
}

bool NoFocusLossController::SuppressMessage(UINT message, WPARAM wParam) const noexcept {
	if (!IsDeactivation(message, wParam)) return false;
	const auto session = currentSession.load(std::memory_order_acquire);
	return session && session->owner == this && ShouldSuppress(session, message, wParam);
}

NoFocusLossObservation NoFocusLossController::Observe() const noexcept {
	NoFocusLossObservation observation;
	const auto session = currentSession.load(std::memory_order_acquire);
	if (!session || session->owner != this) return observation;
	observation.actual = GetActualForegroundWindow();
	observation.perceived = ::GetForegroundWindow();
	observation.output = session->output;
	observation.shouldSpoof = IsEligible(session, observation.actual);
	observation.calls = session->calls.load(std::memory_order_relaxed);
	observation.spoofed = session->spoofed.load(std::memory_order_relaxed);
	observation.suppressedMessages = session->suppressedMessages.load(std::memory_order_relaxed);
#if defined(_M_X64) || defined(_M_IX86)
	DWORD_PTR data = 0;
	observation.subclassRegistered = _subclassWindow &&
		GetWindowSubclass(_subclassWindow, &OutputSubclass, subclassId, &data);
#endif
	return observation;
}

}
