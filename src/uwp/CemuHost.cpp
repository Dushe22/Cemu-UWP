#include "CemuHost.h"
#include "UwpBuildConfig.h"

#include "UwpAudioBackend.h"
#include "UwpFrontend.h"
#include "UwpMemoryBudgetService.h"
#include "UwpCompanionService.h"
#include "UwpStorageService.h"
#include "UwpTranslation.h"
#include "UwpWindowSystem.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/Account/Account.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/vpad/vpad.h"
#include "Cafe/TitleList/SaveList.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cemu/Logging/CemuLogging.h"
#include "WindowSystem.h"
#include "audio/IAudioAPI.h"
#include "audio/IAudioInputAPI.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/LaunchSettings.h"
#include "config/NetworkSettings.h"
#include "input/ControllerFactory.h"
#include "input/InputManager.h"
#include "input/api/WGI/WGIGamepadController.h"
#include "util/crypto/aes128.h"
#include "util/helpers/helpers.h"

#include <pugixml.hpp>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Storage.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#if defined(CEMU_UWP_HOST_TRACE)
#include <chrono>
#include <fstream>
#endif

std::atomic_bool g_isGPUInitFinished = false;

namespace
{
bool s_initialized = false;
std::atomic_bool s_windowVisible{ true };
std::atomic_bool s_windowActive{ true };
std::atomic_bool s_recoveringGraphics{ false };
std::atomic_int s_frontendActivityState{ -1 };
std::once_flag s_audioInitOnce;
std::once_flag s_graphicPacksLoadOnce;
std::optional<fs::path> s_lastLaunchPath;
bool s_titlePaused = false;
bool s_titleLaunchOverridesActive = false;
bool s_titleLaunchObservedRunning = false;

void EnsureDefaultGameDirectory()
{
	auto& gamePaths = GetConfig().game_paths;
	if (!gamePaths.empty())
		return;
	gamePaths.emplace_back("E:\\Games\\wiiu");
	GetConfigHandle().Save();
	cemuLog_log(LogType::Force,
		"UWP configured the default Wii U games directory: {}",
		gamePaths.front());
}

template<typename Callback>
decltype(auto) InvokeRenderer(std::string_view operation, Callback&& callback)
{
	try
	{
		return std::forward<Callback>(callback)();
	}
	catch (const winrt::hresult_error& ex)
	{
		throw CemuHost::GraphicsFailure(fmt::format(
			"{} failed with WinRT error 0x{:08X}: {}", operation, (uint32)ex.code(), winrt::to_string(ex.message())));
	}
	catch (const std::exception& ex)
	{
		throw CemuHost::GraphicsFailure(fmt::format("{} failed: {}", operation, ex.what()));
	}
	catch (...)
	{
		throw CemuHost::GraphicsFailure(fmt::format("{} failed with an unknown renderer exception", operation));
	}
}

void TraceHostStage(std::string_view message)
{
#if defined(CEMU_UWP_HOST_TRACE)
	try
	{
		const auto localFolder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
		fs::path logPath{ std::wstring(localFolder.c_str()) };
		logPath /= "bootstrap.log";

		static const auto start = std::chrono::steady_clock::now();
		const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		std::ofstream logFile(logPath, std::ios::app);
		logFile << "host[" << elapsedMs << "ms]: " << message << '\n';
	}
	catch (...)
	{
	}
#else
	(void)message;
#endif
}

Vector2i CurrentSurfaceSize()
{
	int width = 0;
	int height = 0;
	WindowSystem::GetWindowPhysSize(width, height);
	return { std::max(1, width), std::max(1, height) };
}

void SyncFrontendActivity()
{
	if (!s_initialized)
		return;
	const auto calculateActivity = []()
	{
		return s_windowVisible.load(std::memory_order_acquire) &&
		s_windowActive.load(std::memory_order_acquire) &&
		!s_recoveringGraphics.load(std::memory_order_acquire) &&
		!CafeSystem::IsTitleRunning();
	};
	const int requestedState = calculateActivity() ? 1 : 0;
	if (s_frontendActivityState.load(std::memory_order_acquire) == requestedState)
		return;

	std::unique_lock<std::recursive_mutex> rendererLock(g_rendererMutex);
	const int currentState = calculateActivity() ? 1 : 0;
	if (s_frontendActivityState.load(std::memory_order_relaxed) == currentState)
		return;
	CemuUwp::Frontend::SetActive(currentState != 0);
	s_frontendActivityState.store(currentState, std::memory_order_release);
}

// a GamePad and four controllers, the most players a Wii U game reads by default
constexpr size_t kMaxAutomaticPlayers = 5;
std::atomic_uint32_t s_gamepadArrivals{ 0 };

fs::path GlobalControllerProfilePath(size_t player)
{
	return ActiveSettings::GetConfigPath("controllerProfiles/controller{}.xml", player);
}

// player 1 is the GamePad and the others Pro Controllers, each with the layout its emulated controller defines for Xbox pads
bool CreateDefaultGamepadPlayer(size_t player, uint32 padPosition, WGIGamepadController::Binding binding)
{
	auto& input = InputManager::instance();
	const auto type = player == 0 ? EmulatedController::VPAD : EmulatedController::Pro;
	if (!input.can_set_controller(player, type))
		return false;
	auto emulated = ControllerFactory::CreateEmulatedController(player, type);
	const auto pad = std::make_shared<WGIGamepadController>(padPosition, binding);
	pad->set_rumble(1.0f);
	emulated->add_controller(pad);
	emulated->set_default_mapping(pad);
	input.set_controller(emulated);
	if (!input.save(player))
	{
		input.delete_controller(player);
		cemuLog_log(LogType::Force, "UWP could not save the default controller profile for player {}", player + 1);
		return false;
	}
	cemuLog_log(LogType::Force, "UWP created default controller profile for player {} on Xbox Gamepad {}",
		player + 1, padPosition + 1);
	return true;
}

void RepairLegacyGamepadProfile(const fs::path& profilePath)
{
	std::ifstream input(profilePath);
	std::stringstream buffer;
	buffer << input.rdbuf();
	std::string profile = buffer.str();
	if (profile.find("<api>WGIGamepad</api>") == std::string::npos ||
		profile.find("<type>VPAD</type>") == std::string::npos)
		return;

	constexpr std::string_view oldType = "<type>VPAD</type>";
	constexpr std::string_view newType = "<type>Wii U GamePad</type>";
	profile.replace(profile.find(oldType), oldType.size(), newType);
	std::ofstream output(profilePath, std::ios::trunc);
	output << profile;
	cemuLog_log(LogType::Force, "UWP repaired default WGIGamepad controller profile: {}", _pathToUtf8(profilePath));
}

// recreates untouched default Pro Controller profiles that were written with the GamePad mapping numbers
void RepairDefaultProControllerProfile(size_t player)
{
	const fs::path profilePath = GlobalControllerProfilePath(player);
	std::ifstream input(profilePath);
	if (!input)
		return;
	std::stringstream buffer;
	buffer << input.rdbuf();
	input.close();
	const std::string profile = buffer.str();
	if (profile.find("<type>Wii U Pro Controller</type>") == std::string::npos ||
		profile.find("<api>WGIGamepad</api>") == std::string::npos ||
		profile.find("<uwp_expressions") != std::string::npos ||
		profile.find("<entry><mapping>11</mapping><button>0</button></entry>") == std::string::npos)
	{
		return;
	}

	uint32 padPosition = static_cast<uint32>(player);
	constexpr std::string_view kUuidTag = "<uuid>";
	if (const size_t uuid = profile.find(kUuidTag); uuid != std::string::npos)
		(void)std::from_chars(profile.data() + uuid + kUuidTag.size(), profile.data() + profile.size(), padPosition);
	std::error_code ec;
	fs::remove(profilePath, ec);
	if (CreateDefaultGamepadPlayer(player, padPosition, WGIGamepadController::Binding::Position))
		cemuLog_log(LogType::Force, "UWP repaired the Pro Controller layout for player {}", player + 1);
}

void EnsureDefaultControllerProfile()
{
	const fs::path playerOneProfile = GlobalControllerProfilePath(0);
	std::error_code ec;
	fs::create_directories(playerOneProfile.parent_path(), ec);
	// the pad list is still empty here, so player 1 follows whichever pad is listed first
	if (fs::exists(playerOneProfile, ec))
		RepairLegacyGamepadProfile(playerOneProfile);
	else
		CreateDefaultGamepadPlayer(0, 0, WGIGamepadController::Binding::Position);
	for (size_t player = 1; player < kMaxAutomaticPlayers; ++player)
		RepairDefaultProControllerProfile(player);
}

// extra players are bound like player 1: by position in the pad list, or to their own pad once player 1 is tied to one
WGIGamepadController::Binding AutomaticPlayerBinding()
{
	pugi::xml_document document;
	if (!document.load_file(GlobalControllerProfilePath(0).c_str()))
		return WGIGamepadController::Binding::Position;
	for (const auto& selected : document.child("emulated_controller").select_nodes("controller"))
	{
		const auto node = selected.node();
		if (std::string_view(node.child_value("api")) != InputAPI::to_string(InputAPI::WGIGamepad))
			continue;
		const std::string_view uuid = node.child_value("uuid");
		const bool followsPosition = !uuid.empty() &&
			std::ranges::all_of(uuid, [](char c) { return c >= '0' && c <= '9'; });
		return followsPosition ? WGIGamepadController::Binding::Position : WGIGamepadController::Binding::Gamepad;
	}
	return WGIGamepadController::Binding::Position;
}

struct GamepadAssignments
{
	std::vector<bool> claimed; // per position in the pad list
	std::vector<std::pair<size_t, std::shared_ptr<WGIGamepadController>>> dropped; // players whose pad went away, in player order
	bool playerOneUsesGamepad = false;
};

GamepadAssignments ReadGamepadAssignments(const std::vector<std::string>& identities)
{
	GamepadAssignments result;
	result.claimed.assign(identities.size(), false);
	for (size_t player = 0; player < InputManager::kMaxController; ++player)
	{
		const auto controller = InputManager::instance().get_controller(player);
		if (!controller)
			continue;
		for (const auto& device : controller->get_controllers())
		{
			const auto pad = std::dynamic_pointer_cast<WGIGamepadController>(device);
			if (!pad)
				continue;
			if (player == 0)
				result.playerOneUsesGamepad = true;
			const std::string session = pad->session_gamepad_id();
			const std::string tied = session.empty() ? pad->non_roamable_id() : session;
			if (tied.empty())
			{
				// not connected yet, it takes the pad at its position
				if (pad->enumeration_index() < result.claimed.size())
					result.claimed[pad->enumeration_index()] = true;
				continue;
			}
			if (const auto found = std::ranges::find(identities, tied); found != identities.end())
				result.claimed[std::distance(identities.begin(), found)] = true;
			else if (!session.empty())
				result.dropped.emplace_back(player, pad);
		}
	}
	return result;
}

// unused pads refill dropped players first, then become new players
void AssignNewGamepads()
{
	using winrt::Windows::Gaming::Input::Gamepad;
	static std::once_flag s_watchStarted;
	std::call_once(s_watchStarted, []()
	{
		try
		{
			Gamepad::GamepadAdded([](auto&&, auto&&) { s_gamepadArrivals.fetch_add(1, std::memory_order_release); });
		}
		catch (const winrt::hresult_error& ex)
		{
			cemuLog_log(LogType::Force, "UWP could not watch for new gamepads (0x{:08x})", (uint32)ex.code());
		}
	});

	static uint32 s_handledArrivals = std::numeric_limits<uint32>::max();
	static uint32 s_handledPadCount = 0;
	static bool s_newPlayersPending = false;
	try
	{
		auto& input = InputManager::instance();
		// not while the mapper is open, a title profile is active or player 1 is missing
		bool canAddPlayers = !CemuUwp::Frontend::IsInputMapperOpen() && input.get_controller(0) != nullptr;
		for (size_t player = 0; player < InputManager::kMaxController && canAddPlayers; ++player)
			canAddPlayers = !input.is_gameprofile_set(player);

		const auto gamepads = Gamepad::Gamepads();
		const uint32 arrivals = s_gamepadArrivals.load(std::memory_order_acquire);
		const uint32 padCount = gamepads.Size();
		if (arrivals == s_handledArrivals && padCount == s_handledPadCount && !(s_newPlayersPending && canAddPlayers))
			return;
		s_handledArrivals = arrivals;
		s_handledPadCount = padCount;
		s_newPlayersPending = false;

		std::vector<std::string> identities;
		identities.reserve(padCount);
		for (const auto& gamepad : gamepads)
			identities.push_back(WGIGamepadControllerProvider::GetNonRoamableId(gamepad));
		GamepadAssignments assignments = ReadGamepadAssignments(identities);

		std::vector<uint32> freePads;
		for (uint32 position = 0; position < padCount; ++position)
		{
			if (assignments.claimed[position])
				continue;
			// a dropped slot is refilled first, lowest player first
			if (!assignments.dropped.empty() && !identities[position].empty())
			{
				const auto [player, pad] = assignments.dropped.front();
				assignments.dropped.erase(assignments.dropped.begin());
				pad->use_gamepad_for_session(identities[position]);
				cemuLog_log(LogType::Force, "UWP moved player {} to Xbox Gamepad {}", player + 1, position + 1);
				continue;
			}
			freePads.push_back(position);
		}
		// setups where player 1 is not an Xbox pad, such as a phone over DSU, stay as configured
		if (freePads.empty() || !assignments.playerOneUsesGamepad)
			return;
		if (!canAddPlayers)
		{
			s_newPlayersPending = true;
			return;
		}

		const auto binding = AutomaticPlayerBinding();
		size_t player = 1;
		for (const uint32 position : freePads)
		{
			std::error_code ec;
			while (player < kMaxAutomaticPlayers &&
				(input.get_controller(player) || fs::exists(GlobalControllerProfilePath(player), ec)))
			{
				++player;
			}
			if (player >= kMaxAutomaticPlayers || !CreateDefaultGamepadPlayer(player, position, binding))
				return;
#if HAS_DSU
			if (std::ranges::any_of(GetConfig().dsu_client.player_routes,
				[player](const auto& route) { return route.player == player; }))
			{
				input.apply_uwp_dsu_routes();
			}
#endif
			++player;
		}
	}
	catch (const winrt::hresult_error& ex)
	{
		cemuLog_log(LogType::Force, "UWP could not assign new gamepads (0x{:08x})", (uint32)ex.code());
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "UWP could not assign new gamepads: {}", ex.what());
	}
}

