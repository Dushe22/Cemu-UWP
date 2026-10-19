#include "input/InputManager.h"
#include "config/ActiveSettings.h"
#include "input/ControllerFactory.h"
#include <boost/property_tree/ini_parser.hpp>
#include <pugixml.hpp>
#include "Cafe/GameProfile/GameProfile.h"
#include "util/EventService.h"
#if defined(CEMU_UWP)
#include "UwpImGuiFrontend/InputMapping.h"
#endif
#if defined(CEMU_UWP) && HAS_DSU
#include "uwp/UwpCompanionService.h"
#include "input/api/DSU/DSUController.h"
#endif

#if defined(CEMU_UWP) && HAS_DSU
namespace
{
std::vector<UwpDsuServerEntry> GetConfiguredDsuServers()
{
	const auto& config = GetConfig().dsu_client;
	if (!config.servers.empty())
		return config.servers;
	return { UwpDsuServerEntry{
		.id = "default",
		.name = "DSU Server",
		.host = config.host.GetValue(),
		.port = config.port.GetValue(),
		.enabled = config.enabled.GetValue(),
		.companion_transport = true,
	} };
}

bool IsAnyDsuServerEnabled()
{
	const auto servers = GetConfiguredDsuServers();
	return std::ranges::any_of(servers,
		[](const UwpDsuServerEntry& server) { return server.enabled; });
}

std::optional<UwpDsuServerEntry> ResolveConfiguredDsuServer(
	const DSUProviderSettings& requested)
{
	const auto servers = GetConfiguredDsuServers();
	auto selected = servers.end();
	if (!requested.endpoint_id.empty())
	{
		selected = std::ranges::find_if(servers,
			[&requested](const UwpDsuServerEntry& server) {
				return server.id == requested.endpoint_id;
			});
	}
	if (selected == servers.end() && requested.endpoint_id.empty())
	{
		selected = std::ranges::find_if(servers,
			[&requested](const UwpDsuServerEntry& server) {
				return server.host == requested.ip && server.port == requested.port;
			});
	}
	if (selected == servers.end() || !selected->enabled || selected->host.empty())
		return std::nullopt;
	return *selected;
}

DSUProviderSettings MakeDsuProviderSettings(const UwpDsuServerEntry& server)
{
	return { server.id, server.name, server.host, server.port,
		server.companion_transport };
}

bool HasUnqualifiedControlReference(std::string_view expression)
{
	for (std::size_t start = 0; start < expression.size(); ++start)
	{
		if (expression[start] != '`')
			continue;
		bool escaped = false;
		bool qualified = false;
		for (std::size_t index = start + 1; index < expression.size(); ++index)
		{
			const char character = expression[index];
			if (escaped)
			{
				escaped = false;
				continue;
			}
			if (character == '\\')
			{
				escaped = true;
				continue;
			}
			if (character == '/')
				qualified = true;
			if (character == '`')
			{
				if (!qualified)
					return true;
				start = index;
				break;
			}
		}
	}
	return false;
}

void PopulateDsuProviders(std::vector<ControllerProviderPtr>& providers)
{
	DSUControllerProvider* companionProvider = nullptr;
	for (const UwpDsuServerEntry& server : GetConfiguredDsuServers())
	{
		if (!server.enabled || server.host.empty())
			continue;
		DSUProviderSettings settings(server.id, server.name, server.host,
			server.port, server.companion_transport);
		auto provider = std::make_shared<DSUControllerProvider>(settings);
		if (!companionProvider && server.companion_transport)
			companionProvider = provider.get();
		providers.emplace_back(std::move(provider));
	}
	if (companionProvider)
		CemuUwp::CompanionService::AttachDsuProvider(companionProvider);
}

void ApplyConfiguredDsuRoutes(InputManager& inputManager)
{
	std::array<std::shared_ptr<EmulatedController>, InputManager::kMaxController> players{};
	using SlotKey = std::pair<std::string, uint32>;
	std::map<SlotKey, std::pair<std::size_t, std::shared_ptr<DSUController>>> profileSlots;
	const auto servers = GetConfiguredDsuServers();
	for (std::size_t player = 0; player < players.size(); ++player)
	{
		players[player] = inputManager.get_controller(player);
		if (!players[player])
			continue;
		players[player]->clear_runtime_expression_mappings();
		players[player]->clear_route_controllers();
		std::vector<std::shared_ptr<ControllerBase>> existingDsu;
		for (const auto& controller : players[player]->get_controllers())
		{
			const auto dsu = std::dynamic_pointer_cast<DSUController>(controller);
			if (!dsu)
				continue;
			if (dsu->is_route_managed())
				existingDsu.push_back(controller);
			else
			{
				dsu->reset_runtime_features();
				const auto configured = ResolveConfiguredDsuServer(
					dsu->provider_settings());
				if (!configured || !dsu->rebind_provider(
					MakeDsuProviderSettings(*configured)))
				{
					existingDsu.push_back(controller);
					continue;
				}
				const std::string endpointIdentity = configured->id.empty() ?
					fmt::format("{}:{}", configured->host, configured->port) :
					configured->id;
				const auto [existing, inserted] = profileSlots.try_emplace(
					SlotKey{ endpointIdentity, dsu->remote_slot() },
					player, std::move(dsu));
				if (!inserted)
				{
					cemuLog_log(LogType::Force,
						"Removing duplicate DSU profile assignment from player {}; player {} already uses that endpoint and slot",
						player + 1, existing->second.first + 1);
					existingDsu.push_back(controller);
				}
			}
		}
		for (const auto& controller : existingDsu)
			players[player]->remove_controller(controller);
	}

	auto routes = GetConfig().dsu_client.player_routes;
	for (std::size_t player = 0; player < players.size(); ++player)
	{
		if (!players[player])
			continue;
		const auto& profileRoute = players[player]->get_dsu_route_override();
		if (!profileRoute)
			continue;
		std::erase_if(routes, [player](const UwpDsuPlayerRouteEntry& route) {
			return route.player == player;
		});
		if (!profileRoute->enabled)
			continue;
		routes.push_back({ static_cast<uint8>(player), profileRoute->server_id,
			profileRoute->remote_slot, profileRoute->merge, profileRoute->gamepad,
			profileRoute->motion, profileRoute->touch });
	}
	std::set<SlotKey> claimedSlots;
	for (const UwpDsuPlayerRouteEntry& route : routes)
	{
		if (route.player >= players.size() || route.remote_slot >= DSUControllerProvider::kMaxClients ||
			!players[route.player])
			continue;
		const auto server = std::ranges::find_if(servers,
			[&](const UwpDsuServerEntry& candidate) {
				return candidate.id == route.server_id;
			});
		if (server == servers.end() || !server->enabled || server->host.empty())
			continue;
		const std::string endpointIdentity = server->id.empty() ?
			fmt::format("{}:{}", server->host, server->port) : server->id;
		const SlotKey slotKey{ endpointIdentity, route.remote_slot };
		if (!claimedSlots.emplace(slotKey).second)
		{
			cemuLog_log(LogType::Force,
				"Ignoring duplicate DSU route {} slot {} for player {}",
				server->name, route.remote_slot, route.player + 1);
			continue;
		}

		std::shared_ptr<DSUController> physical;
		if (const auto profileSlot = profileSlots.find(slotKey);
			profileSlot != profileSlots.end())
		{
			if (profileSlot->second.first != route.player)
			{
				cemuLog_log(LogType::Force,
					"Ignoring DSU route {} slot {} for player {} because player {} already uses it",
					server->name, route.remote_slot, route.player + 1,
					profileSlot->second.first + 1);
				continue;
			}
			physical = profileSlot->second.second;
		}
		auto& player = players[route.player];
		if (!physical)
		{
			const DSUProviderSettings settings = MakeDsuProviderSettings(*server);
			try
			{
				physical = std::make_shared<DSUController>(route.remote_slot, settings);
			}
			catch (const std::exception& ex)
			{
				cemuLog_log(LogType::Force,
					"Unable to attach DSU route {} slot {} to player {}: {}",
					server->name, route.remote_slot, route.player + 1, ex.what());
				continue;
			}
			physical->configure_route_features(route.gamepad, route.motion, route.touch);
			player->add_controller(physical);
		}
		else
			physical->configure_runtime_features(route.gamepad, route.motion, route.touch);
		if (route.motion)
			player->set_route_motion_controller(physical);
		if (route.touch)
			player->set_route_pointer_controller(physical);
		if (!route.gamepad)
			continue;

		auto defaults = ControllerFactory::CreateEmulatedController(route.player,
			player->type());
		defaults->add_controller(physical);
		defaults->set_default_mapping(physical);
		const std::string routeDevicePrefix = fmt::format("{}:{}/",
			physical->api_name(), physical->uuid());
		const auto& attached = player->get_controllers();
		const bool routeIsDefault = !attached.empty() && attached.front() &&
			*attached.front() == *physical;
		for (std::uint64_t mapping = 1; mapping < player->get_highest_mapping_id(); ++mapping)
		{
			const std::string configured = player->get_expression_mapping(mapping);
			if (configured.find(routeDevicePrefix) != std::string::npos ||
				(routeIsDefault && HasUnqualifiedControlReference(configured)))
				continue;
			const std::string addition = defaults->get_expression_mapping(mapping);
			if (addition.empty() && route.merge)
				continue;
			player->set_runtime_expression_mapping(mapping, addition, !route.merge);
		}
	}
}
}
#endif

