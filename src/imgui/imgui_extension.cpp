#include "imgui_extension.h"
#include "WindowSystem.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "resource/IconsFontAwesome5.h"
#include "resource/resource.h"
#ifdef ENABLE_OPENGL
#include "imgui_impl_opengl3.h"
#endif
#ifdef ENABLE_VULKAN
#include "imgui_impl_vulkan.h"
#endif
#include "input/InputManager.h"
#if defined(CEMU_UWP)
#include "Common/FileStream.h"
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.System.h>
#include <atomic>

namespace
{
std::atomic_bool s_uwpGamepadNavigationEnabled{ false };
std::atomic_bool s_uwpGamepadNavigationNeedsNeutral{ false };
}

void ImGui_UwpSetGamepadNavigationEnabled(bool enabled)
{
	const bool wasEnabled = s_uwpGamepadNavigationEnabled.load(std::memory_order_acquire);
	if (enabled)
	{
		if (!wasEnabled)
			s_uwpGamepadNavigationNeedsNeutral.store(true, std::memory_order_release);
		s_uwpGamepadNavigationEnabled.store(true, std::memory_order_release);
	}
	else
	{
		s_uwpGamepadNavigationEnabled.store(false, std::memory_order_release);
		s_uwpGamepadNavigationNeedsNeutral.store(false, std::memory_order_release);
	}
}
#endif

// <imgui_internal.h>
template<typename T> static T ImMin(T lhs, T rhs) { return lhs < rhs ? lhs : rhs; }
template<typename T> static T ImMax(T lhs, T rhs) { return lhs >= rhs ? lhs : rhs; }
static ImVec2 ImRotate(const ImVec2& v, float cos_a, float sin_a) { return ImVec2(v.x * cos_a - v.y * sin_a, v.x * sin_a + v.y * cos_a); }

int rotation_start_index;
void ImRotateStart()
{
	rotation_start_index = ImGui::GetWindowDrawList()->VtxBuffer.Size;
}

ImVec2 ImRotationCenter()
{
	ImVec2 l(FLT_MAX, FLT_MAX), u(-FLT_MAX, -FLT_MAX); // bounds

	const auto& buf = ImGui::GetWindowDrawList()->VtxBuffer;
	for (int i = rotation_start_index; i < buf.Size; i++)
		l = ImMin(l, buf[i].pos), u = ImMax(u, buf[i].pos);

	return ImVec2((l.x + u.x) / 2, (l.y + u.y) / 2); // or use _ClipRectStack?
}

void ImRotateEnd(float rad, ImVec2 center)
{
	const float s = sin(rad);
	const float c = cos(rad);
	center = ImRotate(center, s, c) - center;

	auto& buf = ImGui::GetWindowDrawList()->VtxBuffer;
	for (int i = rotation_start_index; i < buf.Size; i++)
		buf[i].pos = ImRotate(buf[i].pos, s, c) - center;
}

uint8* extractCafeDefaultFont(sint32* size);
sint32 g_font_size = 0;
uint8* g_font_data = nullptr;
#if !BOOST_OS_WINDOWS
extern int const g_fontawesome_size;
extern char const g_fontawesome_data[];
#endif
std::unordered_map<int, ImFont*> g_imgui_fonts;
std::stack<int> g_font_requests;

#if defined(CEMU_UWP)
std::unordered_set<int> g_font_requests_queued;
std::vector<ImWchar> g_uwpScriptGlyphRanges;
std::vector<uint8> g_uwpScriptFontData;
bool g_uwpFontRebuildPending = false;

static constexpr ImWchar kUwpTextGlyphRanges[] = {
	0x0020, 0x04FF, // Latin, IPA, combining marks, Greek, and Cyrillic
	0x0E3F, 0x0E3F, // Baht currency symbol
	0x1D00, 0x1EFF, // Phonetic Extensions and Latin Extended Additional
	0x2000, 0x27FF, // Punctuation, currency, letterlike, arrows, math, and symbols
	0x2C60, 0x2C7F, // Latin Extended-C
	0xA700, 0xA7FF, // Modifier Tone Letters and Latin Extended-D
	0xFB00, 0xFB04, // Latin presentation-form ligatures
	0xFE20, 0xFE23, // Combining half marks
	0,
};

