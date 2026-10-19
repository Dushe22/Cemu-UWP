#pragma once

#include <string>

#include <d3d12.h>

// readable summaries of D3D12 resources and view descriptions for logs
std::string D3D12DescribeResource(ID3D12Resource* resource);
std::string D3D12DescribeView(const D3D12_SHADER_RESOURCE_VIEW_DESC* desc);
std::string D3D12DescribeView(const D3D12_RENDER_TARGET_VIEW_DESC* desc);
std::string D3D12DescribeView(const D3D12_DEPTH_STENCIL_VIEW_DESC* desc);
std::string D3D12DescribeView(const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc);
std::string D3D12DescribeSampler(const D3D12_SAMPLER_DESC* desc);
