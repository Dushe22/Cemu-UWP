
typedef float2 vec2;
typedef float3 vec3;
typedef float4 vec4;
typedef int2 ivec2;
typedef int3 ivec3;
typedef int4 ivec4;
typedef uint2 uvec2;
typedef uint3 uvec3;
typedef uint4 uvec4;
typedef bool2 bvec2;
typedef bool3 bvec3;
typedef bool4 bvec4;

float2 make_vec2(float v) { return float2(v, v); }
float2 make_vec2(float x, float y) { return float2(x, y); }
float2 make_vec2(int2 v) { return float2(v); }
float2 make_vec2(uint2 v) { return float2(v); }
float3 make_vec3(float v) { return float3(v, v, v); }
float3 make_vec3(float x, float y, float z) { return float3(x, y, z); }
float3 make_vec3(float2 xy, float z) { return float3(xy, z); }
float3 make_vec3(int3 v) { return float3(v); }
float3 make_vec3(uint3 v) { return float3(v); }
float4 make_vec4(float v) { return float4(v, v, v, v); }
float4 make_vec4(float x, float y, float z, float w) { return float4(x, y, z, w); }
float4 make_vec4(float2 xy, float z, float w) { return float4(xy, z, w); }
float4 make_vec4(float2 xy, float2 zw) { return float4(xy, zw); }
float4 make_vec4(float3 xyz, float w) { return float4(xyz, w); }
float4 make_vec4(int4 v) { return float4(v); }
float4 make_vec4(uint4 v) { return float4(v); }
int2 make_ivec2(int v) { return int2(v, v); }
int2 make_ivec2(int x, int y) { return int2(x, y); }
int2 make_ivec2(float2 v) { return int2(v); }
int2 make_ivec2(int2 v) { return v; }
int2 make_ivec2(uint2 v) { return int2(v); }
int3 make_ivec3(int v) { return int3(v, v, v); }
int3 make_ivec3(int x, int y, int z) { return int3(x, y, z); }
int3 make_ivec3(int2 xy, int z) { return int3(xy, z); }
int3 make_ivec3(float3 v) { return int3(v); }
int3 make_ivec3(int3 v) { return v; }
int3 make_ivec3(uint3 v) { return int3(v); }
int4 make_ivec4(int v) { return int4(v, v, v, v); }
int4 make_ivec4(int x, int y, int z, int w) { return int4(x, y, z, w); }
int4 make_ivec4(int2 xy, int z, int w) { return int4(xy, z, w); }
int4 make_ivec4(int3 xyz, int w) { return int4(xyz, w); }
int4 make_ivec4(float4 v) { return int4(v); }
int4 make_ivec4(uint4 v) { return int4(v); }
int4 make_ivec4(int4 v) { return v; }
uint2 make_uvec2(uint v) { return uint2(v, v); }
uint2 make_uvec2(uint x, uint y) { return uint2(x, y); }
uint3 make_uvec3(uint v) { return uint3(v, v, v); }
uint3 make_uvec3(uint x, uint y, uint z) { return uint3(x, y, z); }
uint4 make_uvec4(uint v) { return uint4(v, v, v, v); }
uint4 make_uvec4(uint x, uint y, uint z, uint w) { return uint4(x, y, z, w); }

