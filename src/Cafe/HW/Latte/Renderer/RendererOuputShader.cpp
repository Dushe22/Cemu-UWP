#include "Cafe/HW/Latte/Renderer/RendererOuputShader.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "config/ActiveSettings.h"

const std::string RendererOutputShader::s_copy_shader_source =
R"(
void outputShader()
{
	colorOut0 = vec4(texture(textureSrc, passUV).rgb,1.0);
}
)";

const std::string RendererOutputShader::s_copy_shader_source_mtl =
R"(#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float2 uv;
};

fragment float4 main0(VertexOut in [[stage_in]], texture2d<float> textureSrc [[texture(0)]], sampler samplr [[sampler(0)]]) {
	return float4(textureSrc.sample(samplr, in.uv).rgb, 1.0);
}
)";

const std::string RendererOutputShader::s_bicubic_shader_source =
R"(
vec4 cubic(float x)
{
	float x2 = x * x;
	float x3 = x2 * x;
	vec4 w;
	w.x = -x3 + 3 * x2 - 3 * x + 1;
	w.y = 3 * x3 - 6 * x2 + 4;
	w.z = -3 * x3 + 3 * x2 + 3 * x + 1;
	w.w = x3;
	return w / 6.0;
}

vec4 bcFilter(vec2 uv, vec4 texelSize)
{
	vec2 pixel = uv*texelSize.zw - 0.5;
	vec2 pixelFrac = fract(pixel);
	vec2 pixelInt = pixel - pixelFrac;

	vec4 xcubic = cubic(pixelFrac.x);
	vec4 ycubic = cubic(pixelFrac.y);

	vec4 c = vec4(pixelInt.x - 0.5, pixelInt.x + 1.5, pixelInt.y - 0.5, pixelInt.y + 1.5);
	vec4 s = vec4(xcubic.x + xcubic.y, xcubic.z + xcubic.w, ycubic.x + ycubic.y, ycubic.z + ycubic.w);
	vec4 offset = c + vec4(xcubic.y, xcubic.w, ycubic.y, ycubic.w) / s;

	vec4 sample0 = texture(textureSrc, vec2(offset.x, offset.z) * texelSize.xy);
	vec4 sample1 = texture(textureSrc, vec2(offset.y, offset.z) * texelSize.xy);
	vec4 sample2 = texture(textureSrc, vec2(offset.x, offset.w) * texelSize.xy);
	vec4 sample3 = texture(textureSrc, vec2(offset.y, offset.w) * texelSize.xy);

	float sx = s.x / (s.x + s.y);
	float sy = s.z / (s.z + s.w);

	return mix(
		mix(sample3, sample2, sx),
		mix(sample1, sample0, sx), sy);
}

void outputShader(){
	vec4 texelSize = vec4( 1.0 / textureSrcResolution.xy, textureSrcResolution.xy);
	colorOut0 = vec4(bcFilter(passUV, texelSize).rgb,1.0);
}
)";

const std::string RendererOutputShader::s_bicubic_shader_source_mtl =
R"(#include <metal_stdlib>
using namespace metal;

float4 cubic(float x) {
	float x2 = x * x;
	float x3 = x2 * x;
	float4 w;
	w.x = -x3 + 3 * x2 - 3 * x + 1;
	w.y = 3 * x3 - 6 * x2 + 4;
	w.z = -3 * x3 + 3 * x2 + 3 * x + 1;
	w.w = x3;
	return w / 6.0;
}

float4 bcFilter(texture2d<float> textureSrc, sampler samplr, float2 texcoord, float2 texscale) {
	float fx = fract(texcoord.x);
	float fy = fract(texcoord.y);
	texcoord.x -= fx;
	texcoord.y -= fy;

	float4 xcubic = cubic(fx);
	float4 ycubic = cubic(fy);

	float4 c = float4(texcoord.x - 0.5, texcoord.x + 1.5, texcoord.y - 0.5, texcoord.y + 1.5);
	float4 s = float4(xcubic.x + xcubic.y, xcubic.z + xcubic.w, ycubic.x + ycubic.y, ycubic.z + ycubic.w);
	float4 offset = c + float4(xcubic.y, xcubic.w, ycubic.y, ycubic.w) / s;

	float4 sample0 = textureSrc.sample(samplr, float2(offset.x, offset.z) * texscale);
	float4 sample1 = textureSrc.sample(samplr, float2(offset.y, offset.z) * texscale);
	float4 sample2 = textureSrc.sample(samplr, float2(offset.x, offset.w) * texscale);
	float4 sample3 = textureSrc.sample(samplr, float2(offset.y, offset.w) * texscale);

	float sx = s.x / (s.x + s.y);
	float sy = s.z / (s.z + s.w);

	return mix(
		mix(sample3, sample2, sx),
		mix(sample1, sample0, sx), sy);
}

struct VertexOut {
    float2 uv;
};

fragment float4 main0(VertexOut in [[stage_in]], texture2d<float> textureSrc [[texture(0)]], sampler samplr [[sampler(0)]]) {
    float2 textureSrcResolution = float2(textureSrc.get_width(), textureSrc.get_height());
	return float4(bcFilter(textureSrc, samplr, in.uv * textureSrcResolution, float2(1.0, 1.0) / textureSrcResolution).rgb, 1.0);
}
)";

const std::string RendererOutputShader::s_hermite_shader_source =
R"(
// https://www.shadertoy.com/view/MllSzX

vec3 CubicHermite (vec3 A, vec3 B, vec3 C, vec3 D, float t)
{
	float t2 = t*t;
    float t3 = t*t*t;
    vec3 a = -A/2.0 + (3.0*B)/2.0 - (3.0*C)/2.0 + D/2.0;
    vec3 b = A - (5.0*B)/2.0 + 2.0*C - D / 2.0;
    vec3 c = -A/2.0 + C/2.0;
   	vec3 d = B;

    return a*t3 + b*t2 + c*t + d;
}


