#include "Cafe/HW/Latte/Renderer/D3D12/TextureReadbackD3D12.h"

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ViewDescription.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"

#include <algorithm>
#include <limits>

LatteTextureReadbackInfoD3D12::LatteTextureReadbackInfoD3D12(D3D12Renderer* renderer, LatteTextureView* textureView)
	: LatteTextureReadbackInfo(textureView, textureView->firstMip), m_renderer(renderer)
{
	auto* texture = static_cast<LatteTextureD3D12*>(textureView->baseTexture);
	if (texture->Is3DTexture())
		m_firstSlice = 0; // GetData() returns the whole depth slab
	m_subresource = texture->GetSubresourceIndex(m_firstSlice, textureView->firstMip);

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
	UINT64 totalBytes = 0;
	UINT numRows = 0;
	UINT64 rowSizeInBytes = 0;
	renderer->GetDevice()->GetCopyableFootprints(&texture->GetDesc(), m_subresource, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);
	const UINT depth = footprint.Footprint.Depth;
	const UINT64 linearSlicePitch = rowSizeInBytes * numRows;
	const UINT64 linearSize = linearSlicePitch * depth;
	if (numRows == 0 || depth == 0 || rowSizeInBytes == 0 || totalBytes == 0 ||
		linearSize > std::numeric_limits<uint32>::max() || totalBytes > std::numeric_limits<uint32>::max())
	{
		cemuLog_log(LogType::Force,
			"D3D12 texture readback footprint is invalid or too large (rows={}, depth={}, rowBytes={}, copyBytes={}).",
			numRows,
			depth,
			(unsigned long long)rowSizeInBytes,
			(unsigned long long)totalBytes);
		return;
	}

	m_footprint = footprint.Footprint;
	m_copySize = totalBytes;
	m_numRows = numRows;
	m_placedRowPitch = footprint.Footprint.RowPitch;
	m_rowSizeInBytes = rowSizeInBytes;
	m_placedSlicePitch = m_placedRowPitch * m_numRows;
	m_linearSlicePitch = linearSlicePitch;
	m_depth = depth;
	m_image_size = (uint32)linearSize;
	m_rowPitch = (uint32)rowSizeInBytes;
}

void LatteTextureReadbackInfoD3D12::StartTransfer()
{
	if (!m_renderer || !m_textureView || m_image_size == 0)
		return;

	auto* texture = static_cast<LatteTextureD3D12*>(m_textureView->baseTexture);
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
	footprint.Footprint = m_footprint;

	m_readback = m_renderer->GetMemoryManager()->AllocateReadback((uint32)m_copySize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, m_renderer->m_nextFenceValue);
	if (!m_readback.IsValid())
		return;

	footprint.Offset = m_readback.offset;

	D3D12_TEXTURE_COPY_LOCATION source{};
	source.pResource = texture->GetResource();
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	source.SubresourceIndex = m_subresource;

	D3D12_TEXTURE_COPY_LOCATION destination{};
	destination.pResource = m_readback.resource;
	destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	destination.PlacedFootprint = footprint;

	if (!m_renderer->m_commandListOpen)
		m_renderer->ResetCommandList();
	const D3D12_RESOURCE_STATES previousState = texture->GetSubresourceState(m_subresource);
	m_renderer->TransitionTextureSubresource(texture, m_subresource, D3D12_RESOURCE_STATE_COPY_SOURCE);
	m_renderer->GetCurrentCommandSlot().commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
	m_renderer->m_gpuWatchdog.RecordOperation(D3D12GpuWatchdog::Operation::Readback);
	m_renderer->CheckDeviceAfterOperation("readback", [&]() {
		return fmt::format("{} subresource={} footprint={}x{} rowPitch={}", D3D12DescribeResource(texture->GetResource()), m_subresource,
			footprint.Footprint.Width, footprint.Footprint.Height, footprint.Footprint.RowPitch); });
	m_renderer->TransitionTextureSubresource(texture, m_subresource, previousState);
	m_renderer->m_submitThreshold = std::min<uint32>(m_renderer->m_submitThreshold, m_renderer->m_recordedDrawcalls + D3D12Renderer::kSubmitSoonDrawDelay);
	m_renderer->m_submitOnIdle = true;
	m_fenceValue = m_renderer->m_nextFenceValue;
	m_textureView = nullptr;
}

bool LatteTextureReadbackInfoD3D12::IsFinished()
{
	return !m_renderer || m_renderer->IsFenceComplete(m_fenceValue);
}

