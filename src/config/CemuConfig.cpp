#include "config/CemuConfig.h"
#include "util/helpers/helpers.h"
#include "config/ActiveSettings.h"
#include "Cafe/Account/Account.h"
#if defined(CEMU_UWP)
#include "uwp/UwpQuickMenuBinding.h"
#endif

void CemuConfig::SetMLCPath(fs::path path, bool save)
{
	mlc_path.SetValue(_pathToUtf8(path));
	if(save)
		GetConfigHandle().Save();
	Account::RefreshAccounts();
}

XMLConfigParser CemuConfig::Load(XMLConfigParser& parser)
{
	auto new_parser = parser.get("content");
	if (new_parser.valid())
		parser = new_parser;

	// general settings
	log_flag = parser.get("logflag", log_flag.GetInitValue());
	cemuLog_setActiveLoggingFlags(GetConfig().log_flag.GetValue());
	advanced_ppc_logging = parser.get("advanced_ppc_logging", advanced_ppc_logging.GetInitValue());

	const char* mlc = parser.get("mlc_path", "");
	mlc_path = mlc;
	graphic_packs_path = parser.get("graphic_packs_path", "");

	permanent_storage = parser.get("permanent_storage", permanent_storage);

	proxy_server = parser.get("proxy_server", "");
	disable_screensaver = parser.get("disable_screensaver", disable_screensaver);
	play_boot_sound = parser.get("play_boot_sound", play_boot_sound);
	console_language = parser.get("console_language", console_language.GetInitValue());

	game_paths.clear();
	auto game_path_parser = parser.get("GamePaths");
	for (auto element = game_path_parser.get("Entry"); element.valid(); element = game_path_parser.get("Entry", element))
	{
		const std::string path = element.value("");
		if (path.empty())
			continue;
		try
		{
			game_paths.emplace_back(path);
		}
		catch (const std::exception&)
		{
			cemuLog_log(LogType::Force, "config load error: can't load game path: {}", path);
		}
	}

	std::unique_lock _lock(game_cache_entries_mutex);
	game_cache_entries.clear();
	auto game_cache_parser = parser.get("GameCache");
	for (auto element = game_cache_parser.get("Entry"); element.valid(); element = game_cache_parser.get("Entry", element))
	{
		const char* rpx = element.get("path", "");
		try
		{
			GameEntry entry{};
			entry.rpx_file = boost::nowide::widen(rpx);
			entry.title_id = element.get<decltype(entry.title_id)>("title_id");
			entry.legacy_name = boost::nowide::widen(element.get("name", ""));
			entry.custom_name = element.get("custom_name", "");
			entry.legacy_region = element.get("region", 0);
			entry.legacy_version = element.get("version", 0);
			entry.legacy_update_version = element.get("version", 0);
			entry.legacy_dlc_version = element.get("dlc_version", 0);
			entry.legacy_time_played = element.get<decltype(entry.legacy_time_played)>("time_played");
			entry.legacy_last_played = element.get<decltype(entry.legacy_last_played)>("last_played");
			entry.favorite = element.get("favorite", false);
			game_cache_entries.emplace_back(entry);

			if (entry.title_id != 0)
			{
				if (entry.favorite)
					game_cache_favorites.emplace(entry.title_id);
				else
					game_cache_favorites.erase(entry.title_id);
			}
		}
		catch (const std::exception&)
		{
			cemuLog_log(LogType::Force, "config load error: can't load game cache entry: {}", rpx);
		}
	}
	_lock.unlock();

	graphic_pack_entries.clear();
	auto graphic_pack_parser = parser.get("GraphicPack");
	for (auto element = graphic_pack_parser.get("Entry"); element.valid(); element = graphic_pack_parser.get("Entry", element))
	{
		std::string filename = element.get_attribute("filename", "");
		if(filename.empty()) // legacy loading
		{
			filename = element.get("filename", "");
			fs::path path = fs::path(filename).lexically_normal();
			graphic_pack_entries.try_emplace(path);
			const std::string category = element.get("category", "");
			const std::string preset = element.get("preset", "");
			graphic_pack_entries[filename].try_emplace(category, preset);
		}
		else
		{
			fs::path path = fs::path(filename).lexically_normal();
			graphic_pack_entries.try_emplace(path);

			const bool disabled = element.get_attribute("disabled", false);
			if (disabled)
			{
				graphic_pack_entries[path].try_emplace("_disabled", "true");
			}

			for (auto preset = element.get("Preset"); preset.valid(); preset = element.get("Preset", preset))
			{
				const std::string category = preset.get("category", "");
				const std::string active_preset = preset.get("preset", "");
				graphic_pack_entries[path].try_emplace(category, active_preset);
			}
		}

	}

	// graphics
	auto graphic = parser.get("Graphic");
	graphic_api = graphic.get("api", kDefaultGraphicsAPI);
	graphic.get("device", legacy_graphic_device_uuid);
	if (graphic.get("vkDevice").valid())
		graphic.get("vkDevice", vk_graphic_device_uuid);
	else
		vk_graphic_device_uuid = legacy_graphic_device_uuid;
	mtl_graphic_device_uuid = graphic.get("mtlDevice", 0);
	graphic.get("d3d12Device", d3d12_graphic_device_luid);
	vsync = graphic.get("VSync", 0);
	overrideAppGammaPreference = graphic.get("OverrideAppGammaPreference", false);
	overrideGammaValue = graphic.get("OverrideGammaValue", 2.2f);
	if(overrideGammaValue < 0)
		overrideGammaValue = 2.2f;
	userDisplayGamma = graphic.get("UserDisplayGamma", 2.2f);
	if(userDisplayGamma < 0)
		userDisplayGamma = 2.2f;
	gx2drawdone_sync = graphic.get("GX2DrawdoneSync", true);
	upscale_filter = graphic.get("UpscaleFilter", kBicubicHermiteFilter);
	fsr_sharpness = graphic.get("FsrSharpness", fsr_sharpness.GetInitValue());
	downscale_filter = graphic.get("DownscaleFilter", kLinearFilter);
	fullscreen_scaling = graphic.get("FullscreenScaling", kKeepAspectRatio);
	async_compile = graphic.get("AsyncCompile", async_compile);
	d3d12_threaded_recording = graphic.get("D3D12ThreadedRecording", d3d12_threaded_recording);
	d3d12_prioritized_pipeline_cache = graphic.get("D3D12PrioritizedPipelineCache", d3d12_prioritized_pipeline_cache);
	// D3D12GpuArrayUpload is not read: on Xbox the texture-to-texture copy misplaces array slices like the buffer copy does
	skip_resolution_rules_of_packs_with_skipped_shaders = graphic.get("SkipResolutionRulesOfPacksWithSkippedShaders", false);
	vk_accurate_barriers = graphic.get("vkAccurateBarriers", true); // this used to be "VulkanAccurateBarriers" but because we changed the default to true in 1.27.1 the option name had to be changed
#ifdef ENABLE_METAL
	force_mesh_shaders = graphic.get("ForceMeshShaders", false);
#endif

	auto overlay_node = graphic.get("Overlay");
	if(overlay_node.valid())
	{
		overlay.position = overlay_node.get("Position", ScreenPosition::kDisabled);
		overlay.text_color = overlay_node.get("TextColor", 0xFFFFFFFF);
		overlay.text_scale = overlay_node.get("TextScale", 100);
		overlay.fps = overlay_node.get("FPS", true);
		overlay.drawcalls = overlay_node.get("DrawCalls", false);
		overlay.cpu_usage = overlay_node.get("CPUUsage", false);
		overlay.cpu_per_core_usage = overlay_node.get("CPUPerCoreUsage", false);
		overlay.ram_usage = overlay_node.get("RAMUsage", false);
		overlay.vram_usage = overlay_node.get("VRAMUsage", false);
		overlay.debug = overlay_node.get("Debug", false);

		notification.controller_profiles = overlay_node.get("ControllerProfiles", true);
		notification.controller_battery = overlay_node.get("ControllerBattery", true);
		notification.shader_compiling = overlay_node.get("ShaderCompiling", true);
	}
	else
	{
		// legacy support
		overlay.position = graphic.get("OverlayPosition", ScreenPosition::kDisabled);
		overlay.text_color = graphic.get("OverlayTextColor", 0xFFFFFFFF);
		overlay.fps = graphic.get("OverlayFPS", true);
		overlay.drawcalls = graphic.get("OverlayDrawCalls", false);
		overlay.cpu_usage = graphic.get("OverlayCPUUsage", false);
		overlay.cpu_per_core_usage = graphic.get("OverlayCPUPerCoreUsage", false);
		overlay.ram_usage = graphic.get("OverlayRAMUsage", false);

		notification.controller_profiles = graphic.get("OverlayControllerProfiles", true);
		notification.controller_battery = graphic.get("OverlayControllerBattery", true);
		notification.shader_compiling = graphic.get("ShaderCompiling", true);
	}

	auto notification_node = graphic.get("Notification");
	if (notification_node.valid())
	{
		notification.position = notification_node.get("Position", ScreenPosition::kTopLeft);
		notification.text_color = notification_node.get("TextColor", 0xFFFFFFFF);
		notification.text_scale = notification_node.get("TextScale", 100);
		notification.controller_profiles = notification_node.get("ControllerProfiles", true);
		notification.controller_battery = notification_node.get("ControllerBattery", false);
		notification.shader_compiling = notification_node.get("ShaderCompiling", true);
		notification.friends = notification_node.get("FriendService", true);
	}

	// audio
	auto audio = parser.get("Audio");
	audio_api = audio.get("api", 0);
	audio_delay = audio.get("delay", 2);
	tv_channels = audio.get("TVChannels", kStereo);
	pad_channels = audio.get("PadChannels", kStereo);
	input_channels = audio.get("InputChannels", kMono);
	tv_volume = audio.get("TVVolume", 20);
	pad_volume = audio.get("PadVolume", 0);
	input_volume = audio.get("InputVolume", 20);
	portal_volume = audio.get("PortalVolume", 20);

	const auto tv = audio.get("TVDevice", "");
	try
	{
		tv_device = boost::nowide::widen(tv);
	}
	catch (const std::exception&)
	{
		cemuLog_log(LogType::Force, "config load error: can't load tv device: {}", tv);
	}

	const auto pad = audio.get("PadDevice", "");
	try
	{
		pad_device = boost::nowide::widen(pad);
	}
	catch (const std::exception&)
	{
		cemuLog_log(LogType::Force, "config load error: can't load pad device: {}", pad);
	}

	const auto input_device_name = audio.get("InputDevice", "");
	try
	{
		input_device = boost::nowide::widen(input_device_name);
	}
	catch (const std::exception&)
	{
		cemuLog_log(LogType::Force, "config load error: can't load input device: {}", input_device_name);
	}

	const auto portal_device_name = audio.get("PortalDevice", "");
	try
	{
		portal_device = boost::nowide::widen(portal_device_name);
	}
	catch (const std::exception&)
	{
		cemuLog_log(LogType::Force, "config load error: can't load input device: {}", portal_device_name);
	}

	// account
	auto acc = parser.get("Account");
	account.m_persistent_id = acc.get("PersistentId", account.m_persistent_id);
	// legacy online settings, we only parse these for upgrading purposes
	account.legacy_online_enabled = acc.get("OnlineEnabled", account.legacy_online_enabled);
	account.legacy_active_service = acc.get("ActiveService",account.legacy_active_service);
	// per-account online setting
	auto accService = parser.get("AccountService");
	account.service_select.clear();
	for (auto element = accService.get("SelectedService"); element.valid(); element = accService.get("SelectedService", element))
	{
		uint32 persistentId = element.get_attribute<uint32>("PersistentId", 0);
		sint32 serviceIndex = element.get_attribute<sint32>("Service", 0);
		NetworkService networkService = static_cast<NetworkService>(serviceIndex);
		if (persistentId < Account::kMinPersistendId)
			continue;
		if(networkService == NetworkService::Offline || networkService == NetworkService::Nintendo || networkService == NetworkService::Pretendo || networkService == NetworkService::Custom)
			account.service_select.emplace(persistentId, networkService);
	}
	// debug
	auto debug = parser.get("Debug");
#if BOOST_OS_WINDOWS
	crash_dump = debug.get("CrashDumpWindows", crash_dump);
#elif BOOST_OS_UNIX
	crash_dump = debug.get("CrashDumpUnix", crash_dump);
#endif
	gdb_port = debug.get("GDBPort", 1337);
#ifdef ENABLE_METAL
	gpu_capture_dir = debug.get("GPUCaptureDir", "");
	framebuffer_fetch = debug.get("FramebufferFetch", true);
#endif

	// input
	auto input = parser.get("Input");
	auto dsuc = input.get("DSUC");
	dsu_client.host = dsuc.get_attribute("host", dsu_client.host);
	dsu_client.port = dsuc.get_attribute("port", dsu_client.port);
#if defined(CEMU_UWP)
	dsu_client.enabled = dsuc.get_attribute("enabled", dsu_client.enabled);
	dsu_client.streaming_enabled = dsuc.get_attribute("streamingEnabled", dsu_client.streaming_enabled);
	dsu_client.streaming_url = dsuc.get_attribute("streamingUrl", dsu_client.streaming_url);
	dsu_client.servers.clear();
	auto dsuServers = dsuc.get("Servers");
	for (auto element = dsuServers.get("Server"); element.valid();
		element = dsuServers.get("Server", element))
	{
		UwpDsuServerEntry server;
		server.id = element.get_attribute<std::string>("id", {});
		server.name = element.get_attribute<std::string>("name", {});
		server.host = element.get_attribute<std::string>("host", "127.0.0.1");
		server.port = element.get_attribute<uint16>("port", 26760);
		server.enabled = element.get_attribute<bool>("enabled", false);
		server.companion_transport = false;
		if (server.id.empty())
			server.id = fmt::format("dsu-{}", dsu_client.servers.size() + 1);
		if (server.name.empty())
			server.name = fmt::format("DSU Server {}", dsu_client.servers.size() + 1);
		dsu_client.servers.emplace_back(std::move(server));
	}
	if (dsu_client.servers.empty())
	{
		dsu_client.servers.emplace_back(UwpDsuServerEntry{
			.id = "default",
			.name = "DSU Server",
			.host = dsu_client.host.GetValue(),
			.port = dsu_client.port.GetValue(),
			.enabled = dsu_client.enabled.GetValue(),
			.companion_transport = true,
		});
	}
	auto primary = std::ranges::find_if(dsu_client.servers,
		[](const UwpDsuServerEntry& server) { return server.enabled; });
	if (primary == dsu_client.servers.end())
		primary = dsu_client.servers.begin();
	for (auto& server : dsu_client.servers)
		server.companion_transport = &server == &*primary;
	if (primary != dsu_client.servers.end())
	{
		dsu_client.host = primary->host;
		dsu_client.port = primary->port;
		dsu_client.enabled = primary->enabled;
	}
	dsu_client.player_routes.clear();
	auto dsuRoutes = dsuc.get("Routes");
	for (auto element = dsuRoutes.get("Route"); element.valid();
		element = dsuRoutes.get("Route", element))
	{
		UwpDsuPlayerRouteEntry route;
		route.player = element.get_attribute<uint8>("player", 0);
		route.server_id = element.get_attribute<std::string>("server", {});
		route.remote_slot = element.get_attribute<uint8>("slot", 0);
		route.merge = element.get_attribute<bool>("merge", false);
		route.gamepad = element.get_attribute<bool>("gamepad", true);
		route.motion = element.get_attribute<bool>("motion", true);
		route.touch = element.get_attribute<bool>("touch", true);
		if (route.player < 8 && route.remote_slot < 16 && !route.server_id.empty())
			dsu_client.player_routes.emplace_back(std::move(route));
	}
	#endif

#if defined(CEMU_UWP)
	auto uwpDisplay = parser.get("UWP").get("Display");
	uwp_display.ui_language = uwpDisplay.get("Language", uwp_display.ui_language);
	uwp_display.default_layout = uwpDisplay.get("DefaultLayout", uwp_display.default_layout);
	uwp_display.swap_screens = uwpDisplay.get("SwapScreens", uwp_display.swap_screens);
	uwp_display.touch_cursor = uwpDisplay.get("TouchCursor", uwp_display.touch_cursor);
	uwp_display.toggle_swap_binding = uwpDisplay.get("ToggleSwapBinding", uwp_display.toggle_swap_binding);
	uwp_display.virtual_gyro_binding = uwpDisplay.get("VirtualGyroBinding", uwp_display.virtual_gyro_binding);
	uwp_display.virtual_gyro_combo_enabled = uwpDisplay.get("VirtualGyroComboEnabled", !uwp_display.virtual_gyro_binding.GetValue().empty());
	uwp_display.quick_menu_binding = uwpDisplay.get("QuickMenuBinding", uwp_display.quick_menu_binding);
	uwp_display.quick_menu_binding = UwpQuickMenuBinding::Normalize(uwp_display.quick_menu_binding.GetValue());
	uwp_display.game_layouts.clear();
	auto uwpGameLayouts = uwpDisplay.get("GameLayouts");
	for (auto element = uwpGameLayouts.get("Entry"); element.valid(); element = uwpGameLayouts.get("Entry", element))
	{
		UwpGameLayoutEntry entry{};
		entry.title_id = element.get_attribute<uint64>("TitleId", 0);
		if (entry.title_id == 0)
			continue;
		entry.layout = element.get("Layout", uwp_display.default_layout.GetValue());
		uwp_display.game_layouts[entry.title_id] = std::move(entry);
	}

	auto uwpScreenScraper = parser.get("UWP").get("ScreenScraper");
	uwp_screenscraper.enabled = uwpScreenScraper.get("Enabled", uwp_screenscraper.enabled);
	uwp_screenscraper.download_metadata = uwpScreenScraper.get("DownloadMetadata", uwp_screenscraper.download_metadata);
	uwp_screenscraper.download_box_art = uwpScreenScraper.get("DownloadBoxArt", uwp_screenscraper.download_box_art);
	uwp_screenscraper.download_fanart = uwpScreenScraper.get("DownloadFanart", uwp_screenscraper.download_fanart);
	uwp_screenscraper.download_videos = uwpScreenScraper.get("DownloadVideos", uwp_screenscraper.download_videos);
	uwp_screenscraper.download_screenshots = uwpScreenScraper.get("DownloadScreenshots", uwp_screenscraper.download_screenshots);
	uwp_screenscraper.download_logos = uwpScreenScraper.get("DownloadLogos", uwp_screenscraper.download_logos);
	uwp_screenscraper.overwrite_existing = uwpScreenScraper.get("OverwriteExisting", uwp_screenscraper.overwrite_existing);
	const sint32 legacyArtwork = uwpScreenScraper.get("BoxArtSource", -1);
	const auto migrateLegacyArtwork = [](sint32 value) {
		switch (value)
		{
		case 0: return (sint32)UwpScreenScraperArtwork::Box2D;
		case 1: return (sint32)UwpScreenScraperArtwork::Box3D;
		case 2: return (sint32)UwpScreenScraperArtwork::MixRecalboxV2;
		case 3: return (sint32)UwpScreenScraperArtwork::Screenshot;
		case 5: return (sint32)UwpScreenScraperArtwork::FanArt;
		default: return (sint32)UwpScreenScraperArtwork::Box3D;
		}
	};
	const sint32 migratedArtwork = migrateLegacyArtwork(legacyArtwork);
	uwp_screenscraper.list_artwork_source = std::clamp(
		uwpScreenScraper.get("ListArtworkSource", migratedArtwork),
		(sint32)UwpScreenScraperArtwork::Box2D,
		(sint32)UwpScreenScraperArtwork::TitleScreen);
	uwp_screenscraper.detail_artwork_source = std::clamp(
		uwpScreenScraper.get("DetailArtworkSource", migratedArtwork),
		(sint32)UwpScreenScraperArtwork::Box2D,
		(sint32)UwpScreenScraperArtwork::TitleScreen);
	uwp_screenscraper.preferred_region = uwpScreenScraper.get("PreferredRegion", uwp_screenscraper.preferred_region);
	uwp_screenscraper.metadata_language = uwpScreenScraper.get("MetadataLanguage", uwp_screenscraper.metadata_language);
	uwp_screenscraper.media_directory = uwpScreenScraper.get(
		"MediaDirectory", uwp_screenscraper.media_directory);

#endif

	// emulatedusbdevices
	auto usbdevices = parser.get("EmulatedUsbDevices");
	emulated_usb_devices.emulate_skylander_portal = usbdevices.get("EmulateSkylanderPortal", emulated_usb_devices.emulate_skylander_portal);
	emulated_usb_devices.emulate_infinity_base = usbdevices.get("EmulateInfinityBase", emulated_usb_devices.emulate_infinity_base);
	emulated_usb_devices.emulate_dimensions_toypad = usbdevices.get("EmulateDimensionsToypad", emulated_usb_devices.emulate_dimensions_toypad);

	return parser;
}

