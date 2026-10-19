#pragma once

#include "Cafe/HW/Latte/Core/LatteTextureReadbackInfo.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12MemoryManager.h"

class D3D12Renderer;

class LatteTextureReadbackInfoD3D12 : public LatteTextureReadbackInfo
{
public:
	LatteTextureReadbackInfoD3D12(D3D12Renderer* renderer, LatteTextureView* textureView);

	void StartTransfer() override;
	bool IsFinished() override;
	void ForceFinish() override;
	uint8* GetData() override;

	bool IsValid() const { return m_image_size != 0; }
	uint32 GetRowSizeInBytes() const { return (uint32)m_rowSizeInBytes; }

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	const D3D12_SUBRESOURCE_FOOTPRINT& GetDiagnosticFootprint() const { return m_footprint; }
	uint64 GetDiagnosticCopySize() const { return m_copySize; }
	uint64 GetDiagnosticReservationOffset() const { return m_readback.offset; }
	uint64 GetDiagnosticReservationSize() const { return m_readback.size; }
	uint64 GetDiagnosticFenceValue() const { return m_fenceValue; }
	const char* GetDiagnosticReadbackStatus() const { return m_diagnosticReadbackStatus; }
	HRESULT GetDiagnosticMapResult() const { return m_diagnosticMapResult; }
#endif

private:
	D3D12Renderer* m_renderer{};
	D3D12MemoryManager::RingReservation m_readback;
	std::vector<uint8> m_linearData;
	uint64 m_fenceValue{};
	D3D12_SUBRESOURCE_FOOTPRINT m_footprint{};
	uint64 m_copySize{};
	uint64 m_placedRowPitch{};
	uint64 m_rowSizeInBytes{};
	uint64 m_placedSlicePitch{};
	uint64 m_linearSlicePitch{};
	uint32 m_numRows{};
	uint32 m_depth{};
	uint32 m_subresource{};
#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
	const char* m_diagnosticReadbackStatus{ "not_read" };
	HRESULT m_diagnosticMapResult{ S_OK };
#endif
};