InputManager::InputManager()
{
	/*
	auto create_provider = []
	template <typename TProvider>
	()
	{
		static_assert(std::is_base_of_v<ControllerProvider, TProvider>);
		try
		{
			auto controller = std::make_shared<TProvider>();
			m_api_available[controller->api()] = controller;
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, ex.what());
		}
	}
	*/
#if HAS_KEYBOARD
	create_provider<KeyboardControllerProvider>();
#endif
#ifdef HAS_SDL
	create_provider<SDLControllerProvider>();
#endif
#if HAS_XINPUT
	create_provider<XInputControllerProvider>();
#endif
#if HAS_DIRECTINPUT
	create_provider<DirectInputControllerProvider>();
#endif
#if HAS_DSU
#if defined(CEMU_UWP)
	if (IsAnyDsuServerEnabled())
	{
		try
		{
			PopulateDsuProviders(m_api_available[InputAPI::DSUClient]);
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, ex.what());
		}
	}
#else
	create_provider<DSUControllerProvider>();
#endif
#endif
#if defined(HAS_GAMECUBE) && HAS_GAMECUBE && defined(HAS_LIBUSB)
	create_provider<GameCubeControllerProvider>();
#endif
#if HAS_WIIMOTE
	create_provider<WiimoteControllerProvider>();
#endif
#if defined(CEMU_UWP)
	create_provider<WGIGamepadControllerProvider>();
#endif

	m_update_thread_shutdown.store(false);
	m_update_thread = std::thread(&InputManager::update_thread, this);
}

InputManager::~InputManager()
{
	// destructors will not invoked forever, so we manually release resources in Shutdown().
}

void InputManager::load() noexcept
{
	for (size_t i = 0; i < kMaxController; ++i)
	{
		try
		{
			load(i);
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "can't load controller profile: {}", ex.what());
		}
	}
#if defined(CEMU_UWP) && HAS_DSU
	ApplyConfiguredDsuRoutes(*this);
#endif
}

