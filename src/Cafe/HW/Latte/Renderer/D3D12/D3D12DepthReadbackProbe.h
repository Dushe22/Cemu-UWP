#pragma once

#if defined(CEMU_UWP_D3D12_DIAGNOSTICS)
struct ID3D12Device;

namespace D3D12DepthReadbackProbe
{
void RunOnce(ID3D12Device* device) noexcept;
}
#endif
