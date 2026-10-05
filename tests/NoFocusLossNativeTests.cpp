#include "NoFocusLossController.h"
#include <CommCtrl.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Magpie;
namespace {
WNDPROC originalOutputProcedure = nullptr;
unsigned deactivationReachedOldHandler = 0;
unsigned inputReachedOldHandler = 0;
NoFocusLossController* stopDuringDestroy = nullptr;
LRESULT CALLBACK ExistingOutputProcedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
	if (message == WM_ACTIVATEAPP && !wParam) ++deactivationReachedOldHandler;
	if (message == WM_KEYDOWN) ++inputReachedOldHandler;
	if (message == WM_DESTROY && stopDuringDestroy) stopDuringDestroy->Stop();
	return CallWindowProcW(originalOutputProcedure, hwnd, message, wParam, lParam);
}
LRESULT CALLBACK OtherSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
	return DefSubclassProc(hwnd, message, wParam, lParam);
}
void Check(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}
struct Handle {
	HANDLE value = nullptr;
	~Handle() { if (value) CloseHandle(value); }
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;
	explicit Handle(HANDLE h = nullptr) : value(h) {}
};
struct Window {
	HWND value = nullptr;
	~Window() { if (value && IsWindow(value)) DestroyWindow(value); }
	explicit Window(const wchar_t* title) {
		value = CreateWindowExW(0, L"STATIC", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
			20, 20, 400, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
		Check(value != nullptr, "CreateWindowExW failed");
	}
};
void Pump() {
	MSG message{};
	while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
}
void Focus(HWND target) {
	// Runner-only test plumbing. Production NoFocusLoss never attaches input
	// queues or calls SetForegroundWindow, SetFocus or SetCursorPos.
	const DWORD current = GetCurrentThreadId();
	const HWND foreground = GetActualForegroundWindow();
	const DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
	const bool attached = foregroundThread && foregroundThread != current &&
		AttachThreadInput(current, foregroundThread, TRUE);
	ShowWindow(target, SW_RESTORE);
	BringWindowToTop(target);
	SetForegroundWindow(target);
	if (attached) AttachThreadInput(current, foregroundThread, FALSE);
	for (int i = 0; i != 100 && GetActualForegroundWindow() != target; ++i) {
		Pump();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	Check(GetActualForegroundWindow() == target, "Could not focus test window on the Windows runner");
}
int RunSource(const wchar_t* name) {
	Handle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name));
	Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, (std::wstring(name) + L"-Ready").c_str()));
	Handle stop(OpenEventW(SYNCHRONIZE, FALSE, (std::wstring(name) + L"-Stop").c_str()));
	Check(mapping.value && ready.value && stop.value, "Source IPC open failed");
	void* view = MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(UINT_PTR));
	Check(view != nullptr, "Source IPC mapping failed");
	Window source(L"NoFocusLoss test game (separate process)");
	*static_cast<UINT_PTR*>(view) = reinterpret_cast<UINT_PTR>(source.value);
	SetEvent(ready.value);
	for (;;) {
		const DWORD result = MsgWaitForMultipleObjects(1, &stop.value, FALSE, 15000, QS_ALLINPUT);
		if (result == WAIT_OBJECT_0) break;
		if (result == WAIT_TIMEOUT) continue;
		Check(result == WAIT_OBJECT_0 + 1, "Source IPC wait failed");
		Pump();
	}
	UnmapViewOfFile(view);
	return 0;
}
struct SourceProcess {
	std::wstring name = L"Local\\Magpie-NoFocusLoss-Test-" + std::to_wstring(GetCurrentProcessId());
	Handle mapping{ CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(UINT_PTR), name.c_str()) };
	Handle ready{ CreateEventW(nullptr, TRUE, FALSE, (name + L"-Ready").c_str()) };
	Handle stop{ CreateEventW(nullptr, TRUE, FALSE, (name + L"-Stop").c_str()) };
	Handle process;
	HWND window = nullptr;
	SourceProcess() {
		Check(mapping.value && ready.value && stop.value, "Parent IPC create failed");
		wchar_t executable[32768]{};
		Check(GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable))) != 0, "Executable path failed");
		std::wstring command = L"\"" + std::wstring(executable) + L"\" --source-window \"" + name + L"\"";
		STARTUPINFOW startup{ sizeof(startup) };
		PROCESS_INFORMATION info{};
		Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info), "Source process launch failed");
		process.value = info.hProcess;
		CloseHandle(info.hThread);
		AllowSetForegroundWindow(info.dwProcessId);
		Check(WaitForSingleObject(ready.value, 10000) == WAIT_OBJECT_0, "Source process did not create its window");
		const void* view = MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, sizeof(UINT_PTR));
		Check(view != nullptr, "Parent IPC mapping failed");
		window = reinterpret_cast<HWND>(*static_cast<const UINT_PTR*>(view));
		UnmapViewOfFile(view);
		Check(IsWindow(window), "Source process returned an invalid window");
	}
	void Close() {
		if (!process.value) return;
		SetEvent(stop.value);
		const DWORD result = WaitForSingleObject(process.value, 10000);
		if (result != WAIT_OBJECT_0) TerminateProcess(process.value, 99);
		CloseHandle(process.value);
		process.value = nullptr;
		Check(result == WAIT_OBJECT_0, "Source process did not close normally");
	}
	~SourceProcess() {
		if (process.value) {
			SetEvent(stop.value);
			if (WaitForSingleObject(process.value, 10000) != WAIT_OBJECT_0) TerminateProcess(process.value, 99);
		}
	}
};
struct Readers {
	std::atomic<bool> running{true};
	std::atomic<bool> invalid{false};
	std::atomic<unsigned> calls{0};
	std::vector<std::thread> threads;
	Readers(HWND source, HWND output) {
		for (int i = 0; i != 4; ++i) threads.emplace_back([this, source, output] {
			while (running.load(std::memory_order_relaxed)) {
				const HWND actual = GetActualForegroundWindow();
				const HWND seen = GetForegroundWindow();
				if (actual != source || (seen != source && seen != output)) invalid.store(true);
				calls.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}
	void Join() {
		running.store(false);
		for (auto& thread : threads) if (thread.joinable()) thread.join();
	}
	~Readers() { Join(); }
};
}

int wmain(int argc, wchar_t** argv) {
	try {
		if (argc == 3 && std::wstring(argv[1]) == L"--source-window") return RunSource(argv[2]);
		SourceProcess source;
		Window output(L"NoFocusLoss scaling output");
		originalOutputProcedure = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(output.value, GWLP_WNDPROC,
			reinterpret_cast<LONG_PTR>(&ExistingOutputProcedure)));
		Check(originalOutputProcedure != nullptr, "Existing output procedure installation failed");
		Window settings(L"NoFocusLoss settings/input host");
		NoFocusLossController controller;
		NoFocusLossSettings options;
		options.enabled = false;
		Check(!controller.Start(output.value, source.window, options) && !controller.IsArmed(), "Disabled setting armed hook");
		options.enabled = true;
		Check(!controller.Start(output.value, output.value, options), "Same output/source accepted");
		Check(!controller.Start(nullptr, source.window, options), "Invalid output accepted");
		Check(!controller.Start(output.value, nullptr, options), "Invalid source accepted");
		Check(!controller.Start(source.window, output.value, options), "Foreign-process output accepted");
		for (const auto mode : {NoFocusLossMode::ForegroundOnly, NoFocusLossMode::Compatibility, NoFocusLossMode::ClassicCompatibility}) {
			options.mode = mode;
			Focus(source.window);
			Check(controller.Start(output.value, source.window, options), "Native hook start failed");
			Check(controller.IsArmed() && controller.IsSpoofing(), "Valid source did not arm/spoof");
			Check(GetForegroundWindow() == output.value, "USER32 foreground was not spoofed");
			Check(GetActualForegroundWindow() == source.window, "Internal Magpie focus was spoofed");
			const bool compatibility = mode != NoFocusLossMode::ForegroundOnly;
			const bool classic = mode == NoFocusLossMode::ClassicCompatibility;
			const auto initial = controller.Observe();
			Check(initial.actual == source.window && initial.perceived == output.value && initial.shouldSpoof &&
				initial.subclassRegistered == classic && initial.calls > 0 && initial.spoofed > 0, "USER32 observation mismatch");
			const unsigned oldDeactivation = deactivationReachedOldHandler;
			SendMessageW(output.value, WM_ACTIVATEAPP, FALSE, 0);
			Check(deactivationReachedOldHandler == oldDeactivation + (classic ? 0u : 1u), "Deactivation reached old procedure before classic filter");
			const unsigned oldInput = inputReachedOldHandler;
			SendMessageW(output.value, WM_KEYDOWN, 'W', 0);
			Check(inputReachedOldHandler == oldInput + 1, "Outer window subclass blocked keyboard input");
			for (const UINT message : {UINT(WM_NCACTIVATE), UINT(WM_ACTIVATEAPP), UINT(WM_IME_SETCONTEXT), UINT(WM_ACTIVATE), UINT(WM_KILLFOCUS)}) {
				Check(controller.SuppressMessage(message, 0) == compatibility, "Deactivation policy mismatch");
			}
			for (const UINT message : {UINT(WM_NCACTIVATE), UINT(WM_ACTIVATEAPP), UINT(WM_IME_SETCONTEXT), UINT(WM_ACTIVATE)}) {
				Check(!controller.SuppressMessage(message, 1), "Activation was suppressed");
			}
			Check(!controller.SuppressMessage(WM_LBUTTONDOWN, 0) && !controller.SuppressMessage(WM_KEYDOWN, 'W'), "Game input message suppressed");
			Check(!controller.SuppressMessage(WM_DESTROY, 0), "Destruction message suppressed");
			POINT cursor{};
			Check(GetCursorPos(&cursor), "Cursor position query failed");
			const int x = cursor.x == 20 ? 21 : 20;
			Check(SetCursorPos(x, 20), "SetCursorPos failed while spoofing");
			POINT moved{};
			Check(GetCursorPos(&moved) && moved.x == x && moved.y == 20, "Cursor warp blocked by NoFocusLoss");
			SetCursorPos(cursor.x, cursor.y);
			NoFocusLossController second;
			Check(!second.Start(output.value, source.window, options), "Second controller took over live session");
			Check(controller.IsSpoofing(), "Rejected second controller disturbed active session");
			Focus(settings.value);
			Check(GetActualForegroundWindow() == settings.value, "Internal settings/input focus was hidden");
			Check(controller.IsSpoofing() == classic && GetForegroundWindow() == (classic ? output.value : settings.value), "Classic/conditional foreground policy mismatch");
			Check(controller.SuppressMessage(WM_KILLFOCUS, 0) == classic, "Classic/conditional deactivation policy mismatch");
			const auto background = controller.Observe();
			Check(background.actual == settings.value && background.shouldSpoof == classic &&
				background.perceived == (classic ? output.value : settings.value), "Background USER32 observation mismatch");
			Focus(source.window);
			Check(controller.IsSpoofing() && GetForegroundWindow() == output.value, "Source focus did not resume automatically");
			ShowWindow(output.value, SW_HIDE);
			Check(!controller.IsSpoofing() && GetForegroundWindow() == source.window, "Hidden output was spoofed");
			ShowWindow(output.value, SW_SHOWNOACTIVATE);
			Check(controller.IsSpoofing(), "Showing output did not resume spoofing");
			ShowWindow(output.value, SW_MINIMIZE);
			Check(!controller.IsSpoofing(), "Minimized output was spoofed");
			ShowWindow(output.value, SW_SHOWNOACTIVATE);
			Focus(output.value);
			Check(controller.IsSpoofing() && GetActualForegroundWindow() == output.value, "Actual output focus rejected");
			Focus(source.window);
			if (classic) Check(SetWindowSubclass(output.value, &OtherSubclass, 1, 0), "Unrelated subclass installation failed");
			controller.Stop();
			if (classic) {
				DWORD_PTR data = 0;
				Check(GetWindowSubclass(output.value, &OtherSubclass, 1, &data), "Stop removed an unrelated window subclass");
				RemoveWindowSubclass(output.value, &OtherSubclass, 1);
			}
			Check(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(output.value, GWLP_WNDPROC)) == &ExistingOutputProcedure,
				"Stop did not preserve the existing window procedure");
			Check(!controller.IsArmed() && GetForegroundWindow() == source.window && GetActualForegroundWindow() == source.window, "Stop did not restore USER32 focus");
			Check(controller.Observe().output == nullptr, "Stopped observation returned a stale window");
			controller.Stop();
		}
		Focus(source.window);
		{
			Readers readers(source.window, output.value);
			for (int i = 0; i != 100; ++i) {
				Check(controller.Start(output.value, source.window, options), "Repeated start failed");
				Check(GetForegroundWindow() == output.value, "Repeated start returned stale output");
				controller.Stop();
				Check(GetForegroundWindow() == source.window, "Repeated stop left hook armed");
			}
			readers.Join();
			Check(!readers.invalid.load() && readers.calls.load() > 0, "Concurrent foreground queries returned invalid focus");
		}
		for (int i = 0; i != 20; ++i) {
			Window fresh(L"NoFocusLoss recreated output");
			Focus(source.window);
			{
				NoFocusLossController scoped;
				Check(scoped.Start(fresh.value, source.window, options), "Recreated output start failed");
				Check(GetForegroundWindow() == fresh.value, "Recreated output returned previous HWND");
			}
			Check(GetForegroundWindow() == source.window, "Controller destructor did not restore foreground");
		}
		Focus(source.window);
		Check(controller.Start(output.value, source.window, options), "Destruction test start failed");
		stopDuringDestroy = &controller;
		DestroyWindow(output.value);
		stopDuringDestroy = nullptr;
		output.value = nullptr;
		Check(!controller.IsArmed() && !controller.IsSpoofing() && GetForegroundWindow() == source.window,
			"WM_DESTROY stop from inside the subclass chain left stale state");
		controller.Stop();
		Window finalOutput(L"NoFocusLoss source shutdown output");
		Focus(source.window);
		Check(controller.Start(finalOutput.value, source.window, options), "Source shutdown test start failed");
		source.Close();
		Check(!controller.IsSpoofing() && GetForegroundWindow() == GetActualForegroundWindow(), "Closed source kept stale spoofing alive");
		controller.Stop();
		std::cout << "PASS: real USER32 hook, all three modes, persistent classic foreground, outer window subclass ordering, keyboard/cursor, unrelated subclass preservation, observations, 100 threaded restarts, 20 HWND recreations and teardown\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