bool InputManager::load(size_t player_index, std::string_view filename)
{
	fs::path file_path;
	if (filename.empty())
		file_path = ActiveSettings::GetConfigPath("controllerProfiles/controller{}", player_index);
	else
		file_path = ActiveSettings::GetConfigPath("controllerProfiles/{}", filename);

	auto old_file = file_path;
	old_file.replace_extension(".txt"); // test .txt extension
	file_path.replace_extension(".xml"); // force .xml extension

	if (fs::exists(old_file) && !fs::exists(file_path))
		migrate_config(old_file);

	if (!fs::exists(file_path))
		return false;

	try
	{
		auto xmlData = FileStream::LoadIntoMemory(file_path);
		if (!xmlData || xmlData->empty())
			return false;
	
		pugi::xml_document doc;
		if (!doc.load_buffer(xmlData->data(), xmlData->size()))
			return false;

		const pugi::xml_node root = doc.document_element();

		const auto type_node = root.child("type");
		if (!type_node)
			return false;

		const auto emulate = EmulatedController::type_from_string(type_node.child_value());
#if defined(CEMU_UWP)
		if (!can_set_controller(player_index, emulate))
		{
			cemuLog_log(LogType::Force,
				"Controller profile for player {} exceeds Cemu's active controller limits",
				player_index + 1);
			return false;
		}
#endif
		auto emulated_controller = ControllerFactory::CreateEmulatedController(player_index, emulate);


		if (const auto profile_name_node = root.child("profile"))
			emulated_controller->m_profile_name = profile_name_node.child_value();

		// custom settings
		emulated_controller->load(root);

#if defined(CEMU_UWP)
		bool physicalControllerLoadFailed = false;
		std::vector<UwpImGuiFrontend::InputDeviceIdAlias> deviceAliases;
		std::shared_ptr<ControllerBase> defaultPhysicalController;
#endif
		for (const auto controller_node : root.select_nodes("controller"))
		{
			const auto cnode = controller_node.node();
			const auto api_node = cnode.child("api");
			if (!api_node)
				continue;

			const auto uuid_node = cnode.child("uuid");
			if (!uuid_node)
				continue;

			const auto* display_name = cnode.child_value("display_name");

			try
			{
				const auto api = InputAPI::from_string(api_node.child_value());
				const std::string persistedDeviceId = fmt::format("{}:{}",
					InputAPI::to_string(api), uuid_node.child_value());
				auto controller = ControllerFactory::CreateController(api, uuid_node.child_value(), display_name);
#if !defined(CEMU_UWP)
				emulated_controller->add_controller(controller);
#endif

				// load optional settings
				auto settings = controller->get_settings();
				if (const auto axis_node = cnode.child("axis"))
				{
					if (const auto value = axis_node.child("deadzone"))
						settings.axis.deadzone = ConvertString<float>(value.child_value());

					if (const auto value = axis_node.child("range"))
						settings.axis.range = ConvertString<float>(value.child_value());
				}
				if (const auto rotation_node = cnode.child("rotation"))
				{
					if (const auto value = rotation_node.child("deadzone"))
						settings.rotation.deadzone = ConvertString<float>(value.child_value());

					if (const auto value = rotation_node.child("range"))
						settings.rotation.range = ConvertString<float>(value.child_value());
				}
				if (const auto trigger_node = cnode.child("trigger"))
				{
					if (const auto value = trigger_node.child("deadzone"))
						settings.trigger.deadzone = ConvertString<float>(value.child_value());

					if (const auto value = trigger_node.child("range"))
						settings.trigger.range = ConvertString<float>(value.child_value());
				}

				if (const auto value = cnode.child("rumble"))
					settings.rumble = ConvertString<float>(value.child_value());

				if (const auto value = cnode.child("motion"))
					settings.motion = ConvertString<bool>(value.child_value());

				controller->set_settings(settings);

				// custom settings
				controller->load(cnode);
#if defined(CEMU_UWP)
				emulated_controller->add_controller(controller);
				if (!defaultPhysicalController)
					defaultPhysicalController = controller;
				const std::string runtimeDeviceId = fmt::format("{}:{}",
					controller->api_name(), controller->uuid());
				if (persistedDeviceId != runtimeDeviceId)
					deviceAliases.push_back({ persistedDeviceId, runtimeDeviceId });
#endif

				// mappings
				if (const auto mappings_node = cnode.child("mappings"))
				{
					for (const auto& entry : mappings_node.select_nodes("entry"))
					{
						const auto enode = entry.node();

						const auto mapping_node = enode.child("mapping");
						if (!mapping_node)
							continue;

						const auto button_node = enode.child("button");
						if (!button_node)
							continue;

						const auto mapping = ConvertString<uint64>(mapping_node.child_value());
						const auto button = ConvertString<uint64>(button_node.child_value());

						emulated_controller->set_mapping(mapping, controller, button);
					}
				}
			}
			catch (const std::exception& ex)
			{
				cemuLog_log(LogType::Force, "can't load controller: {}", ex.what());
#if defined(CEMU_UWP)
				physicalControllerLoadFailed = true;
#endif
			}
		}

#if defined(CEMU_UWP)
		if (physicalControllerLoadFailed)
		{
			cemuLog_log(LogType::Force,
				"Controller profile for player {} was rejected because an attached input device could not be loaded",
				player_index + 1);
			return false;
		}

		if (const auto expressions = root.child("uwp_expressions"))
		{
			EmulatedControllerPtr migrationDefaults;
			if (defaultPhysicalController)
			{
				migrationDefaults = ControllerFactory::CreateEmulatedController(
					player_index, emulate);
				migrationDefaults->add_controller(defaultPhysicalController);
				migrationDefaults->set_default_mapping(defaultPhysicalController);
			}
			for (const auto& entry : expressions.select_nodes("entry"))
			{
				const auto node = entry.node();
				const auto mapping = node.attribute("mapping");
				const auto expression = node.attribute("expression");
				if (mapping && expression)
				{
					const uint64 mappingId = ConvertString<uint64>(mapping.value());
					std::string source = expression.value();
					if (source.empty())
					{
						emulated_controller->set_expression_mapping(mappingId, {});
						continue;
					}

					const auto rewritten =
						UwpImGuiFrontend::RewriteInputControlDeviceIds(
							source, deviceAliases);
					if (rewritten)
						source = *rewritten;
					UwpImGuiFrontend::MappingExpression validation(source);
					if (!validation.IsValid() && !rewritten && migrationDefaults)
					{
						bool affectedByIdentityMigration = false;
						for (const auto& alias : deviceAliases)
						{
							const std::string escaped =
								UwpImGuiFrontend::EscapeInputControlReference(
									alias.previousId);
							if (source.find(alias.previousId) != std::string::npos ||
								(escaped.size() > 1 && source.find(escaped.substr(
									0, escaped.size() - 1)) != std::string::npos))
							{
								affectedByIdentityMigration = true;
								break;
							}
						}
						if (affectedByIdentityMigration)
						{
							source = migrationDefaults->get_expression_mapping(mappingId);
							cemuLog_log(LogType::Force,
								"Recovered controller mapping {} for player {} from an unsafe WGI device identity",
								mappingId, player_index + 1);
						}
					}
					emulated_controller->set_expression_mapping(mappingId,
						std::move(source));
				}
			}
		}
		if (const auto routeNode = root.child("uwp_dsu_route"))
		{
			EmulatedController::DsuRouteOverride route;
			const unsigned remoteSlot = routeNode.attribute("slot").as_uint(0);
			route.enabled = routeNode.attribute("enabled").as_bool(false) &&
				remoteSlot < 16;
			route.server_id = routeNode.attribute("server").as_string();
			route.remote_slot = static_cast<uint8>(std::min(remoteSlot, 255u));
			route.merge = routeNode.attribute("merge").as_bool(false);
			route.gamepad = routeNode.attribute("gamepad").as_bool(true);
			route.motion = routeNode.attribute("motion").as_bool(true);
			route.touch = routeNode.attribute("touch").as_bool(true);
			emulated_controller->set_dsu_route_override(std::move(route));
		}
#endif

#if defined(CEMU_UWP)
		// a loaded profile is an exact device set, do not inherit prior devices
		delete_controller(player_index, false);
#endif
		set_controller(emulated_controller);
		return true;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't load config file: {}", ex.what());
		return false;
	}
}

