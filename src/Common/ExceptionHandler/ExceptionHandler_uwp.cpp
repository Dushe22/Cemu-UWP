#include "Common/precompiled.h"
#include "Common/version.h"
#include "ExceptionHandler.h"

#include "config/ActiveSettings.h"

#include <Windows.h>

#include <atomic>
#include <csignal>
#include <cstdlib>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
// the dump itself is written by the console's error reporting once the crash leaves the process,
// when Crash Dumps is enabled for the package in Device Portal > Crash data
std::atomic_bool s_crashHandled{ false };

std::string CrashTimestamp()
{
	const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	const std::tm time = *std::gmtime(&now);
	return fmt::format("{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}", 1900 + time.tm_year, time.tm_mon + 1, time.tm_mday,
		time.tm_hour, time.tm_min, time.tm_sec);
}

// walks the x64 unwind tables (no DbgHelp on UWP); offsets resolve with the PDB of this build.
// No C++ objects here so a corrupted stack can be caught with __try.
int CaptureStackFrames(const CONTEXT* crashContext, DWORD64* rips, DWORD64* bases, int maxFrames)
{
	int count = 0;
	__try
	{
		CONTEXT context = *crashContext;
		while (count < maxFrames && context.Rip != 0)
		{
			DWORD64 moduleBase = 0;
			PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &moduleBase, nullptr);
			rips[count] = context.Rip;
			bases[count] = moduleBase;
			++count;
			if (!function)
			{
				// leaf function: the return address is on top of the stack
				context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
				context.Rsp += 8;
				continue;
			}
			void* handlerData = nullptr;
			DWORD64 establisherFrame = 0;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, moduleBase, context.Rip, function, &context, &handlerData, &establisherFrame, nullptr);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
	return count;
}

void LogStackTrace(const CONTEXT& crashContext, uint64 cemuImageBase)
{
	PWSTR threadName = nullptr;
	if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &threadName)) && threadName)
	{
		char narrowName[128]{};
		WideCharToMultiByte(CP_UTF8, 0, threadName, -1, narrowName, sizeof(narrowName) - 1, nullptr, nullptr);
		CrashLog_WriteLine(fmt::format("Thread {} \"{}\"", GetCurrentThreadId(), narrowName));
		LocalFree(threadName);
	}
	DWORD64 rips[32]{};
	DWORD64 bases[32]{};
	const int frames = CaptureStackFrames(&crashContext, rips, bases, 32);
	CrashLog_WriteLine("Stack (module base + offset):");
	for (int frame = 0; frame < frames; ++frame)
	{
		CrashLog_WriteLine(fmt::format("  #{:02d} {}@{:x}+0x{:x}", frame, bases[frame] == cemuImageBase ? "CemuUwp.exe" : "module",
			(uint64)bases[frame], (uint64)(rips[frame] - bases[frame])));
	}
}

void LogExceptionRecord(EXCEPTION_POINTERS* pointers)
{
	const uint64 imageBase = reinterpret_cast<uint64>(&__ImageBase);
	CrashLog_WriteLine(fmt::format("CemuUwp.exe at 0x{:x}", imageBase));
	if (!pointers || !pointers->ExceptionRecord)
		return;
	const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
	const uint64 address = reinterpret_cast<uint64>(record->ExceptionAddress);
	CrashLog_WriteLine(fmt::format("Exception 0x{:08x} at 0x{:x} (CemuUwp.exe+0x{:x})",
		(uint32)record->ExceptionCode, address, address - imageBase));
	if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
	{
		CrashLog_WriteLine(fmt::format("{} of address 0x{:x}",
			record->ExceptionInformation[0] == 0 ? "Read" : (record->ExceptionInformation[0] == 8 ? "Execute" : "Write"),
			(uint64)record->ExceptionInformation[1]));
	}
	if (const CONTEXT* context = pointers->ContextRecord)
	{
		CrashLog_WriteLine(fmt::format("RAX={:016x} RBX={:016x} RCX={:016x} RDX={:016x}", context->Rax, context->Rbx, context->Rcx, context->Rdx));
		CrashLog_WriteLine(fmt::format("RSP={:016x} RBP={:016x} RDI={:016x} RSI={:016x}", context->Rsp, context->Rbp, context->Rdi, context->Rsi));
		CrashLog_WriteLine(fmt::format("R8 ={:016x} R9 ={:016x} R10={:016x} R11={:016x}", context->R8, context->R9, context->R10, context->R11));
		CrashLog_WriteLine(fmt::format("R12={:016x} R13={:016x} R14={:016x} R15={:016x}", context->R12, context->R13, context->R14, context->R15));
		CrashLog_WriteLine(fmt::format("RIP={:016x}", context->Rip));
		LogStackTrace(*context, imageBase);
	}
}

