#pragma once

#include <cstdint>
#include <string>

#if defined(CEMU_UWP)
inline constexpr uint32_t LATTE_DECOMPILER_HLSL_EMITTER_VERSION = 39;
#else
inline constexpr uint32_t LATTE_DECOMPILER_HLSL_EMITTER_VERSION = 30;
#endif

struct LatteDecompilerShader;

// the sampler bound to this unit must use comparison filtering exactly when this returns true
bool LatteDecompiler_HLSLTextureUsesComparisonSampler(const LatteDecompilerShader* shader, int32_t textureUnit);
// geometry shaders expand point output to quads exactly when this returns true; those draws need point raster state
bool LatteDecompiler_HLSLGeometryShaderExpandsPoints(const uint32_t* contextRegisters);
// builds HLSL for a GLSL replacement pixel shader from the HLSL generated for the original shader
bool LatteDecompiler_ConvertGLSLPixelShaderToHLSL(const std::string& generatedHLSL, const std::string& glslSource, std::string& hlslOut);