void ImGui_SetTextGlyphs(const std::vector<std::uint32_t>& codepoints,
	const std::filesystem::path& scriptFont)
{
	ImFontGlyphRangesBuilder baseGlyphs;
	baseGlyphs.AddRanges(kUwpTextGlyphRanges);

	ImFontGlyphRangesBuilder scriptGlyphs;
	bool hasScriptGlyphs = false;
	for (std::uint32_t codepoint : codepoints)
	{
		if (codepoint > IM_UNICODE_CODEPOINT_MAX || baseGlyphs.GetBit(codepoint))
			continue;
		scriptGlyphs.AddChar((ImWchar)codepoint);
		hasScriptGlyphs = true;
	}

	g_uwpScriptGlyphRanges.clear();
	g_uwpScriptFontData.clear();
	if (hasScriptGlyphs && !scriptFont.empty())
	{
		ImVector<ImWchar> ranges;
		scriptGlyphs.BuildRanges(&ranges);
		g_uwpScriptGlyphRanges.assign(ranges.begin(), ranges.end());
		if (std::unique_ptr<FileStream> file{ FileStream::openFile2(scriptFont) })
			file->extract(g_uwpScriptFontData);
		else
			cemuLog_log(LogType::Force, "Interface font {} is missing", _pathToUtf8(scriptFont));
	}
	g_uwpFontRebuildPending = true;
}
#endif

void ImGui_PrecacheFonts()
{
#if defined(CEMU_UWP)
	bool fontsAdded = false;
	ImFontAtlas* atlas = ImGui::GetIO().Fonts;
	if (g_uwpFontRebuildPending && !atlas->Locked)
	{
		g_uwpFontRebuildPending = false;
		std::vector<int> cachedSizes;
		cachedSizes.reserve(g_imgui_fonts.size());
		for (const auto& [size, font] : g_imgui_fonts)
			cachedSizes.push_back(size);
		atlas->Clear();
		g_imgui_fonts.clear();
		for (int size : cachedSizes)
		{
			if (g_font_requests_queued.emplace(size).second)
				g_font_requests.emplace(size);
		}
	}
#endif
	while (!g_font_requests.empty())
	{
		const int request = g_font_requests.top();
		g_font_requests.pop();
		const int size = request;

#if defined(CEMU_UWP)
		g_font_requests_queued.erase(request);
		if (g_imgui_fonts.contains(size))
			continue;
#endif

		auto& io = ImGui::GetIO();
		cemu_assert(io.Fonts->Locked == false);

		ImFont* font = nullptr;
		if (!font)
		{
			if (g_font_size == 0)
				g_font_data = extractCafeDefaultFont(&g_font_size);

			ImFontConfig cfg{};
			cfg.FontDataOwnedByAtlas = false;
			//cfg.FontData = g_font_data;
			//cfg.FontDataSize = g_font_size;
			//cfg.SizePixels = size;
#if defined(CEMU_UWP)
			font = io.Fonts->AddFontFromMemoryTTF(g_font_data, g_font_size, (float)size, &cfg,
				kUwpTextGlyphRanges);
			if (!g_uwpScriptFontData.empty())
			{
				ImFontConfig scriptCfg{};
				scriptCfg.FontDataOwnedByAtlas = false;
				scriptCfg.MergeMode = true;
				io.Fonts->AddFontFromMemoryTTF(g_uwpScriptFontData.data(),
					(int)g_uwpScriptFontData.size(), (float)size, &scriptCfg,
					g_uwpScriptGlyphRanges.data());
			}
#else
			font = io.Fonts->AddFontFromMemoryTTF(g_font_data, g_font_size, (float)size, &cfg);
#endif
		}

		ImFontConfig cfgmerge{};
		cfgmerge.FontDataOwnedByAtlas = false;
		cfgmerge.MergeMode = true;
		cfgmerge.GlyphMinAdvanceX = 20.0f;
		//cfgmerge.GlyphOffset = { 2,2 };

		static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };

#if BOOST_OS_WINDOWS && !defined(CEMU_UWP)
		const auto hinstance = GetModuleHandle(nullptr);
		const HRSRC res = FindResource(hinstance, MAKEINTRESOURCE(IDR_FONTAWESOME), RT_RCDATA);
		if (res)
		{
			const HGLOBAL mem = ::LoadResource(hinstance, res);
			if (mem)
			{
				void* data = LockResource(mem);
				const size_t len = SizeofResource(hinstance, res);

				io.Fonts->AddFontFromMemoryTTF(data, (int)len, (float)size, &cfgmerge, icon_ranges);
			}
		}
#elif !defined(CEMU_UWP)
		io.Fonts->AddFontFromMemoryTTF((void*)g_fontawesome_data, (int)g_fontawesome_size, (float)size, &cfgmerge, icon_ranges);
#endif

		g_imgui_fonts[size] = font;
		#if defined(CEMU_UWP)
		fontsAdded = true;
		#else
		// Vulkan doesn't let us destroy resources that are still being used, so we flush here
		g_renderer->Flush(true);
		g_renderer->DeleteFontTextures();
		#endif
	}