#define vec2(...) make_vec2(__VA_ARGS__)
#define vec3(...) make_vec3(__VA_ARGS__)
#define vec4(...) make_vec4(__VA_ARGS__)
#define ivec2(...) make_ivec2(__VA_ARGS__)
#define ivec3(...) make_ivec3(__VA_ARGS__)
#define ivec4(...) make_ivec4(__VA_ARGS__)
#define uvec2(...) make_uvec2(__VA_ARGS__)
#define uvec3(...) make_uvec3(__VA_ARGS__)
#define uvec4(...) make_uvec4(__VA_ARGS__)
#define uintBitsToFloat asfloat
#define intBitsToFloat asfloat
#define floatBitsToUint asuint
#define floatBitsToInt asint
#define dFdx ddx
#define dFdy ddy
#define fract frac
#define roundEven round
#define inversesqrt rsqrt
#define LATTE_LOAD_U8(buffer, byteOffset) (((buffer).Load(((byteOffset) >> 2u)) >> (((byteOffset) & 3u) * 8u)) & 0xffu)
#define LATTE_LOAD_U16_LE(buffer, byteOffset) (LATTE_LOAD_U8(buffer, byteOffset) | (LATTE_LOAD_U8(buffer, ((byteOffset) + 1u)) << 8u))
#define LATTE_LOAD_U16_BE(buffer, byteOffset) ((LATTE_LOAD_U8(buffer, byteOffset) << 8u) | LATTE_LOAD_U8(buffer, ((byteOffset) + 1u)))
#define LATTE_LOAD_U32_LE(buffer, byteOffset) (LATTE_LOAD_U8(buffer, byteOffset) | (LATTE_LOAD_U8(buffer, ((byteOffset) + 1u)) << 8u) | (LATTE_LOAD_U8(buffer, ((byteOffset) + 2u)) << 16u) | (LATTE_LOAD_U8(buffer, ((byteOffset) + 3u)) << 24u))
#define LATTE_LOAD_U32_BE(buffer, byteOffset) ((LATTE_LOAD_U8(buffer, byteOffset) << 24u) | (LATTE_LOAD_U8(buffer, ((byteOffset) + 1u)) << 16u) | (LATTE_LOAD_U8(buffer, ((byteOffset) + 2u)) << 8u) | LATTE_LOAD_U8(buffer, ((byteOffset) + 3u)))

float mix(float a, float b, bool t) { return t ? b : a; }
float2 mix(float2 a, float2 b, bool2 t) { return float2(t.x ? b.x : a.x, t.y ? b.y : a.y); }
float3 mix(float3 a, float3 b, bool3 t) { return float3(t.x ? b.x : a.x, t.y ? b.y : a.y, t.z ? b.z : a.z); }
float4 mix(float4 a, float4 b, bool4 t) { return float4(t.x ? b.x : a.x, t.y ? b.y : a.y, t.z ? b.z : a.z, t.w ? b.w : a.w); }
float mix(float a, float b, float t) { return lerp(a, b, t); }
float2 mix(float2 a, float2 b, float2 t) { return lerp(a, b, t); }
float3 mix(float3 a, float3 b, float3 t) { return lerp(a, b, t); }
float4 mix(float4 a, float4 b, float4 t) { return lerp(a, b, t); }

int clampFI32(int v)
{
	if (v == 0x7FFFFFFF)
		return asint(1.0f);
	if (v == -1)
		return asint(0.0f);
	return asint(clamp(asfloat(v), 0.0f, 1.0f));
}

float mul_nonIEEE(float a, float b)
{
	if (a == 0.0f || b == 0.0f)
		return 0.0f;
	return a * b;
}

uint4 lattePackUnorm8(float4 v)
{
	return (uint4)round(saturate(v) * 255.0f);
}

float4 latteUnpackUnorm8(uint4 v)
{
	return (float4)(v & 0xffu) / 255.0f;
}



#undef LATTE_LOAD_U16_LE
#undef LATTE_LOAD_U16_BE
#undef LATTE_LOAD_U32_LE
#undef LATTE_LOAD_U32_BE

uint latteLoadU16LE(Buffer<uint> buffer, uint byteOffset)
{
	const uint wordIndex = byteOffset >> 2u;
	const uint shift = (byteOffset & 3u) * 8u;
	const uint low = buffer.Load(wordIndex);
	if (shift <= 16u)
		return (low >> shift) & 0xffffu;
	return ((low >> 24u) & 0xffu) | ((buffer.Load(wordIndex + 1u) & 0xffu) << 8u);
}

uint latteLoadU32LE(Buffer<uint> buffer, uint byteOffset)
{
	const uint wordIndex = byteOffset >> 2u;
	const uint shift = (byteOffset & 3u) * 8u;
	const uint low = buffer.Load(wordIndex);
	if (shift == 0u)
		return low;
	return (low >> shift) | (buffer.Load(wordIndex + 1u) << (32u - shift));
}

uint latteByteSwap16(uint value)
{
	return ((value & 0xffu) << 8u) | ((value >> 8u) & 0xffu);
}

uint latteByteSwap32(uint value)
{
	return ((value & 0x000000ffu) << 24u) |
		((value & 0x0000ff00u) << 8u) |
		((value & 0x00ff0000u) >> 8u) |
		((value & 0xff000000u) >> 24u);
}

