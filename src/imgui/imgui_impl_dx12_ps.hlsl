// ImGui pixel shader; built to DXIL at compile time (see src/imgui/CMakeLists.txt).
// Keep in sync with the runtime fallback source in imgui_impl_dx12.cpp.
struct PS_INPUT
{
	float4 pos : SV_POSITION;
	float4 col : COLOR0;
	float2 uv  : TEXCOORD0;
};

SamplerState sampler0 : register(s0);
Texture2D texture0 : register(t0);

float4 main(PS_INPUT input) : SV_Target
{
	return input.col * texture0.Sample(sampler0, input.uv);
}