#if defined(CEMU_UWP)
	if (fontsAdded)
	{
		// both ImGui contexts share the atlas, invalidate its textures once
		g_renderer->Flush(true);
		g_renderer->DeleteFontTextures();
	}
#endif
}

void ImGui_ClearFonts()
{
	g_imgui_fonts.clear();
#if defined(CEMU_UWP)
	g_font_requests_queued.clear();
	while (!g_font_requests.empty())
		g_font_requests.pop();
#endif
}

ImFont* ImGui_GetFont(float size)
{
	const int fontSize = std::max(1, (int)size);
	const auto it = g_imgui_fonts.find(fontSize);
	if (it != g_imgui_fonts.cend())
		return it->second;

#if defined(CEMU_UWP)
	if (g_font_requests_queued.emplace(fontSize).second)
		g_font_requests.emplace(fontSize);
#else
	g_font_requests.emplace(fontSize);
#endif
	return nullptr; // will create the font in next precache call
}

void ImGui_UpdateWindowInformation(bool mainWindow)
{
	static std::map<uint32, ImGuiKey> keyboard_mapping;
	auto& windowInfo = WindowSystem::GetWindowInfo();
	static uint32 current_key = 0;
	ImGuiIO& io = ImGui::GetIO();
	io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
#if BOOST_OS_WINDOWS && !defined(CEMU_UWP)
	io.ImeWindowHandle = mainWindow ? windowInfo.window_main.surface : windowInfo.window_pad.surface;
#else
	io.ImeWindowHandle = nullptr;
#endif

	io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);

	auto& instance = InputManager::instance();

	const auto mousePos = instance.get_mouse_position(!mainWindow);
	io.MousePos = { (float)mousePos.x, (float)mousePos.y };

	bool padDown;
	const auto pos = instance.get_left_down_mouse_info(&padDown);
	io.MouseDown[0] = padDown != mainWindow && pos.has_value();
	auto get_mapping = [&](uint32 key_code)
	{
#if defined(CEMU_UWP)
		using winrt::Windows::System::VirtualKey;
		switch (static_cast<VirtualKey>(key_code))
		{
		case VirtualKey::Enter:
			return ImGuiKey_Enter;
		case VirtualKey::Space:
			return ImGuiKey_Space;
		case VirtualKey::Escape:
			return ImGuiKey_Escape;
		case VirtualKey::Tab:
			return ImGuiKey_Tab;
		case VirtualKey::Back:
			return ImGuiKey_Backspace;
		case VirtualKey::Delete:
			return ImGuiKey_Delete;
		case VirtualKey::Home:
			return ImGuiKey_Home;
		case VirtualKey::End:
			return ImGuiKey_End;
		case VirtualKey::PageUp:
			return ImGuiKey_PageUp;
		case VirtualKey::PageDown:
			return ImGuiKey_PageDown;
		case VirtualKey::Left:
			return ImGuiKey_LeftArrow;
		case VirtualKey::Right:
			return ImGuiKey_RightArrow;
		case VirtualKey::Up:
			return ImGuiKey_UpArrow;
		case VirtualKey::Down:
			return ImGuiKey_DownArrow;
		case VirtualKey::GamepadA:
			return ImGuiKey_GamepadFaceDown;
		case VirtualKey::GamepadB:
			return ImGuiKey_GamepadFaceRight;
		case VirtualKey::GamepadY:
			return ImGuiKey_GamepadFaceUp;
		case VirtualKey::GamepadMenu:
			return ImGuiKey_GamepadStart;
		case VirtualKey::GamepadView:
			return ImGuiKey_GamepadBack;
		case VirtualKey::GamepadLeftShoulder:
			return ImGuiKey_GamepadL1;
		case VirtualKey::GamepadRightShoulder:
			return ImGuiKey_GamepadR1;
		case VirtualKey::GamepadDPadLeft:
			return ImGuiKey_GamepadDpadLeft;
		case VirtualKey::GamepadDPadRight:
			return ImGuiKey_GamepadDpadRight;
		case VirtualKey::GamepadDPadUp:
			return ImGuiKey_GamepadDpadUp;
		case VirtualKey::GamepadDPadDown:
			return ImGuiKey_GamepadDpadDown;
		case VirtualKey::GamepadLeftThumbstickLeft:
			return ImGuiKey_GamepadLStickLeft;
		case VirtualKey::GamepadLeftThumbstickRight:
			return ImGuiKey_GamepadLStickRight;
		case VirtualKey::GamepadLeftThumbstickUp:
			return ImGuiKey_GamepadLStickUp;
		case VirtualKey::GamepadLeftThumbstickDown:
			return ImGuiKey_GamepadLStickDown;
		default:
			break;
		}
#endif
		auto key = keyboard_mapping.find(key_code);
		if (key != keyboard_mapping.end())
			return key->second;
		ImGuiKey mapped_key = (ImGuiKey)((uint32)current_key + ImGuiKey_NamedKey_BEGIN);
		current_key = (current_key + 1) % (uint32)ImGuiKey_NamedKey_COUNT;
		keyboard_mapping[key_code] = mapped_key;
		return mapped_key;
	};
