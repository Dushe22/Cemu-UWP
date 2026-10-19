#pragma once

#include "input/motion/MotionHandler.h"
#include "input/api/DSU/DSUMessages.h"

#include "input/api/ControllerProvider.h"
#include "util/helpers/ConcurrentQueue.h"

#include <boost/asio.hpp>

#if defined(CEMU_UWP)
#include <optional>
#include <variant>
#endif

#ifndef HAS_DSU
#define HAS_DSU 1
#endif

// #define DEBUG_DSU_CLIENT

struct DSUProviderSettings : public ControllerProviderSettings
{
	std::string ip;
	uint16 port;
#if defined(CEMU_UWP)
	std::string endpoint_id;
	std::string display_name;
	bool companion_transport = false;
#endif

	DSUProviderSettings() : ip("127.0.0.1"), port(26760) {}

	DSUProviderSettings(std::string ip, uint16 port)
		: ip(std::move(ip)), port(port)
	{
	}

#if defined(CEMU_UWP)
	DSUProviderSettings(std::string endpointId, std::string displayName,
		std::string ip, uint16 port, bool companionTransport)
		: ip(std::move(ip)), port(port), endpoint_id(std::move(endpointId)),
		  display_name(std::move(displayName)),
		  companion_transport(companionTransport)
	{
	}
#endif

	bool operator==(const DSUProviderSettings& s) const
	{
		if (port != s.port || ip != s.ip)
			return false;
#if defined(CEMU_UWP)
		return endpoint_id.empty() || s.endpoint_id.empty() ||
			endpoint_id == s.endpoint_id;
#else
		return true;
#endif
	}

	bool operator==(const ControllerProviderSettings& s) const override
	{
		const auto* ptr = dynamic_cast<const DSUProviderSettings*>(&s);
		return ptr && *this == *ptr;
	}
};


class DSUControllerProvider : public ControllerProvider<DSUProviderSettings>
{
	friend class DSUController;
	using base_type = ControllerProvider<DSUProviderSettings>;
public:
#if defined(CEMU_UWP)
	constexpr static int kMaxClients = 16;
#else
	constexpr static int kMaxClients = 8;
#endif

	struct ControllerState
	{
		// when was info updated last time
		std::chrono::steady_clock::time_point last_update{};
		uint64_t packet_index = 0; // our packet index count
#if defined(CEMU_UWP)
		uint32_t remote_packet_index = 0;
		bool has_remote_packet = false;
		std::chrono::steady_clock::time_point remote_packet_time{};
#endif

		PortInfoData info{};
		DataResponseData data{};
		MotionSample motion_sample{};

		ControllerState& operator=(const PortInfo& port_info);
		ControllerState& operator=(const DataResponse& data_response);
	};

	DSUControllerProvider();
	DSUControllerProvider(const DSUProviderSettings& settings);
	~DSUControllerProvider();

	inline static InputAPI::Type kAPIType = InputAPI::DSUClient;
	InputAPI::Type api() const override { return kAPIType; }

	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;

	bool connect();

	bool is_connected(uint8_t index);
	ControllerState get_state(uint8_t index) const;
	ControllerState get_prev_state(uint8_t index) const;
	MotionSample get_motion_sample(uint8_t index) const;

	std::array<bool, kMaxClients> wait_update(const std::array<uint64_t, kMaxClients>& indices, size_t timeout) const;
	bool wait_update(uint8_t index, uint64_t packet_index, size_t timeout) const;

	uint64_t get_packet_index(uint8_t index) const;
	// refresh pad info for all pads
	void request_pad_info();
	// refresh pad info for pad with given index
	void request_pad_info(uint8_t index);
	void request_version();
	void request_pad_data();
	void request_pad_data(uint8_t index);

#if defined(CEMU_UWP)
	void send_extension_packet(std::vector<uint8> packet);
	void send_priority_extension_packet(std::vector<uint8> packet);
#endif

private:
	uint16 m_server_version = 0;

	std::atomic_bool m_running = false;
	std::thread m_reader_thread, m_writer_thread;

	void reader_thread();
	void writer_thread();
	void integrate_motion(uint8_t index, const DataResponse& data_response);

	uint32 m_uid;
	boost::asio::io_context m_io_service;
	boost::asio::ip::udp::endpoint m_receiver_endpoint;
	boost::asio::ip::udp::socket m_socket;
	boost::asio::ip::udp::endpoint m_socketWakeupEndpoint;

	std::array<ControllerState, kMaxClients> m_state{};
	std::array<ControllerState, kMaxClients> m_prev_state{};
	mutable std::array<std::mutex, kMaxClients> m_mutex;
	mutable std::array<std::condition_variable, kMaxClients> m_wait_cond;

#if defined(CEMU_UWP)
	using WriterJob = std::variant<std::monostate, std::unique_ptr<ClientMessage>, std::vector<uint8>>;
	ConcurrentQueue<WriterJob> m_writerJobs;
	std::mutex m_priorityWriterMutex;
	std::optional<std::vector<uint8>> m_priorityExtensionPacket;
#else
	ConcurrentQueue<std::unique_ptr<ClientMessage>> m_writerJobs;
#endif
	void queue_message(std::unique_ptr<ClientMessage> message);

	std::array<WiiUMotionHandler, kMaxClients> m_motion_handler;
	std::array<uint64, kMaxClients> m_last_motion_timestamp{};
#if defined(CEMU_UWP)
	std::array<std::chrono::steady_clock::time_point, kMaxClients>
		m_last_reconnect_request{};
#endif
};