#define LATTE_LOAD_U16_LE(buffer, byteOffset) latteLoadU16LE(buffer, byteOffset)
#define LATTE_LOAD_U16_BE(buffer, byteOffset) latteByteSwap16(latteLoadU16LE(buffer, byteOffset))
#define LATTE_LOAD_U32_LE(buffer, byteOffset) latteLoadU32LE(buffer, byteOffset)
#define LATTE_LOAD_U32_BE(buffer, byteOffset) latteByteSwap32(latteLoadU32LE(buffer, byteOffset))


float4 latteLogicOpUnorm8(float4 src, float4 dst, uint mode)
{
	uint4 srcBits = lattePackUnorm8(src);
	uint4 dstBits = lattePackUnorm8(dst);
	uint4 result = srcBits;
	switch (mode)
	{
	case 1u: result = 0u; break;
	case 2u: result = srcBits | dstBits; break;
	case 3u: result = 0xffu; break;
	case 4u: result = ~srcBits; break;
	case 5u: result = dstBits; break;
	case 6u: result = ~dstBits; break;
	case 7u: result = srcBits & dstBits; break;
	case 8u: result = ~(srcBits & dstBits); break;
	case 9u: result = ~(srcBits | dstBits); break;
	case 10u: result = srcBits ^ dstBits; break;
	case 11u: result = ~(srcBits ^ dstBits); break;
	case 12u: result = srcBits & ~dstBits; break;
	case 13u: result = ~srcBits & dstBits; break;
	case 14u: result = srcBits | ~dstBits; break;
	case 15u: result = ~srcBits | dstBits; break;
	default: break;
	}
	return latteUnpackUnorm8(result);
}



int textureSize(Texture1D<float4> tex, int lod)
{
	uint w, levels;
	tex.GetDimensions((uint)lod, w, levels);
	return (int)w;
}

int textureSize(Texture1D<uint4> tex, int lod)
{
	uint w, levels;
	tex.GetDimensions((uint)lod, w, levels);
	return (int)w;
}

int2 textureSize(Texture1DArray<float4> tex, int lod)
{
	uint w, elements, levels;
	tex.GetDimensions((uint)lod, w, elements, levels);
	return int2(w, elements);
}

int2 textureSize(Texture1DArray<uint4> tex, int lod)
{
	uint w, elements, levels;
	tex.GetDimensions((uint)lod, w, elements, levels);
	return int2(w, elements);
}

int2 textureSize(Texture2D<float4> tex, int lod)
{
	uint w, h, levels;
	tex.GetDimensions((uint)lod, w, h, levels);
	return int2(w, h);
}

int2 textureSize(Texture2D<uint4> tex, int lod)
{
	uint w, h, levels;
	tex.GetDimensions((uint)lod, w, h, levels);
	return int2(w, h);
}

int3 textureSize(Texture2DArray<float4> tex, int lod)
{
	uint w, h, elements, levels;
	tex.GetDimensions((uint)lod, w, h, elements, levels);
	return int3(w, h, elements);
}

int3 textureSize(Texture2DArray<uint4> tex, int lod)
{
	uint w, h, elements, levels;
	tex.GetDimensions((uint)lod, w, h, elements, levels);
	return int3(w, h, elements);
}

int3 textureSize(Texture3D<float4> tex, int lod)
{
	uint w, h, d, levels;
	tex.GetDimensions((uint)lod, w, h, d, levels);
	return int3(w, h, d);
}

int3 textureSize(Texture3D<uint4> tex, int lod)
{
	uint w, h, d, levels;
	tex.GetDimensions((uint)lod, w, h, d, levels);
	return int3(w, h, d);
}

int3 textureSize(TextureCubeArray<float4> tex, int lod)
{
	uint w, h, elements, levels;
	tex.GetDimensions((uint)lod, w, h, elements, levels);
	return int3(w, h, elements);
}

int3 textureSize(TextureCubeArray<uint4> tex, int lod)
{
	uint w, h, elements, levels;
	tex.GetDimensions((uint)lod, w, h, elements, levels);
	return int3(w, h, elements);
}