#if defined(CEMU_UWP)
	const auto gamepads = winrt::Windows::Gaming::Input::Gamepad::Gamepads();
	const bool hasDirectGamepad = gamepads.Size() > 0;
	const bool uwpGamepadNavigationEnabled = s_uwpGamepadNavigationEnabled.load(std::memory_order_acquire);
	const bool uwpGamepadNavigationNeedsNeutral = s_uwpGamepadNavigationNeedsNeutral.load(std::memory_order_acquire);
	const auto isGamepadVirtualKey = [](uint32 keyCode)
	{
		using winrt::Windows::System::VirtualKey;
		switch (static_cast<VirtualKey>(keyCode))
		{
		case VirtualKey::GamepadA:
		case VirtualKey::GamepadB:
		case VirtualKey::GamepadX:
		case VirtualKey::GamepadY:
		case VirtualKey::GamepadMenu:
		case VirtualKey::GamepadView:
		case VirtualKey::GamepadLeftShoulder:
		case VirtualKey::GamepadRightShoulder:
		case VirtualKey::GamepadDPadLeft:
		case VirtualKey::GamepadDPadRight:
		case VirtualKey::GamepadDPadUp:
		case VirtualKey::GamepadDPadDown:
		case VirtualKey::GamepadLeftThumbstickLeft:
		case VirtualKey::GamepadLeftThumbstickRight:
		case VirtualKey::GamepadLeftThumbstickUp:
		case VirtualKey::GamepadLeftThumbstickDown:
			return true;
		default:
			return false;
		}
	};
