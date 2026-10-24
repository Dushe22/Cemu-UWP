#include "CemuLogging.h"
#include "Common/precompiled.h"
#include "util/helpers/helpers.h"
#include "config/CemuConfig.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"

#include <mutex>
#include <condition_variable>
#include <chrono>

#include <fmt/printf.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

uint64 s_loggingFlagMask = cemuLog_getFlag(LogType::Force);

class LoggingDispatcher
{
  private:
	static inline class DefaultLoggingCallbacks : public LoggingCallbacks
	{
	} s_defaultCallbacks;

	LoggingCallbacks* m_loggingCallbacks = &s_defaultCallbacks;
	std::shared_mutex m_mutex;

  public:
	void Log(std::string_view filter, std::string_view message)
	{
		std::shared_lock lock(m_mutex);
		m_loggingCallbacks->Log(filter, message);
	}

	void Log(std::string_view message)
	{
		Log("", message);
	}

	void Log(std::string_view filter, std::wstring_view message)
	{
		std::shared_lock lock(m_mutex);
		m_loggingCallbacks->Log(filter, message);
	}

	void Log(std::wstring_view message)
	{
		Log("", message);
	}

	void setCallbacks(LoggingCallbacks* loggingCallbacks)
	{
		std::unique_lock lock(m_mutex);
		cemu_assert_debug(m_loggingCallbacks == &s_defaultCallbacks);
		m_loggingCallbacks = loggingCallbacks;
	}

	void clearCallbacks()
	{
		std::unique_lock lock(m_mutex);
		cemu_assert_debug(m_loggingCallbacks != &s_defaultCallbacks);
		m_loggingCallbacks = &s_defaultCallbacks;
	}
} s_loggingDispatcher;

void cemuLog_setCallbacks(LoggingCallbacks* loggingCallbacks)
{
	s_loggingDispatcher.setCallbacks(loggingCallbacks);
}

void cemuLog_clearCallbacks()
{
	s_loggingDispatcher.clearCallbacks();
}

// On Xbox the log stopped growing while the game ran: lines written through a handle kept open for the whole session
// never reached log.txt, without any error, and later opening log.txt for each batch started failing. Each batch now
// opens, appends and closes the file, checks its size and keeps the lines when the write fails. A writer that stalls
// inside a file operation, or cannot write its file for a while, is replaced by one that continues in log_<n>.txt.
enum class LogWriterPhase : int { Waiting, Opening, Writing, Flushing };
static constexpr uint32 kMaxLogWriters = 8; // log.txt plus log_1.txt .. log_7.txt
static constexpr sint64 kLogWriterStallMs = 5000; // inside a file operation
static constexpr sint64 kLogWakeStallMs = 10000; // lines queued but the writer never woke up
static constexpr sint64 kLogFailSwitchMs = 2000; // writes failing this long move on to the next file
static constexpr size_t kLogMaxKeptEntries = 60000; // failed entries kept for a retry (~20k lines)

struct LogWriter
{
	uint32 index = 0; // 0 = log.txt, n = log_<n>.txt
	uintmax_t expectedSize = 0; // size of the file after our last write
	std::string pendingNote; // written with the next successful batch
	size_t lostEntries = 0; // failed entries dropped because too many were kept
	std::atomic<uint32> failures{ 0 }; // failed writes since the last success (read by the watchdog)
	sint64 failingSinceMs = 0;
	uint32 lastError = 0;
	std::atomic<int> phase{ (int)LogWriterPhase::Waiting };
	std::atomic<sint64> phaseSinceMs{ 0 };
	std::atomic<size_t> batchLines{ 0 };
	bool abandoned = false; // guarded by log_mutex
};

struct _LogContext
{
	std::condition_variable_any log_condition;
	std::recursive_mutex log_mutex;
	LogWriter* writer = nullptr; // current writer, guarded by log_mutex; abandoned ones free themselves if they return
	std::vector<std::string> text_cache;
	std::chrono::steady_clock::time_point oldest_queued{}; // when the first line still in text_cache was queued
	std::thread log_writer;
	std::atomic<bool> threadRunning = false;