bool InputManager::migrate_config(const fs::path& file_path)
{
	try
	{
		auto xmlData = FileStream::LoadIntoMemory(file_path);
		if (!xmlData || xmlData->empty())
			return false;

		std::string iniDataStr((const char*)xmlData->data(), xmlData->size());

		std::stringstream iniData(iniDataStr);
		boost::property_tree::ptree m_data;
		read_ini(iniData, m_data);

		const auto emulate_string = m_data.get<std::string>("General.emulate");
		const auto api_string = m_data.get<std::string>("General.api");
		auto uuid_opt = m_data.get_optional<std::string>("General.controller");
		const auto display_name = m_data.get_optional<std::string>("General.display");

		std::string uuid;
		if (api_string == to_string(InputAPI::Keyboard))
			uuid = to_string(InputAPI::Keyboard);
		else
		{
			if (!uuid_opt)
				return false;

			uuid = uuid_opt.value();
			if (api_string == to_string(InputAPI::SDLController))
			{
				uuid += "_0";
			}
		}

		fs::path out_file = file_path;
		out_file.replace_extension(".xml");

		pugi::xml_document doc;
		auto declaration_node = doc.append_child(pugi::node_declaration);
		declaration_node.append_attribute("version") = "1.0";
		declaration_node.append_attribute("encoding") = "UTF-8";

		auto emulated_controller = doc.append_child("emulated_controller");
		emulated_controller.append_child("type").append_child(pugi::node_pcdata).set_value(emulate_string.c_str());

		bool has_keyboard = api_string == to_string(InputAPI::Keyboard);
		if (!has_keyboard) // test if only keyboard configured
		{
			auto controller = emulated_controller.append_child("controller");
			controller.append_child("api").append_child(pugi::node_pcdata).set_value(api_string.c_str());
			controller.append_child("uuid").append_child(pugi::node_pcdata).set_value(uuid.c_str());
			if (display_name.has_value() && !display_name->empty())
				controller.append_child("display_name").append_child(pugi::node_pcdata).set_value(
					display_name.value().c_str());


			controller.append_child("rumble").append_child(pugi::node_pcdata).set_value(
				m_data.get<std::string>("Controller.rumble").c_str());

			auto axis_node = controller.append_child("axis");
			axis_node.append_child("deadzone").append_child(pugi::node_pcdata).set_value(
				m_data.get<std::string>("Controller.leftDeadzone").c_str());
			axis_node.append_child("range").append_child(pugi::node_pcdata).set_value(
				m_data.get<std::string>("Controller.leftRange").c_str());

			auto rotation_node = controller.append_child("rotation");
			rotation_node.append_child("deadzone").append_child(pugi::node_pcdata).set_value(
				m_data.get<std::string>("Controller.rightDeadzone").c_str());
			rotation_node.append_child("range").append_child(pugi::node_pcdata).set_value(
				m_data.get<std::string>("Controller.rightRange").c_str());

			auto mappings_node = controller.append_child("mappings");
			for (int i = 1; i < 28; ++i) // test all possible mappings (max is 27 for vpad controller)
			{
				auto mapping = m_data.get_optional<std::string>(fmt::format("Controller.{}", i));
				if (!mapping || mapping->empty())
					continue;

				if (!boost::starts_with(mapping.value(), "button_"))
				{
					if (boost::starts_with(mapping.value(), "key_"))
						has_keyboard = true;

					continue;
				}

				const auto button = ConvertString<uint64>(mapping.value().substr(7), 16);

				uint64 flag_bit = 0;
				for (auto b = 0; b < 64; ++b)
				{
					if (HAS_BIT(button, b))
					{
						flag_bit = b;
						break;
					}
				}

				// fix old flag layout to new one for all kind of axis stuff
				if (flag_bit >= 24 && flag_bit <= 31)
					flag_bit += 8;
				else if (flag_bit == 32) flag_bit = kTriggerXP;
				else if (flag_bit == 33) flag_bit = kRotationXP;
				else if (flag_bit == 34) flag_bit = kRotationYP;
				else if (flag_bit == 35) flag_bit = kTriggerYP;
				else if (flag_bit == 36) flag_bit = kAxisXN;
				else if (flag_bit == 37) flag_bit = kAxisYN;
				else if (flag_bit == 38) flag_bit = kTriggerXN;
				else if (flag_bit == 39) flag_bit = kRotationXN;
				else if (flag_bit == 40) flag_bit = kRotationYN;
				else if (flag_bit == 41) flag_bit = kTriggerYN;

				// fix old api mappings
				if (api_string == to_string(InputAPI::XInput))
				{
					const std::unordered_map<uint64, uint64> xinput =
					{
						{kButton0, 12}, // XINPUT_GAMEPAD_A
						{kButton1, 13}, // XINPUT_GAMEPAD_B
						{kButton2, 14}, // XINPUT_GAMEPAD_X
						{kButton3, 15}, // XINPUT_GAMEPAD_Y

						{kButton4, 8}, // XINPUT_GAMEPAD_LEFT_SHOULDER
						{kButton5, 9}, // XINPUT_GAMEPAD_LEFT_SHOULDER

						{kButton6, 4}, // XINPUT_GAMEPAD_START
						{kButton7, 5}, // XINPUT_GAMEPAD_BACK

						{kButton8, 6}, // XINPUT_GAMEPAD_LEFT_THUMB
						{kButton9, 7}, // XINPUT_GAMEPAD_RIGHT_THUMB

						{kButton10, 0}, // XINPUT_GAMEPAD_DPAD_UP
						{kButton11, 1}, // XINPUT_GAMEPAD_DPAD_DOWN
						{kButton12, 2}, // XINPUT_GAMEPAD_DPAD_LEFT
						{kButton13, 3}, // XINPUT_GAMEPAD_DPAD_RIGHT
					};

					const auto it = xinput.find(flag_bit);
					if (it != xinput.cend())
						flag_bit = it->second;
				}
				else if (api_string == "DSU")
				{
					const std::unordered_map<uint64, uint64> dsu =
					{
						{7, kButton0}, // ButtonSelect
						{8, kButton1}, // ButtonLStick
						{9, kButton2}, // ButtonRStick
						{6, kButton3}, // ButtonStart

						{4, kButton10}, // ButtonL
						{5, kButton11}, // ButtonR

						{0, kButton14}, // ButtonA
						{1, kButton13}, // ButtonB
						{2, kButton15}, // ButtonX
						{3, kButton12}, // ButtonY
					};

					const auto it = dsu.find(flag_bit);
					if (it != dsu.cend())
						flag_bit = it->second;
				}


				auto entry_node = mappings_node.append_child("entry");
				entry_node.append_child("mapping").append_child(pugi::node_pcdata).set_value(
					fmt::format("{}", i).c_str());
				entry_node.append_child("button").append_child(pugi::node_pcdata).set_value(
					fmt::format("{}", flag_bit).c_str());
			}
		}

		if (has_keyboard)
		{
			auto controller = emulated_controller.append_child("controller");
			controller.append_child("api").append_child(pugi::node_pcdata).set_value("Keyboard");
			controller.append_child("uuid").append_child(pugi::node_pcdata).set_value("Keyboard");

			auto mappings_node = controller.append_child("mappings");
			for (int i = 1; i < 28; ++i) // test all possible mappings (max is 27 for vpad controller)
			{
				auto mapping = m_data.get_optional<std::string>(fmt::format("Controller.{}", i));
				if (!mapping || mapping->empty())
					continue;

				if (!boost::starts_with(mapping.value(), "key_"))
					continue;

				const auto button = ConvertString<uint64>(mapping.value().substr(4));

				auto entry_node = mappings_node.append_child("entry");
				entry_node.append_child("mapping").append_child(pugi::node_pcdata).set_value(
					fmt::format("{}", i).c_str());
				entry_node.append_child("button").append_child(pugi::node_pcdata).set_value(
					fmt::format("{}", button).c_str());
			}
		}

		std::ofstream write_file(out_file, std::ios::out | std::ios::trunc);
		if (write_file.is_open())
		{
			doc.save(write_file);
			return true;
		}
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't migrate config file {}: {}", file_path.string(), ex.what());
	}

	return false;
}