int textureQueryLevels(Texture1D<float4> tex) { uint w, levels; tex.GetDimensions(0u, w, levels); return (int)levels; }
int textureQueryLevels(Texture1D<uint4> tex) { uint w, levels; tex.GetDimensions(0u, w, levels); return (int)levels; }
int textureQueryLevels(Texture1D<float> tex) { uint w, levels; tex.GetDimensions(0u, w, levels); return (int)levels; }
int textureQueryLevels(Texture1DArray<float4> tex) { uint w, e, levels; tex.GetDimensions(0u, w, e, levels); return (int)levels; }
int textureQueryLevels(Texture1DArray<uint4> tex) { uint w, e, levels; tex.GetDimensions(0u, w, e, levels); return (int)levels; }
int textureQueryLevels(Texture1DArray<float> tex) { uint w, e, levels; tex.GetDimensions(0u, w, e, levels); return (int)levels; }
int textureQueryLevels(Texture2D<float4> tex) { uint w, h, levels; tex.GetDimensions(0u, w, h, levels); return (int)levels; }
int textureQueryLevels(Texture2D<uint4> tex) { uint w, h, levels; tex.GetDimensions(0u, w, h, levels); return (int)levels; }
int textureQueryLevels(Texture2D<float> tex) { uint w, h, levels; tex.GetDimensions(0u, w, h, levels); return (int)levels; }
int textureQueryLevels(Texture2DArray<float4> tex) { uint w, h, e, levels; tex.GetDimensions(0u, w, h, e, levels); return (int)levels; }
int textureQueryLevels(Texture2DArray<uint4> tex) { uint w, h, e, levels; tex.GetDimensions(0u, w, h, e, levels); return (int)levels; }
int textureQueryLevels(Texture2DArray<float> tex) { uint w, h, e, levels; tex.GetDimensions(0u, w, h, e, levels); return (int)levels; }
int textureQueryLevels(Texture3D<float4> tex) { uint w, h, d, levels; tex.GetDimensions(0u, w, h, d, levels); return (int)levels; }
int textureQueryLevels(Texture3D<uint4> tex) { uint w, h, d, levels; tex.GetDimensions(0u, w, h, d, levels); return (int)levels; }
int textureQueryLevels(TextureCubeArray<float4> tex) { uint w, h, e, levels; tex.GetDimensions(0u, w, h, e, levels); return (int)levels; }
int textureQueryLevels(TextureCubeArray<uint4> tex) { uint w, h, e, levels; tex.GetDimensions(0u, w, h, e, levels); return (int)levels; }
int textureQueryLevels(TextureCubeArray<float> tex) { uint w, h, e, levels; tex.GetDimensions(0u, w, h, e, levels); return (int)levels; }

float4 latteTexelFetch(Texture1D<float4> tex, int coord, int lod) { return tex.Load(int2(coord, lod)); }
uint4 latteTexelFetch(Texture1D<uint4> tex, int coord, int lod) { return tex.Load(int2(coord, lod)); }
float4 latteTexelFetch(Texture2D<float4> tex, int2 coord, int lod) { return tex.Load(int3(coord, lod)); }
uint4 latteTexelFetch(Texture2D<uint4> tex, int2 coord, int lod) { return tex.Load(int3(coord, lod)); }
float4 latteTexelFetch(Texture2DArray<float4> tex, int3 coord, int lod) { return tex.Load(int4(coord.xy, coord.z, lod)); }
uint4 latteTexelFetch(Texture2DArray<uint4> tex, int3 coord, int lod) { return tex.Load(int4(coord.xy, coord.z, lod)); }
float4 latteTexelFetch(Texture3D<float4> tex, int3 coord, int lod) { return tex.Load(int4(coord, lod)); }
uint4 latteTexelFetch(Texture3D<uint4> tex, int3 coord, int lod) { return tex.Load(int4(coord, lod)); }

// GatherRed already has GLSL textureGather order. The shared FETCH4 emitter
// applies the guest component mapping; another permutation here corrupts it.
float4 latteTextureGather(Texture2D<float4> tex, SamplerState samplerState, float2 coord) { return tex.GatherRed(samplerState, coord); }
uint4 latteTextureGather(Texture2D<uint4> tex, SamplerState samplerState, float2 coord) { return tex.GatherRed(samplerState, coord); }
float4 latteTextureGather(Texture2DArray<float4> tex, SamplerState samplerState, float3 coord) { return tex.GatherRed(samplerState, coord); }
uint4 latteTextureGather(Texture2DArray<uint4> tex, SamplerState samplerState, float3 coord) { return tex.GatherRed(samplerState, coord); }
float4 latteTextureGather(TextureCubeArray<float4> tex, SamplerState samplerState, float4 coord) { return tex.GatherRed(samplerState, coord); }
uint4 latteTextureGather(TextureCubeArray<uint4> tex, SamplerState samplerState, float4 coord) { return tex.GatherRed(samplerState, coord); }

