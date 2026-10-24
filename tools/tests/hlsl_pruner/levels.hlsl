Texture1D<float4> t0 : register(t0);
Texture1D<uint4> t1 : register(t1);
Texture1D<float> t2 : register(t2);
Texture1DArray<float4> t3 : register(t3);
Texture1DArray<uint4> t4 : register(t4);
Texture1DArray<float> t5 : register(t5);
Texture2D<float4> t6 : register(t6);
Texture2D<uint4> t7 : register(t7);
Texture2D<float> t8 : register(t8);
Texture2DArray<float4> t9 : register(t9);
Texture2DArray<uint4> t10 : register(t10);
Texture2DArray<float> t11 : register(t11);
Texture3D<float4> t12 : register(t12);
Texture3D<uint4> t13 : register(t13);
TextureCubeArray<float4> t14 : register(t14);
TextureCubeArray<uint4> t15 : register(t15);
TextureCubeArray<float> t16 : register(t16);


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


float4 main() : SV_Target {
int l = 0;
	l += textureQueryLevels(t0);
	l += textureQueryLevels(t1);
	l += textureQueryLevels(t2);
	l += textureQueryLevels(t3);
	l += textureQueryLevels(t4);
	l += textureQueryLevels(t5);
	l += textureQueryLevels(t6);
	l += textureQueryLevels(t7);
	l += textureQueryLevels(t8);
	l += textureQueryLevels(t9);
	l += textureQueryLevels(t10);
	l += textureQueryLevels(t11);
	l += textureQueryLevels(t12);
	l += textureQueryLevels(t13);
	l += textureQueryLevels(t14);
	l += textureQueryLevels(t15);
	l += textureQueryLevels(t16);
	return l;
}