void InputManager::save() noexcept
{
	for (size_t i = 0; i < kMaxController; ++i)
	{
		try
		{
			save(i);
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "can't save controller profile: {}", ex.what());
		}
	}
}

bool InputManager::save(size_t player_index, std::string_view filename)
{
	// dont overwrite files if set by gameprofile
	if (m_is_gameprofile_set[player_index])
		return true;

	auto emulated_controller = get_controller(player_index);
	if (!emulated_controller)
		return false;

	fs::path file_path = ActiveSettings::GetConfigPath("controllerProfiles");
	fs::create_directories(file_path);

	const auto is_default_file = filename.empty();
	if (is_default_file)
		file_path /= fmt::format("controller{}", player_index);
	else
		file_path /= _utf8ToPath(filename);

	file_path.replace_extension(".xml"); // force .xml extension

	pugi::xml_document doc;
	auto declaration_node = doc.append_child(pugi::node_declaration);
	declaration_node.append_attribute("version") = "1.0";
	declaration_node.append_attribute("encoding") = "UTF-8";

	auto emulated_controller_node = doc.append_child("emulated_controller");
	emulated_controller_node.append_child("type").append_child(pugi::node_pcdata).set_value(std::string{
		emulated_controller->type_string()
	}.c_str());

	if(!is_default_file)
		emulated_controller->m_profile_name = std::string{filename};

	if (emulated_controller->has_profile_name())
		emulated_controller_node.append_child("profile").append_child(pugi::node_pcdata).set_value(
			emulated_controller->get_profile_name().c_str());

	// custom settings
	emulated_controller->save(emulated_controller_node);

	for (const auto& controller : emulated_controller->get_controllers())
	{
#if defined(CEMU_UWP) && HAS_DSU
		if (const auto dsu = std::dynamic_pointer_cast<DSUController>(controller);
			dsu && dsu->is_route_managed())
		{
			continue;
		}
#endif
		auto controller_node = emulated_controller_node.append_child("controller");

		// general
		controller_node.append_child("api").append_child(pugi::node_pcdata).set_value(std::string{
			controller->api_name()
		}.c_str());
		controller_node.append_child("uuid").append_child(pugi::node_pcdata).set_value(controller->uuid().c_str());
		controller_node.append_child("display_name").append_child(pugi::node_pcdata).set_value(
			controller->display_name().c_str());

		// settings
		const auto& settings = controller->get_settings();

		if (controller->has_motion())
			controller_node.append_child("motion").append_child(pugi::node_pcdata).set_value(
				fmt::format("{}", settings.motion).c_str());

		if (controller->has_rumble())
			controller_node.append_child("rumble").append_child(pugi::node_pcdata).set_value(
				fmt::format("{}", settings.rumble).c_str());

		auto axis_node = controller_node.append_child("axis");
		axis_node.append_child("deadzone").append_child(pugi::node_pcdata).set_value(
			fmt::format("{}", settings.axis.deadzone).c_str());
		axis_node.append_child("range").append_child(pugi::node_pcdata).set_value(
			fmt::format("{}", settings.axis.range).c_str());

		auto rotation_node = controller_node.append_child("rotation");
		rotation_node.append_child("deadzone").append_child(pugi::node_pcdata).set_value(
			fmt::format("{}", settings.rotation.deadzone).c_str());
		rotation_node.append_child("range").append_child(pugi::node_pcdata).set_value(
			fmt::format("{}", settings.rotation.range).c_str());

		auto trigger_node = controller_node.append_child("trigger");
		trigger_node.append_child("deadzone").append_child(pugi::node_pcdata).set_value(
			fmt::format("{}", settings.trigger.deadzone).c_str());
		trigger_node.append_child("range").append_child(pugi::node_pcdata).set_value(
			fmt::format("{}", settings.trigger.range).c_str());

		// custom settings
		controller->save(controller_node);

		// mappings for current controller
		auto mappings_node = controller_node.append_child("mappings");
		for (const auto& mapping : emulated_controller->m_mappings)
		{
			if (!mapping.second.controller.expired() && *controller == *mapping.second.controller.lock())
			{
				auto entry_node = mappings_node.append_child("entry");
				entry_node.append_child("mapping").append_child(pugi::node_pcdata).set_value(
					fmt::format("{}", mapping.first).c_str());
				entry_node.append_child("button").append_child(pugi::node_pcdata).set_value(
					fmt::format("{}", mapping.second.button).c_str());
			}
		}
	}
#if defined(CEMU_UWP)
	auto expressionsNode = emulated_controller_node.append_child("uwp_expressions");
	for (const auto& [mapping, expression] : emulated_controller->get_expression_mappings())
	{
		auto entry = expressionsNode.append_child("entry");
		entry.append_attribute("mapping") = fmt::format("{}", mapping).c_str();
		entry.append_attribute("expression") = expression.c_str();
	}
#endif
	FileStream* fs = FileStream::createFile2(file_path);
	if (!fs)
		return false;
	std::stringstream xmlData;
	doc.save(xmlData);
	std::string xmlStr = xmlData.str();
	fs->writeData(xmlStr.data(), xmlStr.size());
	delete fs;
	return true;
}

