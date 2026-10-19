#include "UwpWindowSystem.h"

#include "CemuHost.h"
#include "UwpBuildConfig.h"
#include "UwpFrontend.h"
#include "UwpInputProvider.h"
#include "UwpStorageService.h"

#include "WindowSystem.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineStableCache.h"
#include "config/LaunchSettings.h"
#include "input/InputManager.h"

#include <boost/program_options/parsers.hpp>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/base.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <exception>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#if defined(CEMU_UWP_ALLOCATION_DIAGNOSTICS)
#include <new.h>
#endif

namespace
{
WindowSystem::WindowInfo g_windowInfo{};
winrt::Windows::UI::Core::CoreWindow g_coreWindow{ nullptr };
std::atomic_bool g_resizePending{ false };
std::atomic_bool g_visible{ true };
std::atomic_bool g_hostInitialized{ false };
winrt::agile_ref<winrt::Windows::UI::Core::CoreDispatcher> g_coreDispatcher;

std::mutex g_graphicsRecoveryMutex;
std::optional<std::string> g_pendingGraphicsRecovery;
std::atomic_bool g_graphicsRecoveryPending{ false };
uint32_t g_coalescedGraphicsRecoveryRequests = 0;

struct ProtocolLaunchRequest
{
	fs::path launchPath;
	std::wstring launchOnExit;
	std::string errorMessage;
};

std::mutex g_protocolLaunchMutex;
std::optional<ProtocolLaunchRequest> g_pendingProtocolLaunch;
std::wstring g_launchOnExitUri;

#if defined(CEMU_UWP_ALLOCATION_DIAGNOSTICS)
HANDLE g_allocationDiagnosticFile = INVALID_HANDLE_VALUE;
std::atomic<uint32_t> g_allocationDiagnosticSequence{ 0 };
std::atomic_flag g_allocationDiagnosticWriteLock = ATOMIC_FLAG_INIT;
uintptr_t g_allocationDiagnosticImageBase = 0;
size_t g_allocationDiagnosticImageSize = 0;
thread_local bool g_newHandlerCapturedThisThread = false;

void WriteAllocationDiagnosticRecord(const char* source, size_t requestedBytes, bool captureStack) noexcept
{
	const uint32_t sequence = g_allocationDiagnosticSequence.fetch_add(1, std::memory_order_relaxed) + 1;
	if (sequence > 16 || g_allocationDiagnosticFile == INVALID_HANDLE_VALUE)
		return;
	if (g_allocationDiagnosticWriteLock.test_and_set(std::memory_order_acquire))
		return;

	std::array<void*, 64> frames{};
	ULONG stackHash = 0;
	const USHORT frameCount = captureStack ?
		RtlCaptureStackBackTrace(1, static_cast<ULONG>(frames.size()), frames.data(), &stackHash) : 0;

	std::array<char, 8192> text{};
	size_t used = 0;
	const auto append = [&]<typename... Args>(const char* format, Args... args)
	{
		if (used >= text.size() - 1)
			return;
		const size_t remaining = text.size() - used;
		const int result = std::snprintf(text.data() + used, remaining, format, args...);
		if (result < 0)
			return;
		if (static_cast<size_t>(result) >= remaining)
			used = text.size() - 1;
		else
			used += static_cast<size_t>(result);
	};

	append("allocation_failure sequence=%u source=%s requested=%llu tick_ms=%llu pid=%lu tid=%lu image_base=0x%016llX image_size=0x%llX frames=%u hash=0x%08lX\r\n",
		sequence,
		source ? source : "unknown",
		static_cast<unsigned long long>(requestedBytes),
		static_cast<unsigned long long>(GetTickCount64()),
		static_cast<unsigned long>(GetCurrentProcessId()),
		static_cast<unsigned long>(GetCurrentThreadId()),
		static_cast<unsigned long long>(g_allocationDiagnosticImageBase),
		static_cast<unsigned long long>(g_allocationDiagnosticImageSize),
		static_cast<unsigned>(frameCount),
		static_cast<unsigned long>(stackHash));

	for (USHORT index = 0; index < frameCount; ++index)
	{
		const uintptr_t pc = reinterpret_cast<uintptr_t>(frames[index]);
		if (pc >= g_allocationDiagnosticImageBase &&
			pc - g_allocationDiagnosticImageBase < g_allocationDiagnosticImageSize)
		{
			append("  frame[%02u]=CemuUwp.exe+0x%llX pc=0x%016llX\r\n",
				static_cast<unsigned>(index),
				static_cast<unsigned long long>(pc - g_allocationDiagnosticImageBase),
				static_cast<unsigned long long>(pc));
		}
		else
		{
			append("  frame[%02u]=external pc=0x%016llX\r\n",
				static_cast<unsigned>(index),
				static_cast<unsigned long long>(pc));
		}
	}

	DWORD written = 0;
	(void)WriteFile(g_allocationDiagnosticFile, text.data(), static_cast<DWORD>(used), &written, nullptr);
	(void)FlushFileBuffers(g_allocationDiagnosticFile);
	g_allocationDiagnosticWriteLock.clear(std::memory_order_release);
}

int __cdecl UwpAllocationFailureHandler(size_t requestedBytes) noexcept
{
	g_newHandlerCapturedThisThread = true;
	WriteAllocationDiagnosticRecord("operator_new", requestedBytes, true);
	return 0;
}

void InstallAllocationDiagnostics() noexcept
{
	try
	{
		const auto localFolder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
		fs::path logPath{ std::wstring(localFolder.c_str()) };
		logPath /= "allocation_failure.log";

		CREATEFILE2_EXTENDED_PARAMETERS parameters{};
		parameters.dwSize = sizeof(parameters);
		parameters.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
		parameters.dwFileFlags = 0;
		g_allocationDiagnosticFile = CreateFile2FromAppW(
			logPath.c_str(),
			GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			CREATE_ALWAYS,
			&parameters);
		if (g_allocationDiagnosticFile == INVALID_HANDLE_VALUE)
			return;

		const auto imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		g_allocationDiagnosticImageBase = imageBase;
		if (imageBase != 0)
		{
			const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
			if (dosHeader->e_magic == IMAGE_DOS_SIGNATURE && dosHeader->e_lfanew > 0)
			{
				const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(imageBase + dosHeader->e_lfanew);
				if (ntHeaders->Signature == IMAGE_NT_SIGNATURE)
					g_allocationDiagnosticImageSize = ntHeaders->OptionalHeader.SizeOfImage;
			}
		}

		std::array<char, 256> header{};
		const int headerLength = std::snprintf(
			header.data(),
			header.size(),
			"allocation_diagnostics_started pid=%lu image_base=0x%016llX image_size=0x%llX\r\n",
			static_cast<unsigned long>(GetCurrentProcessId()),
			static_cast<unsigned long long>(g_allocationDiagnosticImageBase),
			static_cast<unsigned long long>(g_allocationDiagnosticImageSize));
		if (headerLength > 0)
		{
			DWORD written = 0;
			(void)WriteFile(g_allocationDiagnosticFile, header.data(), static_cast<DWORD>(headerLength), &written, nullptr);
			(void)FlushFileBuffers(g_allocationDiagnosticFile);
		}

		(void)_set_new_handler(UwpAllocationFailureHandler);
	}
	catch (...)
	{
	}
}
#endif

void WriteBootstrapLog(std::string_view message)
{
	try
	{
		static const auto bootLogStart = std::chrono::steady_clock::now();
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - bootLogStart).count();
		std::ostringstream prefix;
		prefix << "[boot +" << elapsed << "ms tid=" << std::this_thread::get_id() << "] ";

		const auto localFolder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
		fs::path logPath{ std::wstring(localFolder.c_str()) };
		logPath /= "bootstrap.log";

		std::ofstream logFile(logPath, std::ios::app);
		logFile << prefix.str() << message << '\n';
	}
	catch (...)
	{
	}
}