void redcCUBE(float4 src0, float4 src1, out float3 stm, out int faceId)
{
	float3 inputCoord = normalize(float3(src1.y, src1.x, src0.x));
	float rx = inputCoord.x;
	float ry = inputCoord.y;
	float rz = inputCoord.z;
	if (abs(rx) > abs(ry) && abs(rx) > abs(rz))
	{
		stm.z = rx * 2.0f;
		stm.xy = float2(ry, rz);
		faceId = rx >= 0.0f ? 0 : 1;
	}
	else if (abs(ry) > abs(rx) && abs(ry) > abs(rz))
	{
		stm.z = ry * 2.0f;
		stm.xy = float2(rx, rz);
		faceId = ry >= 0.0f ? 2 : 3;
	}
	else
	{
		stm.z = rz * 2.0f;
		stm.xy = float2(rx, ry);
		faceId = rz >= 0.0f ? 4 : 5;
	}
}

float3 redcCUBEReverse(float2 st, int faceId)
{
	st.yx = st.xy;
	float3 v;
	float majorAxis = 1.0f;
	if (faceId == 0)
	{
		v.yz = (st - float2(1.5f, 1.5f)) * (majorAxis * 2.0f);
		v.x = 1.0f;
	}
	else if (faceId == 1)
	{
		v.yz = (st - float2(1.5f, 1.5f)) * (majorAxis * 2.0f);
		v.x = -1.0f;
	}
	else if (faceId == 2)
	{
		v.xz = (st - float2(1.5f, 1.5f)) * (majorAxis * 2.0f);
		v.y = 1.0f;
	}
	else if (faceId == 3)
	{
		v.xz = (st - float2(1.5f, 1.5f)) * (majorAxis * 2.0f);
		v.y = -1.0f;
	}
	else if (faceId == 4)
	{
		v.xy = (st - float2(1.5f, 1.5f)) * (majorAxis * 2.0f);
		v.z = 1.0f;
	}
	else
	{
		v.xy = (st - float2(1.5f, 1.5f)) * (majorAxis * 2.0f);
		v.z = -1.0f;
	}
	return v;
}

float latteTextureLodFromGradients(float3 dx, float3 dy)
{
	return 0.5f * log2(max(max(dot(dx, dx), dot(dy, dy)), 1.0e-30f));
}

float2 latteCubeFaceCoord(float3 direction)
{
	const float3 magnitude = abs(direction);
	if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
		return float2(direction.x >= 0.0f ? -direction.z : direction.z, -direction.y) / (2.0f * magnitude.x) + 0.5f;
	if (magnitude.y >= magnitude.z)
		return float2(direction.x, direction.y >= 0.0f ? direction.z : -direction.z) / (2.0f * magnitude.y) + 0.5f;
	return float2(direction.z >= 0.0f ? direction.x : -direction.x, -direction.y) / (2.0f * magnitude.z) + 0.5f;
}



int textureSize(Texture1D<float> tex, int lod)
{
	uint width, levels;
	tex.GetDimensions((uint)lod, width, levels);
	return (int)width;
}

int2 textureSize(Texture1DArray<float> tex, int lod)
{
	uint width, elements, levels;
	tex.GetDimensions((uint)lod, width, elements, levels);
	return int2(width, elements);
}

int2 textureSize(Texture2D<float> tex, int lod)
{
	uint width, height, levels;
	tex.GetDimensions((uint)lod, width, height, levels);
	return int2(width, height);
}

int3 textureSize(Texture2DArray<float> tex, int lod)
{
	uint width, height, elements, levels;
	tex.GetDimensions((uint)lod, width, height, elements, levels);
	return int3(width, height, elements);
}

int3 textureSize(TextureCubeArray<float> tex, int lod)
{
	uint width, height, elements, levels;
	tex.GetDimensions((uint)lod, width, height, elements, levels);
	return int3(width, height, elements);
}


int latteIntegerTextureLevel(float lod, uint levels)
{
	return (int)clamp(floor(lod + 0.5f), 0.0f, (float)(levels - 1u));
}

