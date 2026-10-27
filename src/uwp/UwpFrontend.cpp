#include "UwpFrontend.h"

#include "CemuHost.h"
#include "UwpBuildConfig.h"
#include "UwpGraphicPackDownloader.h"
#include "UwpInputProvider.h"
#include "UwpInputMappingHost.h"
#include "UwpQuickMenuBinding.h"
#include "UwpCompanionService.h"
#include "UwpTranslation.h"
#include "UwpScreenScraperDownloader.h"
#include "UwpStorageService.h"
#include "UwpTitleInstaller.h"
#include "UwpVideoPlayer.h"
#include "UwpWindowSystem.h"

#include "UwpImGuiFrontend/ImGuiShell.h"
#include "UwpImGuiFrontend/InputMapper.h"
#include "UwpImGuiFrontend/MetadataEditor.h"
#include "UwpImGuiFrontend/Text.h"
#include "UwpImGuiFrontend/NativeTextInput.h"
#include "UwpImGuiFrontend/CredentialStore.h"
#include "UwpImGuiFrontend/Notifications.h"
#include "UwpImGuiFrontend/Pickers.h"
#include "UwpImGuiFrontend/ServicePanels.h"
#include "UwpImGuiFrontend/StorageLocations.h"
#include "UwpImGuiFrontend/Views.h"
#include "UwpImGuiFrontend/Widgets.h"

#include "Cafe/Account/Account.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/RendererShaderD3D12.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"
#include "Cafe/OS/libs/erreula/erreula.h"
#include "Cafe/TitleList/TitleId.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cemu/Logging/CemuLogging.h"
#include "audio/IAudioAPI.h"
#include "audio/IAudioInputAPI.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui/imgui_extension.h"
#include "input/api/Controller.h"
#include "input/InputManager.h"
#include "util/helpers/helpers.h"
#include "WindowSystem.h"

#include <png.h>
#include <wincodec.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <ctime>
#include <deque>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include "input/api/WGI/WGIGamepadController.h"

namespace
{
std::mutex s_frontendMutex;
std::string s_status = "Ready";
UwpImGuiFrontend::NotificationCenter s_notificationCenter;
bool s_pickerOpen = false;
bool s_frontendActive = true;
std::atomic_bool s_systemBackRequested{ false };
bool s_showActionHints = true;
float s_actionHintsTimer = 0.0f;
bool s_actionHintsSeen = false;
bool s_screenScraperCredentialBuffersLoaded = false;
bool s_screenScraperResumeChecked = false;
std::array<char, 128> s_screenScraperUserBuffer{};
std::array<char, 128> s_screenScraperPasswordBuffer{};
thread_local unsigned s_inputMappingReadDepth = 0;

constexpr float kActionHintsVisibleSeconds = 12.0f;
constexpr std::wstring_view kScreenScraperUserVaultResource = L"CemuUwp.ScreenScraper.User";

int ShellThemeCount();
const UwpImGuiFrontend::ShellTheme& CurrentShellTheme();

ImVec2 operator+(const ImVec2& lhs, const ImVec2& rhs)
{
	return { lhs.x + rhs.x, lhs.y + rhs.y };
}

ImVec2 operator*(const ImVec2& lhs, float rhs)
{
	return { lhs.x * rhs, lhs.y * rhs };
}

enum class Page
{
	Home,
	Settings,
	Controllers,
	GraphicPacks,
	TitleManager,
	Logs,
	About,
};

enum class TitleInstallDialogState
{
	Closed,
	Planning,
	Confirmation,
	Installing,
	Result,
};

enum class SettingsPage
{
	General,
	Graphics,
	Audio,
	Input,
	DsuStreaming,
	Overlay,
	Account,
	Debug,
	Storage,
	Screenscraper,
	Community,
};

enum class UwpScreenLayout : sint32
{
	TVOnly = 0,
	GamePadOnly,
	SideBySide,
	Stacked,
	PictureInPicture,
	GamePadPictureInPicture,
};

struct TitleTile
{
	uint64 titleId = 0;
	uint16 version = 0;
	uint64 uid = 0;
	std::string name;
	std::string type;
	std::string region;
	fs::path path;
	bool launchable = true;
	bool favorite = false;
};

Page s_page = Page::Home;
SettingsPage s_settingsPage = SettingsPage::General;
TitleInstallDialogState s_titleInstallDialogState = TitleInstallDialogState::Closed;
std::optional<CemuUwp::TitleInstaller::InstallPlan> s_titleInstallPlan;
std::future<CemuUwp::TitleInstaller::PlanResult> s_titleInstallPlanFuture;
CemuUwp::TitleInstaller::Snapshot s_titleInstallSnapshot{};
std::string s_titleInstallMessage;
bool s_titleInstallCancelConfirmation = false;
bool s_titleInstallCompletionHandled = false;
bool s_titleInstallDismissRequested = false;
bool s_titleInstallRequestDefaultFocus = false;
bool s_titleInstallPickerRequested = false;
bool s_titleInstallInputReleaseBarrier = false;
bool s_titleInstallBackConsumedUntilIdle = false;
int s_themeIndex = 0;
int s_homePage = 0;
int s_selectedTitle = 0;
int s_titleManagerSelection = 0;
bool s_titleManagerGamesDirectoryOpen = false;
std::optional<uint64> s_pendingCompanionSelectionToken;
int s_selectedGraphicPack = 0;
int s_graphicPackListRow = 0;
std::unordered_set<std::string> s_expandedGraphicPackGroups;
struct GraphicPackGroup
{
	std::string name;
	int firstPack{};
	int packCount{};
};
std::vector<GraphicPackGroup> s_packGroups;
size_t s_packGroupsBuiltForCount = (size_t)-1;
const void* s_packGroupsBuiltForData = nullptr;

const std::vector<GraphicPackGroup>& GetGraphicPackGroups()
{
	const auto& packs = GraphicPack2::GetGraphicPacks();
	if (s_packGroupsBuiltForCount != packs.size() || s_packGroupsBuiltForData != (const void*)packs.data())
	{
		s_packGroupsBuiltForData = (const void*)packs.data();
		s_packGroups.clear();
		for (int i = 0; i < (int)packs.size(); ++i)
		{
			const std::string& virtualPath = packs[i]->GetVirtualPath();
			std::string game = virtualPath.substr(0, virtualPath.find('/'));
			if (s_packGroups.empty() || s_packGroups.back().name != game)
				s_packGroups.push_back({ std::move(game), i, 1 });
			else
				s_packGroups.back().packCount++;
		}
		s_packGroupsBuiltForCount = packs.size();
	}
	return s_packGroups;
}

int GraphicPackGroupIndexForPack(int packIndex)
{
	const auto& groups = GetGraphicPackGroups();
	for (int g = 0; g < (int)groups.size(); ++g)
	{
		if (packIndex < groups[g].firstPack + groups[g].packCount)
			return g;
	}
	return groups.empty() ? 0 : (int)groups.size() - 1;
}

enum class GraphicPackListRowKind
{
	Reload,
	Download,
	Group,
	Pack,
};

struct GraphicPackListRow
{
	GraphicPackListRowKind kind{};
	int groupIndex = 0;
	int packIndex = -1;
};

void PruneGraphicPackExpansion(const std::vector<GraphicPackGroup>& groups)
{
	if (groups.empty())
	{
		s_expandedGraphicPackGroups.clear();
		return;
	}

	std::unordered_set<std::string> validNames;
	validNames.reserve(groups.size());
	for (const auto& group : groups)
		validNames.insert(group.name);

	for (auto it = s_expandedGraphicPackGroups.begin(); it != s_expandedGraphicPackGroups.end();)
	{
		if (validNames.find(*it) == validNames.end())
			it = s_expandedGraphicPackGroups.erase(it);
		else
			++it;
	}
}

bool IsGraphicPackGroupExpanded(const GraphicPackGroup& group)
{
	return s_expandedGraphicPackGroups.find(group.name) != s_expandedGraphicPackGroups.end();
}

void EnsureGraphicPackListState()
{
	const auto& packs = GraphicPack2::GetGraphicPacks();
	const auto& groups = GetGraphicPackGroups();
	if (packs.empty() || groups.empty())
	{
		s_selectedGraphicPack = 0;
		s_graphicPackListRow = 0;
		s_expandedGraphicPackGroups.clear();
		return;
	}

	s_selectedGraphicPack = std::clamp(s_selectedGraphicPack, 0, (int)packs.size() - 1);
	PruneGraphicPackExpansion(groups);
	const int selectedGroup = GraphicPackGroupIndexForPack(s_selectedGraphicPack);
	if (s_expandedGraphicPackGroups.empty() && selectedGroup >= 0 && selectedGroup < (int)groups.size())
		s_expandedGraphicPackGroups.insert(groups[selectedGroup].name);
}

std::vector<GraphicPackListRow> BuildGraphicPackListRows()
{
	EnsureGraphicPackListState();
	std::vector<GraphicPackListRow> rows;
	const auto& groups = GetGraphicPackGroups();
	const auto& packs = GraphicPack2::GetGraphicPacks();
	rows.reserve(groups.size() + packs.size() + 2);
	rows.push_back({ GraphicPackListRowKind::Reload, 0, -1 });
	rows.push_back({ GraphicPackListRowKind::Download, 0, -1 });

	for (int g = 0; g < (int)groups.size(); ++g)
	{
		const auto& group = groups[g];
		rows.push_back({ GraphicPackListRowKind::Group, g, group.firstPack });
		if (!IsGraphicPackGroupExpanded(group))
			continue;

		for (int i = group.firstPack; i < group.firstPack + group.packCount; ++i)
			rows.push_back({ GraphicPackListRowKind::Pack, g, i });
	}
	return rows;
}

int GraphicPackHeaderRowForGroup(int groupIndex)
{
	const auto rows = BuildGraphicPackListRows();
	for (int row = 0; row < (int)rows.size(); ++row)
	{
		if (rows[row].kind == GraphicPackListRowKind::Group && rows[row].groupIndex == groupIndex)
			return row;
	}
	return rows.empty() ? 0 : std::clamp(s_graphicPackListRow, 0, (int)rows.size() - 1);
}

void SelectGraphicPackListRow(int rowIndex)
{
	const auto rows = BuildGraphicPackListRows();
	const auto& groups = GetGraphicPackGroups();
	if (rows.empty() || groups.empty())
	{
		s_graphicPackListRow = 0;
		s_selectedGraphicPack = 0;
		return;
	}

	s_graphicPackListRow = std::clamp(rowIndex, 0, (int)rows.size() - 1);
	const auto& row = rows[s_graphicPackListRow];
	if (row.kind == GraphicPackListRowKind::Reload || row.kind == GraphicPackListRowKind::Download)
		return;
	const auto& group = groups[row.groupIndex];
	s_selectedGraphicPack = row.kind == GraphicPackListRowKind::Pack ? row.packIndex : group.firstPack;
}

void ResetGraphicPackListState()
{
	s_selectedGraphicPack = 0;
	s_expandedGraphicPackGroups.clear();
	EnsureGraphicPackListState();
	s_graphicPackListRow = GetGraphicPackGroups().empty() ? 0 : 2;
}
bool s_requestContentFocus = false;
bool s_showUsers = false;
bool s_usersDialogOpen = false;
bool s_usersDialogDismissRequested = false;
std::optional<int> s_pendingLaunchIndex;
bool s_quickMenuOpen = false;
int s_quickMenuSelection = 0;
bool s_quickMenuLayoutComboOpen = false;
int s_quickMenuLayoutSelection = 0;
int s_pendingQuickMenuActivation = -1;

enum class OverlayPaneFocus
{
	None,
	Left,
	Right,
};

OverlayPaneFocus s_pendingOverlayPaneFocus = OverlayPaneFocus::None;
OverlayPaneFocus s_activeOverlayPaneFocus = OverlayPaneFocus::None;

struct ButtonSnapshot
{
	bool a = false;
	bool b = false;
	bool x = false;
	bool y = false;
	bool up = false;
	bool down = false;
	bool left = false;
	bool right = false;
	bool lb = false;
	bool rb = false;
	bool menu = false;
	bool view = false;
	// stick clicks and triggers: only used by shortcut bindings
	bool ls = false;
	bool rs = false;
	bool lt = false;
	bool rt = false;
	float rightStickY = 0.0f;
	UwpQuickMenuBinding::Snapshot quickMenu{};
};

ButtonSnapshot s_previousButtons{};
ButtonSnapshot s_frameButtons{};
ButtonSnapshot s_framePreviousButtons{};
bool s_frameInputActive = false;
bool s_imguiOwnsNavigation = false;
bool s_nativeTextInputOwnedFrame = false;
constexpr int kHomeColumns = 5;
constexpr int kHomeRows = 3;
constexpr int kHomePageSize = kHomeColumns * kHomeRows;
constexpr int kQuickMenuItemCount = 9;
constexpr float kOverlayFontSizePx = 22.0f;

enum class FocusArea
{
	HomeGrid,
	LeftRail,
	RightRail,
	Content,
};

struct DirectionRepeatState
{
	bool down = false;
	std::chrono::steady_clock::time_point nextRepeat{};
	std::chrono::steady_clock::time_point releasedAt{};
};

struct RepeatCounters
{
	DirectionRepeatState up{};
	DirectionRepeatState down{};
	DirectionRepeatState left{};
	DirectionRepeatState right{};
};

enum class TitleContextAction
{
	Start,
	Favorite,
	EditMetadata,
	WikiPage,
	GameDirectory,
	SaveDirectory,
	UpdateDirectory,
	DlcDirectory,
	RemoveShaderCaches,
	EditGraphicPacks,
	EditGameProfile,
	ControllerMapping,
	CopyTitleName,
	CopyTitleId,
	CopyTitleImage,
	RefreshGames,
};

enum class TitleContextView
{
	Menu,
	EditMetadata,
	EditGameProfile,
	ControllerMapping,
	ConfirmCacheRemoval,
};

struct TitleContextEntry
{
	TitleContextAction action{};
	const char* label = "";
	bool separatorBefore = false;
};

constexpr std::array kTitleContextEntries{
	TitleContextEntry{ TitleContextAction::Start, "Start" },
	TitleContextEntry{ TitleContextAction::Favorite, "Favorite", true },
	TitleContextEntry{ TitleContextAction::EditMetadata, "Edit Metadata" },
	TitleContextEntry{ TitleContextAction::WikiPage, "Wiki page", true },
	TitleContextEntry{ TitleContextAction::GameDirectory, "Game directory" },
	TitleContextEntry{ TitleContextAction::SaveDirectory, "Save directory" },
	TitleContextEntry{ TitleContextAction::UpdateDirectory, "Update directory" },
	TitleContextEntry{ TitleContextAction::DlcDirectory, "DLC directory" },
	TitleContextEntry{ TitleContextAction::RemoveShaderCaches, "Remove shader caches", true },
	TitleContextEntry{ TitleContextAction::EditGraphicPacks, "Edit graphic packs", true },
	TitleContextEntry{ TitleContextAction::EditGameProfile, "Edit game profile" },
	TitleContextEntry{ TitleContextAction::ControllerMapping, "Controller Mapping" },
	TitleContextEntry{ TitleContextAction::CopyTitleName, "Copy Title Name", true },
	TitleContextEntry{ TitleContextAction::CopyTitleId, "Copy Title ID" },
	TitleContextEntry{ TitleContextAction::CopyTitleImage, "Copy Title Image" },
	TitleContextEntry{ TitleContextAction::RefreshGames, "Refresh game list", true },
};

enum class GameProfileSection
{
	General,
	Graphics,
	Controllers,
};

struct TitleGameProfileEditorState
{
	bool loadSharedLibraries = true;
	bool startWithPadView = false;
	int cpuMode = 3;
	int threadQuantum = 1;
	int graphicsApi = 0;
	int accurateShaderMul = 1;
	std::array<int, 8> controllerProfiles{};
	std::vector<std::string> controllerProfileNames;
	GameProfileSection section = GameProfileSection::General;
	int row = 0;
	float scroll = 0.0f;
	bool comboOpen = false;
	int comboSelection = 0;
	int comboScroll = 0;
};

struct TitleContextMenuState
{
	bool open = false;
	TitleContextView view = TitleContextView::Menu;
	TitleTile title{};
	uint64 baseTitleId = 0;
	std::string originalName;
	std::string wikiUri;
	std::vector<fs::path> shaderCachePaths;
	uint64 shaderCacheBytes = 0;
	fs::path gameDirectory;
	fs::path saveDirectory;
	fs::path updateDirectory;
	fs::path dlcDirectory;
	int selection = 0;
	int confirmSelection = 1;
	float scroll = 0.0f;
	bool hasSaveDirectory = false;
	bool hasUpdate = false;
	bool hasDlc = false;
	UwpImGuiFrontend::Metadata metadata;
	TitleGameProfileEditorState gameProfile{};
	RepeatCounters repeat{};
};

constexpr std::array<const char*, 3> kGameProfileSectionLabels{
	"General", "Graphics", "Controllers"
};
constexpr std::array<const char*, 4> kGameProfileCpuModeLabels{
	"Single-core interpreter", "Single-core recompiler", "Multi-core recompiler", "Auto (recommended)"
};
constexpr std::array<uint32, 5> kGameProfileThreadQuantumValues{
	20000, 45000, 60000, 80000, 100000
};
constexpr std::array<const char*, 2> kGameProfileGraphicsApiLabels{
	"Default", "Direct3D 12"
};
constexpr std::array<const char*, 2> kGameProfileShaderMulLabels{
	"False", "True"
};

FocusArea s_focusArea = FocusArea::HomeGrid;
int s_leftRailIndex = 0;
int s_rightRailIndex = 0;
RepeatCounters s_repeatCounters{};
RepeatCounters s_quickMenuRepeatCounters{};
TitleContextMenuState s_titleContextMenu{};
bool s_titleContextInputReleaseBarrier = false;
std::unique_ptr<UwpImGuiFrontend::MetadataEditor> s_metadataEditor;
UwpImGuiFrontend::InputMapperView s_inputMapper;
UwpImGuiFrontend::InputMapperView s_globalInputMapper;
CemuUwp::InputMappingHost s_inputMappingHost;
UwpImGuiFrontend::CompanionPanel s_companionPanel;
UwpImGuiFrontend::FrameInput s_mapperFrameInput{};
UwpImGuiFrontend::FrameInput s_globalMapperFrameInput{};
RepeatCounters s_globalMapperRepeatCounters{};
bool s_quickMenuMapperOpen = false;
std::optional<TitleContextAction> s_pendingTitleContextActivation;
std::optional<uint64> s_pendingContextLaunchToken;
float s_pageAppearTime = 10.0f;
int s_lastAnimatedHomePage = -1;

constexpr std::array kSettingsTabs{
	std::pair{ SettingsPage::General, "System" },
	std::pair{ SettingsPage::Graphics, "Graphics" },
	std::pair{ SettingsPage::Audio, "Audio" },
	std::pair{ SettingsPage::Input, "Input" },
	std::pair{ SettingsPage::DsuStreaming, "DSU/Streaming" },
	std::pair{ SettingsPage::Overlay, "Overlay" },
	std::pair{ SettingsPage::Account, "Account" },
	std::pair{ SettingsPage::Debug, "Debug" },
	std::pair{ SettingsPage::Storage, "Storage" },
	std::pair{ SettingsPage::Screenscraper, "Screenscraper" },
	std::pair{ SettingsPage::Community, "Community" },
};

template <typename T, size_t N>
int IndexOfTab(T value, const std::array<std::pair<T, const char*>, N>& tabs)
{
	for (int i = 0; i < (int)tabs.size(); ++i)
	{
		if (tabs[i].first == value)
			return i;
	}
	return 0;
}

template <typename T, size_t N>
T MoveTab(T value, const std::array<std::pair<T, const char*>, N>& tabs, int delta)
{
	const int index = std::clamp(IndexOfTab(value, tabs) + delta, 0, (int)tabs.size() - 1);
	return tabs[index].first;
}

int HomeSlotCount(size_t titleCount)
{
	return std::max(1, (int)titleCount);
}

int HomePageCount(size_t titleCount)
{
	const int slotCount = HomeSlotCount(titleCount);
	return std::max(1, (slotCount + kHomePageSize - 1) / kHomePageSize);
}

int HomePageFirstSlot(int page)
{
	return std::max(0, page) * kHomePageSize;
}

int HomePageEndSlot(size_t titleCount, int page)
{
	return std::min(HomeSlotCount(titleCount), HomePageFirstSlot(page) + kHomePageSize);
}

int ClampHomeSlotToPage(size_t titleCount, int page, int slot)
{
	const int first = HomePageFirstSlot(page);
	const int end = HomePageEndSlot(titleCount, page);
	return std::clamp(slot, first, std::max(first, end - 1));
}

void SetPickerOpen(bool open)
{
	std::lock_guard lock(s_frontendMutex);
	s_pickerOpen = open;
}

UwpImGuiFrontend::IPickerService& PickerService()
{
	static std::unique_ptr<UwpImGuiFrontend::IPickerService> service =
		UwpImGuiFrontend::CreatePickerService();
	return *service;
}

UwpImGuiFrontend::PickerCallbacks MakePickerCallbacks(
	std::function<void(std::unique_ptr<UwpImGuiFrontend::PickedItem>)> completed,
	std::function<void(std::string_view)> failed)
{
	return {
		.completed = std::move(completed),
		.failed = std::move(failed),
		.openStateChanged = [](bool open) { SetPickerOpen(open); },
	};
}

fs::path FrontendStatePath()
{
	return CemuUwp::StorageService::ConfigRoot() / "uwp_frontend_state.ini";
}

void SaveFrontendState()
{
	std::error_code ec;
	fs::create_directories(CemuUwp::StorageService::ConfigRoot(), ec);

	std::ofstream file(FrontendStatePath(), std::ios::trunc);
	if (!file)
		return;

	file << "theme=" << std::clamp(s_themeIndex, 0, ShellThemeCount() - 1) << '\n';
	file << "actionHintsSeen=" << (s_actionHintsSeen ? 1 : 0) << '\n';
}

void LoadFrontendState()
{
	s_themeIndex = 0;
	s_actionHintsSeen = false;

	std::ifstream file(FrontendStatePath());
	std::string line;
	while (std::getline(file, line))
	{
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = line.substr(0, eq);
		const std::string value = line.substr(eq + 1);
		if (key == "theme")
		{
			try
			{
				s_themeIndex = std::clamp(std::stoi(value), 0, ShellThemeCount() - 1);
			}
			catch (const std::exception&)
			{
				s_themeIndex = 0;
			}
		}
		else if (key == "actionHintsSeen")
		{
			s_actionHintsSeen = value == "1" || value == "true";
		}
	}

	s_showActionHints = !s_actionHintsSeen;
	s_actionHintsTimer = 0.0f;
	if (!s_actionHintsSeen)
	{
		s_actionHintsSeen = true;
		SaveFrontendState();
	}
}

bool ConsumeOverlayPaneFocus(OverlayPaneFocus focus)
{
	if (s_pendingOverlayPaneFocus != focus)
		return false;
	s_pendingOverlayPaneFocus = OverlayPaneFocus::None;
	s_activeOverlayPaneFocus = focus;
	return true;
}

void RequestOverlayPaneFocus(OverlayPaneFocus focus)
{
	s_activeOverlayPaneFocus = focus;
	s_pendingOverlayPaneFocus = focus;
}

void BeginRetainedInputFrame(const ButtonSnapshot& buttons)
{
	s_frameButtons = buttons;
	s_framePreviousButtons = s_previousButtons;
	s_frameInputActive = true;
	UwpImGuiFrontend::BeginWidgetFrame(CurrentShellTheme(), {
		.accept = buttons.a,
		.back = buttons.b,
		.up = buttons.up,
		.down = buttons.down,
		.left = buttons.left,
		.right = buttons.right,
		.leftShoulder = buttons.lb,
		.rightShoulder = buttons.rb,
	}, {
		.accept = s_previousButtons.a,
		.back = s_previousButtons.b,
		.up = s_previousButtons.up,
		.down = s_previousButtons.down,
		.left = s_previousButtons.left,
		.right = s_previousButtons.right,
		.leftShoulder = s_previousButtons.lb,
		.rightShoulder = s_previousButtons.rb,
	}, s_imguiOwnsNavigation);
}

void CommitRetainedInputFrame()
{
	if (s_frameInputActive)
		s_previousButtons = s_frameButtons;
	s_frameInputActive = false;
	UwpImGuiFrontend::EndWidgetFrame();
}

ImU32 Color(const ImVec4& color)
{
	return ImGui::ColorConvertFloat4ToU32(color);
}

ImVec4 WithAlpha(ImVec4 color, float alpha)
{
	color.w = alpha;
	return color;
}

using UwpImGuiFrontend::AddRoundedRectFilledPx;
using UwpImGuiFrontend::AddRoundedRectStrokePx;
using UwpImGuiFrontend::AnimatedFloat;
using UwpImGuiFrontend::ColorWithAlpha;
using UwpImGuiFrontend::CursorAnimation;
using UwpImGuiFrontend::DrawShellPanelPx;
using UwpImGuiFrontend::Ease;
using UwpImGuiFrontend::MakeShellLayout;
using UwpImGuiFrontend::NormalizeRoundedRectPx;
using UwpImGuiFrontend::RectMaxPx;
using UwpImGuiFrontend::RectMinPx;
using UwpImGuiFrontend::ShellEasing;
using UwpImGuiFrontend::ShellLayoutContext;
using UwpImGuiFrontend::ShellTheme;
using UwpImGuiFrontend::ShellThemeMediaMode;
using UwpImGuiFrontend::ShellThemes;
using UwpImGuiFrontend::ToPx;
using UwpImGuiFrontend::UnitFromPx;
using UwpImGuiFrontend::UnitPointFromPx;
using UwpImGuiFrontend::UnitRect;

int ShellThemeCount()
{
	return static_cast<int>(ShellThemes().size());
}

const ShellTheme& CurrentShellTheme()
{
	const auto& themes = ShellThemes();
	return themes[std::clamp(s_themeIndex, 0, static_cast<int>(themes.size()) - 1)];
}
struct DecodedImage
{
	std::vector<uint8> rgba;
	int width = 0;
	int height = 0;
};

struct ShellTexture
{
	ImTextureID id = nullptr;
	int width = 0;
	int height = 0;

	bool Valid() const { return id != nullptr && width > 0 && height > 0; }
	void Reset()
	{
		if (id && g_renderer)
			g_renderer->DeleteTexture(id);
		id = nullptr;
		width = height = 0;
	}
};

std::map<std::string, ShellTexture> s_frontendStaticTextures;
std::map<std::string, ShellTexture> s_frontendContentTextures;
std::unordered_map<std::string, uint64> s_contentTextureLastUse;
std::unordered_map<uint64, ShellTexture> s_titleIconTextures;
std::unordered_map<uint64, uint64> s_titleIconLastUse;
std::unordered_set<uint64> s_missingTitleIconTextures;
std::deque<TitleTile> s_titleIconDecodeQueue;
std::unordered_set<uint64> s_queuedTitleIconTextures;
struct TitleIconDecodeResult
{
	uint64 key = 0;
	uint64 generation = 0;
	bool ok = false;
	DecodedImage image;
};
struct StaticTextureDecodeResult
{
	std::string name;
	uint64 generation = 0;
	bool appAsset = true;
	bool ok = false;
	DecodedImage image;
};
struct StaticTextureDecodeRequest
{
	std::string name;
	fs::path path;
	bool appAsset = true;
	int maxDimension = 0;
	uint64 generation = 0;
};
struct TitleCacheBatchResult
{
	std::vector<TitleTile> titles;
	size_t nextIndex = 0;
	bool complete = true;
};
std::future<TitleIconDecodeResult> s_titleIconDecodeFuture;
std::future<StaticTextureDecodeResult> s_staticTextureDecodeFuture;
std::deque<StaticTextureDecodeRequest> s_staticTextureDecodeQueue;
std::unordered_set<std::string> s_queuedStaticTextures;
std::unordered_set<std::string> s_missingStaticTextures;
uint64 s_textureGeneration = 1;
uint64 s_textureUseCounter = 0;
constexpr size_t kMaximumContentTextureCount = 32;
constexpr uint64 kMaximumContentTextureBytes = 96ull * 1024ull * 1024ull;
constexpr size_t kMaximumTitleIconCount = 64;
std::future<TitleCacheBatchResult> s_titleCacheFuture;
std::vector<TitleTile> s_cachedTitles;
std::vector<TitleTile> s_cachedHomeTitles;
std::chrono::steady_clock::time_point s_lastTitleCacheUpdate{};
size_t s_titleCacheNextIndex = 0;
bool s_titleCacheComplete = false;
bool s_titleCacheDirty = true;
// a scan can start and finish between frames, so IsScanning alone misses it
std::atomic_bool s_titleListChanged{true};
uint64 s_titleListCallbackId = 0;
float s_frontendUptime = 0.0f;
CursorAnimation s_shellCursor;
AnimatedFloat s_titlePillX;
AnimatedFloat s_titlePillW;
AnimatedFloat s_titlePillReveal;
std::string s_titlePillText;
std::unordered_map<int, float> s_tileFocusAmount;

struct LaunchAnimationState
{
	bool active = false;
	bool launched = false;
	float timer = 0.0f;
	UnitRect from{};
	float tileSide = 0.0f;
	// artwork width / height
	float aspect = 1.0f;
	fs::path path;
	ImTextureID texture = nullptr;
};

LaunchAnimationState s_launchAnim;

fs::path FrontendAssetRoot()
{
	return CemuUwp::StorageService::InstalledContentRoot() / "resources";
}

uint64 TitleIconKey(const TitleTile& title)
{
	return title.titleId ? title.titleId : title.uid;
}

bool ShouldUseTitleFallbackIcon(const TitleTile& title)
{
	const uint64 key = TitleIconKey(title);
	return key == 0 || s_missingTitleIconTextures.contains(key);
}

TitleCacheBatchResult CollectTitleBatch(size_t startIndex, size_t launchableBudget);
bool TitleTileSortLess(const TitleTile& lhs, const TitleTile& rhs);
void ApplyTitleTileConfig(TitleTile& title);
fs::path TitleMediaRoot(uint64 titleId);
fs::path TitleMetadataRecordPath(uint64 titleId);
UwpImGuiFrontend::ScreenScraperArtwork ConfiguredSearchArtwork(bool detail);
CemuUwp::ScreenScraperDownloader::Request BuildScreenScraperRequestForTitle(
	uint64 titleId, uint64 matchedGameId, std::string name,
	std::string region, fs::path path);
UwpScreenScraperArtwork CurrentListArtwork();
UwpScreenScraperArtwork CurrentDetailArtwork();
std::optional<fs::path> ConfiguredArtworkPath(const TitleTile& title,
	UwpScreenScraperArtwork source);

void LimitDecodedImageSize(DecodedImage& image, int maxDimension)
{
	if (maxDimension <= 0 || image.width <= 0 || image.height <= 0 ||
		std::max(image.width, image.height) <= maxDimension)
	{
		return;
	}

	const double scale = static_cast<double>(maxDimension) /
		static_cast<double>(std::max(image.width, image.height));
	const int targetWidth = std::max(1, static_cast<int>(std::lround(image.width * scale)));
	const int targetHeight = std::max(1, static_cast<int>(std::lround(image.height * scale)));
	std::vector<uint8> resized(static_cast<size_t>(targetWidth) * targetHeight * 4);

	for (int y = 0; y < targetHeight; ++y)
	{
		const double sourceY = std::clamp(
			(static_cast<double>(y) + 0.5) * image.height / targetHeight - 0.5,
			0.0, static_cast<double>(image.height - 1));
		const int y0 = static_cast<int>(sourceY);
		const int y1 = std::min(y0 + 1, image.height - 1);
		const double fy = sourceY - y0;
		for (int x = 0; x < targetWidth; ++x)
		{
			const double sourceX = std::clamp(
				(static_cast<double>(x) + 0.5) * image.width / targetWidth - 0.5,
				0.0, static_cast<double>(image.width - 1));
			const int x0 = static_cast<int>(sourceX);
			const int x1 = std::min(x0 + 1, image.width - 1);
			const double fx = sourceX - x0;
			const size_t destination = (static_cast<size_t>(y) * targetWidth + x) * 4;
			const size_t topLeft = (static_cast<size_t>(y0) * image.width + x0) * 4;
			const size_t topRight = (static_cast<size_t>(y0) * image.width + x1) * 4;
			const size_t bottomLeft = (static_cast<size_t>(y1) * image.width + x0) * 4;
			const size_t bottomRight = (static_cast<size_t>(y1) * image.width + x1) * 4;
			for (size_t component = 0; component < 4; ++component)
			{
				const double top = image.rgba[topLeft + component] * (1.0 - fx) +
					image.rgba[topRight + component] * fx;
				const double bottom = image.rgba[bottomLeft + component] * (1.0 - fx) +
					image.rgba[bottomRight + component] * fx;
				resized[destination + component] = static_cast<uint8>(std::lround(
					top * (1.0 - fy) + bottom * fy));
			}
		}
	}

	image.rgba = std::move(resized);
	image.width = targetWidth;
	image.height = targetHeight;
}

bool DecodePngFile(const fs::path& path, DecodedImage& out, int maxDimension = 0)
{
	png_image image{};
	image.version = PNG_IMAGE_VERSION;
	const std::string pathUtf8 = _pathToUtf8(path);
	if (!png_image_begin_read_from_file(&image, pathUtf8.c_str()))
		return false;
	image.format = PNG_FORMAT_RGBA;
	out.width = (int)image.width;
	out.height = (int)image.height;
	out.rgba.resize(PNG_IMAGE_SIZE(image));
	const bool ok = png_image_finish_read(&image, nullptr, out.rgba.data(), 0, nullptr) != 0;
	png_image_free(&image);
	if (!ok)
	{
		out = {};
		return false;
	}
	LimitDecodedImageSize(out, maxDimension);
	return true;
}

bool DecodeWicFile(const fs::path& path, DecodedImage& out,
	int maxDimension = 0)
{
	winrt::com_ptr<IWICImagingFactory> factory;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
		CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()))))
	{
		return false;
	}

	winrt::com_ptr<IWICBitmapDecoder> decoder;
	if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr,
		GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put())))
	{
		return false;
	}
	winrt::com_ptr<IWICBitmapFrameDecode> frame;
	if (FAILED(decoder->GetFrame(0, frame.put())))
		return false;

	UINT sourceWidth = 0;
	UINT sourceHeight = 0;
	if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) ||
		sourceWidth == 0 || sourceHeight == 0)
	{
		return false;
	}

	UINT width = sourceWidth;
	UINT height = sourceHeight;
	winrt::com_ptr<IWICBitmapScaler> scaler;
	IWICBitmapSource* source = frame.get();
	if (maxDimension > 0 &&
		std::max(sourceWidth, sourceHeight) > static_cast<UINT>(maxDimension))
	{
		const double scale = static_cast<double>(maxDimension) /
			static_cast<double>(std::max(sourceWidth, sourceHeight));
		width = std::max<UINT>(1, static_cast<UINT>(std::lround(sourceWidth * scale)));
		height = std::max<UINT>(1, static_cast<UINT>(std::lround(sourceHeight * scale)));
		if (FAILED(factory->CreateBitmapScaler(scaler.put())) ||
			FAILED(scaler->Initialize(frame.get(), width, height,
				WICBitmapInterpolationModeFant)))
		{
			return false;
		}
		source = scaler.get();
	}

	winrt::com_ptr<IWICFormatConverter> converter;
	if (FAILED(factory->CreateFormatConverter(converter.put())) ||
		FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppRGBA,
			WICBitmapDitherTypeNone, nullptr, 0.0,
			WICBitmapPaletteTypeCustom)))
	{
		return false;
	}

	if (width > std::numeric_limits<UINT>::max() / 4)
		return false;
	const UINT stride = width * 4;
	const uint64 byteCount = static_cast<uint64>(stride) * height;
	if (byteCount > std::numeric_limits<UINT>::max() ||
		byteCount > std::numeric_limits<size_t>::max())
	{
		return false;
	}

	out.width = static_cast<int>(width);
	out.height = static_cast<int>(height);
	out.rgba.resize(static_cast<size_t>(byteCount));
	if (FAILED(converter->CopyPixels(nullptr, stride,
		static_cast<UINT>(byteCount), out.rgba.data())))
	{
		out = {};
		return false;
	}
	return true;
}

bool DecodeImageFile(const fs::path& path, DecodedImage& out,
	int maxDimension = 0)
{
	std::string extension = _pathToUtf8(path.extension());
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		});
	if (extension == ".png" && DecodePngFile(path, out, maxDimension))
		return true;
	return DecodeWicFile(path, out, maxDimension);
}

bool DecodeTgaBytes(const std::vector<uint8>& data, DecodedImage& out)
{
	if (data.size() < 18)
		return false;
	const uint8 imageType = data[2];
	if (imageType != 2 && imageType != 3)
		return false;
	const uint16 width = (uint16)data[12] | ((uint16)data[13] << 8);
	const uint16 height = (uint16)data[14] | ((uint16)data[15] << 8);
	const uint8 bpp = data[16];
	const uint8 bytesPerPixel = bpp / 8;
	if (width == 0 || height == 0 || (bytesPerPixel != 3 && bytesPerPixel != 4))
		return false;
	const size_t offset = 18u + data[0];
	const size_t imageBytes = (size_t)width * height * bytesPerPixel;
	if (offset + imageBytes > data.size())
		return false;
	out.width = width;
	out.height = height;
	out.rgba.resize((size_t)width * height * 4);
	const bool topOrigin = (data[17] & 0x20) != 0;
	for (uint32 y = 0; y < height; ++y)
	{
		const uint32 srcY = topOrigin ? y : (height - 1 - y);
		for (uint32 x = 0; x < width; ++x)
		{
			const size_t src = offset + ((size_t)srcY * width + x) * bytesPerPixel;
			const size_t dst = ((size_t)y * width + x) * 4;
			if (bytesPerPixel == 3)
			{
				out.rgba[dst + 0] = data[src + 2];
				out.rgba[dst + 1] = data[src + 1];
				out.rgba[dst + 2] = data[src + 0];
				out.rgba[dst + 3] = 0xFF;
			}
			else
			{
				out.rgba[dst + 0] = data[src + 2];
				out.rgba[dst + 1] = data[src + 1];
				out.rgba[dst + 2] = data[src + 0];
				out.rgba[dst + 3] = data[src + 3];
			}
		}
	}
	return true;
}

bool UploadTexture(const DecodedImage& decoded, ShellTexture& texture)
{
	if (!g_renderer || decoded.width <= 0 || decoded.height <= 0 || decoded.rgba.empty())
		return false;
	texture.Reset();
	texture.id = g_renderer->GenerateTexture(decoded.rgba, { decoded.width, decoded.height });
	if (!texture.id)
		return false;
	texture.width = decoded.width;
	texture.height = decoded.height;
	return true;
}

ShellTexture* LoadStaticTexture(std::string name, const fs::path& relative)
{
	auto [it, inserted] = s_frontendStaticTextures.try_emplace(std::move(name));
	if (it->second.Valid() || !inserted)
		return it->second.Valid() ? &it->second : nullptr;
	DecodedImage image;
	if (!DecodeImageFile(FrontendAssetRoot() / relative, image))
		return nullptr;
	if (!UploadTexture(image, it->second))
		return nullptr;
	return &it->second;
}

void QueueStaticTexture(std::string name, fs::path path, bool appAsset = true)
{
	if (name.empty() || s_missingStaticTextures.contains(name) || s_queuedStaticTextures.contains(name))
		return;
	auto& textures = appAsset ? s_frontendStaticTextures : s_frontendContentTextures;
	auto [it, inserted] = textures.try_emplace(name);
	if (it->second.Valid())
		return;

	int maxDimension = 0;
	if (!appAsset)
	{
		if (name.starts_with("boxart:"))
			maxDimension = 512;
		else if (name.starts_with("dynamic-background:"))
			maxDimension = 1920;
		else
			maxDimension = 1024;
	}
	s_queuedStaticTextures.emplace(name);
	s_staticTextureDecodeQueue.push_back({
		std::move(name), std::move(path), appAsset, maxDimension, s_textureGeneration });
}

ShellTexture* StaticTexture(std::string name, fs::path relative)
{
	auto it = s_frontendStaticTextures.find(name);
	if (it != s_frontendStaticTextures.end() && it->second.Valid())
		return &it->second;
	QueueStaticTexture(std::move(name), std::move(relative), true);
	return nullptr;
}

std::string TextureKeyForFile(std::string_view prefix, const fs::path& path)
{
	std::error_code ec;
	const auto size = fs::file_size(path, ec);
	const auto stamp = fs::last_write_time(path, ec).time_since_epoch().count();
	return fmt::format("{}:{}:{}:{}", prefix, _pathToUtf8(path), size, stamp);
}

ShellTexture* FileTexture(std::string_view prefix, const fs::path& path)
{
	std::error_code ec;
	if (path.empty() || !fs::exists(path, ec) || fs::is_directory(path, ec))
		return nullptr;
	const std::string name = TextureKeyForFile(prefix, path);
	auto it = s_frontendContentTextures.find(name);
	if (it != s_frontendContentTextures.end() && it->second.Valid())
	{
		s_contentTextureLastUse[name] = ++s_textureUseCounter;
		return &it->second;
	}
	QueueStaticTexture(name, path, false);
	return nullptr;
}

uint64 FrontendTextureBytes(const ShellTexture& texture)
{
	return texture.Valid() ? static_cast<uint64>(texture.width) * texture.height * 4ull : 0;
}

void TrimContentTextureCache(std::string_view protectedName = {})
{
	uint64 residentBytes = 0;
	size_t residentTextures = 0;
	for (const auto& [_, texture] : s_frontendContentTextures)
	{
		if (!texture.Valid())
			continue;
		residentBytes += FrontendTextureBytes(texture);
		residentTextures++;
	}

	while (residentTextures > kMaximumContentTextureCount || residentBytes > kMaximumContentTextureBytes)
	{
		auto oldest = s_frontendContentTextures.end();
		uint64 oldestUse = std::numeric_limits<uint64>::max();
		for (auto it = s_frontendContentTextures.begin(); it != s_frontendContentTextures.end(); ++it)
		{
			if (!it->second.Valid() || it->first == protectedName)
				continue;
			const auto useIt = s_contentTextureLastUse.find(it->first);
			const uint64 lastUse = useIt != s_contentTextureLastUse.end() ? useIt->second : 0;
			if (lastUse < oldestUse)
			{
				oldestUse = lastUse;
				oldest = it;
			}
		}
		if (oldest == s_frontendContentTextures.end())
			break;

		residentBytes -= FrontendTextureBytes(oldest->second);
		residentTextures--;
		oldest->second.Reset();
		s_contentTextureLastUse.erase(oldest->first);
		s_frontendContentTextures.erase(oldest);
	}
}

void ServiceStaticTextureDecodeQueue()
{
	if (s_staticTextureDecodeFuture.valid())
	{
		if (s_staticTextureDecodeFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return;

		StaticTextureDecodeResult result = s_staticTextureDecodeFuture.get();
		if (result.generation != s_textureGeneration)
		{
			// A title launch invalidated this asynchronous frontend decode.
		}
		else if (result.ok)
		{
			s_queuedStaticTextures.erase(result.name);
			auto& textures = result.appAsset ? s_frontendStaticTextures : s_frontendContentTextures;
			auto& texture = textures[result.name];
			if (!UploadTexture(result.image, texture))
				s_missingStaticTextures.emplace(result.name);
			else if (!result.appAsset)
			{
				s_contentTextureLastUse[result.name] = ++s_textureUseCounter;
				TrimContentTextureCache(result.name);
			}
		}
		else
		{
			s_queuedStaticTextures.erase(result.name);
			s_missingStaticTextures.emplace(result.name);
		}
	}

	if (s_frontendUptime < 0.10f || s_staticTextureDecodeQueue.empty())
		return;

	auto request = std::move(s_staticTextureDecodeQueue.front());
	s_staticTextureDecodeQueue.pop_front();
	if (s_missingStaticTextures.contains(request.name))
		return;

	s_staticTextureDecodeFuture = std::async(std::launch::async, [request = std::move(request)]() mutable {
		StaticTextureDecodeResult result{};
		result.name = std::move(request.name);
		result.generation = request.generation;
		result.appAsset = request.appAsset;
		const fs::path path = request.appAsset ? FrontendAssetRoot() / request.path : request.path;
		result.ok = DecodeImageFile(path, result.image, request.maxDimension);
		return result;
	});
}

std::optional<std::vector<uint8>> ExtractTitleIconTga(const TitleTile& title)
{
	TitleInfo titleInfo;
	if (!CafeTitleList::GetFirstByTitleId(title.titleId, titleInfo))
		return std::nullopt;
	const std::string mountPath = TitleInfo::GetUniqueTempMountingPath();
	if (!titleInfo.Mount(mountPath, "", FSC_PRIORITY_BASE))
		return std::nullopt;
	auto tgaData = fsc_extractFile(fmt::format("{}/meta/iconTex.tga", mountPath).c_str());
	if (!tgaData)
	{
		tgaData = fsc_extractFile(fmt::format("{}/meta/iconTex.tga.gz", mountPath).c_str());
		if (tgaData)
			tgaData = zlibDecompress(*tgaData, 70 * 1024);
	}
	titleInfo.Unmount(mountPath);
	return tgaData;
}

ShellTexture* GetTitleIconTexture(const TitleTile& title)
{
	const uint64 key = TitleIconKey(title);
	if (key == 0 || s_missingTitleIconTextures.contains(key))
		return nullptr;
	if (auto it = s_titleIconTextures.find(key); it != s_titleIconTextures.end())
	{
		s_titleIconLastUse[key] = ++s_textureUseCounter;
		return it->second.Valid() ? &it->second : nullptr;
	}

	if (s_frontendUptime < 0.75f || s_queuedTitleIconTextures.contains(key))
		return nullptr;

	s_titleIconDecodeQueue.emplace_back(title);
	s_queuedTitleIconTextures.emplace(key);
	return nullptr;
}

TitleIconDecodeResult DecodeTitleIconForUpload(TitleTile title, uint64 generation)
{
	TitleIconDecodeResult result{};
	result.key = TitleIconKey(title);
	result.generation = generation;
	auto tgaData = ExtractTitleIconTga(title);
	if (!tgaData || result.key == 0)
		return result;
	result.ok = DecodeTgaBytes(*tgaData, result.image);
	return result;
}

void ServiceTitleIconDecodeQueue()
{
	if (s_titleIconDecodeFuture.valid())
	{
		if (s_titleIconDecodeFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return;

		TitleIconDecodeResult result = s_titleIconDecodeFuture.get();
		if (result.generation != s_textureGeneration)
		{
			// A title launch invalidated this asynchronous frontend decode.
		}
		else if (result.key != 0 && result.ok)
		{
			s_queuedTitleIconTextures.erase(result.key);
			auto& texture = s_titleIconTextures[result.key];
			if (!UploadTexture(result.image, texture))
				s_missingTitleIconTextures.emplace(result.key);
			else
			{
				s_titleIconLastUse[result.key] = ++s_textureUseCounter;
				while (s_titleIconTextures.size() > kMaximumTitleIconCount)
				{
					auto oldest = s_titleIconTextures.end();
					uint64 oldestUse = std::numeric_limits<uint64>::max();
					for (auto it = s_titleIconTextures.begin(); it != s_titleIconTextures.end(); ++it)
					{
						if (it->first == result.key)
							continue;
						const auto useIt = s_titleIconLastUse.find(it->first);
						const uint64 lastUse = useIt != s_titleIconLastUse.end() ? useIt->second : 0;
						if (lastUse < oldestUse)
						{
							oldestUse = lastUse;
							oldest = it;
						}
					}
					if (oldest == s_titleIconTextures.end())
						break;
					oldest->second.Reset();
					s_titleIconLastUse.erase(oldest->first);
					s_titleIconTextures.erase(oldest);
				}
			}
		}
		else if (result.key != 0)
		{
			s_queuedTitleIconTextures.erase(result.key);
			s_missingTitleIconTextures.emplace(result.key);
		}
	}

	if (s_frontendUptime < 0.75f || s_titleIconDecodeQueue.empty())
		return;

	TitleTile title = std::move(s_titleIconDecodeQueue.front());
	s_titleIconDecodeQueue.pop_front();
	const uint64 key = TitleIconKey(title);
	if (key == 0 || s_missingTitleIconTextures.contains(key) || s_titleIconTextures.contains(key))
	{
		s_queuedTitleIconTextures.erase(key);
		return;
	}

	const uint64 generation = s_textureGeneration;
	s_titleIconDecodeFuture = std::async(std::launch::async, [title = std::move(title), generation]() mutable {
		return DecodeTitleIconForUpload(std::move(title), generation);
	});
}

void PreloadStaticFrontendTextures()
{
	QueueStaticTexture("cemu", "icons/cemu.png");
	QueueStaticTexture("gamecard", "icons/gamecard.png");
	QueueStaticTexture("home", "icons/home.png");
	QueueStaticTexture("title_manager", "icons/title_manager.png");
	QueueStaticTexture("settings", "icons/settings.png");
	QueueStaticTexture("controllers", "icons/controllers.png");
	QueueStaticTexture("power", "icons/power.png");
	QueueStaticTexture("graphics", "icons/graphics.png");
	QueueStaticTexture("controller-preview:wii-u-gamepad",
		"controller-previews/wii-u-gamepad.png");
	QueueStaticTexture("controller-preview:wii-u-pro-controller",
		"controller-previews/wii-u-pro-controller.png");
	QueueStaticTexture("controller-preview:wii-remote",
		"controller-previews/wii-remote.png");
}

void MarkTitleCacheDirty()
{
	s_titleCacheDirty = true;
}

constexpr size_t kInitialTitleCacheBatchSize = 12;
constexpr size_t kTitleCacheBatchSize = 6;

void SortTitleCache()
{
	std::sort(s_cachedTitles.begin(), s_cachedTitles.end(), TitleTileSortLess);
}

void RebuildHomeTitleCache()
{
	s_cachedHomeTitles.clear();
	s_cachedHomeTitles.reserve(s_cachedTitles.size());
	for (const auto& title : s_cachedTitles)
	{
		if (title.launchable)
			s_cachedHomeTitles.emplace_back(title);
	}
}

void StartTitleCacheBatch(size_t startIndex, size_t batchSize)
{
	s_titleCacheFuture = std::async(std::launch::async, [startIndex, batchSize]() {
		return CollectTitleBatch(startIndex, batchSize);
	});
}

void StartTitleCacheRefresh()
{
	s_cachedTitles.clear();
	s_cachedHomeTitles.clear();
	s_titleCacheNextIndex = 0;
	s_titleCacheComplete = false;
	s_titleCacheDirty = false;
	StartTitleCacheBatch(0, kInitialTitleCacheBatchSize);
}

void AppendTitleCacheBatch(TitleCacheBatchResult&& result)
{
	if (!result.titles.empty())
	{
		// Keep CemuConfig access on the frontend thread while title discovery runs.
		for (auto& title : result.titles)
			ApplyTitleTileConfig(title);
		s_cachedTitles.insert(s_cachedTitles.end(),
							  std::make_move_iterator(result.titles.begin()),
							  std::make_move_iterator(result.titles.end()));
		SortTitleCache();
		RebuildHomeTitleCache();
	}

	s_titleCacheNextIndex = result.nextIndex;
	s_titleCacheComplete = result.complete;
	s_lastTitleCacheUpdate = std::chrono::steady_clock::now();
}

void ServiceTitleCache()
{
	const bool scanning = CafeTitleList::IsScanning();
	if (!scanning && s_titleListChanged.exchange(false, std::memory_order_acq_rel))
		s_titleCacheDirty = true;

	if (s_titleCacheFuture.valid())
	{
		if (s_titleCacheFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			TitleCacheBatchResult result = s_titleCacheFuture.get();
			if (s_titleCacheDirty)
			{
				if (!scanning)
					StartTitleCacheRefresh();
				return;
			}
			AppendTitleCacheBatch(std::move(result));
			if (!s_titleCacheComplete && !scanning)
				StartTitleCacheBatch(s_titleCacheNextIndex, kTitleCacheBatchSize);
		}
		return;
	}

	// scan completion marks s_titleListChanged, which triggers a full refresh
	if (scanning)
		return;
	const auto now = std::chrono::steady_clock::now();
	const bool hasNeverUpdated = s_lastTitleCacheUpdate.time_since_epoch().count() == 0;
	const bool stale = !hasNeverUpdated && s_cachedTitles.empty() && now - s_lastTitleCacheUpdate > std::chrono::seconds(3);
	if (s_titleCacheDirty || stale || hasNeverUpdated)
		StartTitleCacheRefresh();
	else if (!s_titleCacheComplete)
		StartTitleCacheBatch(s_titleCacheNextIndex, kTitleCacheBatchSize);
}

const std::vector<TitleTile>& GetCachedTitles()
{
	ServiceTitleCache();
	return s_cachedTitles;
}

const std::vector<TitleTile>& GetCachedHomeTitles()
{
	ServiceTitleCache();
	return s_cachedHomeTitles;
}

void ReleaseFrontendContentTextures()
{
	if (++s_textureGeneration == 0)
		s_textureGeneration = 1;
	s_staticTextureDecodeQueue.clear();
	s_titleIconDecodeQueue.clear();
	s_queuedStaticTextures.clear();
	s_queuedTitleIconTextures.clear();
	for (auto& [_, texture] : s_frontendContentTextures)
		texture.Reset();
	s_frontendContentTextures.clear();
	s_contentTextureLastUse.clear();
	for (auto& [_, texture] : s_titleIconTextures)
		texture.Reset();
	s_titleIconTextures.clear();
	s_titleIconLastUse.clear();
	s_missingTitleIconTextures.clear();
	s_missingStaticTextures.clear();
}

void ReleaseFrontendTextures()
{
	ReleaseFrontendContentTextures();
	for (auto& [_, texture] : s_frontendStaticTextures)
		texture.Reset();
	s_frontendStaticTextures.clear();
}

ImFont* ShellFont(float sizeUnits, const ShellLayoutContext& ctx)
{
	return ImGui_GetFont(std::max(10.0f, std::round(sizeUnits * ctx.uiScale)));
}

ImFont* ShellDrawFont(const ShellLayoutContext& ctx)
{
	return ShellFont(24.0f, ctx);
}

ImVec2 CalcTextUnits(const ShellLayoutContext& ctx, float sizeUnits, const std::string& text)
{
	ImFont* font = ShellDrawFont(ctx);
	if (!font)
		return ImGui::CalcTextSize(text.c_str()) * (1.0f / ctx.uiScale);
	const ImVec2 px = font->CalcTextSizeA(sizeUnits * ctx.uiScale, FLT_MAX, 0.0f, text.c_str());
	return { px.x / ctx.uiScale, px.y / ctx.uiScale };
}

void DrawTextUnits(ImDrawList* draw, const ShellLayoutContext& ctx, float sizeUnits, ImVec2 posUnits, ImU32 color, const char* text, const char* textEnd = nullptr)
{
	ImFont* font = ShellDrawFont(ctx);
	if (font)
		draw->AddText(font, sizeUnits * ctx.uiScale, ToPx(ctx, posUnits), color, text, textEnd);
	else
		draw->AddText(ToPx(ctx, posUnits), color, text, textEnd);
}

void DrawGlassPanel(ImDrawList* draw, const ShellLayoutContext& ctx, const UnitRect& rect, float radius, float opacity = 1.0f)
{
	DrawShellPanelPx(draw, RectMinPx(ctx, rect), RectMaxPx(ctx, rect), CurrentShellTheme(), radius * ctx.uiScale, opacity);
}

ImVec2 ResolveChildSize(ImVec2 requested)
{
	return UwpImGuiFrontend::ResolveChildSize(requested);
}

bool BeginShellPane(const char* id, ImVec2 size, ImGuiWindowFlags flags, float radius = 18.0f, float opacity = 0.74f, ImVec2 padding = ImVec2(22.0f, 20.0f))
{
	return UwpImGuiFrontend::BeginShellPane(id, size, flags, radius, opacity, padding);
}

void EndShellPane()
{
	UwpImGuiFrontend::EndShellPane();
}

void DrawReplicaFocus(ImDrawList* draw, ImVec2 min, ImVec2 max, float radius, bool focused, bool hovered)
{
	UwpImGuiFrontend::DrawReplicaFocus(draw, min, max, radius, focused, hovered);
}

void EraseLastUtf8CodePoint(std::string& text)
{
	if (text.empty())
		return;

	size_t start = text.size() - 1;
	while (start > 0 && (static_cast<uint8>(text[start]) & 0xC0) == 0x80)
		--start;
	text.erase(start);
}

void DrawClippedTextPx(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 color, const char* text, bool center = false)
{
	UwpImGuiFrontend::DrawClippedText(draw, min, max, color, text, center);
}

bool ShellButton(const char* label, ImVec2 size = ImVec2(0.0f, 46.0f))
{
	return UwpImGuiFrontend::ShellButton(label, size);
}

bool ShellSelectableWithId(const char* id, const char* label, bool selected, ImVec2 size = ImVec2(0.0f, 46.0f))
{
	return UwpImGuiFrontend::ShellSelectableWithId(id, label, selected, size);
}

bool ShellSelectable(const char* label, bool selected, ImVec2 size = ImVec2(0.0f, 46.0f))
{
	return UwpImGuiFrontend::ShellSelectable(label, selected, size);
}

bool ShellCheckbox(const char* label, bool* value)
{
	return UwpImGuiFrontend::ShellCheckbox(label, value);
}

bool ShellSliderFloat(const char* label, float* value, float minValue, float maxValue, const char* format)
{
	return UwpImGuiFrontend::ShellSliderFloat(label, value, minValue, maxValue, format);
}

bool ShellSliderInt(const char* label, int* value, int minValue, int maxValue, const char* format)
{
	return UwpImGuiFrontend::ShellSliderInt(label, value, minValue, maxValue, format);
}

// caches translated interface text so ImGui keeps a stable pointer
const char* Tr(const char* text)
{
	static std::unordered_map<std::string, std::string> cache;
	static uint32_t generation = 0;
	const uint32_t current = CemuUwp::Translation::GetGeneration();
	if (current != generation)
	{
		cache.clear();
		generation = current;
	}
	auto entry = cache.find(text);
	if (entry == cache.end())
		entry = cache.emplace(text, CemuUwp::Translation::Translate(text)).first;
	return entry->second.c_str();
}

template <typename... TArgs>
std::string TrFormat(const char* text, TArgs&&... args)
{
	return fmt::format(fmt::runtime(Tr(text)), std::forward<TArgs>(args)...);
}

std::string MetadataLanguage()
{
	std::string language = GetConfig().uwp_screenscraper.metadata_language.GetValue();
	if (language.empty())
		language = CemuUwp::Translation::GetLanguage();
	return language.empty() ? "en" : language;
}

std::wstring Widen(std::string_view text)
{
	try
	{
		return boost::nowide::widen(std::string(text));
	}
	catch (...)
	{
		return {};
	}
}

std::string Narrow(std::wstring_view text)
{
	try
	{
		return boost::nowide::narrow(std::wstring(text));
	}
	catch (...)
	{
		return {};
	}
}

std::string LowerAscii(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return (char)std::tolower(c);
	});
	return value;
}

std::string LocalTimeText()
{
	std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_s(&local, &now);
	char buffer[32]{};
	std::strftime(buffer, sizeof(buffer), "%H:%M", &local);
	return buffer;
}

std::string LocalDateText()
{
	std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_s(&local, &now);
	char buffer[32]{};
	std::strftime(buffer, sizeof(buffer), "%d/%m/%Y", &local);
	return buffer;
}

std::string RegionName(CafeConsoleRegion region)
{
	switch (region)
	{
	case CafeConsoleRegion::JPN:
		return "Japan";
	case CafeConsoleRegion::USA:
		return "USA";
	case CafeConsoleRegion::EUR:
		return "Europe";
	case CafeConsoleRegion::AUS_DEPR:
		return "Australia";
	case CafeConsoleRegion::CHN:
		return "China";
	case CafeConsoleRegion::KOR:
		return "Korea";
	case CafeConsoleRegion::TWN:
		return "Taiwan";
	case CafeConsoleRegion::Auto:
		return "Auto";
	default:
		return "Mixed";
	}
}

std::string TitleTypeName(TitleIdParser::TITLE_TYPE type)
{
	switch (type)
	{
	case TitleIdParser::TITLE_TYPE::BASE_TITLE:
		return "Game";
	case TitleIdParser::TITLE_TYPE::BASE_TITLE_DEMO:
		return "Demo";
	case TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE:
		return "Update";
	case TitleIdParser::TITLE_TYPE::AOC:
		return "DLC";
	case TitleIdParser::TITLE_TYPE::HOMEBREW:
		return "Homebrew";
	case TitleIdParser::TITLE_TYPE::SYSTEM_TITLE:
	case TitleIdParser::TITLE_TYPE::SYSTEM_DATA:
	case TitleIdParser::TITLE_TYPE::SYSTEM_OVERLAY_TITLE:
		return "System";
	default:
		return "Unknown";
	}
}

bool IsLaunchableTitle(TitleIdParser::TITLE_TYPE type)
{
	return type == TitleIdParser::TITLE_TYPE::BASE_TITLE ||
		type == TitleIdParser::TITLE_TYPE::BASE_TITLE_DEMO ||
		type == TitleIdParser::TITLE_TYPE::HOMEBREW;
}

std::optional<TitleTile> MakeTitleTile(TitleInfo* titleInfo)
{
	if (!titleInfo || !titleInfo->IsValid() || titleInfo->IsSystemDataTitle())
		return std::nullopt;

	const uint64 titleId = titleInfo->GetAppTitleId();
	const auto type = TitleIdParser(titleId).GetType();
	TitleTile tile;
	tile.titleId = titleId;
	tile.version = titleInfo->GetAppTitleVersion();
	tile.uid = titleInfo->GetUID();
	tile.name = titleInfo->GetMetaTitleName();
	if (tile.name.empty())
		tile.name = fmt::format("{:016x}", titleId);
	tile.type = TitleTypeName(type);
	tile.region = RegionName(titleInfo->GetMetaRegion());
	tile.path = titleInfo->GetPath();
	tile.launchable = IsLaunchableTitle(type);
	return tile;
}

void ApplyTitleTileConfig(TitleTile& tile)
{
	std::string customName;
	if (GetConfig().GetGameListCustomName(tile.titleId, customName) && !customName.empty())
		tile.name = std::move(customName);
	if (const auto metadata = UwpImGuiFrontend::LoadGameMetadataRecord(
		TitleMetadataRecordPath(tile.titleId)); metadata &&
		(metadata->itemId == 0 || metadata->itemId == tile.titleId) &&
		!metadata->title.empty())
	{
		tile.name = metadata->title;
	}
	tile.favorite = GetConfig().IsGameListFavorite(tile.titleId);
}

bool TitleTileSortLess(const TitleTile& lhs, const TitleTile& rhs)
{
	if (lhs.launchable != rhs.launchable)
		return lhs.launchable > rhs.launchable;
	if (lhs.favorite != rhs.favorite)
		return lhs.favorite > rhs.favorite;
	return LowerAscii(lhs.name) < LowerAscii(rhs.name);
}

TitleCacheBatchResult CollectTitleBatch(size_t startIndex, size_t launchableBudget)
{
	TitleCacheBatchResult result{};
	auto list = CafeTitleList::AcquireInternalList();
	const size_t listSize = list.size();
	result.titles.reserve(std::min<size_t>(launchableBudget + 4, std::max<size_t>(1, listSize - std::min(startIndex, listSize))));
	size_t launchableCount = 0;
	size_t i = std::min(startIndex, listSize);
	for (; i < listSize; ++i)
	{
		auto tile = MakeTitleTile(list[i]);
		if (!tile)
			continue;

		if (tile->launchable)
			launchableCount++;
		result.titles.emplace_back(std::move(*tile));
		if (launchableBudget != 0 && launchableCount >= launchableBudget)
		{
			++i;
			break;
		}
	}
	CafeTitleList::ReleaseInternalList();

	result.nextIndex = i;
	result.complete = i >= listSize;
	return result;
}

ButtonSnapshot ReadButtons()
{
	ButtonSnapshot result{};
	result.quickMenu.binding = UwpQuickMenuBinding::EffectiveMask(GetConfig().uwp_display.quick_menu_binding.GetValue());
	using winrt::Windows::System::VirtualKey;
	const auto virtualKeyDown = [](VirtualKey key) {
		return WindowSystem::IsKeyDown(static_cast<uint32>(key));
	};
	const auto gamepads = winrt::Windows::Gaming::Input::Gamepad::Gamepads();
	const bool hasDirectGamepad = gamepads.Size() > 0;
	if (hasDirectGamepad)
	{
		using winrt::Windows::Gaming::Input::GamepadButtons;
		for (uint32 i = 0; i < gamepads.Size(); ++i)
		{
			const auto reading = gamepads.GetAt(i).GetCurrentReading();
			const auto down = [&](GamepadButtons button) {
				return (static_cast<uint32>(reading.Buttons) & static_cast<uint32>(button)) != 0;
			};
			result.quickMenu.ObserveGamepad(UwpQuickMenuBinding::Buttons(
				down(GamepadButtons::A), down(GamepadButtons::B), down(GamepadButtons::X), down(GamepadButtons::Y),
				down(GamepadButtons::DPadUp), down(GamepadButtons::DPadDown),
				down(GamepadButtons::DPadLeft), down(GamepadButtons::DPadRight),
				down(GamepadButtons::LeftShoulder), down(GamepadButtons::RightShoulder),
				down(GamepadButtons::Menu), down(GamepadButtons::View)));
			result.a |= down(GamepadButtons::A);
			result.b |= down(GamepadButtons::B);
			result.x |= down(GamepadButtons::X);
			result.y |= down(GamepadButtons::Y);
			result.up |= down(GamepadButtons::DPadUp) || reading.LeftThumbstickY > 0.45;
			result.down |= down(GamepadButtons::DPadDown) || reading.LeftThumbstickY < -0.45;
			result.left |= down(GamepadButtons::DPadLeft) || reading.LeftThumbstickX < -0.45;
			result.right |= down(GamepadButtons::DPadRight) || reading.LeftThumbstickX > 0.45;
			result.lb |= down(GamepadButtons::LeftShoulder);
			result.rb |= down(GamepadButtons::RightShoulder);
			result.menu |= down(GamepadButtons::Menu);
			result.view |= down(GamepadButtons::View);
			result.ls |= down(GamepadButtons::LeftThumbstick);
			result.rs |= down(GamepadButtons::RightThumbstick);
			result.lt |= reading.LeftTrigger > 0.5;
			result.rt |= reading.RightTrigger > 0.5;
			if (std::abs(reading.RightThumbstickY) > std::abs(result.rightStickY))
				result.rightStickY = static_cast<float>(reading.RightThumbstickY);
		}
	}

	// CoreWindow mirrors the pad as VirtualKeys, mixing with WGI yields false edges
	if (!hasDirectGamepad)
	{
		result.a |= virtualKeyDown(VirtualKey::GamepadA) || ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown);
		result.b |= virtualKeyDown(VirtualKey::GamepadB) || ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight);
		result.x |= virtualKeyDown(VirtualKey::GamepadX) || ImGui::IsKeyDown(ImGuiKey_GamepadFaceLeft);
		result.y |= virtualKeyDown(VirtualKey::GamepadY) || ImGui::IsKeyDown(ImGuiKey_GamepadFaceUp);
		result.up |= virtualKeyDown(VirtualKey::GamepadDPadUp) || virtualKeyDown(VirtualKey::GamepadLeftThumbstickUp) ||
			ImGui::IsKeyDown(ImGuiKey_GamepadDpadUp) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickUp);
		result.down |= virtualKeyDown(VirtualKey::GamepadDPadDown) || virtualKeyDown(VirtualKey::GamepadLeftThumbstickDown) ||
			ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickDown);
		result.left |= virtualKeyDown(VirtualKey::GamepadDPadLeft) || virtualKeyDown(VirtualKey::GamepadLeftThumbstickLeft) ||
			ImGui::IsKeyDown(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickLeft);
		result.right |= virtualKeyDown(VirtualKey::GamepadDPadRight) || virtualKeyDown(VirtualKey::GamepadLeftThumbstickRight) ||
			ImGui::IsKeyDown(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickRight);
		result.lb |= virtualKeyDown(VirtualKey::GamepadLeftShoulder) || ImGui::IsKeyDown(ImGuiKey_GamepadL1);
		result.rb |= virtualKeyDown(VirtualKey::GamepadRightShoulder) || ImGui::IsKeyDown(ImGuiKey_GamepadR1);
		result.menu |= virtualKeyDown(VirtualKey::GamepadMenu) || ImGui::IsKeyDown(ImGuiKey_GamepadStart);
		result.view |= virtualKeyDown(VirtualKey::GamepadView) || ImGui::IsKeyDown(ImGuiKey_GamepadBack);
		result.ls |= virtualKeyDown(VirtualKey::GamepadLeftThumbstickButton);
		result.rs |= virtualKeyDown(VirtualKey::GamepadRightThumbstickButton);
		result.lt |= virtualKeyDown(VirtualKey::GamepadLeftTrigger);
		result.rt |= virtualKeyDown(VirtualKey::GamepadRightTrigger);
		result.quickMenu.ObserveGamepad(UwpQuickMenuBinding::Buttons(
			result.a, result.b, result.x, result.y,
			virtualKeyDown(VirtualKey::GamepadDPadUp) || ImGui::IsKeyDown(ImGuiKey_GamepadDpadUp),
			virtualKeyDown(VirtualKey::GamepadDPadDown) || ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown),
			virtualKeyDown(VirtualKey::GamepadDPadLeft) || ImGui::IsKeyDown(ImGuiKey_GamepadDpadLeft),
			virtualKeyDown(VirtualKey::GamepadDPadRight) || ImGui::IsKeyDown(ImGuiKey_GamepadDpadRight),
			result.lb, result.rb, result.menu, result.view));
		if (virtualKeyDown(VirtualKey::GamepadRightThumbstickUp) || ImGui::IsKeyDown(ImGuiKey_GamepadRStickUp))
			result.rightStickY = 1.0f;
		else if (virtualKeyDown(VirtualKey::GamepadRightThumbstickDown) || ImGui::IsKeyDown(ImGuiKey_GamepadRStickDown))
			result.rightStickY = -1.0f;
	}

	result.a |= ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_Space);
	result.b |= ImGui::IsKeyDown(ImGuiKey_Escape);
	result.up |= ImGui::IsKeyDown(ImGuiKey_UpArrow);
	result.down |= ImGui::IsKeyDown(ImGuiKey_DownArrow);
	result.left |= ImGui::IsKeyDown(ImGuiKey_LeftArrow);
	result.right |= ImGui::IsKeyDown(ImGuiKey_RightArrow);

	if (!hasDirectGamepad)
	{
		auto& input = InputManager::instance();
		for (size_t i = 0; i < InputManager::kMaxController; ++i)
		{
			const auto controller = input.get_controller(i);
			if (!controller)
				continue;
			result.a |= controller->is_a_down();
			result.b |= controller->is_b_down();
			result.x |= controller->is_home_down();
			result.y |= controller->is_start_down();
			result.up |= controller->is_up_down();
			result.down |= controller->is_down_down();
			result.left |= controller->is_left_down();
			result.right |= controller->is_right_down();
		}
	}

	const auto& dsuProviders = InputManager::instance().get_api_providers()[InputAPI::DSUClient];
	if (!dsuProviders.empty())
	{
		const auto provider = std::dynamic_pointer_cast<DSUControllerProvider>(dsuProviders.front());
		if (provider && provider->is_connected(0))
		{
			const auto state = provider->get_state(0);
			if (state.last_update != std::chrono::steady_clock::time_point{} &&
				std::chrono::steady_clock::now() - state.last_update <= std::chrono::seconds(2))
			{
				const uint16 buttons = (uint16)state.data.state1 | ((uint16)state.data.state2 << 8);
				result.quickMenu.ObserveGamepad(UwpQuickMenuBinding::Buttons(
					(buttons & 0x4000) != 0, (buttons & 0x2000) != 0,
					(buttons & 0x8000) != 0, (buttons & 0x1000) != 0,
					(buttons & 0x0010) != 0, (buttons & 0x0040) != 0,
					(buttons & 0x0080) != 0, (buttons & 0x0020) != 0,
					(buttons & 0x0400) != 0, (buttons & 0x0800) != 0,
					(buttons & 0x0008) != 0, (buttons & 0x0001) != 0));
				result.a |= (buttons & 0x4000) != 0;
				result.b |= (buttons & 0x2000) != 0;
				result.x |= (buttons & 0x8000) != 0;
				result.y |= (buttons & 0x1000) != 0;
				result.up |= (buttons & 0x0010) != 0 || state.data.ly > 185;
				result.down |= (buttons & 0x0040) != 0 || state.data.ly < 71;
				result.left |= (buttons & 0x0080) != 0 || state.data.lx < 71;
				result.right |= (buttons & 0x0020) != 0 || state.data.lx > 185;
				result.lb |= (buttons & 0x0400) != 0;
				result.rb |= (buttons & 0x0800) != 0;
				result.menu |= (buttons & 0x0008) != 0;
				result.view |= (buttons & 0x0001) != 0;
				const float rightY = (static_cast<float>(state.data.ry) - 127.5f) / 127.5f;
				if (std::abs(rightY) > std::abs(result.rightStickY) && std::abs(rightY) > 0.18f)
					result.rightStickY = rightY;
			}
		}
	}

	const auto companionNavigation = CemuUwp::CompanionService::GetNavigationState();
	result.up |= companionNavigation.up;
	result.down |= companionNavigation.down;
	result.left |= companionNavigation.left;
	result.right |= companionNavigation.right;
	return result;
}

std::string LowerAscii(std::string value);
void PushStatus(std::string message);
void SaveConfig();
void OpenPage(Page page);
bool RepeatPressed(bool down, DirectionRepeatState& state);
winrt::fire_and_forget CopyTitleImageToClipboardAsync(TitleTile title);

bool Pressed(bool now, bool previous)
{
	return now && !previous;
}

bool MapperControllerInputHeld(const ButtonSnapshot& buttons)
{
	return buttons.a || buttons.b || buttons.x || buttons.y || buttons.up ||
		buttons.down || buttons.left || buttons.right || buttons.lb || buttons.rb ||
		buttons.menu || buttons.view || std::abs(buttons.rightStickY) > 0.2f;
}

constexpr std::array kUwpLayoutEntries{
	std::pair{ UwpScreenLayout::TVOnly, "TV only" },
	std::pair{ UwpScreenLayout::GamePadOnly, "GamePad only" },
	std::pair{ UwpScreenLayout::SideBySide, "Side by side" },
	std::pair{ UwpScreenLayout::Stacked, "Stacked" },
	std::pair{ UwpScreenLayout::PictureInPicture, "Picture in picture" },
	std::pair{ UwpScreenLayout::GamePadPictureInPicture, "GamePad picture in picture" },
};

const char* LayoutName(sint32 layout)
{
	for (const auto& [value, name] : kUwpLayoutEntries)
	{
		if (static_cast<sint32>(value) == layout)
			return name;
	}
	return "TV only";
}

sint32 ClampLayout(sint32 layout)
{
	for (const auto& [value, name] : kUwpLayoutEntries)
	{
		if (static_cast<sint32>(value) == layout)
			return layout;
	}
	return static_cast<sint32>(UwpScreenLayout::TVOnly);
}

UwpGameLayoutEntry& GetOrCreateUwpGameLayout(uint64 titleId)
{
	auto& config = GetConfig().uwp_display;
	auto [it, inserted] = config.game_layouts.try_emplace(titleId);
	if (inserted)
	{
		it->second.title_id = titleId;
		it->second.layout = ClampLayout(config.default_layout.GetValue());
	}
	return it->second;
}

sint32 ResolveUwpLayoutForTitle(uint64 titleId)
{
	auto& display = GetConfig().uwp_display;
	if (titleId != 0)
	{
		const auto it = display.game_layouts.find(titleId);
		if (it != display.game_layouts.end())
			return ClampLayout(it->second.layout);
	}
	return ClampLayout(display.default_layout.GetValue());
}

int LayoutIndexForValue(sint32 layout)
{
	const sint32 clamped = ClampLayout(layout);
	for (int i = 0; i < (int)kUwpLayoutEntries.size(); ++i)
	{
		if (static_cast<sint32>(kUwpLayoutEntries[i].first) == clamped)
			return i;
	}
	return 0;
}

int CurrentTitleLayoutIndex()
{
	const uint64 titleId = CafeSystem::IsTitleRunning() ? CafeSystem::GetForegroundTitleId() : 0;
	return LayoutIndexForValue(ResolveUwpLayoutForTitle(titleId));
}

void SetQuickMenuOpen(bool open)
{
	if (s_quickMenuOpen == open)
		return;

	CemuHost::Trace(open ? "quick menu open" : "quick menu close");
	s_quickMenuOpen = open;
	s_quickMenuLayoutComboOpen = false;
	if (!open)
	{
		s_quickMenuMapperOpen = false;
		s_inputMapper.Close();
	}
	s_quickMenuRepeatCounters = {};
	if (open)
	{
		s_quickMenuSelection = 0;
		s_quickMenuLayoutSelection = CurrentTitleLayoutIndex();
	}
}

void OpenQuickMenuLayoutCombo()
{
	s_quickMenuSelection = 1;
	s_quickMenuLayoutSelection = CurrentTitleLayoutIndex();
	s_quickMenuLayoutComboOpen = true;
	s_quickMenuRepeatCounters = {};
}

bool ButtonTokenDown(const ButtonSnapshot& buttons, std::string token)
{
	token = LowerAscii(std::move(token));
	token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char c) { return std::isspace(c) != 0; }), token.end());
	if (token == "a") return buttons.a;
	if (token == "b" || token == "back") return buttons.b;
	if (token == "x") return buttons.x;
	if (token == "y") return buttons.y;
	if (token == "up" || token == "dpadup") return buttons.up;
	if (token == "down" || token == "dpaddown") return buttons.down;
	if (token == "left" || token == "dpadleft") return buttons.left;
	if (token == "right" || token == "dpadright") return buttons.right;
	if (token == "lb" || token == "l1" || token == "leftshoulder") return buttons.lb;
	if (token == "rb" || token == "r1" || token == "rightshoulder") return buttons.rb;
	if (token == "menu" || token == "start") return buttons.menu;
	if (token == "view" || token == "select") return buttons.view;
	if (token == "ls" || token == "l3" || token == "leftstick") return buttons.ls;
	if (token == "rs" || token == "r3" || token == "rightstick") return buttons.rs;
	if (token == "lt" || token == "l2" || token == "lefttrigger") return buttons.lt;
	if (token == "rt" || token == "r2" || token == "righttrigger") return buttons.rt;
	return false;
}

struct BindingParts
{
	std::string device;
	std::string combo;
};

BindingParts ParseBinding(std::string binding)
{
	const size_t colon = binding.find(':');
	BindingParts result{};
	result.device = colon == std::string::npos ? "gamepad" : LowerAscii(binding.substr(0, colon));
	result.combo = colon == std::string::npos ? std::move(binding) : binding.substr(colon + 1);
	result.device.erase(std::remove_if(result.device.begin(), result.device.end(), [](unsigned char c) { return std::isspace(c) != 0; }), result.device.end());
	return result;
}

bool GamepadComboDown(std::string combo, const ButtonSnapshot& buttons)
{
	std::stringstream stream(combo);
	std::string token;
	bool any = false;
	while (std::getline(stream, token, '+'))
	{
		if (token.empty())
			continue;
		any = true;
		if (!ButtonTokenDown(buttons, token))
			return false;
	}
	return any;
}

std::optional<ImGuiKey> KeyboardBindingKey(std::string key)
{
	key = LowerAscii(std::move(key));
	key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c) != 0; }), key.end());
	if (key == "escape" || key == "esc")
		return ImGuiKey_Escape;
	if (key == "f1")
		return ImGuiKey_F1;
	if (key == "tab")
		return ImGuiKey_Tab;
	if (key == "enter" || key == "return")
		return ImGuiKey_Enter;
	if (key == "space")
		return ImGuiKey_Space;
	if (key == "left")
		return ImGuiKey_LeftArrow;
	if (key == "right")
		return ImGuiKey_RightArrow;
	if (key == "up")
		return ImGuiKey_UpArrow;
	if (key == "down")
		return ImGuiKey_DownArrow;
	return std::nullopt;
}

ImGuiMouseButton PointerBindingButton(std::string button)
{
	button = LowerAscii(std::move(button));
	button.erase(std::remove_if(button.begin(), button.end(), [](unsigned char c) { return std::isspace(c) != 0; }), button.end());
	if (button.find("right") != std::string::npos)
		return ImGuiMouseButton_Right;
	if (button.find("middle") != std::string::npos)
		return ImGuiMouseButton_Middle;
	return ImGuiMouseButton_Left;
}

bool BindingDown(std::string binding, const ButtonSnapshot& buttons)
{
	const BindingParts parts = ParseBinding(std::move(binding));

	if (parts.device == "pointer" || parts.device == "mouse" || parts.device == "click")
		return ImGui::IsMouseDown(PointerBindingButton(parts.combo));

	if (parts.device == "keyboard" || parts.device == "key")
	{
		const auto key = KeyboardBindingKey(parts.combo);
		return key.has_value() && ImGui::IsKeyDown(*key);
	}

	return GamepadComboDown(parts.combo, buttons);
}

bool BindingPressed(const std::string& binding, const ButtonSnapshot& buttons, const ButtonSnapshot& previousButtons)
{
	if (binding.empty())
		return false;

	const BindingParts parts = ParseBinding(binding);
	if (parts.device == "pointer" || parts.device == "mouse" || parts.device == "click")
		return ImGui::IsMouseClicked(PointerBindingButton(parts.combo), false);

	if (parts.device == "keyboard" || parts.device == "key")
	{
		const auto key = KeyboardBindingKey(parts.combo);
		return key.has_value() && ImGui::IsKeyPressed(*key, false);
	}

	if (!GamepadComboDown(parts.combo, buttons))
		return false;
	return !GamepadComboDown(parts.combo, previousButtons);
}

// Settings > Input records the virtual gyro combo from the pad: wait for every button to be released (the A that
// opened it), then collect what is held until everything is released again
enum class GyroComboCapture { Off, WaitRelease, Recording };
GyroComboCapture s_gyroComboCapture = GyroComboCapture::Off;
ButtonSnapshot s_gyroComboCaptured{};
float s_gyroComboCaptureSeconds = 0.0f;
constexpr float kGyroComboCaptureTimeout = 10.0f;

bool AnyComboButtonDown(const ButtonSnapshot& b)
{
	return b.a || b.b || b.x || b.y || b.up || b.down || b.left || b.right || b.lb || b.rb ||
		b.menu || b.view || b.ls || b.rs || b.lt || b.rt;
}

std::string GamepadComboString(const ButtonSnapshot& b)
{
	const std::pair<bool, const char*> parts[] = {
		{ b.lt, "LT" }, { b.rt, "RT" }, { b.lb, "LB" }, { b.rb, "RB" }, { b.ls, "L3" }, { b.rs, "R3" },
		{ b.view, "View" }, { b.menu, "Menu" }, { b.up, "Up" }, { b.down, "Down" }, { b.left, "Left" }, { b.right, "Right" },
		{ b.a, "A" }, { b.b, "B" }, { b.x, "X" }, { b.y, "Y" },
	};
	std::string combo;
	for (const auto& [down, name] : parts)
	{
		if (!down)
			continue;
		combo += combo.empty() ? "Gamepad:" : "+";
		combo += name;
	}
	return combo;
}

// returns true while recording owns the controller (menus ignore it)
bool UpdateGyroComboCapture(const ButtonSnapshot& buttons)
{
	if (s_gyroComboCapture == GyroComboCapture::Off)
		return false;
	s_gyroComboCaptureSeconds += std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.25f);
	const bool anyDown = AnyComboButtonDown(buttons);
	if (s_gyroComboCapture == GyroComboCapture::WaitRelease)
	{
		if (!anyDown)
		{
			s_gyroComboCapture = GyroComboCapture::Recording;
			s_gyroComboCaptured = {};
			s_gyroComboCaptureSeconds = 0.0f;
		}
		return true;
	}
	if (anyDown)
	{
		ButtonSnapshot& c = s_gyroComboCaptured;
		c.a |= buttons.a; c.b |= buttons.b; c.x |= buttons.x; c.y |= buttons.y;
		c.up |= buttons.up; c.down |= buttons.down; c.left |= buttons.left; c.right |= buttons.right;
		c.lb |= buttons.lb; c.rb |= buttons.rb; c.menu |= buttons.menu; c.view |= buttons.view;
		c.ls |= buttons.ls; c.rs |= buttons.rs; c.lt |= buttons.lt; c.rt |= buttons.rt;
		return true;
	}
	if (AnyComboButtonDown(s_gyroComboCaptured))
	{
		GetConfig().uwp_display.virtual_gyro_binding = GamepadComboString(s_gyroComboCaptured);
		SaveConfig();
		s_gyroComboCapture = GyroComboCapture::Off;
	}
	else if (s_gyroComboCaptureSeconds > kGyroComboCaptureTimeout)
	{
		s_gyroComboCapture = GyroComboCapture::Off;
	}
	// stay owned this frame so the release doesn't reach the menus
	return true;
}

bool ProcessGlobalShortcutBindings(const ButtonSnapshot& buttons)
{
	auto& display = GetConfig().uwp_display;
	bool consumed = false;
	if (CafeSystem::IsTitleRunning() && BindingPressed(display.toggle_swap_binding.GetValue(), buttons, s_previousButtons))
	{
		display.swap_screens = !display.swap_screens.GetValue();
		SaveConfig();
		PushStatus(display.swap_screens.GetValue() ? Tr("Screens swapped") : Tr("Screens restored"));
		consumed = true;
	}
	if (CafeSystem::IsTitleRunning() && display.virtual_gyro_combo_enabled.GetValue() &&
		BindingPressed(display.virtual_gyro_binding.GetValue(), buttons, s_previousButtons))
	{
		WGIGamepadController::SetVirtualMotionMode(
			WGIGamepadController::GetVirtualMotionMode() == WGIGamepadController::VirtualMotionMode::Off ?
				WGIGamepadController::VirtualMotionMode::Tilt : WGIGamepadController::VirtualMotionMode::Off);
		// in-game overlay: the frontend's own notifications only draw in the menus
		LatteOverlay_pushNotification(WGIGamepadController::GetVirtualMotionMode() == WGIGamepadController::VirtualMotionMode::Off ?
			Tr("Virtual gyro off") : Tr("Virtual gyro on: the right stick tilts the GamePad"), 3000);
		consumed = true;
	}
	if (CafeSystem::IsTitleRunning() && UwpQuickMenuBinding::Pressed(buttons.quickMenu, s_previousButtons.quickMenu))
	{
		SetQuickMenuOpen(!s_quickMenuOpen);
		consumed = true;
	}
	return consumed;
}

void SaveCurrentTitleLayout(sint32 layout)
{
	if (!CafeSystem::IsTitleRunning())
		return;
	const uint64 titleId = CafeSystem::GetForegroundTitleId();
	auto& entry = GetOrCreateUwpGameLayout(titleId);
	entry.layout = ClampLayout(layout);
	SaveConfig();
	PushStatus(TrFormat("Layout: {}", LayoutName(entry.layout)));
}

void ApplyQuickMenuLayoutSelection()
{
	s_quickMenuLayoutSelection = std::clamp(s_quickMenuLayoutSelection, 0, (int)kUwpLayoutEntries.size() - 1);
	SaveCurrentTitleLayout(static_cast<sint32>(kUwpLayoutEntries[s_quickMenuLayoutSelection].first));
	s_quickMenuLayoutComboOpen = false;
	s_quickMenuRepeatCounters = {};
}

void ActivateQuickMenuSelection()
{
	CemuHost::Trace(fmt::format("quick menu activate {}", s_quickMenuSelection));
	switch (s_quickMenuSelection)
	{
	case 0:
		SetQuickMenuOpen(false);
		break;
	case 1:
		OpenQuickMenuLayoutCombo();
		break;
	case 2:
		s_inputMapper.Open(s_inputMappingHost,
			CafeSystem::IsTitleRunning() ?
				std::optional<uint64>{ CafeSystem::GetForegroundTitleId() } : std::nullopt,
			0);
		s_quickMenuMapperOpen = true;
		s_quickMenuRepeatCounters = {};
		break;
	case 3:
	{
		// one switch for the stats that matter on console: FPS, CPU and memory against the app limit
		auto& config = GetConfig();
		const bool enable = config.overlay.position == ScreenPosition::kDisabled;
		config.overlay.position = enable ? ScreenPosition::kTopRight : ScreenPosition::kDisabled; // top left holds most HUDs (BOTW hearts)
		if (enable)
		{
			config.overlay.fps = true;
			config.overlay.cpu_usage = true;
			config.overlay.ram_usage = true;
			config.overlay.vram_usage = true;
		}
		SaveConfig();
		break;
	}
	case 4:
		if (CemuUwp::CompanionService::IsStreamBusy())
			break;
		if (CemuUwp::CompanionService::IsStreamActive())
			CemuUwp::CompanionService::RequestStreamStop();
		else
			CemuUwp::CompanionService::RequestStreamStart();
		break;
	case 5:
		SetQuickMenuOpen(false);
		if (!CemuHost::RestartTitle())
			PushStatus(Tr("Restart unavailable"));
		break;
	case 6:
		SetQuickMenuOpen(false);
		if (CemuUwp::WindowHost::HasLaunchOnExitUri())
		{
			CemuHost::Shutdown();
			winrt::Windows::ApplicationModel::Core::CoreApplication::Exit();
		}
		else
		{
			CemuHost::ShutdownTitle();
			OpenPage(Page::Home);
		}
		break;
	case 7:
		SetQuickMenuOpen(false);
		CemuHost::Shutdown();
		winrt::Windows::ApplicationModel::Core::CoreApplication::Exit();
		break;
	case 8:
		WGIGamepadController::SetVirtualMotionMode(
			WGIGamepadController::GetVirtualMotionMode() == WGIGamepadController::VirtualMotionMode::Off ?
				WGIGamepadController::VirtualMotionMode::Tilt : WGIGamepadController::VirtualMotionMode::Off);
		break;
	default:
		break;
	}
}

void QueueQuickMenuActivation(int selection)
{
	s_pendingQuickMenuActivation = std::clamp(selection, 0, kQuickMenuItemCount - 1);
	s_quickMenuSelection = s_pendingQuickMenuActivation;
	CemuHost::Trace(fmt::format("quick menu queue {}", s_pendingQuickMenuActivation));
}

bool ConsumeQueuedQuickMenuActivation()
{
	if (s_pendingQuickMenuActivation < 0)
		return false;
	s_quickMenuSelection = std::exchange(s_pendingQuickMenuActivation, -1);
	ActivateQuickMenuSelection();
	return true;
}

void ProcessQuickMenuInput(const ButtonSnapshot& buttons)
{
	const bool moveUp = RepeatPressed(buttons.up, s_quickMenuRepeatCounters.up);
	const bool moveDown = RepeatPressed(buttons.down, s_quickMenuRepeatCounters.down);
	const bool moveLeft = RepeatPressed(buttons.left, s_quickMenuRepeatCounters.left);
	const bool moveRight = RepeatPressed(buttons.right, s_quickMenuRepeatCounters.right);
	const bool backPressed = Pressed(buttons.b, s_previousButtons.b);
	const bool activatePressed = Pressed(buttons.a, s_previousButtons.a);
	if (s_quickMenuMapperOpen)
	{
		s_mapperFrameInput = {
			.deltaSeconds = ImGui::GetIO().DeltaTime,
			.up = moveUp,
			.down = moveDown,
			.left = moveLeft,
			.right = moveRight,
			.accept = activatePressed,
			.back = backPressed,
			.context = Pressed(buttons.x, s_previousButtons.x),
			.alternate = Pressed(buttons.y, s_previousButtons.y),
			.menu = Pressed(buttons.menu, s_previousButtons.menu),
			.view = Pressed(buttons.view, s_previousButtons.view),
			.leftShoulder = Pressed(buttons.lb, s_previousButtons.lb),
			.rightShoulder = Pressed(buttons.rb, s_previousButtons.rb),
			.controllerInputHeld = MapperControllerInputHeld(buttons),
			.rightStickY = buttons.rightStickY,
		};
		return;
	}

	if (s_quickMenuLayoutComboOpen)
	{
		if (moveUp || moveLeft)
			s_quickMenuLayoutSelection = std::max(0, s_quickMenuLayoutSelection - 1);
		if (moveDown || moveRight)
			s_quickMenuLayoutSelection = std::min((int)kUwpLayoutEntries.size() - 1, s_quickMenuLayoutSelection + 1);
		if (backPressed)
		{
			s_quickMenuLayoutComboOpen = false;
			s_quickMenuRepeatCounters = {};
		}
		if (activatePressed)
			ApplyQuickMenuLayoutSelection();
		return;
	}

	if (moveUp)
		s_quickMenuSelection = std::max(0, s_quickMenuSelection - 1);
	if (moveDown)
		s_quickMenuSelection = std::min(kQuickMenuItemCount - 1, s_quickMenuSelection + 1);
	if (s_quickMenuSelection == 1 && (moveLeft || moveRight))
	{
		OpenQuickMenuLayoutCombo();
		if (moveLeft)
		{
			s_quickMenuLayoutSelection = std::max(0, s_quickMenuLayoutSelection - 1);
			s_quickMenuRepeatCounters.left.down = true;
			s_quickMenuRepeatCounters.left.nextRepeat = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
		}
		if (moveRight)
		{
			s_quickMenuLayoutSelection = std::min((int)kUwpLayoutEntries.size() - 1, s_quickMenuLayoutSelection + 1);
			s_quickMenuRepeatCounters.right.down = true;
			s_quickMenuRepeatCounters.right.nextRepeat = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
		}
	}
	if (backPressed)
	{
		SetQuickMenuOpen(false);
		return;
	}
	if (activatePressed)
		ActivateQuickMenuSelection();
}

bool RepeatPressed(bool down, DirectionRepeatState& state)
{
	const auto now = std::chrono::steady_clock::now();
	if (!down)
	{
		if (state.down)
		{
			state.down = false;
			state.releasedAt = now;
		}
		return false;
	}

	if (!state.down)
	{
		state.down = true;
		// a gap this short means the input sources disagreed, not a new press
		if (state.releasedAt.time_since_epoch().count() != 0 &&
			now - state.releasedAt < std::chrono::milliseconds(60))
			return false;
		state.nextRepeat = now + std::chrono::milliseconds(300);
		return true;
	}

	if (now < state.nextRepeat)
		return false;

	state.nextRepeat = now + std::chrono::milliseconds(120);
	return true;
}

void SaveConfig()
{
	if (!GetConfigHandle().Save())
	{
		cemuLog_log(LogType::Force, "UWP failed to save settings to {}", _pathToUtf8(ActiveSettings::GetConfigPath("settings.xml")));
		PushStatus(Tr("Failed to save settings"));
	}
}

void PushStatus(std::string message)
{
	{
		std::lock_guard lock(s_frontendMutex);
		s_status = message;
	}
	s_notificationCenter.Push(std::move(message), 5.0f);
}

UwpImGuiFrontend::ICredentialStore& ScreenScraperCredentialStore()
{
	static auto store = UwpImGuiFrontend::CreatePasswordVaultCredentialStore(
		std::wstring(kScreenScraperUserVaultResource));
	return *store;
}

template <size_t N>
void CopyToBuffer(std::array<char, N>& buffer, const std::string& value)
{
	buffer.fill(0);
	strncpy(buffer.data(), value.c_str(), buffer.size() - 1);
}

void EnsureScreenScraperCredentialBuffers()
{
	if (s_screenScraperCredentialBuffersLoaded)
		return;
	const auto userCredential = ScreenScraperCredentialStore().Load();
	CopyToBuffer(s_screenScraperUserBuffer, userCredential.user);
	CopyToBuffer(s_screenScraperPasswordBuffer, userCredential.password);
	s_screenScraperCredentialBuffersLoaded = true;
}

bool HasBuiltInScreenScraperDeveloperCredentials()
{
	return CEMU_UWP_SCREENSCRAPER_DEV_ID[0] != '\0' &&
		CEMU_UWP_SCREENSCRAPER_DEV_PASSWORD[0] != '\0';
}

void RefreshTitleList()
{
	CafeTitleList::Refresh();
	MarkTitleCacheDirty();
	PushStatus(Tr("Refreshing game list"));
}

bool TitleInstallDialogOwnsInput()
{
	return s_titleInstallDialogState != TitleInstallDialogState::Closed;
}

void CloseTitleInstallDialog()
{
	if (s_titleInstallDialogState == TitleInstallDialogState::Installing &&
		CemuUwp::TitleInstaller::IsBusy())
	{
		return;
	}
	CemuUwp::TitleInstaller::Reset();
	s_titleInstallPlan.reset();
	s_titleInstallSnapshot = {};
	s_titleInstallMessage.clear();
	s_titleInstallCancelConfirmation = false;
	s_titleInstallCompletionHandled = false;
	s_titleInstallDismissRequested = false;
	s_titleInstallRequestDefaultFocus = false;
	s_titleInstallPickerRequested = false;
	s_titleInstallInputReleaseBarrier = false;
	s_titleInstallBackConsumedUntilIdle = false;
	s_titleInstallDialogState = TitleInstallDialogState::Closed;
}

void ServiceTitleInstaller()
{
	if (s_titleInstallDialogState == TitleInstallDialogState::Planning && s_titleInstallPlanFuture.valid() &&
		s_titleInstallPlanFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		try
		{
			auto result = s_titleInstallPlanFuture.get();
			if (result.plan)
			{
				s_titleInstallPlan = std::move(*result.plan);
				s_titleInstallRequestDefaultFocus = true;
				s_titleInstallDialogState = TitleInstallDialogState::Confirmation;
			}
			else
			{
				s_titleInstallMessage = result.error.empty() ? "The selected title could not be installed" : std::move(result.error);
				s_titleInstallRequestDefaultFocus = true;
				s_titleInstallDialogState = TitleInstallDialogState::Result;
			}
		}
		catch (const std::exception& ex)
		{
			s_titleInstallMessage = fmt::format("Could not inspect the selected title: {}", ex.what());
			s_titleInstallRequestDefaultFocus = true;
			s_titleInstallDialogState = TitleInstallDialogState::Result;
		}
	}

	if (s_titleInstallDialogState != TitleInstallDialogState::Installing)
		return;
	s_titleInstallSnapshot = CemuUwp::TitleInstaller::GetSnapshot();
	using InstallState = CemuUwp::TitleInstaller::State;
	if (s_titleInstallSnapshot.state != InstallState::Succeeded &&
		s_titleInstallSnapshot.state != InstallState::Cancelled &&
		s_titleInstallSnapshot.state != InstallState::Failed)
	{
		return;
	}
	if (s_titleInstallCompletionHandled)
		return;
	s_titleInstallCompletionHandled = true;
	s_titleInstallMessage = s_titleInstallSnapshot.message;
	if (s_titleInstallSnapshot.state == InstallState::Succeeded && s_titleInstallPlan)
	{
		CafeTitleList::AddTitleFromPath(s_titleInstallPlan->targetPath);
		CafeTitleList::Refresh();
		MarkTitleCacheDirty();
		PushStatus(s_titleInstallMessage.empty() ? std::string(Tr("Title installed")) : s_titleInstallMessage);
	}
	else if (!s_titleInstallMessage.empty())
	{
		PushStatus(s_titleInstallMessage);
	}
	s_titleInstallCancelConfirmation = false;
	s_titleInstallRequestDefaultFocus = true;
	s_titleInstallDialogState = TitleInstallDialogState::Result;
}

void ProcessTitleInstallDialogInput(const ButtonSnapshot& buttons, bool systemBackRequested)
{
	if (s_titleInstallBackConsumedUntilIdle)
	{
		if (!buttons.b && !systemBackRequested)
			s_titleInstallBackConsumedUntilIdle = false;
		return;
	}
	if (!systemBackRequested && !Pressed(buttons.b, s_previousButtons.b))
		return;
	s_titleInstallBackConsumedUntilIdle = true;
	switch (s_titleInstallDialogState)
	{
	case TitleInstallDialogState::Confirmation:
	case TitleInstallDialogState::Result:
		s_titleInstallDismissRequested = true;
		break;
	case TitleInstallDialogState::Installing:
		if (s_titleInstallSnapshot.state == CemuUwp::TitleInstaller::State::Cancelling)
			break;
		s_titleInstallCancelConfirmation = !s_titleInstallCancelConfirmation;
		s_titleInstallRequestDefaultFocus = true;
		break;
	case TitleInstallDialogState::Planning:
	case TitleInstallDialogState::Closed:
	default:
		break;
	}
}

void PickTitleInstallFolderAsync()
{
	if (CafeSystem::IsTitleRunning())
	{
		PushStatus(Tr("Stop the running game before installing content."));
		return;
	}
	if (TitleInstallDialogOwnsInput() || s_pickerOpen)
		return;

	UwpImGuiFrontend::PickerRequest request;
	request.startLocation = UwpImGuiFrontend::PickerStartLocation::Computer;
	request.fileTypes = { L"*" };
	request.futureAccessToken = L"title-install-source";
	const bool started = PickerService().PickFolder(std::move(request), MakePickerCallbacks(
		[](std::unique_ptr<UwpImGuiFrontend::PickedItem> item) {
			// Consume the press that closed the picker before restoring shell input.
			s_titleInstallInputReleaseBarrier = true;
			if (!item)
				return;
			const fs::path sourceRoot = item->path;
			const fs::path mlcRoot = ActiveSettings::GetMlcPath();
			s_titleInstallPlan.reset();
			s_titleInstallSnapshot = {};
			s_titleInstallMessage.clear();
			s_titleInstallCancelConfirmation = false;
			s_titleInstallCompletionHandled = false;
			s_titleInstallDismissRequested = false;
			s_titleInstallRequestDefaultFocus = false;
			s_titleInstallDialogState = TitleInstallDialogState::Planning;
			s_titleInstallPlanFuture = std::async(std::launch::async, [sourceRoot, mlcRoot]() {
				return CemuUwp::TitleInstaller::BuildPlan(sourceRoot, mlcRoot);
			});
		},
		[](std::string_view error) {
			cemuLog_log(LogType::Force, "UWP title install picker failed: {}", error);
			s_titleInstallMessage = "Could not open the title install picker";
			s_titleInstallRequestDefaultFocus = true;
			s_titleInstallDialogState = TitleInstallDialogState::Result;
		}));
	if (!started)
		PushStatus(Tr("A file or folder picker is already open"));
}

void PickTitleAsync()
{
	UwpImGuiFrontend::PickerRequest request;
	request.startLocation = UwpImGuiFrontend::PickerStartLocation::Computer;
	// WinRT filters by extension only, so title.tmd is listed as .tmd
	request.fileTypes = { L".wud", L".wux", L".wua", L".wuhb", L".iso", L".rpx", L".elf", L".tmd" };
	request.futureAccessToken = L"last-title";
	if (!PickerService().PickFile(std::move(request), MakePickerCallbacks(
		[](std::unique_ptr<UwpImGuiFrontend::PickedItem> item) {
			if (item)
				CemuHost::LaunchPath(item->path);
		},
		[](std::string_view error) {
			cemuLog_log(LogType::Force, "UWP title picker failed: {}", error);
			PushStatus(Tr("Could not open the title picker"));
		})))
	{
		PushStatus(Tr("A file or folder picker is already open"));
	}
}

void AddGameFolder(const fs::path& path)
{
	if (path.empty())
		return;
	const std::string pathUtf8 = _pathToUtf8(path);
	auto& gamePaths = GetConfig().game_paths;
	if (std::find(gamePaths.begin(), gamePaths.end(), pathUtf8) != gamePaths.end())
	{
		PushStatus(Tr("That folder is already in the list"));
		return;
	}
	gamePaths.emplace_back(pathUtf8);
	std::error_code createError;
	fs::create_directories(path, createError);
	CafeTitleList::AddScanPath(path);
	CafeTitleList::Refresh();
	MarkTitleCacheDirty();
	SaveConfig();
	std::error_code existsError;
	if (fs::exists(path, existsError))
		PushStatus(Tr("Game folder added"));
	else
		PushStatus(Tr("Game folder added, but it is not accessible - is the drive connected?"));
}

void SetPrimaryGameFolder(const fs::path& path)
{
	if (path.empty())
		return;
	const std::string pathUtf8 = _pathToUtf8(path.lexically_normal());
	auto& gamePaths = GetConfig().game_paths;
	if (gamePaths.empty())
		gamePaths.emplace_back(pathUtf8);
	else
		gamePaths.front() = pathUtf8;
	gamePaths.erase(std::remove(gamePaths.begin() + 1, gamePaths.end(), pathUtf8),
		gamePaths.end());

	std::error_code createError;
	fs::create_directories(path, createError);
	CafeTitleList::ClearScanPaths();
	for (const std::string& configured : gamePaths)
		CafeTitleList::AddScanPath(_utf8ToPath(configured));
	CafeTitleList::Refresh();
	MarkTitleCacheDirty();
	SaveConfig();

	std::error_code existsError;
	PushStatus(fs::exists(path, existsError) ? Tr("Games directory updated") :
		Tr("Games directory updated, but it is not accessible - is the drive connected?"));
}

bool CreateDefaultMlcFiles(const fs::path& mlc)
{
	const auto ensureDir = [](const fs::path& path) {
		std::error_code ec;
		if (!fs::exists(path, ec))
			return fs::create_directories(path, ec);
		return true;
	};
	const fs::path directories[] = {
		mlc,
		mlc / "sys",
		mlc / "usr",
		mlc / "usr/title/00050000",
		mlc / "usr/title/0005000c",
		mlc / "usr/title/0005000e",
		mlc / "usr/save/00050010/1004a000/user/common/db",
		mlc / "usr/save/00050010/1004a100/user/common/db",
		mlc / "usr/save/00050010/1004a200/user/common/db",
		mlc / "sys/title/0005001b/1005c000/content",
	};
	for (const auto& path : directories)
	{
		if (!ensureDir(path))
			return false;
	}
	try
	{
		const auto langDir = fs::path(mlc).append("sys/title/0005001b/1005c000/content");
		const auto langFile = fs::path(langDir).append("language.txt");
		if (!fs::exists(langFile))
		{
			std::ofstream file(langFile);
			if (file.is_open())
			{
				const char* langStrings[] = { "ja", "en", "fr", "de", "it", "es", "zh", "ko", "nl", "pt", "ru", "zh" };
				for (const char* lang : langStrings)
					file << fmt::format(R"("{}",)", lang) << std::endl;
			}
		}
		const auto countryFile = fs::path(langDir).append("country.txt");
		if (!fs::exists(countryFile))
		{
			std::ofstream file(countryFile);
			for (sint32 i = 0; i < NCrypto::GetCountryCount(); i++)
			{
				const char* countryCode = NCrypto::GetCountryAsString(i);
				if (boost::iequals(countryCode, "NN"))
					file << "NULL," << std::endl;
				else
					file << fmt::format(R"("{}",)", countryCode) << std::endl;
			}
		}
	}
	catch (const std::exception&)
	{
		return false;
	}
	return true;
}

void ApplyMlcPath(const fs::path& path)
{
	if (CafeSystem::IsTitleRunning())
	{
		PushStatus(Tr("Can't change the MLC path while a game is running"));
		return;
	}
	if (!CreateDefaultMlcFiles(path))
	{
		PushStatus(Tr("Could not prepare that MLC folder - is the drive connected and writable?"));
		return;
	}
	GetConfig().SetMLCPath(path);
	SaveConfig();
	PushStatus(Tr("MLC path saved - restart Cemu to use it (accounts and saves are not moved)"));
}

// an empty path returns to the graphicPacks folder in app storage
void ApplyGraphicPacksPath(const fs::path& path)
{
	if (CafeSystem::IsTitleRunning())
	{
		PushStatus(Tr("Can't change the graphic packs folder while a game is running"));
		return;
	}
	const fs::path folder = path.empty() ? ActiveSettings::GetUserDataPath("graphicPacks") : path;
	std::error_code ec;
	fs::create_directories(folder, ec);
	if (!fs::is_directory(folder, ec))
	{
		PushStatus(Tr("Could not open that graphic packs folder - is the drive connected?"));
		return;
	}
	GetConfig().graphic_packs_path.SetValue(path.empty() ? std::string{} : _pathToUtf8(path));
	SaveConfig();
	GraphicPack2::ClearGraphicPacks();
	GraphicPack2::LoadAll();
	ResetGraphicPackListState();
	cemuLog_log(LogType::Force, "UWP graphic packs: folder set to '{}', {} packs loaded",
		_pathToUtf8(folder), GraphicPack2::GetGraphicPacks().size());
	PushStatus(Tr("Graphic packs folder saved and packs reloaded"));
}

void PickUnderlayAsync(uint64 titleId, std::wstring targetFileName, std::string label)
{
	UwpImGuiFrontend::PickerRequest request;
	request.startLocation = UwpImGuiFrontend::PickerStartLocation::Pictures;
	request.fileTypes = { L".png" };
	if (!PickerService().PickFile(std::move(request), MakePickerCallbacks(
		[titleId, targetFileName = std::move(targetFileName), label](
			std::unique_ptr<UwpImGuiFrontend::PickedItem> item) {
			if (!item)
				return;
			std::error_code error;
			const fs::path titleFolder = CemuUwp::StorageService::LocalRoot() /
				"titleImages" / fmt::format("{:016x}", titleId);
			fs::create_directories(titleFolder, error);
			if (!error)
			{
				fs::copy_file(item->path, titleFolder / targetFileName,
					fs::copy_options::overwrite_existing, error);
			}
			if (error)
			{
				cemuLog_log(LogType::Force, "UWP failed to import {} underlay: {}",
					label, error.message());
				PushStatus(TrFormat("Failed to import {} underlay", label));
				return;
			}
			PushStatus(TrFormat("{} underlay updated", label));
		},
		[label](std::string_view error) {
			cemuLog_log(LogType::Force, "UWP failed to open {} underlay picker: {}",
				label, error);
			PushStatus(TrFormat("Failed to import {} underlay", label));
		})))
	{
		PushStatus(Tr("A file or folder picker is already open"));
	}
}

void ClearUnderlay(uint64 titleId, const char* fileName, const char* label)
{
	const std::string title = fmt::format("{:016x}", titleId);
	std::error_code ec;
	fs::remove(ActiveSettings::GetUserDataPath("titleImages/{}/{}", title, fileName), ec);
	PushStatus(ec ? TrFormat("Failed to clear {} underlay", label) : TrFormat("{} underlay cleared", label));
}

void OpenPage(Page page)
{
	const Page previousPage = s_page;
	if (previousPage == Page::Controllers && page != Page::Controllers)
	{
		s_globalInputMapper.Close();
		s_globalMapperFrameInput = {};
		s_globalMapperRepeatCounters = {};
	}
	s_page = page;
	if (page != Page::TitleManager)
		s_titleManagerGamesDirectoryOpen = false;
	s_focusArea = page == Page::Home ? FocusArea::HomeGrid : FocusArea::Content;
	s_requestContentFocus = page != Page::Home;
	s_activeOverlayPaneFocus = OverlayPaneFocus::None;
	if (page == Page::Controllers)
	{
		if (previousPage != Page::Controllers || !s_globalInputMapper.IsOpen())
			s_globalInputMapper.Open(s_inputMappingHost, std::nullopt, 0);
		s_globalMapperFrameInput = {};
		s_globalMapperRepeatCounters = {};
		s_pendingOverlayPaneFocus = OverlayPaneFocus::None;
	}
	else if (page == Page::Settings || page == Page::GraphicPacks)
		RequestOverlayPaneFocus(OverlayPaneFocus::Left);
	else
		s_pendingOverlayPaneFocus = OverlayPaneFocus::None;
}

struct TitleContextActionState
{
	bool enabled = true;
	bool checked = false;
	std::string_view reason;
};

std::vector<fs::path> FindTitleShaderCachePaths(uint64 titleId)
{
	std::vector<fs::path> paths;
	const std::array candidates{
		ActiveSettings::GetCachePath("shaderCache/driver/vk/{:016x}.bin", titleId),
		ActiveSettings::GetCachePath("shaderCache/driver/d3d12/{:016x}", titleId),
		ActiveSettings::GetCachePath("shaderCache/precompiled/{:016x}_spirv.bin", titleId),
		ActiveSettings::GetCachePath("shaderCache/precompiled/{:016x}_gl.bin", titleId),
		ActiveSettings::GetCachePath("shaderCache/precompiled/{:016x}_air.bin", titleId),
		ActiveSettings::GetCachePath("shaderCache/precompiled/{:016x}_dxil.bin", titleId),
		ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_shaders.bin", titleId),
		ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_mtlshaders.bin", titleId),
		ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_vkpipeline.bin", titleId),
		ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_mtlpipeline.bin", titleId),
		ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_d3d12pipeline.bin", titleId),
		ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_d3d12_native_pso.bin", titleId),
		ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_d3d12recipes.bin", titleId),
		ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_d3d12recipes_usage.bin", titleId),
	};
	for (const fs::path& path : candidates)
	{
		std::error_code ec;
		if (fs::exists(path, ec) && !ec)
			paths.emplace_back(path);
	}
	return paths;
}

uint64 GetShaderCacheBytes(const std::vector<fs::path>& paths)
{
	uint64 total = 0;
	for (const fs::path& path : paths)
	{
		std::error_code ec;
		if (fs::is_directory(path, ec))
		{
			for (auto it = fs::recursive_directory_iterator(path, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
			{
				std::error_code sizeEc;
				if (it->is_regular_file(sizeEc))
					total += it->file_size(sizeEc);
			}
		}
		else if (!ec)
		{
			const uint64 size = fs::file_size(path, ec);
			if (!ec)
				total += size;
		}
	}
	return total;
}

std::string BuildTitleWikiUri(GameInfo2& gameInfo)
{
	if (!gameInfo.IsValid() || !gameInfo.GetBase().ParseXmlInfo())
		return {};
	const auto* meta = gameInfo.GetBase().GetMetaInfo();
	if (!meta)
		return {};

	const std::string productCode = meta->GetProductCode();
	const std::string companyCode = meta->GetCompanyCode();
	const size_t lastDash = productCode.rfind('-');
	const std::string_view productSuffix = lastDash == std::string::npos ?
		std::string_view(productCode) : std::string_view(productCode).substr(lastDash + 1);
	if (productSuffix.empty() || companyCode.size() < 2)
		return {};
	return fmt::format("https://wiki.cemu.info/wiki/{}{}", productSuffix, companyCode.substr(companyCode.size() - 2));
}

fs::path ExistingTitleDirectory(fs::path path)
{
	if (path.has_extension())
		path = path.parent_path();
	std::error_code ec;
	return fs::is_directory(path, ec) && !ec ? path : fs::path{};
}

void CopyTitleTextToClipboard(std::string_view text, std::string_view description)
{
	try
	{
		winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
		package.RequestedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
		package.SetText(winrt::to_hstring(text));
		winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
		winrt::Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
		PushStatus(TrFormat("{} copied", description));
	}
	catch (const winrt::hresult_error& e)
	{
		cemuLog_log(LogType::Force, "UWP clipboard copy failed: 0x{:08x} {}", (uint32)e.code(), winrt::to_string(e.message()));
		PushStatus(TrFormat("Failed to copy {}", description));
	}
}

winrt::fire_and_forget LaunchExternalUriAsync(std::string uri, std::string label)
{
	try
	{
		const bool launched = co_await winrt::Windows::System::Launcher::LaunchUriAsync(
			winrt::Windows::Foundation::Uri(winrt::to_hstring(uri)));
		if (!launched)
			PushStatus(TrFormat("No app can open {}", label));
	}
	catch (const winrt::hresult_error& e)
	{
		cemuLog_log(LogType::Force, "UWP URI launch failed for {} ({}): 0x{:08x} {}",
			label, uri, (uint32)e.code(), winrt::to_string(e.message()));
		PushStatus(TrFormat("Failed to open {}", label));
	}
}

winrt::fire_and_forget LaunchTitleFolderAsync(fs::path path, std::string label)
{
	try
	{
		// the await resumes on the caller's ASTA, as LaunchFolderAsync requires
		const auto folder = co_await winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(
			winrt::hstring(path.wstring()));
		const bool launched = co_await winrt::Windows::System::Launcher::LaunchFolderAsync(folder);
		if (!launched)
			PushStatus(TrFormat("No installed app can browse the {}", label));
	}
	catch (const winrt::hresult_error& e)
	{
		cemuLog_log(LogType::Force, "UWP folder launch failed for {} ({}): 0x{:08x} {}",
			label, _pathToUtf8(path), (uint32)e.code(), winrt::to_string(e.message()));
		PushStatus(TrFormat("Could not access the {}", label));
	}
}

void CloseTitleContextMenu(bool blockUntilInputReleased = false)
{
	if (s_metadataEditor && s_metadataEditor->IsOpen())
		s_metadataEditor->Close();
	if (s_titleContextMenu.view == TitleContextView::ControllerMapping)
		s_inputMapper.Close();
	s_titleContextMenu = {};
	s_pendingTitleContextActivation.reset();
	s_titleContextInputReleaseBarrier = blockUntilInputReleased;
	if (ImGui::GetCurrentContext())
		ImGui::ClearActiveID();
}

void OpenTitleContextMenu(const TitleTile& title)
{
	GameInfo2 gameInfo = CafeTitleList::GetGameInfo(title.titleId);
	if (!gameInfo.IsValid())
	{
		PushStatus(Tr("Title information is no longer available"));
		return;
	}

	TitleContextMenuState state{};
	state.open = true;
	state.title = title;
	state.baseTitleId = gameInfo.GetBaseTitleId();
	state.title.path = gameInfo.GetBase().GetPath();
	state.originalName = gameInfo.GetTitleName();
	if (state.originalName.empty())
		state.originalName = title.name;
	state.wikiUri = BuildTitleWikiUri(gameInfo);
	state.hasUpdate = gameInfo.HasUpdate();
	state.hasDlc = gameInfo.HasAOC();
	state.gameDirectory = ExistingTitleDirectory(gameInfo.GetBase().GetPath());
	state.saveDirectory = ExistingTitleDirectory(gameInfo.GetSaveFolder());
	state.hasSaveDirectory = !state.saveDirectory.empty();
	if (state.hasUpdate)
		state.updateDirectory = ExistingTitleDirectory(gameInfo.GetUpdate().GetPath());
	if (state.hasDlc)
		state.dlcDirectory = ExistingTitleDirectory(gameInfo.GetAOC().front().GetPath());
	state.shaderCachePaths = FindTitleShaderCachePaths(state.baseTitleId);
	state.shaderCacheBytes = GetShaderCacheBytes(state.shaderCachePaths);
	state.metadata = UwpImGuiFrontend::LoadScreenScraperMetadata(
		TitleMediaRoot(state.baseTitleId) / "metadata.json",
		state.title.region, MetadataLanguage());
	if (const auto saved = UwpImGuiFrontend::LoadGameMetadataRecord(
		TitleMetadataRecordPath(state.baseTitleId)); saved &&
		(saved->itemId == 0 || saved->itemId == state.baseTitleId))
	{
		state.metadata = saved->metadata;
		if (!saved->title.empty())
			state.title.name = saved->title;
	}
	s_titleContextMenu = std::move(state);
	s_titleContextInputReleaseBarrier = false;
	s_pendingTitleContextActivation.reset();
	s_repeatCounters = {};
	s_showUsers = false;
}

int GameProfileSectionIndex(GameProfileSection section)
{
	return static_cast<int>(section);
}

int GameProfileRowCount(GameProfileSection section)
{
	switch (section)
	{
	case GameProfileSection::General: return 4;
	case GameProfileSection::Graphics: return 2;
	case GameProfileSection::Controllers: return 8;
	}
	return 0;
}

std::string GameProfileFieldLabel(const TitleGameProfileEditorState& editor, int row)
{
	switch (editor.section)
	{
	case GameProfileSection::General:
	{
		constexpr std::array<const char*, 4> labels{
			"Load shared libraries", "Launch with gamepad view", "CPU mode", "Thread quantum"
		};
		return row >= 0 && row < (int)labels.size() ? Tr(labels[row]) : "";
	}
	case GameProfileSection::Graphics:
	{
		constexpr std::array<const char*, 2> labels{
			"Graphics API", "Shader multiplication accuracy"
		};
		return row >= 0 && row < (int)labels.size() ? Tr(labels[row]) : "";
	}
	case GameProfileSection::Controllers:
		return row >= 0 && row < 8 ? TrFormat("Controller {}", row + 1) : "";
	}
	return {};
}

bool GameProfileFieldIsToggle(const TitleGameProfileEditorState& editor, int row)
{
	return editor.section == GameProfileSection::General && (row == 0 || row == 1);
}

int GameProfileChoiceCount(const TitleGameProfileEditorState& editor, int row)
{
	switch (editor.section)
	{
	case GameProfileSection::General:
		if (row == 2) return (int)kGameProfileCpuModeLabels.size();
		if (row == 3) return (int)kGameProfileThreadQuantumValues.size();
		break;
	case GameProfileSection::Graphics:
		if (row == 0) return (int)kGameProfileGraphicsApiLabels.size();
		if (row == 1) return (int)kGameProfileShaderMulLabels.size();
		break;
	case GameProfileSection::Controllers:
		return (int)editor.controllerProfileNames.size();
	}
	return 0;
}

int GameProfileCurrentChoice(const TitleGameProfileEditorState& editor, int row)
{
	switch (editor.section)
	{
	case GameProfileSection::General:
		if (row == 2) return editor.cpuMode;
		if (row == 3) return editor.threadQuantum;
		break;
	case GameProfileSection::Graphics:
		if (row == 0) return editor.graphicsApi;
		if (row == 1) return editor.accurateShaderMul;
		break;
	case GameProfileSection::Controllers:
		if (row >= 0 && row < (int)editor.controllerProfiles.size())
			return editor.controllerProfiles[row];
		break;
	}
	return 0;
}

void SetGameProfileChoice(TitleGameProfileEditorState& editor, int row, int choice)
{
	const int count = GameProfileChoiceCount(editor, row);
	choice = std::clamp(choice, 0, std::max(0, count - 1));
	switch (editor.section)
	{
	case GameProfileSection::General:
		if (row == 2) editor.cpuMode = choice;
		if (row == 3) editor.threadQuantum = choice;
		break;
	case GameProfileSection::Graphics:
		if (row == 0) editor.graphicsApi = choice;
		if (row == 1) editor.accurateShaderMul = choice;
		break;
	case GameProfileSection::Controllers:
		if (row >= 0 && row < (int)editor.controllerProfiles.size())
			editor.controllerProfiles[row] = choice;
		break;
	}
}

std::string GameProfileChoiceLabel(const TitleGameProfileEditorState& editor, int row, int choice)
{
	switch (editor.section)
	{
	case GameProfileSection::General:
		if (row == 2 && choice >= 0 && choice < (int)kGameProfileCpuModeLabels.size())
			return Tr(kGameProfileCpuModeLabels[choice]);
		if (row == 3 && choice >= 0 && choice < (int)kGameProfileThreadQuantumValues.size())
			return TrFormat("{} cycles", kGameProfileThreadQuantumValues[choice]);
		break;
	case GameProfileSection::Graphics:
		if (row == 0 && choice >= 0 && choice < (int)kGameProfileGraphicsApiLabels.size())
			return Tr(kGameProfileGraphicsApiLabels[choice]);
		if (row == 1 && choice >= 0 && choice < (int)kGameProfileShaderMulLabels.size())
			return Tr(kGameProfileShaderMulLabels[choice]);
		break;
	case GameProfileSection::Controllers:
		if (choice == 0)
			return Tr("Default");
		if (choice > 0 && choice < (int)editor.controllerProfileNames.size())
			return editor.controllerProfileNames[choice];
		break;
	}
	return Tr("Default");
}

std::string GameProfileFieldValue(const TitleGameProfileEditorState& editor, int row)
{
	if (editor.section == GameProfileSection::General && row == 0)
		return editor.loadSharedLibraries ? Tr("On") : Tr("Off");
	if (editor.section == GameProfileSection::General && row == 1)
		return editor.startWithPadView ? Tr("On") : Tr("Off");
	return GameProfileChoiceLabel(editor, row, GameProfileCurrentChoice(editor, row));
}

void OpenGameProfileCombo()
{
	auto& editor = s_titleContextMenu.gameProfile;
	const int count = GameProfileChoiceCount(editor, editor.row);
	if (count <= 0)
		return;
	editor.comboOpen = true;
	editor.comboSelection = std::clamp(GameProfileCurrentChoice(editor, editor.row), 0, count - 1);
	editor.comboScroll = std::max(0, editor.comboSelection - 3);
	s_titleContextMenu.repeat = {};
}

void ActivateGameProfileField()
{
	auto& editor = s_titleContextMenu.gameProfile;
	if (editor.section == GameProfileSection::General && editor.row == 0)
		editor.loadSharedLibraries = !editor.loadSharedLibraries;
	else if (editor.section == GameProfileSection::General && editor.row == 1)
		editor.startWithPadView = !editor.startWithPadView;
	else
		OpenGameProfileCombo();
}

void AdjustGameProfileField(int delta)
{
	auto& editor = s_titleContextMenu.gameProfile;
	if (editor.section == GameProfileSection::General && editor.row == 0)
	{
		editor.loadSharedLibraries = delta > 0;
		return;
	}
	if (editor.section == GameProfileSection::General && editor.row == 1)
	{
		editor.startWithPadView = delta > 0;
		return;
	}
	const int count = GameProfileChoiceCount(editor, editor.row);
	if (count > 0)
		SetGameProfileChoice(editor, editor.row,
			std::clamp(GameProfileCurrentChoice(editor, editor.row) + delta, 0, count - 1));
}

void BeginTitleGameProfileEdit()
{
	auto& editor = s_titleContextMenu.gameProfile;
	editor = {};

	GameProfile profile;
	profile.Reset();
	profile.Load(s_titleContextMenu.baseTitleId);
	editor.loadSharedLibraries = profile.ShouldLoadSharedLibraries().value_or(true);
	editor.startWithPadView = profile.StartWithGamepadView();
	switch (profile.GetCPUMode().value_or(CPUMode::Auto))
	{
	case CPUMode::SinglecoreInterpreter: editor.cpuMode = 0; break;
	case CPUMode::SinglecoreRecompiler: editor.cpuMode = 1; break;
	case CPUMode::DualcoreRecompiler:
	case CPUMode::MulticoreRecompiler: editor.cpuMode = 2; break;
	case CPUMode::Auto:
	default: editor.cpuMode = 3; break;
	}
	const auto quantum = std::find(kGameProfileThreadQuantumValues.begin(), kGameProfileThreadQuantumValues.end(), profile.GetThreadQuantum());
	editor.threadQuantum = quantum == kGameProfileThreadQuantumValues.end() ? 1 : (int)std::distance(kGameProfileThreadQuantumValues.begin(), quantum);
	editor.graphicsApi = profile.GetGraphicsAPI().has_value() && profile.GetGraphicsAPI().value() == GraphicAPI::kDirect3D12 ? 1 : 0;
	editor.accurateShaderMul = profile.GetAccurateShaderMul() == AccurateShaderMulOption::False ? 0 : 1;

	editor.controllerProfileNames.emplace_back();
	for (std::string name : InputManager::get_profiles())
	{
		if (!name.empty() && std::find(editor.controllerProfileNames.begin(), editor.controllerProfileNames.end(), name) == editor.controllerProfileNames.end())
			editor.controllerProfileNames.emplace_back(std::move(name));
	}
	const auto& assignedProfiles = profile.GetControllerProfile();
	for (size_t i = 0; i < assignedProfiles.size(); ++i)
	{
		if (!assignedProfiles[i].has_value() || assignedProfiles[i]->empty())
			continue;
		auto found = std::find(editor.controllerProfileNames.begin(), editor.controllerProfileNames.end(), assignedProfiles[i].value());
		if (found == editor.controllerProfileNames.end())
		{
			editor.controllerProfileNames.emplace_back(assignedProfiles[i].value());
			found = std::prev(editor.controllerProfileNames.end());
		}
		editor.controllerProfiles[i] = (int)std::distance(editor.controllerProfileNames.begin(), found);
	}

	s_titleContextMenu.view = TitleContextView::EditGameProfile;
	s_titleContextMenu.repeat = {};
}

void SaveTitleGameProfile()
{
	const auto& editor = s_titleContextMenu.gameProfile;
	GameProfile profile;
	profile.Reset();
	profile.SetLoadSharedLibraries(editor.loadSharedLibraries);
	profile.SetStartWithPadView(editor.startWithPadView);
	constexpr std::array cpuModes{
		CPUMode::SinglecoreInterpreter, CPUMode::SinglecoreRecompiler,
		CPUMode::MulticoreRecompiler, CPUMode::Auto
	};
	profile.SetCPUMode(cpuModes[std::clamp(editor.cpuMode, 0, (int)cpuModes.size() - 1)]);
	profile.SetThreadQuantum(kGameProfileThreadQuantumValues[std::clamp(editor.threadQuantum, 0, (int)kGameProfileThreadQuantumValues.size() - 1)]);
	profile.SetGraphicsAPI(editor.graphicsApi == 1 ? std::optional<GraphicAPI>{ GraphicAPI::kDirect3D12 } : std::nullopt);
	profile.SetAccurateShaderMul(editor.accurateShaderMul == 0 ? AccurateShaderMulOption::False : AccurateShaderMulOption::True);
	for (size_t i = 0; i < editor.controllerProfiles.size(); ++i)
	{
		const int selection = editor.controllerProfiles[i];
		if (selection > 0 && selection < (int)editor.controllerProfileNames.size())
			profile.SetControllerProfile(i, editor.controllerProfileNames[selection]);
		else
			profile.SetControllerProfile(i, std::nullopt);
	}
	profile.Save(s_titleContextMenu.baseTitleId);
	s_titleContextMenu.view = TitleContextView::Menu;
	s_titleContextMenu.repeat = {};
	PushStatus(Tr("Game profile saved"));
}

TitleContextActionState GetTitleContextActionState(TitleContextAction action)
{
	TitleContextActionState state{};
	switch (action)
	{
	case TitleContextAction::Start:
		state.enabled = s_titleContextMenu.title.launchable && !s_titleContextMenu.title.path.empty();
		state.reason = state.enabled ? std::string_view{} : Tr("This entry cannot be launched");
		break;
	case TitleContextAction::Favorite:
		state.checked = GetConfig().IsGameListFavorite(s_titleContextMenu.title.titleId);
		break;
	case TitleContextAction::WikiPage:
		state.enabled = !s_titleContextMenu.wikiUri.empty();
		state.reason = state.enabled ? std::string_view{} : Tr("No Cemu Wiki ID is available");
		break;
	case TitleContextAction::GameDirectory:
		state.enabled = !s_titleContextMenu.gameDirectory.empty();
		state.reason = state.enabled ? std::string_view{} : Tr("Game directory was not found");
		break;
	case TitleContextAction::SaveDirectory:
		state.enabled = s_titleContextMenu.hasSaveDirectory;
		state.reason = state.enabled ? std::string_view{} : Tr("No save data found");
		break;
	case TitleContextAction::UpdateDirectory:
		state.enabled = !s_titleContextMenu.updateDirectory.empty();
		state.reason = state.enabled ? std::string_view{} :
			(s_titleContextMenu.hasUpdate ? Tr("Update directory was not found") : Tr("No update installed"));
		break;
	case TitleContextAction::DlcDirectory:
		state.enabled = !s_titleContextMenu.dlcDirectory.empty();
		state.reason = state.enabled ? std::string_view{} :
			(s_titleContextMenu.hasDlc ? Tr("DLC directory was not found") : Tr("No DLC installed"));
		break;
	case TitleContextAction::RemoveShaderCaches:
		state.enabled = !s_titleContextMenu.shaderCachePaths.empty();
		state.reason = state.enabled ? std::string_view{} : Tr("No shader caches found");
		break;
	case TitleContextAction::RefreshGames:
		state.enabled = !CafeTitleList::IsScanning();
		state.reason = state.enabled ? std::string_view{} : Tr("A title scan is already running");
		break;
	default:
		break;
	}
	return state;
}

void OpenTitleGraphicPacks(uint64 titleId)
{
	CemuHost::EnsureGraphicPacksLoaded();
	const auto& packs = GraphicPack2::GetGraphicPacks();
	const auto pack = std::find_if(packs.begin(), packs.end(), [titleId](const auto& candidate) {
		return candidate && candidate->ContainsTitleId(titleId);
	});
	if (pack != packs.end())
	{
		s_selectedGraphicPack = (int)std::distance(packs.begin(), pack);
		EnsureGraphicPackListState();
		const int groupIndex = GraphicPackGroupIndexForPack(s_selectedGraphicPack);
		const auto& groups = GetGraphicPackGroups();
		if (groupIndex >= 0 && groupIndex < (int)groups.size())
		{
			s_expandedGraphicPackGroups.clear();
			s_expandedGraphicPackGroups.insert(groups[groupIndex].name);
			s_graphicPackListRow = GraphicPackHeaderRowForGroup(groupIndex);
		}
	}
	else
	{
		ResetGraphicPackListState();
		PushStatus(Tr("No graphic packs target this title"));
	}
	OpenPage(Page::GraphicPacks);
}

void RemoveTitleShaderCaches()
{
	size_t removed = 0;
	size_t failed = 0;
	for (const fs::path& path : s_titleContextMenu.shaderCachePaths)
	{
		std::error_code ec;
		if (fs::is_directory(path, ec))
		{
			if (!ec && fs::remove_all(path, ec) != 0)
				removed++;
		}
		else if (!ec && fs::remove(path, ec))
		{
			removed++;
		}
		if (ec)
		{
			failed++;
			cemuLog_log(LogType::Force, "UWP failed to remove shader cache {}: {}", _pathToUtf8(path), ec.message());
		}
	}
	CloseTitleContextMenu();
	if (failed == 0)
		PushStatus(TrFormat("Removed {} shader cache items", removed));
	else
		PushStatus(TrFormat("Removed {} shader cache items; {} failed", removed, failed));
}

bool SaveTitleMetadata(const UwpImGuiFrontend::GameMetadataRecord& record,
	bool selectedMatch)
{
	const uint64 titleId = s_titleContextMenu.baseTitleId != 0 ?
		s_titleContextMenu.baseTitleId : s_titleContextMenu.title.titleId;
	if (!UwpImGuiFrontend::SaveGameMetadataRecordAtomic(
		TitleMetadataRecordPath(titleId), record))
	{
		PushStatus(Tr("Could not save title metadata"));
		return false;
	}

	s_titleContextMenu.title.name = record.title.empty() ?
		s_titleContextMenu.originalName : record.title;
	s_titleContextMenu.metadata = record.metadata;
	s_pendingCompanionSelectionToken = TitleIconKey(s_titleContextMenu.title);
	MarkTitleCacheDirty();
	const bool requestedDownload = selectedMatch && record.sourceGameId != 0;
	bool downloadStarted = false;
	bool statusAlreadyShown = false;
	if (requestedDownload)
	{
		auto request = BuildScreenScraperRequestForTitle(titleId,
			record.sourceGameId, s_titleContextMenu.title.name,
			s_titleContextMenu.title.region, s_titleContextMenu.title.path);
		if (request.user.empty() || request.password.empty())
		{
			PushStatus(Tr("Metadata saved; sign in to ScreenScraper to download media"));
			statusAlreadyShown = true;
		}
		else
		{
			downloadStarted = CemuUwp::ScreenScraperDownloader::Start(std::move(request));
			if (!downloadStarted)
			{
				PushStatus(Tr("Metadata saved; another ScreenScraper download is running"));
				statusAlreadyShown = true;
			}
		}
	}
	if (!statusAlreadyShown)
		PushStatus(downloadStarted ? "Metadata saved; downloading selected media" :
			"Metadata saved");
	return true;
}

void BeginTitleMetadataEdit()
{
	const uint64 titleId = s_titleContextMenu.baseTitleId != 0 ?
		s_titleContextMenu.baseTitleId : s_titleContextMenu.title.titleId;
	UwpImGuiFrontend::GameMetadataRecord record;
	record.itemId = titleId;
	record.title = s_titleContextMenu.title.name;
	record.metadata = UwpImGuiFrontend::LoadScreenScraperMetadata(
		TitleMediaRoot(titleId) / "metadata.json",
		s_titleContextMenu.title.region, MetadataLanguage());
	if (const auto saved = UwpImGuiFrontend::LoadGameMetadataRecord(
		TitleMetadataRecordPath(titleId)); saved &&
		(saved->itemId == 0 || saved->itemId == titleId))
	{
		record = *saved;
		record.itemId = titleId;
		if (record.title.empty())
			record.title = s_titleContextMenu.title.name;
	}

	EnsureScreenScraperCredentialBuffers();
	if (!s_metadataEditor)
	{
		s_metadataEditor = std::make_unique<UwpImGuiFrontend::MetadataEditor>(
			CemuUwp::ScreenScraperDownloader::GetService());
	}
	const auto listArtwork = ConfiguredSearchArtwork(false);
	const auto detailArtwork = ConfiguredSearchArtwork(true);
	s_metadataEditor->Open({
		.record = std::move(record),
		.defaultTitle = s_titleContextMenu.title.name,
		.listArtwork = ConfiguredArtworkPath(s_titleContextMenu.title,
			CurrentListArtwork()).value_or(fs::path{}),
		.detailArtwork = ConfiguredArtworkPath(s_titleContextMenu.title,
			CurrentDetailArtwork()).value_or(fs::path{}),
	}, {
		.listArtwork = listArtwork,
		.detailArtwork = detailArtwork,
		.searchRequest = CemuUwp::ScreenScraperDownloader::MakeSearchRequest(
			s_screenScraperUserBuffer.data(), s_screenScraperPasswordBuffer.data(),
			GetConfig().uwp_screenscraper.preferred_region.GetValue(),
			s_titleContextMenu.title.region, MetadataLanguage()),
	}, {
		.loadTexture = [](const fs::path& path) {
			UwpImGuiFrontend::TextureHandle result;
			if (ShellTexture* texture = FileTexture("metadata", path);
				texture && texture->Valid())
			{
				result.id = reinterpret_cast<std::uintptr_t>(texture->id);
				result.width = static_cast<std::uint32_t>(texture->width);
				result.height = static_cast<std::uint32_t>(texture->height);
			}
			return result;
		},
		.save = [](const UwpImGuiFrontend::GameMetadataRecord& saved,
			bool selectedMatch) { return SaveTitleMetadata(saved, selectedMatch); },
		.notify = [](std::string_view message) { PushStatus(std::string(message)); },
	});
	s_titleContextMenu.view = TitleContextView::EditMetadata;
	s_titleContextMenu.repeat = {};
}

void ActivateTitleContextAction(TitleContextAction action)
{
	const TitleContextActionState actionState = GetTitleContextActionState(action);
	if (!actionState.enabled)
	{
		if (!actionState.reason.empty())
			PushStatus(std::string(actionState.reason));
		return;
	}

	const TitleTile title = s_titleContextMenu.title;
	switch (action)
	{
	case TitleContextAction::Start:
		s_pendingContextLaunchToken = TitleIconKey(title);
		CloseTitleContextMenu();
		break;
	case TitleContextAction::Favorite:
	{
		const bool favorite = !GetConfig().IsGameListFavorite(title.titleId);
		GetConfig().SetGameListFavorite(title.titleId, favorite);
		SaveConfig();
		s_pendingCompanionSelectionToken = TitleIconKey(title);
		MarkTitleCacheDirty();
		CloseTitleContextMenu();
		PushStatus(favorite ? Tr("Added to favorites") : Tr("Removed from favorites"));
		break;
	}
	case TitleContextAction::EditMetadata:
		BeginTitleMetadataEdit();
		break;
	case TitleContextAction::WikiPage:
	{
		const std::string uri = s_titleContextMenu.wikiUri;
		CloseTitleContextMenu();
		LaunchExternalUriAsync(uri, "the Cemu Wiki");
		break;
	}
	case TitleContextAction::GameDirectory:
	{
		const fs::path path = s_titleContextMenu.gameDirectory;
		CloseTitleContextMenu();
		LaunchTitleFolderAsync(path, "game directory");
		break;
	}
	case TitleContextAction::SaveDirectory:
	{
		const fs::path path = s_titleContextMenu.saveDirectory;
		CloseTitleContextMenu();
		LaunchTitleFolderAsync(path, "save directory");
		break;
	}
	case TitleContextAction::UpdateDirectory:
	{
		const fs::path path = s_titleContextMenu.updateDirectory;
		CloseTitleContextMenu();
		LaunchTitleFolderAsync(path, "update directory");
		break;
	}
	case TitleContextAction::DlcDirectory:
	{
		const fs::path path = s_titleContextMenu.dlcDirectory;
		CloseTitleContextMenu();
		LaunchTitleFolderAsync(path, "DLC directory");
		break;
	}
	case TitleContextAction::RemoveShaderCaches:
		s_titleContextMenu.view = TitleContextView::ConfirmCacheRemoval;
		s_titleContextMenu.confirmSelection = 1;
		s_titleContextMenu.repeat = {};
		break;
	case TitleContextAction::EditGraphicPacks:
	{
		const uint64 baseTitleId = s_titleContextMenu.baseTitleId;
		CloseTitleContextMenu();
		OpenTitleGraphicPacks(baseTitleId);
		break;
	}
	case TitleContextAction::EditGameProfile:
		BeginTitleGameProfileEdit();
		break;
	case TitleContextAction::ControllerMapping:
		s_inputMapper.Open(s_inputMappingHost, s_titleContextMenu.baseTitleId, 0);
		s_titleContextMenu.view = TitleContextView::ControllerMapping;
		s_titleContextMenu.repeat = {};
		break;
	case TitleContextAction::CopyTitleName:
		CopyTitleTextToClipboard(s_titleContextMenu.originalName, "Title name");
		CloseTitleContextMenu();
		break;
	case TitleContextAction::CopyTitleId:
		CopyTitleTextToClipboard(fmt::format("{:016x}", s_titleContextMenu.baseTitleId), "Title ID");
		CloseTitleContextMenu();
		break;
	case TitleContextAction::CopyTitleImage:
		CloseTitleContextMenu();
		CopyTitleImageToClipboardAsync(title);
		break;
	case TitleContextAction::RefreshGames:
		CloseTitleContextMenu();
		RefreshTitleList();
		break;
	default:
		break;
	}
}

UwpImGuiFrontend::ScreenScraperArtwork ConfiguredSearchArtwork(bool detail)
{
	const sint32 value = detail ?
		GetConfig().uwp_screenscraper.detail_artwork_source.GetValue() :
		GetConfig().uwp_screenscraper.list_artwork_source.GetValue();
	return static_cast<UwpImGuiFrontend::ScreenScraperArtwork>(std::clamp(
		value, static_cast<sint32>(UwpImGuiFrontend::ScreenScraperArtwork::Box2D),
		static_cast<sint32>(UwpImGuiFrontend::ScreenScraperArtwork::TitleScreen)));
}

void ProcessTitleContextMenuInput(const ButtonSnapshot& buttons, bool systemBackRequested)
{
	if (!s_titleContextMenu.open)
		return;
	if (s_pendingTitleContextActivation)
	{
		const TitleContextAction action = *s_pendingTitleContextActivation;
		s_pendingTitleContextActivation.reset();
		ActivateTitleContextAction(action);
		return;
	}

	const bool backPressed = systemBackRequested || Pressed(buttons.b, s_previousButtons.b);
	const bool activatePressed = Pressed(buttons.a, s_previousButtons.a);
	const bool moveUp = RepeatPressed(buttons.up, s_titleContextMenu.repeat.up);
	const bool moveDown = RepeatPressed(buttons.down, s_titleContextMenu.repeat.down);
	const bool moveLeft = RepeatPressed(buttons.left, s_titleContextMenu.repeat.left);
	const bool moveRight = RepeatPressed(buttons.right, s_titleContextMenu.repeat.right);
	if (s_titleContextMenu.view == TitleContextView::ControllerMapping)
	{
		s_mapperFrameInput = {
			.deltaSeconds = ImGui::GetIO().DeltaTime,
			.up = moveUp,
			.down = moveDown,
			.left = moveLeft,
			.right = moveRight,
			.accept = activatePressed,
			.back = backPressed,
			.context = Pressed(buttons.x, s_previousButtons.x),
			.alternate = Pressed(buttons.y, s_previousButtons.y),
			.menu = Pressed(buttons.menu, s_previousButtons.menu),
			.view = Pressed(buttons.view, s_previousButtons.view),
			.leftShoulder = Pressed(buttons.lb, s_previousButtons.lb),
			.rightShoulder = Pressed(buttons.rb, s_previousButtons.rb),
			.controllerInputHeld = MapperControllerInputHeld(buttons),
			.rightStickY = buttons.rightStickY,
		};
		return;
	}
	if (s_titleContextMenu.view == TitleContextView::EditMetadata)
	{
		if (!s_metadataEditor || !s_metadataEditor->IsOpen())
		{
			s_titleContextMenu.view = TitleContextView::Menu;
			return;
		}
		const auto event = s_metadataEditor->HandleInput({
			.accept = activatePressed,
			.back = backPressed,
			.up = moveUp,
			.down = moveDown,
			.left = moveLeft,
			.right = moveRight,
			.leftShoulder = Pressed(buttons.lb, s_previousButtons.lb),
			.rightShoulder = Pressed(buttons.rb, s_previousButtons.rb),
			.rightStickY = buttons.rightStickY,
			.deltaSeconds = ImGui::GetIO().DeltaTime,
		});
		if (event != UwpImGuiFrontend::MetadataEditorEvent::None)
		{
			s_titleContextMenu.view = TitleContextView::Menu;
			s_titleContextMenu.repeat = {};
		}
		return;
	}
	if (s_titleContextMenu.view == TitleContextView::EditGameProfile)
	{
		auto& editor = s_titleContextMenu.gameProfile;
		if (editor.comboOpen)
		{
			const int choiceCount = GameProfileChoiceCount(editor, editor.row);
			if (moveUp)
				editor.comboSelection = std::max(0, editor.comboSelection - 1);
			if (moveDown)
				editor.comboSelection = std::min(std::max(0, choiceCount - 1), editor.comboSelection + 1);
			if (backPressed)
			{
				editor.comboOpen = false;
				s_titleContextMenu.repeat = {};
			}
			else if (activatePressed && choiceCount > 0)
			{
				SetGameProfileChoice(editor, editor.row, editor.comboSelection);
				editor.comboOpen = false;
				s_titleContextMenu.repeat = {};
			}
			return;
		}

		if (backPressed)
		{
			s_titleContextMenu.view = TitleContextView::Menu;
			s_titleContextMenu.repeat = {};
			return;
		}
		if (Pressed(buttons.y, s_previousButtons.y))
		{
			SaveTitleGameProfile();
			return;
		}

		const bool previousSection = Pressed(buttons.lb, s_previousButtons.lb);
		const bool nextSection = Pressed(buttons.rb, s_previousButtons.rb);
		if (previousSection || nextSection)
		{
			const int sectionCount = (int)kGameProfileSectionLabels.size();
			const int delta = previousSection ? -1 : 1;
			const int section = (GameProfileSectionIndex(editor.section) + delta + sectionCount) % sectionCount;
			editor.section = static_cast<GameProfileSection>(section);
			editor.row = std::clamp(editor.row, 0, std::max(0, GameProfileRowCount(editor.section) - 1));
			editor.scroll = 0.0f;
			s_titleContextMenu.repeat = {};
			return;
		}

		const int rowCount = GameProfileRowCount(editor.section);
		if (moveUp)
			editor.row = std::max(0, editor.row - 1);
		if (moveDown)
			editor.row = std::min(std::max(0, rowCount - 1), editor.row + 1);
		if (moveLeft)
			AdjustGameProfileField(-1);
		if (moveRight)
			AdjustGameProfileField(1);
		if (activatePressed)
			ActivateGameProfileField();
		return;
	}
	if (s_titleContextMenu.view == TitleContextView::ConfirmCacheRemoval)
	{
		if (moveLeft || moveUp)
			s_titleContextMenu.confirmSelection = 0;
		if (moveRight || moveDown)
			s_titleContextMenu.confirmSelection = 1;
		if (backPressed)
		{
			s_titleContextMenu.view = TitleContextView::Menu;
			s_titleContextMenu.repeat = {};
		}
		else if (activatePressed)
		{
			if (s_titleContextMenu.confirmSelection == 0)
				RemoveTitleShaderCaches();
			else
			{
				s_titleContextMenu.view = TitleContextView::Menu;
				s_titleContextMenu.repeat = {};
			}
		}
		return;
	}

	if (moveUp)
		s_titleContextMenu.selection = std::max(0, s_titleContextMenu.selection - 1);
	if (moveDown)
		s_titleContextMenu.selection = std::min((int)kTitleContextEntries.size() - 1, s_titleContextMenu.selection + 1);
	if (backPressed)
	{
		(void)UwpImGuiFrontend::ConsumeWidgetBackPressed();
		CloseTitleContextMenu(true);
		return;
	}
	if (activatePressed)
		ActivateTitleContextAction(kTitleContextEntries[s_titleContextMenu.selection].action);
}

void ActivateLeftRail()
{
	switch (s_leftRailIndex)
	{
	case 0:
		OpenPage(Page::Home);
		break;
	case 1:
		OpenPage(Page::TitleManager);
		break;
	case 2:
	default:
		OpenPage(Page::Settings);
		break;
	}
}

void ActivateRightRail()
{
	switch (s_rightRailIndex)
	{
	case 0:
		OpenPage(Page::Controllers);
		break;
	case 1:
		OpenPage(Page::GraphicPacks);
		break;
	case 2:
	default:
		winrt::Windows::ApplicationModel::Core::CoreApplication::Exit();
		break;
	}
}

void MoveHomeFocus(const std::vector<TitleTile>& titles, bool moveUp, bool moveDown, bool moveLeft, bool moveRight)
{
	const int totalSlots = HomeSlotCount(titles.size());
	s_selectedTitle = std::clamp(s_selectedTitle, 0, totalSlots - 1);
	s_homePage = std::clamp(s_homePage, 0, HomePageCount(titles.size()) - 1);
	const int pageFirst = HomePageFirstSlot(s_homePage);
	const int pageEnd = HomePageEndSlot(titles.size(), s_homePage);

	if (s_focusArea == FocusArea::LeftRail)
	{
		if (moveUp)
			s_leftRailIndex = std::max(0, s_leftRailIndex - 1);
		if (moveDown)
			s_leftRailIndex = std::min(2, s_leftRailIndex + 1);
		if (moveRight)
		{
			s_focusArea = FocusArea::HomeGrid;
			const int row = std::clamp(s_leftRailIndex, 0, kHomeRows - 1);
			s_selectedTitle = ClampHomeSlotToPage(titles.size(), s_homePage, pageFirst + row * kHomeColumns);
		}
		return;
	}

	if (s_focusArea == FocusArea::RightRail)
	{
		if (moveUp)
			s_rightRailIndex = std::max(0, s_rightRailIndex - 1);
		if (moveDown)
			s_rightRailIndex = std::min(2, s_rightRailIndex + 1);
		if (moveLeft)
		{
			s_focusArea = FocusArea::HomeGrid;
			const int row = std::clamp(s_rightRailIndex, 0, kHomeRows - 1);
			s_selectedTitle = ClampHomeSlotToPage(titles.size(), s_homePage, pageFirst + row * kHomeColumns + (kHomeColumns - 1));
		}
		return;
	}

	if (s_focusArea != FocusArea::HomeGrid)
		return;

	int local = s_selectedTitle - s_homePage * kHomePageSize;
	if (local < 0 || local >= kHomePageSize)
		local = 0;
	int row = local / kHomeColumns;
	int col = local % kHomeColumns;

	if (moveLeft)
	{
		if (col > 0)
			col--;
		else
		{
			s_focusArea = FocusArea::LeftRail;
			s_leftRailIndex = std::clamp(row, 0, 2);
			return;
		}
	}
	if (moveRight)
	{
		const int nextSlot = s_selectedTitle + 1;
		if (col + 1 < kHomeColumns && nextSlot < pageEnd)
			col++;
		else
		{
			s_focusArea = FocusArea::RightRail;
			s_rightRailIndex = std::clamp(row, 0, 2);
			return;
		}
	}
	if (moveUp && row > 0)
		row--;
	if (moveDown && row + 1 < kHomeRows && pageFirst + (row + 1) * kHomeColumns < pageEnd)
		row++;

	s_selectedTitle = ClampHomeSlotToPage(titles.size(), s_homePage, pageFirst + row * kHomeColumns + col);
}

void ProcessShellInput(const ButtonSnapshot& buttons, const std::vector<TitleTile>& titles, bool pickerOpen, bool systemBackRequested)
{
	if (s_titleContextMenu.open)
	{
		ProcessTitleContextMenuInput(buttons, systemBackRequested);
		return;
	}
	if (s_page == Page::Controllers)
	{
		s_globalMapperFrameInput = {
			.deltaSeconds = ImGui::GetIO().DeltaTime,
			.up = RepeatPressed(buttons.up, s_globalMapperRepeatCounters.up),
			.down = RepeatPressed(buttons.down, s_globalMapperRepeatCounters.down),
			.left = RepeatPressed(buttons.left, s_globalMapperRepeatCounters.left),
			.right = RepeatPressed(buttons.right, s_globalMapperRepeatCounters.right),
			.accept = Pressed(buttons.a, s_previousButtons.a),
			.back = systemBackRequested || Pressed(buttons.b, s_previousButtons.b),
			.context = Pressed(buttons.x, s_previousButtons.x),
			.alternate = Pressed(buttons.y, s_previousButtons.y),
			.menu = Pressed(buttons.menu, s_previousButtons.menu),
			.view = Pressed(buttons.view, s_previousButtons.view),
			.leftShoulder = Pressed(buttons.lb, s_previousButtons.lb),
			.rightShoulder = Pressed(buttons.rb, s_previousButtons.rb),
			.controllerInputHeld = MapperControllerInputHeld(buttons),
			.rightStickY = buttons.rightStickY,
		};
		return;
	}

	if (s_page == Page::Home)
	{
		if (const auto companionToken = CemuUwp::CompanionService::TakePendingSelectionToken())
			s_pendingCompanionSelectionToken = companionToken;
		if (s_pendingCompanionSelectionToken)
		{
			const auto selected = std::find_if(titles.begin(), titles.end(), [](const TitleTile& title) {
				return TitleIconKey(title) == *s_pendingCompanionSelectionToken;
			});
			if (selected != titles.end())
			{
				s_selectedTitle = (int)std::distance(titles.begin(), selected);
				s_homePage = s_selectedTitle / kHomePageSize;
				s_focusArea = FocusArea::HomeGrid;
				if (s_titleCacheComplete)
					s_pendingCompanionSelectionToken.reset();
			}
			else if (s_titleCacheComplete)
			{
				s_pendingCompanionSelectionToken.reset();
			}
		}
	}

	bool moveUp = false;
	bool moveDown = false;
	bool moveLeft = false;
	bool moveRight = false;
	if (s_page == Page::Home)
	{
		moveUp = RepeatPressed(buttons.up, s_repeatCounters.up);
		moveDown = RepeatPressed(buttons.down, s_repeatCounters.down);
		moveLeft = RepeatPressed(buttons.left, s_repeatCounters.left);
		moveRight = RepeatPressed(buttons.right, s_repeatCounters.right);
	}
	else
	{
		s_repeatCounters = {};
	}

	if (systemBackRequested || Pressed(buttons.b, s_previousButtons.b))
	{
		const bool splitPage = s_page == Page::Settings || s_page == Page::GraphicPacks || s_page == Page::Controllers;
		if (s_page == Page::TitleManager && s_titleManagerGamesDirectoryOpen)
			s_titleManagerGamesDirectoryOpen = false;
		else if (splitPage && s_activeOverlayPaneFocus == OverlayPaneFocus::Right)
			RequestOverlayPaneFocus(OverlayPaneFocus::Left);
		else if (s_page == Page::Home)
			s_focusArea = FocusArea::HomeGrid;
		else
			OpenPage(Page::Home);
	}

	if (s_page == Page::Home && (Pressed(buttons.lb, s_previousButtons.lb) || Pressed(buttons.rb, s_previousButtons.rb)))
	{
		const int pageCount = HomePageCount(titles.size());
		const int local = std::clamp(s_selectedTitle - s_homePage * kHomePageSize, 0, kHomePageSize - 1);
		if (Pressed(buttons.lb, s_previousButtons.lb))
			s_homePage = std::max(0, s_homePage - 1);
		if (Pressed(buttons.rb, s_previousButtons.rb))
			s_homePage = std::min(pageCount - 1, s_homePage + 1);
		s_selectedTitle = ClampHomeSlotToPage(titles.size(), s_homePage, HomePageFirstSlot(s_homePage) + local);
		s_focusArea = FocusArea::HomeGrid;
	}

	if (s_page == Page::Home)
	{
		MoveHomeFocus(titles, moveUp, moveDown, moveLeft, moveRight);
	}

	if (s_page == Page::Settings && (Pressed(buttons.lb, s_previousButtons.lb) || Pressed(buttons.rb, s_previousButtons.rb)))
	{
		const int delta = Pressed(buttons.lb, s_previousButtons.lb) ? -1 : 1;
		s_settingsPage = MoveTab(s_settingsPage, kSettingsTabs, delta);
		RequestOverlayPaneFocus(OverlayPaneFocus::Right);
	}
	else if (s_page == Page::GraphicPacks && (Pressed(buttons.lb, s_previousButtons.lb) || Pressed(buttons.rb, s_previousButtons.rb)))
	{
		const auto& groups = GetGraphicPackGroups();
		if (!groups.empty())
		{
			const int delta = Pressed(buttons.lb, s_previousButtons.lb) ? -1 : 1;
			const int g = std::clamp(GraphicPackGroupIndexForPack(s_selectedGraphicPack) + delta, 0, (int)groups.size() - 1);
			s_expandedGraphicPackGroups.insert(groups[g].name);
			s_selectedGraphicPack = groups[g].firstPack;
			s_graphicPackListRow = GraphicPackHeaderRowForGroup(g);
			RequestOverlayPaneFocus(OverlayPaneFocus::Left);
		}
	}
	if (Pressed(buttons.a, s_previousButtons.a))
	{
		if (s_page == Page::Home && s_focusArea == FocusArea::HomeGrid)
		{
			if (s_selectedTitle >= 0 && s_selectedTitle < (int)titles.size())
			{
				const auto& title = titles[s_selectedTitle];
				if (title.launchable)
					s_pendingLaunchIndex = s_selectedTitle;
			}
			else if (!pickerOpen)
			{
				s_titleManagerGamesDirectoryOpen = true;
				OpenPage(Page::TitleManager);
			}
		}
		else if (s_page == Page::Home && s_focusArea == FocusArea::LeftRail)
		{
			ActivateLeftRail();
		}
		else if (s_page == Page::Home && s_focusArea == FocusArea::RightRail)
		{
			ActivateRightRail();
		}
		else if ((s_page == Page::Settings || s_page == Page::Controllers) && s_activeOverlayPaneFocus == OverlayPaneFocus::Left)
		{
			RequestOverlayPaneFocus(OverlayPaneFocus::Right);
		}
	}

	if (Pressed(buttons.x, s_previousButtons.x))
	{
		if (s_page == Page::Home && s_focusArea == FocusArea::HomeGrid &&
			s_selectedTitle >= 0 && s_selectedTitle < (int)titles.size())
		{
			OpenTitleContextMenu(titles[s_selectedTitle]);
		}
	}

	if (Pressed(buttons.y, s_previousButtons.y))
	{
		s_themeIndex = (s_themeIndex + 1) % ShellThemeCount();
		SaveFrontendState();
		PushStatus(TrFormat("Theme: {}", CurrentShellTheme().name));
	}

	if (Pressed(buttons.menu, s_previousButtons.menu))
		OpenPage(Page::Settings);

	if (Pressed(buttons.view, s_previousButtons.view))
		OpenPage(Page::TitleManager);
}

void ApplyStyle()
{
	UwpImGuiFrontend::SetWidgetTheme(CurrentShellTheme());
	UwpImGuiFrontend::ApplyShellStyle();
}

void PushShellPopupStyle()
{
	UwpImGuiFrontend::PushShellPopupStyle();
}

void PopShellPopupStyle()
{
	UwpImGuiFrontend::PopShellPopupStyle();
}

bool BeginShellPopup(const char* id)
{
	return UwpImGuiFrontend::BeginShellPopup(id);
}

void EndShellPopup()
{
	UwpImGuiFrontend::EndShellPopup();
}

void SectionTitle(const char* title)
{
	UwpImGuiFrontend::SectionTitle(title);
}

template <typename TValue, size_t N>
bool ComboEnum(const char* label, TValue& value, const std::array<std::pair<TValue, const char*>, N>& entries)
{
	std::size_t current = 0;
	std::vector<UwpImGuiFrontend::ChoiceItem> choices;
	choices.reserve(entries.size());
	for (std::size_t i = 0; i < entries.size(); ++i)
	{
		if (entries[i].first == value)
			current = i;
		choices.push_back({ std::to_string(i), Tr(entries[i].second), true });
	}
	if (!UwpImGuiFrontend::ShellCombo(label, &current, choices))
		return false;
	value = entries[current].first;
	return true;
}

template <typename TValue, size_t N>
bool ComboConfigEnum(const char* label, ConfigValue<TValue>& value, const std::array<std::pair<TValue, const char*>, N>& entries)
{
	TValue current = value.GetValue();
	if (!ComboEnum(label, current, entries))
		return false;
	value = current;
	SaveConfig();
	return true;
}

template <typename TValue, size_t N>
bool ComboConfigEnum(const char* label, ConfigValueBounds<TValue>& value, const std::array<std::pair<TValue, const char*>, N>& entries)
{
	TValue current = value.GetValue();
	if (!ComboEnum(label, current, entries))
		return false;
	value = current;
	SaveConfig();
	return true;
}

bool CheckboxConfig(const char* label, ConfigValue<bool>& value)
{
	bool current = value.GetValue();
	if (!ShellCheckbox(label, &current))
		return false;
	value = current;
	SaveConfig();
	return true;
}

bool SliderIntConfig(const char* label, sint32& value, int minValue, int maxValue, const char* format = "%d")
{
	int current = value;
	if (!ShellSliderInt(label, &current, minValue, maxValue, format))
		return false;
	value = current;
	SaveConfig();
	return true;
}

bool SliderConfigValueInt(const char* label, ConfigValue<int>& value, int minValue, int maxValue, const char* format = "%d")
{
	int current = value.GetValue();
	if (!ShellSliderInt(label, &current, minValue, maxValue, format))
		return false;
	value = current;
	SaveConfig();
	return true;
}

bool FloatConfig(const char* label, ConfigValue<float>& value, float minValue, float maxValue)
{
	float current = value.GetValue();
	if (!ShellSliderFloat(label, &current, minValue, maxValue, "%.2f"))
		return false;
	value = current;
	SaveConfig();
	return true;
}

bool StringConfig(const char* label, ConfigValue<std::string>& value)
{
	std::array<char, 512> buffer{};
	const std::string current = value.GetValue();
	strncpy(buffer.data(), current.c_str(), buffer.size() - 1);
	if (!UwpImGuiFrontend::NativeInputText(label, buffer.data(), buffer.size()))
		return false;
	value = std::string(buffer.data());
	SaveConfig();
	return true;
}

void QuickMenuBindingConfig(ConfigValue<std::string>& value)
{
	// NativeInputText reports every keystroke, so edits stay in a draft until Apply
	static std::array<char, 512> draft{};
	static std::string savedBinding;
	static bool initialized = false;
	const auto syncDraft = [&] {
		draft.fill('\0');
		savedBinding = value.GetValue();
		strncpy(draft.data(), savedBinding.c_str(), draft.size() - 1);
		initialized = true;
	};
	if (!initialized || savedBinding != value.GetValue())
		syncDraft();
	(void)UwpImGuiFrontend::NativeInputText(Tr("Quick menu combo"), draft.data(), draft.size());
	const bool valid = UwpQuickMenuBinding::Parse(draft.data()) != 0;
	ImGui::TextDisabled(Tr("Requires at least two different gamepad buttons held together."));
	ImGui::TextDisabled(Tr("Default: Gamepad:Menu+View. Custom examples: Gamepad:LB+RB, Gamepad:View+Y"));
	if (!valid)
		ImGui::TextWrapped(Tr("Invalid combo. Single buttons, duplicate buttons, keyboard and pointer shortcuts are not allowed. Your saved combo is unchanged."));
	ImGui::BeginDisabled(!valid);
	if (ShellButton(Tr("Apply Quick Menu combo")))
	{
		value = UwpQuickMenuBinding::Normalize(draft.data());
		syncDraft();
		SaveConfig();
		PushStatus(Tr("Quick Menu combo saved"));
	}
	ImGui::EndDisabled();
	if (ShellButton(Tr("Reset Quick Menu to Menu + View")))
	{
		value = std::string(UwpQuickMenuBinding::DefaultBinding);
		syncDraft();
		SaveConfig();
		PushStatus(Tr("Quick Menu combo reset to Menu + View"));
	}
}

bool PositionCombo(const char* label, ScreenPosition& position)
{
	constexpr std::array positions{
		std::pair{ ScreenPosition::kDisabled, "Disabled" },
		std::pair{ ScreenPosition::kTopLeft, "Top left" },
		std::pair{ ScreenPosition::kTopCenter, "Top center" },
		std::pair{ ScreenPosition::kTopRight, "Top right" },
		std::pair{ ScreenPosition::kBottomLeft, "Bottom left" },
		std::pair{ ScreenPosition::kBottomCenter, "Bottom center" },
		std::pair{ ScreenPosition::kBottomRight, "Bottom right" },
	};
	return ComboEnum(label, position, positions);
}

bool ColorEditU32(const char* label, uint32& color)
{
	ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
	if (!UwpImGuiFrontend::ShellColorEdit(label, &value, true))
		return false;
	color = ImGui::ColorConvertFloat4ToU32(value);
	SaveConfig();
	return true;
}

void DrawGeneralSettings()
{
	auto& config = GetConfig();
	SectionTitle(Tr("Interface"));
	const auto& uiLanguages = CemuUwp::Translation::GetAvailableLanguages();
	const std::string currentLanguage = config.uwp_display.ui_language.GetValue();
	std::string languageLabel = "English";
	for (const auto& language : uiLanguages)
	{
		if (language.code == currentLanguage)
		{
			languageLabel = language.name;
			break;
		}
	}
	if (ShellButton(fmt::format("{}: {}", Tr("Interface language"), languageLabel).c_str()))
		ImGui::OpenPopup("Interface language");
	if (BeginShellPopup("Interface language"))
	{
		for (const auto& language : uiLanguages)
		{
			if (ShellSelectable(language.name.c_str(), language.code == currentLanguage,
				ImVec2(300.0f, 42.0f)))
			{
				config.uwp_display.ui_language = language.code;
				CemuUwp::Translation::SetLanguage(language.code);
				CemuUwp::CompanionService::SetMetadataLanguage(MetadataLanguage());
				SaveConfig();
				ImGui::CloseCurrentPopup();
			}
			if (language.code == currentLanguage)
				ImGui::SetItemDefaultFocus();
		}
		EndShellPopup();
	}

	SectionTitle(Tr("System"));
	constexpr std::array languages{
		std::pair{ CafeConsoleLanguage::JA, "Japanese" },
		std::pair{ CafeConsoleLanguage::EN, "English" },
		std::pair{ CafeConsoleLanguage::FR, "French" },
		std::pair{ CafeConsoleLanguage::DE, "German" },
		std::pair{ CafeConsoleLanguage::IT, "Italian" },
		std::pair{ CafeConsoleLanguage::ES, "Spanish" },
		std::pair{ CafeConsoleLanguage::ZH, "Chinese" },
		std::pair{ CafeConsoleLanguage::KO, "Korean" },
		std::pair{ CafeConsoleLanguage::NL, "Dutch" },
		std::pair{ CafeConsoleLanguage::PT, "Portuguese" },
		std::pair{ CafeConsoleLanguage::RU, "Russian" },
		std::pair{ CafeConsoleLanguage::TW, "Taiwanese" },
	};
	ComboConfigEnum(Tr("Console language"), config.console_language, languages);

	constexpr std::array cpuModes{
		std::pair{ CPUMode::Auto, "Auto" },
		std::pair{ CPUMode::SinglecoreInterpreter, "Single-core interpreter" },
		std::pair{ CPUMode::SinglecoreRecompiler, "Single-core recompiler" },
		std::pair{ CPUMode::MulticoreRecompiler, "Multi-core recompiler" },
	};
	ComboConfigEnum(Tr("CPU mode"), config.cpu_mode, cpuModes);

	CheckboxConfig(Tr("Permanent storage"), config.permanent_storage);
	CheckboxConfig(Tr("Enable intro sound"), config.play_boot_sound);
	CheckboxConfig(Tr("Disable screen saver"), config.disable_screensaver);
}

void DrawGraphicsSettings()
{
	auto& config = GetConfig();
	SectionTitle(Tr("Renderer"));
	ImGui::BeginDisabled();
	(void)ShellButton(Tr("Graphics API: Direct3D 12"));
	ImGui::EndDisabled();

	constexpr std::array vsyncModes{
		std::pair{ 0, "Off" },
		std::pair{ 1, "Every refresh" },
		std::pair{ 2, "Every second refresh" },
		std::pair{ 3, "Every third refresh" },
	};
	int vsync = config.vsync.GetValue();
	if (ComboEnum(Tr("VSync"), vsync, vsyncModes))
	{
		config.vsync = vsync;
		SaveConfig();
	}
	CheckboxConfig(Tr("Async shader compile"), config.async_compile);
	CheckboxConfig(Tr("Threaded draw recording (experimental, restart Cemu)"), config.d3d12_threaded_recording);
	if (config.d3d12_threaded_recording.GetValue())
		CheckboxConfig(Tr("Two-stage recording (experimental, restart Cemu)"), config.d3d12_two_stage_recording);
	CheckboxConfig(Tr("Prioritized pipeline caching (experimental, applies when a game starts)"), config.d3d12_prioritized_pipeline_cache);
	ImGui::TextDisabled("%s", Tr("Loads every cached pipeline that fits in memory at game start, most used first, and keeps the most used ones under memory pressure. Longer loading, less pop-in."));
	CheckboxConfig(Tr("Full sync at GX2DrawDone()"), config.gx2drawdone_sync);
	CheckboxConfig(Tr("Detailed performance logs (applies when a game starts)"), config.uwp_detailed_profiling);
	ImGui::TextDisabled("%s", Tr("Writes CPU-time splits to log.txt every 5 s. Costs some performance; leave off unless measuring."));
	CheckboxConfig(Tr("Render upside down"), config.render_upside_down);

	// console storage is tight; per-title removal lives in the title menu
	SectionTitle(Tr("Shader caches"));
	static std::optional<uint64> s_allShaderCacheBytes;
	static bool s_confirmAllShaderCacheRemoval = false;
	const fs::path shaderCacheRoot = ActiveSettings::GetCachePath("shaderCache");
	if (!s_allShaderCacheBytes)
		s_allShaderCacheBytes = GetShaderCacheBytes({ shaderCacheRoot });
	ImGui::TextWrapped("%s", TrFormat("All titles: {:.1f} MB", (double)*s_allShaderCacheBytes / (1024.0 * 1024.0)).c_str());
	if (CafeSystem::IsTitleRunning())
	{
		s_confirmAllShaderCacheRemoval = false;
		ImGui::TextDisabled("%s", Tr("Quit the game to remove shader caches."));
	}
	else if (ShellButton(s_confirmAllShaderCacheRemoval ? Tr("Press again to remove every shader cache") : Tr("Remove all shader caches")))
	{
		if (!s_confirmAllShaderCacheRemoval)
			s_confirmAllShaderCacheRemoval = true;
		else
		{
			s_confirmAllShaderCacheRemoval = false;
			size_t failed = 0;
			std::error_code ec;
			for (const auto& entry : fs::directory_iterator(shaderCacheRoot, ec))
			{
				std::error_code removeError;
				fs::remove_all(entry.path(), removeError);
				if (removeError)
				{
					failed++;
					cemuLog_log(LogType::Force, "UWP failed to remove {}: {}", _pathToUtf8(entry.path()), removeError.message());
				}
			}
			s_allShaderCacheBytes.reset();
			PushStatus(failed == 0 ? std::string(Tr("Removed all shader caches")) : TrFormat("Removed shader caches; {} items failed", failed));
		}
	}

	// backup to a USB drive or any folder the picker can reach; the copy runs off the UI thread
	static std::future<std::string> s_shaderCacheCopy;
	if (s_shaderCacheCopy.valid() && s_shaderCacheCopy.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		PushStatus(s_shaderCacheCopy.get());
		s_allShaderCacheBytes.reset();
	}
	auto pickCacheFolder = [shaderCacheRoot](bool exportCaches)
	{
		UwpImGuiFrontend::PickerRequest request;
		request.startLocation = UwpImGuiFrontend::PickerStartLocation::Computer;
		request.fileTypes = { L"*" };
		request.futureAccessToken = L"shader-cache-backup";
		const bool started = PickerService().PickFolder(std::move(request), MakePickerCallbacks(
			[shaderCacheRoot, exportCaches](std::unique_ptr<UwpImGuiFrontend::PickedItem> item) {
				if (!item)
					return;
				const fs::path picked = item->path;
				s_shaderCacheCopy = std::async(std::launch::async, [shaderCacheRoot, picked, exportCaches]() -> std::string {
					std::error_code ec;
					if (exportCaches)
					{
						const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
						std::tm local{};
						localtime_s(&local, &now);
						const fs::path target = picked / fmt::format("Cemu shader caches {:04}-{:02}-{:02} {:02}-{:02}",
							local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
						fs::copy(shaderCacheRoot, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
						if (ec)
							cemuLog_log(LogType::Force, "UWP shader cache export to {} failed: {}", _pathToUtf8(target), ec.message());
						return ec ? std::string(Tr("Shader cache export failed; see log.txt")) : TrFormat("Shader caches exported to {}", _pathToUtf8(target));
					}
					// an export folder holds precompiled/ and transferable/ like the cache folder itself
					if (!fs::exists(picked / "transferable", ec) && !fs::exists(picked / "precompiled", ec))
						return Tr("That folder has no exported shader caches");
					fs::copy(picked, shaderCacheRoot, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
					if (ec)
						cemuLog_log(LogType::Force, "UWP shader cache import from {} failed: {}", _pathToUtf8(picked), ec.message());
					return ec ? std::string(Tr("Shader cache import failed; see log.txt")) : std::string(Tr("Shader caches imported"));
				});
			},
			[](std::string_view error) {
				cemuLog_log(LogType::Force, "UWP shader cache folder picker failed: {}", error);
				PushStatus(Tr("Could not open the folder picker"));
			}));
		if (!started)
			PushStatus(Tr("A file or folder picker is already open"));
	};
	if (s_shaderCacheCopy.valid())
		ImGui::TextDisabled("%s", Tr("Copying shader caches..."));
	else if (CafeSystem::IsTitleRunning())
		ImGui::TextDisabled("%s", Tr("Quit the game to export or import shader caches."));
	else
	{
		if (ShellButton(Tr("Export shader caches to a folder or USB drive")))
			pickCacheFolder(true);
		if (ShellButton(Tr("Import shader caches from an exported folder")))
			pickCacheFolder(false);
	}

	SectionTitle(Tr("Graphic packs"));
	CheckboxConfig(Tr("Skip resolution rules from packs whose shaders cannot run on this renderer"),
		config.skip_resolution_rules_of_packs_with_skipped_shaders);
	ImGui::TextDisabled("%s", Tr("Takes effect the next time a title starts."));

	SectionTitle(Tr("Scaling"));
	constexpr std::array upscaleFilters{
		std::pair{ (sint32)kLinearFilter, "Bilinear" },
		std::pair{ (sint32)kBicubicFilter, "Bicubic" },
		std::pair{ (sint32)kBicubicHermiteFilter, "Hermite" },
		std::pair{ (sint32)kNearestNeighborFilter, "Nearest neighbor" },
		std::pair{ (sint32)kFsrFilter, "AMD FidelityFX Super Resolution 1.0" },
	};
	// FSR only upscales
	constexpr std::array downscaleFilters{
		std::pair{ (sint32)kLinearFilter, "Bilinear" },
		std::pair{ (sint32)kBicubicFilter, "Bicubic" },
		std::pair{ (sint32)kBicubicHermiteFilter, "Hermite" },
		std::pair{ (sint32)kNearestNeighborFilter, "Nearest neighbor" },
	};
	sint32 upscale = config.upscale_filter.GetValue();
	if (ComboEnum(Tr("Upscale filter"), upscale, upscaleFilters))
	{
		config.upscale_filter = upscale;
		SaveConfig();
	}
	if (config.upscale_filter.GetValue() == kFsrFilter)
	{
		SliderConfigValueInt(Tr("FSR sharpness"), config.fsr_sharpness, 0, 100, "%d%%");
		ImGui::TextDisabled("%s", Tr("FSR upscales and sharpens when the game image is smaller than the screen area, and sharpens at the same size. Larger images use the downscale filter."));
	}
	sint32 downscale = config.downscale_filter.GetValue();
	if (ComboEnum(Tr("Downscale filter"), downscale, downscaleFilters))
	{
		config.downscale_filter = downscale;
		SaveConfig();
	}
	constexpr std::array scalingModes{
		std::pair{ (sint32)kKeepAspectRatio, "Keep aspect ratio" },
		std::pair{ (sint32)kStretch, "Stretch" },
	};
	sint32 scaling = config.fullscreen_scaling.GetValue();
	if (ComboEnum(Tr("Fullscreen scaling"), scaling, scalingModes))
	{
		config.fullscreen_scaling = scaling;
		SaveConfig();
	}

	SectionTitle(Tr("Gamma"));
	CheckboxConfig(Tr("Override application gamma"), config.overrideAppGammaPreference);
	FloatConfig(Tr("Target gamma"), config.overrideGammaValue, 0.1f, 4.0f);
	bool srgb = config.userDisplayGamma.GetValue() == 0.0f;
	if (ShellCheckbox(Tr("Display uses sRGB curve"), &srgb))
	{
		config.userDisplayGamma = srgb ? 0.0f : 2.2f;
		SaveConfig();
	}
	if (!srgb)
		FloatConfig(Tr("Display gamma"), config.userDisplayGamma, 0.1f, 4.0f);

	SectionTitle(Tr("GamePad screen"));
	UwpScreenLayout defaultLayout = static_cast<UwpScreenLayout>(ClampLayout(config.uwp_display.default_layout.GetValue()));
	if (ComboEnum(Tr("Global default layout"), defaultLayout, kUwpLayoutEntries))
	{
		config.uwp_display.default_layout = static_cast<sint32>(defaultLayout);
		SaveConfig();
	}
	CheckboxConfig(Tr("Swap TV and GamePad screens"), config.uwp_display.swap_screens);
	CheckboxConfig(Tr("Show the GamePad touch cursor"), config.uwp_display.touch_cursor);
	StringConfig("Swap screens binding", config.uwp_display.toggle_swap_binding);
	ImGui::TextDisabled(Tr("Swap examples: Gamepad:View+Y, Keyboard:Escape, Pointer:RightClick"));
	QuickMenuBindingConfig(config.uwp_display.quick_menu_binding);

	if (CafeSystem::IsTitleRunning())
	{
		const uint64 titleId = CafeSystem::GetForegroundTitleId();
		auto& entry = GetOrCreateUwpGameLayout(titleId);
		UwpScreenLayout gameLayout = static_cast<UwpScreenLayout>(ClampLayout(entry.layout));
		SectionTitle(Tr("Current game override"));
		ImGui::Text(Tr("Title ID: %016llX"), (unsigned long long)titleId);
		if (ComboEnum(Tr("Game layout"), gameLayout, kUwpLayoutEntries))
		{
			entry.layout = static_cast<sint32>(gameLayout);
			SaveConfig();
		}
		const std::string title = fmt::format("{:016x}", titleId);
		const fs::path sideBySidePath = ActiveSettings::GetUserDataPath("titleImages/{}/side_by_side.png", title);
		const fs::path stackedPath = ActiveSettings::GetUserDataPath("titleImages/{}/stacked.png", title);
		ImGui::TextDisabled(Tr("Underlays use titleImages/%s/*.png"), title.c_str());
		ImGui::Text(Tr("Side-by-side underlay: %s"), fs::exists(sideBySidePath) ? Tr("Set") : Tr("Missing"));
		if (ShellButton(Tr("Choose side-by-side underlay")))
			PickUnderlayAsync(titleId, L"side_by_side.png", "Side-by-side");
		ImGui::SameLine();
		if (ShellButton(Tr("Clear side-by-side")))
			ClearUnderlay(titleId, "side_by_side.png", "Side-by-side");
		ImGui::Text(Tr("Stacked underlay: %s"), fs::exists(stackedPath) ? Tr("Set") : Tr("Missing"));
		if (ShellButton(Tr("Choose stacked underlay")))
			PickUnderlayAsync(titleId, L"stacked.png", "Stacked");
		ImGui::SameLine();
		if (ShellButton(Tr("Clear stacked")))
			ClearUnderlay(titleId, "stacked.png", "Stacked");
	}
}

void DrawAudioDeviceCombo(const char* label, std::wstring& target)
{
	const auto api = (IAudioAPI::AudioAPI)GetConfig().audio_api;
	const auto devices = IAudioAPI::GetDevices(api);
	std::string preview = target.empty() ? "Default" : Narrow(target);
	if (preview.empty())
		preview = "Default";
	if (ShellButton(fmt::format("{}: {}", label, preview).c_str()))
		ImGui::OpenPopup(label);
	if (!BeginShellPopup(label))
		return;
	if (ShellSelectable(Tr("Default"), target.empty(), ImVec2(360.0f, 42.0f)))
	{
		target.clear();
		SaveConfig();
		ImGui::CloseCurrentPopup();
	}
	if (target.empty())
		ImGui::SetItemDefaultFocus();
	for (const auto& device : devices)
	{
		const std::wstring id = device->GetIdentifier();
		const std::string name = Narrow(device->GetName());
		const bool selected = id == target;
		if (ShellSelectable(name.empty() ? "Unnamed device" : name.c_str(), selected, ImVec2(360.0f, 42.0f)))
		{
			target = id;
			SaveConfig();
			ImGui::CloseCurrentPopup();
		}
		if (selected)
			ImGui::SetItemDefaultFocus();
	}
	EndShellPopup();
}

void DrawAudioInputDeviceCombo(const char* label, std::wstring& target)
{
	const auto devices = IAudioInputAPI::GetDevices(IAudioInputAPI::Cubeb);
	std::string preview = target.empty() ? "Default" : Narrow(target);
	if (preview.empty())
		preview = "Default";
	if (ShellButton(fmt::format("{}: {}", label, preview).c_str()))
		ImGui::OpenPopup(label);
	if (!BeginShellPopup(label))
		return;
	if (ShellSelectable(Tr("Default"), target.empty(), ImVec2(360.0f, 42.0f)))
	{
		target.clear();
		SaveConfig();
		ImGui::CloseCurrentPopup();
	}
	if (target.empty())
		ImGui::SetItemDefaultFocus();
	for (const auto& device : devices)
	{
		const std::wstring id = device->GetIdentifier();
		const std::string name = Narrow(device->GetName());
		const bool selected = id == target;
		if (ShellSelectable(name.empty() ? "Unnamed input" : name.c_str(), selected, ImVec2(360.0f, 42.0f)))
		{
			target = id;
			SaveConfig();
			ImGui::CloseCurrentPopup();
		}
		if (selected)
			ImGui::SetItemDefaultFocus();
	}
	EndShellPopup();
}

void DrawAudioSettings()
{
	CemuHost::EnsureAudioInitialized();
	auto& config = GetConfig();
	SectionTitle(Tr("Backend"));
	constexpr std::array audioApis{
		std::pair{ IAudioAPI::DirectSound, "DirectSound" },
		std::pair{ IAudioAPI::XAudio27, "XAudio2.7" },
		std::pair{ IAudioAPI::XAudio2, "XAudio2" },
		std::pair{ IAudioAPI::Cubeb, "Cubeb" },
	};
	auto currentApi = (IAudioAPI::AudioAPI)config.audio_api;
	if (ShellButton(fmt::format("Audio API: {}", audioApis[std::clamp((int)currentApi, 0, (int)audioApis.size() - 1)].second).c_str()))
		ImGui::OpenPopup("Audio API");
	if (BeginShellPopup("Audio API"))
	{
		for (const auto& [api, name] : audioApis)
		{
			const bool available = IAudioAPI::IsAudioAPIAvailable(api);
			ImGui::BeginDisabled(!available);
			if (ShellSelectable(name, currentApi == api, ImVec2(260.0f, 42.0f)) && available)
			{
				config.audio_api = api;
				SaveConfig();
				ImGui::CloseCurrentPopup();
			}
			if (currentApi == api && available)
				ImGui::SetItemDefaultFocus();
			ImGui::EndDisabled();
		}
		EndShellPopup();
	}
	SliderIntConfig(Tr("Audio latency"), config.audio_delay, 0, 16, "%d frames");

	SectionTitle(Tr("TV"));
	constexpr std::array channelModes{
		std::pair{ kMono, "Mono" },
		std::pair{ kStereo, "Stereo" },
		std::pair{ kSurround, "Surround" },
	};
	if (ComboEnum(Tr("TV channels"), config.tv_channels, channelModes))
		SaveConfig();
	SliderIntConfig(Tr("TV volume"), config.tv_volume, 0, 100, "%d%%");
	DrawAudioDeviceCombo(Tr("TV device"), config.tv_device);

	SectionTitle(Tr("GamePad"));
	if (ComboEnum(Tr("GamePad channels"), config.pad_channels, channelModes))
		SaveConfig();
	SliderIntConfig(Tr("GamePad volume"), config.pad_volume, 0, 100, "%d%%");
	DrawAudioDeviceCombo(Tr("GamePad device"), config.pad_device);

	SectionTitle(Tr("Microphone and portal"));
	if (ComboEnum(Tr("Input channels"), config.input_channels, channelModes))
		SaveConfig();
	SliderIntConfig(Tr("Input volume"), config.input_volume, 0, 100, "%d%%");
	DrawAudioInputDeviceCombo("Input device", config.input_device);
	SliderIntConfig(Tr("Portal volume"), config.portal_volume, 0, 100, "%d%%");
	DrawAudioDeviceCombo(Tr("Portal device"), config.portal_device);
}

void DrawOverlaySettings()
{
	auto& config = GetConfig();
	SectionTitle(Tr("UWP shell"));
	if (ShellButton(fmt::format("{}: {}", Tr("Shell theme"), CurrentShellTheme().name).c_str()))
		ImGui::OpenPopup("Shell theme");
	if (BeginShellPopup("Shell theme"))
	{
		const auto& themes = ShellThemes();
		for (int i = 0; i < (int)themes.size(); ++i)
		{
			const bool selected = i == s_themeIndex;
			if (ShellSelectable(themes[i].name, selected, ImVec2(260.0f, 42.0f)))
			{
				s_themeIndex = i;
				SaveFrontendState();
				PushStatus(TrFormat("Theme: {}", CurrentShellTheme().name));
				ImGui::CloseCurrentPopup();
			}
			if (selected)
				ImGui::SetItemDefaultFocus();
		}
		EndShellPopup();
	}
	if (ShellButton(Tr("Show controls hint"), ImVec2(190.0f, 42.0f)))
	{
		s_showActionHints = true;
		s_actionHintsTimer = 0.0f;
		s_actionHintsSeen = true;
		SaveFrontendState();
	}

	SectionTitle(Tr("Overlay"));
	if (PositionCombo("Overlay position", config.overlay.position))
		SaveConfig();
	ColorEditU32("Overlay color", config.overlay.text_color);
	SliderIntConfig(Tr("Overlay scale"), config.overlay.text_scale, 50, 200, "%d%%");
	if (ShellCheckbox(Tr("FPS"), &config.overlay.fps)) SaveConfig();
	if (ShellCheckbox(Tr("Draw calls"), &config.overlay.drawcalls)) SaveConfig();
	if (ShellCheckbox(Tr("CPU usage"), &config.overlay.cpu_usage)) SaveConfig();
	if (ShellCheckbox(Tr("RAM usage"), &config.overlay.ram_usage)) SaveConfig();
	if (ShellCheckbox(Tr("VRAM usage"), &config.overlay.vram_usage)) SaveConfig();
	if (ShellCheckbox(Tr("Debug overlay"), &config.overlay.debug)) SaveConfig();

	SectionTitle(Tr("Notifications"));
	if (PositionCombo("Notification position", config.notification.position))
		SaveConfig();
	ColorEditU32("Notification color", config.notification.text_color);
	SliderIntConfig(Tr("Notification scale"), config.notification.text_scale, 50, 200, "%d%%");
	if (ShellCheckbox(Tr("Controller profile notifications"), &config.notification.controller_profiles)) SaveConfig();
	if (ShellCheckbox(Tr("Controller battery notifications"), &config.notification.controller_battery)) SaveConfig();
	if (ShellCheckbox(Tr("Shader compiling notifications"), &config.notification.shader_compiling)) SaveConfig();
	if (ShellCheckbox(Tr("Friend service notifications"), &config.notification.friends)) SaveConfig();
}

void DrawAccountSettings()
{
	auto& config = GetConfig();
	SectionTitle(Tr("Account"));
	const auto& accounts = Account::GetAccounts();
	uint32 active = config.account.m_persistent_id.GetValue();
	std::string preview = fmt::format("{:08x}", active);
	for (const auto& account : accounts)
	{
		if (account.GetPersistentId() == active)
		{
			preview = Narrow(account.ToString());
			break;
		}
	}
	if (ShellButton(fmt::format("Active account: {}", preview).c_str()))
		ImGui::OpenPopup("Active account");
	if (BeginShellPopup("Active account"))
	{
		for (const auto& account : accounts)
		{
			const uint32 id = account.GetPersistentId();
			const std::string label = Narrow(account.ToString());
			const std::string row = label.empty() ? fmt::format("{:08x}", id) : label;
			if (ShellSelectable(row.c_str(), active == id, ImVec2(300.0f, 42.0f)))
			{
				config.account.m_persistent_id = id;
				SaveConfig();
				ImGui::CloseCurrentPopup();
			}
			if (active == id)
				ImGui::SetItemDefaultFocus();
		}
		EndShellPopup();
	}

	constexpr std::array services{
		std::pair{ NetworkService::Offline, "Offline" },
		std::pair{ NetworkService::Nintendo, "Nintendo" },
		std::pair{ NetworkService::Pretendo, "Pretendo" },
		std::pair{ NetworkService::Custom, "Custom" },
	};
	NetworkService service = config.GetAccountNetworkService(config.account.m_persistent_id.GetValue());
	if (ComboEnum(Tr("Network service"), service, services))
	{
		config.SetAccountSelectedService(config.account.m_persistent_id.GetValue(), service);
		SaveConfig();
	}

	const Account& account = Account::GetCurrentAccount();
	constexpr size_t kMaxMiiNameLength = 10;
	static std::array<char, 64> s_miiNameBuffer{};
	static uint32 s_miiNameAccount = 0;
	if (s_miiNameAccount != account.GetPersistentId())
	{
		s_miiNameAccount = account.GetPersistentId();
		const std::string current = Narrow(account.GetMiiName());
		s_miiNameBuffer.fill('\0');
		std::memcpy(s_miiNameBuffer.data(), current.data(),
			std::min(current.size(), s_miiNameBuffer.size() - 1));
	}
	(void)UwpImGuiFrontend::NativeInputText(Tr("Nickname"), s_miiNameBuffer.data(),
		s_miiNameBuffer.size(), 0, UwpImGuiFrontend::TextInputScope::UserName);
	if (ShellButton(Tr("Save nickname"), ImVec2(220.0f, 42.0f)))
	{
		std::wstring name = Widen(s_miiNameBuffer.data());
		if (name.empty())
			name = L"default";
		if (name.size() > kMaxMiiNameLength)
			name.resize(kMaxMiiNameLength);
		Account edited = account;
		edited.SetMiiName(name);
		if (edited.Save())
			PushStatus(Tr("Could not save the nickname"));
		else
		{
			Account::RefreshAccounts();
			s_miiNameAccount = 0;
			PushStatus(Tr("Nickname saved"));
			return; // RefreshAccounts invalidates the account reference
		}
	}
	ImGui::TextDisabled(Tr("Up to %d characters, used as the console profile name"), (int)kMaxMiiNameLength);
	ImGui::Text(Tr("Persistent ID: %08x"), account.GetPersistentId());
	ImGui::Text(Tr("Account ID: %s"), std::string(account.GetAccountId()).empty() ? Tr("(none)") : std::string(account.GetAccountId()).c_str());
	const OnlineValidator validator = account.ValidateOnlineFiles();
	ImGui::Text(Tr("Online files: %s"), validator.IsValid() ? Tr("valid") : Tr("missing or incomplete"));
	if (!validator.IsValid())
	{
		if (validator.otp != OnlineValidator::FileState::Ok)
			ImGui::TextDisabled(Tr("otp.bin is missing or invalid"));
		if (validator.seeprom != OnlineValidator::FileState::Ok)
			ImGui::TextDisabled(Tr("seeprom.bin is missing or invalid"));
		if (!validator.valid_account)
			ImGui::TextDisabled(Tr("Selected account is not a complete online account"));
	}
}

void DrawDebugSettings()
{
	auto& config = GetConfig();
	SectionTitle(Tr("Debugger"));
	// the console writes crash dumps itself, so there is no in-app dump setting
	ImGui::TextWrapped(Tr("Crash dumps are written by the console. Enable Crash Dumps for Cemu in Device Portal > Crash data, then download them from Device Portal > Crash dumps."));
	int gdbPort = config.gdb_port.GetValue();
	if (UwpImGuiFrontend::ShellIntegerStepper(Tr("GDB port"), &gdbPort, 1, 65535))
	{
		config.gdb_port = (uint16)std::clamp(gdbPort, 1, 65535);
		SaveConfig();
	}

	SectionTitle(Tr("Runtime dumps"));
	bool dumpShaders = ActiveSettings::DumpShadersEnabled();
	if (ShellCheckbox(Tr("Dump shaders"), &dumpShaders))
		ActiveSettings::EnableDumpShaders(dumpShaders);
	bool dumpTextures = ActiveSettings::DumpTexturesEnabled();
	if (ShellCheckbox(Tr("Dump textures"), &dumpTextures))
		ActiveSettings::EnableDumpTextures(dumpTextures);
	bool dumpRecompiler = ActiveSettings::DumpRecompilerFunctionsEnabled();
	if (ShellCheckbox(Tr("Dump recompiler functions"), &dumpRecompiler))
		ActiveSettings::EnableDumpRecompilerFunctions(dumpRecompiler);
	bool dumpCurl = ActiveSettings::DumpLibcurlRequestsEnabled();
	if (ShellCheckbox(Tr("Dump libcurl requests"), &dumpCurl))
		ActiveSettings::EnableDumpLibcurlRequests(dumpCurl);
	CheckboxConfig(Tr("Advanced PPC logging"), config.advanced_ppc_logging);
}

void DrawStorageSettings()
{
	auto& config = GetConfig();
	SectionTitle(Tr("UWP storage"));
	const auto mlcLocations = UwpImGuiFrontend::MakeXboxStorageLocationPresets(
		L"D:\\DevelopmentFiles\\Cemu\\mlc01", L"E:\\Cemu\\mlc01");
	UwpImGuiFrontend::StorageLocationSelectorModel mlcModel;
	mlcModel.id = "mlc-storage-location";
	mlcModel.label = Tr("MLC storage");
	mlcModel.currentPath = ActiveSettings::GetMlcPath();
	mlcModel.presets = mlcLocations;
	mlcModel.currentPathLabel = "MLC path";
	mlcModel.customPicker.futureAccessToken = L"mlc-folder";
	(void)UwpImGuiFrontend::DrawStorageLocationSelector(mlcModel,
		PickerService(), {
			.selected = [](UwpImGuiFrontend::StorageLocationSelection selection) {
				ApplyMlcPath(selection.path);
			},
			.failed = [](std::string_view error) {
				cemuLog_log(LogType::Force, "UWP MLC folder picker failed: {}", error);
				PushStatus(Tr("Could not open the MLC folder picker"));
			},
			.pickerOpenChanged = [](bool open) { SetPickerOpen(open); },
		});
	const auto graphicPackLocations = UwpImGuiFrontend::MakeXboxStorageLocationPresets(
		L"D:\\DevelopmentFiles\\Cemu\\graphicPacks", L"E:\\Cemu\\graphicPacks");
	UwpImGuiFrontend::StorageLocationSelectorModel graphicPacksModel;
	graphicPacksModel.id = "graphic-packs-storage-location";
	graphicPacksModel.label = Tr("Graphic packs folder");
	graphicPacksModel.currentPath = ActiveSettings::GetGraphicPacksPath();
	graphicPacksModel.presets = graphicPackLocations;
	graphicPacksModel.currentPathLabel = "Graphic packs path";
	graphicPacksModel.customPicker.futureAccessToken = L"graphic-packs-folder";
	(void)UwpImGuiFrontend::DrawStorageLocationSelector(graphicPacksModel,
		PickerService(), {
			.selected = [](UwpImGuiFrontend::StorageLocationSelection selection) {
				ApplyGraphicPacksPath(selection.path);
			},
			.failed = [](std::string_view error) {
				cemuLog_log(LogType::Force, "UWP graphic packs folder picker failed: {}", error);
				PushStatus(Tr("Could not open the graphic packs folder picker"));
			},
			.pickerOpenChanged = [](bool open) { SetPickerOpen(open); },
		});
	if (!config.graphic_packs_path.GetValue().empty() &&
		ShellButton(Tr("Use app storage for graphic packs"), ImVec2(320.0f, 42.0f)))
	{
		ApplyGraphicPacksPath({});
	}
	if (ShellButton(Tr("Open title"), ImVec2(220.0f, 42.0f)) && !s_pickerOpen)
		PickTitleAsync();
	const auto gameLocations = UwpImGuiFrontend::MakeXboxStorageLocationPresets(
		L"D:\\DevelopmentFiles\\Games\\wiiu", L"E:\\Games\\wiiu");
	UwpImGuiFrontend::StorageLocationSelectorModel gamesModel;
	gamesModel.id = "primary-game-folder-location";
	gamesModel.label = Tr("Games directory");
	gamesModel.currentPath = config.game_paths.empty() ? fs::path{} :
		_utf8ToPath(config.game_paths.front());
	gamesModel.presets = gameLocations;
	gamesModel.currentPathLabel = "Games path";
	gamesModel.customPicker.futureAccessToken = L"primary-game-folder";
	(void)UwpImGuiFrontend::DrawStorageLocationSelector(gamesModel,
		PickerService(), {
			.selected = [](UwpImGuiFrontend::StorageLocationSelection selection) {
				SetPrimaryGameFolder(selection.path);
			},
			.failed = [](std::string_view error) {
				cemuLog_log(LogType::Force, "UWP game folder picker failed: {}", error);
				PushStatus(Tr("Could not open the game folder picker"));
			},
			.pickerOpenChanged = [](bool open) { SetPickerOpen(open); },
		});

	UwpImGuiFrontend::StorageLocationSelectorModel additionalGamesModel;
	additionalGamesModel.id = "additional-game-folder-location";
	additionalGamesModel.label = Tr("Add another game folder");
	additionalGamesModel.presets = gameLocations;
	additionalGamesModel.promptLabel = "Choose location";
	additionalGamesModel.showCurrentPath = false;
	additionalGamesModel.customPicker.futureAccessToken = L"additional-game-folder";
	(void)UwpImGuiFrontend::DrawStorageLocationSelector(additionalGamesModel,
		PickerService(), {
			.selected = [](UwpImGuiFrontend::StorageLocationSelection selection) {
				AddGameFolder(selection.path);
			},
			.failed = [](std::string_view error) {
				cemuLog_log(LogType::Force,
					"UWP additional game folder picker failed: {}", error);
				PushStatus(Tr("Could not open the game folder picker"));
			},
			.pickerOpenChanged = [](bool open) { SetPickerOpen(open); },
		});
	if (ShellButton(Tr("Refresh list"), ImVec2(220.0f, 42.0f)))
		RefreshTitleList();

	for (size_t i = 1; i < config.game_paths.size(); ++i)
	{
		ImGui::PushID((int)i);
		ImGui::TextWrapped("%s", config.game_paths[i].c_str());
		ImGui::SameLine();
		if (ShellButton(Tr("Remove"), ImVec2(110.0f, 34.0f)))
		{
			config.game_paths.erase(config.game_paths.begin() + i);
			CafeTitleList::ClearScanPaths();
			for (auto& path : config.game_paths)
				CafeTitleList::AddScanPath(_utf8ToPath(path));
			CafeTitleList::Refresh();
			MarkTitleCacheDirty();
			SaveConfig();
			ImGui::PopID();
			break;
		}
		ImGui::PopID();
	}
}

void DrawInputSettings()
{
	auto& config = GetConfig();

	SectionTitle(Tr("Virtual gyro"));
	ImGui::TextWrapped(Tr("For games that need GamePad motion: while on, the right stick tilts the GamePad instead of moving the camera. It is always available from the quick menu."));
	CheckboxConfig(Tr("Toggle the virtual gyro with a button combo"), config.uwp_display.virtual_gyro_combo_enabled);
	if (config.uwp_display.virtual_gyro_combo_enabled.GetValue())
	{
		const std::string binding = config.uwp_display.virtual_gyro_binding.GetValue();
		if (s_gyroComboCapture == GyroComboCapture::Recording)
		{
			const std::string held = GamepadComboString(s_gyroComboCaptured);
			ImGui::TextWrapped(Tr("Hold the buttons of the combo, then release them all (%d s)"),
				(int)std::ceil(std::max(0.0f, kGyroComboCaptureTimeout - s_gyroComboCaptureSeconds)));
			if (!held.empty())
				ImGui::TextUnformatted(held.c_str());
		}
		else if (s_gyroComboCapture == GyroComboCapture::WaitRelease)
		{
			ImGui::TextWrapped(Tr("Release all buttons..."));
		}
		else
		{
			ImGui::Text(Tr("Combo: %s"), binding.empty() ? Tr("none") : binding.c_str());
			if (ShellButton(Tr("Record combo"), ImVec2(220.0f, 40.0f)))
			{
				s_gyroComboCapture = GyroComboCapture::WaitRelease;
				s_gyroComboCaptureSeconds = 0.0f;
			}
		}
		ImGui::TextDisabled(Tr("The game also receives these buttons, so pick a combo that does nothing harmful in it. Up/Down/Left/Right also react to the left stick."));
	}

	SectionTitle(Tr("Emulated USB devices"));
	CheckboxConfig(Tr("Emulate Skylander Portal"), config.emulated_usb_devices.emulate_skylander_portal);
	CheckboxConfig(Tr("Emulate Disney Infinity Base"), config.emulated_usb_devices.emulate_infinity_base);
	CheckboxConfig(Tr("Emulate LEGO Dimensions Toy Pad"), config.emulated_usb_devices.emulate_dimensions_toypad);
}

bool IsListArtworkMissing(const TitleTile& title);

CemuUwp::ScreenScraperDownloader::Request MakeScreenScraperRequest()
{
	EnsureScreenScraperCredentialBuffers();
	auto& scraper = GetConfig().uwp_screenscraper;
	CemuUwp::ScreenScraperDownloader::Request request{};
	request.user = s_screenScraperUserBuffer.data();
	request.password = s_screenScraperPasswordBuffer.data();
	request.downloadMetadata = scraper.download_metadata.GetValue();
	request.downloadBoxArt = scraper.download_box_art.GetValue();
	request.downloadFanart = scraper.download_fanart.GetValue();
	request.downloadVideos = scraper.download_videos.GetValue();
	request.downloadScreenshots = scraper.download_screenshots.GetValue();
	request.downloadLogos = scraper.download_logos.GetValue();
	request.overwriteExisting = scraper.overwrite_existing.GetValue();
	request.preferredRegion = scraper.preferred_region.GetValue();
	request.metadataLanguage = MetadataLanguage();
	return request;
}

CemuUwp::ScreenScraperDownloader::Request BuildScreenScraperRequestForTitle(
	uint64 titleId, uint64 matchedGameId, std::string name,
	std::string region, fs::path path)
{
	auto request = MakeScreenScraperRequest();
	request.overwriteExisting = true;
	request.titles.push_back({
		.titleId = titleId,
		.matchedGameId = matchedGameId,
		.name = std::move(name),
		.region = std::move(region),
		.path = std::move(path),
	});
	return request;
}

CemuUwp::ScreenScraperDownloader::Request BuildScreenScraperRequest()
{
	auto request = MakeScreenScraperRequest();

	const auto& titles = GetCachedHomeTitles();
	const auto addTitle = [&request](const TitleTile& title) {
		if (title.titleId == 0 || !title.launchable)
			return;
		request.titles.push_back({
			title.titleId,
			0,
			title.name,
			title.region,
			title.path,
		});
	};

	request.titles.reserve(titles.size());
	for (const auto& title : titles)
		addTitle(title);
	return request;
}

void StartScreenScraperDownload()
{
	auto& scraper = GetConfig().uwp_screenscraper;
	if (!scraper.enabled.GetValue())
	{
		PushStatus(Tr("Enable Screenscraper first"));
		return;
	}

	auto request = BuildScreenScraperRequest();
	if (request.user.empty() || request.password.empty())
	{
		PushStatus(Tr("Sign in to Screenscraper first"));
		return;
	}
	if (request.titles.empty())
	{
		PushStatus(Tr("No titles to scrape"));
		return;
	}

	if (ScreenScraperCredentialStore().Save({ request.user, request.password }))
		s_screenScraperCredentialBuffersLoaded = true;
	if (!CemuUwp::ScreenScraperDownloader::Start(std::move(request)))
		PushStatus(Tr("Screenscraper download is already running"));
}

void DrawScreenScraperSettings()
{
	auto& config = GetConfig();
	auto& scraper = config.uwp_screenscraper;
	EnsureScreenScraperCredentialBuffers();

	SectionTitle(Tr("Screenscraper"));
	CheckboxConfig(Tr("Enable Screenscraper"), scraper.enabled);
	(void)UwpImGuiFrontend::NativeInputText(Tr("Username"), s_screenScraperUserBuffer.data(),
		s_screenScraperUserBuffer.size(), 0, UwpImGuiFrontend::TextInputScope::UserName);
	(void)UwpImGuiFrontend::NativeInputText(Tr("Password"), s_screenScraperPasswordBuffer.data(),
		s_screenScraperPasswordBuffer.size(), ImGuiInputTextFlags_Password,
		UwpImGuiFrontend::TextInputScope::Password);
	if (!HasBuiltInScreenScraperDeveloperCredentials())
		ImGui::TextColored(ImVec4(0.90f, 0.28f, 0.18f, 1.0f), "%s", Tr("Developer API credentials are missing from this build."));
	if (ShellButton(Tr("Save account credentials"), ImVec2(240.0f, 42.0f)))
	{
		if (ScreenScraperCredentialStore().Save({ s_screenScraperUserBuffer.data(),
			s_screenScraperPasswordBuffer.data() }))
			PushStatus(Tr("Screenscraper account saved"));
		else
			PushStatus(Tr("Failed to save Screenscraper account"));
	}
	ImGui::SameLine();
	if (ShellButton(Tr("Clear account credentials"), ImVec2(250.0f, 42.0f)))
	{
		ScreenScraperCredentialStore().Clear();
		CopyToBuffer(s_screenScraperUserBuffer, std::string{});
		CopyToBuffer(s_screenScraperPasswordBuffer, std::string{});
		PushStatus(Tr("Screenscraper account cleared"));
	}

	SectionTitle(Tr("Downloads"));
	CheckboxConfig(Tr("Download metadata"), scraper.download_metadata);
	CheckboxConfig(Tr("Download box art"), scraper.download_box_art);
	CheckboxConfig(Tr("Download fanart backgrounds"), scraper.download_fanart);
	CheckboxConfig(Tr("Download videos"), scraper.download_videos);
	CheckboxConfig(Tr("Download screenshots"), scraper.download_screenshots);
	CheckboxConfig(Tr("Download logos"), scraper.download_logos);
	CheckboxConfig(Tr("Overwrite existing media"), scraper.overwrite_existing);
	const auto scraperState = CemuUwp::ScreenScraperDownloader::GetState();
	const bool scraperRunning = scraperState == CemuUwp::ScreenScraperDownloader::State::Working;
	if (ShellButton(scraperRunning ? "Stop download" : "Download library assets", ImVec2(280.0f, 42.0f)))
	{
		if (scraperRunning)
			CemuUwp::ScreenScraperDownloader::RequestStop();
		else
			StartScreenScraperDownload();
	}
	if (!scraperRunning && CemuUwp::ScreenScraperDownloader::HasPendingScan())
	{
		ImGui::SameLine();
		if (ShellButton(Tr("Resume scan"), ImVec2(180.0f, 42.0f)))
		{
			auto request = BuildScreenScraperRequest();
			if (!CemuUwp::ScreenScraperDownloader::ResumePendingScan(std::move(request)))
				PushStatus(Tr("Nothing left to resume"));
		}
	}
	if (scraperState != CemuUwp::ScreenScraperDownloader::State::Idle)
		ImGui::TextWrapped(Tr("Status: %s"), CemuUwp::ScreenScraperDownloader::GetStatus().c_str());
	if (!scraperRunning && CemuUwp::ScreenScraperDownloader::HasPendingScan())
		ImGui::TextWrapped(Tr("An unfinished scan is saved; it resumes automatically on the next launch."));

	SectionTitle(Tr("Storage"));
	const auto mediaLocations = UwpImGuiFrontend::MakeXboxStorageLocationPresets(
		L"D:\\DevelopmentFiles\\ES-DE\\downloaded_media",
		L"E:\\ES-DE\\downloaded_media");
	UwpImGuiFrontend::StorageLocationSelectorModel mediaModel;
	mediaModel.id = "screenscraper-media-directory";
	mediaModel.label = Tr("Downloaded media location");
	mediaModel.currentPath = CemuUwp::ScreenScraperDownloader::GetConfiguredMediaRoot();
	mediaModel.presets = mediaLocations;
	mediaModel.currentPathLabel = "Media path";
	mediaModel.customPicker.futureAccessToken = L"screenscraper-media-folder";
	(void)UwpImGuiFrontend::DrawStorageLocationSelector(mediaModel,
		PickerService(), {
			.selected = [](UwpImGuiFrontend::StorageLocationSelection selection) {
				const fs::path normalized = selection.path.lexically_normal();
				GetConfig().uwp_screenscraper.media_directory = _pathToUtf8(normalized);
				std::error_code error;
				fs::create_directories(normalized / "wiiu", error);
				SaveConfig();
				PushStatus(error ?
					"ScreenScraper media path saved, but it is not currently accessible" :
					"ScreenScraper media path updated");
			},
			.failed = [](std::string_view error) {
				cemuLog_log(LogType::Force,
					"UWP ScreenScraper media folder picker failed: {}", error);
				PushStatus(Tr("Could not open the ScreenScraper media folder picker"));
			},
			.pickerOpenChanged = [](bool open) { SetPickerOpen(open); },
		});

	SectionTitle(Tr("Artwork"));
	constexpr std::array artworkSources{
		std::pair{ (sint32)UwpScreenScraperArtwork::Box2D, "2D box" },
		std::pair{ (sint32)UwpScreenScraperArtwork::Box3D, "3D box" },
		std::pair{ (sint32)UwpScreenScraperArtwork::MixRecalboxV1, "Recalbox Mix V1" },
		std::pair{ (sint32)UwpScreenScraperArtwork::MixRecalboxV2, "Recalbox Mix V2" },
		std::pair{ (sint32)UwpScreenScraperArtwork::FanArt, "Fan art" },
		std::pair{ (sint32)UwpScreenScraperArtwork::Screenshot, "Screenshot" },
		std::pair{ (sint32)UwpScreenScraperArtwork::Logo, "Game logo" },
		std::pair{ (sint32)UwpScreenScraperArtwork::BackCover, "2D box back" },
		std::pair{ (sint32)UwpScreenScraperArtwork::PhysicalMedia, "2D cartridge" },
		std::pair{ (sint32)UwpScreenScraperArtwork::TitleScreen, "Title screen" },
	};
	sint32 listArtwork = scraper.list_artwork_source.GetValue();
	if (ComboEnum(Tr("Game-list artwork"), listArtwork, artworkSources))
	{
		scraper.list_artwork_source = listArtwork;
		SaveConfig();
	}
	sint32 detailArtwork = scraper.detail_artwork_source.GetValue();
	if (ComboEnum(Tr("Large information artwork"), detailArtwork, artworkSources))
	{
		scraper.detail_artwork_source = detailArtwork;
		SaveConfig();
	}
	{
		const auto& homeTitles = GetCachedHomeTitles();
		size_t missing = 0;
		size_t scraped = 0;
		for (const auto& title : homeTitles)
		{
			if (title.titleId == 0 || !title.launchable)
				continue;
			scraped++;
			if (IsListArtworkMissing(title))
				missing++;
		}
		if (scraped != 0 && missing != 0)
			ImGui::TextWrapped(Tr("%zu of %zu titles have no game-list artwork of this type yet."), missing, scraped);
	}

	constexpr std::array regions{
		std::pair{ (sint32)UwpScreenScraperRegion::Auto, "Auto" },
		std::pair{ (sint32)UwpScreenScraperRegion::USA, "USA" },
		std::pair{ (sint32)UwpScreenScraperRegion::Europe, "Europe" },
		std::pair{ (sint32)UwpScreenScraperRegion::Japan, "Japan" },
		std::pair{ (sint32)UwpScreenScraperRegion::World, "World" },
	};
	sint32 region = scraper.preferred_region.GetValue();
	if (ComboEnum(Tr("Preferred region"), region, regions))
	{
		scraper.preferred_region = region;
		SaveConfig();
	}

	const auto& metadataLanguages = CemuUwp::Translation::GetAvailableLanguages();
	const std::string currentMetadataLanguage = scraper.metadata_language.GetValue();
	std::string metadataLanguageLabel = Tr("Follow interface language");
	for (const auto& language : metadataLanguages)
	{
		if (!language.code.empty() && language.code == currentMetadataLanguage)
		{
			metadataLanguageLabel = language.name;
			break;
		}
	}
	if (ShellButton(fmt::format("{}: {}", Tr("Metadata language"), metadataLanguageLabel).c_str()))
		ImGui::OpenPopup("Metadata language");
	if (BeginShellPopup("Metadata language"))
	{
		for (const auto& language : metadataLanguages)
		{
			const bool followInterface = language.code.empty();
			const char* label = followInterface ?
				Tr("Follow interface language") : language.name.c_str();
			if (ShellSelectable(label, language.code == currentMetadataLanguage,
				ImVec2(300.0f, 42.0f)))
			{
				scraper.metadata_language = language.code;
				CemuUwp::CompanionService::SetMetadataLanguage(MetadataLanguage());
				SaveConfig();
				ImGui::CloseCurrentPopup();
			}
			if (language.code == currentMetadataLanguage)
				ImGui::SetItemDefaultFocus();
		}
		EndShellPopup();
	}

	ImGui::TextWrapped(Tr("Shared title media: %s"), _pathToUtf8(
		CemuUwp::ScreenScraperDownloader::GetConfiguredMediaRoot() / "wiiu").c_str());
	ImGui::TextWrapped(Tr("Platform media: %s"), _pathToUtf8(ActiveSettings::GetUserDataPath("platformImages/wiiu")).c_str());
}

void DrawDsuStreamingSettings()
{
	auto& config = GetConfig().dsu_client;
	UwpImGuiFrontend::CompanionPanelModel model;
	model.streamingEnabled = config.streaming_enabled.GetValue();
	model.streamingUrl = config.streaming_url.GetValue();
	for (const auto& server : config.servers)
	{
		model.servers.push_back({ server.id, server.name, server.host,
			server.port, server.enabled });
	}
	if (model.servers.empty())
	{
		model.servers.push_back({ "default", "DSU Server",
			config.host.GetValue(), config.port.GetValue(),
			config.enabled.GetValue() });
	}
	s_companionPanel.Draw(model,
		[&](const UwpImGuiFrontend::CompanionPanelModel& updated) {
			config.servers.clear();
			for (const auto& server : updated.servers)
			{
				config.servers.push_back({ server.id, server.name, server.host,
					server.port, server.enabled, false });
			}
			config.streaming_enabled = updated.streamingEnabled;
			config.streaming_url = updated.streamingUrl;
			auto primary = std::ranges::find_if(config.servers,
				[](const UwpDsuServerEntry& server) { return server.enabled; });
			if (primary == config.servers.end() && !config.servers.empty())
				primary = config.servers.begin();
			if (primary != config.servers.end())
			{
				primary->companion_transport = true;
				config.host = primary->host;
				config.port = primary->port;
				config.enabled = primary->enabled;
			}
			SaveConfig();
			InputManager::instance().refresh_uwp_dsu_provider();
		});
}

void DrawCommunitySettings()
{
	SectionTitle(Tr("Discord"));
	constexpr std::array discordServers{
		std::pair{ 0, "Xbox Emulation Hub" },
		std::pair{ 1, "Revives Community Server" },
	};
	static int selectedDiscordServer = 0;
	if (ComboEnum(Tr("Discord"), selectedDiscordServer, discordServers))
	{
		if (selectedDiscordServer == 0)
			LaunchExternalUriAsync("https://discord.gg/WCmxvvxHqu", discordServers[0].second);
		else
			LaunchExternalUriAsync("https://discord.com/invite/f82Tr22vRZ", discordServers[1].second);
	}

	SectionTitle(Tr("App Store"));
	if (ShellButton(Tr("APP Store"), ImVec2(220.0f, 42.0f)))
		LaunchExternalUriAsync("https://emulationrevival.github.io/", "APP Store");
}

void DrawSettings()
{
	static UwpImGuiFrontend::SplitViewState view;
	static std::vector<UwpImGuiFrontend::SplitViewTab> tabs;
	static uint32_t tabsGeneration = ~0u;
	if (const uint32_t generation = CemuUwp::Translation::GetGeneration(); generation != tabsGeneration)
	{
		tabsGeneration = generation;
		tabs.clear();
		tabs.reserve(kSettingsTabs.size());
		for (const auto& [_, label] : kSettingsTabs)
			tabs.push_back({ label, Tr(label), true });
	}
	view.Select(IndexOfTab(s_settingsPage, kSettingsTabs), tabs.size());
	if (ConsumeOverlayPaneFocus(OverlayPaneFocus::Left))
		view.RequestFocus(UwpImGuiFrontend::SplitPane::Navigation);
	else if (ConsumeOverlayPaneFocus(OverlayPaneFocus::Right))
		view.RequestFocus(UwpImGuiFrontend::SplitPane::Content);
	UwpImGuiFrontend::DrawSplitView("settings", view, tabs,
		[](std::size_t selected) {
			s_settingsPage = kSettingsTabs[selected].first;
			switch (s_settingsPage)
			{
			case SettingsPage::General: DrawGeneralSettings(); break;
			case SettingsPage::Graphics: DrawGraphicsSettings(); break;
			case SettingsPage::Audio: DrawAudioSettings(); break;
			case SettingsPage::Input: DrawInputSettings(); break;
			case SettingsPage::DsuStreaming: DrawDsuStreamingSettings(); break;
			case SettingsPage::Overlay: DrawOverlaySettings(); break;
			case SettingsPage::Account: DrawAccountSettings(); break;
			case SettingsPage::Debug: DrawDebugSettings(); break;
			case SettingsPage::Storage: DrawStorageSettings(); break;
			case SettingsPage::Screenscraper: DrawScreenScraperSettings(); break;
			case SettingsPage::Community: DrawCommunitySettings(); break;
			}
		});
	s_settingsPage = kSettingsTabs[view.selected].first;
	s_activeOverlayPaneFocus = view.focusedPane == UwpImGuiFrontend::SplitPane::Navigation ?
		OverlayPaneFocus::Left : OverlayPaneFocus::Right;
}

void SaveGraphicPackState()
{
	auto& data = GetConfig();
	data.graphic_pack_entries.clear();
	for (const auto& gp : GraphicPack2::GetGraphicPacks())
	{
		auto filename = _utf8ToPath(gp->GetNormalizedPathString());
		if (gp->IsEnabled())
		{
			data.graphic_pack_entries.try_emplace(filename);
			auto& entry = data.graphic_pack_entries[filename];
			for (const auto& preset : gp->GetActivePresets())
				entry.try_emplace(preset->category, preset->name);
		}
		else if (gp->IsDefaultEnabled())
		{
			data.graphic_pack_entries[filename].try_emplace("_disabled", "true");
		}
	}
	SaveConfig();
}

struct GraphicPackShaderAvailabilityCache
{
	std::weak_ptr<GraphicPack2> pack;
	GraphicPack2::ShaderAvailability availability;
};
GraphicPackShaderAvailabilityCache s_selectedPackShaderAvailability;

// scans the pack folder only when the selection changes; reloading packs replaces the pack objects
const GraphicPack2::ShaderAvailability& GetShaderAvailabilityForSelectedPack(const std::shared_ptr<GraphicPack2>& pack)
{
	if (s_selectedPackShaderAvailability.pack.lock() != pack)
	{
		s_selectedPackShaderAvailability.pack = pack;
		s_selectedPackShaderAvailability.availability = pack->GetShaderAvailability(RendererAPI::Direct3D12);
	}
	return s_selectedPackShaderAvailability.availability;
}

void DrawGraphicPacks()
{
	CemuHost::EnsureGraphicPacksLoaded();
	const auto& packs = GraphicPack2::GetGraphicPacks();
	EnsureGraphicPackListState();
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	constexpr ImGuiWindowFlags childFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NavFlattened;
	BeginShellPane("pack-list", ImVec2(std::max(420.0f, avail.x * 0.45f), avail.y),
		childFlags | ImGuiWindowFlags_NoScrollbar);
	{
		const bool focusGraphicPacksLeft = ConsumeOverlayPaneFocus(OverlayPaneFocus::Left);
		const auto rows = BuildGraphicPackListRows();
		s_graphicPackListRow = rows.empty() ? 0 : std::clamp(s_graphicPackListRow, 0, (int)rows.size() - 1);
		int rowIndex = 0;
		if (focusGraphicPacksLeft && s_graphicPackListRow == rowIndex)
			ImGui::SetKeyboardFocusHere();
		if (CemuUwp::GraphicPackDownloader::ConsumeFinished())
		{
			cemuLog_log(LogType::Force, "UWP graphic packs: download finished, reloading packs");
			GraphicPack2::ClearGraphicPacks();
			GraphicPack2::LoadAll();
			ResetGraphicPackListState();
			PushStatus(Tr("Community graphic packs installed"));
		}
		if (ShellButton(Tr("Reload packs"), ImVec2(150.0f, 42.0f)))
		{
			s_graphicPackListRow = rowIndex;
			cemuLog_log(LogType::Force, "UWP graphic packs: reload requested");
			GraphicPack2::ClearGraphicPacks();
			GraphicPack2::LoadAll();
			ResetGraphicPackListState();
			PushStatus(Tr("Graphic packs reloaded"));
		}
		else if (ImGui::IsItemFocused())
		{
			s_graphicPackListRow = rowIndex;
			s_activeOverlayPaneFocus = OverlayPaneFocus::Left;
		}
		++rowIndex;
		const auto downloadState = CemuUwp::GraphicPackDownloader::GetState();
		if (focusGraphicPacksLeft && s_graphicPackListRow == rowIndex)
			ImGui::SetKeyboardFocusHere();
		if (ShellButton(Tr("Download community packs"), ImVec2(280.0f, 42.0f)) &&
			downloadState != CemuUwp::GraphicPackDownloader::State::Working)
		{
			s_graphicPackListRow = rowIndex;
			cemuLog_log(LogType::Force, "UWP graphic packs: download requested");
			if (!CemuUwp::GraphicPackDownloader::Start())
				cemuLog_log(LogType::Force, "UWP graphic packs: download request ignored because a download is already running");
		}
		else if (ImGui::IsItemFocused())
		{
			s_graphicPackListRow = rowIndex;
			s_activeOverlayPaneFocus = OverlayPaneFocus::Left;
		}
		++rowIndex;
		if (downloadState != CemuUwp::GraphicPackDownloader::State::Idle)
			ImGui::TextWrapped("%s", CemuUwp::GraphicPackDownloader::GetStatus().c_str());
		const auto& groups = GetGraphicPackGroups();
		{
			ImGui::Text(Tr("%zu packs across %zu games (LB/RB change game)"), packs.size(), groups.size());
			for (int g = 0; g < (int)groups.size(); ++g)
			{
				const auto& group = groups[g];
				const bool expanded = IsGraphicPackGroupExpanded(group);
				const bool headerSelected = rowIndex == s_graphicPackListRow;
				const std::string headerId = fmt::format("graphic-pack-group-{}", g);
				const std::string headerLabel = fmt::format("{} {}  ({})", expanded ? "v" : ">", group.name, group.packCount);
				if (focusGraphicPacksLeft && headerSelected)
					ImGui::SetKeyboardFocusHere();
				const bool activated = ShellSelectableWithId(headerId.c_str(), headerLabel.c_str(), headerSelected, ImVec2(0.0f, 42.0f));
				const bool focused = ImGui::IsItemFocused();
				if (activated)
				{
					if (expanded)
						s_expandedGraphicPackGroups.erase(group.name);
					else
						s_expandedGraphicPackGroups.insert(group.name);
				}
				if (activated || focused)
				{
					s_graphicPackListRow = rowIndex;
					s_selectedGraphicPack = group.firstPack;
					s_activeOverlayPaneFocus = OverlayPaneFocus::Left;
				}
				++rowIndex;
				if (!expanded)
					continue;
				for (int i = group.firstPack; i < group.firstPack + group.packCount; ++i)
				{
					const auto& pack = packs[i];
					const std::string& virtualPath = pack->GetVirtualPath();
					std::string label = virtualPath.size() > group.name.size() + 1 ? virtualPath.substr(group.name.size() + 1) : pack->GetName();
					label.insert(0, "      ");
					const bool selected = rowIndex == s_graphicPackListRow;
					if (focusGraphicPacksLeft && selected)
						ImGui::SetKeyboardFocusHere();
					const std::string packId = fmt::format("graphic-pack-{}", i);
					const bool activated = ShellSelectableWithId(packId.c_str(), label.c_str(), selected, ImVec2(0.0f, 42.0f));
					if (activated || ImGui::IsItemFocused())
					{
						s_graphicPackListRow = rowIndex;
						s_selectedGraphicPack = i;
						s_activeOverlayPaneFocus = OverlayPaneFocus::Left;
						if (activated)
							RequestOverlayPaneFocus(OverlayPaneFocus::Right);
					}
					++rowIndex;
				}
			}
		}
	}
	EndShellPane();

	ImGui::SameLine();
	BeginShellPane("pack-detail", ImVec2(0.0f, avail.y), childFlags);
	if (ConsumeOverlayPaneFocus(OverlayPaneFocus::Right))
		ImGui::SetKeyboardFocusHere();
	if (packs.empty())
	{
		ImGui::TextWrapped(Tr("No graphic packs are installed in the UWP user data graphicPacks folder."));
		EndShellPane();
		return;
	}

	s_selectedGraphicPack = std::clamp(s_selectedGraphicPack, 0, (int)packs.size() - 1);
	const auto& pack = packs[s_selectedGraphicPack];
	SectionTitle(pack->GetName().c_str());
	ImGui::TextWrapped("%s", pack->GetDescription().empty() ? Tr("This graphic pack has no description.") : pack->GetDescription().c_str());
	bool enabled = pack->IsEnabled();
	if (ShellCheckbox(Tr("Enabled"), &enabled))
	{
		pack->SetEnabled(enabled);
		SaveGraphicPackState();
		if (CafeSystem::IsTitleRunning() && pack->ContainsTitleId(CafeSystem::GetForegroundTitleId()))
			PushStatus(Tr("Restart the title if this pack needs a full reload"));
	}
	// BOTW runs at 30 FPS on Series S; FPS++ "Normal Settings" keeps game speed right through dips,
	// and the 30 limit keeps frame pacing even instead of chasing the 60 default
	if (pack->GetName() == "FPS++" && pack->GetVirtualPath().find("Breath of the Wild") != std::string::npos &&
		ShellButton(Tr("Series S recommended: 30 FPS, steady game speed")))
	{
		pack->SetEnabled(true);
		const bool modeSet = pack->SetActivePreset("Mode", "Normal Settings");
		const bool limitSet = pack->SetActivePreset("FPS Limit", "30FPS Limit");
		// present every second TV refresh: with VSync off the emulator's 30 Hz drifts against the 60 Hz output
		// and a frame now and then stays 3 refreshes, which shows as judder in camera pans and cutscenes
		GetConfig().vsync = 2;
		SaveConfig();
		SaveGraphicPackState();
		PushStatus(modeSet && limitSet ? Tr("FPS++ set for Series S; restart the game to apply") : Tr("This FPS++ version has different presets; set them manually"));
	}
	ImGui::Text(Tr("Version: %d"), pack->GetVersion());
	ImGui::Text(Tr("Activated: %s"), pack->IsActivated() ? Tr("yes") : Tr("no"));
	const auto& shaderAvailability = GetShaderAvailabilityForSelectedPack(pack);
	const size_t skippedShaderCount = shaderAvailability.skippedShaders.size();
	const size_t totalShaderCount = shaderAvailability.usableCount + skippedShaderCount;
	if (totalShaderCount == 0)
		ImGui::TextUnformatted(Tr("Shaders: none"));
	else
		ImGui::TextUnformatted(TrFormat("Shaders usable on Direct3D 12: {} of {}", shaderAvailability.usableCount, totalShaderCount).c_str());
	if (skippedShaderCount != 0)
	{
		ImGui::TextWrapped("%s", TrFormat("{} shader(s) in this pack have no HLSL version and are skipped on Direct3D 12. The effects they provide are missing.", skippedShaderCount).c_str());
		constexpr size_t kListedSkippedShaderLimit = 8;
		for (size_t i = 0; i < std::min(skippedShaderCount, kListedSkippedShaderLimit); ++i)
			ImGui::TextDisabled("  %s", shaderAvailability.skippedShaders[i].c_str());
		if (skippedShaderCount > kListedSkippedShaderLimit)
			ImGui::TextDisabled("  %s", TrFormat("and {} more", skippedShaderCount - kListedSkippedShaderLimit).c_str());
		const bool resolutionRulesSkipped = pack->IsActivated() ? pack->SkipsResolutionRules() :
			GetConfig().skip_resolution_rules_of_packs_with_skipped_shaders.GetValue();
		if (resolutionRulesSkipped)
			ImGui::TextWrapped("%s", Tr("Resolution rules from this pack are not applied."));
	}
	ImGui::Text(Tr("Restart on enable/preset: %s / %s"),
		pack->RequiresRestart(true, false) ? "yes" : "no",
		pack->RequiresRestart(false, true) ? "yes" : "no");

	std::vector<std::string> order;
	auto presets = pack->GetCategorizedPresets(order);
	for (const auto& category : order)
	{
		const auto& entries = presets[category];
		if (entries.empty())
			continue;
		if (std::none_of(entries.begin(), entries.end(), [&](const auto& preset) { return pack->IsPresetVisible(preset); }))
			continue;
		const std::string label = category.empty() ? "Active preset" : category;
		std::string active = pack->GetActivePreset(category);
		if (active.empty())
			active = entries.front()->name;
		if (ShellButton(fmt::format("{}: {}", label, active).c_str()))
			ImGui::OpenPopup(label.c_str());
		if (BeginShellPopup(label.c_str()))
		{
			for (const auto& preset : entries)
			{
				if (!pack->IsPresetVisible(preset))
					continue;
				const bool selected = preset->name == active;
				if (ShellSelectable(preset->name.c_str(), selected, ImVec2(320.0f, 42.0f)))
				{
					if (pack->SetActivePreset(category, preset->name))
					{
						SaveGraphicPackState();
						// VSync every second/third refresh (set by the Series S button) holds the game at 30 FPS or less,
						// so an FPS++ limit above 30 never showed
						if (pack->GetName() == "FPS++" && category == "FPS Limit" && !preset->name.starts_with("30") &&
							GetConfig().vsync.GetValue() >= 2)
						{
							GetConfig().vsync = 0;
							SaveConfig();
							PushStatus(Tr("VSync turned off: every second refresh limits the game to 30 FPS"));
						}
						if (CafeSystem::IsTitleRunning() && pack->IsEnabled() && pack->RequiresRestart(false, true))
							PushStatus(Tr("Restart the game to apply this preset"));
					}
					ImGui::CloseCurrentPopup();
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			EndShellPopup();
		}
	}
	if (ImGui::IsWindowFocused())
		s_activeOverlayPaneFocus = OverlayPaneFocus::Right;
	EndShellPane();
}

ShellTexture* GetTitleArtworkTexture(const TitleTile& title);

bool DrawTitleManagerTable(const std::vector<TitleTile>& titles,
	bool requestFocus)
{
	const std::array columns{
		UwpImGuiFrontend::DataTableColumn{ "artwork", "", UwpImGuiFrontend::DataTableColumnKind::Artwork, 68.0f, false, false },
		UwpImGuiFrontend::DataTableColumn{ "name", Tr("Name"), UwpImGuiFrontend::DataTableColumnKind::PrimaryText, 3.0f, true, true },
		UwpImGuiFrontend::DataTableColumn{ "type", Tr("Type"), UwpImGuiFrontend::DataTableColumnKind::Text, 112.0f, false, true },
		UwpImGuiFrontend::DataTableColumn{ "title-id", Tr("Title ID"), UwpImGuiFrontend::DataTableColumnKind::Text, 174.0f, false, true },
		UwpImGuiFrontend::DataTableColumn{ "version", Tr("Version"), UwpImGuiFrontend::DataTableColumnKind::Text, 82.0f, false, true },
		UwpImGuiFrontend::DataTableColumn{ "region", Tr("Region"), UwpImGuiFrontend::DataTableColumnKind::Text, 92.0f, false, true },
		UwpImGuiFrontend::DataTableColumn{ "action", Tr("Action"), UwpImGuiFrontend::DataTableColumnKind::Action, 108.0f, false, false },
	};
	std::vector<UwpImGuiFrontend::DataTableRow> rows;
	rows.reserve(titles.size());
	for (const TitleTile& title : titles)
	{
		ShellTexture* artwork = GetTitleArtworkTexture(title);
		if (!artwork)
			artwork = StaticTexture("gamecard", "icons/gamecard.png");
		UwpImGuiFrontend::TextureHandle artworkHandle;
		if (artwork && artwork->Valid())
		{
			artworkHandle.id = reinterpret_cast<std::uintptr_t>(artwork->id);
			artworkHandle.width = static_cast<std::uint32_t>(artwork->width);
			artworkHandle.height = static_cast<std::uint32_t>(artwork->height);
		}
		rows.push_back({
			.id = title.titleId,
			.cells = {
				{ .artwork = artworkHandle },
				{ .text = UwpImGuiFrontend::NormalizeDisplayText(title.name) },
				{ .text = title.type },
				{ .text = fmt::format("{:016X}", title.titleId) },
				{ .text = fmt::format("{}", title.version) },
				{ .text = title.region },
				{ .text = "Launch", .enabled = title.launchable },
			},
			.enabled = title.launchable,
		});
	}
	static UwpImGuiFrontend::DataTableState tableState;
	if (requestFocus && !titles.empty())
		tableState.requestFocus = true;
	const std::size_t selected = titles.empty() ? 0 :
		static_cast<std::size_t>(std::clamp(s_titleManagerSelection, 0,
			static_cast<int>(titles.size()) - 1));
	if (tableState.selected != selected)
	{
		tableState.selected = selected;
		tableState.requestFocus = true;
	}
	UwpImGuiFrontend::DataTableOptions tableOptions;
	tableOptions.font = ShellFont(17.0f,
		MakeShellLayout(ImGui::GetMainViewport()->WorkSize));
	tableOptions.rowHeight = 72.0f;
	tableOptions.showHeaders = false;
	tableOptions.controllerNavigation = true;
	const auto result = UwpImGuiFrontend::DrawDataTable("titles", tableState,
		columns, rows, tableOptions);
	s_titleManagerSelection = static_cast<int>(tableState.selected);
	if (result.activatedRow && *result.activatedRow < titles.size() &&
		titles[*result.activatedRow].launchable)
	{
		CemuHost::LaunchPath(titles[*result.activatedRow].path);
	}
	return result.navigateBeforeTable;
}

void DrawTitleManager(const std::vector<TitleTile>& titles)
{
	static bool focusToolbar = false;
	bool toolbarFocused = false;
	SectionTitle(Tr("Titles"));
	if (focusToolbar)
		ImGui::SetKeyboardFocusHere();
	const bool installTitle =
		ShellButton(Tr("Install title, update or DLC"), ImVec2(270.0f, 42.0f));
	toolbarFocused |= ImGui::IsItemFocused();
	if (installTitle && !s_pickerOpen)
	{
		s_titleInstallPickerRequested = true;
		s_titleInstallInputReleaseBarrier = true;
	}
	ImGui::SameLine();
	const bool refreshTitles = ShellButton(Tr("Refresh titles"), ImVec2(170.0f, 42.0f));
	toolbarFocused |= ImGui::IsItemFocused();
	if (refreshTitles)
		RefreshTitleList();
	ImGui::SameLine();
	const bool openTitle = ShellButton(Tr("Open title file"), ImVec2(170.0f, 42.0f));
	toolbarFocused |= ImGui::IsItemFocused();
	if (openTitle && !s_pickerOpen)
		PickTitleAsync();
	ImGui::SameLine();
	const bool gamesDirectory =
		ShellButton(Tr("Games directory"), ImVec2(180.0f, 42.0f));
	toolbarFocused |= ImGui::IsItemFocused();
	if (gamesDirectory)
		s_titleManagerGamesDirectoryOpen = !s_titleManagerGamesDirectoryOpen;
	focusToolbar = false;
	if (s_titleManagerGamesDirectoryOpen)
	{
		const auto& config = GetConfig();
		const auto presets = UwpImGuiFrontend::MakeXboxStorageLocationPresets(
			L"D:\\DevelopmentFiles\\Games\\wiiu", L"E:\\Games\\wiiu");
		UwpImGuiFrontend::StorageLocationSelectorModel selector;
		selector.id = "title-manager-games-directory";
		selector.label = Tr("Games directory");
		selector.currentPath = config.game_paths.empty() ? fs::path{} :
			_utf8ToPath(config.game_paths.front());
		selector.presets = presets;
		selector.promptLabel = "Select storage location";
		selector.currentPathLabel = "Folder";
		selector.customPicker.futureAccessToken = L"title-manager-games-directory";
		(void)UwpImGuiFrontend::DrawStorageLocationSelector(selector,
			PickerService(), {
				.selected = [](UwpImGuiFrontend::StorageLocationSelection selection) {
					SetPrimaryGameFolder(selection.path);
					s_titleManagerGamesDirectoryOpen = false;
				},
				.failed = [](std::string_view error) {
					cemuLog_log(LogType::Force,
						"UWP title manager folder picker failed: {}", error);
					PushStatus(Tr("Could not open the game folder picker"));
				},
				.pickerOpenChanged = [](bool open) { SetPickerOpen(open); },
			});
	}
	const bool requestTableFocus = toolbarFocused &&
		!s_titleManagerGamesDirectoryOpen && !s_pickerOpen &&
		(ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) ||
		 ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown));
	if (DrawTitleManagerTable(titles, requestTableFocus))
		focusToolbar = true;
}
std::string FormatInstallSize(uint64_t bytes)
{
	constexpr double kib = 1024.0;
	constexpr double mib = kib * 1024.0;
	constexpr double gib = mib * 1024.0;
	if (bytes >= (uint64_t)gib)
		return fmt::format("{:.2f} GiB", (double)bytes / gib);
	if (bytes >= (uint64_t)mib)
		return fmt::format("{:.1f} MiB", (double)bytes / mib);
	if (bytes >= (uint64_t)kib)
		return fmt::format("{:.1f} KiB", (double)bytes / kib);
	return fmt::format("{} bytes", bytes);
}

void DrawTitleInstallConfirmation(const CemuUwp::TitleInstaller::InstallPlan& plan)
{
	using ExistingState = CemuUwp::TitleInstaller::ExistingState;
	const auto& theme = CurrentShellTheme();
	ImGui::TextWrapped("%s", plan.titleName.c_str());
	ImGui::Text(Tr("%s, version %u"), CemuUwp::TitleInstaller::ContentTypeName(plan.type).data(), plan.version);
	ImGui::Text(Tr("Title ID: %016llX"), (unsigned long long)plan.titleId);
	ImGui::Text(Tr("Install size: %s (%llu files)"), FormatInstallSize(plan.requiredBytes).c_str(), (unsigned long long)plan.fileCount);
	ImGui::Text(Tr("Free space: %s"), FormatInstallSize(plan.availableBytes).c_str());
	ImGui::Spacing();
	ImGui::TextWrapped(Tr("Destination: %s"), _pathToUtf8(plan.targetPath).c_str());

	switch (plan.existingState)
	{
	case ExistingState::Upgrade:
		ImGui::TextWrapped(Tr("Installed version %u will be replaced."), plan.existingVersion);
		break;
	case ExistingState::SameVersion:
		ImGui::TextWrapped(Tr("Version %u is already installed. Continuing will reinstall it."), plan.existingVersion);
		break;
	case ExistingState::Downgrade:
		ImGui::TextColored(theme.cursorNormal,
			"A newer version (%u) is installed. Continuing will downgrade it.", plan.existingVersion);
		break;
	case ExistingState::DifferentType:
		ImGui::TextColored(theme.cursorNormal,
			"The destination contains a different title type. Continuing will replace it.");
		break;
	case ExistingState::Invalid:
		ImGui::TextColored(theme.cursorNormal,
			"The destination contains incomplete or invalid title data. Continuing will replace it.");
		break;
	case ExistingState::None:
	default:
		break;
	}

	ImGui::Spacing();
	const bool gameRunning = CafeSystem::IsTitleRunning();
	ImGui::BeginDisabled(gameRunning || s_titleInstallInputReleaseBarrier);
	if (ShellButton(Tr("Install"), ImVec2(180.0f, 48.0f)))
	{
		if (CemuUwp::TitleInstaller::Start(plan))
		{
			s_titleInstallSnapshot = CemuUwp::TitleInstaller::GetSnapshot();
			s_titleInstallCompletionHandled = false;
			s_titleInstallCancelConfirmation = false;
			s_titleInstallRequestDefaultFocus = true;
			s_titleInstallDialogState = TitleInstallDialogState::Installing;
		}
		else
		{
			s_titleInstallSnapshot = CemuUwp::TitleInstaller::GetSnapshot();
			s_titleInstallMessage = s_titleInstallSnapshot.message.empty() ? "Could not start the title installer" : s_titleInstallSnapshot.message;
			s_titleInstallRequestDefaultFocus = true;
			s_titleInstallDialogState = TitleInstallDialogState::Result;
		}
	}
	if (s_titleInstallRequestDefaultFocus && !gameRunning && !s_titleInstallInputReleaseBarrier &&
		s_titleInstallDialogState == TitleInstallDialogState::Confirmation)
	{
		ImGui::SetItemDefaultFocus();
		s_titleInstallRequestDefaultFocus = false;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ShellButton(Tr("Cancel"), ImVec2(180.0f, 48.0f)))
	{
		ImGui::CloseCurrentPopup();
		CloseTitleInstallDialog();
	}
	if (s_titleInstallRequestDefaultFocus && !s_titleInstallInputReleaseBarrier &&
		s_titleInstallDialogState == TitleInstallDialogState::Confirmation)
	{
		ImGui::SetItemDefaultFocus();
		s_titleInstallRequestDefaultFocus = false;
	}
	if (gameRunning)
		ImGui::TextWrapped(Tr("Stop the running game before installing content."));
}

void DrawTitleInstallProgress()
{
	using InstallState = CemuUwp::TitleInstaller::State;
	const auto& snapshot = s_titleInstallSnapshot;
	const ShellLayoutContext ctx = MakeShellLayout(ImGui::GetMainViewport()->WorkSize);
	const float progress = snapshot.requiredBytes == 0 ? 0.0f :
		(float)std::clamp((double)snapshot.copiedBytes / (double)snapshot.requiredBytes, 0.0, 1.0);
	const std::string overlay = fmt::format("{} / {}", FormatInstallSize(snapshot.copiedBytes), FormatInstallSize(snapshot.requiredBytes));
	ImGui::TextUnformatted(snapshot.state == InstallState::Cancelling ? Tr("Cancelling installation...") : Tr("Installing..."));
	UwpImGuiFrontend::ShellProgressBar(progress, overlay,
		ImVec2(-1.0f, 34.0f * ctx.uiScale));
	ImGui::Text(Tr("Files: %llu / %llu"), (unsigned long long)snapshot.copiedFiles, (unsigned long long)snapshot.fileCount);
	if (!snapshot.currentFile.empty())
		ImGui::TextWrapped("%s", snapshot.currentFile.c_str());

	ImGui::Spacing();
	if (snapshot.state == InstallState::Cancelling)
		return;
	if (s_titleInstallCancelConfirmation)
	{
		ImGui::TextWrapped(Tr("Cancel this installation? The previous installed version will remain unchanged."));
		if (ShellButton(Tr("Keep installing"), ImVec2(210.0f, 48.0f)))
		{
			s_titleInstallCancelConfirmation = false;
			s_titleInstallRequestDefaultFocus = true;
		}
		if (s_titleInstallRequestDefaultFocus)
		{
			ImGui::SetItemDefaultFocus();
			s_titleInstallRequestDefaultFocus = false;
		}
		ImGui::SameLine();
		if (ShellButton(Tr("Cancel installation"), ImVec2(220.0f, 48.0f)))
		{
			CemuUwp::TitleInstaller::RequestCancel();
			s_titleInstallCancelConfirmation = false;
			s_titleInstallSnapshot = CemuUwp::TitleInstaller::GetSnapshot();
		}
	}
	else if (ShellButton(Tr("Cancel"), ImVec2(180.0f, 48.0f)))
	{
		s_titleInstallCancelConfirmation = true;
		s_titleInstallRequestDefaultFocus = true;
	}
	if (!s_titleInstallCancelConfirmation && s_titleInstallRequestDefaultFocus)
	{
		ImGui::SetItemDefaultFocus();
		s_titleInstallRequestDefaultFocus = false;
	}
}

void DrawTitleInstallDialog()
{
	if (!TitleInstallDialogOwnsInput())
		return;
	ImGui::OpenPopup("Install title, update or DLC");
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ShellLayoutContext ctx = MakeShellLayout(viewport->WorkSize);
	const float width = std::min(760.0f * ctx.uiScale,
		std::max(420.0f * ctx.uiScale,
			viewport->WorkSize.x - 80.0f * ctx.uiScale));
	if (!UwpImGuiFrontend::BeginThemedModal("Install title, update or DLC",
		ctx, CurrentShellTheme(), { width, 0.0f }))
	{
		return;
	}

	if (ImFont* headingFont = ShellFont(21.0f, ctx))
	{
		ImGui::PushFont(headingFont);
		ImGui::TextUnformatted(Tr("Install title, update or DLC"));
		ImGui::PopFont();
	}
	else
	{
		ImGui::TextUnformatted(Tr("Install title, update or DLC"));
	}
	ImGui::Separator();
	ImGui::Dummy(ToPx(ctx, 0.0f, 2.0f));

	ImFont* font = ShellFont(17.0f, ctx);
	if (font)
		ImGui::PushFont(font);
	if (s_titleInstallDismissRequested)
	{
		ImGui::CloseCurrentPopup();
		if (font)
			ImGui::PopFont();
		UwpImGuiFrontend::EndThemedModal();
		CloseTitleInstallDialog();
		return;
	}

	switch (s_titleInstallDialogState)
	{
	case TitleInstallDialogState::Planning:
		UwpImGuiFrontend::DrawProgressDialogBody("Reading title metadata...", 0.0f);
		break;
	case TitleInstallDialogState::Confirmation:
		if (s_titleInstallPlan)
			DrawTitleInstallConfirmation(*s_titleInstallPlan);
		break;
	case TitleInstallDialogState::Installing:
		DrawTitleInstallProgress();
		break;
	case TitleInstallDialogState::Result:
	{
		using InstallState = CemuUwp::TitleInstaller::State;
		if (s_titleInstallSnapshot.state == InstallState::Succeeded)
			ImGui::TextUnformatted(Tr("Installation complete"));
		else if (s_titleInstallSnapshot.state == InstallState::Cancelled)
			ImGui::TextUnformatted(Tr("Installation cancelled"));
		else
			ImGui::TextUnformatted(Tr("Installation failed"));
		if (!s_titleInstallMessage.empty())
			ImGui::TextWrapped("%s", s_titleInstallMessage.c_str());
		if (ShellButton(Tr("Close"), ImVec2(180.0f, 48.0f)))
		{
			ImGui::CloseCurrentPopup();
			CloseTitleInstallDialog();
		}
		if (s_titleInstallRequestDefaultFocus)
		{
			ImGui::SetItemDefaultFocus();
			s_titleInstallRequestDefaultFocus = false;
		}
		break;
	}
	case TitleInstallDialogState::Closed:
	default:
		break;
	}
	if (font)
		ImGui::PopFont();
	UwpImGuiFrontend::EndThemedModal();
}

std::string TailFile(const fs::path& path, size_t maxLines)
{
	std::ifstream file(path);
	if (!file)
		return {};
	std::deque<std::string> lines;
	std::string line;
	while (std::getline(file, line))
	{
		lines.emplace_back(std::move(line));
		while (lines.size() > maxLines)
			lines.pop_front();
	}
	std::string result;
	for (const auto& it : lines)
	{
		result.append(it);
		result.push_back('\n');
	}
	return result;
}

void DrawLogs()
{
	static std::string logText;
	static std::string bootstrapText;
	SectionTitle(Tr("Logs"));
	if (ShellButton(Tr("Refresh logs"), ImVec2(160.0f, 42.0f)))
	{
		logText = TailFile(cemuLog_GetActiveLogFilePath(), 300);
		bootstrapText = TailFile(CemuUwp::StorageService::LocalRoot() / "bootstrap.log", 120);
	}
	ImGui::SameLine();
	if (ShellButton(Tr("Flush log"), ImVec2(140.0f, 42.0f)))
		cemuLog_waitForFlush();
	if (logText.empty())
	{
		logText = TailFile(cemuLog_GetActiveLogFilePath(), 300);
		bootstrapText = TailFile(CemuUwp::StorageService::LocalRoot() / "bootstrap.log", 120);
	}
	if (ImGui::BeginTabBar("logs"))
	{
		if (ImGui::BeginTabItem("log.txt"))
		{
			BeginShellPane("logtext", ImVec2(0, 0), ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NavFlattened, 14.0f, 0.64f, ImVec2(12.0f, 12.0f));
			ImGui::TextUnformatted(logText.empty() ? Tr("No log text available.") : logText.c_str());
			EndShellPane();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("bootstrap.log"))
		{
			BeginShellPane("bootstraptext", ImVec2(0, 0), ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NavFlattened, 14.0f, 0.64f, ImVec2(12.0f, 12.0f));
			ImGui::TextUnformatted(bootstrapText.empty() ? Tr("No bootstrap log available.") : bootstrapText.c_str());
			EndShellPane();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
}

void DrawAbout()
{
	SectionTitle(Tr("Cemu UWP"));
	ImGui::TextWrapped(Tr("Native UWP app-container frontend for Cemu."));
	ImGui::Text(Tr("Platform: Windows UWP"));
	ImGui::Text(Tr("Renderer: Direct3D 12 CoreWindow"));
	ImGui::Text(Tr("Storage: %s"), _pathToUtf8(CemuUwp::StorageService::LocalRoot()).c_str());
	SectionTitle(Tr("Cemu UWP shell"));
	ImGui::TextWrapped(Tr("The layout mirrors the Cemu UWP home menu: game tiles, side rails, soft system panels, and controller-first navigation."));
	ImGui::TextWrapped(Tr("A: activate  B: back/home  D-pad/left stick: move focus"));
	if (ShellButton(Tr("Home"), ImVec2(140.0f, 42.0f)))
	{
		OpenPage(Page::Home);
	}
}

struct ShellGridMetrics
{
	float cellW = 150.0f;
	float cellH = 150.0f;
	float padX = 20.0f;
	float padY = 16.0f;
	float originX = 225.0f;
	float originY = 119.0f;
	float bandY = 90.0f;
	float bandH = 540.0f;
};

ShellGridMetrics ComputeShellGrid(const ShellLayoutContext& ctx)
{
	constexpr float baseCellW = 150.0f;
	constexpr float baseCellH = 150.0f;
	constexpr float basePadX = 20.0f;
	constexpr float basePadY = 16.0f;
	const float bandY = 90.0f;
	const float bandH = std::max(360.0f, ctx.layoutUnitsH - 180.0f);
	const float safeSide = std::clamp(ctx.layoutUnitsW * 0.171875f, 112.0f, 220.0f);
	const float safeTopBottom = 20.0f;
	const float baseGridW = kHomeColumns * baseCellW + (kHomeColumns - 1) * basePadX;
	const float baseGridH = kHomeRows * baseCellH + (kHomeRows - 1) * basePadY;
	const float safeW = std::max(1.0f, ctx.layoutUnitsW - safeSide * 2.0f);
	const float safeH = std::max(1.0f, bandH - safeTopBottom * 2.0f);
	const float scale = std::min({ 1.0f, safeW / baseGridW, safeH / baseGridH });
	ShellGridMetrics metrics{};
	metrics.cellW = std::max(88.0f, baseCellW * scale);
	metrics.cellH = std::max(88.0f, baseCellH * scale);
	metrics.padX = std::max(8.0f, basePadX * scale);
	metrics.padY = std::max(8.0f, basePadY * scale);
	metrics.bandY = bandY;
	metrics.bandH = bandH;
	const float gridW = kHomeColumns * metrics.cellW + (kHomeColumns - 1) * metrics.padX;
	const float gridH = kHomeRows * metrics.cellH + (kHomeRows - 1) * metrics.padY;
	metrics.originX = (ctx.layoutUnitsW - gridW) * 0.5f;
	metrics.originY = bandY + (bandH - gridH) * 0.5f;
	return metrics;
}

UnitRect HomeSlotRect(const ShellLayoutContext& ctx, int globalIndex)
{
	const ShellGridMetrics grid = ComputeShellGrid(ctx);
	const int local = std::clamp(globalIndex - s_homePage * kHomePageSize, 0, kHomePageSize - 1);
	const int col = local % kHomeColumns;
	const int row = local / kHomeColumns;
	return {
		grid.originX + col * (grid.cellW + grid.padX),
		grid.originY + row * (grid.cellH + grid.padY),
		grid.cellW,
		grid.cellH,
	};
}

void DrawShellTexture(ImDrawList* draw, const ShellLayoutContext& ctx, ShellTexture* texture, const UnitRect& rect, float radius, float alpha = 1.0f)
{
	if (!texture || !texture->Valid())
		return;
	ImVec2 min = RectMinPx(ctx, rect);
	ImVec2 max = RectMaxPx(ctx, rect);
	NormalizeRoundedRectPx(min, max);
	draw->AddImageRounded(texture->id, min, max, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, (int)std::clamp(alpha * 255.0f, 0.0f, 255.0f)), radius * ctx.uiScale, ImDrawFlags_RoundCornersAll);
}

void DrawShellTextureContained(ImDrawList* draw, const ShellLayoutContext& ctx,
	ShellTexture* texture, const UnitRect& bounds, float radius, float alpha = 1.0f)
{
	if (!draw || !texture || !texture->Valid() || bounds.w <= 0.0f || bounds.h <= 0.0f)
		return;

	const float aspect = static_cast<float>(texture->width) /
		std::max(1.0f, static_cast<float>(texture->height));
	float height = bounds.h;
	float width = height * aspect;
	if (width > bounds.w)
	{
		width = bounds.w;
		height = width / std::max(0.001f, aspect);
	}
	const UnitRect image{
		bounds.x + (bounds.w - width) * 0.5f,
		bounds.y + (bounds.h - height) * 0.5f,
		width,
		height,
	};
	DrawShellTexture(draw, ctx, texture, image, radius, alpha);
}

void DrawShellTextureCoverPx(ImDrawList* draw, ShellTexture* texture, ImVec2 min, ImVec2 max, float alpha = 1.0f)
{
	if (!draw || !texture || !texture->Valid() || max.x <= min.x || max.y <= min.y)
		return;

	const float destAspect = (max.x - min.x) / std::max(1.0f, max.y - min.y);
	const float srcAspect = (float)texture->width / std::max(1.0f, (float)texture->height);
	ImVec2 uv0{ 0.0f, 0.0f };
	ImVec2 uv1{ 1.0f, 1.0f };
	if (srcAspect > destAspect)
	{
		const float crop = 1.0f - (destAspect / srcAspect);
		uv0.x = crop * 0.5f;
		uv1.x = 1.0f - crop * 0.5f;
	}
	else if (srcAspect < destAspect)
	{
		const float crop = 1.0f - (srcAspect / destAspect);
		uv0.y = crop * 0.5f;
		uv1.y = 1.0f - crop * 0.5f;
	}
	draw->AddImage(texture->id, min, max, uv0, uv1, IM_COL32(255, 255, 255, (int)std::clamp(alpha * 255.0f, 0.0f, 255.0f)));
}

// a dimmed cover pass fills the sides so the whole image can sit centred on top
void DrawShellTextureContainPx(ImDrawList* draw, ShellTexture* texture, ImVec2 min, ImVec2 max, float alpha = 1.0f)
{
	if (!draw || !texture || !texture->Valid() || max.x <= min.x || max.y <= min.y)
		return;

	DrawShellTextureCoverPx(draw, texture, min, max, alpha);
	draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, 196));

	const float destWidth = max.x - min.x;
	const float destHeight = max.y - min.y;
	const float srcAspect = (float)texture->width / std::max(1.0f, (float)texture->height);
	float width = destHeight * srcAspect;
	float height = destHeight;
	if (width > destWidth)
	{
		width = destWidth;
		height = width / std::max(0.001f, srcAspect);
	}
	const ImVec2 imageMin{ min.x + (destWidth - width) * 0.5f, min.y + (destHeight - height) * 0.5f };
	draw->AddImage(texture->id, imageMin, { imageMin.x + width, imageMin.y + height },
		{ 0.0f, 0.0f }, { 1.0f, 1.0f },
		IM_COL32(255, 255, 255, (int)std::clamp(alpha * 255.0f, 0.0f, 255.0f)));
}

fs::path TitleMediaRoot(uint64 titleId)
{
	return ActiveSettings::GetUserDataPath("titleImages/{:016x}", titleId);
}

fs::path TitleMetadataRecordPath(uint64 titleId)
{
	return TitleMediaRoot(titleId) / "frontend_metadata.json";
}

std::optional<fs::path> FirstExistingPath(std::initializer_list<fs::path> candidates)
{
	for (const auto& path : candidates)
	{
		std::error_code ec;
		if (!path.empty() && fs::exists(path, ec) && !fs::is_directory(path, ec))
			return path;
	}
	return std::nullopt;
}

UwpScreenScraperArtwork CurrentListArtwork()
{
	return static_cast<UwpScreenScraperArtwork>(std::clamp(
		GetConfig().uwp_screenscraper.list_artwork_source.GetValue(),
		(sint32)UwpScreenScraperArtwork::Box2D,
		(sint32)UwpScreenScraperArtwork::TitleScreen));
}

UwpScreenScraperArtwork CurrentDetailArtwork()
{
	return static_cast<UwpScreenScraperArtwork>(std::clamp(
		GetConfig().uwp_screenscraper.detail_artwork_source.GetValue(),
		(sint32)UwpScreenScraperArtwork::Box2D,
		(sint32)UwpScreenScraperArtwork::TitleScreen));
}

const TitleTile& ArtworkTitle(const TitleTile& title)
{
	uint64 baseTitleId = title.titleId;
	CafeTitleList::FindBaseTitleId(title.titleId, baseTitleId);
	if (baseTitleId == title.titleId)
		return title;

	const auto& titles = GetCachedTitles();
	const auto found = std::find_if(titles.begin(), titles.end(),
		[baseTitleId](const TitleTile& candidate) {
			return candidate.titleId == baseTitleId && candidate.launchable;
		});
	return found != titles.end() ? *found : title;
}

std::optional<fs::path> ConfiguredArtworkPath(const TitleTile& title,
	UwpScreenScraperArtwork source)
{
	if (title.titleId == 0)
		return std::nullopt;
	const TitleTile& mediaTitle = ArtworkTitle(title);
	return CemuUwp::ScreenScraperDownloader::FindTitleArtwork(
		mediaTitle.path,
		static_cast<UwpImGuiFrontend::ScreenScraperArtwork>(source));
}

bool IsListArtworkMissing(const TitleTile& title)
{
	return title.titleId != 0 &&
		!ConfiguredArtworkPath(title, CurrentListArtwork()).has_value();
}

ShellTexture* GetTitleScrapedBoxArtTexture(const TitleTile& title)
{
	if (auto path = ConfiguredArtworkPath(title, CurrentListArtwork()))
		return FileTexture("boxart", *path);
	return nullptr;
}

ShellTexture* GetTitleArtworkTexture(const TitleTile& title)
{
	return GetTitleScrapedBoxArtTexture(title);
}

std::optional<fs::path> CreateTitleIconClipboardPng(const TitleTile& title)
{
	auto tgaData = ExtractTitleIconTga(title);
	if (!tgaData)
		return std::nullopt;
	DecodedImage decoded;
	if (!DecodeTgaBytes(*tgaData, decoded) || decoded.width <= 0 || decoded.height <= 0 || decoded.rgba.empty())
		return std::nullopt;

	const fs::path path = CemuUwp::StorageService::CacheRoot() /
		"clipboard" / fmt::format("{:016x}.png", title.titleId);
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	if (ec)
		return std::nullopt;

	png_image image{};
	image.version = PNG_IMAGE_VERSION;
	image.width = (png_uint_32)decoded.width;
	image.height = (png_uint_32)decoded.height;
	image.format = PNG_FORMAT_RGBA;
	const std::string pathUtf8 = _pathToUtf8(path);
	const bool written = png_image_write_to_file(&image, pathUtf8.c_str(), 0, decoded.rgba.data(), 0, nullptr) != 0;
	png_image_free(&image);
	return written ? std::optional<fs::path>(path) : std::nullopt;
}

winrt::fire_and_forget CopyTitleImageToClipboardAsync(TitleTile title)
{
	try
	{
		const auto dispatcher = winrt::Windows::ApplicationModel::Core::CoreApplication::MainView().CoreWindow().Dispatcher();
		const std::optional<fs::path> fallbackBoxArtPath = ConfiguredArtworkPath(
			title, CurrentListArtwork());
		co_await winrt::resume_background();
		std::optional<fs::path> imagePath = CreateTitleIconClipboardPng(title);
		if (!imagePath)
			imagePath = fallbackBoxArtPath;
		if (!imagePath)
		{
			co_await winrt::resume_foreground(dispatcher);
			PushStatus(Tr("No title image is available to copy"));
			co_return;
		}

		const auto file = co_await winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(imagePath->wstring());
		co_await winrt::resume_foreground(dispatcher);
		winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
		package.RequestedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
		package.SetBitmap(winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromFile(file));
		winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
		winrt::Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
		PushStatus(Tr("Title image copied"));
	}
	catch (const winrt::hresult_error& e)
	{
		cemuLog_log(LogType::Force, "UWP title image clipboard copy failed: 0x{:08x} {}", (uint32)e.code(), winrt::to_string(e.message()));
		PushStatus(Tr("Failed to copy title image"));
	}
}

// the video theme shows a still of the same game until its video is ready to play
std::vector<UwpScreenScraperArtwork> ThemeBackgroundArtwork()
{
	using Artwork = UwpScreenScraperArtwork;
	switch (CurrentShellTheme().mediaMode)
	{
	case ShellThemeMediaMode::DynamicBackground:
		return { Artwork::FanArt };
	case ShellThemeMediaMode::DynamicVideo:
		return { Artwork::FanArt, Artwork::TitleScreen, Artwork::Screenshot };
	case ShellThemeMediaMode::DynamicScreenshot:
		return { Artwork::Screenshot, Artwork::TitleScreen };
	case ShellThemeMediaMode::DynamicMixRecalboxV1:
		return { Artwork::MixRecalboxV1 };
	case ShellThemeMediaMode::DynamicMixRecalboxV2:
		return { Artwork::MixRecalboxV2 };
	default:
		return {};
	}
}

// mix images are square composites, cropping them to the viewport cuts off the artwork
bool ThemeBackgroundFitsInside()
{
	const auto mode = CurrentShellTheme().mediaMode;
	return mode == ShellThemeMediaMode::DynamicMixRecalboxV1 ||
		mode == ShellThemeMediaMode::DynamicMixRecalboxV2;
}

std::optional<fs::path> DynamicBackgroundPath(const std::vector<TitleTile>& titles)
{
	const auto artwork = ThemeBackgroundArtwork();
	if (artwork.empty())
		return std::nullopt;

	const TitleTile* title = nullptr;
	if (s_selectedTitle >= 0 && s_selectedTitle < (int)titles.size())
		title = &titles[s_selectedTitle];
	if (title && title->titleId != 0)
	{
		for (UwpScreenScraperArtwork source : artwork)
		{
			if (auto path = ConfiguredArtworkPath(*title, source))
				return path;
		}
	}
	if (artwork.front() != UwpScreenScraperArtwork::FanArt)
		return std::nullopt;
	return FirstExistingPath({
		ActiveSettings::GetUserDataPath("platformImages/wiiu/fanart.png"),
		ActiveSettings::GetUserDataPath("platformImages/wiiu/fanart.jpg"),
		ActiveSettings::GetUserDataPath("platformImages/wiiu/fanart.webp"),
		ActiveSettings::GetUserDataPath("platformImages/wiiu/background.png"),
		ActiveSettings::GetUserDataPath("platformImages/wiiu/background.jpg"),
		ActiveSettings::GetUserDataPath("platformImages/wiiu/background.webp"),
	});
}

std::optional<fs::path> DynamicBackgroundVideoPath(const std::vector<TitleTile>& titles)
{
	if (CurrentShellTheme().mediaMode != ShellThemeMediaMode::DynamicVideo)
		return std::nullopt;
	const TitleTile* title = nullptr;
	if (s_selectedTitle >= 0 && s_selectedTitle < (int)titles.size())
		title = &titles[s_selectedTitle];

	if (title && title->titleId != 0)
	{
		const TitleTile& mediaTitle = ArtworkTitle(*title);
		if (auto path = CemuUwp::ScreenScraperDownloader::FindTitleVideo(
			mediaTitle.path))
		{
			return path;
		}
	}
	return FirstExistingPath({
		ActiveSettings::GetUserDataPath("platformImages/wiiu/video.mp4"),
	});
}

ShellTexture s_videoBackgroundTexture;
int s_videoBackgroundWidth = 0;
int s_videoBackgroundHeight = 0;
std::vector<uint8> s_videoFrameBuffer;
fs::path s_pendingVideoPath;
float s_pendingVideoSince = 0.0f;

void ReleaseVideoBackground()
{
	s_videoBackgroundTexture.Reset();
	s_videoBackgroundWidth = s_videoBackgroundHeight = 0;
	std::vector<uint8>().swap(s_videoFrameBuffer);
	s_pendingVideoPath.clear();
	CemuUwp::VideoPlayer::Close();
}

ShellTexture* ServiceVideoBackground(const std::vector<TitleTile>& titles)
{
	if (!s_frontendActive)
		return nullptr;
	const auto path = DynamicBackgroundVideoPath(titles);
	if (!path)
	{
		if (CemuUwp::VideoPlayer::IsOpen())
			ReleaseVideoBackground();
		return nullptr;
	}

	if (CemuUwp::VideoPlayer::CurrentPath() != *path)
	{
		if (s_pendingVideoPath != *path)
		{
			s_pendingVideoPath = *path;
			s_pendingVideoSince = s_frontendUptime;
			return nullptr;
		}
		if (s_frontendUptime - s_pendingVideoSince < 0.45f)
			return nullptr;

		s_videoBackgroundTexture.Reset();
		s_videoBackgroundWidth = s_videoBackgroundHeight = 0;
		if (!CemuUwp::VideoPlayer::Open(*path))
			return nullptr;
	}
	else
	{
		s_pendingVideoPath.clear();
	}

	int width = 0;
	int height = 0;
	if (CemuUwp::VideoPlayer::AcquireFrame(s_videoFrameBuffer, width, height) && width > 0 && height > 0)
	{
		auto* d3d12 = static_cast<D3D12Renderer*>(g_renderer.get());
		const bool sizeChanged = width != s_videoBackgroundWidth || height != s_videoBackgroundHeight;
		if (d3d12 && (sizeChanged || !s_videoBackgroundTexture.Valid()))
		{
			s_videoBackgroundTexture.Reset();
			s_videoBackgroundTexture.id = d3d12->GenerateTextureBGRA(s_videoFrameBuffer, { width, height });
			s_videoBackgroundTexture.width = width;
			s_videoBackgroundTexture.height = height;
			s_videoBackgroundWidth = width;
			s_videoBackgroundHeight = height;
		}
		else if (d3d12 && !d3d12->UpdateTextureBGRA(s_videoBackgroundTexture.id, s_videoFrameBuffer, { width, height }))
		{
			s_videoBackgroundTexture.Reset();
			s_videoBackgroundTexture.id = d3d12->GenerateTextureBGRA(s_videoFrameBuffer, { width, height });
			s_videoBackgroundTexture.width = width;
			s_videoBackgroundTexture.height = height;
		}
	}

	return s_videoBackgroundTexture.Valid() ? &s_videoBackgroundTexture : nullptr;
}

std::string EllipsizeUnits(const ShellLayoutContext& ctx, float fontUnits, std::string text, float maxW)
{
	if (text.empty() || CalcTextUnits(ctx, fontUnits, text).x <= maxW)
		return text;
	const std::string dots = "...";
	while (!text.empty() && CalcTextUnits(ctx, fontUnits, text + dots).x > maxW)
		EraseLastUtf8CodePoint(text);
	return text.empty() ? dots : text + dots;
}

struct ShellShape
{
	ImVec2 pos{};
	float size = 20.0f;
	float speed = 10.0f;
	float phase = 0.0f;
	float wobble = 12.0f;
	float rotation = 0.0f;
	float rotSpeed = 0.0f;
	int type = 0;
	float alpha = 0.08f;
};

std::vector<ShellShape> s_shellShapes;
ImVec2 s_shellShapeArea{};
constexpr uint32 kShellShapeSeed = 0x51574955u;

void EnsureShellShapes(const ShellLayoutContext& ctx)
{
	const int targetCount = std::clamp((int)std::round(30.0f * std::sqrt((ctx.layoutUnitsW * ctx.layoutUnitsH) / (1280.0f * 720.0f))), 30, 56);
	if ((int)s_shellShapes.size() == targetCount && std::abs(s_shellShapeArea.x - ctx.layoutUnitsW) < 8.0f && std::abs(s_shellShapeArea.y - ctx.layoutUnitsH) < 8.0f)
		return;

	s_shellShapeArea = { ctx.layoutUnitsW, ctx.layoutUnitsH };
	s_shellShapes.clear();
	std::mt19937 rng(kShellShapeSeed);
	std::uniform_real_distribution<float> unit(0.0f, 1.0f);
	for (int i = 0; i < targetCount; ++i)
	{
		ShellShape shape{};
		shape.pos = { unit(rng) * ctx.layoutUnitsW, unit(rng) * ctx.layoutUnitsH };
		shape.size = 14.0f + unit(rng) * 40.0f;
		shape.speed = 6.0f + unit(rng) * 22.0f;
		shape.phase = unit(rng) * 6.2831853f;
		shape.wobble = 6.0f + unit(rng) * 16.0f;
		shape.rotation = unit(rng) * 6.2831853f;
		shape.rotSpeed = (unit(rng) > 0.5f ? 1.0f : -1.0f) * (0.18f + unit(rng) * 0.32f);
		shape.type = i % 5;
		shape.alpha = 0.05f + unit(rng) * 0.11f;
		s_shellShapes.emplace_back(shape);
	}
}

void DrawShellShape(ImDrawList* draw, const ShellLayoutContext& ctx, const ShellShape& shape, ImU32 color)
{
	const ImVec2 c = ToPx(ctx, shape.pos);
	const float r = shape.size * ctx.uiScale;
	switch (shape.type)
	{
	case 0:
		draw->AddCircleFilled(c, r, color, 24);
		break;
	case 1:
	{
		std::array<ImVec2, 3> points{};
		for (int i = 0; i < 3; ++i)
		{
			const float a = shape.rotation + (float)i * 2.0943951f - 1.5707963f;
			points[i] = { c.x + std::cos(a) * r, c.y + std::sin(a) * r };
		}
		draw->AddTriangleFilled(points[0], points[1], points[2], color);
		break;
	}
	case 2:
		draw->AddRectFilled({ c.x - r, c.y - r }, { c.x + r, c.y + r }, color, r * 0.28f);
		break;
	case 3:
	{
		std::array<ImVec2, 4> points{};
		for (int i = 0; i < 4; ++i)
		{
			const float a = shape.rotation + (float)i * 1.5707963f;
			points[i] = { c.x + std::cos(a) * r, c.y + std::sin(a) * r };
		}
		draw->AddConvexPolyFilled(points.data(), (int)points.size(), color);
		break;
	}
	default:
	{
		std::array<ImVec2, 6> points{};
		for (int i = 0; i < 6; ++i)
		{
			const float a = shape.rotation + (float)i * 1.0471976f;
			points[i] = { c.x + std::cos(a) * r, c.y + std::sin(a) * r };
		}
		draw->AddConvexPolyFilled(points.data(), (int)points.size(), color);
		break;
	}
	}
}

// white controller silhouettes the floating shapes are drawn as, tinted with the theme's shape colour
constexpr float kShellShapeImageScale = 1.7f;
constexpr std::array<std::pair<const char*, const char*>, 5> kShellShapeImages{ {
	{ "background-gamepad", "background/bg-wii-u-gamepad.png" },
	{ "background-pro", "background/bg-wii-u-pro.png" },
	{ "background-remote", "background/bg-wii-remote.png" },
	{ "background-nunchuk", "background/bg-nunchuk.png" },
	{ "background-classic", "background/bg-classic.png" },
} };

// draws the shape's controller picture rotated with the shape and keeping its proportions; false until it is loaded
bool DrawShellShapeImage(ImDrawList* draw, const ShellLayoutContext& ctx, const ShellShape& shape, ImU32 color)
{
	const auto& [name, path] = kShellShapeImages[shape.type % kShellShapeImages.size()];
	const ShellTexture* texture = StaticTexture(name, path);
	if (!texture)
		return false;
	const float length = shape.size * kShellShapeImageScale * ctx.uiScale;
	const float aspect = (float)texture->width / (float)texture->height;
	const ImVec2 half = aspect >= 1.0f ? ImVec2(length, length / aspect) : ImVec2(length * aspect, length);
	const ImVec2 c = ToPx(ctx, shape.pos);
	const float cosine = std::cos(shape.rotation);
	const float sine = std::sin(shape.rotation);
	const auto corner = [&](float x, float y) {
		return ImVec2(c.x + x * cosine - y * sine, c.y + x * sine + y * cosine);
	};
	draw->AddImageQuad(texture->id,
		corner(-half.x, -half.y), corner(half.x, -half.y), corner(half.x, half.y), corner(-half.x, half.y),
		ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), color);
	return true;
}

void DrawShellBackground(const ShellLayoutContext& ctx, ImDrawList* draw, float dt, const std::vector<TitleTile>& titles)
{
	const auto& theme = CurrentShellTheme();
	const bool videoBackground = theme.mediaMode == ShellThemeMediaMode::DynamicVideo;
	if (!videoBackground && CemuUwp::VideoPlayer::IsOpen())
		ReleaseVideoBackground();
	if (theme.mediaMode != ShellThemeMediaMode::None)
	{
		if (videoBackground)
		{
			if (ShellTexture* videoTexture = ServiceVideoBackground(titles))
			{
				DrawShellTextureCoverPx(draw, videoTexture, { 0.0f, 0.0f }, ctx.viewportPx, 1.0f);
				draw->AddRectFilled({ 0, 0 }, ctx.viewportPx, IM_COL32(0, 0, 0, 112));
				draw->AddRectFilledMultiColor({ 0, 0 }, ctx.viewportPx,
					IM_COL32(0, 0, 0, 92), IM_COL32(0, 0, 0, 92),
					IM_COL32(0, 0, 0, 176), IM_COL32(0, 0, 0, 176));
				return;
			}
		}
		if (auto path = DynamicBackgroundPath(titles))
		{
			if (ShellTexture* texture = FileTexture("dynamic-background", *path))
			{
				if (ThemeBackgroundFitsInside())
					DrawShellTextureContainPx(draw, texture, { 0.0f, 0.0f }, ctx.viewportPx, 1.0f);
				else
					DrawShellTextureCoverPx(draw, texture, { 0.0f, 0.0f }, ctx.viewportPx, 1.0f);
				draw->AddRectFilled({ 0, 0 }, ctx.viewportPx, IM_COL32(0, 0, 0, 96));
				draw->AddRectFilledMultiColor({ 0, 0 }, ctx.viewportPx,
					IM_COL32(0, 0, 0, 92), IM_COL32(0, 0, 0, 92),
					IM_COL32(0, 0, 0, 176), IM_COL32(0, 0, 0, 176));
				return;
			}
		}
	}

	draw->AddRectFilledMultiColor({ 0, 0 }, ctx.viewportPx,
		Color(theme.backgroundAccent), Color(theme.backgroundAccent),
		Color(theme.background), Color(theme.background));

	EnsureShellShapes(ctx);
	for (auto& shape : s_shellShapes)
	{
		shape.pos.y -= shape.speed * dt;
		shape.pos.x += static_cast<float>(std::sin(ImGui::GetTime() * 0.7f + shape.phase)) * shape.wobble * dt;
		shape.rotation += shape.rotSpeed * dt;
		// the pictures reach further than the shapes, so they wrap once fully off screen
		const float extent = shape.size * kShellShapeImageScale;
		if (shape.pos.y + extent < -20.0f)
		{
			shape.pos.y = ctx.layoutUnitsH + extent + 20.0f;
			shape.pos.x = std::fmod(shape.pos.x + 319.0f, ctx.layoutUnitsW);
		}
		ImVec4 c = theme.shapeColor;
		c.w = shape.alpha;
		if (!DrawShellShapeImage(draw, ctx, shape, Color(c)))
			DrawShellShape(draw, ctx, shape, Color(c));
	}
}

void DrawShellClock(const ShellLayoutContext& ctx, ImDrawList* draw)
{
	const auto& theme = CurrentShellTheme();
	const UnitRect rect{ 24.0f, 14.0f, 150.0f, 62.0f };
	DrawGlassPanel(draw, ctx, rect, 20.0f, 0.92f);
	const std::string time = LocalTimeText();
	const std::string date = LocalDateText();
	ImVec2 timeSz = CalcTextUnits(ctx, 24.0f, time);
	ImVec2 dateSz = CalcTextUnits(ctx, 18.0f, date);
	const float totalH = timeSz.y + 2.0f + dateSz.y * 0.70f;
	float y = rect.y + (rect.h - totalH) * 0.5f;
	DrawTextUnits(draw, ctx, 24.0f, { rect.x + (rect.w - timeSz.x) * 0.5f, y }, Color(theme.textPrimary), time.c_str());
	y += timeSz.y + 2.0f;
	DrawTextUnits(draw, ctx, 12.6f, { rect.x + (rect.w - dateSz.x * 0.70f) * 0.5f, y }, Color(theme.textSecondary), date.c_str());
}

void DrawShellBattery(const ShellLayoutContext& ctx, ImDrawList* draw)
{
	const auto& theme = CurrentShellTheme();
	const UnitRect rect{ ctx.layoutUnitsW - 24.0f - 150.0f, 14.0f, 150.0f, 62.0f };
	DrawGlassPanel(draw, ctx, rect, 20.0f, 0.92f);
	const auto gamepads = winrt::Windows::Gaming::Input::Gamepad::Gamepads();
	const std::string label = gamepads.Size() > 0 ? fmt::format("{} pad", gamepads.Size()) : "No pad";
	const UnitRect body{ rect.x + 45.0f, rect.y + 12.0f, 46.0f, 22.0f };
	AddRoundedRectStrokePx(draw, RectMinPx(ctx, body), RectMaxPx(ctx, body), Color(theme.textPrimary), 9.0f * ctx.uiScale, 2.0f * ctx.uiScale);
	const UnitRect nub{ body.right() + 2.0f, body.y + 6.0f, 4.0f, 10.0f };
	draw->AddRectFilled(RectMinPx(ctx, nub), RectMaxPx(ctx, nub), Color(theme.textPrimary), 2.0f * ctx.uiScale);
	const UnitRect fill{ body.x + 4.0f, body.y + 4.0f, 30.0f, body.h - 8.0f };
	draw->AddRectFilled(RectMinPx(ctx, fill), RectMaxPx(ctx, fill), Color(theme.pageIndicatorActive), 5.0f * ctx.uiScale);
	ImVec2 textSz = CalcTextUnits(ctx, 13.5f, label);
	DrawTextUnits(draw, ctx, 13.5f, { rect.x + (rect.w - textSz.x) * 0.5f, rect.y + 40.0f }, Color(theme.textSecondary), label.c_str());
}

void DrawShellHomeLogo(const ShellLayoutContext& ctx, ImDrawList* draw)
{
	const UnitRect r{ ctx.layoutUnitsW * 0.5f - 32.0f, 13.0f, 64.0f, 64.0f };
	DrawGlassPanel(draw, ctx, r, 32.0f, 0.86f);
	DrawShellTexture(draw, ctx, StaticTexture("cemu", "icons/cemu.png"), r.Shrunk(8.0f), 24.0f, 1.0f);
}

void DrawShellTopHud(const ShellLayoutContext& ctx, ImDrawList* draw)
{
	DrawShellClock(ctx, draw);
	DrawShellHomeLogo(ctx, draw);
	DrawShellBattery(ctx, draw);
}

void ActivateShellRail(bool left, int index)
{
	if (left)
	{
		s_leftRailIndex = index;
		ActivateLeftRail();
		return;
	}
	s_rightRailIndex = index;
	ActivateRightRail();
}

UnitRect DrawShellRailButton(const ShellLayoutContext& ctx, ImDrawList* draw, bool left, int index, const char* textureName, const fs::path& assetPath, const char* label)
{
	const auto& theme = CurrentShellTheme();
	const float btnSize = 70.0f;
	const float gap = 16.0f;
	const float x = left ? 14.0f : ctx.layoutUnitsW - 14.0f - btnSize;
	const float totalH = 3.0f * btnSize + 2.0f * gap;
	const float y = ctx.layoutUnitsH * 0.5f - totalH * 0.5f + index * (btnSize + gap);
	const UnitRect rect{ x, y, btnSize, btnSize };
	const bool focused = (left && s_focusArea == FocusArea::LeftRail && s_leftRailIndex == index) || (!left && s_focusArea == FocusArea::RightRail && s_rightRailIndex == index);
	DrawGlassPanel(draw, ctx, rect, 16.0f, focused ? 1.0f : 0.88f);
	if (focused)
		AddRoundedRectStrokePx(draw, RectMinPx(ctx, rect.Expanded(1.5f)), RectMaxPx(ctx, rect.Expanded(1.5f)), Color(theme.cursorNormal), 17.5f * ctx.uiScale, 2.2f * ctx.uiScale);
	ShellTexture* texture = StaticTexture(textureName, assetPath);
	DrawShellTexture(draw, ctx, texture, rect.Shrunk(6.0f), 12.0f, focused ? 1.0f : 0.92f);

	const ImGuiIO& io = ImGui::GetIO();
	if (!s_titleContextMenu.open && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && rect.Contains(UnitPointFromPx(ctx, io.MousePos)))
	{
		s_focusArea = left ? FocusArea::LeftRail : FocusArea::RightRail;
		if (left)
			s_leftRailIndex = index;
		else
			s_rightRailIndex = index;
		ActivateShellRail(left, index);
	}
	if (focused)
	{
		const std::string text(label);
		ImVec2 size = CalcTextUnits(ctx, 13.0f, text);
		DrawTextUnits(draw, ctx, 13.0f, { rect.x + (rect.w - size.x) * 0.5f, rect.bottom() + 5.0f }, Color(theme.textSecondary), text.c_str());
	}
	return rect;
}

void DrawShellSidebars(const ShellLayoutContext& ctx, ImDrawList* draw)
{
	DrawShellRailButton(ctx, draw, true, 0, "home", "icons/home.png", Tr("Home"));
	DrawShellRailButton(ctx, draw, true, 1, "title_manager", "icons/title_manager.png", Tr("Title Manager"));
	DrawShellRailButton(ctx, draw, true, 2, "settings", "icons/settings.png", Tr("Settings"));
	DrawShellRailButton(ctx, draw, false, 0, "controllers", "icons/controllers.png", Tr("Controllers"));
	DrawShellRailButton(ctx, draw, false, 1, "graphics", "icons/graphics.png", Tr("Graphics"));
	DrawShellRailButton(ctx, draw, false, 2, "power", "icons/power.png", Tr("Power"));
}

constexpr float kFocusedTileGrowth = 0.20f;
constexpr float kTileArtworkInset = 8.0f;
constexpr float kTileArtworkRadius = 13.0f;

// a home tile at the size it grows to with the focus
UnitRect FocusedTileRect(const UnitRect& base)
{
	UnitRect tile = base;
	tile.w *= 1.0f + kFocusedTileGrowth;
	tile.h *= 1.0f + kFocusedTileGrowth;
	tile.x += (base.w - tile.w) * 0.5f;
	tile.y += (base.h - tile.h) * 0.5f;
	return tile;
}

// the largest part of bounds an image of this aspect ratio fills without changing shape
UnitRect ContainedRect(float aspect, const UnitRect& bounds)
{
	UnitRect rect = bounds;
	rect.w = std::min(bounds.w, bounds.h * aspect);
	rect.h = rect.w / std::max(0.001f, aspect);
	rect.x = bounds.x + (bounds.w - rect.w) * 0.5f;
	rect.y = bounds.y + (bounds.h - rect.h) * 0.5f;
	return rect;
}

float TileFocusAmount(int index, bool focused, float dt)
{
	float& amount = s_tileFocusAmount[index];
	const float target = focused ? 1.0f : 0.0f;
	const float rate = focused ? 10.0f : 8.0f;
	amount += (target - amount) * std::clamp(dt * rate, 0.0f, 1.0f);
	if (!focused && amount < 0.001f)
		amount = 0.0f;
	return amount;
}

void StartShellLaunch(const TitleTile& title, const UnitRect& rect, ShellTexture* texture)
{
	if (!title.launchable)
		return;
	// without DXC no game shader compiles: say why instead of booting into a black screen
	if (const std::string& compilerError = RendererShaderD3D12::GetCompilerError(); !compilerError.empty())
	{
		s_notificationCenter.Push(compilerError, 12.0f);
		return;
	}
	s_launchAnim.active = true;
	s_launchAnim.launched = false;
	s_launchAnim.timer = 0.0f;
	s_launchAnim.tileSide = std::min(rect.w, rect.h);
	s_launchAnim.aspect = 1.0f;
	s_launchAnim.path = title.path;
	s_launchAnim.texture = texture && texture->Valid() ? texture->id : nullptr;
	// start where the focused tile shows the artwork, so nothing jumps when the launch begins
	const UnitRect tile = FocusedTileRect(rect);
	s_launchAnim.from = tile;
	if (s_launchAnim.texture)
	{
		s_launchAnim.aspect = (float)texture->width / (float)texture->height;
		s_launchAnim.from = ContainedRect(s_launchAnim.aspect, tile.Shrunk(kTileArtworkInset * (1.0f + kFocusedTileGrowth)));
	}
}

struct TileArtwork
{
	UnitRect rect{};
	float scale = 1.0f;
	float alpha = 1.0f;
	ShellTexture* icon = nullptr;
	bool hasTitle = false;
	const std::string* typeLabel = nullptr;
};

void DrawShellTileArtwork(const ShellLayoutContext& ctx, ImDrawList* draw, const TileArtwork& artwork)
{
	const auto& theme = CurrentShellTheme();
	const UnitRect& rect = artwork.rect;
	const float scale = artwork.scale;
	const float alpha = artwork.alpha;
	if (artwork.icon)
	{
		DrawShellTextureContained(draw, ctx, artwork.icon, rect.Shrunk(kTileArtworkInset * scale),
			kTileArtworkRadius * scale, alpha);
	}
	else
	{
		ShellTexture* card = StaticTexture("gamecard", "icons/gamecard.png");
		DrawShellTexture(draw, ctx, card, rect.Shrunk(18.0f * scale), 12.0f * scale, alpha * (artwork.hasTitle ? 0.86f : 0.42f));
		if (!card)
		{
			const UnitRect cardRect = rect.Shrunk(36.0f * scale);
			AddRoundedRectFilledPx(draw, RectMinPx(ctx, cardRect), RectMaxPx(ctx, cardRect), ColorWithAlpha(theme.textSecondary, 0.18f * alpha), 7.0f * scale * ctx.uiScale);
		}
		if (!artwork.hasTitle)
		{
			const char* plus = "+";
			ImVec2 plusSize = CalcTextUnits(ctx, 42.0f * scale, plus);
			DrawTextUnits(draw, ctx, 42.0f * scale, { rect.x + (rect.w - plusSize.x) * 0.5f, rect.y + (rect.h - plusSize.y) * 0.5f }, ColorWithAlpha(theme.textSecondary, 0.55f * alpha), plus);
		}
	}
	if (artwork.typeLabel)
		DrawTextUnits(draw, ctx, 12.0f * scale, { rect.x + 10.0f * scale, rect.y + 9.0f * scale }, IM_COL32(255, 255, 255, (int)(210 * alpha)), artwork.typeLabel->c_str());
}

void DrawShellTile(const ShellLayoutContext& ctx, ImDrawList* draw, const std::vector<TitleTile>& titles, int globalIndex, const UnitRect& baseRect, float dt, bool pickerOpen, std::vector<TileArtwork>& artworks)
{
	const bool hasTitle = globalIndex < (int)titles.size();
	const bool focused = s_focusArea == FocusArea::HomeGrid && s_selectedTitle == globalIndex;
	const float focusT = TileFocusAmount(globalIndex, focused, dt);
	const int local = std::clamp(globalIndex - s_homePage * kHomePageSize, 0, kHomePageSize - 1);
	const int col = local % kHomeColumns;
	const int row = local / kHomeColumns;
	const float appearDelay = ((float)(col + row) / (float)((kHomeColumns - 1) + (kHomeRows - 1))) * 0.40f;
	const float appearT = Ease(ShellEasing::OutExpo, std::clamp((s_pageAppearTime - appearDelay) / 0.40f, 0.0f, 1.0f));
	if (appearT <= 0.01f)
		return;

	const float focusScale = 1.0f + kFocusedTileGrowth * std::clamp(
		Ease(ShellEasing::OutBack, focusT), 0.0f, 1.0f);
	const float scale = appearT * focusScale;
	UnitRect rect = baseRect;
	rect.w *= scale;
	rect.h *= scale;
	rect.x = baseRect.x + (baseRect.w - rect.w) * 0.5f;
	rect.y = baseRect.y + (baseRect.h - rect.h) * 0.5f;
	const float alpha = std::clamp(appearT, 0.0f, 1.0f);

	DrawGlassPanel(draw, ctx, rect, 16.0f, alpha);

	// the artwork is drawn in a later pass, above the cursor glow
	ShellTexture* icon = hasTitle ? GetTitleArtworkTexture(titles[globalIndex]) : nullptr;
	artworks.push_back({ rect, scale, alpha, icon, hasTitle,
		hasTitle && !titles[globalIndex].launchable ? &titles[globalIndex].type : nullptr });

	const ImGuiIO& io = ImGui::GetIO();
	if (!s_titleContextMenu.open)
	{
		if (hasTitle && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && baseRect.Contains(UnitPointFromPx(ctx, io.MousePos)))
		{
			s_focusArea = FocusArea::HomeGrid;
			s_selectedTitle = globalIndex;
			OpenTitleContextMenu(titles[globalIndex]);
		}
		else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && baseRect.Contains(UnitPointFromPx(ctx, io.MousePos)))
		{
			s_focusArea = FocusArea::HomeGrid;
			s_selectedTitle = globalIndex;
			if (hasTitle)
				StartShellLaunch(titles[globalIndex], baseRect, icon);
			else if (!pickerOpen)
			{
				s_titleManagerGamesDirectoryOpen = true;
				OpenPage(Page::TitleManager);
			}
		}
	}

	if (s_pendingLaunchIndex && *s_pendingLaunchIndex == globalIndex && hasTitle)
	{
		StartShellLaunch(titles[globalIndex], baseRect, icon);
		s_pendingLaunchIndex.reset();
	}
	if (s_pendingContextLaunchToken && hasTitle && TitleIconKey(titles[globalIndex]) == *s_pendingContextLaunchToken)
	{
		StartShellLaunch(titles[globalIndex], baseRect, icon);
		s_pendingContextLaunchToken.reset();
	}
}

void DrawShellTitlePill(const ShellLayoutContext& ctx, ImDrawList* draw, const std::vector<TitleTile>& titles, float dt)
{
	const auto& theme = CurrentShellTheme();
	std::string text;
	if (s_focusArea == FocusArea::HomeGrid && s_selectedTitle >= 0 && s_selectedTitle < (int)titles.size())
		text = titles[s_selectedTitle].launchable ? titles[s_selectedTitle].name : fmt::format("{} ({})", titles[s_selectedTitle].name, titles[s_selectedTitle].type);
	if (text.empty())
		text = Tr("Open title");

	const bool textChanged = text != s_titlePillText;
	if (textChanged)
	{
		s_titlePillText = text;
		s_titlePillReveal.SetImmediate(0.32f);
		s_titlePillReveal.Set(1.0f, 0.18f);
	}

	constexpr float horizontalPadding = 44.0f;
	const ImVec2 fullTextSize = CalcTextUnits(ctx, 24.0f, s_titlePillText);
	const float maxPillW = std::max(54.0f, ctx.layoutUnitsW - 64.0f);
	// slack so animation rounding or glyph overhang never trigger the ellipsis
	const float naturalPillW = std::ceil(fullTextSize.x) + horizontalPadding + 4.0f;
	const bool ellipsizeAtRest = naturalPillW > maxPillW;
	const float targetW = std::min(naturalPillW, maxPillW);
	const float targetX = (ctx.layoutUnitsW - targetW) * 0.5f;
	if (s_titlePillW.value <= 0.5f)
	{
		s_titlePillW.SetImmediate(std::min(targetW, 54.0f));
		s_titlePillX.SetImmediate((ctx.layoutUnitsW - s_titlePillW.value) * 0.5f);
	}
	if (textChanged || std::abs(s_titlePillW.target - targetW) >= 0.01f || std::abs(s_titlePillX.target - targetX) >= 0.01f)
	{
		s_titlePillW.Set(targetW, 0.22f);
		s_titlePillX.Set(targetX, 0.22f);
	}
	s_titlePillW.Update(dt);
	s_titlePillX.Update(dt);
	s_titlePillReveal.Update(dt);

	const UnitRect rect{ s_titlePillX.value, ctx.layoutUnitsH - 90.0f, s_titlePillW.value, 42.0f };
	DrawGlassPanel(draw, ctx, rect, 21.0f, 0.92f);
	const std::string fitted = ellipsizeAtRest ?
		EllipsizeUnits(ctx, 24.0f, s_titlePillText, std::max(4.0f, rect.w - horizontalPadding)) :
		s_titlePillText;
	const ImVec2 textSize = CalcTextUnits(ctx, 24.0f, fitted);
	const float reveal = std::clamp(s_titlePillReveal.value, 0.0f, 1.0f);
	draw->PushClipRect(RectMinPx(ctx, rect), RectMaxPx(ctx, rect));
	DrawTextUnits(draw, ctx, 24.0f, { rect.x + (rect.w - textSize.x) * 0.5f, rect.y + (rect.h - textSize.y) * 0.5f + (1.0f - reveal) * 3.0f }, ColorWithAlpha(theme.textPrimary, reveal), fitted.c_str());
	draw->PopClipRect();
}

void DrawShellPageIndicator(const ShellLayoutContext& ctx, ImDrawList* draw, int pageCount)
{
	if (pageCount <= 1)
		return;
	const auto& theme = CurrentShellTheme();
	constexpr float dotR = 4.0f;
	constexpr float gap = 14.0f;
	const float dotsW = pageCount * dotR * 2.0f + (pageCount - 1) * gap;
	const UnitRect panel{ (ctx.layoutUnitsW - (dotsW + 36.0f)) * 0.5f, ctx.layoutUnitsH - 35.0f, dotsW + 36.0f, 28.0f };
	DrawGlassPanel(draw, ctx, panel, 14.0f, 0.70f);
	const float startX = panel.x + 18.0f;
	const float cy = panel.y + panel.h * 0.5f;
	for (int i = 0; i < pageCount; ++i)
	{
		const ImVec2 center = ToPx(ctx, startX + i * (dotR * 2.0f + gap) + dotR, cy);
		draw->AddCircleFilled(center, dotR * ctx.uiScale, i == s_homePage ? Color(theme.pageIndicatorActive) : Color(theme.pageIndicator), 18);
	}
}

void DrawShellActionHints(const ShellLayoutContext& ctx, ImDrawList* draw)
{
	const auto& theme = CurrentShellTheme();
	const std::array<std::pair<const char*, const char*>, 6> hints{ {
		{ "A", "Select" },
		{ "B", s_page == Page::Home ? "Home" : "Back" },
		{ "X", "Title options" },
		{ "Y", "Theme" },
		{ "Menu", "Settings" },
		{ "View", "List" },
	} };
	const float rowH = 22.0f;
	const float padX = 10.0f;
	const float padY = 8.0f;
	const float gap = 3.0f;
	float contentW = 0.0f;
	for (const auto& [button, text] : hints)
		contentW = std::max(contentW, CalcTextUnits(ctx, 13.0f, std::string(button) + "  " + text).x);
	const float panelW = std::clamp(contentW + padX * 2.0f, 104.0f, 210.0f);
	const float panelH = padY * 2.0f + hints.size() * rowH + (hints.size() - 1) * gap;
	const UnitRect panel{ ctx.layoutUnitsW - 18.0f - panelW, ctx.layoutUnitsH - 18.0f - panelH, panelW, panelH };
	DrawGlassPanel(draw, ctx, panel, 16.0f, 0.72f);
	float y = panel.y + padY;
	for (const auto& [button, text] : hints)
	{
		DrawTextUnits(draw, ctx, 13.0f, { panel.x + padX, y + 2.0f }, Color(theme.textPrimary), button);
		DrawTextUnits(draw, ctx, 12.0f, { panel.x + padX + 42.0f, y + 3.0f }, Color(theme.textSecondary), text);
		y += rowH + gap;
	}
}

void UpdateShellCursor(const ShellLayoutContext& ctx, float dt)
{
	UnitRect cursorTarget{};
	bool hasCursorTarget = false;
	if (s_focusArea == FocusArea::HomeGrid)
	{
		// the cursor surrounds the tile at the size it grows to, so the artwork stays inside
		cursorTarget = FocusedTileRect(HomeSlotRect(ctx, s_selectedTitle));
		hasCursorTarget = true;
	}
	else if (s_focusArea == FocusArea::LeftRail)
	{
		const float btnSize = 70.0f;
		const float gap = 16.0f;
		const float totalH = 3.0f * btnSize + 2.0f * gap;
		cursorTarget = { 14.0f, ctx.layoutUnitsH * 0.5f - totalH * 0.5f + s_leftRailIndex * (btnSize + gap), btnSize, btnSize };
		hasCursorTarget = true;
	}
	else if (s_focusArea == FocusArea::RightRail)
	{
		const float btnSize = 70.0f;
		const float gap = 16.0f;
		const float totalH = 3.0f * btnSize + 2.0f * gap;
		cursorTarget = { ctx.layoutUnitsW - 14.0f - btnSize, ctx.layoutUnitsH * 0.5f - totalH * 0.5f + s_rightRailIndex * (btnSize + gap), btnSize, btnSize };
		hasCursorTarget = true;
	}
	if (hasCursorTarget)
		s_shellCursor.MoveTo(cursorTarget, s_focusArea == FocusArea::HomeGrid ? 18.0f : 16.0f, 0.10f);
	s_shellCursor.Update(dt);
}

void DrawShellHome(const ShellLayoutContext& ctx, ImDrawList* draw, const std::vector<TitleTile>& titles, bool pickerOpen, float dt)
{
	if (s_lastAnimatedHomePage != s_homePage)
	{
		s_lastAnimatedHomePage = s_homePage;
		s_pageAppearTime = 0.0f;
	}
	s_pageAppearTime += dt;

	const int pageCount = HomePageCount(titles.size());
	const int totalSlots = HomeSlotCount(titles.size());
	s_homePage = std::clamp(s_homePage, 0, pageCount - 1);
	s_selectedTitle = std::clamp(s_selectedTitle, 0, std::max(0, totalSlots - 1));
	const int firstSlot = HomePageFirstSlot(s_homePage);
	const int endSlot = HomePageEndSlot(titles.size(), s_homePage);
	std::vector<TileArtwork> artworks;
	artworks.reserve(endSlot - firstSlot);
	for (int globalIndex = firstSlot; globalIndex < endSlot; ++globalIndex)
	{
		DrawShellTile(ctx, draw, titles, globalIndex, HomeSlotRect(ctx, globalIndex), dt, pickerOpen, artworks);
	}
	// after the tiles so clicks and clamping have settled the selection for this frame
	UpdateShellCursor(ctx, dt);
	// layers: tile panels, cursor glow, artwork, then the cursor border once the caller has drawn the rest
	if (s_focusArea == FocusArea::HomeGrid)
		s_shellCursor.DrawGlow(ctx, draw, CurrentShellTheme());
	for (const TileArtwork& artwork : artworks)
		DrawShellTileArtwork(ctx, draw, artwork);
	DrawShellTitlePill(ctx, draw, titles, dt);
	DrawShellPageIndicator(ctx, draw, pageCount);
}

void DrawShellLaunchAnimation(const ShellLayoutContext& ctx, ImDrawList* draw, float dt)
{
	if (!s_launchAnim.active)
		return;
	s_launchAnim.timer += dt;
	constexpr float zoomDur = 0.45f;
	constexpr float holdDur = 0.35f;
	constexpr float fadeDur = 0.25f;
	constexpr float blackDur = 0.30f;
	// fixed width, height follows the artwork's aspect ratio
	float targetW = s_launchAnim.tileSide * 1.8f;
	float targetH = targetW / std::max(0.001f, s_launchAnim.aspect);
	const float maxTargetH = ctx.layoutUnitsH * 0.85f;
	if (targetH > maxTargetH)
	{
		targetH = maxTargetH;
		targetW = targetH * s_launchAnim.aspect;
	}
	const UnitRect target{ (ctx.layoutUnitsW - targetW) * 0.5f, (ctx.layoutUnitsH - targetH) * 0.5f, targetW, targetH };
	const float zoomT = Ease(ShellEasing::OutCubic, std::clamp(s_launchAnim.timer / zoomDur, 0.0f, 1.0f));
	UnitRect r{
		s_launchAnim.from.x + (target.x - s_launchAnim.from.x) * zoomT,
		s_launchAnim.from.y + (target.y - s_launchAnim.from.y) * zoomT,
		s_launchAnim.from.w + (target.w - s_launchAnim.from.w) * zoomT,
		s_launchAnim.from.h + (target.h - s_launchAnim.from.h) * zoomT,
	};
	const float fadeStart = zoomDur + holdDur;
	const float fadeT = std::clamp((s_launchAnim.timer - fadeStart) / fadeDur, 0.0f, 1.0f);
	draw->AddRectFilled({ 0, 0 }, ctx.viewportPx, IM_COL32(0, 0, 0, (int)(128.0f * zoomT)));
	// corners start as rounded as on the focused tile and round off further as it grows
	const float startRadius = s_launchAnim.texture ? kTileArtworkRadius * (1.0f + kFocusedTileGrowth) : 16.0f;
	const float radius = startRadius + (24.0f - startRadius) * zoomT;
	if (s_launchAnim.texture)
	{
		ImVec2 min = RectMinPx(ctx, r);
		ImVec2 max = RectMaxPx(ctx, r);
		NormalizeRoundedRectPx(min, max);
		draw->AddImageRounded(s_launchAnim.texture, min, max, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, (int)(255.0f * (1.0f - fadeT))), radius * ctx.uiScale, ImDrawFlags_RoundCornersAll);
	}
	else
		DrawGlassPanel(draw, ctx, r, radius, 1.0f - fadeT);

	if (!s_launchAnim.launched && s_launchAnim.timer >= fadeStart)
	{
		s_launchAnim.launched = true;
		CemuHost::LaunchPath(s_launchAnim.path);
	}
	const float blackT = std::clamp((s_launchAnim.timer - fadeStart - fadeDur) / blackDur, 0.0f, 1.0f);
	if (blackT > 0.0f)
		draw->AddRectFilled({ 0, 0 }, ctx.viewportPx, IM_COL32(0, 0, 0, (int)(255.0f * blackT)));
	if (s_launchAnim.timer > fadeStart + fadeDur + blackDur + 0.15f)
		s_launchAnim.active = false;
}

void DrawShellOverlay(const ShellLayoutContext& ctx, Page page)
{
	const auto& theme = CurrentShellTheme();
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const float panelInset = page == Page::GraphicPacks ? 0.0f : 32.0f;
	const UnitRect panel{ panelInset, panelInset, ctx.layoutUnitsW - panelInset * 2.0f, ctx.layoutUnitsH - panelInset * 2.0f };
	draw->AddRectFilled({ 0, 0 }, ctx.viewportPx, IM_COL32(0, 0, 0, theme.dark ? 122 : 78));
	DrawGlassPanel(draw, ctx, panel, 26.0f, 0.96f);

	ImGui::SetCursorScreenPos(RectMinPx(ctx, panel.Shrunk(30.0f)));
	const ImVec4 overlayFrameBg = theme.dark ? ImVec4(0.18f, 0.18f, 0.23f, 0.94f) : ImVec4(0.95f, 0.97f, 1.00f, 0.94f);
	const ImU32 overlayHoverBg = ColorWithAlpha(theme.cursorNormal, 0.28f);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_FrameBg, overlayFrameBg);
	ImGui::PushStyleColor(ImGuiCol_Button, overlayFrameBg);
	ImGui::PushStyleColor(ImGuiCol_Header, ColorWithAlpha(theme.cursorNormal, 0.22f));
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, overlayHoverBg);
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, ColorWithAlpha(theme.cursorNormal, 0.38f));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
	ImFont* overlayFont = ImGui_GetFont(kOverlayFontSizePx);
	if (overlayFont)
		ImGui::PushFont(overlayFont);
	ImGui::BeginChild("shell-overlay-content", ToPx(ctx, panel.w - 60.0f, panel.h - 60.0f), false, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NavFlattened | ImGuiWindowFlags_NoBackground);
	switch (page)
	{
	case Page::Settings:
		DrawSettings();
		break;
	case Page::Controllers:
		break;
	case Page::GraphicPacks:
		DrawGraphicPacks();
		break;
	case Page::TitleManager:
		DrawTitleManager(GetCachedTitles());
		break;
	case Page::Logs:
		DrawLogs();
		break;
	case Page::About:
		DrawAbout();
		break;
	case Page::Home:
	default:
		break;
	}
	ImGui::EndChild();
	if (overlayFont)
		ImGui::PopFont();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(6);
}

void DrawGlobalControllerMapper(const ShellLayoutContext& context, ImDrawList* draw)
{
	if (!s_globalInputMapper.IsOpen())
		s_globalInputMapper.Open(s_inputMappingHost, std::nullopt, 0);
	const auto result = s_globalInputMapper.Draw(context, draw,
		s_globalMapperFrameInput, ShellFont(18.0f, context));
	s_globalMapperFrameInput = {};
	if (!result.error.empty())
		PushStatus(result.error);
	if (result.saved)
		PushStatus(Tr("Global controller profiles saved"));
	if (result.saved || result.cancelled || !s_globalInputMapper.IsOpen())
		OpenPage(Page::Home);
}

void DrawShellFrontend(const std::vector<TitleTile>& titles,
	const std::vector<UwpImGuiFrontend::Notification>& notifications, bool pickerOpen)
{
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ShellLayoutContext ctx = MakeShellLayout(viewport->WorkSize);
	const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 1.0f / 15.0f);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->PushClipRect(viewport->WorkPos, viewport->WorkPos + viewport->WorkSize, false);
	DrawShellBackground(ctx, draw, dt, titles);
	DrawShellTopHud(ctx, draw);
	DrawShellSidebars(ctx, draw);
	// updates the cursor and, on the home grid, draws its glow under the artwork
	DrawShellHome(ctx, draw, titles, pickerOpen, dt);
	if (s_focusArea != FocusArea::HomeGrid)
		s_shellCursor.DrawGlow(ctx, draw, CurrentShellTheme());
	s_shellCursor.DrawBorder(ctx, draw, CurrentShellTheme());

	if (s_page == Page::Controllers)
		DrawGlobalControllerMapper(ctx, draw);
	else if (s_page != Page::Home)
		DrawShellOverlay(ctx, s_page);

	DrawShellLaunchAnimation(ctx, draw, dt);
	if (s_showActionHints && s_page == Page::Home)
	{
		s_actionHintsTimer += dt;
		if (s_actionHintsTimer <= kActionHintsVisibleSeconds)
			DrawShellActionHints(ctx, draw);
		else
			s_showActionHints = false;
	}

	UwpImGuiFrontend::DrawNotificationCards(draw, ctx, CurrentShellTheme(),
		notifications, ShellDrawFont(ctx));
	draw->PopClipRect();
}

void DrawTitleContextHeader(const ShellLayoutContext& ctx, ImDrawList* draw, const UnitRect& panel, const char* heading)
{
	const std::string displayTitle = UwpImGuiFrontend::NormalizeDisplayText(
		s_titleContextMenu.title.name);
	const std::string title = EllipsizeUnits(ctx, 15.0f, displayTitle, panel.w - 240.0f);
	const std::string titleId = fmt::format("{:016x}", s_titleContextMenu.baseTitleId);
	UwpImGuiFrontend::DrawOverlayHeader(draw, ctx, panel, CurrentShellTheme(),
		heading, title, titleId, ShellFont(18.0f, ctx));
}

void DrawTitleContextMainMenu(const ShellLayoutContext& ctx,
	ImDrawList* draw, const UnitRect& panel)
{
	UwpImGuiFrontend::TitleOptionsModel model;
	model.title = UwpImGuiFrontend::NormalizeDisplayText(s_titleContextMenu.title.name);
	model.titleId = fmt::format("{:016x}", s_titleContextMenu.baseTitleId);
	model.subtitle = s_titleContextMenu.title.type;
	model.artworkPlaceholder = "No selected large artwork is downloaded";
	if (const auto path = ConfiguredArtworkPath(s_titleContextMenu.title,
		CurrentDetailArtwork()); path)
	{
		if (const ShellTexture* artwork = FileTexture("title-information", *path);
			artwork && artwork->Valid())
		{
			model.artwork = {
				reinterpret_cast<std::uintptr_t>(artwork->id),
				static_cast<std::uint32_t>(artwork->width),
				static_cast<std::uint32_t>(artwork->height),
			};
		}
	}
	model.metadata = s_titleContextMenu.metadata;
	model.information = {
		{ Tr("Type"), s_titleContextMenu.title.type },
		{ Tr("Version"), fmt::format("{}", s_titleContextMenu.title.version) },
		{ Tr("Region"), s_titleContextMenu.title.region },
		{ Tr("Update"), s_titleContextMenu.hasUpdate ? Tr("Installed") : Tr("Not installed") },
		{ Tr("DLC"), s_titleContextMenu.hasDlc ? Tr("Installed") : Tr("Not installed") },
	};
	model.detailControllerScroll = -s_frameButtons.rightStickY * 520.0f *
		ImGui::GetIO().DeltaTime;
	model.actions.reserve(kTitleContextEntries.size());
	for (const auto& entry : kTitleContextEntries)
	{
		const TitleContextActionState actionState = GetTitleContextActionState(entry.action);
		model.actions.push_back({
			.id = entry.label,
			.label = Tr(entry.label),
			.detail = actionState.enabled ? std::string{} : std::string(actionState.reason),
			.enabled = actionState.enabled,
			.checkable = entry.action == TitleContextAction::Favorite,
			.checked = actionState.checked,
			.separatorBefore = entry.separatorBefore,
		});
	}
	UwpImGuiFrontend::TitleOptionsState state;
	state.actions.selected = static_cast<std::size_t>(std::clamp(
		s_titleContextMenu.selection, 0, static_cast<int>(model.actions.size()) - 1));
	state.actions.scroll = s_titleContextMenu.scroll;
	const auto result = UwpImGuiFrontend::DrawTitleOptionsView(ctx, draw, panel,
		state, model, ShellFont(18.0f, ctx));
	s_titleContextMenu.selection = static_cast<int>(state.actions.selected);
	s_titleContextMenu.scroll = state.actions.scroll;
	if (result.activatedAction)
	{
		const auto entry = std::ranges::find_if(kTitleContextEntries,
			[&](const TitleContextEntry& candidate) {
				return candidate.label == *result.activatedAction;
			});
		if (entry != kTitleContextEntries.end())
			s_pendingTitleContextActivation = entry->action;
	}
	if (result.closeRequested)
		CloseTitleContextMenu(true);
}

void DrawTitleGameProfileEditor(const ShellLayoutContext& ctx, ImDrawList* draw, const UnitRect& panel)
{
	auto& editor = s_titleContextMenu.gameProfile;
	std::vector<UwpImGuiFrontend::FormSection> sections;
	sections.reserve(kGameProfileSectionLabels.size());
	for (int sectionIndex = 0; sectionIndex < (int)kGameProfileSectionLabels.size(); ++sectionIndex)
	{
		TitleGameProfileEditorState sectionEditor = editor;
		sectionEditor.section = static_cast<GameProfileSection>(sectionIndex);
		UwpImGuiFrontend::FormSection section;
		section.id = std::to_string(sectionIndex);
		section.label = Tr(kGameProfileSectionLabels[sectionIndex]);
		const int rowCount = GameProfileRowCount(sectionEditor.section);
		section.fields.reserve(rowCount);
		for (int row = 0; row < rowCount; ++row)
		{
			const bool toggle = GameProfileFieldIsToggle(sectionEditor, row);
			const bool checked = sectionEditor.section == GameProfileSection::General &&
				(row == 0 ? sectionEditor.loadSharedLibraries :
				 row == 1 ? sectionEditor.startWithPadView : false);
			section.fields.push_back({
				.id = fmt::format("{}:{}", sectionIndex, row),
				.label = GameProfileFieldLabel(sectionEditor, row),
				.value = GameProfileFieldValue(sectionEditor, row),
				.kind = toggle ? UwpImGuiFrontend::FormFieldKind::Toggle :
					UwpImGuiFrontend::FormFieldKind::Choice,
				.checked = checked,
			});
		}
		sections.emplace_back(std::move(section));
	}
	std::vector<UwpImGuiFrontend::ChoiceItem> choices;
	const int choiceCount = GameProfileChoiceCount(editor, editor.row);
	choices.reserve(std::max(0, choiceCount));
	for (int choice = 0; choice < choiceCount; ++choice)
	{
		choices.push_back({ std::to_string(choice),
			GameProfileChoiceLabel(editor, editor.row, choice), true });
	}
	UwpImGuiFrontend::FormEditorState view{
		.section = static_cast<std::size_t>(GameProfileSectionIndex(editor.section)),
		.row = static_cast<std::size_t>(std::max(0, editor.row)),
		.scroll = editor.scroll,
		.choiceOpen = editor.comboOpen,
		.choiceSelection = static_cast<std::size_t>(std::max(0, editor.comboSelection)),
		.choiceScroll = static_cast<float>(editor.comboScroll),
	};
	const auto result = UwpImGuiFrontend::DrawFormEditor(ctx, draw, panel, view,
		sections, choices, ShellFont(18.0f, ctx));
	editor.section = static_cast<GameProfileSection>(view.section);
	editor.row = static_cast<int>(view.row);
	editor.scroll = view.scroll;
	editor.comboOpen = view.choiceOpen;
	editor.comboSelection = static_cast<int>(view.choiceSelection);
	editor.comboScroll = static_cast<int>(view.choiceScroll);
	if (result.selectedSection)
	{
		editor.row = std::clamp(editor.row, 0,
			std::max(0, GameProfileRowCount(editor.section) - 1));
		s_titleContextMenu.repeat = {};
	}
	if (result.activatedField)
	{
		editor.row = static_cast<int>(*result.activatedField);
		ActivateGameProfileField();
	}
	if (result.selectedChoice)
	{
		SetGameProfileChoice(editor, editor.row,
			static_cast<int>(*result.selectedChoice));
		editor.comboOpen = false;
		s_titleContextMenu.repeat = {};
	}
	if (result.save)
		SaveTitleGameProfile();
	else if (result.cancel)
	{
		s_titleContextMenu.view = TitleContextView::Menu;
		s_titleContextMenu.repeat = {};
	}
}
void DrawTitleCacheConfirmation(const ShellLayoutContext& ctx, ImDrawList* draw, const UnitRect& panel)
{
	const std::string count = fmt::format("{} cache item{} found ({:.1f} MB)", s_titleContextMenu.shaderCachePaths.size(),
		s_titleContextMenu.shaderCachePaths.size() == 1 ? "" : "s",
		(double)s_titleContextMenu.shaderCacheBytes / (1024.0 * 1024.0));
	const UwpImGuiFrontend::OverlayConfirmationModel model{
		.prompt = "Remove this title's shader caches?",
		.details = {
			"They will be rebuilt the next time the title starts.",
			count,
		},
		.actions = {
			{ "remove", "Remove" },
			{ "cancel", "Cancel" },
		},
		.selectedAction = static_cast<std::size_t>(
			std::clamp(s_titleContextMenu.confirmSelection, 0, 1)),
	};
	const auto result = UwpImGuiFrontend::DrawOverlayConfirmation(ctx, draw,
		panel, model, ShellFont(18.0f, ctx));
	if (result.activatedAction)
	{
		s_titleContextMenu.confirmSelection = static_cast<int>(*result.activatedAction);
		if (*result.activatedAction == 0)
		{
			RemoveTitleShaderCaches();
			return;
		}
		s_titleContextMenu.view = TitleContextView::Menu;
	}
}

void DrawTitleContextMenu()
{
	if (!s_titleContextMenu.open)
		return;

	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ShellLayoutContext ctx = MakeShellLayout(viewport->WorkSize);
	if (s_titleContextMenu.view == TitleContextView::ControllerMapping)
	{
		const auto result = s_inputMapper.Draw(ctx, ImGui::GetWindowDrawList(),
			s_mapperFrameInput, ShellFont(18.0f, ctx));
		s_mapperFrameInput = {};
		if (result.saved)
			PushStatus(Tr("Controller mapping saved"));
		if (result.saved || result.cancelled || !s_inputMapper.IsOpen())
		{
			s_titleContextMenu.view = TitleContextView::Menu;
			s_titleContextMenu.repeat = {};
		}
		if (!result.error.empty())
			PushStatus(result.error);
		return;
	}
	if (s_titleContextMenu.view == TitleContextView::Menu)
	{
		const UnitRect titlePanel{ 32.0f, 32.0f,
			std::max(1.0f, ctx.layoutUnitsW - 64.0f),
			std::max(1.0f, ctx.layoutUnitsH - 64.0f) };
		UwpImGuiFrontend::DrawDimmedBackground();
		DrawTitleContextMainMenu(ctx, ImGui::GetWindowDrawList(), titlePanel);
		return;
	}
	const bool metadataEditor = s_titleContextMenu.view == TitleContextView::EditMetadata;
	const bool metadataSearch = metadataEditor && s_metadataEditor &&
		s_metadataEditor->View() == UwpImGuiFrontend::MetadataEditorView::MatchBrowser;
	const float requestedWidth = metadataSearch ? ctx.layoutUnitsW - 36.0f :
		(metadataEditor ? 1080.0f : 720.0f);
	const float panelWidth = std::min(requestedWidth,
		std::max(420.0f, ctx.layoutUnitsW - (metadataSearch ? 36.0f : 80.0f)));
	const float requestedHeight = metadataSearch ? ctx.layoutUnitsH - 32.0f : 650.0f;
	const float panelHeight = std::min(requestedHeight,
		std::max(360.0f, ctx.layoutUnitsH - (metadataSearch ? 32.0f : 48.0f)));
	const UnitRect panel = UwpImGuiFrontend::CenteredPanel(ctx, panelWidth, panelHeight);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	UwpImGuiFrontend::DrawDimmedBackground();
	UwpImGuiFrontend::DrawOverlayPanel(draw, ctx, panel, CurrentShellTheme());
	const char* heading = "Title options";
	switch (s_titleContextMenu.view)
	{
	case TitleContextView::EditMetadata:
		heading = s_metadataEditor ? s_metadataEditor->Heading().data() : "Edit Metadata";
		break;
	case TitleContextView::EditGameProfile:
		heading = "Edit game profile";
		break;
	case TitleContextView::ControllerMapping:
		heading = "Controller Mapping";
		break;
	case TitleContextView::ConfirmCacheRemoval:
		heading = "Remove shader caches";
		break;
	case TitleContextView::Menu:
	default:
		break;
	}
	DrawTitleContextHeader(ctx, draw, panel, heading);

	ImGui::PushID("title-context-menu");
	ImFont* menuFont = metadataEditor ?
		ShellFont(18.0f, ctx) : ShellFont(18.0f, ctx);
	if (menuFont)
		ImGui::PushFont(menuFont);
	switch (s_titleContextMenu.view)
	{
	case TitleContextView::EditMetadata:
		if (s_metadataEditor)
		{
			const auto event = s_metadataEditor->Draw(ctx, draw, panel,
				CurrentShellTheme(), menuFont);
			if (event != UwpImGuiFrontend::MetadataEditorEvent::None)
			{
				s_titleContextMenu.view = TitleContextView::Menu;
				s_titleContextMenu.repeat = {};
			}
		}
		break;
	case TitleContextView::EditGameProfile:
		DrawTitleGameProfileEditor(ctx, draw, panel);
		break;
	case TitleContextView::ControllerMapping:
		break;
	case TitleContextView::ConfirmCacheRemoval:
		DrawTitleCacheConfirmation(ctx, draw, panel);
		break;
	case TitleContextView::Menu:
	default:
		DrawTitleContextMainMenu(ctx, draw, panel);
		break;
	}
	if (menuFont)
		ImGui::PopFont();
	ImGui::PopID();
}

void DrawUsersDialog(const ImGuiViewport& viewport)
{
	const auto& accounts = Account::GetAccounts();
	const uint32 activeAccount = GetConfig().account.m_persistent_id.GetValue();
	UwpImGuiFrontend::SelectionDialogModel model;
	model.popupId = "Who's playing?";
	model.title = Tr("Who's playing?");
	model.dismissRequested = s_usersDialogDismissRequested;
	model.entries.reserve(accounts.size());
	for (std::size_t index = 0; index < accounts.size(); ++index)
	{
		const Account& account = accounts[index];
		const uint32 id = account.GetPersistentId();
		std::string label = Narrow(account.ToString());
		if (label.empty())
			label = fmt::format("{:08x}", id);
		model.entries.push_back({ fmt::format("{:08x}", id), std::move(label), {}, true });
		if (id == activeAccount)
			model.selected = index;
	}
	const ShellLayoutContext context = MakeShellLayout(viewport.WorkSize);
	const auto result = UwpImGuiFrontend::DrawSelectionDialog(model, context,
		CurrentShellTheme(), { std::min(520.0f * context.uiScale,
			viewport.WorkSize.x - 80.0f * context.uiScale), 0.0f });
	if (result.selected && *result.selected < accounts.size())
	{
		GetConfig().account.m_persistent_id = accounts[*result.selected].GetPersistentId();
		SaveConfig();
		s_usersDialogOpen = false;
	}
	if (result.closed)
	{
		s_usersDialogOpen = false;
		s_usersDialogDismissRequested = false;
	}
	if (!ImGui::IsPopupOpen(model.popupId.c_str()))
		s_usersDialogOpen = false;
}

void DrawFrontend(const std::vector<TitleTile>& titles)
{
	const auto notifications = s_notificationCenter.Snapshot();
	bool pickerOpen;
	{
		std::lock_guard lock(s_frontendMutex);
		pickerOpen = s_pickerOpen;
	}

	ApplyStyle();
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

	if (!ImGui::Begin("CemuUwpFrontend", nullptr, flags))
	{
		ImGui::End();
		return;
	}

	s_requestContentFocus = false;
	DrawShellFrontend(titles, notifications, pickerOpen);

	if (s_showUsers)
	{
		ImGui::OpenPopup("Who's playing?");
		s_showUsers = false;
		s_usersDialogOpen = true;
		s_usersDialogDismissRequested = false;
	}
	if (s_usersDialogOpen)
		DrawUsersDialog(*viewport);

	DrawTitleContextMenu();
	DrawTitleInstallDialog();

	ImGui::End();
}

void DrawQuickMenuWindow()
{
	ApplyStyle();
	const uint64 titleId = CafeSystem::IsTitleRunning() ?
		CafeSystem::GetForegroundTitleId() : 0;
	const sint32 currentLayout = ResolveUwpLayoutForTitle(titleId);
	const bool streamBusy = CemuUwp::CompanionService::IsStreamBusy();
	const bool streamActive = CemuUwp::CompanionService::IsStreamActive();
	UwpImGuiFrontend::QuickMenuModel model;
	model.windowId = "CemuUwpQuickMenu";
	model.title = Tr("Quick Menu");
	model.footer = s_quickMenuLayoutComboOpen ?
		Tr("A Apply    B Cancel") : Tr("A Select    B Back    Menu Close");
	model.selectedRow = static_cast<std::size_t>(std::clamp(
		s_quickMenuSelection, 0, kQuickMenuItemCount - 1));
	model.choicePopupOpen = s_quickMenuLayoutComboOpen;
	model.choiceTitle = Tr("Layout mode");
	model.selectedChoice = static_cast<std::size_t>(std::clamp(
		s_quickMenuLayoutSelection, 0, static_cast<int>(kUwpLayoutEntries.size()) - 1));
	model.rows = {
		{ "return", Tr("Return to Game"), {}, true },
		{ "layout", Tr("Layout Mode"), Tr(LayoutName(currentLayout)), true },
		{ "mapping", Tr("Controller Mapping"), Tr("Per-game profile"), true },
		{ "overlay", Tr("Performance Overlay"),
			GetConfig().overlay.position == ScreenPosition::kDisabled ? Tr("Off") : Tr("On"), true },
		{ "stream", streamBusy ? Tr("Stream: working...") :
			(streamActive ? Tr("Stop Stream") : Tr("Start Stream")), {}, !streamBusy },
		{ "restart", Tr("Restart Game"), {}, true },
		{ "quit", Tr("Quit Game"), {}, true },
		{ "exit", Tr("Exit Cemu"), {}, true },
		{ "motion", Tr("Virtual gyro"),
			WGIGamepadController::GetVirtualMotionMode() == WGIGamepadController::VirtualMotionMode::Off ?
				Tr("Off") : Tr("Tilt (right stick)"), true },
	};
	model.choices.reserve(kUwpLayoutEntries.size());
	for (const auto& [_, label] : kUwpLayoutEntries)
		model.choices.push_back({ label, Tr(label), true });

	const auto result = UwpImGuiFrontend::DrawQuickMenu(model,
		CurrentShellTheme(), ImGui_GetFont(kOverlayFontSizePx));
	if (result.activatedRow)
		QueueQuickMenuActivation(static_cast<int>(*result.activatedRow));
	if (result.activatedChoice)
	{
		s_quickMenuLayoutSelection = static_cast<int>(*result.activatedChoice);
		ApplyQuickMenuLayoutSelection();
	}
}

void DrawQuickMenuMapperWindow()
{
	ApplyStyle();
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	if (!ImGui::Begin("CemuUwpControllerMapper", nullptr, flags))
	{
		ImGui::End();
		return;
	}
	const ShellLayoutContext context = MakeShellLayout(viewport->WorkSize);
	const auto result = s_inputMapper.Draw(context, ImGui::GetWindowDrawList(),
		s_mapperFrameInput, ShellFont(18.0f, context));
	s_mapperFrameInput = {};
	if (result.saved)
		PushStatus(Tr("Controller mapping saved"));
	if (result.saved || result.cancelled || !s_inputMapper.IsOpen())
	{
		s_quickMenuMapperOpen = false;
		s_quickMenuRepeatCounters = {};
	}
	if (!result.error.empty())
		PushStatus(result.error);
	ImGui::End();
}

std::unordered_map<uint64, std::unordered_set<std::string>> s_skippedShaderPacksNotifiedByTitle;

void NotifySkippedGraphicPackShaders()
{
	if (!GraphicPack2::IsReady())
		return;
	const uint64 titleId = CafeSystem::GetForegroundTitleId();
	auto& notifiedPacks = s_skippedShaderPacksNotifiedByTitle[titleId];
	std::vector<std::string> packNames;
	for (const auto& pack : GraphicPack2::GetActiveGraphicPacks())
	{
		if (!pack->HasSkippedShaders() || notifiedPacks.find(pack->GetVirtualPath()) != notifiedPacks.end())
			continue;
		notifiedPacks.emplace(pack->GetVirtualPath());
		packNames.emplace_back(pack->GetVirtualPath());
	}
	if (packNames.empty())
		return;
	constexpr size_t kListedPackLimit = 3;
	std::string packList;
	for (size_t i = 0; i < std::min(packNames.size(), kListedPackLimit); ++i)
	{
		if (!packList.empty())
			packList.append(", ");
		packList.append(packNames[i]);
	}
	if (packNames.size() > kListedPackLimit)
		packList.append(TrFormat(" and {} more", packNames.size() - kListedPackLimit));
	LatteOverlay_pushNotification(TrFormat("Some graphic pack shaders cannot run on Direct3D 12 and are skipped: {}", packList), 10000);
}
}

namespace CemuUwp::Frontend
{
ScopedInputMappingRead::ScopedInputMappingRead() noexcept
{
	++s_inputMappingReadDepth;
}

ScopedInputMappingRead::~ScopedInputMappingRead()
{
	if (s_inputMappingReadDepth > 0)
		--s_inputMappingReadDepth;
}

bool IsInputMappingReadActive()
{
	return s_inputMappingReadDepth != 0;
}

bool IsInputMapperOpen()
{
	return s_inputMapper.IsOpen();
}

void PushGameplayDialogStyle()
{
	UwpImGuiFrontend::SetWidgetTheme(CurrentShellTheme());
	UwpImGuiFrontend::PushGameplayDialogStyle();
}

void PopGameplayDialogStyle()
{
	UwpImGuiFrontend::PopGameplayDialogStyle();
}

void HandleTextEditingCommand(TextEditingCommand command, bool extendSelection)
{
	UwpImGuiFrontend::HandleNativeTextEditingCommand(
		static_cast<UwpImGuiFrontend::TextEditingCommand>(command), extendSelection);
}

void SetActive(bool active)
{
	if (s_frontendActive == active)
		return;
	s_frontendActive = active;
	if (!active)
	{
		if (CemuUwp::TitleInstaller::IsBusy())
			CemuUwp::TitleInstaller::RequestCancel();
		ReleaseVideoBackground();
		ReleaseFrontendContentTextures();
	}
}

void Initialize()
{
	s_frontendActive = true;
	UwpImGuiFrontend::SetTextTranslator(&Tr);
	LoadFrontendState();
	{
		std::lock_guard lock(s_frontendMutex);
		s_status = Tr("Ready");
		s_notificationCenter.Clear();
		s_pickerOpen = false;
	}
	s_page = Page::Home;
	s_settingsPage = SettingsPage::General;
	if (s_titleInstallPlanFuture.valid())
	{
		s_titleInstallPlanFuture.wait();
		s_titleInstallPlanFuture = std::future<CemuUwp::TitleInstaller::PlanResult>{};
	}
	CemuUwp::TitleInstaller::Shutdown();
	s_titleInstallDialogState = TitleInstallDialogState::Closed;
	s_titleInstallPlan.reset();
	s_titleInstallSnapshot = {};
	s_titleInstallMessage.clear();
	s_titleInstallCancelConfirmation = false;
	s_titleInstallCompletionHandled = false;
	s_titleInstallDismissRequested = false;
	s_titleInstallRequestDefaultFocus = false;
	s_titleInstallPickerRequested = false;
	s_titleInstallInputReleaseBarrier = false;
	s_titleInstallBackConsumedUntilIdle = false;
	s_homePage = 0;
	s_selectedTitle = 0;
	s_titleManagerSelection = 0;
	s_titleManagerGamesDirectoryOpen = false;
	s_pendingCompanionSelectionToken.reset();
	s_selectedGraphicPack = 0;
	s_graphicPackListRow = 0;
	s_expandedGraphicPackGroups.clear();
	s_quickMenuOpen = false;
	s_quickMenuSelection = 0;
	s_quickMenuLayoutComboOpen = false;
	s_quickMenuLayoutSelection = 0;
	s_pendingQuickMenuActivation = -1;
	s_quickMenuMapperOpen = false;
	s_inputMapper.Close();
	s_mapperFrameInput = {};
	s_globalInputMapper.Close();
	s_globalMapperFrameInput = {};
	s_globalMapperRepeatCounters = {};
	s_requestContentFocus = false;
	s_showUsers = false;
	s_usersDialogOpen = false;
	s_usersDialogDismissRequested = false;
	s_focusArea = FocusArea::HomeGrid;
	s_leftRailIndex = 0;
	s_rightRailIndex = 0;
	s_repeatCounters = {};
	s_quickMenuRepeatCounters = {};
	s_titleContextMenu = {};
	s_titleContextInputReleaseBarrier = false;
	s_pendingTitleContextActivation.reset();
	s_pendingLaunchIndex.reset();
	s_pendingContextLaunchToken.reset();
	s_pendingOverlayPaneFocus = OverlayPaneFocus::None;
	s_activeOverlayPaneFocus = OverlayPaneFocus::None;
	s_pageAppearTime = 10.0f;
	s_lastAnimatedHomePage = -1;
	s_shellCursor = {};
	s_titlePillX = {};
	s_titlePillW = {};
	s_titlePillReveal = {};
	s_titlePillText.clear();
	s_launchAnim = {};
	s_tileFocusAmount.clear();
	s_frontendUptime = 0.0f;
	s_titleIconDecodeQueue.clear();
	s_queuedTitleIconTextures.clear();
	if (s_titleIconDecodeFuture.valid())
	{
		s_titleIconDecodeFuture.wait();
		s_titleIconDecodeFuture = std::future<TitleIconDecodeResult>{};
	}
	if (s_staticTextureDecodeFuture.valid())
	{
		s_staticTextureDecodeFuture.wait();
		s_staticTextureDecodeFuture = std::future<StaticTextureDecodeResult>{};
	}
	if (s_titleCacheFuture.valid())
	{
		s_titleCacheFuture.wait();
		s_titleCacheFuture = std::future<TitleCacheBatchResult>{};
	}
	s_staticTextureDecodeQueue.clear();
	s_queuedStaticTextures.clear();
	s_missingStaticTextures.clear();
	PreloadStaticFrontendTextures();
	s_cachedTitles.clear();
	s_cachedHomeTitles.clear();
	s_titleManagerSelection = 0;
	s_titleCacheNextIndex = 0;
	s_titleCacheComplete = false;
	s_titleCacheDirty = true;
	s_lastTitleCacheUpdate = {};
	s_titleListChanged.store(true, std::memory_order_release);
	if (s_titleListCallbackId == 0)
	{
		s_titleListCallbackId = CafeTitleList::RegisterCallback(
			[](CafeTitleListCallbackEvent*, void*) {
				// runs under the scanner lock, must not touch frontend state
				s_titleListChanged.store(true, std::memory_order_release);
			}, nullptr);
	}
	UwpImGuiFrontend::ShutdownNativeTextInput();
	CemuUwp::InputProvider::SetCoreTextInputActive(false);
	if (ImGui::GetCurrentContext())
	{
		UwpImGuiFrontend::InitializeNativeTextInput({
			.contextName = "Cemu UWP text input",
			.setInputCapture = [](bool active) {
				CemuUwp::InputProvider::SetCoreTextInputActive(active);
			},
			.log = [](UwpImGuiFrontend::LogLevel level, std::string_view message) {
				if (level == UwpImGuiFrontend::LogLevel::Debug)
					cemuLog_logDebug(LogType::Force, "{}", message);
				else
					cemuLog_log(LogType::Force, "{}", message);
			},
		});
		int width = 0;
		int height = 0;
		WindowSystem::GetWindowPhysSize(width, height);
		const float uiScale = std::max(0.1f, std::min((float)std::max(width, 1) / 1280.0f,
			(float)std::max(height, 1) / 720.0f));
		ImGui_GetFont(std::max(10.0f, std::round(24.0f * uiScale)));
	}
	s_screenScraperResumeChecked = false;
}

void ServiceScreenScraperAutoResume()
{
	if (s_screenScraperResumeChecked)
		return;
	s_screenScraperResumeChecked = true;

	EnsureScreenScraperCredentialBuffers();
	auto& scraper = GetConfig().uwp_screenscraper;
	if (!scraper.enabled.GetValue() || !CemuUwp::ScreenScraperDownloader::HasPendingScan())
		return;

	const std::string user = s_screenScraperUserBuffer.data();
	const std::string password = s_screenScraperPasswordBuffer.data();
	if (user.empty() || password.empty())
		return;

	auto request = BuildScreenScraperRequest();
	if (CemuUwp::ScreenScraperDownloader::ResumePendingScan(std::move(request)))
		PushStatus(Tr("Resuming unfinished Screenscraper scan"));
}

void BeginFrame()
{
	const bool textInputWasActive =
		CemuUwp::InputProvider::IsCoreTextInputActive();
	UwpImGuiFrontend::BeginNativeTextInputFrame();
	ServiceTitleInstaller();
	const ButtonSnapshot releaseButtons = ReadButtons();
	if (s_titleInstallInputReleaseBarrier && !releaseButtons.a)
	{
		s_titleInstallInputReleaseBarrier = false;
		if (s_titleInstallPickerRequested)
		{
			s_titleInstallPickerRequested = false;
			PickTitleInstallFolderAsync();
		}
	}
	const bool nativeTextInputActive = CemuUwp::InputProvider::IsCoreTextInputActive();
	s_nativeTextInputOwnedFrame = textInputWasActive || nativeTextInputActive;
	const bool titleContextOwnsInput = s_titleContextMenu.open;
	const bool titleInstallOwnsInput = TitleInstallDialogOwnsInput();
	const bool usersDialogOwnsInput = s_usersDialogOpen;
	// the system keyboard and title context menu drive their own controller focus
	s_imguiOwnsNavigation = nativeTextInputActive || titleContextOwnsInput ||
		titleInstallOwnsInput || usersDialogOwnsInput ||
		(s_page != Page::Home && !s_quickMenuOpen);
	ImGui_UwpSetGamepadNavigationEnabled(!nativeTextInputActive && !titleContextOwnsInput &&
		!s_titleInstallInputReleaseBarrier && s_imguiOwnsNavigation && s_gyroComboCapture == GyroComboCapture::Off);

	s_frontendUptime += std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 1.0f / 15.0f);
	ServiceStaticTextureDecodeQueue();
	ServiceTitleIconDecodeQueue();
	ServiceScreenScraperAutoResume();
	if (CemuUwp::ScreenScraperDownloader::ConsumeFinished())
	{
		s_missingStaticTextures.clear();
		PushStatus(CemuUwp::ScreenScraperDownloader::GetStatus());
	}
}

void Draw()
{
	const bool nativeTextInputActive = CemuUwp::InputProvider::IsCoreTextInputActive();
	ButtonSnapshot buttons = ReadButtons();
	const bool comboCaptureOwnsInput = UpdateGyroComboCapture(buttons);
	if (comboCaptureOwnsInput)
		buttons = ButtonSnapshot{};
	BeginRetainedInputFrame(buttons);
	const auto& titles = GetCachedHomeTitles();
	bool pickerOpen;
	{
		std::lock_guard lock(s_frontendMutex);
		pickerOpen = s_pickerOpen;
	}
	const bool systemBackRequested = s_systemBackRequested.exchange(false);
	if (s_nativeTextInputOwnedFrame)
	{
		if (systemBackRequested || Pressed(buttons.b, s_previousButtons.b))
			UwpImGuiFrontend::DismissNativeTextInput();
	}
	else if (!nativeTextInputActive && !s_titleInstallInputReleaseBarrier && !comboCaptureOwnsInput)
	{
		if (s_titleContextInputReleaseBarrier)
		{
			if (!MapperControllerInputHeld(buttons) && !systemBackRequested)
				s_titleContextInputReleaseBarrier = false;
		}
		else if (TitleInstallDialogOwnsInput())
		{
			ProcessTitleInstallDialogInput(buttons, systemBackRequested);
		}
		else if (s_usersDialogOpen)
		{
			if (systemBackRequested || Pressed(buttons.b, s_previousButtons.b))
				s_usersDialogDismissRequested = true;
		}
		else
		{
			if (!s_titleContextMenu.open)
				ProcessGlobalShortcutBindings(buttons);
			ProcessShellInput(buttons, titles, pickerOpen, systemBackRequested);
		}
	}

	DrawFrontend(titles);
	if (s_page == Page::Home && s_focusArea == FocusArea::HomeGrid &&
		s_selectedTitle >= 0 && s_selectedTitle < (int)titles.size())
	{
		CemuUwp::CompanionService::SetSelectedTitleToken(TitleIconKey(titles[s_selectedTitle]));
	}
	UwpImGuiFrontend::EndNativeTextInputFrame();
	CommitRetainedInputFrame();
}

void TickGameplayInput()
{
	NotifySkippedGraphicPackShaders();
	const bool softwareKeyboardOwnsInput = swkbd_hasKeyboardInputHook() && !s_quickMenuOpen;
	const bool errorDialogOwnsInput = nn::erreula::hasInputHook() && !s_quickMenuOpen;
	const bool gameDialogOwnsInput = softwareKeyboardOwnsInput || errorDialogOwnsInput;
	s_imguiOwnsNavigation = gameDialogOwnsInput;
	ImGui_UwpSetGamepadNavigationEnabled(gameDialogOwnsInput);
	ButtonSnapshot buttons = ReadButtons();
	BeginRetainedInputFrame(buttons);
	if (softwareKeyboardOwnsInput)
	{
		// swkbd confirms with Menu, keep frontend shortcuts out of its session
		CommitRetainedInputFrame();
		return;
	}
	if (errorDialogOwnsInput)
	{
		CommitRetainedInputFrame();
		return;
	}
	const bool queuedActionConsumed = ConsumeQueuedQuickMenuActivation();
	const bool mapperOwnsInput = s_quickMenuMapperOpen && s_inputMapper.IsOpen();
	const bool shortcutConsumed = queuedActionConsumed || mapperOwnsInput ?
		false : ProcessGlobalShortcutBindings(buttons);
	if (s_quickMenuOpen && !queuedActionConsumed && !shortcutConsumed)
		ProcessQuickMenuInput(buttons);
	CommitRetainedInputFrame();
}

bool WantsGameplayOverlay()
{
	return s_quickMenuOpen;
}

bool IsGameplayInputCaptured()
{
	return s_quickMenuOpen || nn::erreula::hasInputHook();
}

void DrawGameplayOverlay()
{
	if (!s_quickMenuOpen)
		return;
	if (s_quickMenuMapperOpen)
		DrawQuickMenuMapperWindow();
	else
		DrawQuickMenuWindow();
}

void Shutdown()
{
	s_frontendActive = false;
	if (s_titleListCallbackId != 0)
	{
		CafeTitleList::UnregisterCallback(s_titleListCallbackId);
		s_titleListCallbackId = 0;
	}
	CemuUwp::ScreenScraperDownloader::RequestStop();
	if (s_titleInstallPlanFuture.valid())
	{
		s_titleInstallPlanFuture.wait();
		s_titleInstallPlanFuture = std::future<CemuUwp::TitleInstaller::PlanResult>{};
	}
	CemuUwp::TitleInstaller::Shutdown();
	ReleaseVideoBackground();
	CemuUwp::VideoPlayer::Shutdown();
	if (s_titleIconDecodeFuture.valid())
	{
		s_titleIconDecodeFuture.wait();
		s_titleIconDecodeFuture = std::future<TitleIconDecodeResult>{};
	}
	if (s_staticTextureDecodeFuture.valid())
	{
		s_staticTextureDecodeFuture.wait();
		s_staticTextureDecodeFuture = std::future<StaticTextureDecodeResult>{};
	}
	if (s_titleCacheFuture.valid())
	{
		s_titleCacheFuture.wait();
		s_titleCacheFuture = std::future<TitleCacheBatchResult>{};
	}
	ReleaseFrontendTextures();
	s_titleIconDecodeQueue.clear();
	s_queuedTitleIconTextures.clear();
	s_staticTextureDecodeQueue.clear();
	s_queuedStaticTextures.clear();
	s_missingStaticTextures.clear();
	s_cachedTitles.clear();
	s_cachedHomeTitles.clear();
	s_titleCacheNextIndex = 0;
	s_titleCacheComplete = false;
	s_shellShapes.clear();
	s_tileFocusAmount.clear();
	s_pendingLaunchIndex.reset();
	s_pendingContextLaunchToken.reset();
	s_titleContextMenu = {};
	s_titleContextInputReleaseBarrier = false;
	s_pendingTitleContextActivation.reset();
	s_titleInstallDialogState = TitleInstallDialogState::Closed;
	s_titleInstallPlan.reset();
	s_titleInstallSnapshot = {};
	s_titleInstallMessage.clear();
	s_titleInstallCancelConfirmation = false;
	s_titleInstallCompletionHandled = false;
	s_titleInstallDismissRequested = false;
	s_titleInstallRequestDefaultFocus = false;
	s_titleInstallPickerRequested = false;
	s_titleInstallInputReleaseBarrier = false;
	s_titleInstallBackConsumedUntilIdle = false;
	s_showUsers = false;
	s_usersDialogOpen = false;
	s_usersDialogDismissRequested = false;
	s_quickMenuOpen = false;
	s_quickMenuSelection = 0;
	s_quickMenuLayoutComboOpen = false;
	s_quickMenuLayoutSelection = 0;
	s_pendingQuickMenuActivation = -1;
	s_quickMenuMapperOpen = false;
	s_inputMapper.Close();
	s_mapperFrameInput = {};
	s_globalInputMapper.Close();
	s_globalMapperFrameInput = {};
	s_globalMapperRepeatCounters = {};
	s_quickMenuRepeatCounters = {};
	s_nativeTextInputOwnedFrame = false;
	s_launchAnim = {};
	if (s_pickerOpen)
		PickerService().CancelCallbacks();
	UwpImGuiFrontend::ShutdownNativeTextInput();
	CemuUwp::InputProvider::SetCoreTextInputActive(false);
	std::lock_guard lock(s_frontendMutex);
	s_notificationCenter.Clear();
	s_status.clear();
	s_pickerOpen = false;
}

void RequestBack()
{
	s_systemBackRequested = true;
}

void SetStatus(std::string message)
{
	std::lock_guard lock(s_frontendMutex);
	s_status = std::move(message);
}

void PushNotification(std::string message)
{
	s_notificationCenter.Push(std::move(message), 5.0f);
}

UwpImGuiFrontend::TextureHandle GetControllerPreviewTexture(
	std::string_view assetName)
{
	if (assetName != "wii-u-gamepad" &&
		assetName != "wii-u-pro-controller" && assetName != "wii-remote")
	{
		return {};
	}
	ShellTexture* texture = StaticTexture(
		"controller-preview:" + std::string(assetName),
		fs::path("controller-previews") /
			(std::string(assetName) + ".png"));
	if (!texture || !texture->Valid())
		return {};
	return { reinterpret_cast<std::uintptr_t>(texture->id),
		static_cast<std::uint32_t>(texture->width),
		static_cast<std::uint32_t>(texture->height) };
}
}
