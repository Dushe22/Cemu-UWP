// braces in comments { } and a fake function: float fake(float x) { return x; }
/* block comment with float alsoFake(int a) { } */
#define LATTE_CONCAT_INNER(a, b) a##b
#define LATTE_CONCAT(a, b) LATTE_CONCAT_INNER(a, b)
#define SAMPLER(tex) LATTE_CONCAT(tex, Sampler)
#define MULTILINE(x) \
	{ x; } \
	usedByMacro(x)
cbuffer Uniforms : register(b0)
{
	float4 uf_remappedVS[8];
	int uf_flags;
};
struct VSOut
{
	float4 pos : SV_Position;
	float4 col : COLOR0;
	float4 Scaled(float s) { return col * s; }
};
Texture2D<float4> tex0 : register(t0);
SamplerState tex0Sampler : register(s0);
static const float kTable[3] = { 1.0f, 2.0f, 3.0f };
float usedByMacro(float x) { return x * 2.0f; }
float usedViaMacro(float x) { return x * 5.0f; }
float chainHelper(float x) { return x * 7.0f; }
#define CALLHELPER(x) usedViaMacro(x)
#define INNER(x) chainHelper(x)
#define OUTER(x) INNER(x)
float unusedA(float x) { return x + 1.0f; }
float unusedA(float2 x) { return x.x; }
float usedOverload(float x) { return x; }
float usedOverload(float2 x) { return x.y; }
int unusedChainLeaf(int a) { return a * 3; }
int unusedChainRoot(int a) { return unusedChainLeaf(a); }
float4 helperCalledByHelper(float4 v) { return v * kTable[1]; }
float4 helperTop(float4 v)
{
	// nested braces in body
	if (v.x > 0) { v = helperCalledByHelper(v); } else { v = -v; }
	return v;
}
uint4 makeU(uint v) { return uint4(v, v, v, v); }
float unusedB(float x) { return x; }
#if defined(NOT_DEFINED)
float onlyInInactiveBranchUser() { return unusedB(1.0f); }
#endif
float4 main(VSOut input) : SV_Target
{
	float4 c = tex0.Sample(SAMPLER(tex0), input.pos.xy);
	c = helperTop(c) + usedOverload(c.x) + input.Scaled(2.0f) + uf_remappedVS[uf_flags & 7];
	return c + (float)makeU(1).x + CALLHELPER(c.y) + OUTER(c.z);
}
