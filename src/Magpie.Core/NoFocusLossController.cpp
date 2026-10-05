#include "NoFocusLossController.h"
#include <memory>
#include <mutex>

#if defined(_M_X64) || defined(_M_IX86)
#include "third_party/minhook/include/MinHook.h"
#endif

namespace Magpie {
namespace {

struct Session {
	const NoFocusLossController* owner;
	HWND output;
	HWND source;
	DWORD sourceProcess;
	NoFocusLossMode mode;
};
std::atomic<std::shared_ptr<const Session>> currentSession;
std::mutex hookMutex;
bool hookCreated = false;

bool IsEligible(const std::shared_ptr<const Session>& session, HWND foreground) noexcept {
	if (!session) return false;
	DWORD outputProcess = 0;
	DWORD sourceProcess = 0;
	GetWindowThreadProcessId(session->output, &outputProcess);
	GetWindowThreadProcessId(session->source, &sourceProcess);
	const bool valid = outputProcess == GetCurrentProcessId() &&
		sourceProcess != 0 && sourceProcess == session->sourceProcess &&
		IsWindowVisible(session->output) && IsWindowVisible(session->source) &&
		!IsIconic(session->output) && !IsIconic(session->source);
	DWORD foregroundProcess = 0;
	if (foreground) GetWindowThreadProcessId(foreground, &foregroundProcess);
	return ShouldSpoofForeground(valid, foregroundProcess != 0 &&
		foregroundProcess == session->sourceProcess, foreground == session->output);
}

HWND WINAPI ForegroundDetour() noexcept {
	const HWND actual = GetActualForegroundWindow();
	const DWORD lastError = GetLastError();
	const auto session = currentSession.load(std::memory_order_acquire);
	const HWND result = IsEligible(session, actual) &&
		currentSession.load(std::memory_order_acquire) == session ? session->output : actual;
	SetLastError(lastError);
	return result;
}

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
		const auto session = std::make_shared<const Session>(Session{
			this, output, source, sourceProcess, SanitizeNoFocusLossMode(uint32_t(settings.mode)) });
		const MH_STATUS status = MH_EnableHook(reinterpret_cast<void*>(&::GetForegroundWindow));
		if (status != MH_OK && status != MH_ERROR_ENABLED) {
			_lastError = int(status);
			return false;
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
	const auto session = currentSession.load(std::memory_order_acquire);
	if (!session || session->owner != this || session->mode != NoFocusLossMode::Compatibility ||
		!IsEligible(session, GetActualForegroundWindow())) return false;
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

}