bool InputManager::is_gameprofile_set(size_t player_index) const
{
	return m_is_gameprofile_set[player_index];
}

EmulatedControllerPtr InputManager::set_controller(EmulatedControllerPtr controller)
{
	auto prev_controller = delete_controller(controller->player_index());

	// assign controllers to new emulated controller if empty
	if (prev_controller && controller->get_controllers().empty())
	{
		for (const auto& c : prev_controller->get_controllers())
		{
			controller->add_controller(c);
		}
	}

	// try to connect all controllers
	/*for (auto& c : controller->get_controllers())
	{
		c->connect();
	}*/

	std::scoped_lock lock(m_mutex);
	switch (controller->type())
	{
	case EmulatedController::Type::VPAD:
		for (auto& pad : m_vpad)
		{
			if (!pad)
			{
				pad.swap(controller);
				return prev_controller;
			}
		}

		break;

	default:
		for (auto& pad : m_wpad)
		{
			if (!pad)
			{
				pad.swap(controller);
				return prev_controller;
			}
		}

		break;
	}
	
	cemu_assert_debug(false);
	return prev_controller;
}

EmulatedControllerPtr InputManager::set_controller(size_t player_index, EmulatedController::Type type)
{
	try
	{
#if defined(CEMU_UWP)
		if (!can_set_controller(player_index, type))
			return {};
#endif
		auto emulated_controller = ControllerFactory::CreateEmulatedController(player_index, type);
		set_controller(emulated_controller);
		return emulated_controller;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "Unable to set controller type {} on player index {}: {}", type, player_index, ex.what());
	}

	return {};
}

EmulatedControllerPtr InputManager::set_controller(size_t player_index, EmulatedController::Type type,
                                                   const std::shared_ptr<ControllerBase>& controller)
{
	auto result = set_controller(player_index, type);
	if (result)
		result->add_controller(controller);

	return result;
}

EmulatedControllerPtr InputManager::get_controller(size_t player_index) const
{
	std::shared_lock lock(m_mutex);
	for (const auto& pad : m_vpad)
	{
		if (pad && pad->player_index() == player_index)
			return pad;
	}

	for (const auto& pad : m_wpad)
	{
		if (pad && pad->player_index() == player_index)
			return pad;
	}

	return {};
}

EmulatedControllerPtr InputManager::delete_controller(size_t player_index, bool delete_profile)
{
	std::scoped_lock lock(m_mutex);
	for (auto& controller : m_vpad)
	{
		auto result = controller;
		if (result && result->player_index() == player_index)
		{
			controller = {};

			if(delete_profile)
			{
				std::error_code ec{};
				fs::remove(ActiveSettings::GetConfigPath("controllerProfiles/controller{}.xml", player_index), ec);
				fs::remove(ActiveSettings::GetConfigPath("controllerProfiles/controller{}.txt", player_index), ec);
			}

			return result;
		}
	}

	for (auto& controller : m_wpad)
	{
		auto result = controller;
		if (result && result->player_index() == player_index)
		{
			controller = {};

			if(delete_profile)
			{
				std::error_code ec{};
				fs::remove(ActiveSettings::GetConfigPath("controllerProfiles/controller{}.xml", player_index), ec);
				fs::remove(ActiveSettings::GetConfigPath("controllerProfiles/controller{}.txt", player_index), ec);
			}

			return result;
		}
	}

	return {};
}


std::shared_ptr<VPADController> InputManager::get_vpad_controller(size_t index) const
{
	if (index >= m_vpad.size())
		return {};

	std::shared_lock lock(m_mutex);
	return std::dynamic_pointer_cast<VPADController>(m_vpad[index]);
}

std::shared_ptr<WPADController> InputManager::get_wpad_controller(size_t index) const
{
	if (index >= m_wpad.size())
		return {};

	std::shared_lock lock(m_mutex);
	return std::dynamic_pointer_cast<WPADController>(m_wpad[index]);
}

std::pair<size_t, size_t> InputManager::get_controller_count() const
{
	std::shared_lock lock(m_mutex);
	const size_t vpad = std::count_if(m_vpad.cbegin(), m_vpad.cend(), [](const auto& v) { return v != nullptr; });
	const size_t wpad = std::count_if(m_wpad.cbegin(), m_wpad.cend(), [](const auto& v) { return v != nullptr; });
	return std::make_pair(vpad, wpad);
}

void InputManager::on_device_changed()
{
	std::shared_lock lock(m_mutex);
	for (auto& pad : m_vpad)
	{
		if (pad)
			pad->connect();
	}

	for (auto& pad : m_wpad)
	{
		if (pad)
			pad->connect();
	}
	lock.unlock();

	EventService::instance().signal<Events::ControllerChanged>();
}

ControllerProviderPtr InputManager::get_api_provider(InputAPI::Type api) const
{
	if(!m_api_available[api].empty())
		return *(m_api_available[api].begin());
	
	cemu_assert_debug(false);
	return {};
}