vec3 BicubicHermiteTexture(vec2 uv, vec4 texelSize)
{
	vec2 pixel = uv*texelSize.zw + 0.5;
	vec2 frac = fract(pixel);
    pixel = floor(pixel) / texelSize.zw - vec2(texelSize.xy/2.0);

	vec4 doubleSize = texelSize*2.0;

	vec3 C00 = texture(textureSrc, pixel + vec2(-texelSize.x ,-texelSize.y)).rgb;
    vec3 C10 = texture(textureSrc, pixel + vec2( 0.0        ,-texelSize.y)).rgb;
    vec3 C20 = texture(textureSrc, pixel + vec2( texelSize.x ,-texelSize.y)).rgb;
    vec3 C30 = texture(textureSrc, pixel + vec2( doubleSize.x,-texelSize.y)).rgb;

    vec3 C01 = texture(textureSrc, pixel + vec2(-texelSize.x , 0.0)).rgb;
    vec3 C11 = texture(textureSrc, pixel + vec2( 0.0        , 0.0)).rgb;
    vec3 C21 = texture(textureSrc, pixel + vec2( texelSize.x , 0.0)).rgb;
    vec3 C31 = texture(textureSrc, pixel + vec2( doubleSize.x, 0.0)).rgb;

    vec3 C02 = texture(textureSrc, pixel + vec2(-texelSize.x , texelSize.y)).rgb;
    vec3 C12 = texture(textureSrc, pixel + vec2( 0.0        , texelSize.y)).rgb;
    vec3 C22 = texture(textureSrc, pixel + vec2( texelSize.x , texelSize.y)).rgb;
    vec3 C32 = texture(textureSrc, pixel + vec2( doubleSize.x, texelSize.y)).rgb;

    vec3 C03 = texture(textureSrc, pixel + vec2(-texelSize.x , doubleSize.y)).rgb;
    vec3 C13 = texture(textureSrc, pixel + vec2( 0.0        , doubleSize.y)).rgb;
    vec3 C23 = texture(textureSrc, pixel + vec2( texelSize.x , doubleSize.y)).rgb;
    vec3 C33 = texture(textureSrc, pixel + vec2( doubleSize.x, doubleSize.y)).rgb;

    vec3 CP0X = CubicHermite(C00, C10, C20, C30, frac.x);
    vec3 CP1X = CubicHermite(C01, C11, C21, C31, frac.x);
    vec3 CP2X = CubicHermite(C02, C12, C22, C32, frac.x);
    vec3 CP3X = CubicHermite(C03, C13, C23, C33, frac.x);

    return CubicHermite(CP0X, CP1X, CP2X, CP3X, frac.y);
}

void outputShader(){
	vec4 texelSize = vec4( 1.0 / textureSrcResolution.xy, textureSrcResolution.xy);
	colorOut0 = vec4(BicubicHermiteTexture(passUV, texelSize), 1.0);
}
)";

const std::string RendererOutputShader::s_hermite_shader_source_mtl =
R"(#include <metal_stdlib>
using namespace metal;

// https://www.shadertoy.com/view/MllSzX

float3 CubicHermite(float3 A, float3 B, float3 C, float3 D, float t) {
	float t2 = t*t;
    float t3 = t*t*t;
    float3 a = -A/2.0 + (3.0*B)/2.0 - (3.0*C)/2.0 + D/2.0;
    float3 b = A - (5.0*B)/2.0 + 2.0*C - D / 2.0;
    float3 c = -A/2.0 + C/2.0;
   	float3 d = B;

    return a*t3 + b*t2 + c*t + d;
}


float3 BicubicHermiteTexture(texture2d<float> textureSrc, sampler samplr, float2 uv, float4 texelSize) {
	float2 pixel = uv*texelSize.zw + 0.5;
	float2 frac = fract(pixel);
    pixel = floor(pixel) / texelSize.zw - float2(texelSize.xy/2.0);

	float4 doubleSize = texelSize*texelSize;

	float3 C00 = textureSrc.sample(samplr, pixel + float2(-texelSize.x ,-texelSize.y)).rgb;
    float3 C10 = textureSrc.sample(samplr, pixel + float2( 0.0        ,-texelSize.y)).rgb;
    float3 C20 = textureSrc.sample(samplr, pixel + float2( texelSize.x ,-texelSize.y)).rgb;
    float3 C30 = textureSrc.sample(samplr, pixel + float2( doubleSize.x,-texelSize.y)).rgb;

    float3 C01 = textureSrc.sample(samplr, pixel + float2(-texelSize.x , 0.0)).rgb;
    float3 C11 = textureSrc.sample(samplr, pixel + float2( 0.0        , 0.0)).rgb;
    float3 C21 = textureSrc.sample(samplr, pixel + float2( texelSize.x , 0.0)).rgb;
    float3 C31 = textureSrc.sample(samplr, pixel + float2( doubleSize.x, 0.0)).rgb;

    float3 C02 = textureSrc.sample(samplr, pixel + float2(-texelSize.x , texelSize.y)).rgb;
    float3 C12 = textureSrc.sample(samplr, pixel + float2( 0.0        , texelSize.y)).rgb;
    float3 C22 = textureSrc.sample(samplr, pixel + float2( texelSize.x , texelSize.y)).rgb;
    float3 C32 = textureSrc.sample(samplr, pixel + float2( doubleSize.x, texelSize.y)).rgb;

    float3 C03 = textureSrc.sample(samplr, pixel + float2(-texelSize.x , doubleSize.y)).rgb;
    float3 C13 = textureSrc.sample(samplr, pixel + float2( 0.0        , doubleSize.y)).rgb;
    float3 C23 = textureSrc.sample(samplr, pixel + float2( texelSize.x , doubleSize.y)).rgb;
    float3 C33 = textureSrc.sample(samplr, pixel + float2( doubleSize.x, doubleSize.y)).rgb;

    float3 CP0X = CubicHermite(C00, C10, C20, C30, frac.x);
    float3 CP1X = CubicHermite(C01, C11, C21, C31, frac.x);
    float3 CP2X = CubicHermite(C02, C12, C22, C32, frac.x);
    float3 CP3X = CubicHermite(C03, C13, C23, C33, frac.x);

    return CubicHermite(CP0X, CP1X, CP2X, CP3X, frac.y);
}

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

fragment float4 main0(VertexOut in [[stage_in]], texture2d<float> textureSrc [[texture(0)]], sampler samplr [[sampler(0)]], constant float2& outputResolution [[buffer(0)]]) {
	float4 texelSize = float4(1.0 / outputResolution.xy, outputResolution.xy);
	return float4(BicubicHermiteTexture(textureSrc, samplr, in.uv, texelSize), 1.0);
}
)";

const std::string RendererOutputShader::s_copy_shader_source_hlsl =
R"(
Texture2D<float4> textureSrc : register(t0);
SamplerState textureSampler : register(s0);

cbuffer parameters : register(b0)
{
	float2 textureSrcResolution;
	float2 nativeResolution;
	float2 outputResolution;
	uint applySRGBEncoding;
	float targetGamma;
	float displayGamma;
};

struct PSInput
{
	float4 position : SV_Position;
	float2 passUV : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
	return float4(textureSrc.Sample(textureSampler, input.passUV).rgb, 1.0);
}
)";

const std::string RendererOutputShader::s_bicubic_shader_source_hlsl =
R"(
Texture2D<float4> textureSrc : register(t0);
SamplerState textureSampler : register(s0);

cbuffer parameters : register(b0)
{
	float2 textureSrcResolution;
	float2 nativeResolution;
	float2 outputResolution;
	uint applySRGBEncoding;
	float targetGamma;
	float displayGamma;
};

struct PSInput
{
	float4 position : SV_Position;
	float2 passUV : TEXCOORD0;
};

float4 cubic(float x)
{
	float x2 = x * x;
	float x3 = x2 * x;
	float4 w;
	w.x = -x3 + 3.0 * x2 - 3.0 * x + 1.0;
	w.y = 3.0 * x3 - 6.0 * x2 + 4.0;
	w.z = -3.0 * x3 + 3.0 * x2 + 3.0 * x + 1.0;
	w.w = x3;
	return w / 6.0;
}

