#include "input/api/DSU/DSUControllerProvider.h"
#include "input/api/DSU/DSUController.h"

#if defined(CEMU_UWP)
#include "uwp/UwpCompanionService.h"
#endif

#if BOOST_OS_WINDOWS
#include <boost/asio/detail/socket_option.hpp>
#include <winsock2.h>
#elif BOOST_OS_LINUX || BOOST_OS_MACOS
#include <sys/time.h>
#include <sys/socket.h>
#endif

DSUControllerProvider::DSUControllerProvider()
	: base_type(), m_uid(rand()), m_socket(m_io_service)
{
	if (!connect())
	{
		throw std::runtime_error("dsu client can't open the udp connection");
	}

	m_running = true;
	m_reader_thread = std::thread(&DSUControllerProvider::reader_thread, this);
	m_writer_thread = std::thread(&DSUControllerProvider::writer_thread, this);
	request_version();
}

DSUControllerProvider::DSUControllerProvider(const DSUProviderSettings& settings)
	: base_type(settings), m_uid(rand()), m_socket(m_io_service)
{
	if (!connect())
	{
		throw std::runtime_error("dsu client can't open the udp connection");
	}

	m_running = true;
	m_reader_thread = std::thread(&DSUControllerProvider::reader_thread, this);
	m_writer_thread = std::thread(&DSUControllerProvider::writer_thread, this);
	request_version();
}

DSUControllerProvider::~DSUControllerProvider()
{
	if (m_running)
	{
		m_running = false;
		// wake up the reader thread by sending a packet to self
		if (m_socketWakeupEndpoint.port() != 0)
		{
			boost::asio::io_context io_context;
			boost::asio::ip::udp::socket socket(io_context);
			boost::system::error_code ec;
			socket.open(m_socketWakeupEndpoint.protocol(), ec);
			if (!ec)
			{
				std::array<char, 1> data{};
				socket.send_to(boost::asio::buffer(data), m_socketWakeupEndpoint, 0, ec);
			}
			else
				cemuLog_log(LogType::Force, "DSUControllerProvider wakeup failed");
		}
		m_reader_thread.join();
#if defined(CEMU_UWP)
		m_writerJobs.push(WriterJob{}); // monostate wakes the writer during shutdown
#else
		m_writerJobs.push(nullptr); // wake up writer thread by pushing an empty message
#endif
		m_writer_thread.join();
	}
}

std::vector<std::shared_ptr<ControllerBase>> DSUControllerProvider::get_controllers()
{
	std::vector<ControllerPtr> result;

#if defined(CEMU_UWP)
	request_pad_info();
	for (auto i = 0; i < kMaxClients; ++i)
	{
		if (is_connected(i))
			result.emplace_back(std::make_shared<DSUController>(i, m_settings));
	}
#else
	std::array<uint64_t, kMaxClients> indices;
	for (auto i = 0; i < kMaxClients; ++i)
		indices[i] = get_packet_index(i);

	request_pad_info();

	const auto controller_result = wait_update(indices, 3000);
	for (auto i = 0; i < kMaxClients; ++i)
	{
		if (controller_result[i] && is_connected(i))
			result.emplace_back(std::make_shared<DSUController>(i, m_settings));
	}
#endif

	return result;
}

bool DSUControllerProvider::connect()
{
	// already connected?
	if (m_receiver_endpoint.address().to_string() == get_settings().ip && m_receiver_endpoint.port() == get_settings().port)
		return true;

	try
	{
		using namespace boost::asio;

		ip::udp::resolver resolver(m_io_service);
		m_receiver_endpoint = *resolver.resolve(get_settings().ip, fmt::format("{}", get_settings().port)).cbegin();

		if (m_socket.is_open())
			m_socket.close();

		m_socket.open(ip::udp::v4());
		m_socket.bind(ip::udp::endpoint(ip::udp::v4(), 0));
		m_socketWakeupEndpoint = ip::udp::endpoint(boost::asio::ip::address_v4::loopback(), m_socket.local_endpoint().port());
		// reset data
		m_state = {};
		m_prev_state = {};

		// restart threads
		return true;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "dsu client connect error: {}", ex.what());
		return false;
	}
}