#if defined(CEMU_DEBUG_ASSERT)
#define CEMU_UWP_BOOTSTRAP_TRACE(message) WriteBootstrapLog(message)
#else
#define CEMU_UWP_BOOTSTRAP_TRACE(message) ((void)0)
#endif

std::optional<std::string> TakePendingGraphicsRecovery()
{
	if (!g_graphicsRecoveryPending.load(std::memory_order_acquire))
		return {};

	std::string message;
	uint32_t coalescedRequests = 0;
	{
		std::scoped_lock lock(g_graphicsRecoveryMutex);
		if (!g_pendingGraphicsRecovery)
		{
			g_graphicsRecoveryPending.store(false, std::memory_order_release);
			return {};
		}
		message = std::move(*g_pendingGraphicsRecovery);
		g_pendingGraphicsRecovery.reset();
		coalescedRequests = g_coalescedGraphicsRecoveryRequests;
		g_coalescedGraphicsRecoveryRequests = 0;
		g_graphicsRecoveryPending.store(false, std::memory_order_release);
	}

	if (coalescedRequests != 0)
		message += fmt::format(" ({} additional recovery request{} coalesced)",
			coalescedRequests, coalescedRequests == 1 ? "" : "s");
	return message;
}

void WakeCoreWindowDispatcher() noexcept
{
	try
	{
		auto dispatcher = g_coreDispatcher.get();
		if (dispatcher)
		{
			(void)dispatcher.RunAsync(
				winrt::Windows::UI::Core::CoreDispatcherPriority::High,
				[]() {});
		}
	}
	catch (const winrt::hresult_error& ex)
	{
		WriteBootstrapLog(fmt::format("CemuUwp could not wake the UI for graphics recovery 0x{:08X}: {}",
			(uint32)ex.code(), winrt::to_string(ex.message())));
	}
	catch (...)
	{
		WriteBootstrapLog("CemuUwp could not wake the UI for graphics recovery");
	}
}

void InstallProcessDiagnostics()
{
	ExceptionHandler_Init();
	std::set_terminate([]()
	{
		std::string reason = "std::terminate without active exception";
		if (const auto exception = std::current_exception())
		{
			try
			{
				std::rethrow_exception(exception);
			}
			catch (const std::exception& ex)
			{
				reason = fmt::format("std::terminate: {}", ex.what());
			}
			catch (...)
			{
				reason = "std::terminate: unknown active exception";
			}
		}
		WriteBootstrapLog(fmt::format("CemuUwp {}", reason));
		ExceptionHandler_HandleFatalTermination(reason);
		std::abort();
	});
	std::atexit([]()
	{
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwp process atexit");
	});
}

std::wstring Trim(std::wstring value)
{
	const auto isSpace = [](wchar_t ch)
	{
		return std::iswspace(ch) != 0;
	};

	while (!value.empty() && isSpace(value.front()))
		value.erase(value.begin());
	while (!value.empty() && isSpace(value.back()))
		value.pop_back();
	return value;
}

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i)
	{
		if (std::towlower(a[i]) != std::towlower(b[i]))
			return false;
	}
	return true;
}

bool IsHexDigit(wchar_t ch)
{
	return (ch >= L'0' && ch <= L'9') ||
		   (ch >= L'a' && ch <= L'f') ||
		   (ch >= L'A' && ch <= L'F');
}