float4 bcFilter(float2 uv, float4 texelSize)
{
	float2 pixel = uv * texelSize.zw - 0.5;
	float2 pixelFrac = frac(pixel);
	float2 pixelInt = pixel - pixelFrac;

	float4 xcubic = cubic(pixelFrac.x);
	float4 ycubic = cubic(pixelFrac.y);

	float4 c = float4(pixelInt.x - 0.5, pixelInt.x + 1.5, pixelInt.y - 0.5, pixelInt.y + 1.5);
	float4 s = float4(xcubic.x + xcubic.y, xcubic.z + xcubic.w, ycubic.x + ycubic.y, ycubic.z + ycubic.w);
	float4 offset = c + float4(xcubic.y, xcubic.w, ycubic.y, ycubic.w) / s;

	float4 sample0 = textureSrc.Sample(textureSampler, float2(offset.x, offset.z) * texelSize.xy);
	float4 sample1 = textureSrc.Sample(textureSampler, float2(offset.y, offset.z) * texelSize.xy);
	float4 sample2 = textureSrc.Sample(textureSampler, float2(offset.x, offset.w) * texelSize.xy);
	float4 sample3 = textureSrc.Sample(textureSampler, float2(offset.y, offset.w) * texelSize.xy);

	float sx = s.x / (s.x + s.y);
	float sy = s.z / (s.z + s.w);

	return lerp(lerp(sample3, sample2, sx), lerp(sample1, sample0, sx), sy);
}

float4 main(PSInput input) : SV_Target0
{
	float4 texelSize = float4(1.0 / textureSrcResolution.xy, textureSrcResolution.xy);
	return float4(bcFilter(input.passUV, texelSize).rgb, 1.0);
}
)";

const std::string RendererOutputShader::s_hermite_shader_source_hlsl =
R"(
Texture2D<float4> textureSrc : register(t0);
SamplerState textureSampler : register(s0);

cbuffer parameters : register(b0)
{
	float2 textureSrcResolution;
	float2 nativeResolution;
	float2 outputResolution;
	uint applySRGBEncoding;
	float targetGamma;
	float displayGamma;
};

struct PSInput
{
	float4 position : SV_Position;
	float2 passUV : TEXCOORD0;
};

float3 CubicHermite(float3 A, float3 B, float3 C, float3 D, float t)
{
	float t2 = t * t;
	float t3 = t2 * t;
	float3 a = -A / 2.0 + (3.0 * B) / 2.0 - (3.0 * C) / 2.0 + D / 2.0;
	float3 b = A - (5.0 * B) / 2.0 + 2.0 * C - D / 2.0;
	float3 c = -A / 2.0 + C / 2.0;
	float3 d = B;
	return a * t3 + b * t2 + c * t + d;
}

float3 BicubicHermiteTexture(float2 uv, float4 texelSize)
{
	float2 pixel = uv * texelSize.zw + 0.5;
	float2 fracPart = frac(pixel);
	pixel = floor(pixel) / texelSize.zw - float2(texelSize.xy / 2.0);

	float4 doubleSize = texelSize * 2.0;

	float3 C00 = textureSrc.Sample(textureSampler, pixel + float2(-texelSize.x, -texelSize.y)).rgb;
	float3 C10 = textureSrc.Sample(textureSampler, pixel + float2(0.0, -texelSize.y)).rgb;
	float3 C20 = textureSrc.Sample(textureSampler, pixel + float2(texelSize.x, -texelSize.y)).rgb;
	float3 C30 = textureSrc.Sample(textureSampler, pixel + float2(doubleSize.x, -texelSize.y)).rgb;

	float3 C01 = textureSrc.Sample(textureSampler, pixel + float2(-texelSize.x, 0.0)).rgb;
	float3 C11 = textureSrc.Sample(textureSampler, pixel + float2(0.0, 0.0)).rgb;
	float3 C21 = textureSrc.Sample(textureSampler, pixel + float2(texelSize.x, 0.0)).rgb;
	float3 C31 = textureSrc.Sample(textureSampler, pixel + float2(doubleSize.x, 0.0)).rgb;

	float3 C02 = textureSrc.Sample(textureSampler, pixel + float2(-texelSize.x, texelSize.y)).rgb;
	float3 C12 = textureSrc.Sample(textureSampler, pixel + float2(0.0, texelSize.y)).rgb;
	float3 C22 = textureSrc.Sample(textureSampler, pixel + float2(texelSize.x, texelSize.y)).rgb;
	float3 C32 = textureSrc.Sample(textureSampler, pixel + float2(doubleSize.x, texelSize.y)).rgb;

	float3 C03 = textureSrc.Sample(textureSampler, pixel + float2(-texelSize.x, doubleSize.y)).rgb;
	float3 C13 = textureSrc.Sample(textureSampler, pixel + float2(0.0, doubleSize.y)).rgb;
	float3 C23 = textureSrc.Sample(textureSampler, pixel + float2(texelSize.x, doubleSize.y)).rgb;
	float3 C33 = textureSrc.Sample(textureSampler, pixel + float2(doubleSize.x, doubleSize.y)).rgb;

	float3 CP0X = CubicHermite(C00, C10, C20, C30, fracPart.x);
	float3 CP1X = CubicHermite(C01, C11, C21, C31, fracPart.x);
	float3 CP2X = CubicHermite(C02, C12, C22, C32, fracPart.x);
	float3 CP3X = CubicHermite(C03, C13, C23, C33, fracPart.x);

	return CubicHermite(CP0X, CP1X, CP2X, CP3X, fracPart.y);
}

float4 main(PSInput input) : SV_Target0
{
	float4 texelSize = float4(1.0 / textureSrcResolution.xy, textureSrcResolution.xy);
	return float4(BicubicHermiteTexture(input.passUV, texelSize), 1.0);
}
)";

// AMD FidelityFX Super Resolution 1.0 edge adaptive spatial upsampling (EASU), the 32-bit path of ffx_fsr1.h.
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved. MIT license.
// Draws at the output size from the whole game image; FsrEasuCon is folded into the shader.
const std::string RendererOutputShader::s_fsr_easu_shader_source_hlsl =
R"(
Texture2D<float4> textureSrc : register(t0);
SamplerState textureSampler : register(s0);

cbuffer parameters : register(b0)
{
	float2 textureSrcResolution;
	float2 nativeResolution;
	float2 outputResolution;
	uint applySRGBEncoding;
	float targetGamma;
	float displayGamma;
	float sharpness;
};

struct PSInput
{
	float4 position : SV_Position;
	float2 passUV : TEXCOORD0;
};

float FsrRcpLow(float a)
{
	return asfloat(0x7ef07ebbu - asuint(a));
}

float FsrRsqLow(float a)
{
	return asfloat(0x5f347d74u - (asuint(a) >> 1));
}