ControllerProviderPtr InputManager::get_api_provider(InputAPI::Type api, const ControllerProviderSettings& settings)
{
#if defined(CEMU_UWP) && HAS_DSU
	std::optional<DSUProviderSettings> resolvedDsuSettings;
	const ControllerProviderSettings* effectiveSettings = &settings;
	if (api == InputAPI::DSUClient)
	{
		const auto* requested = dynamic_cast<const DSUProviderSettings*>(&settings);
		if (!requested)
			return {};
		const auto server = ResolveConfiguredDsuServer(*requested);
		if (!server)
			return {};
		resolvedDsuSettings = MakeDsuProviderSettings(*server);
		effectiveSettings = &*resolvedDsuSettings;
	}
	const ControllerProviderSettings& selectedSettings = *effectiveSettings;
#else
	const ControllerProviderSettings& selectedSettings = settings;
#endif
	for(const auto& p : m_api_available[api])
	{
		if(*p == selectedSettings)
		{
			return p;
		}
	}

	const auto result = ControllerFactory::CreateControllerProvider(api, selectedSettings);
	if (!result)
		return {};
	m_api_available[api].emplace_back(result);
	return result;
}

#if defined(CEMU_UWP) && HAS_DSU
void InputManager::apply_uwp_dsu_routes()
{
	ApplyConfiguredDsuRoutes(*this);
}

void InputManager::refresh_uwp_dsu_provider()
{
	auto& providers = m_api_available[InputAPI::DSUClient];
	for (const auto& provider : providers)
	{
		if (const auto dsu = std::dynamic_pointer_cast<DSUControllerProvider>(provider))
			CemuUwp::CompanionService::DetachDsuProvider(dsu.get());
	}
	providers.clear();

	if (!IsAnyDsuServerEnabled())
	{
		ApplyConfiguredDsuRoutes(*this);
		EventService::instance().signal<Events::ControllerChanged>();
		return;
	}

	try
	{
		PopulateDsuProviders(providers);
		ApplyConfiguredDsuRoutes(*this);
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "UWP DSU provider refresh failed: {}", ex.what());
	}

	EventService::instance().signal<Events::ControllerChanged>();
}
#endif

void InputManager::apply_game_profile()
{
	const auto& profiles = g_current_game_profile->GetControllerProfile();
#if defined(CEMU_UWP)
	std::array<EmulatedControllerPtr, kMaxController> previousControllers{};
	const auto previousGameProfileState = m_is_gameprofile_set;
	const auto previousPreGameControllers = m_uwp_pre_game_controllers;
	const auto previousPreGameCaptured = m_uwp_pre_game_controller_captured;
	std::array<bool, kMaxController> targeted{};
	for (size_t i = 0; i < kMaxController; ++i)
	{
		if (!profiles[i] || profiles[i]->empty())
			continue;
		targeted[i] = true;
		previousControllers[i] = get_controller(i);
		m_is_gameprofile_set[i] = false;
		delete_controller(i, false);
	}

	bool loadedAll = true;
	for (size_t i = 0; i < kMaxController; ++i)
	{
		if (!targeted[i])
			continue;
		if (!load(i, profiles[i].value()))
		{
			loadedAll = false;
			break;
		}
		m_is_gameprofile_set[i] = true;
		if (!previousGameProfileState[i])
		{
			m_uwp_pre_game_controllers[i] = previousControllers[i];
			m_uwp_pre_game_controller_captured[i] = true;
		}
		if (const auto controller = get_controller(i);
			controller && !controller->has_profile_name())
		{
			controller->m_profile_name = profiles[i].value();
		}
	}
	if (!loadedAll)
	{
		for (size_t i = 0; i < kMaxController; ++i)
		{
			if (targeted[i])
				delete_controller(i, false);
		}
		for (size_t i = 0; i < kMaxController; ++i)
		{
			if (targeted[i] && previousControllers[i])
				set_controller(previousControllers[i]);
			if (targeted[i])
			{
				m_is_gameprofile_set[i] = previousGameProfileState[i];
				m_uwp_pre_game_controllers[i] = previousPreGameControllers[i];
				m_uwp_pre_game_controller_captured[i] = previousPreGameCaptured[i];
			}
		}
		cemuLog_log(LogType::Force,
			"Controller game profile was rejected; the previous controller set was restored");
	}
#else
	for (int i = 0; i < kMaxController; ++i)
	{
		if (profiles[i] && !profiles[i]->empty())
		{
			if (load(i, profiles[i].value()))
			{
				m_is_gameprofile_set[i] = true;
				if (const auto controller = get_controller(i))
				{
					if (!controller->has_profile_name())
						controller->m_profile_name = profiles[i].value();
				}
			}
		}
	}
#endif
#if defined(CEMU_UWP) && HAS_DSU
	ApplyConfiguredDsuRoutes(*this);
#endif
}

#if defined(CEMU_UWP)
bool InputManager::activate_uwp_title_profile(size_t player_index,
	std::string_view filename)
{
	if (player_index >= kMaxController || filename.empty())
		return false;
	const bool replacingTitleProfile = m_is_gameprofile_set[player_index];
	const EmulatedControllerPtr previousController = replacingTitleProfile ?
		EmulatedControllerPtr{} : get_controller(player_index);
	if (!load(player_index, filename))
		return false;
	if (!replacingTitleProfile)
	{
		m_uwp_pre_game_controllers[player_index] = previousController;
		m_uwp_pre_game_controller_captured[player_index] = true;
	}
	m_is_gameprofile_set[player_index] = true;
#if HAS_DSU
	ApplyConfiguredDsuRoutes(*this);
#endif
	return true;
}

void InputManager::restore_uwp_default_profiles()
{
	std::array<bool, kMaxController> restore{};
	for (size_t i = 0; i < kMaxController; ++i)
	{
		if (!m_is_gameprofile_set[i])
			continue;
		restore[i] = true;
		m_is_gameprofile_set[i] = false;
		delete_controller(i, false);
	}
	for (size_t i = 0; i < kMaxController; ++i)
	{
		if (!restore[i])
			continue;
		if (m_uwp_pre_game_controller_captured[i])
		{
			if (m_uwp_pre_game_controllers[i])
				set_controller(m_uwp_pre_game_controllers[i]);
			m_uwp_pre_game_controllers[i].reset();
			m_uwp_pre_game_controller_captured[i] = false;
			continue;
		}
		try
		{
			(void)load(i);
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "Unable to restore UWP controller {} after game profile: {}", i + 1, ex.what());
		}
	}
