#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

// Single-producer single-consumer ring of operations for the threaded draw recorder. Each entry holds a callable built
// in place (no heap allocation, no mutex per op) plus optional data bytes written by the producer, which the callable
// receives as a pointer that stays valid until it returns. Entries run in push order.
// Positions are monotonic byte counters; an entry never wraps (a skip marker fills the end of the buffer instead).
class D3D12RecordRing
{
public:
	static constexpr size_t kAlign = 64;

	explicit D3D12RecordRing(size_t capacity)
		: m_capacity(capacity), m_buffer(static_cast<uint8_t*>(::operator new(capacity, std::align_val_t{ kAlign })))
	{
		if (capacity == 0 || (capacity % kAlign) != 0)
			throw std::invalid_argument("D3D12RecordRing capacity must be a non-zero multiple of 64");
	}
	~D3D12RecordRing()
	{
		// destroy entries nobody ran (shutdown after an error)
		uint64_t tail = m_tail.load(std::memory_order_relaxed);
		const uint64_t head = m_head.load(std::memory_order_relaxed);
		while (tail != head)
		{
			Header* header = HeaderAt(tail);
			if (header->destroy)
				header->destroy(Payload(header));
			tail += header->size;
		}
		::operator delete(m_buffer, std::align_val_t{ kAlign });
	}
	D3D12RecordRing(const D3D12RecordRing&) = delete;
	D3D12RecordRing& operator=(const D3D12RecordRing&) = delete;

	size_t Capacity() const { return m_capacity; }

	// producer: bytes an entry for F with dataBytes of data occupies (round up to the alignment)
	template<typename F>
	static size_t EntryBytes(size_t dataBytes)
	{
		return AlignUp(PayloadOffset() + AlignUp(sizeof(F), alignof(std::max_align_t)) + dataBytes, kAlign);
	}

	// producer: true if an entry of this size fits now (including a skip to the start of the buffer)
	bool CanPush(size_t entryBytes) const
	{
		if (entryBytes > m_capacity / 2)
			throw std::length_error("D3D12RecordRing entry larger than half the ring");
		const uint64_t head = m_head.load(std::memory_order_relaxed);
		const size_t needed = Needed(head, entryBytes);
		// the consumer writes m_tail constantly from another core: reading it on every push cost a cache miss per op,
		// so the producer works from its last copy (older = less space, never too much) and reloads only when short
		if (needed <= m_capacity - (size_t)(head - m_producerTail))
			return true;
		m_producerTail = m_tail.load(std::memory_order_acquire);
		return needed <= m_capacity - (size_t)(head - m_producerTail);
	}

	// producer: builds the entry; fill(uint8_t* data) writes the data bytes, then op is moved in. op is called as
	// op(const uint8_t* data) (data is nullptr when dataBytes is 0). The caller must have checked CanPush.
	template<typename Fill, typename F>
	void Push(size_t dataBytes, Fill&& fill, F&& op)
	{
		using Op = std::decay_t<F>;
		static_assert(alignof(Op) <= alignof(std::max_align_t), "over-aligned recording op");
		const size_t entryBytes = EntryBytes<Op>(dataBytes);
		const uint64_t head = ReserveHead(entryBytes);
		Header* header = HeaderAt(head);
		const size_t opBytes = AlignUp(sizeof(Op), alignof(std::max_align_t));
		uint8_t* payload = Payload(header);
		uint8_t* data = dataBytes ? payload + opBytes : nullptr;
		if (data)
			fill(data);
		new (payload) Op(std::forward<F>(op));
		header->run = [](uint8_t* p, const uint8_t* d) { (*reinterpret_cast<Op*>(p))(d); };
		header->destroy = [](uint8_t* p) { reinterpret_cast<Op*>(p)->~Op(); };
		header->relocate = [](uint8_t* from, uint8_t* to) { Op* op = reinterpret_cast<Op*>(from); new (to) Op(std::move(*op)); op->~Op(); };
		header->size = (uint32_t)entryBytes;
		header->dataOffset = dataBytes ? (uint32_t)opBytes : 0;
		// seq_cst pairs with the consumer's sleep check (see HasWork / the sleeping flag in the user)
		m_head.store(head + entryBytes, std::memory_order_seq_cst);
		PrefetchAhead(head + entryBytes, entryBytes);
	}

	// consumer
	bool HasWork() const { return m_tail.load(std::memory_order_relaxed) != m_head.load(std::memory_order_seq_cst); }
	bool IsEmpty() const { return m_tail.load(std::memory_order_seq_cst) == m_head.load(std::memory_order_seq_cst); }

	// consumer: runs every entry published when called; onError(exception) is called for an op that throws, which stops
	// the batch (later entries are destroyed without running, like the old queue dropped the rest of a batch).
	// afterEach() runs after each entry is released (lets the user wake a producer waiting for space). Returns ops run.
	template<typename OnError, typename AfterEach>
	size_t RunAvailable(OnError&& onError, AfterEach&& afterEach)
	{
		const uint64_t head = m_head.load(std::memory_order_acquire);
		uint64_t tail = m_tail.load(std::memory_order_relaxed);
		size_t ran = 0;
		bool failed = false;
		while (tail != head)
		{
			Header* header = HeaderAt(tail);
			if (header->run)
			{
				uint8_t* payload = Payload(header);
				if (!failed)
				{
					try
					{
						header->run(payload, header->dataOffset ? payload + header->dataOffset : nullptr);
						++ran;
					}
					catch (...)
					{
						failed = true;
						onError(std::current_exception());
					}
				}
				header->destroy(payload);
			}
			tail += header->size;
			m_tail.store(tail, std::memory_order_seq_cst);
			afterEach();
		}
		return ran;
	}