uint4 latteIntegerTextureLoad(Texture1D<uint4> tex, SamplerState samplerState, float coord, float lod, int offset)
{
	uint width, levels;
	tex.GetDimensions(0u, width, levels);
	const int level = latteIntegerTextureLevel(lod, levels);
	tex.GetDimensions((uint)level, width, levels);
	return tex.Load(int2(clamp((int)floor(coord * width) + offset, 0, (int)width - 1), level));
}

uint4 latteIntegerTextureLoad(Texture1DArray<uint4> tex, SamplerState samplerState, float2 coord, float lod, int offset)
{
	uint width, elements, levels;
	tex.GetDimensions(0u, width, elements, levels);
	const int level = latteIntegerTextureLevel(lod, levels);
	tex.GetDimensions((uint)level, width, elements, levels);
	const int layer = clamp((int)floor(coord.y + 0.5f), 0, (int)elements - 1);
	return tex.Load(int3(clamp((int)floor(coord.x * width) + offset, 0, (int)width - 1), layer, level));
}

uint4 latteIntegerTextureLoad(Texture2D<uint4> tex, SamplerState samplerState, float2 coord, float lod, int2 offset)
{
	uint width, height, levels;
	tex.GetDimensions(0u, width, height, levels);
	const int level = latteIntegerTextureLevel(lod, levels);
	tex.GetDimensions((uint)level, width, height, levels);
	const int2 size = int2(width, height);
	return tex.Load(int3(clamp((int2)floor(coord * size) + offset, 0, size - 1), level));
}

uint4 latteIntegerTextureLoad(Texture2DArray<uint4> tex, SamplerState samplerState, float3 coord, float lod, int2 offset)
{
	uint width, height, elements, levels;
	tex.GetDimensions(0u, width, height, elements, levels);
	const int level = latteIntegerTextureLevel(lod, levels);
	tex.GetDimensions((uint)level, width, height, elements, levels);
	const int2 size = int2(width, height);
	const int layer = clamp((int)floor(coord.z + 0.5f), 0, (int)elements - 1);
	return tex.Load(int4(clamp((int2)floor(coord.xy * size) + offset, 0, size - 1), layer, level));
}

uint4 latteIntegerTextureLoad(Texture3D<uint4> tex, SamplerState samplerState, float3 coord, float lod, int3 offset)
{
	uint width, height, depth, levels;
	tex.GetDimensions(0u, width, height, depth, levels);
	const int level = latteIntegerTextureLevel(lod, levels);
	tex.GetDimensions((uint)level, width, height, depth, levels);
	const int3 size = int3(width, height, depth);
	return tex.Load(int4(clamp((int3)floor(coord * size) + offset, 0, size - 1), level));
}

// cube resources cannot be loaded; gather reads the most detailed mip in the order (0,1) (1,1) (1,0) (0,0)
uint4 latteIntegerTextureLoad(TextureCubeArray<uint4> tex, SamplerState samplerState, float4 coord, float lod)
{
	uint width, height, elements, levels;
	tex.GetDimensions(0u, width, height, elements, levels);
	const float2 footprint = frac(latteCubeFaceCoord(coord.xyz) * float2(width, height) - 0.5f);
	const uint corner = footprint.y >= 0.5f ? (footprint.x >= 0.5f ? 1u : 0u) : (footprint.x >= 0.5f ? 2u : 3u);
	return uint4(tex.GatherRed(samplerState, coord)[corner], tex.GatherGreen(samplerState, coord)[corner], tex.GatherBlue(samplerState, coord)[corner], tex.GatherAlpha(samplerState, coord)[corner]);
}

float latteIntegerTextureLod(Texture1D<uint4> tex, float dx, float dy)
{
	uint width, levels;
	tex.GetDimensions(0u, width, levels);
	return latteTextureLodFromGradients(float3(dx * width, 0.0f, 0.0f), float3(dy * width, 0.0f, 0.0f));
}

float latteIntegerTextureLod(Texture1DArray<uint4> tex, float2 dx, float2 dy)
{
	uint width, elements, levels;
	tex.GetDimensions(0u, width, elements, levels);
	return latteTextureLodFromGradients(float3(dx.x * width, 0.0f, 0.0f), float3(dy.x * width, 0.0f, 0.0f));
}

