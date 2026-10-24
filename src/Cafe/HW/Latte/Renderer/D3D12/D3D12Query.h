#pragma once

#include "Cafe/HW/Latte/Core/LatteQueryObject.h"

#include <atomic>
#include <vector>

class D3D12Renderer;

class LatteQueryObjectD3D12 : public LatteQueryObject
{
	friend class D3D12Renderer;

public:
	explicit LatteQueryObjectD3D12(D3D12Renderer* renderer);
	~LatteQueryObjectD3D12();

	bool getResult(uint64& numSamplesPassed) override;
	void begin() override;
	void end() override;
private:
	// the command-list half of begin/end, run where the recording state lives
	void beginRecorded();
	void endRecorded();
public:
	void beginFragment();
	void endFragment();
	void handleFinishedFragments();

private:
	void retireFragments();

	struct QueryFragment
	{
		uint32 pageIndex{};
		uint32 queryIndex{};
		uint64 fenceValue{};
		bool isFinished{};
	};

	D3D12Renderer* m_renderer{};
	std::vector<QueryFragment> m_fragments;
	uint64 m_accumulatedSum{};
	bool m_queryEnded{};
	bool m_hasActiveQuery{};
	bool m_hasActiveFragment{};
	bool m_isStartingFragment{};
	// threaded recording: the recorder publishes the final sample count here, so the Latte side reads it without
	// draining the recording queue
	std::atomic<bool> m_resultPublished{};
	uint64 m_publishedResult{};
};