// direction and edge length from the '+' around c, weighted for one corner of the bilinear footprint
void FsrEasuSet(inout float2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE)
{
	float dc = lD - lC;
	float cb = lC - lB;
	float lenX = FsrRcpLow(max(abs(dc), abs(cb)));
	float dirX = lD - lB;
	dir.x += dirX * w;
	lenX = saturate(abs(dirX) * lenX);
	lenX *= lenX;
	len += lenX * w;

	float ec = lE - lC;
	float ca = lC - lA;
	float lenY = FsrRcpLow(max(abs(ec), abs(ca)));
	float dirY = lE - lA;
	dir.y += dirY * w;
	lenY = saturate(abs(dirY) * lenY);
	lenY *= lenY;
	len += lenY * w;
}

// one tap of the rotated, anisotropic approximation of lanczos2
void FsrEasuTap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c)
{
	float2 v;
	v.x = (off.x * dir.x) + (off.y * dir.y);
	v.y = (off.x * -dir.y) + (off.y * dir.x);
	v *= len;
	float d2 = min(v.x * v.x + v.y * v.y, clp);
	float wB = (2.0 / 5.0) * d2 - 1.0;
	float wA = lob * d2 - 1.0;
	wB *= wB;
	wA *= wA;
	wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
	float w = wB * wA;
	aC += c * w;
	aW += w;
}

float4 main(PSInput input) : SV_Target0
{
	float2 rcpInput = 1.0 / textureSrcResolution;
	float2 scale = textureSrcResolution / outputResolution;
	float2 pp = floor(input.position.xy) * scale + (0.5 * scale - 0.5);
	float2 fp = floor(pp);
	pp -= fp;

	// 12-tap kernel
	//    b c
	//  e f g h
	//  i j k l
	//    n o
	float2 p0 = fp * rcpInput + float2(1.0, -1.0) * rcpInput;
	float2 p1 = p0 + float2(-1.0, 2.0) * rcpInput;
	float2 p2 = p0 + float2(1.0, 2.0) * rcpInput;
	float2 p3 = p0 + float2(0.0, 4.0) * rcpInput;
	float4 bczzR = textureSrc.GatherRed(textureSampler, p0);
	float4 bczzG = textureSrc.GatherGreen(textureSampler, p0);
	float4 bczzB = textureSrc.GatherBlue(textureSampler, p0);
	float4 ijfeR = textureSrc.GatherRed(textureSampler, p1);
	float4 ijfeG = textureSrc.GatherGreen(textureSampler, p1);
	float4 ijfeB = textureSrc.GatherBlue(textureSampler, p1);
	float4 klhgR = textureSrc.GatherRed(textureSampler, p2);
	float4 klhgG = textureSrc.GatherGreen(textureSampler, p2);
	float4 klhgB = textureSrc.GatherBlue(textureSampler, p2);
	float4 zzonR = textureSrc.GatherRed(textureSampler, p3);
	float4 zzonG = textureSrc.GatherGreen(textureSampler, p3);
	float4 zzonB = textureSrc.GatherBlue(textureSampler, p3);

	// luma times two
	float4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
	float4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
	float4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
	float4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
	float bL = bczzL.x;
	float cL = bczzL.y;
	float iL = ijfeL.x;
	float jL = ijfeL.y;
	float fL = ijfeL.z;
	float eL = ijfeL.w;
	float kL = klhgL.x;
	float lL = klhgL.y;
	float hL = klhgL.z;
	float gL = klhgL.w;
	float oL = zzonL.z;
	float nL = zzonL.w;

	float2 dir = float2(0.0, 0.0);
	float len = 0.0;
	FsrEasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
	FsrEasuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
	FsrEasuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
	FsrEasuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

	// normalize the direction, close to zero falls back to horizontal
	float2 dir2 = dir * dir;
	float dirR = dir2.x + dir2.y;
	bool zro = dirR < (1.0 / 32768.0);
	dirR = FsrRsqLow(dirR);
	dirR = zro ? 1.0 : dirR;
	dir.x = zro ? 1.0 : dir.x;
	dir *= dirR;
	len = len * 0.5;
	len *= len;
	// the kernel stretches from 1.0 on axes to sqrt(2.0) on diagonals
	float stretch = (dir.x * dir.x + dir.y * dir.y) * FsrRcpLow(max(abs(dir.x), abs(dir.y)));
	float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
	float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
	float clp = FsrRcpLow(lob);

	float3 min4 = min(min(float3(ijfeR.z, ijfeG.z, ijfeB.z), float3(klhgR.w, klhgG.w, klhgB.w)),
		min(float3(ijfeR.y, ijfeG.y, ijfeB.y), float3(klhgR.x, klhgG.x, klhgB.x)));
	float3 max4 = max(max(float3(ijfeR.z, ijfeG.z, ijfeB.z), float3(klhgR.w, klhgG.w, klhgB.w)),
		max(float3(ijfeR.y, ijfeG.y, ijfeB.y), float3(klhgR.x, klhgG.x, klhgB.x)));

	float3 aC = float3(0.0, 0.0, 0.0);
	float aW = 0.0;
	FsrEasuTap(aC, aW, float2( 0.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.x, bczzG.x, bczzB.x));
	FsrEasuTap(aC, aW, float2( 1.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.y, bczzG.y, bczzB.y));
	FsrEasuTap(aC, aW, float2(-1.0,  1.0) - pp, dir, len2, lob, clp, float3(ijfeR.x, ijfeG.x, ijfeB.x));
	FsrEasuTap(aC, aW, float2( 0.0,  1.0) - pp, dir, len2, lob, clp, float3(ijfeR.y, ijfeG.y, ijfeB.y));
	FsrEasuTap(aC, aW, float2( 0.0,  0.0) - pp, dir, len2, lob, clp, float3(ijfeR.z, ijfeG.z, ijfeB.z));
	FsrEasuTap(aC, aW, float2(-1.0,  0.0) - pp, dir, len2, lob, clp, float3(ijfeR.w, ijfeG.w, ijfeB.w));
	FsrEasuTap(aC, aW, float2( 1.0,  1.0) - pp, dir, len2, lob, clp, float3(klhgR.x, klhgG.x, klhgB.x));
	FsrEasuTap(aC, aW, float2( 2.0,  1.0) - pp, dir, len2, lob, clp, float3(klhgR.y, klhgG.y, klhgB.y));
	FsrEasuTap(aC, aW, float2( 2.0,  0.0) - pp, dir, len2, lob, clp, float3(klhgR.z, klhgG.z, klhgB.z));
	FsrEasuTap(aC, aW, float2( 1.0,  0.0) - pp, dir, len2, lob, clp, float3(klhgR.w, klhgG.w, klhgB.w));
	FsrEasuTap(aC, aW, float2( 1.0,  2.0) - pp, dir, len2, lob, clp, float3(zzonR.z, zzonG.z, zzonB.z));
	FsrEasuTap(aC, aW, float2( 0.0,  2.0) - pp, dir, len2, lob, clp, float3(zzonR.w, zzonG.w, zzonB.w));

	// normalize and remove ringing
	return float4(min(max4, max(min4, aC * (1.0 / aW))), 1.0);
}
)";

