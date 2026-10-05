#pragma once

#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>

namespace Magpie {

// No DbgHelp, logger, allocator or loader calls run on the faulting thread.
// A prestarted, independent process writes the dump while that thread waits.
// The helper mode returns before Magpie creates XAML, windows or GPU devices.
class CrashReporter {
public:
	static constexpr DWORD TERMINATE_CODE = 0xE0004D50;

	static bool TryRunHelper(int& exitCode) noexcept {
		int argc = 0;
		wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (!argv) return false;
		const bool helper = argc > 1 && std::wstring_view(argv[1]) == L"--magpie-crash-helper";
		if (!helper) { LocalFree(argv); return false; }
		HANDLE handles[4]{};
		bool valid = argc == 6;
		for (int i = 0; valid && i < 4; ++i) {
			wchar_t* end = nullptr;
			const auto value = _wcstoui64(argv[i + 2], &end, 16);
			valid = value != 0 && end != argv[i + 2] && *end == L'\0' &&
				value <= UINTPTR_MAX;
			handles[i] = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
		}
		LocalFree(argv);
		try { exitCode = valid ? _RunHelper(handles) : 2; }
		catch (...) { exitCode = 3; }
		return true;
	}

	static bool Start(const std::filesystem::path& exe,
		const std::filesystem::path& directory, const char* version) noexcept {
		if (_packet) return true;
		try { return _Start(exe, directory, version); }
		catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
	}