void InitializeCoreServices()
{
	if (!SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS))
		cemuLog_log(LogType::Force, "UWP could not enable Cemu's above-normal process priority (Win32 error {}).", GetLastError());

	AES128_init();
	PPCTimer_init();
	LatteOverlay_init();

	GetConfigHandle().Load();
	EnsureDefaultGameDirectory();
	GetConfig().graphic_api = kDirect3D12;
	CemuUwp::AudioBackend::ConfigureDefaultDevice();

	if (NetworkConfig::XMLExists())
		n_config.Load();

	const fs::path mlcPath = ActiveSettings::GetMlcPath();
	ActiveSettings::Init();

	CemuUwp::Translation::SetLanguage(GetConfig().uwp_display.ui_language.GetValue());
	EnsureDefaultControllerProfile();
	InputManager::instance().load();

	CafeSystem::Initialize();
	CafeTitleList::Initialize(ActiveSettings::GetUserDataPath("title_list_cache.xml"));
	for (auto& it : GetConfig().game_paths)
		CafeTitleList::AddScanPath(_utf8ToPath(it));

	if (!mlcPath.empty())
		CafeTitleList::SetMLCPath(mlcPath);
	CafeTitleList::Refresh();

	CemuUwp::CompanionService::Initialize();
	{
		std::string metadataLanguage = GetConfig().uwp_screenscraper.metadata_language.GetValue();
		if (metadataLanguage.empty())
			metadataLanguage = GetConfig().uwp_display.ui_language.GetValue();
		CemuUwp::CompanionService::SetMetadataLanguage(
			metadataLanguage.empty() ? "en" : metadataLanguage);
	}
	CafeSaveList::Initialize();
	if (!mlcPath.empty())
	{
		CafeSaveList::SetMLCPath(mlcPath);
		CafeSaveList::Refresh();
	}
}

