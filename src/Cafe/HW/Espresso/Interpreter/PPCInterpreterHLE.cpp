#include "../PPCState.h"
#include "Common/UwpProfiling.h"
#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"

std::unordered_set<std::string> s_unsupportedHLECalls;

void PPCInterpreter_handleUnsupportedHLECall(PPCInterpreter_t* hCPU)
{
	const char* libFuncName = (char*)memory_getPointerFromVirtualOffset(hCPU->instructionPointer + 8);
	std::string tempString = fmt::format("Unsupported lib call: {}", libFuncName);
	if (s_unsupportedHLECalls.find(tempString) == s_unsupportedHLECalls.end())
	{
		cemuLog_log(LogType::UnsupportedAPI, "{}", tempString);
		s_unsupportedHLECalls.emplace(tempString);
	}
	hCPU->gpr[3] = 0;
	PPCInterpreter_nextInstruction(hCPU);
}

static constexpr size_t HLE_TABLE_CAPACITY = 0x4000;
HLECALL s_ppcHleTable[HLE_TABLE_CAPACITY]{};
sint32 s_ppcHleTableWriteIndex = 0;
std::mutex s_ppcHleTableMutex;

HLEIDX PPCInterpreter_registerHLECall(HLECALL hleCall, std::string hleName)
{
	std::unique_lock _l(s_ppcHleTableMutex);
	if (s_ppcHleTableWriteIndex >= HLE_TABLE_CAPACITY)
	{
		cemuLog_log(LogType::Force, "HLE table is full");
		cemu_assert(false);
	}
	for (sint32 i = 0; i < s_ppcHleTableWriteIndex; i++)
	{
		if (s_ppcHleTable[i] == hleCall)
		{
			return i;
		}
	}
	cemu_assert(s_ppcHleTableWriteIndex < HLE_TABLE_CAPACITY);
	s_ppcHleTable[s_ppcHleTableWriteIndex] = hleCall;
#if defined(CEMU_UWP)
	extern std::string s_uwpHleNames[];
	s_uwpHleNames[s_ppcHleTableWriteIndex] = hleName;
#endif
	HLEIDX funcIndex = s_ppcHleTableWriteIndex;
	s_ppcHleTableWriteIndex++;
	return funcIndex;
}

HLECALL PPCInterpreter_getHLECall(HLEIDX funcIndex)
{
	if (funcIndex < 0 || funcIndex >= HLE_TABLE_CAPACITY)
		return nullptr;
	return s_ppcHleTable[funcIndex];
}

std::mutex s_hleLogMutex;

void PPCInterpreter_virtualHLE(PPCInterpreter_t* hCPU, unsigned int opcode)
{
	uint32 hleFuncId = opcode & 0xFFFF;
	if (hleFuncId == 0xFFD0) [[unlikely]]
	{
		s_hleLogMutex.lock();
		PPCInterpreter_handleUnsupportedHLECall(hCPU);
		s_hleLogMutex.unlock();
	}
	else
	{
		// os lib function
		auto hleCall = PPCInterpreter_getHLECall(hleFuncId);
		cemu_assert(hleCall);
#if defined(CEMU_UWP)
		if (g_uwpDetailedProfiling)
		{
			UwpCpuProfile::NoteHleCall((sint32)hleFuncId);
			const auto previous = UwpCpuProfile::Enter(UwpCpuProfile::Hle, (sint32)hleFuncId);
			hleCall(hCPU);
			UwpCpuProfile::Restore(previous);
		}
		else
			hleCall(hCPU);
#else
		hleCall(hCPU);
#endif
	}
}

#if defined(CEMU_UWP)
#include <intrin.h>
std::string s_uwpHleNames[HLE_TABLE_CAPACITY];

