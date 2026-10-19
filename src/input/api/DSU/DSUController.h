#pragma once

#include "input/api/Controller.h"
#include "input/api/DSU/DSUControllerProvider.h"

#include <atomic>

class DSUController : public Controller<DSUControllerProvider>
{
public:
	DSUController(uint32 index);
	DSUController(uint32 index, const DSUProviderSettings& settings);
	
	std::string_view api_name() const override
	{
		static_assert(to_string(InputAPI::DSUClient) == "DSUController");
		return to_string(InputAPI::DSUClient);
	}
	InputAPI::Type api() const override { return InputAPI::DSUClient; }

	void save(pugi::xml_node& node) override;
	void load(const pugi::xml_node& node) override;

	bool connect() override;
	bool is_connected() override;
	void configure_route_features(bool gamepad, bool motion, bool touch) noexcept;
	void configure_runtime_features(bool gamepad, bool motion, bool touch) noexcept;
	void reset_runtime_features() noexcept;
#if defined(CEMU_UWP)
	bool rebind_provider(const DSUProviderSettings& settings) noexcept;
#endif
	[[nodiscard]] bool is_route_managed() const noexcept
	{
		return m_route_managed.load(std::memory_order_relaxed);
	}
	[[nodiscard]] uint32 remote_slot() const noexcept { return m_index; }
	[[nodiscard]] const DSUProviderSettings& provider_settings() const noexcept
	{
		return m_provider->get_settings();
	}
	
	bool has_motion() override
	{
		return m_motion_enabled.load(std::memory_order_relaxed);
	}
	MotionSample get_motion_sample() override;

	bool has_position() override;
	glm::vec2 get_position() override;
	glm::vec2 get_prev_position() override;
	PositionVisibility GetPositionVisibility() override;
#if defined(CEMU_UWP)
	[[nodiscard]] bool get_touch_position(uint32 index, glm::vec2& position);
#endif

	std::string get_button_name(uint64 button) const override;

protected:
	ControllerState raw_state() override;

private:
	uint32 m_index;
	std::atomic_bool m_gamepad_enabled{ true };
	std::atomic_bool m_motion_enabled{ true };
	std::atomic_bool m_touch_enabled{ true };
	std::atomic_bool m_route_managed{ false };
#if defined(CEMU_UWP)
	bool m_runtime_feature_override{};
	bool m_saved_use_motion{};
#endif
};