bool DSUControllerProvider::is_connected(uint8_t index)
{
	if (index >= kMaxClients)
		return false;

#if defined(CEMU_UWP)
	constexpr auto kStaleTimeout = std::chrono::seconds(5);
	constexpr auto kRetryInterval = std::chrono::seconds(1);
	const auto now = std::chrono::steady_clock::now();
	bool retry = false;
	bool connected = false;
	{
		std::scoped_lock lock(m_mutex[index]);
		connected = m_state[index].info.state == DsState::Connected &&
			m_state[index].last_update != std::chrono::steady_clock::time_point{} &&
			now - m_state[index].last_update <= kStaleTimeout;
		if (!connected && (m_last_reconnect_request[index] ==
			std::chrono::steady_clock::time_point{} ||
			now - m_last_reconnect_request[index] >= kRetryInterval))
		{
			m_last_reconnect_request[index] = now;
			retry = true;
		}
	}
	if (retry)
	{
		request_pad_info(index);
		request_pad_data(index);
	}
	return connected;
#else
	std::scoped_lock lock(m_mutex[index]);
	return m_state[index].info.state == DsState::Connected;
#endif
}

DSUControllerProvider::ControllerState DSUControllerProvider::get_state(uint8_t index) const
{
	if (index >= kMaxClients)
		return {};

	std::scoped_lock lock(m_mutex[index]);
	return m_state[index];
}

DSUControllerProvider::ControllerState DSUControllerProvider::get_prev_state(uint8_t index) const
{
	if (index >= kMaxClients)
		return {};

	std::scoped_lock lock(m_mutex[index]);
	return m_prev_state[index];
}

std::array<bool, DSUControllerProvider::kMaxClients> DSUControllerProvider::wait_update(
	const std::array<uint64_t, kMaxClients>& indices, size_t timeout) const
{
	std::array<bool, kMaxClients> result{};

	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
	do
	{
		for (int i = 0; i < kMaxClients; ++i)
		{
			if (result[i])
				continue;

			std::unique_lock lock(m_mutex[i]);
			result[i] = indices[i] < m_state[i].packet_index;
		}

		if (std::all_of(result.cbegin(), result.cend(), [](const bool& v) { return v == true; }))
			break;

		//std::this_thread::sleep_for(std::chrono::milliseconds(1));
		std::this_thread::yield();
	}
	while (std::chrono::steady_clock::now() < end);

	return result;
}

bool DSUControllerProvider::wait_update(uint8_t index, uint64_t packet_index, size_t timeout) const
{
	if (index >= kMaxClients)
		return false;

	std::unique_lock lock(m_mutex[index]);

	if (packet_index < m_state[index].packet_index)
		return true;

	const auto result = m_wait_cond[index].wait_for(lock, std::chrono::milliseconds(timeout),
	                                                [this, index, packet_index]()
	                                                {
		                                                return packet_index < m_state[index].packet_index;
	                                                });

	return result;
}

uint64_t DSUControllerProvider::get_packet_index(uint8_t index) const
{
	if (index >= kMaxClients)
		return 0;
	std::scoped_lock lock(m_mutex[index]);
	return m_state[index].packet_index;
}

void DSUControllerProvider::request_version()
{
	queue_message(std::make_unique<VersionRequest>(m_uid));
}

void DSUControllerProvider::request_pad_info()
{
	#if defined(CEMU_UWP)
	for (uint8_t first = 0; first < kMaxClients; first += 4)
	{
		queue_message(std::make_unique<ListPorts>(m_uid, 4,
			std::array<uint8_t, 4>{ first, static_cast<uint8_t>(first + 1),
				static_cast<uint8_t>(first + 2), static_cast<uint8_t>(first + 3) }));
	}
	#else
	queue_message(std::make_unique<ListPorts>(m_uid, 4, std::array<uint8_t, 4>{0, 1, 2, 3}));
	#endif
}

