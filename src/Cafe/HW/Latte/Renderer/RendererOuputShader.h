#pragma once

#include "Cafe/HW/Latte/Renderer/RendererShader.h"
#include "util/math/vector2.h"

#include "Cafe/HW/Latte/Core/LatteTexture.h"

class RendererOutputShader
{
public:
	struct OutputUniformVariables
	{
		Vector2f textureSrcResolution;
		Vector2f nativeResolution;
		Vector2f outputResolution;
		uint32 applySRGBEncoding;
		float targetGamma;
		float displayGamma;
		// RCAS sharpening amount, exp2(-stops)
		float sharpness;
	};
	enum Shader
	{
		kCopy,
		kBicubic,
		kHermit,
	};
	// without applyColorTransfer the shader writes its result unchanged, for a pass whose input already went through it
	RendererOutputShader(const std::string& vertex_source, const std::string& fragment_source, bool applyColorTransfer = true);
	virtual ~RendererOutputShader() = default;

	// a shader with a pre-pass reads the pre-pass result, drawn at the output size, instead of the game image
	RendererOutputShader* GetPrePass() const { return m_prePass; }

	OutputUniformVariables FillUniformBlockBuffer(const LatteTextureView& texture_view, const Vector2i& output_res, const bool padView) const;
	static void FillColorTransfer(OutputUniformVariables& vars, bool drcScanBuffer);

	// the main window presents the GamePad scan buffer, so its sRGB flag and gamma apply
	static void SetMainWindowShowsDRC(bool showsDRC);
	static bool UsesDRCColorTransfer(bool padView);

	RendererShader* GetVertexShader() const
	{
		return m_vertex_shader.get();
	}

	RendererShader* GetFragmentShader() const
	{
		return m_fragment_shader.get();
	}

	static void InitializeStatic();
	static void ShutdownStatic();

	static RendererOutputShader* s_copy_shader;
	static RendererOutputShader* s_copy_shader_ud;

	static RendererOutputShader* s_bicubic_shader;
	static RendererOutputShader* s_bicubic_shader_ud;

	static RendererOutputShader* s_hermit_shader;
	static RendererOutputShader* s_hermit_shader_ud;

	// AMD FidelityFX Super Resolution 1.0: EASU upscaling pre-pass, then RCAS sharpening (Direct3D 12 only)
	static RendererOutputShader* s_fsr_shader;
	static RendererOutputShader* s_fsr_shader_ud;
	static RendererOutputShader* s_fsr_upscale_shader;
	// percent from the settings, 100 sharpest
	static float GetFsrSharpness(sint32 sharpnessPercent);

	static std::string GetOpenGlVertexSource(bool render_upside_down);
	static std::string GetVulkanVertexSource(bool render_upside_down);
	static std::string GetMetalVertexSource(bool render_upside_down);
	static std::string GetD3D12VertexSource(bool render_upside_down);

	static std::string PrependFragmentPreamble(const std::string& shaderSrc);
	static std::string WrapFragmentEntryHLSL(const std::string& shaderSrc);

protected:
	std::unique_ptr<RendererShader> m_vertex_shader;
	std::unique_ptr<RendererShader> m_fragment_shader;
	RendererOutputShader* m_prePass{};


private:
	static const std::string s_copy_shader_source;
	static const std::string s_bicubic_shader_source;
	static const std::string s_hermite_shader_source;

	static const std::string s_bicubic_shader_source_vk;
	static const std::string s_hermite_shader_source_vk;

	static const std::string s_copy_shader_source_mtl;
	static const std::string s_bicubic_shader_source_mtl;
	static const std::string s_hermite_shader_source_mtl;

	static const std::string s_copy_shader_source_hlsl;
	static const std::string s_bicubic_shader_source_hlsl;
	static const std::string s_hermite_shader_source_hlsl;
	static const std::string s_fsr_easu_shader_source_hlsl;
	static const std::string s_fsr_rcas_shader_source_hlsl;

	static bool s_mainWindowShowsDRC;
};