uint8_t HexValue(wchar_t ch)
{
	if (ch >= L'0' && ch <= L'9')
		return (uint8_t)(ch - L'0');
	if (ch >= L'a' && ch <= L'f')
		return (uint8_t)(ch - L'a' + 10);
	return (uint8_t)(ch - L'A' + 10);
}

std::wstring PercentDecodeQueryComponent(std::wstring_view value)
{
	std::wstring output;
	std::string utf8Bytes;

	const auto flushUtf8Bytes = [&]()
	{
		if (utf8Bytes.empty())
			return;
		const winrt::hstring decoded = winrt::to_hstring(utf8Bytes);
		output.append(decoded.c_str(), decoded.size());
		utf8Bytes.clear();
	};

	for (size_t i = 0; i < value.size(); ++i)
	{
		const wchar_t ch = value[i];
		if (ch == L'%' && i + 2 < value.size() && IsHexDigit(value[i + 1]) && IsHexDigit(value[i + 2]))
		{
			const uint8_t decodedByte = (HexValue(value[i + 1]) << 4) | HexValue(value[i + 2]);
			utf8Bytes.push_back((char)decodedByte);
			i += 2;
			continue;
		}

		flushUtf8Bytes();
		output.push_back(ch == L'+' ? L' ' : ch);
	}

	flushUtf8Bytes();
	return output;
}

std::optional<std::wstring> TryGetQueryParameter(std::wstring_view rawQuery, std::wstring_view requestedKey)
{
	size_t start = 0;
	while (start <= rawQuery.size())
	{
		const size_t end = rawQuery.find(L'&', start);
		const auto pair = rawQuery.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start);
		const size_t equals = pair.find(L'=');
		const auto rawKey = equals == std::wstring_view::npos ? pair : pair.substr(0, equals);
		const auto rawValue = equals == std::wstring_view::npos ? std::wstring_view{} : pair.substr(equals + 1);
		const std::wstring key = PercentDecodeQueryComponent(rawKey);
		if (EqualsIgnoreCase(key, requestedKey))
			return PercentDecodeQueryComponent(rawValue);

		if (end == std::wstring_view::npos)
			break;
		start = end + 1;
	}
	return {};
}

std::optional<ProtocolLaunchRequest> ParseProtocolLaunchUri(const winrt::Windows::Foundation::Uri& uri)
{
	const std::wstring rawUri{ uri.RawUri().c_str() };
	const size_t colon = rawUri.find(L':');
	if (colon == std::wstring::npos || !EqualsIgnoreCase(std::wstring_view(rawUri).substr(0, colon), L"cemu"))
	{
		WriteBootstrapLog(fmt::format("CemuUwp protocol activation rejected unexpected URI scheme: {}", winrt::to_string(uri.RawUri())));
		return {};
	}

	const size_t queryStart = rawUri.find(L'?', colon + 1);
	if (queryStart == std::wstring::npos)
	{
		WriteBootstrapLog("CemuUwp protocol activation missing query string");
		return {};
	}

	const size_t fragmentStart = rawUri.find(L'#', queryStart + 1);
	const std::wstring_view rawQuery(rawUri.c_str() + queryStart + 1,
									 (fragmentStart == std::wstring::npos ? rawUri.size() : fragmentStart) - queryStart - 1);

	ProtocolLaunchRequest request;
	if (const auto launchOnExit = TryGetQueryParameter(rawQuery, L"launchOnExit"))
		request.launchOnExit = Trim(*launchOnExit);

	const auto reject = [&request](std::string message) -> std::optional<ProtocolLaunchRequest>
	{
		WriteBootstrapLog(message);
		request.errorMessage = std::move(message);
		return request;
	};

	const auto cmdValue = TryGetQueryParameter(rawQuery, L"cmd");
	if (!cmdValue)
		return reject("Cemu protocol activation is missing the cmd parameter.");
	if (cmdValue->empty() || cmdValue->find(L'\0') != std::wstring::npos)
		return reject("Cemu protocol activation contains an invalid cmd parameter.");

	std::vector<std::wstring> arguments;
	try
	{
		arguments = boost::program_options::split_winmain(*cmdValue);
	}
	catch (const std::exception& ex)
	{
		return reject(fmt::format("Cemu protocol command could not be tokenized: {}", ex.what()));
	}

	if (!arguments.empty() && EqualsIgnoreCase(fs::path(arguments.front()).filename().wstring(), L"cemu.exe"))
		arguments.erase(arguments.begin());
	if (arguments.empty())
		return reject("Cemu protocol command does not contain launch arguments.");
	if (std::any_of(arguments.begin(), arguments.end(), [](const std::wstring& argument)
		{ return argument.find(L'\0') != std::wstring::npos; }))
		return reject("Cemu protocol command contains an invalid argument.");

	// launchers may pass a bare game path without -g
	if (!arguments.front().empty() && arguments.front().front() != L'-')
		arguments.insert(arguments.begin(), L"--game");

	LaunchSettings::ResetGameLaunchOverrides();
	std::string parseError;
	if (LaunchSettings::HandleCommandline(arguments, &parseError).has_value())
	{
		LaunchSettings::ResetGameLaunchOverrides();
		return reject(parseError.empty() ? "Cemu protocol command contains invalid launch arguments." : std::move(parseError));
	}

	const auto launchPath = LaunchSettings::GetLoadFile();
	if (!launchPath || launchPath->empty())
	{
		LaunchSettings::ResetGameLaunchOverrides();
		return reject("Cemu protocol command must provide a game path with -g or --game.");
	}
	request.launchPath = *launchPath;

	CEMU_UWP_BOOTSTRAP_TRACE(fmt::format("CemuUwp protocol activation parsed launch path: {}", _pathToUtf8(request.launchPath)));
	if (!request.launchOnExit.empty())
		CEMU_UWP_BOOTSTRAP_TRACE(fmt::format("CemuUwp protocol activation parsed launchOnExit: {}", winrt::to_string(winrt::hstring(request.launchOnExit.c_str()))));
	return request;
}

