#include "Cafe/HW/Latte/Renderer/D3D12/D3D12RecordRing.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdio>
#include <vector>
#include <random>
#include <string>

// mirrors the planned recorder protocol: consumer sleeps when empty, producer waits when full
struct Recorder
{
	D3D12RecordRing ring{ 1 << 16 };
	std::mutex m;
	std::condition_variable workCv, stateCv;
	std::atomic<bool> consumerSleeping{ false }, producerWaiting{ false }, stop{ false };
	std::exception_ptr error;
	uint64_t ran = 0, waits = 0;

	template<typename Fill, typename F>
	void Push(size_t dataBytes, Fill&& fill, F&& op)
	{
		const size_t bytes = D3D12RecordRing::EntryBytes<std::decay_t<F>>(dataBytes);
		if (!ring.CanPush(bytes))
		{
			std::unique_lock lock(m);
			producerWaiting.store(true, std::memory_order_seq_cst);
			stateCv.wait(lock, [&] { return ring.CanPush(bytes) || error; });
			producerWaiting.store(false);
			waits++;
		}
		{
			std::scoped_lock lock(m);
			if (error) { auto e = error; error = nullptr; std::rethrow_exception(e); }
		}
		ring.Push(dataBytes, std::forward<Fill>(fill), std::forward<F>(op));
		if (consumerSleeping.load(std::memory_order_seq_cst))
		{
			std::scoped_lock lock(m);
			workCv.notify_one();
		}
	}
	void Drain()
	{
		std::unique_lock lock(m);
		producerWaiting.store(true, std::memory_order_seq_cst);
		stateCv.wait(lock, [&] { return ring.IsEmpty(); });
		producerWaiting.store(false);
	}
	void Run()
	{
		while (true)
		{
			if (!ring.HasWork())
			{
				std::unique_lock lock(m);
				consumerSleeping.store(true, std::memory_order_seq_cst);
				workCv.wait(lock, [&] { return stop.load() || ring.HasWork(); });
				consumerSleeping.store(false);
				if (!ring.HasWork() && stop.load())
					return;
			}
			ran += ring.RunAvailable(
				[&](std::exception_ptr e) { std::scoped_lock lock(m); error = e; },
				[&] { if (producerWaiting.load(std::memory_order_seq_cst)) { std::scoped_lock lock(m); stateCv.notify_all(); } });
		}
	}
};

int main()
{
	Recorder r;
	std::thread consumer([&] { r.Run(); });
	std::mt19937 rng(42);
	uint64_t expected = 0, observed = 0, checksum = 0, expectedChecksum = 0;
	uint64_t nextSeq = 0;
	std::atomic<uint64_t> lastSeq{ 0 };
	bool orderOk = true, dataOk = true;
	int liveObjects = 0;
	std::atomic<int> live{ 0 };
	struct Tracker { std::atomic<int>* live; Tracker(std::atomic<int>* l) : live(l) { ++*live; } Tracker(Tracker&& o) noexcept : live(o.live) { ++*live; } ~Tracker() { --*live; } };
	const int N = 2'000'000;
	for (int i = 0; i < N; ++i)
	{
		const uint64_t seq = ++nextSeq;
		size_t dataBytes = (rng() % 4 == 0) ? 0 : (rng() % 3000);
		if (i % 100000 == 7) dataBytes = 20000; // big entries
		const uint8_t pattern = (uint8_t)(seq * 31);
		std::string capture(rng() % 40, 'x'); // non-trivial capture
		expectedChecksum += dataBytes * pattern + capture.size();
		r.Push(dataBytes, [&](uint8_t* d) { memset(d, pattern, dataBytes); },
			[&, seq, dataBytes, pattern, capture, t = Tracker(&live)](const uint8_t* d) {
				if (lastSeq.load() + 1 != seq) orderOk = false;
				lastSeq.store(seq);
				uint64_t sum = 0;
				for (size_t k = 0; k < dataBytes; ++k) { if (d[k] != pattern) dataOk = false; sum += d[k]; }
				if (dataBytes == 0 && d) dataOk = false;
				checksum += sum + capture.size();
			});
		if (i % 250000 == 0) r.Drain();
	}
	r.Drain();
	// error path: op throws, later ops of the batch are dropped, producer sees the error on its next push
	bool sawError = false;
	r.Push(0, [](uint8_t*) {}, [](const uint8_t*) { throw std::runtime_error("boom"); });
	r.Drain();
	try { r.Push(0, [](uint8_t*) {}, [](const uint8_t*) {}); } catch (const std::runtime_error&) { sawError = true; }
	r.Drain();
	r.stop = true;
	{ std::scoped_lock lock(r.m); r.workCv.notify_all(); }
	consumer.join();
	printf("ops ran %llu (pushed %d+1), order %s, data %s, checksum %s, live objects %d, producer full waits %llu, error seen %s\n",
		(unsigned long long)r.ran, N, orderOk ? "ok" : "BAD", dataOk ? "ok" : "BAD",
		checksum == expectedChecksum ? "ok" : "BAD", live.load(), (unsigned long long)r.waits, sawError ? "yes" : "NO");
	return (orderOk && dataOk && checksum == expectedChecksum && live.load() == 0 && sawError) ? 0 : 1;
}
