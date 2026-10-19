#include "UwpImGuiFrontend/InputMapper.h"

#include "UwpImGuiFrontend/Text.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <tuple>
#include <unordered_map>

namespace UwpImGuiFrontend
{
namespace
{
double SteadySeconds()
{
	return std::chrono::duration<double>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool IsActive(const InputCaptureSample& sample, const InputCaptureSample* baseline)
{
	const float origin = baseline ? baseline->value : 0.0f;
	const float difference = std::abs(sample.value - origin);
	if (sample.kind == InputControlKind::Button || sample.kind == InputControlKind::Touch)
		return sample.value >= 0.5f;
	if (sample.kind == InputControlKind::Trigger)
		return difference >= 0.22f;
	return difference >= 0.35f;
}

bool HasControllerUiInput(const FrameInput& input)
{
	return input.controllerInputHeld || input.up || input.down || input.left ||
		input.right || input.accept || input.back || input.context ||
		input.alternate || input.menu || input.view || input.leftShoulder ||
		input.rightShoulder || input.pointerPressed ||
		std::abs(input.leftStickX) > 0.2f || std::abs(input.leftStickY) > 0.2f ||
		std::abs(input.rightStickX) > 0.2f || std::abs(input.rightStickY) > 0.2f;
}

bool SameSettingValue(const InputSettingDraft& left, const InputSettingDraft& right)
{
	return left.id == right.id && left.enabled == right.enabled &&
		left.boolValue == right.boolValue && left.numberValue == right.numberValue &&
		left.choiceValue == right.choiceValue;
}

bool SameDeviceSettings(const std::vector<InputDeviceSettingsDraft>& left,
	const std::vector<InputDeviceSettingsDraft>& right)
{
	if (left.size() != right.size())
		return false;
	for (std::size_t index = 0; index < left.size(); ++index)
	{
		if (left[index].deviceId != right[index].deviceId ||
			left[index].settings.size() != right[index].settings.size())
		{
			return false;
		}
		for (std::size_t setting = 0; setting < left[index].settings.size(); ++setting)
		{
			if (!SameSettingValue(left[index].settings[setting],
				right[index].settings[setting]))
			{
				return false;
			}
		}
	}
	return true;
}

bool SameDsuRoute(const std::optional<DsuPlayerRoute>& left,
	const std::optional<DsuPlayerRoute>& right)
{
	if (left.has_value() != right.has_value())
		return false;
	if (!left)
		return true;
	return left->player == right->player && left->serverId == right->serverId &&
		left->remoteSlot == right->remoteSlot && left->mode == right->mode &&
		left->gamepad == right->gamepad && left->motion == right->motion &&
		left->touch == right->touch;
}

bool SameSharedProfileState(const ControllerProfileDraft& left,
	const ControllerProfileDraft& right)
{
	return left.name == right.name &&
		left.sourceProfileName == right.sourceProfileName &&
		left.player == right.player && left.titleId == right.titleId &&
		left.defaultDeviceId == right.defaultDeviceId &&
		left.attachedDeviceIds == right.attachedDeviceIds &&
		SameDeviceSettings(left.deviceSettings, right.deviceSettings) &&
		SameDsuRoute(left.dsuRoute, right.dsuRoute);
}

void CopySharedProfileState(const ControllerProfileDraft& source,
	ControllerProfileDraft& destination)
{
	destination.name = source.name;
	destination.sourceProfileName = source.sourceProfileName;
	destination.player = source.player;
	destination.titleId = source.titleId;
	destination.defaultDeviceId = source.defaultDeviceId;
	destination.attachedDeviceIds = source.attachedDeviceIds;
	destination.deviceSettings = source.deviceSettings;
	destination.dsuRoute = source.dsuRoute;
}

std::string SettingValueLabel(const InputSettingDraft& setting)
{
	if (!setting.enabled)
		return "Unavailable";
	if (setting.kind == InputSettingKind::Toggle)
		return setting.boolValue ? "On" : "Off";
	if (setting.kind == InputSettingKind::Choice)
	{
		const auto choice = std::ranges::find_if(setting.choices,
			[&setting](const InputSettingChoice& value) {
				return value.value == setting.choiceValue;
			});
		return choice == setting.choices.end() ? setting.choiceValue : choice->label;
	}
	char value[64]{};
	if (setting.step >= 1.0f)
		std::snprintf(value, sizeof(value), "%.0f%s", setting.numberValue,
			setting.suffix.c_str());
	else if (setting.step >= 0.1f)
		std::snprintf(value, sizeof(value), "%.1f%s", setting.numberValue,
			setting.suffix.c_str());
	else
		std::snprintf(value, sizeof(value), "%.2f%s", setting.numberValue,
			setting.suffix.c_str());
	return value;
}

bool AdjustSetting(InputSettingDraft& setting, int direction)
{
	if (!setting.enabled || direction == 0)
		return false;
	if (setting.kind == InputSettingKind::Toggle)
	{
		setting.boolValue = !setting.boolValue;
		return true;
	}
	if (setting.kind == InputSettingKind::Number)
	{
		const float previous = setting.numberValue;
		setting.numberValue = std::clamp(setting.numberValue +
			setting.step * static_cast<float>(direction), setting.minimum, setting.maximum);
		return setting.numberValue != previous;
	}
	if (setting.choices.empty())
		return false;
	auto current = std::ranges::find_if(setting.choices,
		[&setting](const InputSettingChoice& value) {
			return value.value == setting.choiceValue;
		});
	std::ptrdiff_t index = current == setting.choices.end() ? 0 :
		std::distance(setting.choices.begin(), current);
	index = (index + direction + static_cast<std::ptrdiff_t>(setting.choices.size())) %
		static_cast<std::ptrdiff_t>(setting.choices.size());
	setting.choiceValue = setting.choices[static_cast<std::size_t>(index)].value;
	return true;
}

enum class VisualMapperItemKind : std::uint8_t
{
	Tab,
	Selector,
	Mapping,
	Setting,
	Action,
	Toggle,
	Information,
};

struct VisualMapperItem
{
	std::string id;
	std::string label;
	std::string value;
	UnitRect rect;
	VisualMapperItemKind kind = VisualMapperItemKind::Action;
	bool enabled = true;
	bool checked = false;
	bool horizontalAdjust = false;
	std::optional<std::size_t> mappingIndex;
};

constexpr std::array<std::string_view, 4> kVisualPageLabels{
	"Controller", "Motion & Pointer", "Input Sources", "Profiles & Advanced",
};

bool IsMotionOrPointer(InputControlKind kind)
{
	return kind == InputControlKind::Accelerometer ||
		kind == InputControlKind::Gyroscope || kind == InputControlKind::Pointer ||
		kind == InputControlKind::Touch;
}

std::string Lowercase(std::string_view value)
{
	std::string result(value);
	std::ranges::transform(result, result.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return result;
}

ImVec2 RectCenter(const UnitRect& rect)
{
	return { rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f };
}

std::optional<std::size_t> FindDirectionalItem(
	std::span<const VisualMapperItem> items, std::size_t currentIndex,
	int horizontal, int vertical)
{
	if (currentIndex >= items.size() || (horizontal == 0 && vertical == 0))
		return std::nullopt;
	const ImVec2 origin = RectCenter(items[currentIndex].rect);
	float bestScore = std::numeric_limits<float>::max();
	std::optional<std::size_t> best;
	for (std::size_t index = 0; index < items.size(); ++index)
	{
		if (index == currentIndex || !items[index].enabled ||
			items[index].kind == VisualMapperItemKind::Information)
			continue;
		const ImVec2 candidate = RectCenter(items[index].rect);
		const float dx = candidate.x - origin.x;
		const float dy = candidate.y - origin.y;
		if ((horizontal < 0 && dx >= -1.0f) || (horizontal > 0 && dx <= 1.0f) ||
			(vertical < 0 && dy >= -1.0f) || (vertical > 0 && dy <= 1.0f))
			continue;
		const float primary = horizontal == 0 ? std::abs(dy) : std::abs(dx);
		const float secondary = horizontal == 0 ? std::abs(dx) : std::abs(dy);
		if (horizontal != 0 && secondary > primary * 0.65f + 12.0f)
			continue;
		if (vertical != 0 && secondary > primary * 1.25f + 12.0f)
			continue;
		const float score = primary + secondary * 8.0f;
		if (score < bestScore)
		{
			bestScore = score;
			best = index;
		}
	}
	return best;
}

void DrawMapperText(ImDrawList* draw, const ShellLayoutContext& context,
	ImFont* font, float sizeUnits, ImVec2 positionUnits, ImU32 color,
	std::string_view text, const UnitRect* clip = nullptr)
{
	if (!draw || text.empty())
		return;
	font = ResolveFrontendTextFont(font);
	const ImVec2 position = ToPx(context, positionUnits);
	if (!clip)
	{
		draw->AddText(font, sizeUnits * context.uiScale, position, color,
			text.data(), text.data() + text.size());
		return;
	}
	const ImVec2 clipMin = RectMinPx(context, *clip);
	const ImVec2 clipMax = RectMaxPx(context, *clip);
	const ImVec4 clipRect{ clipMin.x, clipMin.y, clipMax.x, clipMax.y };
	draw->AddText(font, sizeUnits * context.uiScale, position, color,
		text.data(), text.data() + text.size(), 0.0f, &clipRect);
}

void DrawMapperGroup(ImDrawList* draw, const ShellLayoutContext& context,
	const UnitRect& rect, std::string_view title, ImFont* font)
{
	const ShellTheme& theme = CurrentWidgetTheme();
	AddRoundedRectFilledPx(draw, RectMinPx(context, rect), RectMaxPx(context, rect),
		ColorWithAlpha(theme.panelBase, 0.68f), 13.0f * context.uiScale);
	AddRoundedRectStrokePx(draw, RectMinPx(context, rect), RectMaxPx(context, rect),
		ColorWithAlpha(theme.panelBorder, 0.72f), 13.0f * context.uiScale,
		1.0f * context.uiScale);
	DrawMapperText(draw, context, font, 14.0f, { rect.x + 12.0f, rect.y + 8.0f },
		ImGui::ColorConvertFloat4ToU32(theme.textSecondary), title, &rect);
}

void DrawMapperItem(ImDrawList* draw, const ShellLayoutContext& context,
	const VisualMapperItem& item, bool focused, bool capturing, ImFont* font)
{
	const ShellTheme& theme = CurrentWidgetTheme();
	ImVec4 fill = theme.panelBase;
	fill.w = item.enabled ? 0.82f : 0.42f;
	if (item.checked)
	{
		fill = theme.panelHighlight;
		fill.w = item.enabled ? 0.82f : 0.42f;
	}
	const ImVec2 min = RectMinPx(context, item.rect);
	const ImVec2 max = RectMaxPx(context, item.rect);
	AddRoundedRectFilledPx(draw, min, max, ImGui::ColorConvertFloat4ToU32(fill),
		8.0f * context.uiScale);
	const ImVec4 border = focused || capturing ? theme.cursorNormal : theme.panelBorder;
	AddRoundedRectStrokePx(draw, min, max, ImGui::ColorConvertFloat4ToU32(border),
		8.0f * context.uiScale, (focused || capturing ? 2.5f : 1.0f) * context.uiScale);
	if (focused)
	{
		AddRoundedRectStrokePx(draw,
			{ min.x - 3.0f * context.uiScale, min.y - 3.0f * context.uiScale },
			{ max.x + 3.0f * context.uiScale, max.y + 3.0f * context.uiScale },
			ColorWithAlpha(theme.cursorGlow, 0.55f), 11.0f * context.uiScale,
			2.0f * context.uiScale);
	}
	const ImU32 labelColor = ColorWithAlpha(theme.textSecondary,
		item.enabled ? 1.0f : 0.48f);
	const ImU32 valueColor = ColorWithAlpha(theme.textPrimary,
		item.enabled ? 1.0f : 0.48f);
	if (item.value.empty())
	{
		DrawMapperText(draw, context, font, 14.0f,
			{ item.rect.x + 10.0f, item.rect.y + item.rect.h * 0.5f - 8.0f },
			valueColor, item.label, &item.rect);
	}
	else if (item.rect.h < 40.0f)
	{
		const UnitRect labelClip{ item.rect.x + 8.0f, item.rect.y,
			item.rect.w * 0.53f - 10.0f, item.rect.h };
		const UnitRect valueClip{ item.rect.x + item.rect.w * 0.53f, item.rect.y,
			item.rect.w * 0.47f - 8.0f, item.rect.h };
		DrawMapperText(draw, context, font, 11.5f,
			{ labelClip.x, item.rect.y + item.rect.h * 0.5f - 7.0f },
			labelColor, item.label, &labelClip);
		DrawMapperText(draw, context, font, 11.5f,
			{ valueClip.x, item.rect.y + item.rect.h * 0.5f - 7.0f },
			valueColor, capturing ? "Waiting..." : item.value, &valueClip);
	}
	else
	{
		DrawMapperText(draw, context, font, 11.5f,
			{ item.rect.x + 10.0f, item.rect.y + 5.0f }, labelColor,
			item.label, &item.rect);
		DrawMapperText(draw, context, font, 14.0f,
			{ item.rect.x + 10.0f, item.rect.y + item.rect.h - 22.0f },
			valueColor, capturing ? "Waiting for input..." : item.value, &item.rect);
	}
}

#if IMGUI_VERSION_NUM >= 19200
ImTextureRef TextureReference(std::uintptr_t id)
{
	return ImTextureRef(static_cast<ImTextureID>(id));
}
#else
ImTextureID TextureReference(std::uintptr_t id)
{
	return reinterpret_cast<ImTextureID>(id);
}
#endif

void DrawControllerPreview(ImDrawList* draw, const ShellLayoutContext& context,
	const UnitRect& rect, TextureHandle texture, ImFont* font)
{
	if (!draw)
		return;
	if (!texture || texture.width == 0 || texture.height == 0)
	{
		const ShellTheme& theme = CurrentWidgetTheme();
		const std::string_view message = "Controller preview unavailable";
		font = ResolveFrontendTextFont(font);
		const float fontSize = 13.0f * context.uiScale;
		const ImVec2 size = font->CalcTextSizeA(fontSize,
			std::numeric_limits<float>::max(), 0.0f, message.data(),
			message.data() + message.size());
		const ImVec2 center = ToPx(context,
			{ rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f });
		draw->AddText(font, fontSize,
			{ center.x - size.x * 0.5f, center.y - size.y * 0.5f },
			ImGui::ColorConvertFloat4ToU32(theme.textSecondary), message.data(),
			message.data() + message.size());
		return;
	}

	const float imageHeight = rect.h;
	const float imageWidth = imageHeight * static_cast<float>(texture.width) /
		static_cast<float>(texture.height);
	const UnitRect imageRect{ rect.x + (rect.w - imageWidth) * 0.5f,
		rect.y, imageWidth, imageHeight };
	draw->PushClipRect(RectMinPx(context, rect), RectMaxPx(context, rect), true);
	draw->AddImage(TextureReference(texture.id), RectMinPx(context, imageRect),
		RectMaxPx(context, imageRect));
	draw->PopClipRect();
}
}

void InputMapperView::Open(IInputMappingHost& host,
	std::optional<std::uint64_t> titleId, std::uint8_t player)
{
	m_host = &host;
	m_titleId = titleId;
	m_open = true;
	m_visualPage = InputMapperVisualPage::Controller;
	m_visualFocusId = "setup:controller";
	m_selectedMappingIndex.reset();
	m_playerDrafts.fill(std::nullopt);
	for (auto& drafts : m_controllerDrafts)
		drafts.clear();
	m_playerLoaded = false;
	m_devices = host.EnumerateInputDevices();
	LoadPlayer(std::min<std::uint8_t>(player, 7));
}

void InputMapperView::Close() noexcept
{
	if (IsNativeTextInputActive())
		DismissNativeTextInput();
	m_open = false;
	m_expressionEditorOpen = false;
	m_expressionTextEditorOpen = false;
	m_profileNameEditorOpen = false;
	m_profileDeleteConfirmation = false;
	m_capturePhase = CapturePhase::None;
	m_captureInputReleaseBarrier = false;
	m_captured.clear();
	m_playerLoaded = false;
}

bool InputMapperView::IsCapturing() const noexcept
{
	return m_open && (m_capturePhase != CapturePhase::None ||
		m_captureInputReleaseBarrier ||
		m_expressionEditorOpen || m_profileNameEditorOpen || IsNativeTextInputActive());
}

void InputMapperView::LoadPlayer(std::uint8_t player)
{
	if (!m_host)
		return;
	if (m_playerLoaded)
	{
		m_playerDrafts[m_player] = m_draft;
		if (!m_draft.emulatedControllerTypeId.empty())
			m_controllerDrafts[m_player][m_draft.emulatedControllerTypeId] = m_draft;
	}
	m_player = std::min<std::uint8_t>(player, 7);
	if (m_playerDrafts[m_player])
		m_draft = *m_playerDrafts[m_player];
	else
		m_draft = m_host->LoadProfileDraft(m_player, m_titleId);
	m_playerLoaded = true;
	m_dsuServers = m_host->EnumerateDsuServers();
	m_controllerTypes = m_host->EnumerateEmulatedControllerTypes(m_player);
	if (m_draft.emulatedControllerTypeId.empty() && !m_controllerTypes.empty())
		m_draft.emulatedControllerTypeId = m_controllerTypes.front().id;
	RebuildControls(true);
	if (!m_draft.emulatedControllerTypeId.empty())
		m_controllerDrafts[m_player][m_draft.emulatedControllerTypeId] = m_draft;
	m_selectedMappingIndex.reset();
	m_profiles = m_host->EnumerateProfileNames(m_player);
	if (m_settingsDeviceId.empty() ||
		std::ranges::find(m_draft.attachedDeviceIds, m_settingsDeviceId) ==
			m_draft.attachedDeviceIds.end())
	{
		m_settingsDeviceId = m_draft.defaultDeviceId.empty() ?
			(m_draft.attachedDeviceIds.empty() ? std::string{} :
				m_draft.attachedDeviceIds.front()) : m_draft.defaultDeviceId;
	}
	if (!m_settingsDeviceId.empty())
		EnsureDeviceSettings(m_settingsDeviceId);
	m_status.clear();
	ValidateDraft();
}

void InputMapperView::BeginCapture(bool append)
{
	const auto mappingIndex = SelectedMappingIndex();
	if (!m_host || !mappingIndex)
		return;
	if (!m_captureFromAllDevices && m_draft.defaultDeviceId.empty())
	{
		m_status = "Select a default input device before capturing";
		return;
	}
	m_captureAppend = append;
	m_capturePhase = CapturePhase::WaitingForNeutral;
	m_captureInputReleaseBarrier = true;
	m_captureStarted = SteadySeconds();
	m_neutralSince = 0.0;
	m_chordStarted = 0.0;
	m_baseline = FilterCaptureSamples(m_host->ReadLiveInputs());
	m_captured.clear();
	m_status = "Release controls, then press the new binding";
}

std::vector<InputCaptureSample> InputMapperView::FilterCaptureSamples(
	std::vector<InputCaptureSample> samples) const
{
	if (m_captureFromAllDevices || m_draft.defaultDeviceId.empty())
		return samples;
	std::erase_if(samples, [this](const InputCaptureSample& sample) {
		return sample.deviceId != m_draft.defaultDeviceId;
	});
	return samples;
}

void InputMapperView::UpdateCapture(double nowSeconds)
{
	if (!m_host || m_capturePhase == CapturePhase::None)
		return;
	if (nowSeconds - m_captureStarted >= 3.0)
	{
		m_capturePhase = CapturePhase::None;
		m_status = "Capture timed out";
		return;
	}
	const auto samples = FilterCaptureSamples(m_host->ReadLiveInputs());
	auto baselineFor = [this](const InputCaptureSample& sample) -> const InputCaptureSample* {
		const auto found = std::find_if(m_baseline.begin(), m_baseline.end(),
			[&](const InputCaptureSample& value) {
				return value.deviceId == sample.deviceId && value.controlId == sample.controlId;
			});
		return found == m_baseline.end() ? nullptr : &*found;
	};
	const bool anyActive = std::ranges::any_of(samples,
		[&](const InputCaptureSample& sample) {
			return IsActive(sample, baselineFor(sample));
		});
	if (m_capturePhase == CapturePhase::WaitingForNeutral)
	{
		if (anyActive)
			m_neutralSince = 0.0;
		else if (m_neutralSince == 0.0)
			m_neutralSince = nowSeconds;
		else if (nowSeconds - m_neutralSince >= 0.15)
		{
			m_capturePhase = CapturePhase::Collecting;
			m_baseline = samples;
			m_status = "Press a button, axis, or chord";
		}
		return;
	}

	for (const InputCaptureSample& sample : samples)
	{
		if (!IsActive(sample, baselineFor(sample)))
			continue;
		const auto duplicate = std::find_if(m_captured.begin(), m_captured.end(),
			[&](const InputCaptureSample& value) {
				return value.deviceId == sample.deviceId && value.controlId == sample.controlId;
			});
		if (duplicate == m_captured.end())
		{
			m_captured.push_back(sample);
			if (m_chordStarted == 0.0)
				m_chordStarted = nowSeconds;
		}
	}
	const double confirmationSeconds = m_waitForAlternateInputs ? 0.75 : 0.35;
	if (!m_captured.empty() && nowSeconds - m_chordStarted >= confirmationSeconds)
		AcceptCapture();
}

std::string InputMapperView::BuildCapturedExpression() const
{
	std::string result;
	for (const InputCaptureSample& sample : m_captured)
	{
		if (!result.empty())
			result += " & ";
		const bool useDefaultReference = !m_draft.defaultDeviceId.empty() &&
			sample.deviceId == m_draft.defaultDeviceId;
		result += EscapeInputControlReference(useDefaultReference ? sample.controlId :
			sample.deviceId + "/" + sample.controlId);
	}
	return result;
}

void InputMapperView::AcceptCapture()
{
	const auto mappingIndex = SelectedMappingIndex();
	if (!mappingIndex)
		return;
	std::string expression = BuildCapturedExpression();
	if (expression.empty())
		return;
	for (const InputCaptureSample& sample : m_captured)
	{
		if (std::find(m_draft.attachedDeviceIds.begin(), m_draft.attachedDeviceIds.end(),
			sample.deviceId) == m_draft.attachedDeviceIds.end())
		{
			m_draft.attachedDeviceIds.push_back(sample.deviceId);
			if (m_draft.defaultDeviceId.empty())
				m_draft.defaultDeviceId = sample.deviceId;
		}
	}
	auto& mapping = m_draft.mappings[*mappingIndex];
	if (m_captureAppend && !mapping.expression.empty())
		expression = "(" + mapping.expression + ") | (" + expression + ")";
	mapping.expression = std::move(expression);
	m_draft.dirty = true;
	m_capturePhase = CapturePhase::None;
	m_status = "Binding captured; release the control";
	ValidateDraft();

	if (m_iterativeMapping)
	{
		const auto visible = VisibleMappingIndices();
		const auto current = std::ranges::find(visible, *mappingIndex);
		if (current != visible.end() && std::next(current) != visible.end())
		{
			m_selectedMappingIndex = *std::next(current);
			m_visualFocusId = "mapping:" + std::to_string(*m_selectedMappingIndex);
			BeginCapture(false);
			m_status = "Release controls, then map the next binding";
		}
		else
			m_status = "Iterative mapping complete";
	}
}

void InputMapperView::OpenExpressionEditor()
{
	const auto mappingIndex = SelectedMappingIndex();
	if (!mappingIndex)
		return;
	const std::string& expression = m_draft.mappings[*mappingIndex].expression;
	std::memset(m_expressionBuffer.data(), 0, m_expressionBuffer.size());
	std::memcpy(m_expressionBuffer.data(), expression.data(),
		std::min(expression.size(), m_expressionBuffer.size() - 1));
	m_expressionEditorOpen = true;
	m_expressionTextEditorOpen = false;
	m_expressionRows.selected = 0;
	m_expressionRows.scroll = 0.0f;
	m_expressionDeviceId = m_draft.defaultDeviceId;
	if (m_expressionDeviceId.empty() && !m_devices.empty())
		m_expressionDeviceId = m_devices.front().id;
}

void InputMapperView::OpenProfileNameEditor()
{
	if (m_titleId)
		return;
	std::memset(m_profileNameBuffer.data(), 0, m_profileNameBuffer.size());
	std::memcpy(m_profileNameBuffer.data(), m_draft.name.data(),
		std::min(m_draft.name.size(), m_profileNameBuffer.size() - 1));
	m_profileNameEditorOpen = true;
	m_profileNameEditorState.requestFocus = true;
}

void InputMapperView::RebuildControls(bool preserveMappings)
{
	if (!m_host)
		return;
	std::unordered_map<std::string, ControllerMappingDraft> previous;
	if (preserveMappings)
	{
		for (auto& mapping : m_draft.mappings)
			previous.emplace(mapping.emulatedControlId, std::move(mapping));
	}
	m_controls = m_host->EnumerateEmulatedControls(m_player,
		m_draft.emulatedControllerTypeId);
	m_draft.mappings.clear();
	m_draft.mappings.reserve(m_controls.size());
	for (const auto& control : m_controls)
	{
		const auto found = previous.find(control.id);
		if (found == previous.end())
			m_draft.mappings.push_back({ control.id, {} });
		else
			m_draft.mappings.push_back(std::move(found->second));
	}
	m_selectedMappingIndex.reset();
	ValidateDraft();
}

void InputMapperView::SelectControllerType(std::string_view typeId)
{
	if (!m_host || typeId.empty() || typeId == m_draft.emulatedControllerTypeId)
		return;
	if (!m_draft.emulatedControllerTypeId.empty())
		m_controllerDrafts[m_player][m_draft.emulatedControllerTypeId] = m_draft;

	ControllerProfileDraft next;
	auto& drafts = m_controllerDrafts[m_player];
	const auto cached = drafts.find(std::string(typeId));
	const bool restored = cached != drafts.end();
	if (restored)
		next = cached->second;
	else
	{
		next = m_host->CreateDefaultProfile(m_player, m_titleId, typeId,
			m_draft.defaultDeviceId);
		next.dirty = true;
	}
	const bool sharedStateChanged = !SameSharedProfileState(m_draft, next);
	CopySharedProfileState(m_draft, next);
	next.emulatedControllerTypeId = std::string(typeId);
	next.dirty = next.dirty || sharedStateChanged;
	m_draft = std::move(next);
	RebuildControls(true);
	drafts[m_draft.emulatedControllerTypeId] = m_draft;
	m_status = restored ? "Emulated controller mappings restored" :
		"Default mappings staged for the emulated controller";
}

void InputMapperView::EnsureDeviceSettings(std::string_view deviceId)
{
	if (!m_host || deviceId.empty() || std::ranges::any_of(m_draft.deviceSettings,
		[deviceId](const InputDeviceSettingsDraft& settings) {
			return settings.deviceId == deviceId;
		}))
		return;
	m_draft.deviceSettings.push_back(m_host->CreateInputDeviceSettings(deviceId));
}

InputDeviceSettingsDraft* InputMapperView::SelectedDeviceSettings()
{
	const auto found = std::ranges::find_if(m_draft.deviceSettings,
		[this](const InputDeviceSettingsDraft& settings) {
			return settings.deviceId == m_settingsDeviceId;
		});
	return found == m_draft.deviceSettings.end() ? nullptr : &*found;
}

const InputDeviceSettingsDraft* InputMapperView::SelectedDeviceSettings() const
{
	const auto found = std::ranges::find_if(m_draft.deviceSettings,
		[this](const InputDeviceSettingsDraft& settings) {
			return settings.deviceId == m_settingsDeviceId;
		});
	return found == m_draft.deviceSettings.end() ? nullptr : &*found;
}

std::vector<std::size_t> InputMapperView::VisibleMappingIndices() const
{
	std::vector<std::size_t> indices;
	if (m_visualPage != InputMapperVisualPage::Controller &&
		m_visualPage != InputMapperVisualPage::MotionPointer)
		return indices;
	for (std::size_t index = 0; index < m_controls.size() && index < m_draft.mappings.size(); ++index)
	{
		const auto kind = m_controls[index].kind;
		const bool motion = kind == InputControlKind::Accelerometer ||
			kind == InputControlKind::Gyroscope || kind == InputControlKind::Pointer ||
			kind == InputControlKind::Touch;
		if ((m_visualPage == InputMapperVisualPage::MotionPointer) == motion)
			indices.push_back(index);
	}
	return indices;
}

std::optional<std::size_t> InputMapperView::SelectedMappingIndex() const
{
	if (m_selectedMappingIndex && *m_selectedMappingIndex < m_draft.mappings.size())
		return m_selectedMappingIndex;
	return std::nullopt;
}

void InputMapperView::ValidateDraft()
{
	for (ControllerMappingDraft& mapping : m_draft.mappings)
	{
		mapping.validationWarning.clear();
		if (mapping.expression.empty())
		{
			mapping.validationError.clear();
			continue;
		}
		MappingExpression expression(mapping.expression);
		mapping.validationError = expression.IsValid() ? std::string{} : expression.Error();
	}

	std::unordered_map<std::string, std::size_t> firstUse;
	for (std::size_t index = 0; index < m_draft.mappings.size(); ++index)
	{
		auto& mapping = m_draft.mappings[index];
		if (mapping.expression.empty() || !mapping.validationError.empty())
			continue;
		const auto [existing, inserted] = firstUse.emplace(mapping.expression, index);
		if (inserted)
			continue;
		const std::size_t first = existing->second;
		const std::string firstLabel = first < m_controls.size() ?
			m_controls[first].label : m_draft.mappings[first].emulatedControlId;
		const std::string currentLabel = index < m_controls.size() ?
			m_controls[index].label : mapping.emulatedControlId;
		mapping.validationWarning = "Also assigned to " + firstLabel;
		m_draft.mappings[first].validationWarning = "Also assigned to " + currentLabel;
	}
}

InputMapperResult InputMapperView::Draw(const ShellLayoutContext& context,
	ImDrawList* draw, const FrameInput& input, ImFont* font)
{
	InputMapperResult result;
	if (!m_open || !m_host || !draw)
		return result;

	const bool captureOwnedFrame = m_capturePhase != CapturePhase::None;
	UpdateCapture(SteadySeconds());
	const bool releaseBarrierOwnedFrame = m_captureInputReleaseBarrier;
	if (m_capturePhase == CapturePhase::None && m_captureInputReleaseBarrier &&
		!HasControllerUiInput(input))
	{
		m_captureInputReleaseBarrier = false;
		if (m_status == "Binding captured; release the control")
			m_status = "Binding captured";
	}
	const bool navigationAvailable = !captureOwnedFrame &&
		!releaseBarrierOwnedFrame && m_capturePhase == CapturePhase::None &&
		!m_expressionEditorOpen && !m_profileNameEditorOpen && !IsNativeTextInputActive();
	if (navigationAvailable && input.leftShoulder && m_player > 0)
	{
		LoadPlayer(m_player - 1);
		m_visualFocusId = "player:" + std::to_string(m_player);
	}
	else if (navigationAvailable && input.rightShoulder && m_player < 7)
	{
		LoadPlayer(m_player + 1);
		m_visualFocusId = "player:" + std::to_string(m_player);
	}

	DrawDimmedBackground();
	const UnitRect panel{ 24.0f, 18.0f, context.layoutUnitsW - 48.0f,
		context.layoutUnitsH - 36.0f };
	DrawOverlayPanel(draw, context, panel, CurrentWidgetTheme());
	DrawOverlayHeader(draw, context, panel, CurrentWidgetTheme(),
		"Controller Mapping",
		m_titleId ? "Per-game controller profiles" : "Global controller profiles",
		"Player " + std::to_string(m_player + 1), font);

	const float innerX = panel.x + 18.0f;
	const float innerW = panel.w - 36.0f;
	const float gap = 7.0f;
	std::vector<VisualMapperItem> items;
	items.reserve(96);
	auto addItem = [&items](VisualMapperItem item) {
		items.emplace_back(std::move(item));
	};

	auto deviceLabel = [this](std::string_view id) {
		if (id.empty())
			return std::string("None");
		const auto found = std::ranges::find_if(m_devices,
			[id](const InputDeviceDescriptor& device) { return device.id == id; });
		return found == m_devices.end() ? std::string("Disconnected input device") :
			found->label;
	};
	auto controllerDescriptor = [this]() -> const EmulatedControllerDescriptor* {
		const auto found = std::ranges::find_if(m_controllerTypes,
			[this](const EmulatedControllerDescriptor& type) {
				return type.id == m_draft.emulatedControllerTypeId;
			});
		return found == m_controllerTypes.end() ? nullptr : &*found;
	};
	auto controllerLabel = [&controllerDescriptor, this]() {
		const auto* descriptor = controllerDescriptor();
		return descriptor ? descriptor->label : m_draft.emulatedControllerTypeId;
	};
	auto routeServerLabel = [this]() {
		if (!m_draft.dsuRoute)
			return std::string("Off");
		const auto found = std::ranges::find_if(m_dsuServers,
			[this](const DsuServerModel& server) {
				return server.id == m_draft.dsuRoute->serverId;
			});
		if (found == m_dsuServers.end())
			return m_draft.dsuRoute->serverId + " (missing)";
		return found->name + (found->enabled ? "" : " (disabled)");
	};
	auto findDeviceSetting = [this](std::string_view id) -> InputSettingDraft* {
		auto* device = SelectedDeviceSettings();
		if (!device)
			return nullptr;
		const auto found = std::ranges::find_if(device->settings,
			[id](const InputSettingDraft& setting) { return setting.id == id; });
		return found == device->settings.end() ? nullptr : &*found;
	};
	auto findControllerSetting = [this](std::string_view id) -> InputSettingDraft* {
		const auto found = std::ranges::find_if(m_draft.controllerSettings,
			[id](const InputSettingDraft& setting) { return setting.id == id; });
		return found == m_draft.controllerSettings.end() ? nullptr : &*found;
	};
	auto settingLabel = [](std::string_view id) {
		if (id == "axis_deadzone") return std::string("Left stick deadzone");
		if (id == "axis_range") return std::string("Left stick range");
		if (id == "rotation_deadzone") return std::string("Right stick deadzone");
		if (id == "rotation_range") return std::string("Right stick range");
		if (id == "trigger_deadzone") return std::string("Trigger deadzone");
		if (id == "trigger_range") return std::string("Trigger range");
		if (id == "motion") return std::string("Use motion");
		if (id == "rumble") return std::string("Rumble strength");
		if (id == "accelerometer_sensitivity") return std::string("Accelerometer sensitivity");
		if (id == "gyroscope_sensitivity") return std::string("Gyroscope sensitivity");
		if (id == "gyroscope_deadzone") return std::string("Gyroscope dead zone");
		if (id == "gyroscope_calibration_period") return std::string("Calibration period");
		if (id == "pointer_sensitivity_x") return std::string("Horizontal sensitivity");
		if (id == "pointer_sensitivity_y") return std::string("Vertical sensitivity");
		if (id == "pointer_invert_x") return std::string("Invert pointer X");
		if (id == "pointer_invert_y") return std::string("Invert pointer Y");
		if (id == "pointer_enabled") return std::string("Virtual touch / IR pointer");
		if (id == "pointer_speed") return std::string("Pointer speed");
		if (id == "pointer_deadzone") return std::string("Pointer stick dead zone");
		return std::string(id);
	};
	auto mappingValue = [this, &routeServerLabel](std::size_t index) {
		if (index >= m_draft.mappings.size() || index >= m_controls.size())
			return std::string("Not mapped");
		const auto& mapping = m_draft.mappings[index];
		const auto& control = m_controls[index];
		if (!mapping.validationError.empty())
			return "Error: " + mapping.validationError;
		if (!mapping.expression.empty())
		{
			std::string value = FormatInputExpressionForDisplay(mapping.expression,
				m_devices, m_draft.defaultDeviceId, control.label);
			if (!mapping.validationWarning.empty())
				value += " / " + mapping.validationWarning;
			return value;
		}
		const bool routeMotion = m_draft.dsuRoute && m_draft.dsuRoute->motion &&
			(control.kind == InputControlKind::Accelerometer ||
				control.kind == InputControlKind::Gyroscope);
		const bool routePointer = m_draft.dsuRoute && m_draft.dsuRoute->touch &&
			(control.kind == InputControlKind::Pointer ||
				control.kind == InputControlKind::Touch);
		if (routeMotion || routePointer)
			return "Automatic from " + routeServerLabel() + ", slot " +
				std::to_string(m_draft.dsuRoute->remoteSlot);
		return std::string("Not mapped");
	};
	auto pendingMapping = [this, &mappingValue](std::size_t index,
		std::string label = {}) {
		return VisualMapperItem{ "mapping:" + std::to_string(index),
			label.empty() ? m_controls[index].label : std::move(label),
			mappingValue(index), {},
			VisualMapperItemKind::Mapping, true, false, false, index };
	};
	auto pendingDeviceSetting = [&findDeviceSetting, &settingLabel](std::string_view id) {
		InputSettingDraft* setting = findDeviceSetting(id);
		return VisualMapperItem{ "device-setting:" + std::string(id),
			setting ? setting->label : settingLabel(id),
			setting ? SettingValueLabel(*setting) : "Attach a compatible device",
			{}, setting && setting->kind == InputSettingKind::Toggle ?
				VisualMapperItemKind::Toggle : VisualMapperItemKind::Setting,
			setting && setting->enabled,
			setting && setting->kind == InputSettingKind::Toggle && setting->boolValue,
			setting && setting->kind != InputSettingKind::Toggle };
	};
	auto pendingControllerSetting = [](InputSettingDraft& setting) {
		return VisualMapperItem{ "controller-setting:" + setting.id, setting.label,
			SettingValueLabel(setting), {},
			setting.kind == InputSettingKind::Toggle ?
				VisualMapperItemKind::Toggle : VisualMapperItemKind::Setting,
			setting.enabled,
			setting.kind == InputSettingKind::Toggle && setting.boolValue,
			setting.kind != InputSettingKind::Toggle };
	};
	auto drawStack = [&](const UnitRect& rect, std::string_view title,
		std::vector<VisualMapperItem> stack, float maximumHeight = 37.0f) {
		DrawMapperGroup(draw, context, rect, title, font);
		if (stack.empty())
		{
			stack.push_back({ "info:" + std::string(title), "No options available", {},
				{}, VisualMapperItemKind::Information, false });
		}
		const float stackGap = 4.0f;
		const float available = rect.h - 40.0f -
			stackGap * static_cast<float>(stack.size() - 1);
		const float itemHeight = std::min(maximumHeight,
			std::max(24.0f, available / static_cast<float>(stack.size())));
		float y = rect.y + 31.0f;
		for (auto& item : stack)
		{
			item.rect = { rect.x + 8.0f, y, rect.w - 16.0f, itemHeight };
			addItem(std::move(item));
			y += itemHeight + stackGap;
		}
	};
	auto drawGrid = [&](const UnitRect& rect, std::string_view title,
		std::vector<VisualMapperItem> grid, std::size_t columns,
		float maximumHeight = 37.0f) {
		DrawMapperGroup(draw, context, rect, title, font);
		columns = std::max<std::size_t>(1, columns);
		if (grid.empty())
		{
			grid.push_back({ "info:" + std::string(title), "No options available", {},
				{}, VisualMapperItemKind::Information, false });
		}
		const std::size_t rowCount = (grid.size() + columns - 1) / columns;
		const float itemGap = 4.0f;
		const float columnGap = 5.0f;
		const float available = rect.h - 40.0f -
			itemGap * static_cast<float>(rowCount - 1);
		const float itemHeight = std::min(maximumHeight,
			std::max(27.0f, available / static_cast<float>(rowCount)));
		const float itemWidth = (rect.w - 16.0f -
			columnGap * static_cast<float>(columns - 1)) /
			static_cast<float>(columns);
		for (std::size_t index = 0; index < grid.size(); ++index)
		{
			const std::size_t row = index / columns;
			const std::size_t column = index % columns;
			grid[index].rect = {
				rect.x + 8.0f + static_cast<float>(column) * (itemWidth + columnGap),
				rect.y + 31.0f + static_cast<float>(row) * (itemHeight + itemGap),
				itemWidth, itemHeight };
			addItem(std::move(grid[index]));
		}
	};

	const float playerY = panel.y + 72.0f;
	const float playerWidth = (innerW - gap * 7.0f) / 8.0f;
	for (std::size_t player = 0; player < 8; ++player)
	{
		const bool modified = player == m_player ? m_draft.dirty :
			(m_playerDrafts[player] && m_playerDrafts[player]->dirty);
		addItem({ "player:" + std::to_string(player),
			"Player " + std::to_string(player + 1),
			modified ? "Modified" : std::string{},
			{ innerX + static_cast<float>(player) * (playerWidth + gap), playerY,
				playerWidth, 31.0f }, VisualMapperItemKind::Tab, true,
			player == m_player });
	}

	const float setupY = playerY + 38.0f;
	const float actionWidth = 78.0f;
	const float selectorWidth = innerW - actionWidth * 4.0f - gap * 6.0f;
	const float controllerWidth = selectorWidth * 0.25f;
	const float deviceWidth = selectorWidth * 0.42f;
	const float profileWidth = selectorWidth - controllerWidth - deviceWidth;
	float setupX = innerX;
	addItem({ "setup:controller", "Emulated controller", controllerLabel(),
		{ setupX, setupY, controllerWidth, 47.0f }, VisualMapperItemKind::Selector,
		!m_controllerTypes.empty(), false, true });
	setupX += controllerWidth + gap;
	addItem({ "setup:device", "Input device", deviceLabel(m_draft.defaultDeviceId),
		{ setupX, setupY, deviceWidth, 47.0f }, VisualMapperItemKind::Selector,
		!m_devices.empty(), false, true });
	setupX += deviceWidth + gap;
	const std::string profileName = m_draft.sourceProfileName.empty() ?
		m_draft.name : m_draft.sourceProfileName;
	addItem({ "setup:profile", "Profile", profileName.empty() ? "Recommended" : profileName,
		{ setupX, setupY, profileWidth, 47.0f }, VisualMapperItemKind::Selector,
		true, false, true });
	setupX += profileWidth + gap;
	addItem({ "setup:save", "Save", m_draft.dirty ? "Modified" : "Ready",
		{ setupX, setupY, actionWidth, 47.0f }, VisualMapperItemKind::Action });
	setupX += actionWidth + gap;
	addItem({ "setup:reset", "Reset", {},
		{ setupX, setupY, actionWidth, 47.0f }, VisualMapperItemKind::Action,
		!m_controls.empty() });
	setupX += actionWidth + gap;
	addItem({ "setup:new", "New", {},
		{ setupX, setupY, actionWidth, 47.0f }, VisualMapperItemKind::Action,
		!m_titleId });
	setupX += actionWidth + gap;
	const bool canDeleteProfile = !m_titleId &&
		std::ranges::find(m_profiles, m_draft.name) != m_profiles.end();
	addItem({ "setup:delete", "Delete",
		m_profileDeleteConfirmation ? "Confirm" : std::string{},
		{ setupX, setupY, actionWidth, 47.0f }, VisualMapperItemKind::Action,
		canDeleteProfile });

	const float pageY = setupY + 55.0f;
	const float pageWidth = (innerW - gap * 3.0f) / 4.0f;
	for (std::size_t page = 0; page < kVisualPageLabels.size(); ++page)
	{
		addItem({ "page:" + std::to_string(page), std::string(kVisualPageLabels[page]), {},
			{ innerX + static_cast<float>(page) * (pageWidth + gap), pageY,
				pageWidth, 33.0f }, VisualMapperItemKind::Tab, true,
			page == static_cast<std::size_t>(m_visualPage) });
	}

	const UnitRect content{ innerX, pageY + 40.0f, innerW,
		panel.bottom() - (pageY + 40.0f) - 38.0f };
	DrawOverlayPanel(draw, context, content, CurrentWidgetTheme());

	if (m_visualPage == InputMapperVisualPage::Controller)
	{
		std::vector<std::size_t> dpadMappings;
		std::vector<std::size_t> leftStickMappings;
		std::vector<std::size_t> faceMappings;
		std::vector<std::size_t> rightStickMappings;
		std::vector<std::size_t> centerMappings;
		const auto* selectedController = controllerDescriptor();
		const std::array<std::string, 4> emptyFaceButtons{};
		const auto& faceButtonLayout = selectedController ?
			selectedController->faceButtonLabels : emptyFaceButtons;
		auto isFaceButton = [&faceButtonLayout](std::string_view label) {
			return std::ranges::any_of(faceButtonLayout,
				[label](const std::string& button) {
					return !button.empty() && Lowercase(button) == Lowercase(label);
				});
		};
		for (const std::size_t index : VisibleMappingIndices())
		{
			const std::string group = Lowercase(m_controls[index].group);
			const std::string label = Lowercase(m_controls[index].label);
			if (group.find("d-pad") != std::string::npos)
			{
				dpadMappings.push_back(index);
			}
			else if (group.find("left stick") != std::string::npos ||
				group.find("nunchuk stick") != std::string::npos)
			{
				leftStickMappings.push_back(index);
			}
			else if (group.find("right stick") != std::string::npos)
			{
				rightStickMappings.push_back(index);
			}
			else if (isFaceButton(m_controls[index].label) ||
				label == "a" || label == "b" || label == "x" || label == "y" ||
				label == "1" || label == "2" ||
				group.find("nunchuk buttons") != std::string::npos)
			{
				faceMappings.push_back(index);
			}
			else
				centerMappings.push_back(index);
		}
		auto leafLabel = [this](std::size_t index) {
			const std::string& label = m_controls[index].label;
			const std::string prefix = m_controls[index].group + " ";
			return label.starts_with(prefix) ? label.substr(prefix.size()) : label;
		};
		auto directionalOrder = [&leafLabel](std::size_t index) {
			const std::string label = Lowercase(leafLabel(index));
			if (label == "click" || label.ends_with(" click")) return 0;
			if (label == "up" || label.ends_with(" up")) return 1;
			if (label == "right" || label.ends_with(" right")) return 2;
			if (label == "down" || label.ends_with(" down")) return 3;
			if (label == "left" || label.ends_with(" left")) return 4;
			return 5;
		};
		for (auto* mappings : { &dpadMappings, &leftStickMappings,
			&rightStickMappings })
		{
			std::ranges::stable_sort(*mappings, {}, directionalOrder);
		}
		std::ranges::stable_sort(faceMappings, {},
			[&faceButtonLayout, this](std::size_t index) {
				const std::string label = Lowercase(m_controls[index].label);
				const auto found = std::ranges::find_if(faceButtonLayout,
					[&label](const std::string& button) {
						return Lowercase(button) == label;
					});
				return found == faceButtonLayout.end() ? faceButtonLayout.size() :
					static_cast<std::size_t>(std::distance(faceButtonLayout.begin(), found));
			});

		const float sideWidth = std::clamp(content.w * 0.225f, 225.0f, 280.0f);
		const UnitRect left{ content.x + 9.0f, content.y + 9.0f,
			sideWidth, content.h - 18.0f };
		const UnitRect right{ content.right() - sideWidth - 9.0f, content.y + 9.0f,
			sideWidth, content.h - 18.0f };
		const UnitRect center{ left.right() + 8.0f, content.y + 9.0f,
			right.x - left.right() - 16.0f, content.h - 18.0f };

		auto mappingItems = [&pendingMapping, &leafLabel](
			const std::vector<std::size_t>& mappings) {
			std::vector<VisualMapperItem> result;
			result.reserve(mappings.size());
			for (const auto index : mappings)
				result.push_back(pendingMapping(index, leafLabel(index)));
			return result;
		};
		auto drawSide = [&drawStack](const UnitRect& rect,
			std::string_view firstTitle, std::vector<VisualMapperItem> first,
			std::string_view secondTitle, std::vector<VisualMapperItem> second) {
			if (first.empty())
			{
				drawStack(rect, secondTitle, std::move(second), 35.0f);
				return;
			}
			if (second.empty())
			{
				drawStack(rect, firstTitle, std::move(first), 35.0f);
				return;
			}
			const float sectionGap = 7.0f;
			const float firstWeight = static_cast<float>(first.size()) + 1.25f;
			const float secondWeight = static_cast<float>(second.size()) + 1.25f;
			const float firstHeight = std::clamp(
				(rect.h - sectionGap) * firstWeight / (firstWeight + secondWeight),
				(rect.h - sectionGap) * 0.30f, (rect.h - sectionGap) * 0.60f);
			drawStack({ rect.x, rect.y, rect.w, firstHeight }, firstTitle,
				std::move(first), 35.0f);
			drawStack({ rect.x, rect.y + firstHeight + sectionGap, rect.w,
				rect.h - firstHeight - sectionGap }, secondTitle,
				std::move(second), 35.0f);
		};

		auto dpadItems = mappingItems(dpadMappings);
		auto leftStickItems = mappingItems(leftStickMappings);
		if (!leftStickItems.empty())
		{
			leftStickItems.push_back(pendingDeviceSetting("axis_deadzone"));
			leftStickItems.push_back(pendingDeviceSetting("axis_range"));
		}
		const std::string leftStickTitle = leftStickMappings.empty() ? "Left Stick" :
			m_controls[leftStickMappings.front()].group;
		drawSide(left, dpadMappings.empty() ? "D-Pad" :
			m_controls[dpadMappings.front()].group, std::move(dpadItems),
			leftStickTitle, std::move(leftStickItems));

		auto faceItems = mappingItems(faceMappings);
		auto rightStickItems = mappingItems(rightStickMappings);
		if (!rightStickItems.empty())
		{
			rightStickItems.push_back(pendingDeviceSetting("rotation_deadzone"));
			rightStickItems.push_back(pendingDeviceSetting("rotation_range"));
		}
		drawSide(right, "Face Buttons", std::move(faceItems), "Right Stick",
			std::move(rightStickItems));

		DrawMapperGroup(draw, context, center, "Controller", font);
		const float previewHeight = std::clamp(center.h * 0.43f, 145.0f, 185.0f);
		const TextureHandle previewTexture = selectedController ?
			m_host->GetControllerPreviewTexture(selectedController->id) :
			TextureHandle{};
		DrawControllerPreview(draw, context,
			{ center.x + 14.0f, center.y + 27.0f, center.w - 28.0f, previewHeight },
			previewTexture, font);
		std::vector<VisualMapperItem> centerItems;
		for (const auto index : centerMappings)
			centerItems.push_back(pendingMapping(index));
		for (auto& setting : m_draft.controllerSettings)
		{
			if (setting.group == "Accelerometer" || setting.group == "Gyroscope" ||
				setting.group == "Pointer")
				continue;
			centerItems.push_back(pendingControllerSetting(setting));
		}
		centerItems.push_back(pendingDeviceSetting("trigger_deadzone"));
		centerItems.push_back(pendingDeviceSetting("trigger_range"));
		const float controlsY = center.y + 31.0f + previewHeight;
		const float controlsHeight = center.bottom() - controlsY - 8.0f;
		const std::size_t rows = (centerItems.size() + 1) / 2;
		const float itemGap = 4.0f;
		const float columnGap = 6.0f;
		const float itemHeight = rows == 0 ? controlsHeight :
			std::min(34.0f, std::max(25.0f,
				(controlsHeight - itemGap * static_cast<float>(rows - 1)) /
				static_cast<float>(rows)));
		const float columnWidth = (center.w - 16.0f - columnGap) * 0.5f;
		for (std::size_t index = 0; index < centerItems.size(); ++index)
		{
			const std::size_t row = index / 2;
			const std::size_t column = index % 2;
			centerItems[index].rect = {
				center.x + 8.0f + static_cast<float>(column) * (columnWidth + columnGap),
				controlsY + static_cast<float>(row) * (itemHeight + itemGap),
				columnWidth, itemHeight };
			addItem(std::move(centerItems[index]));
		}
	}
	else if (m_visualPage == InputMapperVisualPage::MotionPointer)
	{
		const auto selectedSettingsDevice = std::ranges::find_if(m_devices,
			[this](const InputDeviceDescriptor& device) {
				return device.id == m_settingsDeviceId;
			});
		if (selectedSettingsDevice == m_devices.end() ||
			!selectedSettingsDevice->supportsMotion)
		{
			const auto motionDevice = std::ranges::find_if(m_devices,
				[this](const InputDeviceDescriptor& device) {
					return device.supportsMotion &&
						std::ranges::find(m_draft.attachedDeviceIds, device.id) !=
							m_draft.attachedDeviceIds.end();
				});
			if (motionDevice != m_devices.end())
			{
				m_settingsDeviceId = motionDevice->id;
				EnsureDeviceSettings(m_settingsDeviceId);
			}
		}
		std::vector<std::size_t> accelerometerMappings;
		std::vector<std::size_t> gyroscopeMappings;
		std::vector<std::size_t> pointerMappings;
		for (const std::size_t index : VisibleMappingIndices())
		{
			if (m_controls[index].kind == InputControlKind::Pointer ||
				m_controls[index].kind == InputControlKind::Touch)
				pointerMappings.push_back(index);
			else if (m_controls[index].kind == InputControlKind::Accelerometer)
				accelerometerMappings.push_back(index);
			else
				gyroscopeMappings.push_back(index);
		}
		const float columnGap = 8.0f;
		const float columnWidth = (content.w - 18.0f - columnGap * 2.0f) / 3.0f;
		const UnitRect accelerometerRect{ content.x + 9.0f, content.y + 9.0f,
			columnWidth, content.h - 18.0f };
		const UnitRect gyroscopeRect{ accelerometerRect.right() + columnGap,
			content.y + 9.0f, columnWidth, content.h - 18.0f };
		const UnitRect pointerRect{ gyroscopeRect.right() + columnGap,
			content.y + 9.0f, columnWidth, content.h - 18.0f };

		auto appendControllerSettings = [&pendingControllerSetting, this](
			std::vector<VisualMapperItem>& target, std::string_view group) {
			for (auto& setting : m_draft.controllerSettings)
			{
				if (setting.group == group)
					target.push_back(pendingControllerSetting(setting));
			}
		};
		std::vector<VisualMapperItem> accelerometerItems;
		for (const auto index : accelerometerMappings)
			accelerometerItems.push_back(pendingMapping(index));
		appendControllerSettings(accelerometerItems, "Accelerometer");
		accelerometerItems.push_back(pendingDeviceSetting("motion"));
		drawStack(accelerometerRect, "Accelerometer",
			std::move(accelerometerItems), 42.0f);

		std::vector<VisualMapperItem> gyroscopeItems;
		for (const auto index : gyroscopeMappings)
			gyroscopeItems.push_back(pendingMapping(index));
		appendControllerSettings(gyroscopeItems, "Gyroscope");
		const auto selectedDevice = std::ranges::find_if(m_devices,
			[this](const InputDeviceDescriptor& device) {
				return device.id == m_settingsDeviceId;
			});
		gyroscopeItems.push_back({ "input:calibrate", "Calibrate / reset bias",
			selectedDevice == m_devices.end() ? "No device selected" : selectedDevice->label,
			{}, VisualMapperItemKind::Action,
			selectedDevice != m_devices.end() && selectedDevice->supportsCalibration });
		drawStack(gyroscopeRect, "Gyroscope", std::move(gyroscopeItems), 42.0f);

		std::vector<VisualMapperItem> pointerItems;
		for (const auto index : pointerMappings)
			pointerItems.push_back(pendingMapping(index));
		appendControllerSettings(pointerItems, "Pointer");
		pointerItems.push_back(pendingDeviceSetting("pointer_enabled"));
		pointerItems.push_back(pendingDeviceSetting("pointer_speed"));
		pointerItems.push_back(pendingDeviceSetting("pointer_deadzone"));
		drawStack(pointerRect, "Pointer and Touch", std::move(pointerItems), 38.0f);
	}
	else if (m_visualPage == InputMapperVisualPage::InputSources)
	{
		const float columnWidth = (content.w - 34.0f) / 3.0f;
		const UnitRect devicesRect{ content.x + 9.0f, content.y + 9.0f,
			columnWidth, content.h - 18.0f };
		const UnitRect settingsRect{ devicesRect.right() + 8.0f, content.y + 9.0f,
			columnWidth, content.h - 18.0f };
		const UnitRect dsuRect{ settingsRect.right() + 8.0f, content.y + 9.0f,
			content.right() - settingsRect.right() - 17.0f, content.h - 18.0f };

		std::vector<VisualMapperItem> deviceItems;
		deviceItems.push_back({ "setup:device", "Default input device",
			deviceLabel(m_draft.defaultDeviceId), {}, VisualMapperItemKind::Selector,
			!m_devices.empty(), false, true });
		deviceItems.push_back({ "input:refresh", "Refresh devices",
			"Rescan connected controllers and enabled DSU endpoints", {},
			VisualMapperItemKind::Action });
		for (const auto& device : m_devices)
		{
			const bool attached = std::ranges::find(m_draft.attachedDeviceIds,
				device.id) != m_draft.attachedDeviceIds.end();
			std::string detail = device.connected ? "Connected" : "Disconnected";
			if (device.id == m_draft.defaultDeviceId)
				detail += " / Default";
			deviceItems.push_back({ "input:device:" + device.id, device.label,
				std::move(detail), {}, VisualMapperItemKind::Toggle, true, attached });
		}
		for (const std::string& attached : m_draft.attachedDeviceIds)
		{
			if (std::ranges::any_of(m_devices,
				[&attached](const InputDeviceDescriptor& device) {
					return device.id == attached;
				}))
				continue;
			deviceItems.push_back({ "input:device:" + attached,
				"Disconnected input device",
				"Disconnected / Attached", {}, VisualMapperItemKind::Toggle,
				true, true });
		}
		drawStack(devicesRect, "Input Devices", std::move(deviceItems), 34.0f);

		std::vector<VisualMapperItem> settingItems;
		settingItems.push_back({ "input:settings-device", "Configure device",
			deviceLabel(m_settingsDeviceId), {}, VisualMapperItemKind::Selector,
			!m_draft.attachedDeviceIds.empty(), false, true });
		if (auto* selected = SelectedDeviceSettings())
		{
			for (auto& setting : selected->settings)
				settingItems.push_back(pendingDeviceSetting(setting.id));
		}
		else
		{
			for (const std::string_view id : { "motion", "rumble", "axis_deadzone",
				"axis_range", "rotation_deadzone", "rotation_range",
				"trigger_deadzone", "trigger_range", "pointer_enabled", "pointer_speed",
				"pointer_deadzone" })
				settingItems.push_back(pendingDeviceSetting(id));
		}
		const auto settingsDevice = std::ranges::find_if(m_devices,
			[this](const InputDeviceDescriptor& device) {
				return device.id == m_settingsDeviceId;
			});
		settingItems.push_back({ "input:connect", "Connect / refresh",
			settingsDevice == m_devices.end() ? "Unavailable" : settingsDevice->label,
			{}, VisualMapperItemKind::Action,
			settingsDevice != m_devices.end() && settingsDevice->supportsConnection });
		settingItems.push_back({ "input:calibrate", "Calibrate",
			"Sticks, triggers and motion", {}, VisualMapperItemKind::Action,
			settingsDevice != m_devices.end() && settingsDevice->supportsCalibration });
		settingItems.push_back({ "input:rumble", "Test rumble", {},
			{}, VisualMapperItemKind::Action,
			settingsDevice != m_devices.end() && settingsDevice->supportsRumble });
		drawGrid(settingsRect, "Device Settings", std::move(settingItems), 2, 37.0f);

		const bool hasRoute = m_draft.dsuRoute.has_value();
		std::vector<VisualMapperItem> dsuItems;
		dsuItems.push_back({ "dsu:server", "Server", routeServerLabel(), {},
			VisualMapperItemKind::Selector, !m_dsuServers.empty(), false, true });
		if (hasRoute)
		{
			const auto server = std::ranges::find_if(m_dsuServers,
				[this](const DsuServerModel& value) {
					return value.id == m_draft.dsuRoute->serverId;
				});
			if (server != m_dsuServers.end())
				dsuItems.push_back({ "dsu:server-info", "Endpoint",
					server->host + ":" + std::to_string(server->port),
					{}, VisualMapperItemKind::Information, false });
		}
		dsuItems.push_back({ "dsu:slot", "Remote controller slot",
			hasRoute ? std::to_string(m_draft.dsuRoute->remoteSlot) : "Select a server",
			{}, VisualMapperItemKind::Selector, hasRoute, false, true });
		dsuItems.push_back({ "dsu:mode", "Routing mode",
			hasRoute && m_draft.dsuRoute->mode == DsuRouteMode::Merge ?
				"Merge with other devices" : "Full controller",
			{}, VisualMapperItemKind::Selector, hasRoute, false, true });
		dsuItems.push_back({ "dsu:gamepad", "Buttons, sticks and triggers",
			hasRoute && m_draft.dsuRoute->gamepad ? "On" : "Off", {},
			VisualMapperItemKind::Toggle, hasRoute,
			hasRoute && m_draft.dsuRoute->gamepad });
		dsuItems.push_back({ "dsu:motion", "Accelerometer and gyroscope",
			hasRoute && m_draft.dsuRoute->motion ? "On" : "Off", {},
			VisualMapperItemKind::Toggle, hasRoute,
			hasRoute && m_draft.dsuRoute->motion });
		dsuItems.push_back({ "dsu:touch", "Touch and pointer",
			hasRoute && m_draft.dsuRoute->touch ? "On" : "Off", {},
			VisualMapperItemKind::Toggle, hasRoute,
			hasRoute && m_draft.dsuRoute->touch });
		dsuItems.push_back({ "dsu:remove", "Remove DSU route",
			"Server configuration is kept", {}, VisualMapperItemKind::Action,
			hasRoute });
		if (m_dsuServers.empty())
			dsuItems.push_back({ "dsu:none", "No DSU servers configured",
				"Add a server under Settings - DSU/Streaming", {},
				VisualMapperItemKind::Information, false });
		drawStack(dsuRect, "DSU Player Route", std::move(dsuItems), 36.0f);
	}
	else
	{
		const float columnWidth = (content.w - 34.0f) / 3.0f;
		const UnitRect profilesRect{ content.x + 9.0f, content.y + 9.0f,
			columnWidth, content.h - 18.0f };
		const UnitRect optionsRect{ profilesRect.right() + 8.0f, content.y + 9.0f,
			columnWidth, content.h - 18.0f };
		const UnitRect liveRect{ optionsRect.right() + 8.0f, content.y + 9.0f,
			content.right() - optionsRect.right() - 17.0f, content.h - 18.0f };

		std::vector<VisualMapperItem> profileItems;
		profileItems.push_back({ "profile:recommended", "Recommended defaults",
			"Build mappings for the selected input device", {},
			VisualMapperItemKind::Action });
		profileItems.push_back({ "profile:name",
			m_titleId ? "Per-game profile" : "Profile name", m_draft.name, {},
			VisualMapperItemKind::Action, !m_titleId });
		profileItems.push_back({ "setup:save", "Save profile",
			m_draft.dirty ? "Unsaved changes" : "No changes", {},
			VisualMapperItemKind::Action });
		for (const std::string& profile : m_profiles)
		{
			profileItems.push_back({ "profile:load:" + profile, profile,
				profile == m_draft.sourceProfileName ? "Loaded source" : "Load profile",
				{}, VisualMapperItemKind::Action, true,
				profile == m_draft.sourceProfileName });
		}
		if (m_profiles.empty())
			profileItems.push_back({ "profile:none", "No named profiles",
				"Use New to create one", {}, VisualMapperItemKind::Information, false });
		drawStack(profilesRect, "Profiles", std::move(profileItems), 36.0f);

		const auto settingsDevice = std::ranges::find_if(m_devices,
			[this](const InputDeviceDescriptor& device) {
				return device.id == m_settingsDeviceId;
			});
		const bool canCalibrate = settingsDevice != m_devices.end() &&
			settingsDevice->supportsCalibration;
		const bool canRumble = settingsDevice != m_devices.end() &&
			settingsDevice->supportsRumble;
		std::vector<VisualMapperItem> optionItems{
			{ "options:capture-all", "Capture from all devices",
				m_captureFromAllDevices ? "On" : "Default device only", {},
				VisualMapperItemKind::Toggle, true, m_captureFromAllDevices },
			{ "options:alternate-wait", "Wait for alternate inputs",
				m_waitForAlternateInputs ? "750 ms" : "350 ms", {},
				VisualMapperItemKind::Toggle, true, m_waitForAlternateInputs },
			{ "options:iterative", "Iterative mapping",
				m_iterativeMapping ? "Continue through controls" : "One control", {},
				VisualMapperItemKind::Toggle, true, m_iterativeMapping },
			{ "options:defaults", "Restore defaults",
				"Uses the selected input device", {}, VisualMapperItemKind::Action },
			{ "options:clear", "Clear all mappings",
				"Devices and DSU routing are retained", {}, VisualMapperItemKind::Action },
			{ "input:calibrate", "Calibrate selected device", {},
				{}, VisualMapperItemKind::Action, canCalibrate },
			{ "input:rumble", "Test rumble", {},
				{}, VisualMapperItemKind::Action, canRumble },
			{ "options:cancel", "Cancel", "Discard staged changes",
				{}, VisualMapperItemKind::Action },
		};
		drawStack(optionsRect, "Mapping Options", std::move(optionItems), 39.0f);

		std::vector<VisualMapperItem> liveItems;
		for (const auto& sample : m_host->ReadLiveInputs())
		{
			if (std::abs(sample.value) < 0.01f)
				continue;
			char value[32]{};
			std::snprintf(value, sizeof(value), "%.3f", sample.value);
			const std::string controlLabel = FormatInputExpressionForDisplay(
				EscapeInputControlReference(sample.deviceId + "/" + sample.controlId), m_devices,
				m_draft.defaultDeviceId, "Input");
			liveItems.push_back({ "live:" + sample.deviceId + "/" + sample.controlId,
				controlLabel, value + std::string(" / ") + deviceLabel(sample.deviceId),
				{}, VisualMapperItemKind::Information, false });
			if (liveItems.size() >= 10)
				break;
		}
		if (liveItems.empty())
			liveItems.push_back({ "live:none", "Waiting for input",
				"Move or press a control to inspect its value", {},
				VisualMapperItemKind::Information, false });
		drawStack(liveRect, "Live Input", std::move(liveItems), 37.0f);
	}

	if (items.empty())
		return result;
	auto focus = std::ranges::find_if(items, [this](const VisualMapperItem& item) {
		return item.id == m_visualFocusId && item.enabled &&
			item.kind != VisualMapperItemKind::Information;
	});
	if (focus == items.end())
	{
		focus = std::ranges::find_if(items, [](const VisualMapperItem& item) {
			return item.enabled && item.kind != VisualMapperItemKind::Information;
		});
		if (focus != items.end())
			m_visualFocusId = focus->id;
	}

	std::optional<std::string> pointerActivated;
	for (const auto& item : items)
	{
		ImGui::PushID(item.id.c_str());
		ImGui::SetCursorScreenPos(RectMinPx(context, item.rect));
		const ImVec2 itemSize{ item.rect.w * context.uiScale,
			item.rect.h * context.uiScale };
		ImGui::InvisibleButton("##mapper-hit", itemSize);
		if (navigationAvailable && item.enabled && ImGui::IsItemHovered())
			m_visualFocusId = item.id;
		if (navigationAvailable && item.enabled &&
			ImGui::IsItemClicked(ImGuiMouseButton_Left))
			pointerActivated = item.id;
		ImGui::PopID();
		const bool capturing = item.mappingIndex &&
			m_capturePhase != CapturePhase::None &&
			m_selectedMappingIndex == item.mappingIndex;
		DrawMapperItem(draw, context, item, item.id == m_visualFocusId,
			capturing, font);
	}

	auto parseIndex = [](std::string_view id, std::string_view prefix) ->
		std::optional<std::size_t> {
		if (!id.starts_with(prefix) || id.size() == prefix.size())
			return std::nullopt;
		std::size_t value = 0;
		for (const char character : id.substr(prefix.size()))
		{
			if (character < '0' || character > '9')
				return std::nullopt;
			value = value * 10 + static_cast<std::size_t>(character - '0');
		}
		return value;
	};
	auto selectDefaultDevice = [&](int direction) {
		if (m_devices.empty())
			return;
		auto current = std::ranges::find_if(m_devices,
			[this](const InputDeviceDescriptor& device) {
				return device.id == m_draft.defaultDeviceId;
			});
		std::ptrdiff_t index = current == m_devices.end() ? 0 :
			std::distance(m_devices.begin(), current);
		index = (index + direction + static_cast<std::ptrdiff_t>(m_devices.size())) %
			static_cast<std::ptrdiff_t>(m_devices.size());
		const std::string id = m_devices[static_cast<std::size_t>(index)].id;
		m_draft.defaultDeviceId = id;
		if (std::ranges::find(m_draft.attachedDeviceIds, id) ==
			m_draft.attachedDeviceIds.end())
			m_draft.attachedDeviceIds.push_back(id);
		m_settingsDeviceId = id;
		EnsureDeviceSettings(id);
		m_draft.dirty = true;
		m_status = "Default input device selected";
	};
	auto selectController = [&](int direction) {
		if (m_controllerTypes.empty())
			return;
		auto current = std::ranges::find_if(m_controllerTypes,
			[this](const EmulatedControllerDescriptor& type) {
				return type.id == m_draft.emulatedControllerTypeId;
			});
		std::ptrdiff_t index = current == m_controllerTypes.end() ? 0 :
			std::distance(m_controllerTypes.begin(), current);
		index = (index + direction +
			static_cast<std::ptrdiff_t>(m_controllerTypes.size())) %
			static_cast<std::ptrdiff_t>(m_controllerTypes.size());
		SelectControllerType(m_controllerTypes[static_cast<std::size_t>(index)].id);
	};
	auto loadProfile = [&](std::string_view name) {
		ControllerProfileDraft selected = name.empty() ?
			m_host->CreateDefaultProfile(m_player, m_titleId) :
			m_host->LoadNamedProfileDraft(m_player, name, m_titleId);
		if (selected.mappings.empty())
		{
			m_status = "The selected profile could not be loaded";
			return;
		}
		m_draft = std::move(selected);
		m_controllerTypes = m_host->EnumerateEmulatedControllerTypes(m_player);
		RebuildControls(true);
		m_controllerDrafts[m_player].clear();
		m_controllerDrafts[m_player][m_draft.emulatedControllerTypeId] = m_draft;
		m_settingsDeviceId = m_draft.defaultDeviceId.empty() ?
			(m_draft.attachedDeviceIds.empty() ? std::string{} :
				m_draft.attachedDeviceIds.front()) : m_draft.defaultDeviceId;
		if (!m_settingsDeviceId.empty())
			EnsureDeviceSettings(m_settingsDeviceId);
		m_draft.dirty = true;
		m_status = name.empty() ? "Recommended mappings staged" :
			(m_titleId ? "Profile cloned for this title" : "Profile loaded");
	};
	auto resetMappings = [&]() {
		ControllerProfileDraft defaults = m_host->CreateDefaultProfile(m_player,
			m_titleId, m_draft.emulatedControllerTypeId, m_draft.defaultDeviceId);
		if (defaults.mappings.empty())
		{
			m_status = "Default mappings are unavailable for this controller";
			return;
		}
		m_draft.mappings = std::move(defaults.mappings);
		m_draft.controllerSettings = std::move(defaults.controllerSettings);
		for (auto& defaultDevice : defaults.deviceSettings)
		{
			const auto current = std::ranges::find_if(m_draft.deviceSettings,
				[&defaultDevice](const InputDeviceSettingsDraft& settings) {
					return settings.deviceId == defaultDevice.deviceId;
				});
			if (current == m_draft.deviceSettings.end())
				m_draft.deviceSettings.push_back(std::move(defaultDevice));
			else
				*current = std::move(defaultDevice);
		}
		RebuildControls(true);
		m_draft.dirty = true;
		m_controllerDrafts[m_player][m_draft.emulatedControllerTypeId] = m_draft;
		m_status = "Controller mappings reset to defaults";
	};
	auto cycleProfile = [&](int direction) {
		std::vector<std::string> choices{ {} };
		choices.insert(choices.end(), m_profiles.begin(), m_profiles.end());
		std::string currentName = m_draft.sourceProfileName;
		auto current = std::ranges::find(choices, currentName);
		std::ptrdiff_t index = current == choices.end() ? 0 :
			std::distance(choices.begin(), current);
		index = (index + direction + static_cast<std::ptrdiff_t>(choices.size())) %
			static_cast<std::ptrdiff_t>(choices.size());
		loadProfile(choices[static_cast<std::size_t>(index)]);
	};
	auto cycleSettingsDevice = [&](int direction) {
		if (m_draft.attachedDeviceIds.empty())
			return;
		auto current = std::ranges::find(m_draft.attachedDeviceIds, m_settingsDeviceId);
		std::ptrdiff_t index = current == m_draft.attachedDeviceIds.end() ? 0 :
			std::distance(m_draft.attachedDeviceIds.begin(), current);
		index = (index + direction +
			static_cast<std::ptrdiff_t>(m_draft.attachedDeviceIds.size())) %
			static_cast<std::ptrdiff_t>(m_draft.attachedDeviceIds.size());
		m_settingsDeviceId =
			m_draft.attachedDeviceIds[static_cast<std::size_t>(index)];
		EnsureDeviceSettings(m_settingsDeviceId);
		m_status = "Device settings selected";
	};
	auto cycleDsuServer = [&](int direction) {
		if (m_dsuServers.empty())
			return;
		std::ptrdiff_t index = 0;
		if (m_draft.dsuRoute)
		{
			const auto current = std::ranges::find_if(m_dsuServers,
				[this](const DsuServerModel& server) {
					return server.id == m_draft.dsuRoute->serverId;
				});
			if (current != m_dsuServers.end())
				index = std::distance(m_dsuServers.begin(), current) + 1;
		}
		const std::ptrdiff_t count =
			static_cast<std::ptrdiff_t>(m_dsuServers.size()) + 1;
		index = (index + direction + count) % count;
		if (index == 0)
			m_draft.dsuRoute.reset();
		else
		{
			const auto& server = m_dsuServers[static_cast<std::size_t>(index - 1)];
			if (!m_draft.dsuRoute)
				m_draft.dsuRoute = DsuPlayerRoute{ .player = m_player,
					.serverId = server.id, .remoteSlot = m_player };
			else
				m_draft.dsuRoute->serverId = server.id;
		}
		m_draft.dirty = true;
		m_status = m_draft.dsuRoute ? "DSU server selected" :
			"DSU route disabled";
	};
	auto adjustSettingById = [&](std::string_view id, int direction) {
		InputSettingDraft* setting = nullptr;
		if (id.starts_with("device-setting:"))
			setting = findDeviceSetting(id.substr(std::string_view("device-setting:").size()));
		else if (id.starts_with("controller-setting:"))
			setting = findControllerSetting(
				id.substr(std::string_view("controller-setting:").size()));
		if (!setting || !AdjustSetting(*setting, direction))
			return false;
		m_draft.dirty = true;
		m_status = setting->label + " changed";
		return true;
	};
	auto saveAllDrafts = [&]() {
		m_playerDrafts[m_player] = m_draft;
		for (std::size_t first = 0; first < m_playerDrafts.size(); ++first)
		{
			if (!m_playerDrafts[first] || !m_playerDrafts[first]->dsuRoute)
				continue;
			const DsuPlayerRoute& route = *m_playerDrafts[first]->dsuRoute;
			for (std::size_t second = first + 1; second < m_playerDrafts.size(); ++second)
			{
				if (!m_playerDrafts[second] || !m_playerDrafts[second]->dsuRoute)
					continue;
				const DsuPlayerRoute& other = *m_playerDrafts[second]->dsuRoute;
				if (route.serverId != other.serverId || route.remoteSlot != other.remoteSlot)
					continue;
				result.error = "Players " + std::to_string(first + 1) + " and " +
					std::to_string(second + 1) +
					" cannot use the same DSU server and slot";
				m_status = result.error;
				return false;
			}
		}
		std::vector<ControllerProfileDraft> draftsToSave;
		for (const auto& draft : m_playerDrafts)
		{
			if (draft && draft->dirty)
				draftsToSave.push_back(*draft);
		}
		// a no-op save still persists the selected player
		if (draftsToSave.empty())
			draftsToSave.push_back(m_draft);
		const MappingSaveResult saved = m_host->SaveProfileDrafts(draftsToSave);
		if (!saved.succeeded)
		{
			result.error = saved.error;
			m_status = result.error;
			return false;
		}
		for (auto& draft : m_playerDrafts)
		{
			if (draft && std::ranges::any_of(draftsToSave,
				[&draft](const ControllerProfileDraft& savedDraft) {
					return savedDraft.player == draft->player;
				}))
			{
				draft->dirty = false;
			}
		}
		m_host->ResetMappingRuntime();
		Close();
		result.saved = true;
		return true;
	};

	auto activate = [&](const VisualMapperItem& item, int direction) {
		if (!item.enabled)
			return false;
		if (const auto player = parseIndex(item.id, "player:"))
		{
			if (*player < 8)
			{
				LoadPlayer(static_cast<std::uint8_t>(*player));
				m_visualFocusId = "player:" + std::to_string(*player);
			}
			return false;
		}
		if (const auto page = parseIndex(item.id, "page:"))
		{
			if (*page < kVisualPageLabels.size())
			{
				m_visualPage = static_cast<InputMapperVisualPage>(*page);
				m_selectedMappingIndex.reset();
				m_visualFocusId = "page:" + std::to_string(*page);
			}
			return false;
		}
		if (item.mappingIndex)
		{
			m_selectedMappingIndex = item.mappingIndex;
			BeginCapture(false);
			return false;
		}
		if (item.id == "setup:controller")
			selectController(direction);
		else if (item.id == "setup:device")
			selectDefaultDevice(direction);
		else if (item.id == "setup:profile")
			cycleProfile(direction);
		else if (item.id == "setup:save")
			return saveAllDrafts();
		else if (item.id == "setup:reset")
			resetMappings();
		else if (item.id == "setup:new" || item.id == "profile:name")
			OpenProfileNameEditor();
		else if (item.id == "setup:delete")
		{
			if (!m_profileDeleteConfirmation)
			{
				m_profileDeleteConfirmation = true;
				m_status = "Press A again to delete " + m_draft.name;
			}
			else
			{
				const MappingSaveResult deleted = m_host->DeleteNamedProfile(m_draft.name);
				m_profileDeleteConfirmation = false;
				if (!deleted.succeeded)
					m_status = deleted.error;
				else
				{
					m_profiles = m_host->EnumerateProfileNames(m_player);
					loadProfile({});
					m_status = "Controller profile deleted";
				}
			}
		}
		else if (item.id == "input:refresh")
		{
			m_host->RefreshInputDevices();
			m_devices = m_host->EnumerateInputDevices();
			m_dsuServers = m_host->EnumerateDsuServers();
			m_status = "Input devices refreshed";
		}
		else if (item.id == "input:settings-device")
			cycleSettingsDevice(direction);
		else if (item.id.starts_with("input:device:"))
		{
			const std::string id =
				item.id.substr(std::string_view("input:device:").size());
			const auto attached = std::ranges::find(m_draft.attachedDeviceIds, id);
			if (attached == m_draft.attachedDeviceIds.end())
			{
				m_draft.attachedDeviceIds.push_back(id);
				if (m_draft.defaultDeviceId.empty())
					m_draft.defaultDeviceId = id;
				m_settingsDeviceId = id;
				EnsureDeviceSettings(id);
				m_status = "Input device attached";
			}
			else
			{
				m_draft.attachedDeviceIds.erase(attached);
				if (m_draft.defaultDeviceId == id)
					m_draft.defaultDeviceId = m_draft.attachedDeviceIds.empty() ?
						std::string{} : m_draft.attachedDeviceIds.front();
				if (m_settingsDeviceId == id)
					m_settingsDeviceId = m_draft.defaultDeviceId;
				m_status = "Input device detached";
			}
			m_draft.dirty = true;
		}
		else if (item.id == "input:connect")
		{
			const MappingSaveResult connected =
				m_host->ConnectInputDevice(m_settingsDeviceId);
			m_status = connected.succeeded ? "Input device connected" : connected.error;
			m_devices = m_host->EnumerateInputDevices();
		}
		else if (item.id == "input:calibrate")
		{
			const MappingSaveResult calibrated =
				m_host->CalibrateInputDevice(m_settingsDeviceId);
			m_status = calibrated.succeeded ? "Input device calibrated" : calibrated.error;
		}
		else if (item.id == "input:rumble")
		{
			float strength = 0.5f;
			if (InputSettingDraft* rumble = findDeviceSetting("rumble"))
			{
				strength = rumble->numberValue;
				if (rumble->suffix == "%" && rumble->maximum > 1.0f)
					strength /= 100.0f;
				strength = std::max(0.1f, strength);
			}
			m_host->TestRumble(m_settingsDeviceId, strength,
				std::chrono::milliseconds(350));
			m_status = "Rumble test started";
		}
		else if (item.id == "dsu:server")
			cycleDsuServer(direction);
		else if (item.id == "dsu:slot" && m_draft.dsuRoute)
		{
			m_draft.dsuRoute->remoteSlot = static_cast<std::uint8_t>(
				(m_draft.dsuRoute->remoteSlot + (direction > 0 ? 1 : 15)) % 16);
			m_draft.dirty = true;
			m_status = "DSU controller slot changed";
		}
		else if (item.id == "dsu:mode" && m_draft.dsuRoute)
		{
			m_draft.dsuRoute->mode =
				m_draft.dsuRoute->mode == DsuRouteMode::Merge ?
				DsuRouteMode::FullController : DsuRouteMode::Merge;
			m_draft.dirty = true;
			m_status = m_draft.dsuRoute->mode == DsuRouteMode::Merge ?
				"DSU will merge with attached devices" :
				"DSU will provide the full controller";
		}
		else if (item.id == "dsu:gamepad" && m_draft.dsuRoute)
		{
			m_draft.dsuRoute->gamepad = !m_draft.dsuRoute->gamepad;
			m_draft.dirty = true;
		}
		else if (item.id == "dsu:motion" && m_draft.dsuRoute)
		{
			m_draft.dsuRoute->motion = !m_draft.dsuRoute->motion;
			m_draft.dirty = true;
		}
		else if (item.id == "dsu:touch" && m_draft.dsuRoute)
		{
			m_draft.dsuRoute->touch = !m_draft.dsuRoute->touch;
			m_draft.dirty = true;
		}
		else if (item.id == "dsu:remove")
		{
			m_draft.dsuRoute.reset();
			m_draft.dirty = true;
			m_status = "DSU route removed";
		}
		else if (item.id == "profile:recommended")
			loadProfile({});
		else if (item.id.starts_with("profile:load:"))
			loadProfile(item.id.substr(std::string_view("profile:load:").size()));
		else if (item.id == "options:capture-all")
			m_captureFromAllDevices = !m_captureFromAllDevices;
		else if (item.id == "options:alternate-wait")
			m_waitForAlternateInputs = !m_waitForAlternateInputs;
		else if (item.id == "options:iterative")
			m_iterativeMapping = !m_iterativeMapping;
		else if (item.id == "options:defaults")
			resetMappings();
		else if (item.id == "options:clear")
		{
			for (auto& mapping : m_draft.mappings)
				mapping.expression.clear();
			m_draft.dirty = true;
			ValidateDraft();
			m_status = "All mappings cleared";
		}
		else if (item.id == "options:cancel")
		{
			Close();
			result.cancelled = true;
			return true;
		}
		else
			adjustSettingById(item.id, direction);
		return false;
	};

	focus = std::ranges::find_if(items, [this](const VisualMapperItem& item) {
		return item.id == m_visualFocusId;
	});
	if (navigationAvailable && focus != items.end())
	{
		const std::size_t focusIndex =
			static_cast<std::size_t>(std::distance(items.begin(), focus));
		const int horizontal = input.left ? -1 : (input.right ? 1 : 0);
		const int vertical = input.up ? -1 : (input.down ? 1 : 0);
		if (const auto next = FindDirectionalItem(items, focusIndex,
			horizontal, vertical))
			m_visualFocusId = items[*next].id;
	}

	if (pointerActivated)
		m_visualFocusId = *pointerActivated;
	focus = std::ranges::find_if(items, [this](const VisualMapperItem& item) {
		return item.id == m_visualFocusId;
	});
	if (navigationAvailable && focus != items.end())
	{
		if (input.accept || pointerActivated == focus->id)
		{
			if (activate(*focus, 1))
				return result;
			// starting capture consumes the activation frame so chords become bindings
			if (m_capturePhase != CapturePhase::None)
				return result;
		}
		if (focus->mappingIndex)
		{
			m_selectedMappingIndex = focus->mappingIndex;
			if (input.menu)
				OpenExpressionEditor();
		}
		else if (input.alternate && focus->id.starts_with("input:device:"))
		{
			const std::string id =
				focus->id.substr(std::string_view("input:device:").size());
			if (std::ranges::find(m_draft.attachedDeviceIds, id) ==
				m_draft.attachedDeviceIds.end())
				m_draft.attachedDeviceIds.push_back(id);
			m_draft.defaultDeviceId = id;
			m_settingsDeviceId = id;
			EnsureDeviceSettings(id);
			m_draft.dirty = true;
			m_status = "Default input device selected";
		}
		else if (input.alternate && focus->horizontalAdjust)
		{
			if (activate(*focus, -1))
				return result;
		}
		if (input.view)
			resetMappings();
	}

	if (!m_status.empty())
	{
		const UnitRect statusRect{ innerX + 6.0f, panel.bottom() - 31.0f,
			innerW - 12.0f, 22.0f };
		DrawMapperText(draw, context, font, 13.0f,
			{ statusRect.x, statusRect.y + 2.0f },
			ImGui::ColorConvertFloat4ToU32(CurrentWidgetTheme().textSecondary),
			m_status, &statusRect);
	}
	if (m_profileNameEditorOpen)
	{
		const UnitRect editor = CenteredPanel(context, 680.0f, 270.0f);
		DrawOverlayPanel(draw, context, editor, CurrentWidgetTheme());
		DrawOverlayHeader(draw, context, editor, CurrentWidgetTheme(),
			"Controller Profile Name", {}, {}, font);
		const auto editorResult = DrawTextEditorPanel(context, draw, editor,
			m_profileNameEditorState, m_profileNameBuffer.data(), m_profileNameBuffer.size(),
			font, { .label = "Profile name",
				.footer = "Enter a new name to save as a separate reusable profile." });
		if (editorResult.save)
		{
			const std::string name = m_profileNameBuffer.data();
			if (name.empty())
				m_status = "Enter a profile name";
			else
			{
				m_draft.name = name;
				m_draft.dirty = true;
				m_profileDeleteConfirmation = false;
				m_profileNameEditorOpen = false;
				m_status = "Profile name staged";
			}
		}
		if (editorResult.cancel)
			m_profileNameEditorOpen = false;
		return result;
	}

	if (m_expressionEditorOpen)
	{
		const UnitRect editor = CenteredPanel(context, 980.0f, 590.0f);
		DrawOverlayPanel(draw, context, editor, CurrentWidgetTheme());
		DrawOverlayHeader(draw, context, editor, CurrentWidgetTheme(),
			"Advanced Input Expression", "Device-qualified controls, operators and functions",
			{}, font);
		if (m_expressionTextEditorOpen)
		{
			const auto editorResult = DrawTextEditorPanel(context, draw, editor,
				m_expressionEditorState, m_expressionBuffer.data(), m_expressionBuffer.size(),
				font, { .label = "Mapping expression",
					.footer = "Edit the complete expression, then choose Apply." });
			if (editorResult.save || editorResult.cancel)
				m_expressionTextEditorOpen = false;
			return result;
		}

		std::vector<ActionListEntry> expressionRows;
		expressionRows.push_back({ "advanced:edit", "Edit complete expression",
			m_expressionBuffer.data() });
		std::string deviceLabel = "No input device";
		const auto selectedDevice = std::ranges::find_if(m_devices,
			[this](const InputDeviceDescriptor& device) {
				return device.id == m_expressionDeviceId;
			});
		if (selectedDevice != m_devices.end())
			deviceLabel = selectedDevice->label;
		expressionRows.push_back({ "advanced:device", "Input device", std::move(deviceLabel),
			!m_devices.empty(), false, false, true });
		expressionRows.push_back({ "advanced:section:inputs", "Inputs", {}, false });
		if (selectedDevice != m_devices.end())
		{
			for (const auto& control : selectedDevice->controls)
				expressionRows.push_back({ "advanced:input:" + control.id,
					control.label, {} });
		}
		expressionRows.push_back({ "advanced:section:operators", "Operators", {}, false });
		for (const auto& [id, label, token] : std::array{
			std::tuple{ "or", "OR - either input", " | " },
			std::tuple{ "and", "AND - chord", " & " },
			std::tuple{ "xor", "XOR", " ^ " },
			std::tuple{ "logical-or", "Logical OR", " || " },
			std::tuple{ "logical-and", "Logical AND", " && " },
			std::tuple{ "not", "NOT", "!" },
			std::tuple{ "invert", "Digital invert", "~" },
			std::tuple{ "add", "Add", " + " },
			std::tuple{ "subtract", "Subtract", " - " },
			std::tuple{ "multiply", "Multiply", " * " },
			std::tuple{ "divide", "Divide", " / " },
			std::tuple{ "modulo", "Modulo", " % " },
			std::tuple{ "greater", "Greater than", " > " },
			std::tuple{ "less", "Less than", " < " },
			std::tuple{ "greater-equal", "Greater than or equal", " >= " },
			std::tuple{ "less-equal", "Less than or equal", " <= " },
			std::tuple{ "equal", "Equal", " == " },
			std::tuple{ "not-equal", "Not equal", " != " },
			std::tuple{ "conditional", "Conditional then", " ? " },
			std::tuple{ "conditional-else", "Conditional else", " : " },
			std::tuple{ "assign", "Assign variable", " = " },
			std::tuple{ "add-assign", "Add to variable", " += " },
			std::tuple{ "subtract-assign", "Subtract from variable", " -= " },
			std::tuple{ "multiply-assign", "Multiply variable", " *= " },
			std::tuple{ "divide-assign", "Divide variable", " /= " },
			std::tuple{ "open", "Open parenthesis", "(" },
			std::tuple{ "close", "Close parenthesis", ")" } })
		{
			expressionRows.push_back({ "advanced:operator:" + std::string(id), label, token });
		}
		expressionRows.push_back({ "advanced:section:values", "Values and variables", {}, false });
		for (const auto& [id, label, value] : std::array{
			std::tuple{ "zero", "Literal 0", "0" },
			std::tuple{ "half", "Literal 0.5", "0.5" },
			std::tuple{ "one", "Literal 1", "1" },
			std::tuple{ "negative-one", "Literal -1", "-1" },
			std::tuple{ "variable", "User variable", "$value" } })
		{
			expressionRows.push_back({ "advanced:value:" + std::string(id), label, value });
		}
		expressionRows.push_back({ "advanced:section:functions", "Functions", {}, false });
		for (const auto& [id, label] : std::array{
			std::pair{ "if", "If / then / else" }, std::pair{ "not", "Logical not" },
			std::pair{ "abs", "Absolute value" }, std::pair{ "sin", "Sine" },
			std::pair{ "cos", "Cosine" }, std::pair{ "tan", "Tangent" },
			std::pair{ "asin", "Arc sine" }, std::pair{ "acos", "Arc cosine" },
			std::pair{ "atan", "Arc tangent" }, std::pair{ "atan2", "Two-axis arc tangent" },
			std::pair{ "sqrt", "Square root" }, std::pair{ "pow", "Power" },
			std::pair{ "min", "Minimum" }, std::pair{ "max", "Maximum" },
			std::pair{ "clamp", "Clamp to -1..1" },
			std::pair{ "deadzone", "Deadzone 20%" }, std::pair{ "timer", "Held time" },
			std::pair{ "toggle", "Toggle" }, std::pair{ "smooth", "Smooth" },
			std::pair{ "hold", "Hold 500 ms" }, std::pair{ "tap", "Tap" },
			std::pair{ "relative", "Relative" }, std::pair{ "pulse", "Pulse" } })
			expressionRows.push_back({ "advanced:function:" + std::string(id), label, {} });
		expressionRows.push_back({ "advanced:save", "Apply expression", "Validate and stage" });
		expressionRows.push_back({ "advanced:cancel", "Cancel", "Discard advanced edits" });

		m_expressionRows.SetItemCount(expressionRows.size());
		auto moveExpression = [&](int direction) {
			for (std::size_t attempt = 0; attempt < expressionRows.size(); ++attempt)
			{
				m_expressionRows.Move(direction, expressionRows.size());
				if (m_expressionRows.selected < expressionRows.size() &&
					expressionRows[m_expressionRows.selected].enabled)
					break;
			}
		};
		if (input.up)
			moveExpression(-1);
		if (input.down)
			moveExpression(1);

		auto cycleExpressionDevice = [&](int direction) {
			if (m_devices.empty())
				return;
			auto current = std::ranges::find_if(m_devices,
				[this](const InputDeviceDescriptor& device) {
					return device.id == m_expressionDeviceId;
				});
			std::ptrdiff_t index = current == m_devices.end() ? 0 :
				std::distance(m_devices.begin(), current);
			index = (index + direction + static_cast<std::ptrdiff_t>(m_devices.size())) %
				static_cast<std::ptrdiff_t>(m_devices.size());
			m_expressionDeviceId = m_devices[static_cast<std::size_t>(index)].id;
			m_expressionRows.selected = 1;
		};
		if (m_expressionRows.selected < expressionRows.size() &&
			expressionRows[m_expressionRows.selected].id == "advanced:device")
		{
			if (input.left)
				cycleExpressionDevice(-1);
			if (input.right)
				cycleExpressionDevice(1);
		}

		const UnitRect expressionList{ editor.x + 24.0f, editor.y + 82.0f,
			editor.w - 48.0f, editor.h - 108.0f };
		const auto activatedExpression = DrawActionList(context, draw, expressionList,
			10.0f, m_expressionRows, expressionRows, font,
			{ .rowHeight = 46.0f, .fontSizeUnits = 16.0f,
				.defaultFooter = "A Insert / Select   Y Add OR   Menu Type   B Cancel" });
		const bool activateExpression = m_expressionRows.selected < expressionRows.size() &&
			expressionRows[m_expressionRows.selected].enabled &&
			((activatedExpression && *activatedExpression == m_expressionRows.selected) ||
				input.accept);
		const std::string selectedId = m_expressionRows.selected < expressionRows.size() ?
			expressionRows[m_expressionRows.selected].id : std::string{};
		auto setBuffer = [this](std::string value) {
			std::memset(m_expressionBuffer.data(), 0, m_expressionBuffer.size());
			std::memcpy(m_expressionBuffer.data(), value.data(),
				std::min(value.size(), m_expressionBuffer.size() - 1));
		};
		auto selectedReference = [&]() -> std::string {
			if (!selectedId.starts_with("advanced:input:"))
				return {};
			const std::string controlId = selectedId.substr(
				std::string_view("advanced:input:").size());
			return EscapeInputControlReference(m_expressionDeviceId == m_draft.defaultDeviceId ?
				controlId : m_expressionDeviceId + "/" + controlId);
		};
		if (input.alternate)
		{
			const std::string reference = selectedReference();
			if (!reference.empty())
			{
				const std::string current = m_expressionBuffer.data();
				setBuffer(current.empty() ? reference :
					"(" + current + ") | (" + reference + ")");
			}
		}
		if (input.menu || (activateExpression && selectedId == "advanced:edit"))
		{
			m_expressionTextEditorOpen = true;
			m_expressionEditorState.requestFocus = true;
			return result;
		}
		if (input.back || (activateExpression && selectedId == "advanced:cancel"))
		{
			m_expressionEditorOpen = false;
			return result;
		}
		if (activateExpression && selectedId == "advanced:device")
			cycleExpressionDevice(1);
		else if (activateExpression && selectedId.starts_with("advanced:input:"))
			setBuffer(selectedReference());
		else if (activateExpression && selectedId.starts_with("advanced:operator:"))
		{
			const std::string token = expressionRows[m_expressionRows.selected].detail;
			setBuffer(std::string(m_expressionBuffer.data()) + token);
		}
		else if (activateExpression && selectedId.starts_with("advanced:value:"))
		{
			const std::string value = expressionRows[m_expressionRows.selected].detail;
			setBuffer(std::string(m_expressionBuffer.data()) + value);
		}
		else if (activateExpression && selectedId.starts_with("advanced:function:"))
		{
			const std::string name = selectedId.substr(
				std::string_view("advanced:function:").size());
			const std::string current = m_expressionBuffer.data();
			const std::string value = current.empty() ? "0" : current;
			if (name == "if") setBuffer("if(" + value + ", 1, 0)");
			else if (name == "atan2") setBuffer("atan2(" + value + ", 1)");
			else if (name == "pow") setBuffer("pow(" + value + ", 2)");
			else if (name == "min") setBuffer("min(" + value + ", 0)");
			else if (name == "max") setBuffer("max(" + value + ", 0)");
			else if (name == "deadzone") setBuffer("deadzone(" + value + ", 0.2)");
			else if (name == "hold") setBuffer("hold(" + value + ", 0.5)");
			else if (name == "clamp") setBuffer("clamp(" + value + ", -1, 1)");
			else setBuffer(name + "(" + value + ")");
		}
		else if (activateExpression && selectedId == "advanced:save")
		{
			MappingExpression expression(m_expressionBuffer.data());
			if (!expression.IsValid())
				m_status = expression.Error();
			else
			{
				const auto mappingIndex = SelectedMappingIndex();
				if (!mappingIndex)
				{
					m_status = "The selected mapping is no longer available";
					return result;
				}
				m_draft.mappings[*mappingIndex].expression = m_expressionBuffer.data();
				m_draft.dirty = true;
				m_expressionEditorOpen = false;
				ValidateDraft();
				m_status = "Advanced expression staged";
			}
		}
		return result;
	}

	if (navigationAvailable && input.back)
	{
		if (m_profileDeleteConfirmation)
		{
			m_profileDeleteConfirmation = false;
			m_status = "Profile deletion cancelled";
			return result;
		}
		Close();
		result.cancelled = true;
	}
	return result;
}
}