XMLConfigParser CemuConfig::Save(XMLConfigParser& parser)
{
	auto config = parser.set("content");
	// general settings
	config.set("logflag", log_flag.GetValue());
	config.set("advanced_ppc_logging", advanced_ppc_logging.GetValue());
	config.set("mlc_path", mlc_path.GetValue().c_str());
	config.set("graphic_packs_path", graphic_packs_path.GetValue().c_str());
	config.set<bool>("permanent_storage", permanent_storage);
	config.set("proxy_server", proxy_server.GetValue().c_str());
	config.set<bool>("disable_screensaver", disable_screensaver);
	config.set<bool>("play_boot_sound", play_boot_sound);

	// config.set("cpu_mode", cpu_mode.GetValue());
	//config.set("console_region", console_region.GetValue());
	config.set("console_language", console_language.GetValue());

	// game paths
	auto game_path_parser = config.set("GamePaths");
	for (const auto& entry : game_paths)
	{
		game_path_parser.set("Entry", entry.c_str());
	}

	// game list cache
	std::unique_lock _lock(game_cache_entries_mutex);
	auto game_cache_parser = config.set("GameCache");
	for (const auto& game : game_cache_entries)
	{
		auto entry = game_cache_parser.set("Entry");

		entry.set("title_id", (sint64)game.title_id);
		entry.set("name", boost::nowide::narrow(game.legacy_name).c_str());
		entry.set("custom_name", game.custom_name.c_str());
		entry.set("region", (sint32)game.legacy_region);
		entry.set("version", (sint32)game.legacy_update_version);
		entry.set("dlc_version", (sint32)game.legacy_dlc_version);
		entry.set("path", boost::nowide::narrow(game.rpx_file).c_str());
		entry.set("time_played", game.legacy_time_played);
		entry.set("last_played", game.legacy_last_played);
		entry.set("favorite", game.favorite);
	}
	_lock.unlock();

	auto graphic_pack_parser = config.set("GraphicPack");
	for (const auto& game : graphic_pack_entries)
	{
		auto entry = graphic_pack_parser.set("Entry");
		entry.set_attribute("filename",_pathToUtf8(game.first).c_str());
		for(const auto& kv : game.second)
		{
			// TODO: less hacky pls
			if(boost::iequals(kv.first, "_disabled"))
			{
				entry.set_attribute("disabled", true);
				continue;
			}

			auto preset = entry.set("Preset");
			if(!kv.first.empty())
				preset.set("category", kv.first.c_str());

			preset.set("preset", kv.second.c_str());
		}
	}

	// graphics
	auto graphic = config.set("Graphic");
	graphic.set("api", graphic_api);
	graphic.set("device", legacy_graphic_device_uuid);
	graphic.set("vkDevice", vk_graphic_device_uuid);
	graphic.set("mtlDevice", mtl_graphic_device_uuid);
	graphic.set("d3d12Device", d3d12_graphic_device_luid);
	graphic.set("VSync", vsync);
	graphic.set("OverrideAppGammaPreference", overrideAppGammaPreference);
	graphic.set("OverrideGammaValue", overrideGammaValue);
	graphic.set("UserDisplayGamma", userDisplayGamma);
	graphic.set("GX2DrawdoneSync", gx2drawdone_sync);
#ifdef ENABLE_METAL
	graphic.set("ForceMeshShaders", force_mesh_shaders);
#endif
	//graphic.set("PrecompiledShaders", precompiled_shaders.GetValue());
	graphic.set("UpscaleFilter", upscale_filter);
	graphic.set("FsrSharpness", fsr_sharpness.GetValue());
	graphic.set("DownscaleFilter", downscale_filter);
	graphic.set("FullscreenScaling", fullscreen_scaling);
	graphic.set("AsyncCompile", async_compile.GetValue());
	graphic.set("D3D12ThreadedRecording", d3d12_threaded_recording.GetValue());
	graphic.set("D3D12PrioritizedPipelineCache", d3d12_prioritized_pipeline_cache.GetValue());
	graphic.set("D3D12GpuArrayUpload", d3d12_gpu_array_upload.GetValue());
	graphic.set("SkipResolutionRulesOfPacksWithSkippedShaders", skip_resolution_rules_of_packs_with_skipped_shaders.GetValue());
	graphic.set("vkAccurateBarriers", vk_accurate_barriers);

	auto overlay_node = graphic.set("Overlay");
	overlay_node.set("Position", overlay.position);
	overlay_node.set("TextColor", overlay.text_color);
	overlay_node.set("TextScale", overlay.text_scale);
	overlay_node.set("FPS", overlay.fps);
	overlay_node.set("DrawCalls", overlay.drawcalls);
	overlay_node.set("CPUUsage", overlay.cpu_usage);
	overlay_node.set("CPUPerCoreUsage", overlay.cpu_per_core_usage);
	overlay_node.set("RAMUsage", overlay.ram_usage);
	overlay_node.set("VRAMUsage", overlay.vram_usage);
	overlay_node.set("Debug", overlay.debug);

	auto notification_node = graphic.set("Notification");
	notification_node.set("Position", notification.position);
	notification_node.set("TextColor", notification.text_color);
	notification_node.set("TextScale", notification.text_scale);
	notification_node.set("ControllerProfiles", notification.controller_profiles);
	notification_node.set("ControllerBattery", notification.controller_battery);
	notification_node.set("ShaderCompiling", notification.shader_compiling);
	notification_node.set("FriendService", notification.friends);

	// audio
	auto audio = config.set("Audio");
	audio.set("api", audio_api);
	audio.set("delay", audio_delay);
	audio.set("TVChannels", tv_channels);
	audio.set("PadChannels", pad_channels);
	audio.set("InputChannels", input_channels);
	audio.set("TVVolume", tv_volume);
	audio.set("PadVolume", pad_volume);
	audio.set("InputVolume", input_volume);
	audio.set("PortalVolume", portal_volume);
	audio.set("TVDevice", boost::nowide::narrow(tv_device).c_str());
	audio.set("PadDevice", boost::nowide::narrow(pad_device).c_str());
	audio.set("InputDevice", boost::nowide::narrow(input_device).c_str());
	audio.set("PortalDevice", boost::nowide::narrow(portal_device).c_str());

	// account
	auto acc = config.set("Account");
	acc.set("PersistentId", account.m_persistent_id.GetValue());
	// legacy online mode setting
	acc.set("OnlineEnabled", account.legacy_online_enabled.GetValue());
	acc.set("ActiveService",account.legacy_active_service.GetValue());
	// per-account online setting
	auto accService = config.set("AccountService");
	for(auto& it : account.service_select)
	{
		auto entry = accService.set("SelectedService");
		entry.set_attribute("PersistentId", it.first);
		entry.set_attribute("Service", static_cast<sint32>(it.second));
	}
	// debug
	auto debug = config.set("Debug");
#if BOOST_OS_WINDOWS
	debug.set("CrashDumpWindows", crash_dump.GetValue());
#elif BOOST_OS_UNIX
	debug.set("CrashDumpUnix", crash_dump.GetValue());
#endif
	debug.set("GDBPort", gdb_port);
#ifdef ENABLE_METAL
	debug.set("GPUCaptureDir", gpu_capture_dir);
	debug.set("FramebufferFetch", framebuffer_fetch);
#endif

	// input
	auto input = config.set("Input");
	auto dsuc = input.set("DSUC");
	dsuc.set_attribute("host", dsu_client.host);
	dsuc.set_attribute("port", dsu_client.port);
#if defined(CEMU_UWP)
	dsuc.set_attribute("enabled", dsu_client.enabled);
	dsuc.set_attribute("streamingEnabled", dsu_client.streaming_enabled);
	dsuc.set_attribute("streamingUrl", dsu_client.streaming_url.GetValue().c_str());
	auto dsuServers = dsuc.set("Servers");
	for (const UwpDsuServerEntry& server : dsu_client.servers)
	{
		auto element = dsuServers.set("Server");
		element.set_attribute("id", server.id.c_str());
		element.set_attribute("name", server.name.c_str());
		element.set_attribute("host", server.host.c_str());
		element.set_attribute("port", server.port);
		element.set_attribute("enabled", server.enabled);
	}
	auto dsuRoutes = dsuc.set("Routes");
	for (const UwpDsuPlayerRouteEntry& route : dsu_client.player_routes)
	{
		auto element = dsuRoutes.set("Route");
		element.set_attribute("player", route.player);
		element.set_attribute("server", route.server_id.c_str());
		element.set_attribute("slot", route.remote_slot);
		element.set_attribute("merge", route.merge);
		element.set_attribute("gamepad", route.gamepad);
		element.set_attribute("motion", route.motion);
		element.set_attribute("touch", route.touch);
	}
	#endif

#if defined(CEMU_UWP)
	auto uwp = config.set("UWP");
	auto uwpDisplay = uwp.set("Display");
	uwpDisplay.set("Language", uwp_display.ui_language.GetValue());
	uwpDisplay.set("DefaultLayout", uwp_display.default_layout.GetValue());
	uwpDisplay.set("SwapScreens", uwp_display.swap_screens.GetValue());
	uwpDisplay.set("TouchCursor", uwp_display.touch_cursor.GetValue());
	uwpDisplay.set("ToggleSwapBinding", uwp_display.toggle_swap_binding.GetValue().c_str());
	uwpDisplay.set("VirtualGyroBinding", uwp_display.virtual_gyro_binding.GetValue().c_str());
	uwpDisplay.set("VirtualGyroComboEnabled", uwp_display.virtual_gyro_combo_enabled.GetValue());
	uwpDisplay.set("QuickMenuBinding", UwpQuickMenuBinding::Normalize(uwp_display.quick_menu_binding.GetValue()).c_str());
	auto uwpGameLayouts = uwpDisplay.set("GameLayouts");
	for (const auto& [titleId, entry] : uwp_display.game_layouts)
	{
		auto node = uwpGameLayouts.set("Entry");
		node.set_attribute("TitleId", (sint64)titleId);
		node.set("Layout", entry.layout);
	}

	auto uwpScreenScraper = uwp.set("ScreenScraper");
	uwpScreenScraper.set("Enabled", uwp_screenscraper.enabled.GetValue());
	uwpScreenScraper.set("DownloadMetadata", uwp_screenscraper.download_metadata.GetValue());
	uwpScreenScraper.set("DownloadBoxArt", uwp_screenscraper.download_box_art.GetValue());
	uwpScreenScraper.set("DownloadFanart", uwp_screenscraper.download_fanart.GetValue());
	uwpScreenScraper.set("DownloadVideos", uwp_screenscraper.download_videos.GetValue());
	uwpScreenScraper.set("DownloadScreenshots", uwp_screenscraper.download_screenshots.GetValue());
	uwpScreenScraper.set("DownloadLogos", uwp_screenscraper.download_logos.GetValue());
	uwpScreenScraper.set("OverwriteExisting", uwp_screenscraper.overwrite_existing.GetValue());
	uwpScreenScraper.set("ListArtworkSource", uwp_screenscraper.list_artwork_source.GetValue());
	uwpScreenScraper.set("DetailArtworkSource", uwp_screenscraper.detail_artwork_source.GetValue());
	uwpScreenScraper.set("PreferredRegion", uwp_screenscraper.preferred_region.GetValue());
	uwpScreenScraper.set("MetadataLanguage", uwp_screenscraper.metadata_language.GetValue().c_str());
	uwpScreenScraper.set("MediaDirectory", uwp_screenscraper.media_directory.GetValue());

#endif

	// emulated usb devices
	auto usbdevices = config.set("EmulatedUsbDevices");
	usbdevices.set("EmulateSkylanderPortal", emulated_usb_devices.emulate_skylander_portal.GetValue());
	usbdevices.set("EmulateInfinityBase", emulated_usb_devices.emulate_infinity_base.GetValue());
	usbdevices.set("EmulateDimensionsToypad", emulated_usb_devices.emulate_dimensions_toypad.GetValue());

	return config;
}