	// consumer of this ring and producer of dst: moves every published entry into dst in order (the op is moved, its
	// data copied) without running it. waitForSpace(entryBytes) is called while dst has no room and must return once it
	// has; afterEach() runs after each entry left this ring. Returns entries moved.
	template<typename WaitForSpace, typename AfterEach>
	size_t TransferAvailable(D3D12RecordRing& dst, WaitForSpace&& waitForSpace, AfterEach&& afterEach)
	{
		const uint64_t head = m_head.load(std::memory_order_acquire);
		uint64_t tail = m_tail.load(std::memory_order_relaxed);
		size_t moved = 0;
		while (tail != head)
		{
			Header* header = HeaderAt(tail);
			if (header->run)
			{
				while (!dst.CanPush(header->size))
					waitForSpace((size_t)header->size);
				dst.PushRelocated(header);
				++moved;
			}
			tail += header->size;
			m_tail.store(tail, std::memory_order_seq_cst);
			afterEach();
		}
		return moved;
	}

private:
	struct Header
	{
		void (*run)(uint8_t* payload, const uint8_t* data);
		void (*destroy)(uint8_t* payload);
		void (*relocate)(uint8_t* from, uint8_t* to); // move-constructs the op at to and destroys it at from
		uint32_t size;
		uint32_t dataOffset; // from the payload start; 0 = no data
	};
	static_assert(sizeof(Header) <= kAlign);

	// producer: position for an entry of entryBytes, writing a skip marker when it would cross the end of the buffer
	uint64_t ReserveHead(size_t entryBytes)
	{
		uint64_t head = m_head.load(std::memory_order_relaxed);
		const size_t offset = (size_t)(head % m_capacity);
		if (offset + entryBytes > m_capacity)
		{
			Header* skip = reinterpret_cast<Header*>(m_buffer + offset);
			skip->run = nullptr;
			skip->destroy = nullptr;
			skip->relocate = nullptr;
			skip->size = (uint32_t)(m_capacity - offset);
			skip->dataOffset = 0;
			head += m_capacity - offset;
		}
		return head;
	}

	// producer: same layout as source, so the data keeps its offset from the payload
	void PushRelocated(Header* source)
	{
		const size_t entryBytes = source->size;
		const uint64_t head = ReserveHead(entryBytes);
		Header* header = HeaderAt(head);
		uint8_t* sourcePayload = Payload(source);
		uint8_t* payload = Payload(header);
		source->relocate(sourcePayload, payload);
		if (source->dataOffset)
			std::memcpy(payload + source->dataOffset, sourcePayload + source->dataOffset, entryBytes - PayloadOffset() - source->dataOffset);
		header->run = source->run;
		header->destroy = source->destroy;
		header->relocate = source->relocate;
		header->size = source->size;
		header->dataOffset = source->dataOffset;
		m_head.store(head + entryBytes, std::memory_order_seq_cst);
		PrefetchAhead(head + entryBytes, entryBytes);
	}

	// a 16 MB ring is never in cache: each new line written cost a read from RAM first (Kakariko: ~0.7 us per draw of
	// ~1 KB). Each push asks for as many bytes as it used, kPrefetchDistance ahead, so those reads overlap other work
	static constexpr size_t kPrefetchDistance = 8 * 1024;
	void PrefetchAhead(uint64_t head, size_t bytes) const
	{
		const uint64_t start = (head + kPrefetchDistance) & ~(uint64_t)(kAlign - 1);
		for (uint64_t position = start; position < start + bytes; position += kAlign)
		{
			const uint8_t* line = m_buffer + (size_t)(position % m_capacity);
#if defined(_MSC_VER)
			_m_prefetchw(line);
#else
			__builtin_prefetch(line, 1);
#endif
		}
	}

	static size_t AlignUp(size_t value, size_t alignment) { return (value + alignment - 1) / alignment * alignment; }
	Header* HeaderAt(uint64_t position) const { return reinterpret_cast<Header*>(m_buffer + (size_t)(position % m_capacity)); }
	static constexpr size_t PayloadOffset() { return (sizeof(Header) + alignof(std::max_align_t) - 1) / alignof(std::max_align_t) * alignof(std::max_align_t); }
	static uint8_t* Payload(Header* header) { return reinterpret_cast<uint8_t*>(header) + PayloadOffset(); }
	size_t Needed(uint64_t head, size_t entryBytes) const
	{
		const size_t offset = (size_t)(head % m_capacity);
		return offset + entryBytes > m_capacity ? (m_capacity - offset) + entryBytes : entryBytes;
	}

	const size_t m_capacity;
	uint8_t* const m_buffer;
	alignas(64) std::atomic<uint64_t> m_head{ 0 }; // written by the producer
	mutable uint64_t m_producerTail = 0; // producer's copy of m_tail
	alignas(64) std::atomic<uint64_t> m_tail{ 0 }; // written by the consumer
};