void DSUControllerProvider::request_pad_info(uint8_t index)
{
	if (index >= kMaxClients)
		return;

	queue_message(std::make_unique<ListPorts>(m_uid, 1, std::array<uint8_t, 4>{index}));
}

void DSUControllerProvider::request_pad_data()
{
	queue_message(std::make_unique<DataRequest>(m_uid));
}

void DSUControllerProvider::request_pad_data(uint8_t index)
{
	if (index >= kMaxClients)
		return;

	queue_message(std::make_unique<DataRequest>(m_uid, index));
}

void DSUControllerProvider::queue_message(std::unique_ptr<ClientMessage> message)
{
#if defined(CEMU_UWP)
	m_writerJobs.push(WriterJob{ std::move(message) });
#else
	m_writerJobs.push(std::move(message));
#endif
}

#if defined(CEMU_UWP)
void DSUControllerProvider::send_extension_packet(std::vector<uint8> packet)
{
	if (!packet.empty() && m_running.load(std::memory_order_relaxed))
		m_writerJobs.push(WriterJob{ std::move(packet) });
}

void DSUControllerProvider::send_priority_extension_packet(std::vector<uint8> packet)
{
	if (packet.empty() || !m_running.load(std::memory_order_relaxed))
		return;

	bool wakeWriter = false;
	{
		std::lock_guard lock(m_priorityWriterMutex);
		wakeWriter = !m_priorityExtensionPacket.has_value();
		m_priorityExtensionPacket = std::move(packet);
	}
	if (wakeWriter)
		m_writerJobs.push(WriterJob{});
}
#endif

MotionSample DSUControllerProvider::get_motion_sample(uint8_t index) const
{
	if (index >= kMaxClients)
		return MotionSample();
	std::scoped_lock lock(m_mutex[index]);
	return m_state[index].motion_sample;
}