GameEntry* CemuConfig::GetGameEntryByTitleId(uint64 titleId)
{
	// assumes game_cache_entries_mutex is already held
	for (auto& it : game_cache_entries)
	{
		if (it.title_id == titleId)
			return &it;
	}
	return nullptr;
}

GameEntry* CemuConfig::CreateGameEntry(uint64 titleId)
{
	// assumes game_cache_entries_mutex is already held
	GameEntry gameEntry;
	gameEntry.title_id = titleId;
	game_cache_entries.emplace_back(gameEntry);
	return &game_cache_entries.back();
}

bool CemuConfig::IsGameListFavorite(uint64 titleId)
{
	std::unique_lock _lock(game_cache_entries_mutex);
	return game_cache_favorites.find(titleId) != game_cache_favorites.end();
}

void CemuConfig::SetGameListFavorite(uint64 titleId, bool isFavorite)
{
	std::unique_lock _lock(game_cache_entries_mutex);
	GameEntry* gameEntry = GetGameEntryByTitleId(titleId);
	if (!gameEntry)
		gameEntry = CreateGameEntry(titleId);
	gameEntry->favorite = isFavorite;
	if (isFavorite)
		game_cache_favorites.emplace(titleId);
	else
		game_cache_favorites.erase(titleId);
}