	// Registration lives until process exit, including late DLL teardown. The
	// OS releases the mapping/handles, and the helper watches the process handle.
	// No registry changes, debugger attachment or network reporting is involved.
	static void ReinstallFilter() noexcept {
		if (_packet) {
			SetUnhandledExceptionFilter(_Filter);
			std::set_terminate(_Terminate);
		}
	}

private:
	struct Packet {
		DWORD magic = 0x4D504352;
		DWORD schema = 1;
		DWORD processId = 0;
		DWORD threadId = 0;
		DWORD kind = 0;
		volatile LONG claimed = 0;
		volatile LONG ready = 0;
		EXCEPTION_RECORD record{};
		CONTEXT context{};
		wchar_t directory[32768]{};
		wchar_t version[64]{};
	};
	class Handle {
	public:
		explicit Handle(HANDLE value = nullptr) noexcept : _value(value) {}
		~Handle() { if (_value && _value != INVALID_HANDLE_VALUE) CloseHandle(_value); }
		Handle(const Handle&) = delete;
		Handle& operator=(const Handle&) = delete;
		HANDLE Get() const noexcept { return _value; }
		HANDLE Release() noexcept { HANDLE value = _value; _value = nullptr; return value; }
	private:
		HANDLE _value;
	};
	class View {
	public:
		explicit View(HANDLE mapping) noexcept : packet(static_cast<Packet*>(
			MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Packet)))) {}
		~View() { if (packet) UnmapViewOfFile(packet); }
		Packet* Release() noexcept { Packet* result = packet; packet = nullptr; return result; }
		Packet* packet;
	};
	class Completion {
	public:
		explicit Completion(HANDLE event) noexcept : _event(event) {}
		~Completion() { SetEvent(_event); }
	private:
		HANDLE _event;
	};

	static bool _Start(const std::filesystem::path& exe,
		const std::filesystem::path& directory, const char* version) {
		SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
		Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &sa,
			PAGE_READWRITE, 0, sizeof(Packet), nullptr));
		Handle request(CreateEventW(&sa, TRUE, FALSE, nullptr));
		Handle done(CreateEventW(&sa, TRUE, FALSE, nullptr));
		HANDLE processValue = nullptr;
		if (!mapping.Get() || !request.Get() || !done.Get() || !DuplicateHandle(
			GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &processValue,
			SYNCHRONIZE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, TRUE, 0)) return false;
		Handle process(processValue);
		View view(mapping.Get());
		if (!view.packet) return false;
		*view.packet = Packet{};
		view.packet->processId = GetCurrentProcessId();
		if (wcscpy_s(view.packet->directory, directory.c_str()) ||
			!MultiByteToWideChar(CP_UTF8, 0, version, -1,
				view.packet->version, static_cast<int>(std::size(view.packet->version)))) return false;
		HANDLE inherited[]{ mapping.Get(), request.Get(), done.Get(), process.Get() };
		SIZE_T attributeSize = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
		std::string storage(attributeSize, '\0');
		auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
		if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize)) return false;
		const bool updated = UpdateProcThreadAttribute(attributes, 0,
			PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr) != FALSE;
		STARTUPINFOEXW startup{};
		startup.StartupInfo.cb = sizeof(startup);
		startup.lpAttributeList = attributes;
		PROCESS_INFORMATION child{};
		wchar_t arguments[192]{};
		swprintf_s(arguments, L" --magpie-crash-helper %llx %llx %llx %llx",
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(inherited[0])),
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(inherited[1])),
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(inherited[2])),
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(inherited[3])));
		std::wstring command = L"\"" + exe.native() + L"\"" + arguments;
		const bool launched = updated && CreateProcessW(exe.c_str(), command.data(),
			nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
			nullptr, nullptr, &startup.StartupInfo, &child) != FALSE;
		const DWORD launchError = GetLastError();
		DeleteProcThreadAttributeList(attributes);
		if (!launched) { SetLastError(launchError); return false; }
		Handle childProcess(child.hProcess), childThread(child.hThread);
		HANDLE waitHandles[]{ done.Get(), childProcess.Get() };
		if (WaitForMultipleObjects(2, waitHandles, FALSE, 5000) != WAIT_OBJECT_0 ||
			InterlockedCompareExchange(&view.packet->ready, 0, 0) != 1) {
			// A startup timeout must not leave an orphan helper or block Magpie.
			TerminateProcess(childProcess.Get(), ERROR_TIMEOUT);
			WaitForSingleObject(childProcess.Get(), 1000);
			SetLastError(ERROR_TIMEOUT);
			return false;
		}
		ResetEvent(done.Get());
		_request = request.Release();
		_done = done.Release();
		_packet = view.Release();
		// The child owns its inherited copies; the mapping view remains valid
		// when this process closes its mapping handle.
		ReinstallFilter();
		return true;
	}

	static void _Capture(EXCEPTION_POINTERS* exception, DWORD kind) noexcept {
		Packet* packet = _packet;
		if (!packet || !exception || !exception->ExceptionRecord || !exception->ContextRecord) return;
		if (InterlockedCompareExchange(&packet->claimed, 1, 0) != 0) return;
		packet->threadId = GetCurrentThreadId();
		packet->kind = kind;
		packet->record = *exception->ExceptionRecord;
		packet->record.ExceptionRecord = nullptr;
		packet->context = *exception->ContextRecord;
		MemoryBarrier();
		SetEvent(_request);
		WaitForSingleObject(_done, 15000);
	}
	static LONG WINAPI _Filter(EXCEPTION_POINTERS* exception) noexcept {
		_Capture(exception, 1);
		// Keep Windows' normal fatal-exception handling. Never resume the
		// corrupted process, and do not swallow the exception as a recovery.
		return EXCEPTION_CONTINUE_SEARCH;
	}
	[[noreturn]] static void _Terminate() noexcept {
		CONTEXT context{};
		RtlCaptureContext(&context);
		EXCEPTION_RECORD record{};
		record.ExceptionCode = TERMINATE_CODE;
		record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
#if defined(_M_X64)
		record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
#elif defined(_M_ARM64)
		record.ExceptionAddress = reinterpret_cast<void*>(context.Pc);
#else
		record.ExceptionAddress = reinterpret_cast<void*>(static_cast<uintptr_t>(context.Eip));
#endif
		EXCEPTION_POINTERS exception{ &record, &context };
		_Capture(&exception, 2);
		TerminateProcess(GetCurrentProcess(), TERMINATE_CODE);
		std::abort();
	}

	static void _Write(HANDLE file, std::wstring_view text) {
		const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
			static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		if (!length) return;
		std::string encoded(static_cast<size_t>(length), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
			encoded.data(), length, nullptr, nullptr);
		DWORD written = 0;
		WriteFile(file, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr);
	}
	static int _RunHelper(HANDLE (&handles)[4]) {
		Handle mapping(handles[0]), request(handles[1]), done(handles[2]), process(handles[3]);
		View view(mapping.Get());
		if (!view.packet || view.packet->magic != 0x4D504352 || view.packet->schema != 1 ||
			GetProcessId(process.Get()) != view.packet->processId ||
			view.packet->processId == GetCurrentProcessId()) return 2;
		Packet& packet = *view.packet;
		Completion completion(done.Get());
		std::filesystem::path directory(packet.directory);
		std::filesystem::create_directories(directory);
		// Verify write access before telling the parent that reporting is ready.
		const auto probe = directory / (L"reporter-" + std::to_wstring(packet.processId) + L".tmp");
		{ Handle file(CreateFileW(probe.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
			CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
			if (file.Get() == INVALID_HANDLE_VALUE) return 3; }
		InterlockedExchange(&packet.ready, 1);
		SetEvent(done.Get());
		HANDLE waits[]{ request.Get(), process.Get() };
		const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
		DWORD processExit = 0;
		const bool captured = wait == WAIT_OBJECT_0 && packet.kind != 0;
		if (!captured && (wait != WAIT_OBJECT_0 + 1 ||
			!GetExitCodeProcess(process.Get(), &processExit) || processExit == 0)) return 0;
		SYSTEMTIME now{}; GetLocalTime(&now);
		wchar_t stem[128]{};
		swprintf_s(stem, L"Magpie-crash-%04u%02u%02u-%02u%02u%02u-%lu",
			now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, packet.processId);
		const auto reportPath = directory / (std::wstring(stem) + L".txt");
		Handle report(CreateFileW(reportPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
			nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (report.Get() == INVALID_HANDLE_VALUE) { SetEvent(done.Get()); return 3; }
		wchar_t line[1024]{};
		swprintf_s(line, L"Magpie native crash report\r\nversion=%s\r\nprocess=%lu\r\n",
			packet.version, packet.processId);
		_Write(report.Get(), line);
		if (!captured) {
			swprintf_s(line, L"capture=process-exit-only\r\nexitCode=0x%08lx\r\n", processExit);
			_Write(report.Get(), line);
			_Write(report.Get(), L"No exception context: fail-fast, overwritten filter or external termination can bypass the handler. No dump was taken after process exit.\r\n");
			FlushFileBuffers(report.Get());
			return 0;
		}
		swprintf_s(line, L"capture=%s\r\nthread=%lu\r\nexceptionCode=0x%08lx\r\nexceptionAddress=0x%llx\r\n",
			packet.kind == 2 ? L"std::terminate (synthetic context)" : L"unhandled-exception",
			packet.threadId, packet.record.ExceptionCode,
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(packet.record.ExceptionAddress)));
		_Write(report.Get(), line);
		for (DWORD i = 0; i < packet.record.NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i) {
			swprintf_s(line, L"exceptionParameter[%lu]=0x%llx\r\n", i,
				static_cast<unsigned long long>(packet.record.ExceptionInformation[i]));
			_Write(report.Get(), line);
		}
		// Resolve module/offset outside the faulting process, including proxies.
		Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, packet.processId));
		MODULEENTRY32W module{}; module.dwSize = sizeof(module);
		const uintptr_t address = reinterpret_cast<uintptr_t>(packet.record.ExceptionAddress);
		if (snapshot.Get() != INVALID_HANDLE_VALUE && Module32FirstW(snapshot.Get(), &module)) do {
			const uintptr_t base = reinterpret_cast<uintptr_t>(module.modBaseAddr);
			if (address >= base && address - base < module.modBaseSize) {
				swprintf_s(line, L"faultModule=%s\r\nfaultOffset=0x%llx\r\n", module.szModule,
					static_cast<unsigned long long>(address - base));
				_Write(report.Get(), line);
			}
			_Write(report.Get(), L"module=" + std::wstring(module.szExePath));
			swprintf_s(line, L" base=0x%llx size=0x%lx\r\n",
				static_cast<unsigned long long>(base), module.modBaseSize);
			_Write(report.Get(), line);
		} while (Module32NextW(snapshot.Get(), &module));
		// Commit the text before a dump failure or timeout. Only the helper loads
		// the system DbgHelp; a local FG proxy named dbghelp.dll is not selected.
		FlushFileBuffers(report.Get());
		const auto dumpPath = directory / (std::wstring(stem) + L".dmp");
		Handle dump(CreateFileW(dumpPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
			nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
		DWORD dumpError = dump.Get() == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
		wchar_t systemDirectory[MAX_PATH]{};
		const UINT systemLength = GetSystemDirectoryW(systemDirectory, MAX_PATH);
		const auto dbghelpPath = std::filesystem::path(systemDirectory) / L"dbghelp.dll";
		HMODULE dbghelp = systemLength > 0 && systemLength < MAX_PATH ?
			LoadLibraryExW(dbghelpPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) : nullptr;
		if (!dbghelp && !dumpError) dumpError = GetLastError();
		const auto writeDump = dbghelp ? reinterpret_cast<decltype(&MiniDumpWriteDump)>(
			GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
		if (dbghelp && !writeDump && !dumpError) dumpError = GetLastError();
		EXCEPTION_POINTERS exception{ &packet.record, &packet.context };
		MINIDUMP_EXCEPTION_INFORMATION info{ packet.threadId, &exception, FALSE };
		const bool dumped = dump.Get() != INVALID_HANDLE_VALUE && writeDump && writeDump(
			process.Get(), packet.processId, dump.Get(),
			static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
			&info, nullptr, nullptr) != FALSE;
		if (!dumped && !dumpError) dumpError = GetLastError();
		swprintf_s(line, L"dumpWritten=%s\r\ndumpError=0x%08lx\r\n", dumped ? L"true" : L"false", dumpError);
		_Write(report.Get(), line);
		FlushFileBuffers(report.Get());
		if (dumped) FlushFileBuffers(dump.Get());
		if (dbghelp) FreeLibrary(dbghelp);
		SetEvent(done.Get());
		return dumped ? 0 : 4;
	}
	static inline Packet* _packet = nullptr;
	static inline HANDLE _request = nullptr;
	static inline HANDLE _done = nullptr;
};

}