#endif
	windowInfo.iter_keystates([&](auto&& el)
	{
#if defined(CEMU_UWP)
		// X is reserved by the frontend, ImGui would map FaceLeft to its window switcher
		if (static_cast<winrt::Windows::System::VirtualKey>(el.first) ==
			winrt::Windows::System::VirtualKey::GamepadX)
		{
			return;
		}
		// CoreWindow mirrors WGI buttons as virtual keys, feed ImGui only one stream
		if ((hasDirectGamepad || !uwpGamepadNavigationEnabled || uwpGamepadNavigationNeedsNeutral) && isGamepadVirtualKey(el.first))
			return;
#endif
		io.AddKeyEvent(get_mapping(el.first), el.second);
	});

	// printf("%f %f %d\n", io.MousePos.x, io.MousePos.y, io.MouseDown[0]);

#if defined(CEMU_UWP)
	const auto releaseGamepadNavigation = [&io]() {
		io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
		io.AddKeyEvent(ImGuiKey_GamepadStart, false);
		io.AddKeyEvent(ImGuiKey_GamepadBack, false);
		io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
		io.AddKeyEvent(ImGuiKey_GamepadFaceRight, false);
		io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, false);
		io.AddKeyEvent(ImGuiKey_GamepadFaceUp, false);
		io.AddKeyEvent(ImGuiKey_GamepadL1, false);
		io.AddKeyEvent(ImGuiKey_GamepadR1, false);
		io.AddKeyAnalogEvent(ImGuiKey_GamepadDpadLeft, false, 0.0f);
		io.AddKeyAnalogEvent(ImGuiKey_GamepadDpadRight, false, 0.0f);
		io.AddKeyAnalogEvent(ImGuiKey_GamepadDpadUp, false, 0.0f);
		io.AddKeyAnalogEvent(ImGuiKey_GamepadDpadDown, false, 0.0f);
		io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, false, 0.0f);
		io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, false, 0.0f);
		io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, false, 0.0f);
		io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, false, 0.0f);
	};

	if (hasDirectGamepad)
	{
		using winrt::Windows::Gaming::Input::GamepadButtons;
		bool menu = false;
		bool view = false;
		bool faceDown = false;
		bool faceRight = false;
		bool faceLeft = false;
		bool faceUp = false;
		bool leftShoulder = false;
		bool rightShoulder = false;
		bool dpadLeft = false;
		bool dpadRight = false;
		bool dpadUp = false;
		bool dpadDown = false;
		float stickLeft = 0.0f;
		float stickRight = 0.0f;
		float stickUp = 0.0f;
		float stickDown = 0.0f;
		const auto maxAnalog = [](float a, float b) { return a > b ? a : b; };

		for (uint32 i = 0; i < gamepads.Size(); ++i)
		{
			const auto reading = gamepads.GetAt(i).GetCurrentReading();
			const auto down = [&](GamepadButtons button) {
				return (static_cast<uint32>(reading.Buttons) & static_cast<uint32>(button)) != 0;
			};
			menu |= down(GamepadButtons::Menu);
			view |= down(GamepadButtons::View);
			faceDown |= down(GamepadButtons::A);
			faceRight |= down(GamepadButtons::B);
			faceLeft |= down(GamepadButtons::X);
			faceUp |= down(GamepadButtons::Y);
			leftShoulder |= down(GamepadButtons::LeftShoulder);
			rightShoulder |= down(GamepadButtons::RightShoulder);
			dpadLeft |= down(GamepadButtons::DPadLeft);
			dpadRight |= down(GamepadButtons::DPadRight);
			dpadUp |= down(GamepadButtons::DPadUp);
			dpadDown |= down(GamepadButtons::DPadDown);
			stickLeft = maxAnalog(stickLeft, reading.LeftThumbstickX < -0.45 ? (float)-reading.LeftThumbstickX : 0.0f);
			stickRight = maxAnalog(stickRight, reading.LeftThumbstickX > 0.45 ? (float)reading.LeftThumbstickX : 0.0f);
			stickUp = maxAnalog(stickUp, reading.LeftThumbstickY > 0.45 ? (float)reading.LeftThumbstickY : 0.0f);
			stickDown = maxAnalog(stickDown, reading.LeftThumbstickY < -0.45 ? (float)-reading.LeftThumbstickY : 0.0f);
		}

		const bool anyNavigationDown = menu || view || faceDown || faceRight || faceLeft || faceUp ||
			leftShoulder || rightShoulder || dpadLeft || dpadRight || dpadUp || dpadDown ||
			stickLeft > 0.0f || stickRight > 0.0f || stickUp > 0.0f || stickDown > 0.0f;
		if (!uwpGamepadNavigationEnabled || (uwpGamepadNavigationNeedsNeutral && anyNavigationDown))
		{
			releaseGamepadNavigation();
			return;
		}
		s_uwpGamepadNavigationNeedsNeutral.store(false, std::memory_order_release);
		io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

		io.AddKeyEvent(ImGuiKey_GamepadStart, menu);
		io.AddKeyEvent(ImGuiKey_GamepadBack, view);
		io.AddKeyEvent(ImGuiKey_GamepadFaceDown, faceDown);
		io.AddKeyEvent(ImGuiKey_GamepadFaceRight, faceRight);
		io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, false);
		io.AddKeyEvent(ImGuiKey_GamepadFaceUp, faceUp);
		io.AddKeyEvent(ImGuiKey_GamepadL1, leftShoulder);
		io.AddKeyEvent(ImGuiKey_GamepadR1, rightShoulder);
		io.AddKeyAnalogEvent(ImGuiKey_GamepadDpadLeft, dpadLeft, dpadLeft ? 1.0f : 0.0f);
		io.AddKeyAnalogEvent(ImGuiKey_GamepadDpadRight, dpadRight, dpadRight ? 1.0f : 0.0f);
		io.AddKeyAnalogEvent(ImGuiKey_GamepadDpadUp, dpadUp, dpadUp ? 1.0f : 0.0f);
		io.AddKeyAnalogEvent(ImGuiKey_GamepadDpadDown, dpadDown, dpadDown ? 1.0f : 0.0f);
		io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, stickLeft > 0.0f, stickLeft);
		io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, stickRight > 0.0f, stickRight);
		io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, stickUp > 0.0f, stickUp);
		io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, stickDown > 0.0f, stickDown);

		return;
	}
	if (!uwpGamepadNavigationEnabled)
	{
		releaseGamepadNavigation();
		return;
	}
	s_uwpGamepadNavigationNeedsNeutral.store(false, std::memory_order_release);
	io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
#endif
	for (auto i = 0; i < InputManager::kMaxController; ++i)
	{
		const auto controller = instance.get_controller(i);
		if (!controller)
			continue;

		if (controller->is_start_down())
			io.NavInputs[ImGuiNavInput_Input] = 1.0f;

		if (controller->is_a_down())
			io.NavInputs[ImGuiNavInput_Activate] = 1.0f;
		
		if (controller->is_b_down())
			io.NavInputs[ImGuiNavInput_Cancel] = 1.0f;
		
		if (controller->is_left_down())
			io.NavInputs[ImGuiNavInput_DpadLeft] = 1.0f;
		
		if (controller->is_right_down())
			io.NavInputs[ImGuiNavInput_DpadRight] = 1.0f;
		
		if (controller->is_up_down())
			io.NavInputs[ImGuiNavInput_DpadUp] = 1.0f;
		
		if (controller->is_down_down())
			io.NavInputs[ImGuiNavInput_DpadDown] = 1.0f;
	}
}
