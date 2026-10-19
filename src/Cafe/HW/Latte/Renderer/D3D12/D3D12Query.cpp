#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Query.h"

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"

#include <algorithm>

LatteQueryObjectD3D12::LatteQueryObjectD3D12(D3D12Renderer* renderer)
	: m_renderer(renderer)
{
}

LatteQueryObjectD3D12::~LatteQueryObjectD3D12()
{
	if (!m_renderer)
		return;

	try
	{
		retireFragments();
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "D3D12 occlusion query retirement failed: {}", ex.what());
	}
	catch (...)
	{
		cemuLog_log(LogType::Force, "D3D12 occlusion query retirement failed with an unknown exception");
	}
}

void LatteQueryObjectD3D12::retireFragments()
{
	if (m_hasActiveFragment)
	{
		if (m_renderer->m_commandListOpen)
			endFragment();
		else
			cemuLog_log(LogType::Force, "D3D12 occlusion query lost its active command list during destruction; retiring it behind the last submitted fence.");
	}
	m_hasActiveQuery = false;
	m_hasActiveFragment = false;

	for (const auto& fragment : m_fragments)
	{
		const D3D12Renderer::OcclusionQuerySlot slot{ fragment.pageIndex, fragment.queryIndex };
		const uint64 retirementFence = fragment.isFinished ? fragment.fenceValue : m_renderer->GetRetirementFence();
		m_renderer->RetireOcclusionQuerySlot(slot, retirementFence);
	}
	m_fragments.clear();
}

bool LatteQueryObjectD3D12::getResult(uint64& numSamplesPassed)
{
	numSamplesPassed = 0;
	if (!m_queryEnded)
		return false;

	handleFinishedFragments();
	if (!m_fragments.empty())
		return false;

	numSamplesPassed = m_accumulatedSum;
	return true;
}

void LatteQueryObjectD3D12::beginFragment()
{
	if (!m_renderer || m_hasActiveFragment || m_isStartingFragment)
		return;

	m_isStartingFragment = true;
	D3D12Renderer::OcclusionQuerySlot slot;
	try
	{
		m_renderer->EnsureOcclusionQueryResources();
		if (!m_renderer->m_commandListOpen)
			m_renderer->ResetCommandList();

		handleFinishedFragments();
		slot = m_renderer->AcquireOcclusionQuerySlot();
		if (!m_renderer->m_commandListOpen)
			m_renderer->ResetCommandList();

		m_fragments.push_back({ slot.pageIndex, slot.queryIndex, 0, false });
		index = slot.queryIndex;
		auto& page = m_renderer->m_occlusionQueryPages[slot.pageIndex];
		m_renderer->GetCurrentCommandSlot().commandList->BeginQuery(page.heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, slot.queryIndex);
		m_hasActiveFragment = true;
		m_isStartingFragment = false;
	}
	catch (...)
	{
		if (slot.IsValid())
			m_renderer->ReleaseOcclusionQuerySlot(slot);
		m_isStartingFragment = false;
		throw;
	}
}

void LatteQueryObjectD3D12::endFragment()
{
	if (!m_renderer || !m_hasActiveFragment || m_fragments.empty())
		return;
	if (!m_renderer->m_commandListOpen)
		return;

	auto& fragment = m_fragments.back();
	const D3D12Renderer::OcclusionQuerySlot slot{ fragment.pageIndex, fragment.queryIndex };
	auto& page = m_renderer->m_occlusionQueryPages[fragment.pageIndex];
	m_renderer->GetCurrentCommandSlot().commandList->EndQuery(page.heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, fragment.queryIndex);
	m_renderer->ResolveOcclusionQuery(slot);
	fragment.fenceValue = m_renderer->m_nextFenceValue;
	m_renderer->m_lastQueryFenceValue = m_renderer->m_nextFenceValue;
	fragment.isFinished = true;
	m_renderer->GetCurrentCommandSlot().queryFragmentCount++;
	m_hasActiveFragment = false;
}

void LatteQueryObjectD3D12::handleFinishedFragments()
{
	if (!m_renderer)
		return;

	while (!m_fragments.empty())
	{
		auto& fragment = m_fragments.front();
		if (!fragment.isFinished || !m_renderer->IsFenceComplete(fragment.fenceValue))
			break;
		if (fragment.pageIndex >= m_renderer->m_occlusionQueryPages.size())
			throw std::runtime_error("D3D12 occlusion query references an invalid heap page");

		auto& page = m_renderer->m_occlusionQueryPages[fragment.pageIndex];
		if (!page.readback.resource)
			throw std::runtime_error("D3D12 occlusion query page has no readback buffer");

		uint64* mappedResults = nullptr;
		D3D12_RANGE readRange{ sizeof(uint64) * fragment.queryIndex, sizeof(uint64) * (fragment.queryIndex + 1) };
		const HRESULT mapHr = page.readback.resource->Map(0, &readRange, reinterpret_cast<void**>(&mappedResults));
		if (FAILED(mapHr))
		{
			cemuLog_log(LogType::Force, "D3D12 occlusion query readback map failed with HRESULT 0x{:08x}; retaining page {} slot {}.",
				(uint32)mapHr,
				fragment.pageIndex,
				fragment.queryIndex);
			m_renderer->CheckDeviceRemoved(mapHr, "occlusion query readback map");
			throw std::runtime_error("Failed to map D3D12 occlusion query result");
		}
		m_accumulatedSum += mappedResults[fragment.queryIndex];
		D3D12_RANGE writeRange{ 0, 0 };
		page.readback.resource->Unmap(0, &writeRange);
		m_renderer->ReleaseOcclusionQuerySlot({ fragment.pageIndex, fragment.queryIndex });
		m_fragments.erase(m_fragments.begin());
	}
}

void LatteQueryObjectD3D12::begin()
{
	if (!m_renderer)
		return;
	if (!m_renderer->m_commandListOpen)
		m_renderer->ResetCommandList();

	handleFinishedFragments();
	if (!m_fragments.empty())
	{
		cemuLog_logOnce(LogType::Force, "D3D12 occlusion query reused before all fragments completed; begin is deferred.");
		return;
	}

	m_queryEnded = false;
	m_hasActiveQuery = true;
	m_hasActiveFragment = false;
	m_accumulatedSum = 0;
	queryEnded = false;
	beginFragment();
}

void LatteQueryObjectD3D12::end()
{
	if (!m_renderer)
		return;
	if (m_hasActiveFragment)
		endFragment();

	m_queryEnded = true;
	m_hasActiveQuery = false;
	queryEnded = true;

	m_renderer->m_submitThreshold = std::min<uint32>(m_renderer->m_submitThreshold, m_renderer->m_recordedDrawcalls + D3D12Renderer::kSubmitSoonDrawDelay);
	m_renderer->m_submitOnIdle = true;
	handleFinishedFragments();
}