	~_LogContext()
	{
		// safely shut down the log writer thread
		threadRunning.store(false);
		if (log_writer.joinable())
		{
			log_condition.notify_all();
			// a writer stuck in a file operation would block exit forever
			if (writer && writer->phase.load() == (int)LogWriterPhase::Waiting)
				log_writer.join();
			else
				log_writer.detach();
		}
	}
}LogContext;

const std::map<LogType, std::string> g_logging_window_mapping
{
	{LogType::UnsupportedAPI,     "Unsupported API calls"},
	{LogType::APIErrors, 		  "Invalid API usage"},
	{LogType::CoreinitLogging,    "Coreinit Logging"},
	{LogType::CoreinitFile,       "Coreinit File-Access"},
	{LogType::CoreinitThreadSync, "Coreinit Thread-Synchronization"},
	{LogType::CoreinitMem,        "Coreinit Memory"},
	{LogType::CoreinitMP,         "Coreinit MP"},
	{LogType::CoreinitThread,     "Coreinit Thread"},
	{LogType::NN_NFP,             "nn::nfp"},
	{LogType::NN_FP,              "nn::fp"},
	{LogType::NN_BOSS,            "nn::boss"},
	{LogType::GX2,                "GX2"},
	{LogType::SoundAPI,           "Audio"},
	{LogType::InputAPI,           "Input"},
	{LogType::Socket,             "Socket"},
	{LogType::Save,               "Save"},
	{LogType::H264,               "H264"},
	{LogType::NFC,                "NFC"},
	{LogType::NTAG,               "NTAG"},
	{LogType::Patches,            "Graphic pack patches"},
	{LogType::TextureCache,       "Texture cache"},
	{LogType::TextureReadback,    "Texture readback"},
	{LogType::OpenGLLogging,      "OpenGL debug output"},
	{LogType::VulkanValidation,   "Vulkan validation layer"},
};

bool cemuLog_advancedPPCLoggingEnabled()
{
	return GetConfig().advanced_ppc_logging;
}

static sint64 cemuLog_nowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static fs::path cemuLog_writerPath(uint32 index)
{
	return index == 0 ? cemuLog_GetLogFilePath() : ActiveSettings::GetUserDataPath(fmt::format("log_{}.txt", index));
}

static const char* cemuLog_phaseName(int phase)
{
	switch ((LogWriterPhase)phase)
	{
	case LogWriterPhase::Opening: return "opening the file";
	case LogWriterPhase::Writing: return "in a file write";
	case LogWriterPhase::Flushing: return "in a file flush";
	default: return "waiting to be scheduled (lines were queued)";
	}
}

static void cemuLog_setPhase(LogWriter* w, LogWriterPhase phase)
{
	w->phaseSinceMs.store(cemuLog_nowMs());
	w->phase.store((int)phase);
}

static std::string cemuLog_errorText(uint32 error)
{
	return fmt::format("error {}: {}", error, std::system_category().message((int)error));
}

