#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

// Watches GPU progress from its own thread. Every submitted command list keeps a short summary until the GPU
// completes it, so a stall or a device removal can be logged together with the work that was still in flight.
class D3D12GpuWatchdog
{
public:
	enum class Activity : uint32
	{
		None,
		PipelineCreate,
		FenceWait,
		Present,
	};

	enum class Operation : uint32
	{
		OutputDraw,
		SnapshotDraw,
		OverlayDraw,
		SurfaceCopy,
		Readback,
		Count,
	};

	class ActivityScope
	{
	public:
		ActivityScope(D3D12GpuWatchdog& watchdog, Activity activity, uint64 detail);
		~ActivityScope();
		ActivityScope(const ActivityScope&) = delete;
		ActivityScope& operator=(const ActivityScope&) = delete;

	private:
		D3D12GpuWatchdog& m_watchdog;
		uint32 m_previousActivity;
		uint64 m_previousDetail;
		sint64 m_previousSince;
	};

	~D3D12GpuWatchdog();

	void Start(ID3D12Device* device, ID3D12Fence* fence);
	void Stop();

	// called by the thread recording the open command list
	void RecordGamePipeline(uint64 pipelineHash, uint64 vertexHash, uint64 geometryHash, uint64 pixelHash);
	void RecordOperation(Operation operation);
	void Submit(uint64 fenceValue, uint32 drawCount);
	// last calls recorded into the open command list (static names), logged when closing it fails
	void NoteCall(const char* name) { m_recentCalls[m_recentCallIndex++ % m_recentCalls.size()] = name; }
	void LogOpenList(const char* reason);

	void NotifyGameFrame();

	// true while game frames or GPU progress have stopped for a moment, when costlier checks are affordable
	bool IsStalled() const { return m_stalled.load(std::memory_order_relaxed); }

private:
	struct PipelineEntry
	{
		uint64 pipelineHash;
		uint64 vertexHash;
		uint64 geometryHash;
		uint64 pixelHash;
	};

	struct ListSummary
	{
		uint64 fenceValue{};
		std::chrono::steady_clock::time_point submitted;
		uint32 drawCount{};
		std::array<uint32, (size_t)Operation::Count> operations{};
		std::vector<PipelineEntry> pipelines;
		uint32 droppedPipelines{};
	};

	static constexpr size_t kMaxPipelinesPerList = 48;
	std::array<const char*, 32> m_recentCalls{};
	uint32 m_recentCallIndex = 0;
	static constexpr size_t kMaxTrackedLists = 64;

	void Run();
	void Report(const char* what, uint64 completedFence, HRESULT removedReason);
	void LogUnfinishedWorkLocked(uint64 completedFence);
	static sint64 NowTicks();

	Microsoft::WRL::ComPtr<ID3D12Device> m_device;
	Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
	std::thread m_thread;
	std::mutex m_threadMutex;
	std::condition_variable m_threadCondition;
	bool m_stop{};

	ListSummary m_current;
	std::mutex m_listMutex;
	std::deque<ListSummary> m_submitted;
	std::atomic<uint64> m_lastSubmittedFence{};
	std::atomic<uint64> m_lastCompletedFence{};

	std::atomic<sint64> m_lastGameFrame{};
	std::atomic<bool> m_stalled{};
	std::atomic<uint32> m_activity{};
	std::atomic<uint64> m_activityDetail{};
	std::atomic<sint64> m_activitySince{};
};
