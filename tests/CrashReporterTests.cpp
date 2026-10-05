#define NOMINMAX
#include "../src/Shared/CrashReporter.h"
#include "../src/Magpie.Core/include/NgxRuntimeGuard.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

using Magpie::CrashReporter;

static void Require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}
static std::filesystem::path Exe() {
	std::wstring path(32768, L'\0');
	const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	Require(length > 0 && length < path.size(), "executable path");
	path.resize(length);
	return path;
}
__declspec(noinline) static DWORD WINAPI Fault(void* pointer) {
	*static_cast<volatile int*>(pointer) = 1;
	return 0;
}
static int Child(std::wstring_view scenario, const std::filesystem::path& directory,
	const std::filesystem::path& helperExe) {
	SetErrorMode(SEM_NOGPFAULTERRORBOX);
	Require(CrashReporter::Start(helperExe, directory, "crash-test-vrr3"), "reporter startup");
	if (scenario == L"normal") return 0;
	if (scenario == L"handled") {
		DWORD seh = 0;
		const int result = Magpie::NgxRuntimeGuard::Invoke([]() -> int {
			RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
			return 42;
		}, -1, &seh);
		Require(result == -1 && seh == EXCEPTION_ACCESS_VIOLATION, "handled fault contract");
		return 0;
	}
	if (scenario == L"terminate") std::terminate();
	if (scenario == L"noexcept") {
		void (*thrower)() = [] { throw std::runtime_error("noexcept fault"); };
		auto boundary = [thrower]() noexcept { thrower(); };
		boundary();
	}
	if (scenario == L"hard-exit") {
		TerminateProcess(GetCurrentProcess(), 0xC0000409);
		return 23;
	}
	void* noAccess = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
	Require(noAccess != nullptr, "fault page allocation");
	if (scenario == L"worker") {
		HANDLE thread = CreateThread(nullptr, 0, Fault, noAccess, 0, nullptr);
		Require(thread != nullptr, "fault worker creation");
		WaitForSingleObject(thread, INFINITE);
		CloseHandle(thread);
	} else if (scenario == L"main") {
		Fault(noAccess);
	}
	return 24;
}
struct ChildResult { DWORD code, mainThread; };
static ChildResult Launch(std::wstring arguments, const std::filesystem::path& executable = {}) {
	const auto exe = executable.empty() ? Exe() : executable;
	std::wstring command = L"\"" + exe.native() + L"\" " + arguments;
	STARTUPINFOW startup{}; startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	Require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE, "child launch");
	const DWORD waited = WaitForSingleObject(process.hProcess, 30000);
	if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, ERROR_TIMEOUT);
	DWORD code = 0; GetExitCodeProcess(process.hProcess, &code);
	const DWORD mainThread = process.dwThreadId;
	CloseHandle(process.hThread); CloseHandle(process.hProcess);
	Require(waited == WAIT_OBJECT_0, "child timeout");
	return { code, mainThread };
}
static std::string Read(const std::filesystem::path& path) {
	std::ifstream input(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(input), {});
}
static std::filesystem::path Report(const std::filesystem::path& directory) {
	for (int attempt = 0; attempt < 250; ++attempt) {
		if (std::filesystem::exists(directory)) {
			for (const auto& entry : std::filesystem::directory_iterator(directory))
				if (entry.path().extension() == L".txt") return entry.path();
		}
		Sleep(20);
	}
	throw std::runtime_error("missing crash report");
}
static void CheckDump(const std::filesystem::path& path, DWORD code, DWORD mainThread, bool worker) {
	const std::string bytes = Read(path);
	Require(bytes.size() >= sizeof(MINIDUMP_HEADER), "missing minidump header");
	Require(reinterpret_cast<const MINIDUMP_HEADER*>(bytes.data())->Signature == MINIDUMP_SIGNATURE,
		"invalid minidump signature");
	void* buffer = nullptr; ULONG size = 0;
	PMINIDUMP_DIRECTORY stream = nullptr;
	Require(MiniDumpReadDumpStream(const_cast<char*>(bytes.data()), ExceptionStream,
		&stream, &buffer, &size) != FALSE, "missing exception stream");
	const auto* exception = static_cast<MINIDUMP_EXCEPTION_STREAM*>(buffer);
	Require(size >= sizeof(*exception) && exception->ExceptionRecord.ExceptionCode == code,
		"wrong exception code in dump");
	Require((exception->ThreadId != mainThread) == worker, "wrong faulting thread");
	const auto contextRva = exception->ThreadContext.Rva;
	Require(contextRva < bytes.size() && bytes.size() - contextRva >= sizeof(CONTEXT), "missing fault context");
	const auto* context = reinterpret_cast<const CONTEXT*>(bytes.data() + contextRva);
	Require(context->Rip == exception->ExceptionRecord.ExceptionAddress, "dump has helper context, not fault context");
	Require(MiniDumpReadDumpStream(const_cast<char*>(bytes.data()), ModuleListStream,
		&stream, &buffer, &size) != FALSE && size > 0, "missing module list");
}
int wmain(int argc, wchar_t** argv) {
	int helperExit = 0;
	if (CrashReporter::TryRunHelper(helperExit)) return helperExit;
	try {
		if (argc == 5 && std::wstring_view(argv[1]) == L"--crash-child") return Child(argv[2], argv[3], argv[4]);
		const auto helperExe = argc == 3 && std::wstring_view(argv[1]) == L"--helper-exe" ?
			std::filesystem::path(argv[2]) : Exe();
		const auto root = std::filesystem::temp_directory_path() /
			(L"Magpie crash тест " + std::to_wstring(GetCurrentProcessId()));
		std::filesystem::create_directories(root);
		for (const wchar_t* scenario : { L"normal", L"handled", L"main", L"worker", L"terminate", L"noexcept", L"hard-exit" }) {
			const auto directory = root / scenario;
			const auto child = Launch(L"--crash-child " + std::wstring(scenario) + L" \"" + directory.native() +
				L"\" \"" + helperExe.native() + L"\"");
			if (std::wstring_view(scenario) == L"normal" || std::wstring_view(scenario) == L"handled") {
				Require(child.code == 0, "normal/handled fault exited with failure");
				Require(std::filesystem::is_empty(directory), "reported a handled exception or normal exit");
			} else {
				const bool hard = std::wstring_view(scenario) == L"hard-exit";
				const bool terminate = std::wstring_view(scenario) == L"terminate" || std::wstring_view(scenario) == L"noexcept";
				const DWORD expected = hard ? 0xC0000409 : terminate ? CrashReporter::TERMINATE_CODE : EXCEPTION_ACCESS_VIOLATION;
				Require(child.code == expected, "changed fatal exit code");
				const auto reportPath = Report(directory);
				const std::string report = Read(reportPath);
				Require(report.find("version=crash-test-vrr3") != std::string::npos, "lost build identity");
				auto dumpPath = reportPath; dumpPath.replace_extension(L".dmp");
				if (hard) {
					Require(report.find("capture=process-exit-only") != std::string::npos &&
						report.find("exitCode=0xc0000409") != std::string::npos, "lost abrupt exit evidence");
					Require(!std::filesystem::exists(dumpPath), "claimed a post-exit dump");
				} else {
					Require(report.find("dumpWritten=true") != std::string::npos, "dump writing failed");
					Require(report.find("faultModule=CrashReporterTests.exe") != std::string::npos &&
						report.find("faultOffset=0x") != std::string::npos, "missing fault module and offset");
					CheckDump(dumpPath, expected, child.mainThread, std::wstring_view(scenario) == L"worker");
				}
			}
			std::wcout << L"PASS native crash scenario: " << scenario << L'\n';
		}
		Require(Launch(L"--magpie-crash-helper invalid 0 0 0", helperExe).code == 2, "invalid helper entered application startup");
		std::filesystem::remove_all(root);
		std::cout << "PASS invalid helper arguments, Unicode/space paths, production IPC and exception streams\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