// AMD FidelityFX Super Resolution 1.0 robust contrast adaptive sharpening (RCAS) with noise removal, the 32-bit path of ffx_fsr1.h.
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved. MIT license.
// Reads the EASU result one to one; the colour transfer already ran there, so this pass writes its result unchanged.
const std::string RendererOutputShader::s_fsr_rcas_shader_source_hlsl =
R"(
Texture2D<float4> textureSrc : register(t0);
SamplerState textureSampler : register(s0);

cbuffer parameters : register(b0)
{
	float2 textureSrcResolution;
	float2 nativeResolution;
	float2 outputResolution;
	uint applySRGBEncoding;
	float targetGamma;
	float displayGamma;
	float sharpness;
};

struct PSInput
{
	float4 position : SV_Position;
	float2 passUV : TEXCOORD0;
};

#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

float FsrRcpMedium(float a)
{
	float b = asfloat(0x7ef19fffu - asuint(a));
	return b * (-b * a + 2.0);
}

float FsrMax3(float a, float b, float c)
{
	return max(a, max(b, c));
}

float FsrMin3(float a, float b, float c)
{
	return min(a, min(b, c));
}

float3 FsrRcasLoad(int2 position)
{
	int2 lastPixel = int2(outputResolution) - 1;
	return textureSrc.Load(int3(clamp(position, int2(0, 0), lastPixel), 0)).rgb;
}

float4 main(PSInput input) : SV_Target0
{
	// the UV names the pixel, so drawing upside down flips the image too
	int2 sp = int2(input.passUV * outputResolution);
	//    b
	//  d e f
	//    h
	float3 b = FsrRcasLoad(sp + int2(0, -1));
	float3 d = FsrRcasLoad(sp + int2(-1, 0));
	float3 e = FsrRcasLoad(sp);
	float3 f = FsrRcasLoad(sp + int2(1, 0));
	float3 h = FsrRcasLoad(sp + int2(0, 1));

	// luma times two
	float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
	float dL = d.b * 0.5 + (d.r * 0.5 + d.g);
	float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
	float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
	float hL = h.b * 0.5 + (h.r * 0.5 + h.g);

	// noise detection
	float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
	nz = saturate(abs(nz) * FsrRcpMedium(FsrMax3(FsrMax3(bL, dL, eL), fL, hL) - FsrMin3(FsrMin3(bL, dL, eL), fL, hL)));
	nz = -0.5 * nz + 1.0;

	// min and max of the ring
	float3 mn4 = min(min(b, d), min(f, h));
	float3 mx4 = max(max(b, d), max(f, h));
	float2 peakC = float2(1.0, -1.0 * 4.0);
	// limiters
	float3 hitMin = min(mn4, e) * (1.0 / (4.0 * mx4));
	float3 hitMax = (peakC.x - max(mx4, e)) * (1.0 / (4.0 * mn4 + peakC.y));
	float3 lobeRGB = max(-hitMin, hitMax);
	float lobe = max(-FSR_RCAS_LIMIT, min(FsrMax3(lobeRGB.r, lobeRGB.g, lobeRGB.b), 0.0)) * sharpness;
	lobe *= nz;

	float rcpL = FsrRcpMedium(4.0 * lobe + 1.0);
	float3 pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
	return float4(pix, 1.0);
}
)";

RendererOutputShader::RendererOutputShader(const std::string& vertex_source, const std::string& fragment_source, bool applyColorTransfer)
{
    std::string finalFragmentSrc;
	switch(g_renderer->GetType())
	{
#ifdef ENABLE_METAL
	case RendererAPI::Metal:
		finalFragmentSrc = fragment_source;
		break;
#endif
#ifdef ENABLE_D3D12
	case RendererAPI::Direct3D12:
		finalFragmentSrc = applyColorTransfer ? WrapFragmentEntryHLSL(fragment_source) : fragment_source;
		break;
#endif
	default:
		finalFragmentSrc = PrependFragmentPreamble(fragment_source);
		break;
	}

	m_vertex_shader.reset(g_renderer->shader_create(RendererShader::ShaderType::kVertex, 0, 0, vertex_source, false, false));
	m_fragment_shader.reset(g_renderer->shader_create(RendererShader::ShaderType::kFragment, 0, 0, finalFragmentSrc, false, false));

	m_vertex_shader->PreponeCompilation(true);
	m_fragment_shader->PreponeCompilation(true);

	if (!m_vertex_shader->WaitForCompiled())
		throw std::exception();

	if(!m_fragment_shader->WaitForCompiled())
		throw std::exception();

}

RendererOutputShader::OutputUniformVariables RendererOutputShader::FillUniformBlockBuffer(const LatteTextureView& texture_view, const Vector2i& output_res, const bool padView) const
{
	OutputUniformVariables vars;

	sint32 effectiveWidth, effectiveHeight;
#if defined(CEMU_UWP)
	const sint32 mipLevel = std::max<sint32>(texture_view.firstMip, 0);
	texture_view.baseTexture->GetEffectiveSize(effectiveWidth, effectiveHeight, mipLevel);
#else
	texture_view.baseTexture->GetEffectiveSize(effectiveWidth, effectiveHeight, 0);
#endif
	vars.textureSrcResolution = {(float)effectiveWidth, (float)effectiveHeight};

#if defined(CEMU_UWP)
	vars.nativeResolution = {
		(float)std::max<sint32>(texture_view.baseTexture->width >> mipLevel, 1),
		(float)std::max<sint32>(texture_view.baseTexture->height >> mipLevel, 1),
	};
#else
	vars.nativeResolution = {(float)texture_view.baseTexture->width, (float)texture_view.baseTexture->height};
#endif
	vars.outputResolution = output_res;
	vars.sharpness = GetFsrSharpness(GetConfig().fsr_sharpness.GetValue());

	FillColorTransfer(vars, UsesDRCColorTransfer(padView));

	return vars;
}

void RendererOutputShader::FillColorTransfer(OutputUniformVariables& vars, bool drcScanBuffer)
{
	vars.applySRGBEncoding = drcScanBuffer ? LatteGPUState.drcBufferUsesSRGB : LatteGPUState.tvBufferUsesSRGB;
	vars.targetGamma = drcScanBuffer ? ActiveSettings::GetDRCGamma() : ActiveSettings::GetTVGamma();
	vars.displayGamma = GetConfig().userDisplayGamma;
}

bool RendererOutputShader::s_mainWindowShowsDRC = false;

void RendererOutputShader::SetMainWindowShowsDRC(bool showsDRC)
{
	s_mainWindowShowsDRC = showsDRC;
}

bool RendererOutputShader::UsesDRCColorTransfer(bool padView)
{
	return padView || s_mainWindowShowsDRC;
}

RendererOutputShader* RendererOutputShader::s_copy_shader;
RendererOutputShader* RendererOutputShader::s_copy_shader_ud;