void DSUControllerProvider::reader_thread()
{
	SetThreadName("DSU-reader");
	bool first_read = true;
	while (m_running.load(std::memory_order_relaxed))
	{
		ServerMessage* msg;
		//try
		//{
#if defined(CEMU_UWP)
		std::array<char, 4096> recv_buf; // extended requests exceed classic DSU packets
#else
		std::array<char, 100> recv_buf; // NOLINT(cppcoreguidelines-pro-type-member-init, hicpp-member-init)
#endif
		boost::asio::ip::udp::endpoint sender_endpoint;
		boost::system::error_code ec{};
		const size_t len = m_socket.receive_from(boost::asio::buffer(recv_buf), sender_endpoint, 0, ec);
		if (!m_running.load(std::memory_order_relaxed))
			break;
		if (ec)
		{

#ifdef DEBUG_DSU_CLIENT
				printf(" DSUControllerProvider::ReaderThread: exception %s\n", ec.what());
#endif

			// there's probably no server listening on the given address:port
			if (first_read) // workaroud: first read always fails?
				first_read = false;
			else
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				std::this_thread::yield();
			}
			continue;
		}

#if defined(CEMU_UWP)
		if (sender_endpoint.address() != m_receiver_endpoint.address() ||
			sender_endpoint.port() != m_receiver_endpoint.port())
		{
			continue;
		}
#endif

#ifdef DEBUG_DSU_CLIENT
			printf(" DSUControllerProvider::ReaderThread: received message with len: 0x%llx\n", len);
#endif

		if (len < sizeof(ServerMessage)) // cant be a valid message
			continue;

#if defined(CEMU_UWP)
		std::vector<std::vector<uint8>> extensionResponses;
		if (CemuUwp::CompanionService::HandleExtensionPacket(
				reinterpret_cast<const uint8*>(recv_buf.data()), len, extensionResponses))
		{
			for (auto& response : extensionResponses)
				send_extension_packet(std::move(response));
			continue;
		}
#endif

		msg = (ServerMessage*)recv_buf.data();
		//		}
		//		catch (const std::exception&)
		//		{
		//#ifdef DEBUG_DSU_CLIENT
		//			printf(" DSUControllerProvider::ReaderThread: exception %s\n", ex.what());
		//#endif
		//
		//			// there's probably no server listening on the given address:port
		//			if (first_read) // workaroud: first read always fails?
		//				first_read = false;
		//			else
		//			{
		//				std::this_thread::sleep_for(std::chrono::milliseconds(250));
		//				std::this_thread::yield();
		//			}
		//			continue;
		//		}

		uint8_t index = 0xFF;
		switch (msg->GetMessageType())
		{
		case MessageType::Version:
			{
				const auto rsp = (VersionResponse*)msg;
				if (!rsp->IsValid())
				{
#ifdef DEBUG_DSU_CLIENT
				printf(" DSUControllerProvider::ReaderThread: VersionResponse is invalid!\n");
#endif
					continue;
				}

#ifdef DEBUG_DSU_CLIENT
			printf(" DSUControllerProvider::ReaderThread: server version is: 0x%x\n", rsp->GetVersion());
#endif

				m_server_version = rsp->GetVersion();
				// wdc
				break;
			}
		case MessageType::Information:
			{
				const auto info = (PortInfo*)msg;
				if (!info->IsValid())
				{
#ifdef DEBUG_DSU_CLIENT
				printf(" DSUControllerProvider::ReaderThread: PortInfo is invalid!\n");
#endif
					continue;
				}

					index = info->GetIndex();
					if (index >= kMaxClients)
						continue;
#ifdef DEBUG_DSU_CLIENT
			printf(" DSUControllerProvider::ReaderThread: received PortInfo for index %d\n", index);
#endif

				auto& mutex = m_mutex[index];
				std::scoped_lock lock(mutex);
				m_prev_state[index] = m_state[index];
				m_state[index] = *info;
#if defined(CEMU_UWP)
				if (info->GetState() != DsState::Connected)
				{
					m_state[index].data = {};
					m_state[index].has_remote_packet = false;
					m_state[index].remote_packet_time = {};
					m_last_motion_timestamp[index] = 0;
					m_motion_handler[index] = {};
					m_state[index].motion_sample = {};
				}
#endif
				m_wait_cond[index].notify_all();
				break;
			}
		case MessageType::Data:
			{
				const auto rsp = (DataResponse*)msg;
				if (!rsp->IsValid())
				{
#ifdef DEBUG_DSU_CLIENT
				printf(" DSUControllerProvider::ReaderThread: DataResponse is invalid!\n");
#endif
					continue;
				}

					index = rsp->GetIndex();
					if (index >= kMaxClients)
						continue;
#ifdef DEBUG_DSU_CLIENT
			printf(" DSUControllerProvider::ReaderThread: received DataResponse for index %d\n", index);
#endif

				auto& mutex = m_mutex[index];
				std::scoped_lock lock(mutex);
#if defined(CEMU_UWP)
				const uint32_t remoteIndex = rsp->GetPacketIndex();
				if (m_state[index].has_remote_packet)
				{
					const auto age = std::chrono::steady_clock::now() -
						m_state[index].remote_packet_time;
					const bool newer = static_cast<int32_t>(remoteIndex -
						m_state[index].remote_packet_index) > 0;
					if (!newer && age <= std::chrono::seconds(5))
						break;
					if (age > std::chrono::seconds(5))
					{
						m_last_motion_timestamp[index] = 0;
						m_motion_handler[index] = {};
					}
				}
#endif
				m_prev_state[index] = m_state[index];
				m_state[index] = *rsp;
#if defined(CEMU_UWP)
				m_state[index].remote_packet_index = remoteIndex;
				m_state[index].has_remote_packet = true;
				m_state[index].remote_packet_time = std::chrono::steady_clock::now();
#endif
				m_wait_cond[index].notify_all();
				// update motion info immediately, guaranteeing that we dont drop packets
				integrate_motion(index, *rsp);
				break;
			}
		}

		if (index != 0xFF)
			request_pad_data(index);
	}
}