#if defined(CEMU_UWP) && HAS_DSU
	ApplyConfiguredDsuRoutes(*this);
#endif
}

void InputManager::set_uwp_gameprofile_state(size_t player_index, bool active)
{
	if (player_index < kMaxController)
	{
		m_is_gameprofile_set[player_index] = active;
		if (!active)
		{
			m_uwp_pre_game_controllers[player_index].reset();
			m_uwp_pre_game_controller_captured[player_index] = false;
		}
	}
}

void InputManager::reset_uwp_expression_state()
{
	for (std::size_t player = 0; player < kMaxController; ++player)
	{
		if (const auto controller = get_controller(player))
			controller->reset_expression_state();
	}
}
#endif

#if defined(CEMU_UWP)
bool InputManager::can_set_controller(size_t player_index,
	EmulatedController::Type type) const
{
	if (player_index >= kMaxController)
		return false;
	std::shared_lock lock(m_mutex);
	std::size_t vpadCount = 0;
	std::size_t wpadCount = 0;
	for (const auto& controller : m_vpad)
	{
		if (controller && controller->player_index() != player_index)
			++vpadCount;
	}
	for (const auto& controller : m_wpad)
	{
		if (controller && controller->player_index() != player_index)
			++wpadCount;
	}
	return type == EmulatedController::VPAD ?
		vpadCount < kMaxVPADControllers : wpadCount < kMaxWPADControllers;
}
#endif

std::vector<std::string> InputManager::get_profiles()
{
	const auto path = ActiveSettings::GetConfigPath("controllerProfiles");
	if (!exists(path))
		return {};

	std::set<std::string> tmp;
	for (const auto& entry : fs::directory_iterator(path))
	{
		const auto& p = entry.path();
		if (p.has_extension() && (p.extension() == ".xml" || p.extension() == ".txt"))
		{
			auto stem = _pathToUtf8(p.filename().stem());
			if (is_valid_profilename(stem))
			{
				tmp.emplace(stem);
			}
		}
	}

	std::vector<std::string> result;
	result.reserve(tmp.size());
	result.insert(result.end(), tmp.begin(), tmp.end());
	return result;
}

bool InputManager::is_valid_profilename(const std::string& name)
{
	if (!IsValidFilename(name))
		return false;

	// dont allow default profile names
	for (size_t i = 0; i < kMaxController; i++)
	{
		if (name == fmt::format("controller{}", i))
			return false;
	}

	return true;
}

glm::ivec2 InputManager::get_mouse_position(bool pad_window) const
{
	if (pad_window)
	{
		std::shared_lock lock(m_pad_mouse.m_mutex);
		return m_pad_mouse.position;
	}
	else
	{
		std::shared_lock lock(m_main_mouse.m_mutex);
		return m_main_mouse.position;
	}
}

std::optional<glm::ivec2> InputManager::get_left_down_mouse_info(bool* is_pad)
{
	if (is_pad)
		*is_pad = false;

	{
		std::unique_lock lock(m_main_mouse.m_mutex);
		if (std::exchange(m_main_mouse.left_down_toggle, false))
			return m_main_mouse.position;

		if (m_main_mouse.left_down)
			return m_main_mouse.position;
	}

	{
		std::unique_lock lock(m_main_touch.m_mutex);
		if (std::exchange(m_main_touch.left_down_toggle, false))
			return m_main_touch.position;

		if (m_main_touch.left_down)
			return m_main_touch.position;
	}

	if (is_pad)
		*is_pad = true;

	{
		std::unique_lock lock(m_pad_mouse.m_mutex);
		if (std::exchange(m_pad_mouse.left_down_toggle, false))
			return m_pad_mouse.position;

		if (m_pad_mouse.left_down)
			return m_pad_mouse.position;
	}

	{
		std::unique_lock lock(m_pad_touch.m_mutex);
		if (std::exchange(m_pad_touch.left_down_toggle, false))
			return m_pad_touch.position;

		if (m_pad_touch.left_down)
			return m_pad_touch.position;
	}

	return {};
}

std::optional<glm::ivec2> InputManager::get_right_down_mouse_info(bool* is_pad)
{
	if (is_pad)
		*is_pad = false;

	{
		std::unique_lock lock(m_main_mouse.m_mutex);
		if (std::exchange(m_main_mouse.right_down_toggle, false))
			return m_main_mouse.position;

		if (m_main_mouse.right_down)
			return m_main_mouse.position;
	}

	{
		std::unique_lock lock(m_main_touch.m_mutex);
		if (std::exchange(m_main_touch.right_down_toggle, false))
			return m_main_touch.position;

		if (m_main_touch.right_down)
			return m_main_touch.position;
	}

	if (is_pad)
		*is_pad = true;

	{
		std::unique_lock lock(m_pad_mouse.m_mutex);
		if (std::exchange(m_pad_mouse.right_down_toggle, false))
			return m_pad_mouse.position;

		if (m_pad_mouse.right_down)
			return m_pad_mouse.position;
	}

	{
		std::unique_lock lock(m_pad_touch.m_mutex);
		if (std::exchange(m_pad_touch.right_down_toggle, false))
			return m_pad_touch.position;

		if (m_pad_touch.right_down)
			return m_pad_touch.position;
	}

	return {};
}

void InputManager::update_thread()
{
	SetThreadName("Input_update");
	while (!m_update_thread_shutdown.load(std::memory_order::relaxed))
	{
		std::shared_lock lock(m_mutex);
		for (auto& pad : m_vpad)
		{
			if (pad)
				pad->update();
		}

		for (auto& pad : m_wpad)
		{
			if (pad)
				pad->update();
		}
		lock.unlock();

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		std::this_thread::yield();
	}
}

void InputManager::Shutdown()
{
    m_update_thread_shutdown = true;

    if (m_update_thread.joinable())
    {
        m_update_thread.join();
    }

    for (auto& pad : m_vpad) pad.reset();
	for (auto& pad : m_wpad) pad.reset();
#if defined(CEMU_UWP)
	for (auto& controller : m_uwp_pre_game_controllers)
		controller.reset();
	m_uwp_pre_game_controller_captured.fill(false);
#endif

	for (auto& providers : m_api_available)
	{
		providers.clear();
	}
}