std::string OnlineAccountErrorMessage(OnlineAccountError error)
{
	switch (error)
	{
	case OnlineAccountError::kNoAccountId:
		return "the account ID is missing";
	case OnlineAccountError::kNoPasswordCached:
		return "password caching was disabled when the account was dumped";
	case OnlineAccountError::kPasswordCacheEmpty:
		return "the dumped password cache is empty";
	case OnlineAccountError::kNoPrincipalId:
		return "the principal ID is missing";
	default:
		return "the dumped online account is incomplete";
	}
}

bool RejectLaunchOverride(std::string message)
{
	cemuLog_log(LogType::Force, "UWP launch validation failed: {}", message);
	WindowSystem::ShowErrorDialog(message);
	return false;
}

bool ValidateLaunchOverrides()
{
	const auto serviceOverride = LaunchSettings::GetNetworkService();
	const auto accountOverride = LaunchSettings::GetPersistentId();
	const bool onlineLaunch = serviceOverride && *serviceOverride != NetworkService::Offline;
	if (!onlineLaunch && !accountOverride)
		return true;

	const uint32 persistentId = ActiveSettings::GetPersistentId();
	const auto& accounts = Account::GetAccounts();
	const auto accountIt = std::find_if(accounts.begin(), accounts.end(), [persistentId](const Account& account)
	{
		return account.GetPersistentId() == persistentId;
	});
	if (accountIt == accounts.end())
	{
		return RejectLaunchOverride(fmt::format(
			"The requested account {:08x} is not installed in this MLC.", persistentId));
	}

	if (!onlineLaunch)
		return true;

	const char* serviceName = *serviceOverride == NetworkService::Pretendo ? "Pretendo" : "Custom network service";
	if (*serviceOverride == NetworkService::Custom && !NetworkConfig::XMLExists())
	{
		return RejectLaunchOverride(
			"Custom network service launch requires network_services.xml in Cemu's configuration folder.");
	}

	OnlineValidator validator;
	try
	{
		validator = accountIt->ValidateOnlineFiles();
	}
	catch (const std::exception& ex)
	{
		return RejectLaunchOverride(fmt::format(
			"{} launch could not validate the selected account and online files: {}", serviceName, ex.what()));
	}
	std::vector<std::string> errors;
	if (!validator.valid_account)
		errors.emplace_back(fmt::format("Account {:08x}: {}.", persistentId, OnlineAccountErrorMessage(validator.account_error)));
	if (validator.otp == OnlineValidator::FileState::Missing)
		errors.emplace_back("otp.bin is missing from Cemu's local data folder.");
	else if (validator.otp == OnlineValidator::FileState::Corrupted)
		errors.emplace_back("otp.bin is invalid; it must be exactly 1024 bytes.");
	if (validator.seeprom == OnlineValidator::FileState::Missing)
		errors.emplace_back("seeprom.bin is missing from Cemu's local data folder.");
	else if (validator.seeprom == OnlineValidator::FileState::Corrupted)
		errors.emplace_back("seeprom.bin is invalid; it must be exactly 512 bytes.");
	if (!validator.missing_files.empty())
	{
		const size_t loggedFileCount = std::min<size_t>(validator.missing_files.size(), 10);
		for (size_t i = 0; i < loggedFileCount; ++i)
			cemuLog_log(LogType::Force, "Online launch missing certificate or key file: {}", _pathToUtf8(validator.missing_files[i]));
		if (validator.missing_files.size() > loggedFileCount)
			cemuLog_log(LogType::Force, "Online launch is missing {} additional certificate or key files",
				validator.missing_files.size() - loggedFileCount);
		errors.emplace_back(fmt::format(
			"{} Wii U certificate or key file{} missing from the selected MLC. See log.txt for the paths.",
			validator.missing_files.size(), validator.missing_files.size() == 1 ? " is" : "s are"));
	}

	if (errors.empty())
		return true;

	std::string message = fmt::format("{} launch cannot start:\n", serviceName);
	for (const auto& error : errors)
		message += fmt::format("\n- {}", error);
	return RejectLaunchOverride(std::move(message));
}