// appends one batch with its own open/close; returns 0, or the OS error that stopped it (nothing was written then)
static uint32 cemuLog_appendBatch(LogWriter* w, const fs::path& path, const std::vector<std::string>& entries)
{
	const std::string name = path.filename().string();
	cemuLog_setPhase(w, LogWriterPhase::Opening);
#if defined(_WIN32)
	HANDLE file = CreateFile2(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_ALWAYS, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return (std::max<uint32>)(GetLastError(), 1);
	LARGE_INTEGER size{};
	const bool sizeKnown = GetFileSizeEx(file, &size) != FALSE;
	const uintmax_t sizeBefore = sizeKnown ? (uintmax_t)size.QuadPart : 0;
#else
	std::error_code ec;
	const uintmax_t sizeBefore = fs::file_size(path, ec);
	const bool sizeKnown = !ec;
	std::ofstream file(path, std::ios::out | std::ios::app | std::ios::binary);
	if (!file)
		return (std::max)(errno, 1);
#endif

	std::string data = w->pendingNote;
	if (sizeKnown && sizeBefore != w->expectedSize)
		data += fmt::format("[log] {} was {} bytes before this write, expected {}: earlier output was lost or the file was changed\n",
			name, sizeBefore, w->expectedSize);
	if (w->lostEntries != 0)
		data += fmt::format("[log] {} log entries before this point were dropped after failed writes\n", w->lostEntries);
	for (const auto& entry : entries)
		data += entry;

	cemuLog_setPhase(w, LogWriterPhase::Writing);
#if defined(_WIN32)
	DWORD written = 0;
	uint32 error = 0;
	if (!WriteFile(file, data.data(), (DWORD)data.size(), &written, nullptr) || written != data.size())
		error = (std::max<uint32>)(GetLastError(), 1);
	cemuLog_setPhase(w, LogWriterPhase::Flushing);
	const bool sizeAfterKnown = GetFileSizeEx(file, &size) != FALSE;
	const uintmax_t sizeAfter = sizeAfterKnown ? (uintmax_t)size.QuadPart : 0;
	CloseHandle(file);
	if (error != 0)
		return error; // a partial write shows up as a size mismatch next time
#else
	file.write(data.data(), data.size());
	cemuLog_setPhase(w, LogWriterPhase::Flushing);
	file.close();
	if (file.fail())
		return (std::max)(errno, 1);
	const uintmax_t sizeAfter = fs::file_size(path, ec);
	const bool sizeAfterKnown = !ec;
#endif
	w->pendingNote.clear();
	w->lostEntries = 0;
	const uintmax_t expected = sizeBefore + data.size();
	if (sizeKnown && sizeAfterKnown && sizeAfter != expected)
		w->pendingNote = fmt::format("[log] {} is {} bytes after a {}-byte write, expected {}\n", name, sizeAfter, data.size(), expected);
	w->expectedSize = sizeAfterKnown ? sizeAfter : expected;
	return 0;
}

void cemuLog_thread(LogWriter* w);

// called with log_mutex held: a new writer continues in the next file, starting with the note
static void cemuLog_replaceWriterLocked(LogWriter* w, std::string note)
{
	auto* next = new LogWriter();
	next->index = w->index + 1;
	LogContext.text_cache.insert(LogContext.text_cache.begin(), std::move(note));
	if (LogContext.oldest_queued.time_since_epoch().count() == 0)
		LogContext.oldest_queued = std::chrono::steady_clock::now();
	w->abandoned = true;
	LogContext.writer = next;
	LogContext.log_writer.detach();
	LogContext.log_writer = std::thread(cemuLog_thread, next);
	LogContext.log_condition.notify_all();
}

void cemuLog_thread(LogWriter* w)
{
	SetThreadName("cemuLog_thread");
	const fs::path path = cemuLog_writerPath(w->index);
	while (true)
	{
		std::unique_lock lock(LogContext.log_mutex);
		while (LogContext.text_cache.empty() || w->abandoned)
		{
			if (w->abandoned)
			{
				// replaced; the new writer owns the queue
				lock.unlock();
				delete w;
				return;
			}
			LogContext.log_condition.wait(lock);
			if (!LogContext.threadRunning.load(std::memory_order::relaxed))
				return;
		}

		std::vector<std::string> cache_copy;
		cache_copy.swap(LogContext.text_cache); // fast copy & clear
		const auto oldestQueued = std::exchange(LogContext.oldest_queued, {});
		w->batchLines.store(cache_copy.size());
		cemuLog_setPhase(w, LogWriterPhase::Writing);
		lock.unlock();

		const auto writeStarted = std::chrono::steady_clock::now();
		const std::string notesBefore = w->pendingNote;
		if (w->failures != 0)
			w->pendingNote += fmt::format("[log] the lines below waited for {} failed writes over {} ms (last {})\n",
				w->failures.load(), cemuLog_nowMs() - w->failingSinceMs, cemuLog_errorText(w->lastError));
		const uint32 error = cemuLog_appendBatch(w, path, cache_copy);
		cemuLog_setPhase(w, LogWriterPhase::Waiting);
		if (error != 0)
		{
			w->pendingNote = notesBefore;
			const sint64 now = cemuLog_nowMs();
			if (w->failures++ == 0)
				w->failingSinceMs = now;
			w->lastError = error;
			lock.lock();
			// keep the batch in front of newer lines for the next attempt
			if (cache_copy.size() + LogContext.text_cache.size() > kLogMaxKeptEntries)
			{
				const size_t drop = (std::min)(cache_copy.size(), cache_copy.size() + LogContext.text_cache.size() - kLogMaxKeptEntries);
				cache_copy.erase(cache_copy.begin(), cache_copy.begin() + drop);
				w->lostEntries += drop;
			}
			LogContext.text_cache.insert(LogContext.text_cache.begin(), std::make_move_iterator(cache_copy.begin()), std::make_move_iterator(cache_copy.end()));
			if (oldestQueued.time_since_epoch().count() != 0)
				LogContext.oldest_queued = oldestQueued;
			if (now - w->failingSinceMs >= kLogFailSwitchMs && w->index + 1 < kMaxLogWriters && !w->abandoned)
				cemuLog_replaceWriterLocked(w, fmt::format("[log] {} could not be written for {} ms ({} attempts, last {}); continuing in this file\n",
					path.filename().string(), now - w->failingSinceMs, w->failures.load(), cemuLog_errorText(error)));
			lock.unlock();
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}
		w->failures = 0;
		// tell a blocked file write from a starved thread
		const auto writeEnded = std::chrono::steady_clock::now();
		const auto writeMs = std::chrono::duration_cast<std::chrono::milliseconds>(writeEnded - writeStarted).count();
		const auto waitMs = oldestQueued.time_since_epoch().count() != 0 ?
			std::chrono::duration_cast<std::chrono::milliseconds>(writeStarted - oldestQueued).count() : 0;
		if (writeMs >= 1000 || waitMs >= 1000)
			w->pendingNote += fmt::format("[log] {} entries written {} ms after the first was queued; the write took {} ms\n",
				cache_copy.size(), waitMs, writeMs);
		// each batch opens and closes the file: let lines gather instead of doing that for every single line
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
}

// called with log_mutex held: replace a writer that stopped making progress
static void cemuLog_checkWriterLocked()
{
	LogWriter* w = LogContext.writer;
	if (!w || w->index + 1 >= kMaxLogWriters || !LogContext.threadRunning.load(std::memory_order::relaxed))
		return;
	const int phase = w->phase.load();
	sint64 stalledMs = 0;
	if (phase != (int)LogWriterPhase::Waiting)
	{
		stalledMs = cemuLog_nowMs() - w->phaseSinceMs.load();
		if (stalledMs < kLogWriterStallMs)
			return;
	}
	else
	{
		// a writer retrying failed writes handles that itself
		if (w->failures != 0 || LogContext.text_cache.empty() || LogContext.oldest_queued.time_since_epoch().count() == 0)
			return;
		stalledMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - LogContext.oldest_queued).count();
		if (stalledMs < kLogWakeStallMs)
			return;
	}
	cemuLog_replaceWriterLocked(w, fmt::format("[log] the writer of {} stalled {} for {} ms{}; continuing in this file\n",
		cemuLog_writerPath(w->index).filename().string(), cemuLog_phaseName(phase), stalledMs,
		phase == (int)LogWriterPhase::Waiting ? std::string() :
			fmt::format(" (its batch of {} log entries is missing here and only reaches that file if the write ever completes)", w->batchLines.load())));
}

fs::path cemuLog_GetLogFilePath()
{
    return ActiveSettings::GetUserDataPath("log.txt");
}

fs::path cemuLog_GetActiveLogFilePath()
{
	std::unique_lock lock(LogContext.log_mutex);
	return cemuLog_writerPath(LogContext.writer ? LogContext.writer->index : 0);
}

void cemuLog_createLogFile(bool triggeredByCrash)
{
	std::unique_lock lock(LogContext.log_mutex);
	if (LogContext.writer)
		return;

	const auto path = cemuLog_GetLogFilePath();
#if defined(CEMU_UWP)
	// problems are often reported after a relaunch, so keep the previous session next to the new one
	if (!triggeredByCrash)
	{
		std::error_code ec;
		fs::copy_file(path, fs::path(path).replace_filename("log_prev.txt"), fs::copy_options::overwrite_existing, ec);
	}
#endif
	// replacement files from the previous session
	for (uint32 i = 1; i < kMaxLogWriters; i++)
	{
		std::error_code ec;
		fs::remove(cemuLog_writerPath(i), ec);
	}
	{
		std::ofstream truncate(path, std::ios::out | std::ios::binary);
		if (!truncate)
		{
			cemu_assert_debug(false);
			return;
		}
	}
	auto* w = new LogWriter();

	LogContext.writer = w;
	LogContext.threadRunning.store(true);
	LogContext.log_writer = std::thread(cemuLog_thread, w);
	lock.unlock();
}

void cemuLog_writeLineToLog(std::string_view text, bool date, bool new_line)
{
	std::unique_lock lock(LogContext.log_mutex);
	if (LogContext.oldest_queued.time_since_epoch().count() == 0)
		LogContext.oldest_queued = std::chrono::steady_clock::now();

	if (date)
	{
		const auto now = std::chrono::system_clock::now();
		const auto temp_time = std::chrono::system_clock::to_time_t(now);
		const auto& time = *std::localtime(&temp_time);

		auto time_str = fmt::format("[{:02d}:{:02d}:{:02d}.{:03d}] ", time.tm_hour, time.tm_min, time.tm_sec,
			std::chrono::duration_cast<std::chrono::milliseconds>(now - std::chrono::time_point_cast<std::chrono::seconds>(now)).count());

		LogContext.text_cache.emplace_back(std::move(time_str));
	}

	LogContext.text_cache.emplace_back(text);

	if (new_line)
		LogContext.text_cache.emplace_back("\n");

	cemuLog_checkWriterLocked();
	lock.unlock();
	LogContext.log_condition.notify_one();
}

bool cemuLog_log(LogType type, std::string_view text)
{
	if (!cemuLog_isLoggingEnabled(type))
		return false;

	if (LaunchSettings::Verbose())
		std::cout << text << std::endl;

	cemuLog_writeLineToLog(text);

	const auto it = std::find_if(g_logging_window_mapping.cbegin(), g_logging_window_mapping.cend(),
		[type](const auto& entry) { return entry.first == type; });
	if (it == g_logging_window_mapping.cend())
		s_loggingDispatcher.Log(text);
	else
		s_loggingDispatcher.Log(it->second, text);

	return true;
}

bool cemuLog_log(LogType type, std::u8string_view text)
{
	std::basic_string_view<char> s((char*)text.data(), text.size());
	return cemuLog_log(type, s);
}

void cemuLog_logHexDump(LogType type, const void* data, size_t size, size_t lineSize)
{
	const uint8* dataU8 = static_cast<const uint8*>(data);
	std::vector<char> hexLine;
	hexLine.resize(6 + lineSize * 3 + 1, ' ');
	for (size_t i = 0; i < size; i += lineSize)
	{
		sprintf(hexLine.data(), "%04X: ", (int)i);
		size_t remainingSize = std::min<size_t>(lineSize, size - i);
		for (size_t j=0; j<remainingSize; j++)
			sprintf(hexLine.data() + 6 + j * 3, "%02X ", dataU8[j]);
		dataU8 += remainingSize;
		if (remainingSize == lineSize)
			hexLine.resize(6 + remainingSize * 3 + 1);
		cemuLog_log(type, hexLine.data());
	}
}

void cemuLog_waitForFlush()
{
	cemuLog_createLogFile(false);
	std::unique_lock lock(LogContext.log_mutex);
	// callers include the render thread and the crash handler: never wait forever on a stuck writer
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while(!LogContext.text_cache.empty() && std::chrono::steady_clock::now() < deadline)
	{
		cemuLog_checkWriterLocked();
		lock.unlock();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		std::this_thread::yield();
		lock.lock();
	}
}

// used to atomically write multiple lines to the log
std::unique_lock<decltype(LogContext.log_mutex)> cemuLog_acquire()
{
	return std::unique_lock(LogContext.log_mutex);
}

void cemuLog_setActiveLoggingFlags(uint64 flagMask)
{
	s_loggingFlagMask = flagMask | cemuLog_getFlag(LogType::Force);
}