void LatteTextureReadbackInfoD3D12::ForceFinish()
{
	if (!m_renderer || IsFinished())
		return;
	if (m_renderer->m_commandListOpen && m_fenceValue >= m_renderer->m_nextFenceValue)
		m_renderer->ExecuteCurrentCommandList();
	m_renderer->WaitForFence(m_fenceValue);
}

uint8* LatteTextureReadbackInfoD3D12::GetData()
{
	ForceFinish();
	if (!m_readback.IsValid())
	{
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		m_linearData.clear();
		m_diagnosticReadbackStatus = "invalid_reservation";
#endif
		return nullptr;
	}

	if (m_linearData.empty())
	{
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
		const auto resourceDesc = m_readback.resource->GetDesc();
		constexpr uint64 maxOffset = std::numeric_limits<size_t>::max();
		if (resourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
			m_copySize == 0 || m_copySize > m_readback.size ||
			m_readback.offset > resourceDesc.Width ||
			m_copySize > resourceDesc.Width - m_readback.offset ||
			m_readback.offset > maxOffset || m_copySize > maxOffset - m_readback.offset)
		{
			m_diagnosticReadbackStatus = "invalid_buffer_range";
			return nullptr;
		}
		if (m_image_size == 0 || m_numRows == 0 || m_depth == 0 ||
			m_rowSizeInBytes == 0 || m_rowSizeInBytes > m_placedRowPitch ||
			m_placedRowPitch != m_footprint.RowPitch || m_depth != m_footprint.Depth ||
			m_placedRowPitch > maxOffset / m_numRows ||
			m_rowSizeInBytes > maxOffset / m_numRows ||
			m_placedSlicePitch != m_placedRowPitch * m_numRows ||
			m_linearSlicePitch != m_rowSizeInBytes * m_numRows ||
			m_placedSlicePitch > maxOffset / m_depth ||
			m_linearSlicePitch > maxOffset / m_depth ||
			m_linearSlicePitch * m_depth != m_image_size)
		{
			m_diagnosticReadbackStatus = "invalid_row_layout";
			return nullptr;
		}
		// GetCopyableFootprints may omit padding after the last row
		const uint64 lastReadEnd = m_placedSlicePitch * (m_depth - 1) +
			m_placedRowPitch * (m_numRows - 1) + m_rowSizeInBytes;
		if (lastReadEnd > m_copySize)
		{
			m_diagnosticReadbackStatus = "short_placed_footprint";
			return nullptr;
		}
		std::vector<uint8> linearData(m_image_size);
		const D3D12_RANGE readRange{
			(size_t)m_readback.offset, (size_t)(m_readback.offset + m_copySize) };
		const D3D12_RANGE writtenRange{ 0, 0 };
		void* mappedBase = nullptr;
		// nested Map refreshes CPU visibility for this range
		m_diagnosticMapResult = m_readback.resource->Map(0, &readRange, &mappedBase);
		if (FAILED(m_diagnosticMapResult) || !mappedBase)
		{
			if (SUCCEEDED(m_diagnosticMapResult))
				m_readback.resource->Unmap(0, &writtenRange);
			m_diagnosticReadbackStatus = FAILED(m_diagnosticMapResult) ? "map_failed" : "map_null";
			return nullptr;
		}
		const auto* sourceBase = static_cast<const uint8*>(mappedBase) + (size_t)m_readback.offset;
		for (uint32 slice = 0; slice < m_depth; slice++)
		{
			const uint8* sourceSlice = sourceBase + (size_t)slice * (size_t)m_placedSlicePitch;
			uint8* destinationSlice = linearData.data() + (size_t)slice * (size_t)m_linearSlicePitch;
			for (uint32 row = 0; row < m_numRows; row++)
				memcpy(destinationSlice + (size_t)row * (size_t)m_rowSizeInBytes,
					   sourceSlice + (size_t)row * (size_t)m_placedRowPitch,
					   (size_t)m_rowSizeInBytes);
		}
		m_readback.resource->Unmap(0, &writtenRange);
		m_linearData = std::move(linearData);
		m_diagnosticReadbackStatus = "mapped_after_fence";
#else
		m_linearData.resize(m_image_size);
		for (uint32 slice = 0; slice < m_depth; slice++)
		{
			const uint8* sourceSlice = m_readback.cpuPtr + (size_t)slice * (size_t)m_placedSlicePitch;
			uint8* destinationSlice = m_linearData.data() + (size_t)slice * (size_t)m_linearSlicePitch;
			for (uint32 row = 0; row < m_numRows; row++)
				memcpy(destinationSlice + (size_t)row * (size_t)m_rowSizeInBytes,
					   sourceSlice + (size_t)row * (size_t)m_placedRowPitch,
					   (size_t)m_rowSizeInBytes);
		}
#endif
	}
	return m_linearData.data();
}