bool CemuConfig::GetGameListCustomName(uint64 titleId, std::string& customName)
{
	std::unique_lock _lock(game_cache_entries_mutex);
	GameEntry* gameEntry = GetGameEntryByTitleId(titleId);
	if (gameEntry && !gameEntry->custom_name.empty())
	{
		customName = gameEntry->custom_name;
		return true;
	}
	return false;
}

void CemuConfig::SetGameListCustomName(uint64 titleId, std::string customName)
{
	std::unique_lock _lock(game_cache_entries_mutex);
	GameEntry* gameEntry = GetGameEntryByTitleId(titleId);
	if (!gameEntry)
	{
		if (customName.empty())
			return;
		gameEntry = CreateGameEntry(titleId);
	}
	gameEntry->custom_name = std::move(customName);
}

NetworkService CemuConfig::GetAccountNetworkService(uint32 persistentId)
{
	auto it = account.service_select.find(persistentId);
	if (it != account.service_select.end())
	{
		NetworkService serviceIndex = it->second;
		// make sure the returned service is valid
		if (serviceIndex != NetworkService::Offline &&
			serviceIndex != NetworkService::Nintendo &&
			serviceIndex != NetworkService::Pretendo &&
			serviceIndex != NetworkService::Custom)
			return NetworkService::Offline;
		if( static_cast<NetworkService>(serviceIndex) == NetworkService::Custom && !NetworkConfig::XMLExists() )
			return NetworkService::Offline; // custom is selected but no custom config exists
		return serviceIndex;
	}
	// if not found, return the legacy value
	if(!account.legacy_online_enabled)
		return NetworkService::Offline;
	return static_cast<NetworkService>(account.legacy_active_service.GetValue() + 1); // +1 because "Offline" now takes index 0
}

void CemuConfig::SetAccountSelectedService(uint32 persistentId, NetworkService serviceIndex)
{
	account.service_select[persistentId] = serviceIndex;
}
