// two-stage version: producer -> ring A -> forwarder (TransferAvailable) -> ring B -> consumer runs ops
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12RecordRing.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdio>
#include <random>
#include <string>

struct Pipeline
{
	D3D12RecordRing a{ 1 << 16 }, b{ 1 << 15 };
	std::mutex m;
	std::condition_variable workA, workB, stateCv;
	std::atomic<bool> sleepA{ false }, sleepB{ false }, stop{ false };
	std::atomic<int> waiters{ 0 };
	std::exception_ptr error;
	uint64_t ran = 0, moved = 0, producerWaits = 0, forwarderWaits = 0;

	void NotifyState() { if (waiters.load(std::memory_order_seq_cst)) { std::scoped_lock l(m); stateCv.notify_all(); } }
	bool Idle() { return a.IsEmpty() && b.IsEmpty(); }

	template<typename Fill, typename F>
	void Push(size_t dataBytes, Fill&& fill, F&& op)
	{
		const size_t bytes = D3D12RecordRing::EntryBytes<std::decay_t<F>>(dataBytes);
		if (!a.CanPush(bytes))
		{
			std::unique_lock l(m);
			waiters++;
			stateCv.wait(l, [&] { return a.CanPush(bytes); });
			waiters--;
			producerWaits++;
		}
		{
			std::scoped_lock l(m);
			if (error) { auto e = error; error = nullptr; std::rethrow_exception(e); }
		}
		a.Push(dataBytes, std::forward<Fill>(fill), std::forward<F>(op));
		if (sleepA.load(std::memory_order_seq_cst)) { std::scoped_lock l(m); workA.notify_one(); }
	}
	void Drain()
	{
		std::unique_lock l(m);
		waiters++;
		stateCv.wait(l, [&] { return Idle(); });
		waiters--;
	}
	void Forward()
	{
		while (true)
		{
			if (!a.HasWork())
			{
				std::unique_lock l(m);
				sleepA.store(true, std::memory_order_seq_cst);
				workA.wait(l, [&] { return stop.load() || a.HasWork(); });
				sleepA.store(false);
				if (!a.HasWork() && stop.load())
					return;
			}
			moved += a.TransferAvailable(b,
				[&](size_t bytes) {
					if (sleepB.load(std::memory_order_seq_cst)) { std::scoped_lock l(m); workB.notify_one(); }
					std::unique_lock l(m);
					waiters++;
					stateCv.wait(l, [&] { return b.CanPush(bytes); });
					waiters--;
					forwarderWaits++;
				},
				[&] {
					if (sleepB.load(std::memory_order_seq_cst)) { std::scoped_lock l(m); workB.notify_one(); }
					NotifyState();
				});
		}
	}
	void Consume()
	{
		while (true)
		{
			if (!b.HasWork())
			{
				std::unique_lock l(m);
				sleepB.store(true, std::memory_order_seq_cst);
				workB.wait(l, [&] { return stop.load() || b.HasWork(); });
				sleepB.store(false);
				if (!b.HasWork() && stop.load())
					return;
			}
			ran += b.RunAvailable([&](std::exception_ptr e) { std::scoped_lock l(m); error = e; }, [&] { NotifyState(); });
		}
	}
};

int main()
{
	Pipeline p;
	std::thread forwarder([&] { p.Forward(); });
	std::thread consumer([&] { p.Consume(); });
	std::mt19937 rng(7);
	std::atomic<uint64_t> lastSeq{ 0 };
	bool orderOk = true, dataOk = true;
	uint64_t checksum = 0, expectedChecksum = 0;
	std::atomic<int> live{ 0 };
	struct Tracker { std::atomic<int>* live; Tracker(std::atomic<int>* l) : live(l) { ++*live; } Tracker(Tracker&& o) noexcept : live(o.live) { ++*live; } ~Tracker() { --*live; } };
	const int N = 2'000'000;
	for (int i = 0; i < N; ++i)
	{
		const uint64_t seq = (uint64_t)i + 1;
		size_t dataBytes = (rng() % 4 == 0) ? 0 : (rng() % 3000);
		if (i % 100000 == 7) dataBytes = 15000; // under half of ring B
		const uint8_t pattern = (uint8_t)(seq * 31);
		std::string capture(rng() % 40, 'x');
		expectedChecksum += dataBytes * pattern + capture.size();
		p.Push(dataBytes, [&](uint8_t* d) { memset(d, pattern, dataBytes); },
			[&, seq, dataBytes, pattern, capture, t = Tracker(&live)](const uint8_t* d) {
				if (lastSeq.load() + 1 != seq) orderOk = false;
				lastSeq.store(seq);
				uint64_t sum = 0;
				for (size_t k = 0; k < dataBytes; ++k) { if (d[k] != pattern) dataOk = false; sum += d[k]; }
				if (dataBytes == 0 && d) dataOk = false;
				checksum += sum + capture.size();
			});
		if (i % 250000 == 0) p.Drain();
	}
	p.Drain();
	bool sawError = false;
	p.Push(0, [](uint8_t*) {}, [](const uint8_t*) { throw std::runtime_error("boom"); });
	p.Drain();
	try { p.Push(0, [](uint8_t*) {}, [](const uint8_t*) {}); } catch (const std::runtime_error&) { sawError = true; }
	p.Drain();
	p.stop = true;
	{ std::scoped_lock l(p.m); p.workA.notify_all(); p.workB.notify_all(); }
	forwarder.join();
	consumer.join();
	printf("moved %llu, ran %llu (pushed %d+1), order %s, data %s, checksum %s, live %d, producer waits %llu, forwarder waits %llu, error seen %s\n",
		(unsigned long long)p.moved, (unsigned long long)p.ran, N, orderOk ? "ok" : "BAD", dataOk ? "ok" : "BAD",
		checksum == expectedChecksum ? "ok" : "BAD", live.load(), (unsigned long long)p.producerWaits, (unsigned long long)p.forwarderWaits, sawError ? "yes" : "NO");
	return (orderOk && dataOk && checksum == expectedChecksum && live.load() == 0 && sawError && p.moved == (uint64_t)N + 1) ? 0 : 1;
}