RendererOutputShader* RendererOutputShader::s_bicubic_shader;
RendererOutputShader* RendererOutputShader::s_bicubic_shader_ud;

RendererOutputShader* RendererOutputShader::s_hermit_shader;
RendererOutputShader* RendererOutputShader::s_hermit_shader_ud;

RendererOutputShader* RendererOutputShader::s_fsr_shader;
RendererOutputShader* RendererOutputShader::s_fsr_shader_ud;
RendererOutputShader* RendererOutputShader::s_fsr_upscale_shader;

float RendererOutputShader::GetFsrSharpness(sint32 sharpnessPercent)
{
	// RCAS takes stops of sharpness reduction: 100% is 0 stops (sharpest), 0% is 2 stops
	const float stops = 2.0f * (1.0f - (float)std::clamp(sharpnessPercent, 0, 100) / 100.0f);
	return std::exp2(-stops);
}

std::string RendererOutputShader::GetOpenGlVertexSource(bool render_upside_down)
{
	// vertex shader
	std::ostringstream vertex_source;
		vertex_source <<
			R"(#version 420
layout(location = 0) smooth out vec2 passUV;

out gl_PerVertex
{
   vec4 gl_Position;
};

void main(){
	vec2 vPos;
	vec2 vUV;
	int vID = gl_VertexID;
)";

		if (render_upside_down)
		{
			vertex_source <<
				R"(	if( vID == 0 ) { vPos = vec2(1.0,1.0); vUV = vec2(1.0,0.0); }
	else if( vID == 1 ) { vPos = vec2(-1.0,1.0); vUV = vec2(0.0,0.0); }
	else if( vID == 2 ) { vPos = vec2(-1.0,-1.0); vUV = vec2(0.0,1.0); }
	else if( vID == 3 ) { vPos = vec2(-1.0,-1.0); vUV = vec2(0.0,1.0); }
	else if( vID == 4 ) { vPos = vec2(1.0,-1.0); vUV = vec2(1.0,1.0); }
	else if( vID == 5 ) { vPos = vec2(1.0,1.0); vUV = vec2(1.0,0.0); }
	)";
		}
		else
		{
			vertex_source <<
				R"(	if( vID == 0 ) { vPos = vec2(1.0,1.0); vUV = vec2(1.0,1.0); }
	else if( vID == 1 ) { vPos = vec2(-1.0,1.0); vUV = vec2(0.0,1.0); }
	else if( vID == 2 ) { vPos = vec2(-1.0,-1.0); vUV = vec2(0.0,0.0); }
	else if( vID == 3 ) { vPos = vec2(-1.0,-1.0); vUV = vec2(0.0,0.0); }
	else if( vID == 4 ) { vPos = vec2(1.0,-1.0); vUV = vec2(1.0,0.0); }
	else if( vID == 5 ) { vPos = vec2(1.0,1.0); vUV = vec2(1.0,1.0); }
	)";
		}

		vertex_source <<
			R"(	passUV = vUV;
	gl_Position = vec4(vPos, 0.0, 1.0);
}
)";
		return vertex_source.str();
}

std::string RendererOutputShader::GetVulkanVertexSource(bool render_upside_down)
{
	// vertex shader
	std::ostringstream vertex_source;
		vertex_source <<
			R"(#version 450
layout(location = 0) out vec2 passUV;

out gl_PerVertex
{
   vec4 gl_Position;
};

void main(){
	vec2 vPos;
	vec2 vUV;
	int vID = gl_VertexIndex;
)";

		if (render_upside_down)
		{
			vertex_source <<
				R"(	if( vID == 0 ) { vPos = vec2(1.0,1.0); vUV = vec2(1.0,0.0); }
	else if( vID == 1 ) { vPos = vec2(-1.0,1.0); vUV = vec2(0.0,0.0); }
	else if( vID == 2 ) { vPos = vec2(-1.0,-1.0); vUV = vec2(0.0,1.0); }
	else if( vID == 3 ) { vPos = vec2(-1.0,-1.0); vUV = vec2(0.0,1.0); }
	else if( vID == 4 ) { vPos = vec2(1.0,-1.0); vUV = vec2(1.0,1.0); }
	else if( vID == 5 ) { vPos = vec2(1.0,1.0); vUV = vec2(1.0,0.0); }
	)";
		}
		else
		{
			vertex_source <<
				R"(	if( vID == 0 ) { vPos = vec2(1.0,1.0); vUV = vec2(1.0,1.0); }
	else if( vID == 1 ) { vPos = vec2(-1.0,1.0); vUV = vec2(0.0,1.0); }
	else if( vID == 2 ) { vPos = vec2(-1.0,-1.0); vUV = vec2(0.0,0.0); }
	else if( vID == 3 ) { vPos = vec2(-1.0,-1.0); vUV = vec2(0.0,0.0); }
	else if( vID == 4 ) { vPos = vec2(1.0,-1.0); vUV = vec2(1.0,0.0); }
	else if( vID == 5 ) { vPos = vec2(1.0,1.0); vUV = vec2(1.0,1.0); }
	)";
		}

		vertex_source <<
			R"(	passUV = vUV;
	gl_Position = vec4(vPos, 0.0, 1.0);
}
)";
		return vertex_source.str();
}

std::string RendererOutputShader::GetMetalVertexSource(bool render_upside_down)
{
	// vertex shader
	std::ostringstream vertex_source;
		vertex_source <<
			R"(#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut main0(ushort vid [[vertex_id]]) {
	VertexOut out;
	float2 pos;
	if (vid == 0) pos = float2(-1.0, -3.0);
	else if (vid == 1) pos = float2(-1.0, 1.0);
	else if (vid == 2) pos = float2(3.0, 1.0);
	out.uv = pos * 0.5 + 0.5;
	out.uv.y = 1.0 - out.uv.y;
)";

		if (render_upside_down)
		{
			vertex_source <<
				R"(	pos.y = -pos.y;
	)";
		}

		vertex_source <<
			R"(	out.position = float4(pos, 0.0, 1.0);
	return out;
}
)";
		return vertex_source.str();
}