float latteIntegerTextureLod(Texture2D<uint4> tex, float2 dx, float2 dy)
{
	uint width, height, levels;
	tex.GetDimensions(0u, width, height, levels);
	return latteTextureLodFromGradients(float3(dx * float2(width, height), 0.0f), float3(dy * float2(width, height), 0.0f));
}

float latteIntegerTextureLod(Texture2DArray<uint4> tex, float3 dx, float3 dy)
{
	uint width, height, elements, levels;
	tex.GetDimensions(0u, width, height, elements, levels);
	return latteTextureLodFromGradients(float3(dx.xy * float2(width, height), 0.0f), float3(dy.xy * float2(width, height), 0.0f));
}

float latteIntegerTextureLod(Texture3D<uint4> tex, float3 dx, float3 dy)
{
	uint width, height, depth, levels;
	tex.GetDimensions(0u, width, height, depth, levels);
	return latteTextureLodFromGradients(dx * float3(width, height, depth), dy * float3(width, height, depth));
}

float latteIntegerTextureLod(TextureCubeArray<uint4> tex, float4 dx, float4 dy)
{
	return 0.0f;
}


#if defined(LATTE_HAS_SAMPLE_CMP_LEVEL)
#define LATTE_SAMPLE_CMP_LEVEL(tex, samplerState, location, reference, lod) tex.SampleCmpLevel(samplerState, location, reference, lod)
#define LATTE_SAMPLE_CMP_LEVEL_OFFSET(tex, samplerState, location, reference, lod, offset) tex.SampleCmpLevel(samplerState, location, reference, lod, offset)
#else
#define LATTE_SAMPLE_CMP_LEVEL(tex, samplerState, location, reference, lod) tex.SampleCmpLevelZero(samplerState, location, reference)
#define LATTE_SAMPLE_CMP_LEVEL_OFFSET(tex, samplerState, location, reference, lod, offset) tex.SampleCmpLevelZero(samplerState, location, reference, offset)
#endif


#define LATTE_CONCAT_INNER(a, b) a##b
#define LATTE_CONCAT(a, b) LATTE_CONCAT_INNER(a, b)
#define LATTE_TEXTURE_SAMPLER(tex) LATTE_CONCAT(tex, Sampler)
#define texture(tex, ...) latteTextureSample(tex, LATTE_TEXTURE_SAMPLER(tex), __VA_ARGS__)
#define textureOffset(tex, ...) latteTextureSampleOffset(tex, LATTE_TEXTURE_SAMPLER(tex), __VA_ARGS__)
#define textureGrad(tex, coord, dx, dy) latteTextureGrad(tex, LATTE_TEXTURE_SAMPLER(tex), coord, dx, dy)
#define textureGradOffset(tex, coord, dx, dy, offset) latteTextureGradOffset(tex, LATTE_TEXTURE_SAMPLER(tex), coord, dx, dy, offset)
#define textureLod(tex, ...) latteTextureSampleLevel(tex, LATTE_TEXTURE_SAMPLER(tex), __VA_ARGS__)
#define textureLodOffset(tex, ...) latteTextureSampleLevelOffset(tex, LATTE_TEXTURE_SAMPLER(tex), __VA_ARGS__)
#define texelFetch(tex, coord, lod) latteTexelFetch(tex, coord, lod)
#define textureGather(tex, coord) latteTextureGather(tex, LATTE_TEXTURE_SAMPLER(tex), coord)

cbuffer Uniforms : register(b0) { float4 uf_remappedPS[16]; float uf_alphaTestRef; };
Texture2D<float4> tex0 : register(t0);
SamplerState tex0Sampler : register(s0);
struct PSIn { float4 pos : SV_Position; float4 passParameterSem0 : TEXCOORD0; };
float4 main(PSIn input) : SV_Target
{
	float4 R0f = input.passParameterSem0;
	ivec2 coord = ivec2(int(R0f.x), 3);
	uvec4 bits = uvec4(floatBitsToUint(R0f.x), 1u, 2u, 3u);
	float4 R1f = vec4(R0f.xy, vec2(0.5f, 1.0f));
	R1f = mix(R1f, vec4(uf_remappedPS[0].x), bvec4(true, false, true, false));
	R1f.x = mul_nonIEEE(R1f.x, uf_remappedPS[1].y) + float(coord.y) + asfloat(bits.x);
	return R1f + tex0.Sample(tex0Sampler, R0f.xy);
}