namespace UwpCpuProfile
{
	namespace
	{
		constexpr sint32 kCores = 3;
		thread_local sint32 t_core = -1;
		thread_local uint64 t_mark = 0;
		thread_local State t_state{ Scheduler, -1 };
		thread_local uint64 t_threadSince = 0;
		std::atomic<uint64> s_coreTicks[kCores][CategoryCount]{};
		std::atomic<uint64> s_hleTicks[HLE_TABLE_CAPACITY]{};
		std::atomic<uint64> s_hleCalls[HLE_TABLE_CAPACITY]{};
		struct ThreadTicks { std::string name; uint64 ticks = 0; sint32 core = -1; };
		std::mutex s_threadMutex;
		std::unordered_map<const void*, ThreadTicks> s_threadTicks;
		// blocking waits: per thread the open wait, then totals per (thread, HLE call, waker)
		struct OpenWait { uint64 start = 0; uint64 woken = 0; sint32 hleId = -1; std::string waker; };
		std::unordered_map<const void*, OpenWait> s_openWaits;
		struct WaitTotal { std::string thread, call, waker; uint64 waitTicks = 0, readyTicks = 0, count = 0; };
		std::unordered_map<std::string, WaitTotal> s_waitTotals;
		uint64 s_lastLog = 0;

		void Charge(uint64 now)
		{
			if (t_core < 0)
				return;
			const uint64 elapsed = now - t_mark;
			t_mark = now;
			s_coreTicks[t_core][t_state.category].fetch_add(elapsed, std::memory_order_relaxed);
			if (t_state.category == Hle && t_state.hleId >= 0 && t_state.hleId < (sint32)HLE_TABLE_CAPACITY)
				s_hleTicks[t_state.hleId].fetch_add(elapsed, std::memory_order_relaxed);
		}
	}

	void SetCore(sint32 core)
	{
		t_core = core >= 0 && core < kCores ? core : -1;
		t_mark = __rdtsc();
		t_state = { Scheduler, -1 };
	}

	State Enter(uint32 category, sint32 hleId)
	{
		if (!g_uwpDetailedProfiling)
			return t_state;
		Charge(__rdtsc());
		const State previous = t_state;
		t_state = { category, hleId };
		return previous;
	}

	void Restore(State state)
	{
		if (!g_uwpDetailedProfiling)
			return;
		Charge(__rdtsc());
		t_state = state;
	}

	void NoteHleCall(sint32 hleId)
	{
		if (t_core >= 0 && hleId >= 0 && hleId < (sint32)HLE_TABLE_CAPACITY)
			s_hleCalls[hleId].fetch_add(1, std::memory_order_relaxed);
	}

	void NoteThreadSwitchOut(const void* osThread, const char* name, bool waiting, sint32 hleId)
	{
		if (!g_uwpDetailedProfiling || t_core < 0 || !osThread)
			return;
		const uint64 now = __rdtsc();
		std::scoped_lock lock(s_threadMutex);
		if (t_threadSince != 0)
		{
			auto& entry = s_threadTicks[osThread];
			if (entry.name.empty() && name)
				entry.name = name;
			entry.ticks += now - t_threadSince;
			entry.core = t_core;
		}
		t_threadSince = 0;
		if (waiting)
			s_openWaits[osThread] = { now, 0, hleId, {} };
	}

	void NoteWakeup(const void* osThread, const void* waker, const char* wakerName)
	{
		if (!g_uwpDetailedProfiling)
			return;
		std::scoped_lock lock(s_threadMutex);
		auto it = s_openWaits.find(osThread);
		if (it == s_openWaits.end() || it->second.woken)
			return;
		it->second.woken = __rdtsc();
		it->second.waker = wakerName ? wakerName : (waker ? "?" : "host/alarm");
	}

	void NoteThreadSwitchIn(const void* osThread)
	{
		if (!g_uwpDetailedProfiling)
			return;
		const uint64 now = __rdtsc();
		t_threadSince = now;
		std::scoped_lock lock(s_threadMutex);
		auto it = s_openWaits.find(osThread);
		if (it == s_openWaits.end())
			return;
		const OpenWait wait = std::move(it->second);
		s_openWaits.erase(it);
		const uint64 woken = wait.woken ? wait.woken : now;
		auto threadIt = s_threadTicks.find(osThread);
		const std::string threadName = threadIt != s_threadTicks.end() && !threadIt->second.name.empty() ? threadIt->second.name : "?";
		const std::string call = wait.hleId >= 0 ? s_uwpHleNames[wait.hleId] : "?";
		const std::string waker = wait.woken ? wait.waker : "(not woken by a queue)";
		auto& total = s_waitTotals[threadName + "|" + call + "|" + waker];
		total.thread = threadName;
		total.call = call;
		total.waker = waker;
		total.waitTicks += woken - wait.start;
		total.readyTicks += now - woken;
		total.count++;
	}

