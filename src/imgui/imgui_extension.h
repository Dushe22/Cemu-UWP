#pragma once
#include "imgui.h"
#if defined(CEMU_UWP)
#include <cstdint>
#include <filesystem>
#include <vector>
#endif

void ImRotateStart();
ImVec2 ImRotationCenter();
void ImRotateEnd(float rad, ImVec2 center = ImRotationCenter());

inline ImVec2 operator-(const ImVec2& l, const ImVec2& r) { return{ l.x - r.x, l.y - r.y }; }
inline bool operator<(const ImVec2& lhs, const ImVec2& rhs) { return lhs.x < rhs.x; }
inline bool operator>=(const ImVec2& lhs, const ImVec2& rhs) { return lhs.x >= rhs.x; }

bool ImGui_BeginPadDistinct(const char* name, bool* p_open, ImGuiWindowFlags flags, bool pad);

void ImGui_PrecacheFonts();
void ImGui_ClearFonts();
ImFont* ImGui_GetFont(float size);
#if defined(CEMU_UWP)
// merges a script font for the codepoints the built-in font does not provide
void ImGui_SetTextGlyphs(const std::vector<std::uint32_t>& codepoints,
	const std::filesystem::path& scriptFont);
#endif
void ImGui_UpdateWindowInformation(bool mainWindow);

#if defined(CEMU_UWP)
void ImGui_UwpSetGamepadNavigationEnabled(bool enabled);
#endif