std::string RendererOutputShader::GetD3D12VertexSource(bool render_upside_down)
{
	std::ostringstream vertex_source;
	vertex_source <<
		R"(
struct VSOutput
{
	float4 position : SV_Position;
	float2 passUV : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
	uint vID = vertexId % 6;
	float2 vPos;
	float2 vUV;
)";

	if (render_upside_down)
	{
		vertex_source <<
			R"(	if (vID == 0) { vPos = float2( 1.0,  1.0); vUV = float2(1.0, 1.0); }
	else if (vID == 1) { vPos = float2(-1.0,  1.0); vUV = float2(0.0, 1.0); }
	else if (vID == 2) { vPos = float2(-1.0, -1.0); vUV = float2(0.0, 0.0); }
	else if (vID == 3) { vPos = float2(-1.0, -1.0); vUV = float2(0.0, 0.0); }
	else if (vID == 4) { vPos = float2( 1.0, -1.0); vUV = float2(1.0, 0.0); }
	else { vPos = float2( 1.0,  1.0); vUV = float2(1.0, 1.0); }
)";
	}
	else
	{
		vertex_source <<
			R"(	if (vID == 0) { vPos = float2( 1.0,  1.0); vUV = float2(1.0, 0.0); }
	else if (vID == 1) { vPos = float2(-1.0,  1.0); vUV = float2(0.0, 0.0); }
	else if (vID == 2) { vPos = float2(-1.0, -1.0); vUV = float2(0.0, 1.0); }
	else if (vID == 3) { vPos = float2(-1.0, -1.0); vUV = float2(0.0, 1.0); }
	else if (vID == 4) { vPos = float2( 1.0, -1.0); vUV = float2(1.0, 1.0); }
	else { vPos = float2( 1.0,  1.0); vUV = float2(1.0, 0.0); }
)";
	}

	vertex_source << R"(	VSOutput output;
	output.passUV = vUV;
	output.position = float4(vPos, 0.0, 1.0);
	return output;
}
)";
	return vertex_source.str();
}

std::string RendererOutputShader::PrependFragmentPreamble(const std::string& shaderSrc)
{
	return R"(#version 430
layout(location = 0) smooth in vec2 passUV;
layout(binding = 0) uniform sampler2D textureSrc;
layout(location = 0) out vec4 colorOut0;

#ifdef VULKAN
layout (binding = 1, std140)
#else
layout (binding = 0, std140)
#endif
uniform parameters {
uniform vec2 textureSrcResolution;
uniform vec2 nativeResolution;
uniform vec2 outputResolution;
uniform bool applySRGBEncoding;
uniform float targetGamma;
uniform float displayGamma;
};

float sRGBEncode(float linear)
{
	if(linear <= 0.0031308)
		return 12.92f * linear;
	else
		return 1.055f * pow(linear, 1.0f / 2.4f) - 0.055f;

}

vec3 sRGBEncode(vec3 linear)
{
	return vec3(sRGBEncode(linear.r), sRGBEncode(linear.g), sRGBEncode(linear.b));
}

// fwd. declaration
void outputShader();
void main()
{
	outputShader(); // sets colorOut0
	if(applySRGBEncoding)
		colorOut0 = vec4(sRGBEncode(colorOut0.rgb), 1.0f);

	if (displayGamma > 0.0f)
		colorOut0 = pow(colorOut0, vec4(targetGamma / displayGamma) );
	else
		colorOut0 = vec4( sRGBEncode( pow(colorOut0.rgb, vec3(targetGamma)) ), 1.0f);

}

)" + shaderSrc;
}

namespace
{
	bool IsHLSLIdentifierChar(char c)
	{
		return std::isalnum((unsigned char)c) || c == '_';
	}

	// replaces comments with spaces so offsets stay aligned with the original source
	std::string BlankHLSLComments(const std::string& src)
	{
		std::string out = src;
		size_t i = 0;
		while (i + 1 < out.size())
		{
			if (out[i] == '/' && out[i + 1] == '/')
			{
				while (i < out.size() && out[i] != '\n')
					out[i++] = ' ';
			}
			else if (out[i] == '/' && out[i + 1] == '*')
			{
				const size_t end = out.find("*/", i + 2);
				const size_t stop = end == std::string::npos ? out.size() : end + 2;
				for (; i < stop; i++)
				{
					if (out[i] != '\n')
						out[i] = ' ';
				}
			}
			else
				i++;
		}
		return out;
	}

	std::string TrimHLSL(std::string_view s)
	{
		const size_t first = s.find_first_not_of(" \t\r\n");
		if (first == std::string_view::npos)
			return {};
		const size_t last = s.find_last_not_of(" \t\r\n");
		return std::string(s.substr(first, last - first + 1));
	}

	// finds 'float4 main(...)' and returns the offset of 'main' plus the parameter list and argument names
	bool FindHLSLFragmentEntry(const std::string& code, size_t& nameOffset, std::string& parameterList, std::string& argumentList)
	{
		size_t searchPos = 0;
		while ((searchPos = code.find("main", searchPos)) != std::string::npos)
		{
			const size_t nameStart = searchPos;
			const size_t nameEnd = searchPos + 4;
			searchPos = nameEnd;
			if ((nameStart > 0 && IsHLSLIdentifierChar(code[nameStart - 1])) || (nameEnd < code.size() && IsHLSLIdentifierChar(code[nameEnd])))
				continue;
			size_t openParen = nameEnd;
			while (openParen < code.size() && std::isspace((unsigned char)code[openParen]))
				openParen++;
			if (openParen >= code.size() || code[openParen] != '(')
				continue;
			size_t typeEnd = nameStart;
			while (typeEnd > 0 && std::isspace((unsigned char)code[typeEnd - 1]))
				typeEnd--;
			size_t typeStart = typeEnd;
			while (typeStart > 0 && IsHLSLIdentifierChar(code[typeStart - 1]))
				typeStart--;
			if (code.compare(typeStart, typeEnd - typeStart, "float4") != 0)
				return false;
			const size_t closeParen = code.find(')', openParen);
			if (closeParen == std::string::npos)
				return false;
			const size_t bodyStart = code.find_first_of("{;", closeParen);
			if (bodyStart == std::string::npos || code[bodyStart] != '{')
				continue;

			parameterList = code.substr(openParen + 1, closeParen - openParen - 1);
			argumentList.clear();
			size_t paramStart = 0;
			while (paramStart <= parameterList.size())
			{
				size_t paramEnd = parameterList.find(',', paramStart);
				if (paramEnd == std::string::npos)
					paramEnd = parameterList.size();
				std::string_view param(parameterList.data() + paramStart, paramEnd - paramStart);
				param = param.substr(0, std::min(param.find(':'), param.find('=')));
				const size_t bracket = param.find('[');
				if (bracket != std::string_view::npos)
					param = param.substr(0, bracket);
				const std::string trimmed = TrimHLSL(param);
				size_t argStart = trimmed.size();
				while (argStart > 0 && IsHLSLIdentifierChar(trimmed[argStart - 1]))
					argStart--;
				const std::string argName = trimmed.substr(argStart);
				if (!argName.empty() && argName != "void")
				{
					if (argStart == 0)
						return false;
					if (!argumentList.empty())
						argumentList += ", ";
					argumentList += argName;
				}
				paramStart = paramEnd + 1;
			}
			nameOffset = nameStart;
			return true;
		}
		return false;
	}
}