void HandleCrash(std::string_view reason, EXCEPTION_POINTERS* pointers)
{
	if (s_crashHandled.exchange(true))
		return;
	CrashLog_Create();
	CrashLog_SetOutputChannels(false, true);
	CrashLog_WriteLine("");
	CrashLog_WriteHeader(fmt::format("Crash: {}", reason).c_str());
	CrashLog_WriteLine(BUILD_VERSION_WITH_NAME_STRING);
	LogExceptionRecord(pointers);
	CrashLog_WriteLine("Crash dump: written by the console if Crash Dumps is enabled for this app in Device Portal > Crash data");

	ExceptionHandler_LogGeneralInfo();
	cemuLog_waitForFlush();
	// the next launch replaces log.txt, so keep this one next to the crash time
	const fs::path crashFolder = ActiveSettings::GetUserDataPath("crashdump");
	std::error_code ec;
	fs::create_directories(crashFolder, ec);
	fs::copy_file(cemuLog_GetActiveLogFilePath(), crashFolder / fmt::format("log_{}.txt", CrashTimestamp()), ec);
}

void HandleFatalTermination(std::string_view reason)
{
	CONTEXT context{};
	RtlCaptureContext(&context);
	EXCEPTION_RECORD record{};
	record.ExceptionCode = STATUS_FATAL_APP_EXIT;
	record.ExceptionAddress = reinterpret_cast<PVOID>(context.Rip);
	EXCEPTION_POINTERS pointers{ &record, &context };
	HandleCrash(reason, &pointers);
}

DWORD WINAPI HandleStackOverflowThread(LPVOID pointers)
{
	HandleCrash("stack overflow", static_cast<EXCEPTION_POINTERS*>(pointers));
	return 0;
}

LONG WINAPI UnhandledExceptionFilterUwp(EXCEPTION_POINTERS* pointers)
{
	// the overflowed stack has no room for logging, so that runs on a thread of its own
	const bool stackOverflow = pointers && pointers->ExceptionRecord &&
		pointers->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW;
	if (!stackOverflow)
		HandleCrash("unhandled exception", pointers);
	else if (const HANDLE thread = CreateThread(nullptr, 0, HandleStackOverflowThread, pointers, 0, nullptr))
	{
		WaitForSingleObject(thread, INFINITE);
		CloseHandle(thread);
	}
	// keep searching so the console's error reporting still sees the crash and writes its dump
	return EXCEPTION_CONTINUE_SEARCH;
}

void AbortSignalHandler(int)
{
	// terminate handlers are per thread, so an exception escaping a worker thread arrives here as abort
	std::string reason = "abort";
	if (const std::exception_ptr exception = std::current_exception())
	{
		try
		{
			std::rethrow_exception(exception);
		}
		catch (const std::exception& ex)
		{
			reason = fmt::format("uncaught exception: {}", ex.what());
		}
		catch (...)
		{
			reason = "uncaught exception of unknown type";
		}
	}
	HandleFatalTermination(reason);
}

void InvalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
	HandleFatalTermination("invalid CRT parameter");
	std::abort();
}

void PureCallHandler()
{
	HandleFatalTermination("pure virtual call");
	std::abort();
}
}

void ExceptionHandler_Init()
{
	SetUnhandledExceptionFilter(UnhandledExceptionFilterUwp);
	// these end the process without passing through the exception filter
	std::signal(SIGABRT, AbortSignalHandler);
	_set_invalid_parameter_handler(InvalidParameterHandler);
	_set_purecall_handler(PureCallHandler);
}

void ExceptionHandler_HandleFatalTermination(std::string_view reason)
{
	HandleFatalTermination(reason);
}
