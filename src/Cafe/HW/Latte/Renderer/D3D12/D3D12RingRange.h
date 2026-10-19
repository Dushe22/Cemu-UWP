#pragma once

#include <cstdint>

namespace D3D12RingRange
{
// alignment padding must not carry a wrapped cursor past oldestOffset
inline bool TryReserve(uint64_t capacity, uint64_t writeOffset, uint64_t size,
	uint64_t alignment, bool hasPending, uint64_t oldestOffset,
	bool currentFenceOwnsOldestRange, uint64_t& resultOffset)
{
	if (size == 0 || size > capacity || writeOffset > capacity || alignment == 0 ||
		(alignment & (alignment - 1)) != 0)
		return false;
	if (hasPending && (oldestOffset >= capacity || writeOffset == oldestOffset))
		return false; // equal live cursors mean full, not empty

	const uint64_t padding = (alignment - (writeOffset & (alignment - 1))) & (alignment - 1);
	const bool fitsTail = padding <= capacity - writeOffset &&
		size <= capacity - writeOffset - padding;
	if (hasPending && writeOffset < oldestOffset)
	{
		// wrapped cursor: only the gap before the oldest range is free
		if (padding > oldestOffset - writeOffset ||
			size > oldestOffset - writeOffset - padding)
			return false;
		resultOffset = writeOffset + padding;
		return true;
	}
	if (fitsTail)
	{
		resultOffset = writeOffset + padding;
		return true;
	}
	if (hasPending && (currentFenceOwnsOldestRange || size > oldestOffset))
		return false;
	resultOffset = 0;
	return true;
}
}