bool TryConsumePendingProtocolLaunch()
{
	std::optional<ProtocolLaunchRequest> request;
	{
		std::scoped_lock lock(g_protocolLaunchMutex);
		if (!g_pendingProtocolLaunch || !g_hostInitialized)
			return false;
		request = std::move(g_pendingProtocolLaunch);
		g_pendingProtocolLaunch.reset();
	}

	if (!request)
		return false;
	if (!request->errorMessage.empty())
	{
		WindowSystem::ShowErrorDialog(request->errorMessage);
		LaunchSettings::ResetGameLaunchOverrides();
		return true;
	}

	CEMU_UWP_BOOTSTRAP_TRACE(fmt::format("CemuUwp launching protocol path: {}", _pathToUtf8(request->launchPath)));
	if (!CemuHost::LaunchPath(request->launchPath))
	{
		WriteBootstrapLog("CemuUwp protocol launch failed");
		LaunchSettings::ResetGameLaunchOverrides();
		return true;
	}
	return true;
}

void QueueProtocolLaunch(ProtocolLaunchRequest request)
{
	{
		std::scoped_lock lock(g_protocolLaunchMutex);
		g_launchOnExitUri = request.launchOnExit;
		g_pendingProtocolLaunch = std::move(request);
	}
	TryConsumePendingProtocolLaunch();
}

void LaunchProtocolReturnUri()
{
	std::wstring launchOnExit;
	{
		std::scoped_lock lock(g_protocolLaunchMutex);
		launchOnExit = std::move(g_launchOnExitUri);
		g_launchOnExitUri.clear();
	}

	launchOnExit = Trim(std::move(launchOnExit));
	if (launchOnExit.empty())
		return;

	try
	{
		CEMU_UWP_BOOTSTRAP_TRACE(fmt::format("CemuUwp launching return URI: {}", winrt::to_string(winrt::hstring(launchOnExit.c_str()))));
		(void)winrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri(winrt::hstring(launchOnExit.c_str())));
	}
	catch (const winrt::hresult_error& ex)
	{
		WriteBootstrapLog(fmt::format("CemuUwp failed to launch return URI 0x{:08X}: {}", (uint32)ex.code(), winrt::to_string(ex.message())));
	}
	catch (const std::exception& ex)
	{
		WriteBootstrapLog(fmt::format("CemuUwp failed to launch return URI: {}", ex.what()));
	}
}