bool PrepareLaunchPath(const fs::path& launchPath)
{
	TraceHostStage(fmt::format("prepare path begin: {}", _pathToUtf8(launchPath)));
	TitleInfo launchTitle{ launchPath };
	if (launchTitle.IsValid())
	{
		TraceHostStage("prepare path: title info valid");
		CafeTitleList::AddTitleFromPath(launchPath);

		TitleId baseTitleId;
		if (!CafeTitleList::FindBaseTitleId(launchTitle.GetAppTitleId(), baseTitleId))
		{
			TraceHostStage(fmt::format("prepare path: base title not found for {:016x}", launchTitle.GetAppTitleId()));
			WindowSystem::ShowErrorDialog("Unable to launch game because the base files were not found.");
			return false;
		}

		const CafeSystem::PREPARE_STATUS_CODE status = CafeSystem::PrepareForegroundTitle(baseTitleId);
		if (status == CafeSystem::PREPARE_STATUS_CODE::UNABLE_TO_MOUNT)
		{
			TraceHostStage("prepare path: unable to mount title");
			WindowSystem::ShowErrorDialog(fmt::format("Unable to mount title: {}", _pathToUtf8(launchPath)));
			return false;
		}
		if (status != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
		{
			TraceHostStage(fmt::format("prepare path: prepare status {}", static_cast<int>(status)));
			WindowSystem::ShowErrorDialog(fmt::format("Failed to launch title: {}", _pathToUtf8(launchPath)));
			return false;
		}
		TraceHostStage("prepare path: prepared installed title");
		return true;
	}

	const CafeTitleFileType fileType = DetermineCafeSystemFileType(launchPath);
	TraceHostStage(fmt::format("prepare path: title info invalid, file type {}", static_cast<int>(fileType)));
	if (fileType == CafeTitleFileType::RPX || fileType == CafeTitleFileType::ELF)
	{
		const CafeSystem::PREPARE_STATUS_CODE status = CafeSystem::PrepareForegroundTitleFromStandaloneRPX(launchPath);
		if (status == CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
		{
			TraceHostStage("prepare path: prepared standalone executable");
			return true;
		}
		TraceHostStage(fmt::format("prepare path: standalone prepare status {}", static_cast<int>(status)));
	}

	TraceHostStage("prepare path: unable to launch");
	WindowSystem::ShowErrorDialog(fmt::format("Unable to launch game: {}", _pathToUtf8(launchPath)));
	return false;
}

bool StopRunningTitle()
{
	CemuHost::SetTitlePaused(false);
	const bool wasRunning = CafeSystem::IsTitleRunning();
	if (wasRunning)
		CafeSystem::ShutdownTitle();
	InputManager::instance().reset_uwp_expression_state();
	InputManager::instance().restore_uwp_default_profiles();
	CemuUwp::CompanionService::SetRunningTitle({});
	return wasRunning;
}

void ClearTitleLaunchOverrides()
{
	LaunchSettings::ResetGameLaunchOverrides();
	s_titleLaunchOverridesActive = false;
	s_titleLaunchObservedRunning = false;
}

void UpdateTitleLaunchOverrides(bool titleRunning)
{
	if (!s_titleLaunchOverridesActive)
		return;
	if (titleRunning)
	{
		s_titleLaunchObservedRunning = true;
		return;
	}
	if (s_titleLaunchObservedRunning)
		ClearTitleLaunchOverrides();
}

void RebuildFrontendRenderer()
{
	std::unique_lock<std::recursive_mutex> rendererLock(g_rendererMutex);
	TraceHostStage("frontend renderer rebuild begin");

	CemuUwp::Frontend::Shutdown();
	g_renderer.reset();
	try
	{
		g_renderer = std::make_unique<D3D12Renderer>();
		InvokeRenderer("D3D12 frontend renderer initialize", [&]() { g_renderer->Initialize(); });
		InvokeRenderer("D3D12 frontend surface initialize", [&]()
		{
			static_cast<D3D12Renderer*>(g_renderer.get())->InitializeSurface(CurrentSurfaceSize(), true);
		});
		CemuUwp::Frontend::Initialize();
	}
	catch (...)
	{
		g_renderer.reset();
		throw;
	}

	s_frontendActivityState.store(-1, std::memory_order_release);
	TraceHostStage("frontend renderer rebuild complete");
}
}

namespace CemuHost
{
void EnsureAudioInitialized()
{
	std::call_once(s_audioInitOnce, [] {
		TraceHostStage("audio static init begin");
		IAudioAPI::InitializeStatic();
		IAudioInputAPI::InitializeStatic();
		TraceHostStage("audio static init done");
	});
}

void EnsureGraphicPacksLoaded()
{
	std::call_once(s_graphicPacksLoadOnce, [] {
		TraceHostStage("graphic packs load begin");
		GraphicPack2::LoadAll();
		TraceHostStage("graphic packs load done");
	});
}

bool Initialize(const winrt::Windows::UI::Core::CoreWindow& window)
{
	if (s_initialized)
		return true;

	try
	{
		TraceHostStage("initialize begin");
		CemuUwp::WindowHost::SetCoreWindow(window);
		TraceHostStage("core window set");
		CemuUwp::StorageService::Initialize();
		TraceHostStage("storage initialized");

		std::set<fs::path> failedWriteAccess;
		ActiveSettings::SetPaths(false,
			CemuUwp::StorageService::InstalledContentRoot() / "CemuUwp.exe",
			CemuUwp::StorageService::LocalRoot(),
			CemuUwp::StorageService::ConfigRoot(),
			CemuUwp::StorageService::CacheRoot(),
			CemuUwp::StorageService::InstalledContentRoot(),
			failedWriteAccess);
		GetConfigHandle().SetFilename(ActiveSettings::GetConfigPath("settings.xml").generic_wstring());
		n_config.SetFilename(ActiveSettings::GetConfigPath("network_services.xml").generic_wstring());
		TraceHostStage("paths configured");

		if (!failedWriteAccess.empty())
			CemuUwp::Frontend::PushNotification("Some UWP local storage paths are not writable.");

		cemuLog_createLogFile(false);
		cemuLog_log(LogType::Force, "UWP log initialized");
		CemuUwp::MemoryBudgetService::Initialize();

		TraceHostStage("core services begin");
		InitializeCoreServices();
		TraceHostStage("core services initialized");

		TraceHostStage("d3d12 global init begin");
		if (!InitializeGlobalD3D12())
			throw std::runtime_error("Direct3D 12 is unavailable in the UWP app container.");
		TraceHostStage("d3d12 global init done");

		TraceHostStage("renderer create begin");
		g_renderer = std::make_unique<D3D12Renderer>();
		g_renderer->Initialize();
		TraceHostStage("renderer initialized");
		static_cast<D3D12Renderer*>(g_renderer.get())->InitializeSurface(CurrentSurfaceSize(), true);
		TraceHostStage("surface initialized");

		CemuUwp::Frontend::Initialize();
		TraceHostStage("frontend initialized");
		s_initialized = true;
		s_frontendActivityState.store(-1, std::memory_order_release);
		SyncFrontendActivity();
		TraceHostStage("initialize done");
		return true;
	}
	catch (const std::exception& ex)
	{
		CemuUwp::MemoryBudgetService::Shutdown();
		TraceHostStage(fmt::format("initialize exception: {}", ex.what()));
		CemuUwp::Frontend::PushNotification(ex.what());
		cemuLog_log(LogType::Force, "UWP host initialization failed: {}", ex.what());
		return false;
	}
}

bool LaunchPath(const fs::path& launchPath)
{
	if (!s_initialized)
		return false;
	TraceHostStage(fmt::format("launch begin: {}", _pathToUtf8(launchPath)));
	if (!ValidateLaunchOverrides())
	{
		cemuLog_log(LogType::Force, "UWP launch rejected by account or network-service validation");
		TraceHostStage("launch validation failed");
		return false;
	}
	if (CafeSystem::IsTitleRunning())
		StopRunningTitle();
	if (!g_renderer)
		RebuildFrontendRenderer();
	EnsureAudioInitialized();
	EnsureGraphicPacksLoaded();
	if (!PrepareLaunchPath(launchPath))
	{
		TraceHostStage("launch prepare failed");
		return false;
	}

	s_lastLaunchPath = launchPath;
	CemuUwp::Frontend::SetStatus("Launching title");
	// free shell artwork before the title competes for the Xbox app memory budget
	CemuUwp::Frontend::SetActive(false);
	s_frontendActivityState.store(0, std::memory_order_release);
	CafeSystem::LaunchForegroundTitle();
	s_titleLaunchOverridesActive = LaunchSettings::GetPersistentId().has_value() || LaunchSettings::GetNetworkService().has_value();
	s_titleLaunchObservedRunning = s_titleLaunchOverridesActive && CafeSystem::IsTitleRunning();
	CemuUwp::CompanionService::SetRunningTitle(CafeSystem::GetForegroundTitleName());
	SyncFrontendActivity();
	TraceHostStage("launch submitted");
	return true;
}

bool RestartTitle()
{
	if (!s_initialized || !s_lastLaunchPath)
		return false;
	const fs::path launchPath = *s_lastLaunchPath;
	SetTitlePaused(false);
	return LaunchPath(launchPath);
}

void SetTitlePaused(bool paused)
{
#if defined(CEMU_UWP)
	const bool desiredPaused = paused && CafeSystem::IsTitleRunning();
	if (s_titlePaused == desiredPaused && coreinit::OSSchedulerIsPaused() == desiredPaused)
		return;
	s_titlePaused = desiredPaused;
	TraceHostStage(desiredPaused ? "scheduler pause on" : "scheduler pause off");
	coreinit::OSSchedulerSetPaused(desiredPaused);
#else
	(void)paused;
#endif
}

void SetWindowVisible(bool visible)
{
	s_windowVisible.store(visible, std::memory_order_release);
	SyncFrontendActivity();
}

void SetWindowActive(bool active)
{
	s_windowActive.store(active, std::memory_order_release);
	SyncFrontendActivity();
}

bool IsGraphicsDeviceLost()
{
	std::unique_lock<std::recursive_mutex> rendererLock(g_rendererMutex);
	const auto* renderer = static_cast<D3D12Renderer*>(g_renderer.get());
	return renderer && renderer->GetDevice() && FAILED(renderer->GetDevice()->GetDeviceRemovedReason());
}

bool HandleGraphicsFailure(std::string_view message)
{
	bool expected = false;
	if (!s_initialized || !s_recoveringGraphics.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		return false;

	const std::string failure(message);
	cemuLog_log(LogType::Force, "UWP graphics frame failed: {}", failure);
	TraceHostStage(fmt::format("graphics recovery begin: {}", failure));
	try
	{
		StopRunningTitle();
		RebuildFrontendRenderer();

		s_recoveringGraphics.store(false, std::memory_order_release);
		s_frontendActivityState.store(-1, std::memory_order_release);
		SyncFrontendActivity();
		CemuUwp::Frontend::PushNotification(
			fmt::format("Graphics was restarted and the running title was closed: {}", failure));
		TraceHostStage("graphics recovery completed");
		return true;
	}
	catch (const winrt::hresult_error& ex)
	{
		cemuLog_log(LogType::Force, "UWP graphics recovery failed with WinRT error 0x{:08x}: {}",
			(uint32)ex.code(), winrt::to_string(ex.message()));
		TraceHostStage(fmt::format("graphics recovery WinRT exception 0x{:08X}: {}",
			(uint32)ex.code(), winrt::to_string(ex.message())));
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "UWP graphics recovery failed: {}", ex.what());
		TraceHostStage(fmt::format("graphics recovery exception: {}", ex.what()));
	}
	catch (...)
	{
		cemuLog_log(LogType::Force, "UWP graphics recovery failed with an unknown exception");
		TraceHostStage("graphics recovery unknown exception");
	}

	s_recoveringGraphics.store(false, std::memory_order_release);
	return false;
}

void Trace(std::string_view message)
{
	TraceHostStage(message);
}

void ShutdownTitle()
{
	StopRunningTitle();
	ClearTitleLaunchOverrides();
	if (!g_renderer)
		RebuildFrontendRenderer();
	SyncFrontendActivity();
}

void Shutdown()
{
	if (!s_initialized)
		return;

	StopRunningTitle();
	ClearTitleLaunchOverrides();
	CemuUwp::CompanionService::Shutdown();
	CafeSystem::Shutdown();
	InputManager::instance().Shutdown();
	CemuUwp::Frontend::Shutdown();
	g_renderer.reset();
	CemuUwp::MemoryBudgetService::Shutdown();
	cemuLog_waitForFlush();
	s_frontendActivityState.store(-1, std::memory_order_release);
	s_initialized = false;
}

bool TickFrontend()
{
	if (!s_initialized)
		return false;
	CemuUwp::MemoryBudgetService::Tick();
	if (auto streamMessage = CemuUwp::CompanionService::TakeStreamMessage())
		CemuUwp::Frontend::PushNotification(*streamMessage);
	std::string launchError;
	if (CafeSystem::ConsumeLaunchError(launchError))
	{
		TraceHostStage(fmt::format("title launch failed: {}", launchError));
		StopRunningTitle();
		ClearTitleLaunchOverrides();
		CemuUwp::Frontend::PushNotification(launchError);
	}
	if (!g_renderer && !CafeSystem::IsTitleRunning())
	{
		RebuildFrontendRenderer();
		SyncFrontendActivity();
	}
	if (!g_renderer)
		return false;

	if (auto pendingLaunch = CemuUwp::CompanionService::TakePendingLaunchPath())
	{
		cemuLog_logDebug(LogType::Force, "Companion app: launching catalogue selection {}", _pathToUtf8(*pendingLaunch));
		const bool launched = LaunchPath(*pendingLaunch);
		CemuUwp::CompanionService::CompletePendingLaunch(launched);
		if (!launched)
			CemuUwp::Frontend::PushNotification("Companion app launch failed");
	}

	// titles can also stop on their own, outside the quick menu
	CemuUwp::CompanionService::SetRunningTitle(
		CafeSystem::IsTitleRunning() ? CafeSystem::GetForegroundTitleName() : std::string{});

	AssignNewGamepads();
	if (CafeSystem::IsTitleRunning())
		CemuUwp::Frontend::TickGameplayInput();

	if (!s_initialized || !g_renderer)
		return false;

	SetTitlePaused(CemuUwp::Frontend::IsGameplayInputCaptured());

	const bool titleRunning = CafeSystem::IsTitleRunning();
	UpdateTitleLaunchOverrides(titleRunning);
	if (!titleRunning)
		InputManager::instance().restore_uwp_default_profiles();
	SyncFrontendActivity();
	const bool wantsOverlay = titleRunning && CemuUwp::Frontend::WantsGameplayOverlay();
	const bool resizePending = CemuUwp::WindowHost::ConsumeResize();

	if (titleRunning && !wantsOverlay && !resizePending)
		return false;

#if defined(CEMU_UWP)
	std::unique_lock<std::recursive_mutex> rendererLock(g_rendererMutex);
#endif

	if (resizePending)
	{
		InvokeRenderer("D3D12 surface resize", [&]()
		{
			static_cast<D3D12Renderer*>(g_renderer.get())->ResizeSurface(CurrentSurfaceSize(), true);
		});
	}

	if (titleRunning)
	{
		if (wantsOverlay)
		{
			InvokeRenderer("D3D12 gameplay overlay clear", [&]() { g_renderer->DrawEmptyFrame(true); });
			if (InvokeRenderer("D3D12 gameplay overlay begin", [&]() { return g_renderer->ImguiBegin(true); }))
			{
				try
				{
					CemuUwp::Frontend::DrawGameplayOverlay();
				}
				catch (...)
				{
					const auto frontendFailure = std::current_exception();
					InvokeRenderer("D3D12 gameplay overlay end", [&]() { g_renderer->ImguiEnd(); });
					std::rethrow_exception(frontendFailure);
				}
				InvokeRenderer("D3D12 gameplay overlay end", [&]() { g_renderer->ImguiEnd(); });
			}
			InvokeRenderer("D3D12 gameplay overlay present", [&]() { g_renderer->SwapBuffers(true, false); });
			return true;
		}
		return false;
	}

	CemuUwp::Frontend::BeginFrame();
	InvokeRenderer("D3D12 frontend clear", [&]() { g_renderer->DrawEmptyFrame(true); });
	if (InvokeRenderer("D3D12 frontend begin", [&]() { return g_renderer->ImguiBegin(true); }))
	{
		try
		{
			CemuUwp::Frontend::Draw();
		}
		catch (...)
		{
			const auto frontendFailure = std::current_exception();
			InvokeRenderer("D3D12 frontend end", [&]() { g_renderer->ImguiEnd(); });
			std::rethrow_exception(frontendFailure);
		}
		InvokeRenderer("D3D12 frontend end", [&]() { g_renderer->ImguiEnd(); });
	}
	InvokeRenderer("D3D12 frontend present", [&]() { g_renderer->SwapBuffers(true, false); });
	return true;
}
}