void DSUControllerProvider::writer_thread()
{
	SetThreadName("DSU-writer");
	while (m_running.load(std::memory_order_relaxed))
	{
#if defined(CEMU_UWP)
		WriterJob job = m_writerJobs.pop();
		if (!m_running.load(std::memory_order_relaxed))
			return;

		const void* data = nullptr;
		size_t size = 0;
		if (auto message = std::get_if<std::unique_ptr<ClientMessage>>(&job); message && *message)
		{
			data = message->get();
			size = (*message)->GetSize();
		}
		else if (auto packet = std::get_if<std::vector<uint8>>(&job); packet && !packet->empty())
		{
			data = packet->data();
			size = packet->size();
		}

		std::optional<std::vector<uint8>> priorityPacket;
		{
			std::lock_guard lock(m_priorityWriterMutex);
			priorityPacket = std::exchange(m_priorityExtensionPacket, std::nullopt);
		}
		if (priorityPacket)
		{
			try
			{
				m_socket.send_to(boost::asio::buffer(*priorityPacket), m_receiver_endpoint);
			}
			catch (const std::exception&)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
			}
		}
		if (!data || size == 0)
			continue;
#else
		std::unique_ptr<ClientMessage> msg = m_writerJobs.pop();
		if (!m_running.load(std::memory_order_relaxed))
			return;
		cemu_assert_debug(msg.get());
		const void* data = msg.get();
		const size_t size = msg->GetSize();
#endif
#ifdef DEBUG_DSU_CLIENT
		printf(" DSUControllerProvider::WriterThread: sending message (len: 0x%llx)\n", (unsigned long long)size);
#endif
		try
		{
			m_socket.send_to(boost::asio::buffer(data, size), m_receiver_endpoint);
		}
		catch (const std::exception& ec)
		{
#ifdef DEBUG_DSU_CLIENT
			printf(" DSUControllerProvider::WriterThread: exception %s\n", ec.what());
#else
			(void)ec;
#endif
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		}
	}
}

void DSUControllerProvider::integrate_motion(uint8_t index, const DataResponse& data_response)
{
	const uint64 ts = data_response.GetMotionTimestamp();
	if (ts <= m_last_motion_timestamp[index])
	{
		const uint64 dif = m_last_motion_timestamp[index] - ts;
		if (dif >= 10000000) // timestamp more than 10 seconds in the past, a controller reset probably happened
			m_last_motion_timestamp[index] = 0;
		return;
	}

#if defined(CEMU_UWP)
	const uint64 elapsedTime = m_last_motion_timestamp[index] == 0 ?
		10000 :
		std::min<uint64>(ts - m_last_motion_timestamp[index], 100000);
#else
	const uint64 elapsedTime = ts - m_last_motion_timestamp[index];
#endif
	m_last_motion_timestamp[index] = ts;
	const double elapsedTimeD = (double)elapsedTime / 1000000.0;
	const auto& acc = data_response.GetAcceleration();
	const auto& gyro = data_response.GetGyro();

	m_motion_handler[index].processMotionSample((float)elapsedTimeD,
	                                            gyro.x * 0.0174533f,
	                                            gyro.y * 0.0174533f,
	                                            gyro.z * 0.0174533f,
	                                            acc.x,
	                                            -acc.y,
	                                            -acc.z);

	m_state[index].motion_sample = m_motion_handler[index].getMotionSample();
}

DSUControllerProvider::ControllerState& DSUControllerProvider::ControllerState::operator=(const PortInfo& port_info)
{
	info = port_info.GetInfo();
	last_update = std::chrono::steady_clock::now();
	packet_index++; // increase packet index for every packet we assign/recv
	return *this;
}

DSUControllerProvider::ControllerState& DSUControllerProvider::ControllerState::operator=(
	const DataResponse& data_response)
{
	this->operator=(static_cast<const PortInfo&>(data_response));
	data = data_response.GetData();
	return *this;
}