std::optional<std::string> TakeOneShotAutoLaunchRequest()
{
	try
	{
		const auto localFolder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
		fs::path requestPath{ std::wstring(localFolder.c_str()) };
		requestPath /= L"autolaunch.txt";
		std::error_code ec;
		if (!fs::exists(requestPath, ec))
			return std::nullopt;
		std::string line;
		bool tooLong = false;
		{
			std::ifstream file(requestPath, std::ios::binary);
			if (!file.is_open())
				return std::nullopt;
			std::array<char, 65537> buffer{};
			file.getline(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			tooLong = file.fail() && !file.eof();
			line.assign(buffer.data(), static_cast<size_t>(file.gcount()));
			if (!line.empty() && line.back() == '\0')
				line.pop_back();
		}
		if (!fs::remove(requestPath, ec) || ec)
		{
			WriteBootstrapLog("CemuUwp one-shot auto-launch request could not be consumed; launch skipped");
			return std::nullopt;
		}
		if (tooLong || line.find('\0') != std::string::npos)
		{
			WriteBootstrapLog("CemuUwp one-shot auto-launch request rejected: oversized line or embedded NUL");
			return std::nullopt;
		}
		// strip a UTF-8 BOM
		if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
			static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
			line.erase(0, 3);
		const auto isStrip = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"'; };
		while (!line.empty() && isStrip(line.back()))
			line.pop_back();
		size_t start = 0;
		while (start < line.size() && isStrip(line[start]))
			++start;
		line.erase(0, start);
		if (line.empty())
			return std::nullopt;
		WriteBootstrapLog("CemuUwp one-shot auto-launch request consumed");
		return line;
	}
	catch (...)
	{
		return std::nullopt;
	}
}

bool IsCemuProtocolRequest(std::string_view request)
{
	constexpr std::string_view scheme = "cemu:";
	if (request.size() < scheme.size())
		return false;
	for (size_t index = 0; index < scheme.size(); ++index)
	{
		char character = request[index];
		if (character >= 'A' && character <= 'Z')
			character += 'a' - 'A';
		if (character != scheme[index])
			return false;
	}
	return true;
}

void LaunchOneShotProtocolUri(std::string_view request)
{
	try
	{
		const winrt::Windows::Foundation::Uri uri{ winrt::to_hstring(request) };
		winrt::Windows::System::LauncherOptions options;
		options.TargetApplicationPackageFamilyName(winrt::Windows::ApplicationModel::Package::Current().Id().FamilyName());
		WriteBootstrapLog("CemuUwp one-shot URI: requesting OS protocol activation");
		auto operation = winrt::Windows::System::Launcher::LaunchUriAsync(uri, options);
		operation.Completed([](const auto& completed, winrt::Windows::Foundation::AsyncStatus status) noexcept
		{
			try
			{
				if (status == winrt::Windows::Foundation::AsyncStatus::Completed && completed.GetResults())
					WriteBootstrapLog("CemuUwp one-shot URI: OS protocol activation accepted");
				else
					WriteBootstrapLog("CemuUwp one-shot URI: OS protocol activation failed or was cancelled");
			}
			catch (...)
			{
				WriteBootstrapLog("CemuUwp one-shot URI: failed to read OS activation result");
			}
		});
	}
	catch (const winrt::hresult_error& ex)
	{
		WriteBootstrapLog(fmt::format("CemuUwp one-shot URI activation failed 0x{:08X}", (uint32)ex.code()));
	}
	catch (...)
	{
		WriteBootstrapLog("CemuUwp one-shot URI activation failed");
	}
}

std::optional<fs::path> FindPackagedAutoLaunchTitle()
{
	const auto titleRoot = CemuUwp::StorageService::InstalledContentRoot() / "titles";
	std::error_code ec;
	if (!fs::is_directory(titleRoot, ec))
		return {};

	std::optional<fs::path> firstWua;
	std::optional<fs::path> firstTitleDirectory;
	fs::directory_iterator end;
	for (fs::directory_iterator itr(titleRoot, ec); !ec && itr != end; itr.increment(ec))
	{
		const auto path = itr->path();
		if (itr->is_regular_file(ec))
		{
			const auto extension = path.extension().wstring();
			if (_wcsicmp(extension.c_str(), L".wua") == 0)
				firstWua = path;
		}
		else if (itr->is_directory(ec) &&
			fs::is_directory(path / "code", ec) &&
			fs::is_directory(path / "content", ec) &&
			fs::is_directory(path / "meta", ec))
		{
			if (!firstTitleDirectory)
				firstTitleDirectory = path;

			const auto folderName = path.filename().wstring();
			if (folderName.rfind(L"00050000", 0) == 0)
				return path;
		}
	}

	if (firstWua)
		return firstWua;
	return firstTitleDirectory;
}

void UpdateWindowMetrics(const winrt::Windows::UI::Core::CoreWindow& window)
{
	if (!window)
		return;

	const auto bounds = window.Bounds();
	const auto displayInfo = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
	double dpiScale = displayInfo.RawPixelsPerViewPixel();
	if (!std::isfinite(dpiScale) || dpiScale <= 0.0)
		dpiScale = displayInfo.LogicalDpi() / 96.0;
	if (!std::isfinite(dpiScale) || dpiScale <= 0.0)
		dpiScale = 1.0;
	const int width = std::max(1, (int)std::lround(bounds.Width));
	const int height = std::max(1, (int)std::lround(bounds.Height));
	const int physWidth = std::max(1, (int)std::lround(bounds.Width * dpiScale));
	const int physHeight = std::max(1, (int)std::lround(bounds.Height * dpiScale));
	const double pointerScaleX = bounds.Width > 0.0f ? (double)physWidth / bounds.Width : dpiScale;
	const double pointerScaleY = bounds.Height > 0.0f ? (double)physHeight / bounds.Height : dpiScale;

	g_windowInfo.width = width;
	g_windowInfo.height = height;
	g_windowInfo.phys_width = physWidth;
	g_windowInfo.phys_height = physHeight;
	g_windowInfo.dpi_scale = dpiScale;
	g_windowInfo.pad_open = false;
	g_windowInfo.pad_width = width;
	g_windowInfo.pad_height = height;
	g_windowInfo.phys_pad_width = physWidth;
	g_windowInfo.phys_pad_height = physHeight;
	g_windowInfo.pad_dpi_scale = dpiScale;
	g_windowInfo.window_main.backend = WindowSystem::WindowHandleInfo::Backend::WindowsCoreWindow;
	g_windowInfo.window_main.surface = winrt::get_abi(g_coreWindow);
	g_windowInfo.canvas_main = g_windowInfo.window_main;
	g_windowInfo.window_pad = g_windowInfo.window_main;
	g_windowInfo.canvas_pad = g_windowInfo.window_main;
	CemuUwp::InputProvider::SetPointerScale(pointerScaleX, pointerScaleY);
	g_resizePending = true;
}

struct CemuUwpApp : winrt::implements<CemuUwpApp, winrt::Windows::ApplicationModel::Core::IFrameworkView>
{
	winrt::Windows::UI::Core::CoreWindow m_window{ nullptr };
	winrt::Windows::Graphics::Display::DisplayInformation m_displayInformation{ nullptr };
	winrt::event_token m_dpiChangedToken{};
	bool m_closed = false;

	bool HandleFrameFailure(std::string message)
	{
		if (auto concurrentFailure = TakePendingGraphicsRecovery())
		{
			message += " | ";
			message += *concurrentFailure;
		}
		WriteBootstrapLog(fmt::format("CemuUwp frame failure: {}", message));
		if (CemuHost::HandleGraphicsFailure(message))
		{
			if (auto duplicateFailure = TakePendingGraphicsRecovery())
				WriteBootstrapLog(fmt::format("CemuUwp coalesced a failure from the retired graphics thread: {}", *duplicateFailure));
			WriteBootstrapLog("CemuUwp graphics recovery completed; returning to frontend");
			return true;
		}
		(void)TakePendingGraphicsRecovery();
		// a clean close leaves no crash dump, so end the process the way a crash does and let the console capture it
		WriteBootstrapLog("CemuUwp graphics recovery failed; ending the process so the console writes a crash dump");
		ExceptionHandler_HandleFatalTermination(fmt::format("unrecoverable graphics failure: {}", message));
		__fastfail(FAST_FAIL_FATAL_APP_EXIT);
	}

	bool HandlePendingGraphicsRecovery()
	{
		auto failure = TakePendingGraphicsRecovery();
		if (!failure)
			return false;
		HandleFrameFailure(std::move(*failure));
		return true;
	}

	void HandleFrontendFailure(std::string message)
	{
		if (CemuHost::IsGraphicsDeviceLost())
		{
			HandleFrameFailure(fmt::format("D3D12 device was lost while handling frontend work: {}", message));
			return;
		}

		WriteBootstrapLog(fmt::format("CemuUwp frontend failure (renderer remains active): {}", message));
		cemuLog_log(LogType::Force, "UWP frontend operation failed without D3D12 device loss: {}", message);
		try
		{
			CemuUwp::Frontend::PushNotification(fmt::format("Frontend operation failed: {}", message));
		}
		catch (...)
		{
		}
	}

	void Initialize(const winrt::Windows::ApplicationModel::Core::CoreApplicationView& view)
	{
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwpApp::Initialize begin");
		winrt::Windows::UI::ViewManagement::ApplicationView::PreferredLaunchWindowingMode(
			winrt::Windows::UI::ViewManagement::ApplicationViewWindowingMode::FullScreen);
		view.Activated({ this, &CemuUwpApp::OnActivated });
		winrt::Windows::ApplicationModel::Core::CoreApplication::Suspending({ this, &CemuUwpApp::OnSuspending });
		winrt::Windows::ApplicationModel::Core::CoreApplication::Resuming({ this, &CemuUwpApp::OnResuming });
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwpApp::Initialize done");
	}

	void SetWindow(const winrt::Windows::UI::Core::CoreWindow& window)
	{
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwpApp::SetWindow begin");
		m_window = window;
		CemuUwp::WindowHost::SetCoreWindow(window);
		if (m_displayInformation && m_dpiChangedToken)
			m_displayInformation.DpiChanged(m_dpiChangedToken);
		m_displayInformation = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
		m_dpiChangedToken = m_displayInformation.DpiChanged([this](auto&&, auto&&)
		{
			UpdateWindowMetrics(m_window);
		});

		window.SizeChanged([this](auto&& sender, auto&&)
		{
			UpdateWindowMetrics(sender);
		});
		window.VisibilityChanged([](auto&&, const winrt::Windows::UI::Core::VisibilityChangedEventArgs& args)
		{
			CemuUwp::WindowHost::SetVisible(args.Visible());
		});
		window.Closed([this](auto&&, auto&&)
		{
			m_closed = true;
		});
		winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView().BackRequested([](auto&&, const winrt::Windows::UI::Core::BackRequestedEventArgs& args)
		{
			args.Handled(true);
			CemuUwp::Frontend::RequestBack();
		});
		window.Activated([](auto&&, const winrt::Windows::UI::Core::WindowActivatedEventArgs& args)
		{
			CemuUwp::WindowHost::SetActive(args.WindowActivationState() != winrt::Windows::UI::Core::CoreWindowActivationState::Deactivated);
		});
		window.KeyDown([](auto&&, const winrt::Windows::UI::Core::KeyEventArgs& args)
		{
			CemuUwp::InputProvider::SetKeyState(args.VirtualKey(), true);
		});
		window.KeyUp([](auto&&, const winrt::Windows::UI::Core::KeyEventArgs& args)
		{
			CemuUwp::InputProvider::SetKeyState(args.VirtualKey(), false);
		});
		window.CharacterReceived([](auto&&, const winrt::Windows::UI::Core::CharacterReceivedEventArgs& args)
		{
			CemuUwp::InputProvider::AddTextCharacter(args.KeyCode());
		});
		window.PointerMoved([](auto&&, const winrt::Windows::UI::Core::PointerEventArgs& args)
		{
			CemuUwp::InputProvider::SetPointerState(args);
		});
		window.PointerPressed([](auto&&, const winrt::Windows::UI::Core::PointerEventArgs& args)
		{
			CemuUwp::InputProvider::SetPointerState(args);
		});
		window.PointerReleased([](auto&&, const winrt::Windows::UI::Core::PointerEventArgs& args)
		{
			CemuUwp::InputProvider::SetPointerState(args);
		});
		window.PointerExited([](auto&&, auto&&)
		{
			CemuUwp::InputProvider::ClearPointerState();
		});
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwpApp::SetWindow done");
	}

	void Load(const winrt::hstring&)
	{
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwpApp::Load");
	}

	void Run()
	{
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwpApp::Run begin");
		if (!CemuHost::Initialize(m_window))
		{
			WriteBootstrapLog("CemuUwpApp::Run host initialize failed");
			return;
		}
		g_hostInitialized = true;
		const bool protocolLaunchConsumed = TryConsumePendingProtocolLaunch();
		std::optional<std::string> oneShotProtocolUri;

		if (!protocolLaunchConsumed)
		{
			if (const auto oneShotLaunch = TakeOneShotAutoLaunchRequest())
			{
				CEMU_UWP_BOOTSTRAP_TRACE("CemuUwp launching one-shot auto-launch request");
				if (IsCemuProtocolRequest(*oneShotLaunch))
					oneShotProtocolUri = *oneShotLaunch;
				else
					CemuHost::LaunchPath(fs::path(reinterpret_cast<const char8_t*>(oneShotLaunch->c_str())));
			}
			else if constexpr (CEMU_UWP_ENABLE_AUTO_LAUNCH)
			{
				constexpr std::wstring_view autoLaunchPath{ CEMU_UWP_AUTO_LAUNCH_PATH };
				if (!autoLaunchPath.empty())
				{
					CemuHost::LaunchPath(fs::path(autoLaunchPath));
				}
				else if (const auto packagedTitle = FindPackagedAutoLaunchTitle())
				{
					CEMU_UWP_BOOTSTRAP_TRACE("CemuUwp auto-launching packaged title");
					CemuHost::LaunchPath(*packagedTitle);
				}
			}
		}

		auto dispatcher = m_window.Dispatcher();
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwpApp::Run loop begin");
		while (!m_closed)
		{
			if (HandlePendingGraphicsRecovery())
				continue;
			if (g_visible)
			{
				dispatcher.ProcessEvents(winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
				if (HandlePendingGraphicsRecovery())
					continue;
				if (oneShotProtocolUri)
				{
					// LaunchUriAsync needs a visible caller on the UI thread
					const auto uri = std::move(*oneShotProtocolUri);
					oneShotProtocolUri.reset();
					LaunchOneShotProtocolUri(uri);
				}
				bool presentedFrame = false;
				try
				{
					presentedFrame = CemuHost::TickFrontend();
				}
				catch (const CemuHost::GraphicsFailure& ex)
				{
					HandleFrameFailure(ex.what());
				}
				catch (const winrt::hresult_error& ex)
				{
					HandleFrontendFailure(fmt::format("WinRT error 0x{:08X}: {}", (uint32)ex.code(), winrt::to_string(ex.message())));
				}
				catch (const std::exception& ex)
				{
					HandleFrontendFailure(ex.what());
				}
				catch (...)
				{
					HandleFrontendFailure("unknown exception while processing a frontend frame");
				}
				if (m_closed)
					continue;
				if (!presentedFrame)
					std::this_thread::sleep_for(std::chrono::milliseconds(4));
			}
			else
			{
				dispatcher.ProcessEvents(winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessOneAndAllPending);
			}
		}

		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwpApp::Run shutdown begin");
		CemuHost::Shutdown();
		g_hostInitialized = false;
		(void)TakePendingGraphicsRecovery();
		LaunchProtocolReturnUri();
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwpApp::Run shutdown done");
	}

	void Uninitialize()
	{
		if (m_displayInformation && m_dpiChangedToken)
		{
			m_displayInformation.DpiChanged(m_dpiChangedToken);
			m_dpiChangedToken = {};
		}
		m_displayInformation = nullptr;
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwpApp::Uninitialize");
	}

	void OnActivated(const winrt::Windows::ApplicationModel::Core::CoreApplicationView&, const winrt::Windows::ApplicationModel::Activation::IActivatedEventArgs& args)
	{
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwpApp::OnActivated begin");
		if (args && args.Kind() == winrt::Windows::ApplicationModel::Activation::ActivationKind::Protocol)
		{
			try
			{
				const auto protocolArgs = args.as<winrt::Windows::ApplicationModel::Activation::ProtocolActivatedEventArgs>();
				CEMU_UWP_BOOTSTRAP_TRACE(fmt::format("CemuUwpApp::OnActivated protocol URI: {}", winrt::to_string(protocolArgs.Uri().RawUri())));
				if (auto request = ParseProtocolLaunchUri(protocolArgs.Uri()))
					QueueProtocolLaunch(std::move(*request));
			}
			catch (const winrt::hresult_error& ex)
			{
				WriteBootstrapLog(fmt::format("CemuUwpApp::OnActivated protocol parse failed 0x{:08X}: {}", (uint32)ex.code(), winrt::to_string(ex.message())));
			}
			catch (const std::exception& ex)
			{
				WriteBootstrapLog(fmt::format("CemuUwpApp::OnActivated protocol parse failed: {}", ex.what()));
			}
		}
		auto window = m_window;
		if (!window)
		{
			CEMU_UWP_BOOTSTRAP_TRACE("CemuUwpApp::OnActivated using current thread CoreWindow fallback");
			window = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread();
		}
		if (window)
			window.Activate();
		else
			WriteBootstrapLog("CemuUwpApp::OnActivated no CoreWindow available");
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwpApp::OnActivated done");
	}

	void OnSuspending(const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::ApplicationModel::SuspendingEventArgs&)
	{
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwpApp::OnSuspending");
		// a suspended app is often terminated without a clean title shutdown
		D3D12PipelineStableCache::GetInstance().SaveUsage();
		InputManager::instance().reset_uwp_expression_state();
		CemuUwp::WindowHost::SetActive(false);
	}

	void OnResuming(const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::Foundation::IInspectable&)
	{
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwpApp::OnResuming");
		UpdateWindowMetrics(m_window);
		CemuUwp::WindowHost::SetActive(true);
	}
};

struct CemuUwpAppSource : winrt::implements<CemuUwpAppSource, winrt::Windows::ApplicationModel::Core::IFrameworkViewSource>
{
	winrt::Windows::ApplicationModel::Core::IFrameworkView CreateView()
	{
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwpAppSource::CreateView");
		return winrt::make<CemuUwpApp>();
	}
};
}

namespace CemuUwp::WindowHost
{
void SetCoreWindow(const winrt::Windows::UI::Core::CoreWindow& window)
{
	g_coreWindow = window;
	g_coreDispatcher = winrt::make_agile(window.Dispatcher());
	g_windowInfo.app_active = true;
	g_windowInfo.is_fullscreen = false;
	g_windowInfo.debugger_focused = false;
	UpdateWindowMetrics(window);
}

void SetVisible(bool visible)
{
	g_visible = visible;
	CemuHost::SetWindowVisible(visible);
}

void SetActive(bool active)
{
	g_windowInfo.app_active = active;
	CemuHost::SetWindowActive(active);
}

bool ConsumeResize()
{
	return g_resizePending.exchange(false);
}

bool HasLaunchOnExitUri()
{
	std::scoped_lock lock(g_protocolLaunchMutex);
	return !Trim(g_launchOnExitUri).empty();
}

void RecordCaughtAllocationFailure() noexcept
{
#if defined(CEMU_UWP_ALLOCATION_DIAGNOSTICS)
	if (g_newHandlerCapturedThisThread)
	{
		WriteAllocationDiagnosticRecord("latte_caught_after_operator_new", 0, false);
		g_newHandlerCapturedThisThread = false;
	}
	else
	{
		WriteAllocationDiagnosticRecord("latte_caught_without_operator_new", 0, true);
	}
#endif
}

void RequestGameplayGraphicsRecovery(std::string message) noexcept
{
	bool shouldWakeDispatcher = false;
	try
	{
		std::scoped_lock lock(g_graphicsRecoveryMutex);
		if (!g_pendingGraphicsRecovery)
		{
			g_pendingGraphicsRecovery = std::move(message);
			g_coalescedGraphicsRecoveryRequests = 0;
			g_graphicsRecoveryPending.store(true, std::memory_order_release);
			shouldWakeDispatcher = true;
		}
		else
		{
			++g_coalescedGraphicsRecoveryRequests;
		}
	}
	catch (...)
	{
		return;
	}

	if (shouldWakeDispatcher)
		WakeCoreWindowDispatcher();
	WriteBootstrapLog("CemuUwp queued gameplay-thread graphics recovery on the CoreWindow thread");
}
}

void WindowSystem::Create()
{
	CEMU_UWP_BOOTSTRAP_TRACE("WindowSystem::Create CoreApplication::Run begin");
	winrt::Windows::ApplicationModel::Core::CoreApplication::Run(winrt::make<CemuUwpAppSource>());
	CEMU_UWP_BOOTSTRAP_TRACE("WindowSystem::Create CoreApplication::Run returned");
}

void WindowSystem::ShowErrorDialog(std::string_view message, std::string_view, std::optional<WindowSystem::ErrorCategory>)
{
	CemuUwp::Frontend::PushNotification(std::string(message));
}

WindowSystem::WindowInfo& WindowSystem::GetWindowInfo()
{
	return g_windowInfo;
}

void WindowSystem::UpdateWindowTitles(bool, bool, double) {}

void WindowSystem::GetWindowSize(int& w, int& h)
{
	w = g_windowInfo.width;
	h = g_windowInfo.height;
}

void WindowSystem::GetPadWindowSize(int& w, int& h)
{
	w = g_windowInfo.pad_width;
	h = g_windowInfo.pad_height;
}

void WindowSystem::GetWindowPhysSize(int& w, int& h)
{
	w = g_windowInfo.phys_width;
	h = g_windowInfo.phys_height;
}

void WindowSystem::GetPadWindowPhysSize(int& w, int& h)
{
	w = g_windowInfo.phys_pad_width;
	h = g_windowInfo.phys_pad_height;
}

double WindowSystem::GetWindowDPIScale()
{
	return g_windowInfo.dpi_scale;
}

double WindowSystem::GetPadDPIScale()
{
	return g_windowInfo.pad_dpi_scale;
}

bool WindowSystem::IsPadWindowOpen()
{
	return false;
}

bool WindowSystem::IsKeyDown(uint32 key)
{
	return g_windowInfo.get_keystate(key);
}

bool WindowSystem::IsKeyDown(PlatformKeyCodes key)
{
	using winrt::Windows::System::VirtualKey;
	switch (key)
	{
	case PlatformKeyCodes::LCONTROL:
	case PlatformKeyCodes::RCONTROL:
		return IsKeyDown((uint32)VirtualKey::Control);
	case PlatformKeyCodes::TAB:
		return IsKeyDown((uint32)VirtualKey::Tab);
	case PlatformKeyCodes::ESCAPE:
		return IsKeyDown((uint32)VirtualKey::Escape);
	}
	return false;
}

std::string WindowSystem::GetKeyCodeName(uint32 key)
{
	return fmt::format("Key {}", key);
}

bool WindowSystem::InputConfigWindowHasFocus()
{
	return false;
}

void WindowSystem::NotifyGameLoaded()
{
	CemuUwp::Frontend::SetStatus("Title running");
}

void WindowSystem::NotifyGameExited()
{
	CemuUwp::Frontend::SetStatus("Ready");
}

void WindowSystem::RefreshGameList() {}

bool WindowSystem::IsFullScreen()
{
	return false;
}

void WindowSystem::CaptureInput(const ControllerState&, const ControllerState&) {}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	try
	{
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwp bootstrap starting");
		InstallProcessDiagnostics();
		try
		{
			CEMU_UWP_BOOTSTRAP_TRACE("CemuUwp init_apartment begin");
			winrt::init_apartment(winrt::apartment_type::single_threaded);
			CEMU_UWP_BOOTSTRAP_TRACE("CemuUwp init_apartment done");
		}
		catch (const winrt::hresult_error& ex)
		{
			if (static_cast<HRESULT>(ex.code()) != RPC_E_CHANGED_MODE)
				throw;
			CEMU_UWP_BOOTSTRAP_TRACE("CemuUwp bootstrap using existing COM apartment");
		}
#if defined(CEMU_UWP_ALLOCATION_DIAGNOSTICS)
		InstallAllocationDiagnostics();
#endif
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwp WindowSystem::Create begin");
		WindowSystem::Create();
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwp WindowSystem::Create done");
		CEMU_UWP_BOOTSTRAP_TRACE("CemuUwp bootstrap exited");
	}
	catch (const winrt::hresult_error& ex)
	{
		WriteBootstrapLog(fmt::format("CemuUwp bootstrap WinRT exception 0x{:08X}: {}",
			static_cast<uint32_t>(ex.code()),
			winrt::to_string(ex.message())));
	}
	catch (const std::exception& ex)
	{
		WriteBootstrapLog(fmt::format("CemuUwp bootstrap exception: {}", ex.what()));
	}
	catch (...)
	{
		WriteBootstrapLog("CemuUwp bootstrap unknown exception");
	}
	return 0;
}