	void Log()
	{
		if (!g_uwpDetailedProfiling)
			return;
		const uint64 now = __rdtsc();
		const uint64 window = s_lastLog ? now - s_lastLog : 0;
		s_lastLog = now;
		if (window == 0)
			return;
		auto ms = [](uint64 ticks) { return (double)PPCTimer_tscToMicroseconds(ticks) / 1000.0; };
		static const char* kNames[CategoryCount] = { "guest code", "HLE", "idle", "system events", "scheduler" };
		std::string cores;
		for (sint32 core = 0; core < kCores; core++)
		{
			cores += fmt::format(" core{}:", core);
			for (uint32 c = 0; c < CategoryCount; c++)
				cores += fmt::format(" {} {:.0f}", kNames[c], ms(s_coreTicks[core][c].exchange(0, std::memory_order_relaxed)));
			cores += ";";
		}
		std::vector<std::pair<uint64, sint32>> hle;
		for (sint32 i = 0; i < (sint32)HLE_TABLE_CAPACITY; i++)
		{
			const uint64 ticks = s_hleTicks[i].load(std::memory_order_relaxed);
			if (ticks)
				hle.emplace_back(ticks, i);
		}
		std::sort(hle.begin(), hle.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		std::string hleText;
		for (size_t i = 0; i < hle.size() && i < 12; i++)
			hleText += fmt::format("{}{} {:.0f} ms ({}x)", i ? ", " : "", s_uwpHleNames[hle[i].second], ms(hle[i].first),
				s_hleCalls[hle[i].second].load(std::memory_order_relaxed));
		uint64 totalCalls = 0;
		for (sint32 i = 0; i < (sint32)HLE_TABLE_CAPACITY; i++)
		{
			s_hleTicks[i].store(0, std::memory_order_relaxed);
			totalCalls += s_hleCalls[i].exchange(0, std::memory_order_relaxed);
		}
		std::vector<ThreadTicks> threads;
		std::vector<WaitTotal> waits;
		{
			std::scoped_lock lock(s_threadMutex);
			for (auto& [key, entry] : s_threadTicks)
				if (entry.ticks)
					threads.push_back(entry);
			for (auto& [key, entry] : s_threadTicks)
				entry.ticks = 0; // names stay for the wait report
			for (auto& [key, total] : s_waitTotals)
				waits.push_back(total);
			s_waitTotals.clear();
		}
		// the main thread first, then the longest waits of any thread
		std::sort(waits.begin(), waits.end(), [](const WaitTotal& a, const WaitTotal& b)
		{
			const bool mainA = a.thread == "Default Core 1", mainB = b.thread == "Default Core 1";
			if (mainA != mainB)
				return mainA;
			return a.waitTicks + a.readyTicks > b.waitTicks + b.readyTicks;
		});
		std::string waitText;
		for (size_t i = 0; i < waits.size() && i < 12; i++)
			waitText += fmt::format("{}{} in {} {:.0f} ms ({}x) woken by {}, then ready {:.0f} ms", i ? "; " : "", waits[i].thread, waits[i].call,
				ms(waits[i].waitTicks), waits[i].count, waits[i].waker, ms(waits[i].readyTicks));
		std::sort(threads.begin(), threads.end(), [](const auto& a, const auto& b) { return a.ticks > b.ticks; });
		std::string threadText;
		for (size_t i = 0; i < threads.size() && i < 8; i++)
			threadText += fmt::format("{}{} (core{}) {:.0f} ms", i ? ", " : "", threads[i].name.empty() ? "?" : threads[i].name, threads[i].core, ms(threads[i].ticks));
		cemuLog_log(LogType::Force, "UWP emulated CPU over {:.0f} ms (ms per category):{}", ms(window), cores);
		cemuLog_log(LogType::Force, "UWP emulated CPU HLE: {} calls; top: {}", totalCalls, hleText);
		cemuLog_log(LogType::Force, "UWP emulated CPU threads (time running, top 8): {}", threadText);
		cemuLog_log(LogType::Force, "UWP emulated CPU waits (main thread first; blocked time, then time ready but not running): {}", waitText);
	}
}
#endif