std::string RendererOutputShader::WrapFragmentEntryHLSL(const std::string& shaderSrc)
{
	size_t nameOffset = 0;
	std::string parameterList, argumentList;
	if (!FindHLSLFragmentEntry(BlankHLSLComments(shaderSrc), nameOffset, parameterList, argumentList))
	{
		cemuLog_log(LogType::Force, "D3D12: output shader has no 'float4 main(...)' entry point, sRGB encoding and gamma are not applied");
		return shaderSrc;
	}
	std::string wrapped = shaderSrc;
	wrapped.replace(nameOffset, 4, "cemuOutputShaderMain");
	// same packing as OutputUniformVariables, bound to the same constant buffer as b0
	wrapped += R"(

cbuffer CemuOutputColorTransfer : register(b0, space1)
{
	uint cemuOutputApplySRGBEncoding : packoffset(c1.z);
	float cemuOutputTargetGamma : packoffset(c1.w);
	float cemuOutputDisplayGamma : packoffset(c2.x);
};

float3 cemuOutputSRGBEncode(float3 linearColor)
{
	return select(linearColor <= 0.0031308, 12.92 * linearColor, 1.055 * pow(linearColor, 1.0 / 2.4) - 0.055);
}

float4 cemuOutputColorTransfer(float4 color)
{
	if (cemuOutputApplySRGBEncoding != 0)
		color = float4(cemuOutputSRGBEncode(color.rgb), 1.0);
	if (cemuOutputDisplayGamma > 0.0)
		color = pow(max(color, 0.0), cemuOutputTargetGamma / cemuOutputDisplayGamma);
	else
		color = float4(cemuOutputSRGBEncode(pow(max(color.rgb, 0.0), cemuOutputTargetGamma)), 1.0);
	return color;
}

float4 main()" + parameterList + R"() : SV_Target0
{
	return cemuOutputColorTransfer(cemuOutputShaderMain()" + argumentList + R"());
}
)";
	return wrapped;
}

void RendererOutputShader::InitializeStatic()
{
	switch(g_renderer->GetType())
	{
#ifdef ENABLE_METAL
    case RendererAPI::Metal:
    {
        std::string vertex_source = GetMetalVertexSource(false);
        std::string vertex_source_ud = GetMetalVertexSource(true);

       	s_copy_shader = new RendererOutputShader(vertex_source, s_copy_shader_source_mtl);
       	s_copy_shader_ud = new RendererOutputShader(vertex_source_ud, s_copy_shader_source_mtl);

       	s_bicubic_shader = new RendererOutputShader(vertex_source, s_bicubic_shader_source_mtl);
       	s_bicubic_shader_ud = new RendererOutputShader(vertex_source_ud, s_bicubic_shader_source_mtl);

       	s_hermit_shader = new RendererOutputShader(vertex_source, s_hermite_shader_source_mtl);
       	s_hermit_shader_ud = new RendererOutputShader(vertex_source_ud, s_hermite_shader_source_mtl);
		break;
    }
#endif
#ifdef ENABLE_OPENGL
    case RendererAPI::OpenGL:
    {
    	std::string vertex_source, vertex_source_ud;
    	// vertex shader
		vertex_source = GetOpenGlVertexSource(false);
		vertex_source_ud = GetOpenGlVertexSource(true);

    	s_copy_shader = new RendererOutputShader(vertex_source, s_copy_shader_source);
    	s_copy_shader_ud = new RendererOutputShader(vertex_source_ud, s_copy_shader_source);

    	s_bicubic_shader = new RendererOutputShader(vertex_source, s_bicubic_shader_source);
    	s_bicubic_shader_ud = new RendererOutputShader(vertex_source_ud, s_bicubic_shader_source);

    	s_hermit_shader = new RendererOutputShader(vertex_source, s_hermite_shader_source);
    	s_hermit_shader_ud = new RendererOutputShader(vertex_source_ud, s_hermite_shader_source);
		break;
    }
#endif
#ifdef ENABLE_VULKAN
    case RendererAPI::Vulkan:
    {
    	std::string vertex_source, vertex_source_ud;
    	// vertex shader
		vertex_source = GetVulkanVertexSource(false);
		vertex_source_ud = GetVulkanVertexSource(true);
			
    	s_copy_shader = new RendererOutputShader(vertex_source, s_copy_shader_source);
    	s_copy_shader_ud = new RendererOutputShader(vertex_source_ud, s_copy_shader_source);

    	s_bicubic_shader = new RendererOutputShader(vertex_source, s_bicubic_shader_source);
    	s_bicubic_shader_ud = new RendererOutputShader(vertex_source_ud, s_bicubic_shader_source);

    	s_hermit_shader = new RendererOutputShader(vertex_source, s_hermite_shader_source);
    	s_hermit_shader_ud = new RendererOutputShader(vertex_source_ud, s_hermite_shader_source);
		break;
    }
#endif
#ifdef ENABLE_D3D12
	case RendererAPI::Direct3D12:
	{
		std::string vertex_source, vertex_source_ud;
		vertex_source = GetD3D12VertexSource(false);
		vertex_source_ud = GetD3D12VertexSource(true);

		s_copy_shader = new RendererOutputShader(vertex_source, s_copy_shader_source_hlsl);
		s_copy_shader_ud = new RendererOutputShader(vertex_source_ud, s_copy_shader_source_hlsl);

		s_bicubic_shader = new RendererOutputShader(vertex_source, s_bicubic_shader_source_hlsl);
		s_bicubic_shader_ud = new RendererOutputShader(vertex_source_ud, s_bicubic_shader_source_hlsl);

		s_hermit_shader = new RendererOutputShader(vertex_source, s_hermite_shader_source_hlsl);
		s_hermit_shader_ud = new RendererOutputShader(vertex_source_ud, s_hermite_shader_source_hlsl);

		// the upscale pass always draws upright into its own target; the sharpening pass handles upside down
		try
		{
			s_fsr_upscale_shader = new RendererOutputShader(vertex_source, s_fsr_easu_shader_source_hlsl);
			s_fsr_shader = new RendererOutputShader(vertex_source, s_fsr_rcas_shader_source_hlsl, false);
			s_fsr_shader_ud = new RendererOutputShader(vertex_source_ud, s_fsr_rcas_shader_source_hlsl, false);
			s_fsr_shader->m_prePass = s_fsr_upscale_shader;
			s_fsr_shader_ud->m_prePass = s_fsr_upscale_shader;
		}
		catch (const std::exception&)
		{
			// FSR is optional: without its shaders the filter falls back to bilinear
			cemuLog_log(LogType::Force, "D3D12: the FSR output shaders failed to compile; the FSR filter uses bilinear instead.");
			delete s_fsr_upscale_shader;
			delete s_fsr_shader;
			delete s_fsr_shader_ud;
			s_fsr_upscale_shader = nullptr;
			s_fsr_shader = nullptr;
			s_fsr_shader_ud = nullptr;
		}
		break;
	}
#endif
	}
}

void RendererOutputShader::ShutdownStatic()
{
	delete s_copy_shader;
	delete s_copy_shader_ud;

	delete s_bicubic_shader;
	delete s_bicubic_shader_ud;

	delete s_hermit_shader;
	delete s_hermit_shader_ud;

	delete s_fsr_shader;
	delete s_fsr_shader_ud;
	delete s_fsr_upscale_shader;
	s_fsr_shader = nullptr;
	s_fsr_shader_ud = nullptr;
	s_fsr_upscale_shader = nullptr;
}
