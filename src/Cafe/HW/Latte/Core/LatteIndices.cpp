#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Common/cpu_features.h"
#if defined(CEMU_UWP)
#include "Cafe/HW/Latte/Core/LattePrimitiveRestartPolicy.h"
#endif

#if defined(ARCH_X86_64) && defined(__GNUC__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

struct
{
	struct CacheEntry
	{
#if defined(CEMU_UWP)
		bool valid{};
#endif
		// input data
		const void* lastPtr;
		uint32 lastCount;
		LattePrimitiveMode lastPrimitiveMode;
		LatteIndexType lastIndexType;
#if defined(CEMU_UWP)
		RendererAPI lastRendererType;
		bool lastPrimitiveRestartEnabled;
		uint32 lastPrimitiveRestartIndex;
#endif
		uint64 lastUsed;
		// output
		uint32 indexMax;
		Renderer::INDEX_TYPE renderIndexType;
		uint32 outputCount;
		Renderer::IndexAllocation indexAllocation;
	};
	std::array<CacheEntry, 8> entry;
	uint64 currentUsageCounter{0};
}LatteIndexCache{};

void LatteIndices_invalidate(const void* memPtr, uint32 size)
{
#if defined(CEMU_UWP)
	if (!memPtr || size == 0)
		return;
	const uintptr_t rangeBegin = reinterpret_cast<uintptr_t>(memPtr);
	const uintptr_t rangeEnd = size <= std::numeric_limits<uintptr_t>::max() - rangeBegin ?
		rangeBegin + size : std::numeric_limits<uintptr_t>::max();
	for (auto& entry : LatteIndexCache.entry)
	{
		if (!entry.valid || !entry.lastPtr || entry.lastIndexType == LatteIndexType::AUTO)
			continue;
		const size_t indexSize =
			(entry.lastIndexType == LatteIndexType::U16_BE || entry.lastIndexType == LatteIndexType::U16_LE) ?
			sizeof(uint16) : sizeof(uint32);
		const uintptr_t entryBegin = reinterpret_cast<uintptr_t>(entry.lastPtr);
		const uint64 entryBytes = static_cast<uint64>(entry.lastCount) * indexSize;
		const uintptr_t entryEnd = entryBytes <= std::numeric_limits<uintptr_t>::max() - entryBegin ?
			entryBegin + static_cast<uintptr_t>(entryBytes) : std::numeric_limits<uintptr_t>::max();
		if (rangeBegin < entryEnd && entryBegin < rangeEnd)
		{
			g_renderer->indexData_releaseIndexMemory(entry.indexAllocation);
			entry.valid = false;
			entry.lastPtr = nullptr;
			entry.lastCount = 0;
		}
	}
#else
	for(auto& entry : LatteIndexCache.entry)
	{
		if (entry.lastPtr >= memPtr && (entry.lastPtr < ((uint8*)memPtr + size)) )
		{
			if(entry.lastPtr != nullptr)
				g_renderer->indexData_releaseIndexMemory(entry.indexAllocation);
			entry.lastPtr = nullptr;
			entry.lastCount = 0;
		}
	}
#endif
}

void LatteIndices_invalidateAll()
{
	for(auto& entry : LatteIndexCache.entry)
	{
#if defined(CEMU_UWP)
		if (entry.valid)
			g_renderer->indexData_releaseIndexMemory(entry.indexAllocation);
		entry.valid = false;
#else
		if (entry.lastPtr != nullptr)
			g_renderer->indexData_releaseIndexMemory(entry.indexAllocation);
#endif
		entry.lastPtr = nullptr;
		entry.lastCount = 0;
	}
}

uint64 LatteIndices_GetNextUsageIndex()
{
	return LatteIndexCache.currentUsageCounter++;
}

uint32 LatteIndices_calculateIndexOutputSize(LattePrimitiveMode primitiveMode, LatteIndexType indexType, uint32 count, bool segmentedLineLoop, bool forceU32Output)
{
	uint64 outputElementCount = count;
	if (primitiveMode == LattePrimitiveMode::QUADS)
	{
		outputElementCount = std::max<uint64>((uint64)(count / 4) * 6, 1);
	}
	else if (primitiveMode == LattePrimitiveMode::QUAD_STRIP)
	{
		const uint64 numQuads = count >= 4 ? (count - 2) / 2 : 0;
		outputElementCount = std::max<uint64>(numQuads * 6, 1);
	}
	else if (primitiveMode == LattePrimitiveMode::LINE_LOOP)
	{
		// every restart segment re-appends its first index, so count * 2 + 1 bounds it
		outputElementCount = segmentedLineLoop ? (uint64)count * 2 + 1 : (uint64)count + 1;
		outputElementCount = std::max<uint64>(outputElementCount, 1);
	}
	else if (primitiveMode == LattePrimitiveMode::TRIANGLE_FAN && (g_renderer->GetType() == RendererAPI::Metal || g_renderer->GetType() == RendererAPI::Direct3D12))
	{
#if defined(CEMU_UWP)
		// N fan vertices yield at most N-2 triangles; 3*N covers restart markers too
		outputElementCount = std::max<uint64>((uint64)count * 3, 1);
#else
		outputElementCount = std::max<uint64>(count, 1);
#endif
	}
	else if (indexType == LatteIndexType::AUTO)
		return 0;

	uint32 elementSize;
	if (forceU32Output)
		elementSize = sizeof(uint32);
	else if (indexType == LatteIndexType::AUTO)
		elementSize = count <= 0xFFFF ? sizeof(uint16) : sizeof(uint32);
	else if (indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE)
		elementSize = sizeof(uint16);
	else if (indexType == LatteIndexType::U32_BE || indexType == LatteIndexType::U32_LE)
		elementSize = sizeof(uint32);
	else
	{
		cemu_assert_suspicious();
		return 0;
	}

	const uint64 outputSize = outputElementCount * elementSize;
	if (outputSize > std::numeric_limits<uint32>::max())
	{
		cemu_assert_suspicious();
		return 0;
	}
	return (uint32)outputSize;
}

template<typename T>
void LatteIndices_convertBE(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	const betype<T>* src = (betype<T>*)indexDataInput;
	T* dst = (T*)indexDataOutput;
	for (uint32 i = 0; i < count; i++)
	{
		T v = *src;
		*dst = v;
		indexMax = std::max(indexMax, (uint32)v);
		dst++;
		src++;
	}
}

template<typename T>
void LatteIndices_convertLE(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	const T* src = (T*)indexDataInput;
	T* dst = (T*)indexDataOutput;
	for (uint32 i = 0; i < count; i++)
	{
		T v = *src;
		*dst = v;
		indexMax = std::max(indexMax, (uint32)v);
		dst++;
		src++;
	}
}

template<typename T, bool isBigEndian>
T LatteIndices_readIndex(const void* indexDataInput, uint32 index)
{
	if constexpr (isBigEndian)
		return static_cast<const betype<T>*>(indexDataInput)[index];
	else
		return static_cast<const T*>(indexDataInput)[index];
}

template<typename T, bool isBigEndian, typename SegmentCallback>
void LatteIndices_forEachSegment(const void* indexDataInput, uint32 count, bool primitiveRestartEnabled, T primitiveRestartIndex, SegmentCallback&& callback)
{
	uint32 segmentStart = 0;
	for (uint32 i = 0; i < count; ++i)
	{
		if (!primitiveRestartEnabled || LatteIndices_readIndex<T, isBigEndian>(indexDataInput, i) != primitiveRestartIndex)
			continue;
		if (i != segmentStart)
			callback(segmentStart, i - segmentStart);
		segmentStart = i + 1;
	}
	if (segmentStart != count)
		callback(segmentStart, count - segmentStart);
}

bool LatteIndices_isD3D12NativeStripMode(LattePrimitiveMode primitiveMode)
{
	return primitiveMode == LattePrimitiveMode::LINE_STRIP ||
		primitiveMode == LattePrimitiveMode::LINE_STRIP_ADJACENT ||
		primitiveMode == LattePrimitiveMode::TRIANGLE_STRIP ||
		primitiveMode == LattePrimitiveMode::TRIANGLE_STRIP_ADJACENT;
}

bool LatteIndices_isD3D12FixedRestartMode(LattePrimitiveMode primitiveMode)
{
	return LatteIndices_isD3D12NativeStripMode(primitiveMode) ||
		primitiveMode == LattePrimitiveMode::LINE_LOOP ||
		primitiveMode == LattePrimitiveMode::TRIANGLE_FAN;
}

template<typename T, bool isBigEndian, typename OutputT = T>
void LatteIndices_convertAndRemapRestart(const void* indexDataInput, void* indexDataOutput, uint32 count, T primitiveRestartIndex, uint32& indexMax)
{
	OutputT* dst = static_cast<OutputT*>(indexDataOutput);
	const OutputT hostRestartIndex = std::numeric_limits<OutputT>::max();
	for (uint32 i = 0; i < count; ++i)
	{
		const T idx = LatteIndices_readIndex<T, isBigEndian>(indexDataInput, i);
		if (idx == primitiveRestartIndex)
		{
			dst[i] = hostRestartIndex;
			continue;
		}
		dst[i] = (OutputT)idx;
		indexMax = std::max(indexMax, (uint32)idx);
	}
}

template<typename T, bool isBigEndian>
uint32 LatteIndices_unpackQuadsAndConvert(const void* indexDataInput, void* indexDataOutput, uint32 count, bool primitiveRestartEnabled, T primitiveRestartIndex, uint32& indexMax)
{
	T* dst = (T*)indexDataOutput;
	uint32 outputCount = 0;
	LatteIndices_forEachSegment<T, isBigEndian>(indexDataInput, count, primitiveRestartEnabled, primitiveRestartIndex, [&](uint32 segmentStart, uint32 segmentCount)
	{
		const uint32 numQuads = segmentCount / 4;
		for (uint32 i = 0; i < numQuads; ++i)
		{
			const uint32 srcIndex = segmentStart + i * 4;
			const T idx0 = LatteIndices_readIndex<T, isBigEndian>(indexDataInput, srcIndex + 0);
			const T idx1 = LatteIndices_readIndex<T, isBigEndian>(indexDataInput, srcIndex + 1);
			const T idx2 = LatteIndices_readIndex<T, isBigEndian>(indexDataInput, srcIndex + 2);
			const T idx3 = LatteIndices_readIndex<T, isBigEndian>(indexDataInput, srcIndex + 3);
			indexMax = std::max(indexMax, (uint32)idx0);
			indexMax = std::max(indexMax, (uint32)idx1);
			indexMax = std::max(indexMax, (uint32)idx2);
			indexMax = std::max(indexMax, (uint32)idx3);
			dst[outputCount + 0] = idx0;
			dst[outputCount + 1] = idx1;
			dst[outputCount + 2] = idx2;
			dst[outputCount + 3] = idx0;
			dst[outputCount + 4] = idx2;
			dst[outputCount + 5] = idx3;
			outputCount += 6;
		}
	});
	return outputCount;
}

template<typename T>
void LatteIndices_generateAutoQuadIndices(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	sint32 numQuads = count / 4;
	const betype<T>* src = (betype<T>*)indexDataInput;
	T* dst = (T*)indexDataOutput;
	for (sint32 i = 0; i < numQuads; i++)
	{
		T idx0 = i * 4 + 0;
		T idx1 = i * 4 + 1;
		T idx2 = i * 4 + 2;
		T idx3 = i * 4 + 3;
		dst[0] = idx0;
		dst[1] = idx1;
		dst[2] = idx2;
		dst[3] = idx0;
		dst[4] = idx2;
		dst[5] = idx3;
		src += 4;
		dst += 6;
	}
	indexMax = std::max(count, 1u) - 1;
}

template<typename T, bool isBigEndian>
uint32 LatteIndices_unpackQuadStripAndConvert(const void* indexDataInput, void* indexDataOutput, uint32 count, bool primitiveRestartEnabled, T primitiveRestartIndex, uint32& indexMax)
{
	T* dst = (T*)indexDataOutput;
	uint32 outputCount = 0;
	LatteIndices_forEachSegment<T, isBigEndian>(indexDataInput, count, primitiveRestartEnabled, primitiveRestartIndex, [&](uint32 segmentStart, uint32 segmentCount)
	{
		if (segmentCount < 4)
			return;
		const uint32 numQuads = (segmentCount - 2) / 2;
		for (uint32 i = 0; i < numQuads; ++i)
		{
			const uint32 srcIndex = segmentStart + i * 2;
			const T idx0 = LatteIndices_readIndex<T, isBigEndian>(indexDataInput, srcIndex + 0);
			const T idx1 = LatteIndices_readIndex<T, isBigEndian>(indexDataInput, srcIndex + 1);
			const T idx2 = LatteIndices_readIndex<T, isBigEndian>(indexDataInput, srcIndex + 2);
			const T idx3 = LatteIndices_readIndex<T, isBigEndian>(indexDataInput, srcIndex + 3);
			indexMax = std::max(indexMax, (uint32)idx0);
			indexMax = std::max(indexMax, (uint32)idx1);
			indexMax = std::max(indexMax, (uint32)idx2);
			indexMax = std::max(indexMax, (uint32)idx3);
			dst[outputCount + 0] = idx0;
			dst[outputCount + 1] = idx1;
			dst[outputCount + 2] = idx2;
			dst[outputCount + 3] = idx2;
			dst[outputCount + 4] = idx1;
			dst[outputCount + 5] = idx3;
			outputCount += 6;
		}
	});
	return outputCount;
}

template<typename T, bool isBigEndian, typename OutputT = T>
uint32 LatteIndices_unpackLineLoopAndConvert(const void* indexDataInput, void* indexDataOutput, uint32 count, bool primitiveRestartEnabled, T primitiveRestartIndex, uint32& indexMax)
{
	OutputT* dst = static_cast<OutputT*>(indexDataOutput);
	uint32 outputCount = 0;
	bool wroteSegment = false;
	LatteIndices_forEachSegment<T, isBigEndian>(indexDataInput, count, primitiveRestartEnabled, primitiveRestartIndex, [&](uint32 segmentStart, uint32 segmentCount)
	{
		if (wroteSegment)
			dst[outputCount++] = std::numeric_limits<OutputT>::max();
		const T firstIndex = LatteIndices_readIndex<T, isBigEndian>(indexDataInput, segmentStart);
		for (uint32 i = 0; i < segmentCount; ++i)
		{
			const T idx = LatteIndices_readIndex<T, isBigEndian>(indexDataInput, segmentStart + i);
			indexMax = std::max(indexMax, (uint32)idx);
			dst[outputCount++] = (OutputT)idx;
		}
		dst[outputCount++] = (OutputT)firstIndex;
		wroteSegment = true;
	});
	return outputCount;
}

template<typename T>
void LatteIndices_generateAutoQuadStripIndices(void* indexDataOutput, uint32 count, uint32& indexMax)
{
	if (count <= 3)
		return;
	sint32 numQuads = (count - 2) / 2;
	T* dst = (T*)indexDataOutput;
	for (sint32 i = 0; i < numQuads; i++)
	{
		T idx0 = i * 2 + 0;
		T idx1 = i * 2 + 1;
		T idx2 = i * 2 + 2;
		T idx3 = i * 2 + 3;
		dst[0] = idx0;
		dst[1] = idx1;
		dst[2] = idx2;
		dst[3] = idx2;
		dst[4] = idx1;
		dst[5] = idx3;
		dst += 6;
	}
	indexMax = std::max(count, 1u) - 1;
}


template<typename T>
void LatteIndices_generateAutoLineLoopIndices(void* indexDataOutput, uint32 count, uint32& indexMax)
{
	if (count == 0)
		return;
	T* dst = (T*)indexDataOutput;
	for (sint32 i = 0; i < (sint32)count; i++)
	{
		*dst = (T)i;
		dst++;
	}
	*dst = 0;
	dst++;
	indexMax = std::max(count, 1u) - 1;
}

template<typename T, bool isBigEndian, typename OutputT = T>
uint32 LatteIndices_unpackTriangleFanAndConvert(const void* indexDataInput, void* indexDataOutput, uint32 count, bool primitiveRestartEnabled, T primitiveRestartIndex, uint32& indexMax)
{
	OutputT* dst = static_cast<OutputT*>(indexDataOutput);
	uint32 outputCount = 0;
	bool wroteSegment = false;
	LatteIndices_forEachSegment<T, isBigEndian>(indexDataInput, count, primitiveRestartEnabled, primitiveRestartIndex, [&](uint32 segmentStart, uint32 segmentCount)
	{
		if (wroteSegment)
			dst[outputCount++] = std::numeric_limits<OutputT>::max();
		for (uint32 i = 0; i < segmentCount; ++i)
		{
			const uint32 sourceOffset = (i % 2) == 0 ? i / 2 : segmentCount - 1 - i / 2;
			const T idx = LatteIndices_readIndex<T, isBigEndian>(indexDataInput, segmentStart + sourceOffset);
			indexMax = std::max(indexMax, (uint32)idx);
			dst[outputCount++] = (OutputT)idx;
		}
		wroteSegment = true;
	});
	return outputCount;
}

template<typename T>
void LatteIndices_generateAutoTriangleFanIndices(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	const betype<T>* src = (betype<T>*)indexDataInput;
	T* dst = (T*)indexDataOutput;
	for (sint32 i = 0; i < count; i++)
	{
		T idx = i;
		if (idx % 2 == 0)
            idx = idx / 2;
        else
            idx = count - 1 - idx / 2;
		dst[i] = idx;
	}
	indexMax = std::max(count, 1u) - 1;
}

#if defined(CEMU_UWP)
template<typename T, bool isBigEndian, typename OutputT = T>
uint32 LatteIndices_unpackTriangleFanToList(const void* indexDataInput, void* indexDataOutput, uint32 count, bool primitiveRestartEnabled, T primitiveRestartIndex, uint32& indexMax)
{
	OutputT* dst = static_cast<OutputT*>(indexDataOutput);
	uint32 outputCount = 0;
	LatteIndices_forEachSegment<T, isBigEndian>(indexDataInput, count, primitiveRestartEnabled, primitiveRestartIndex, [&](uint32 segmentStart, uint32 segmentCount)
	{
		if (segmentCount < 3)
			return;
		const T centre = LatteIndices_readIndex<T, isBigEndian>(indexDataInput, segmentStart);
		indexMax = std::max(indexMax, (uint32)centre);
		for (uint32 i = 1; i + 1 < segmentCount; ++i)
		{
			const T second = LatteIndices_readIndex<T, isBigEndian>(indexDataInput, segmentStart + i);
			const T third = LatteIndices_readIndex<T, isBigEndian>(indexDataInput, segmentStart + i + 1);
			indexMax = std::max(indexMax, std::max((uint32)second, (uint32)third));
			dst[outputCount++] = (OutputT)centre;
			dst[outputCount++] = (OutputT)second;
			dst[outputCount++] = (OutputT)third;
		}
	});
	return outputCount;
}

template<typename T>
uint32 LatteIndices_generateAutoTriangleFanList(void* indexDataOutput, uint32 count, uint32& indexMax)
{
	T* dst = static_cast<T*>(indexDataOutput);
	uint32 outputCount = 0;
	for (uint32 i = 1; i + 1 < count; ++i)
	{
		dst[outputCount++] = 0;
		dst[outputCount++] = (T)i;
		dst[outputCount++] = (T)(i + 1);
	}
	indexMax = count == 0 ? 0 : count - 1;
	return outputCount;
}
#endif

#if defined(ARCH_X86_64)
ATTRIBUTE_AVX2
void LatteIndices_fastConvertU16_AVX2(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	// using AVX + AVX2 we can process 16 indices at a time
	const uint16* indicesU16BE = (const uint16*)indexDataInput;
	uint16* indexOutput = (uint16*)indexDataOutput;
	sint32 count16 = count >> 4;
	sint32 countRemaining = count & 15;
	if (count16)
	{
		__m256i mMin = _mm256_set_epi16((sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF,
						           		(sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF);
		__m256i mMax = _mm256_set_epi16(0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000);
		__m256i mShuffle16Swap = _mm256_set_epi8(30, 31, 28, 29, 26, 27, 24, 25, 22, 23, 20, 21, 18, 19, 16, 17, 14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);

		do
		{
			__m256i mIndexData = _mm256_loadu_si256((const __m256i*)indicesU16BE);
			indicesU16BE += 16;
			_mm_prefetch((const char*)indicesU16BE, _MM_HINT_T0);
			// endian swap
			mIndexData = _mm256_shuffle_epi8(mIndexData, mShuffle16Swap);
			_mm256_store_si256((__m256i*)indexOutput, mIndexData);
			mMax = _mm256_max_epu16(mIndexData, mMax);
			indexOutput += 16;
		} while (--count16);

		// fold 32 to 16 byte
		mMax = _mm256_max_epu16(mMax, _mm256_permute2x128_si256(mMax, mMax, 1));
		// fold 16 to 8 byte
		mMax = _mm256_max_epu16(mMax, _mm256_shuffle_epi32(mMax, (2 << 0) | (3 << 2) | (2 << 4) | (3 << 6)));

		uint16* mMaxU16 = (uint16*)&mMax;

		indexMax = std::max(indexMax, (uint32)mMaxU16[0]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[1]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[2]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[3]);
	}
	// process remaining indices
	uint32 _maxIndex = 0;
	for (sint32 i = countRemaining; (--i) >= 0;)
	{
		uint16 idx = _swapEndianU16(*indicesU16BE);
		*indexOutput = idx;
		indexOutput++;
		indicesU16BE++;
		_maxIndex = std::max(_maxIndex, (uint32)idx);
	}
	// update max
	indexMax = std::max(indexMax, _maxIndex);
}

ATTRIBUTE_SSE41
void LatteIndices_fastConvertU16_SSE41(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	// SSSE3 & SSE4.1 optimized decoding
	const uint16* indicesU16BE = (const uint16*)indexDataInput;
	uint16* indexOutput = (uint16*)indexDataOutput;
	sint32 count8 = count >> 3;
	sint32 countRemaining = count & 7;
	if (count8)
	{
		__m128i mMax = _mm_set_epi16(0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000);
		__m128i mTemp;
		__m128i* mRawIndices = (__m128i*)indicesU16BE;
		indicesU16BE += count8 * 8;
		__m128i* mOutputIndices = (__m128i*)indexOutput;
		indexOutput += count8 * 8;
		__m128i shufmask = _mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
		while (count8--)
		{
			mTemp = _mm_loadu_si128(mRawIndices);
			mRawIndices++;
			mTemp = _mm_shuffle_epi8(mTemp, shufmask);
			mMax = _mm_max_epu16(mMax, mTemp);
			_mm_store_si128(mOutputIndices, mTemp);
			mOutputIndices++;
		}

		uint16* mMaxU16 = (uint16*)&mMax;

		indexMax = std::max(indexMax, (uint32)mMaxU16[0]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[1]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[2]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[3]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[4]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[5]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[6]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[7]);
	}
	uint32 _maxIndex = 0;
	for (sint32 i = countRemaining; (--i) >= 0;)
	{
		uint16 idx = _swapEndianU16(*indicesU16BE);
		*indexOutput = idx;
		indexOutput++;
		indicesU16BE++;
		_maxIndex = std::max(_maxIndex, (uint32)idx);
	}
	indexMax = std::max(indexMax, _maxIndex);
}

ATTRIBUTE_AVX2
void LatteIndices_fastConvertU32_AVX2(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	// using AVX + AVX2 we can process 8 indices at a time
	const uint32* indicesU32BE = (const uint32*)indexDataInput;
	uint32* indexOutput = (uint32*)indexDataOutput;
	sint32 count8 = count >> 3;
	sint32 countRemaining = count & 7;
	if (count8)
	{
		__m256i mMax = _mm256_set_epi32(0, 0, 0, 0, 0, 0, 0, 0);
		__m256i mShuffle32Swap = _mm256_set_epi8(28,29,30,31,
			24,25,26,27,
			20,21,22,23,
			16,17,18,19,
			12,13,14,15,
			8,9,10,11,
			4,5,6,7,
			0,1,2,3);
		// unaligned
		do
		{
			__m256i mIndexData = _mm256_loadu_si256((const __m256i*)indicesU32BE);
			indicesU32BE += 8;
			_mm_prefetch((const char*)indicesU32BE, _MM_HINT_T0);
			// endian swap
			mIndexData = _mm256_shuffle_epi8(mIndexData, mShuffle32Swap);
			_mm256_store_si256((__m256i*)indexOutput, mIndexData);
			mMax = _mm256_max_epu32(mIndexData, mMax);
			indexOutput += 8;
		} while (--count8);

		// fold 32 to 16 byte
		mMax = _mm256_max_epu32(mMax, _mm256_permute2x128_si256(mMax, mMax, 1));
		// fold 16 to 8 byte
		mMax = _mm256_max_epu32(mMax, _mm256_shuffle_epi32(mMax, (2 << 0) | (3 << 2) | (2 << 4) | (3 << 6)));

		uint32* mMaxU32 = (uint32*)&mMax;
		indexMax = std::max(indexMax, (uint32)mMaxU32[0]);
		indexMax = std::max(indexMax, (uint32)mMaxU32[1]);
	}
	// process remaining indices
	uint32 _maxIndex = 0;
	for (sint32 i = countRemaining; (--i) >= 0;)
	{
		uint32 idx = _swapEndianU32(*indicesU32BE);
		*indexOutput = idx;
		indexOutput++;
		indicesU32BE++;
		_maxIndex = std::max(_maxIndex, (uint32)idx);
	}
	// update min/max
	indexMax = std::max(indexMax, _maxIndex);
}
#elif defined(__aarch64__)

void LatteIndices_fastConvertU16_NEON(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	const uint16* indicesU16BE = (const uint16*)indexDataInput;
	uint16* indexOutput = (uint16*)indexDataOutput;
	sint32 count8 = count >> 3;
	sint32 countRemaining = count & 7;

	if (count8)
	{
		uint16x8_t mMax = vdupq_n_u16(0x0000);
		uint16x8_t mTemp;
		uint16x8_t* mRawIndices = (uint16x8_t*) indicesU16BE;
		indicesU16BE += count8 * 8;
		uint16x8_t* mOutputIndices = (uint16x8_t*) indexOutput;
		indexOutput += count8 * 8;

		while (count8--)
		{
			mTemp = vld1q_u16((uint16*)mRawIndices);
			mRawIndices++;
			mTemp = vrev16q_u8(mTemp);
			mMax = vmaxq_u16(mMax, mTemp);
			vst1q_u16((uint16*)mOutputIndices, mTemp);
			mOutputIndices++;
		}

		uint16* mMaxU16 = (uint16*)&mMax;

		for (int i = 0; i < 8; ++i) {
			indexMax = std::max(indexMax, (uint32)mMaxU16[i]);
		}
	}
	// process remaining indices
	uint32 _maxIndex = 0;
	for (sint32 i = countRemaining; (--i) >= 0;)
	{
		uint16 idx = _swapEndianU16(*indicesU16BE);
		*indexOutput = idx;
		indexOutput++;
		indicesU16BE++;
		_maxIndex = std::max(_maxIndex, (uint32)idx);
	}
	// update min/max
	indexMax = std::max(indexMax, _maxIndex);
}

void LatteIndices_fastConvertU32_NEON(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	const uint32* indicesU32BE = (const uint32*)indexDataInput;
	uint32* indexOutput = (uint32*)indexDataOutput;
	sint32 count8 = count >> 2;
	sint32 countRemaining = count & 3;

	if (count8)
	{
		uint32x4_t mMax = vdupq_n_u32(0x00000000);
		uint32x4_t mTemp;
		uint32x4_t* mRawIndices = (uint32x4_t*) indicesU32BE;
		indicesU32BE += count8 * 4;
		uint32x4_t* mOutputIndices = (uint32x4_t*) indexOutput;
		indexOutput += count8 * 4;

		while (count8--)
		{
			mTemp = vld1q_u32((uint32*)mRawIndices);
			mRawIndices++;
			mTemp = vrev32q_u8(mTemp);
			mMax = vmaxq_u32(mMax, mTemp);
			vst1q_u32((uint32*)mOutputIndices, mTemp);
			mOutputIndices++;
		}

		uint32* mMaxU32 = (uint32*)&mMax;

		for (int i = 0; i < 4; ++i) {
			indexMax = std::max(indexMax, mMaxU32[i]);
		}
	}
	// process remaining indices
	uint32 _maxIndex = 0;
	for (sint32 i = countRemaining; (--i) >= 0;)
	{
		uint32 idx = _swapEndianU32(*indicesU32BE);
		*indexOutput = idx;
		indexOutput++;
		indicesU32BE++;
		_maxIndex = std::max(_maxIndex, idx);
	}
	// update min/max
	indexMax = std::max(indexMax, _maxIndex);
}

#endif

template<typename T, bool isBigEndian>
void _LatteIndices_alternativeCalculateIndexMax(const void* indexData, uint32 count, T primitiveRestartIndex, uint32& indexMax)
{
	uint32 calculatedMax = 0;
	for (uint32 i = 0; i < count; ++i)
	{
		const T idx = LatteIndices_readIndex<T, isBigEndian>(indexData, i);
		if (idx != primitiveRestartIndex)
			calculatedMax = std::max(calculatedMax, (uint32)idx);
	}
	indexMax = calculatedMax;
}

// calculate min and max index while taking primitive restart into account
// fallback implementation in case the fast path gives us invalid results
void LatteIndices_alternativeCalculateIndexMax(const void* indexData, LatteIndexType indexType, uint32 count, uint32 primitiveRestartIndex, uint32& indexMax)
{
	if (count == 0)
	{
		indexMax = 0;
		return;
	}
	if (indexType == LatteIndexType::U16_BE && primitiveRestartIndex <= std::numeric_limits<uint16>::max())
		_LatteIndices_alternativeCalculateIndexMax<uint16, true>(indexData, count, (uint16)primitiveRestartIndex, indexMax);
	else if (indexType == LatteIndexType::U16_LE && primitiveRestartIndex <= std::numeric_limits<uint16>::max())
		_LatteIndices_alternativeCalculateIndexMax<uint16, false>(indexData, count, (uint16)primitiveRestartIndex, indexMax);
	else if (indexType == LatteIndexType::U32_BE)
		_LatteIndices_alternativeCalculateIndexMax<uint32, true>(indexData, count, primitiveRestartIndex, indexMax);
	else if (indexType == LatteIndexType::U32_LE)
		_LatteIndices_alternativeCalculateIndexMax<uint32, false>(indexData, count, primitiveRestartIndex, indexMax);
	else
		cemu_assert_suspicious();
}

void LatteIndices_decode(const void* indexData, LatteIndexType indexType, uint32 count, LattePrimitiveMode primitiveMode, uint32& indexMax, Renderer::INDEX_TYPE& renderIndexType, uint32& outputCount, Renderer::IndexAllocation& indexAllocation)
{
	// what this should do:
	// [x] use fast SIMD-based index decoding
	// [x] unpack QUAD indices to triangle indices
	// [x] calculate min and max index, be careful about primitive restart index
	// [x] decode data directly into coherent memory buffer?
	// [ ] better cache implementation, allow to cache across frames

	const RendererAPI rendererType = g_renderer->GetType();
	const uint32 primitiveRestartIndex = LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.get_RESTART_INDEX();
#if defined(CEMU_UWP)
	const bool isD3D12Renderer = rendererType == RendererAPI::Direct3D12;
	const unsigned indexBits = (indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE) ? 16 :
		(indexType == LatteIndexType::U32_BE || indexType == LatteIndexType::U32_LE) ? 32 : 0;
	const bool primitiveRestartEnabled = LattePrimitiveRestartPolicy::IsEnabled(
		isD3D12Renderer, LattePrimitiveRestartPolicy::IsNativeStrip(primitiveMode),
		LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_EN.get_RESET_EN(), indexBits, primitiveRestartIndex);
#else
	const bool isD3D12Renderer = false;
	const bool primitiveRestartEnabled = LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_EN.get_RESET_EN();
#endif
	const bool primitiveRestart16Enabled = primitiveRestartEnabled && primitiveRestartIndex <= std::numeric_limits<uint16>::max();
	const uint16 primitiveRestartIndex16 = (uint16)primitiveRestartIndex;
	const bool d3d12PrimitiveRestartEnabled = isD3D12Renderer && primitiveRestartEnabled;
	const bool d3d12PrimitiveRestart16Enabled = d3d12PrimitiveRestartEnabled && primitiveRestart16Enabled;
	// D3D12 only cuts 0xffff/0xffffffff; promoting to U32 keeps a real 0xffff index
	const bool promoteD3D12RestartToU32 = isD3D12Renderer &&
		(indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE) &&
		primitiveRestart16Enabled && primitiveRestartIndex16 != std::numeric_limits<uint16>::max() &&
		LatteIndices_isD3D12FixedRestartMode(primitiveMode);
	const bool segmentedLineLoop = d3d12PrimitiveRestartEnabled;

	// reuse from cache if data didn't change
	auto cacheEntry = std::find_if(LatteIndexCache.entry.begin(), LatteIndexCache.entry.end(), [=](const auto& entry)
	{
		const bool baseKeyMatches =
#if defined(CEMU_UWP)
			entry.valid &&
#endif
			entry.lastPtr == indexData &&
			entry.lastCount == count &&
			entry.lastPrimitiveMode == primitiveMode &&
			entry.lastIndexType == indexType;
#if defined(CEMU_UWP)
		return baseKeyMatches &&
			entry.lastRendererType == rendererType &&
			entry.lastPrimitiveRestartEnabled == primitiveRestartEnabled &&
			entry.lastPrimitiveRestartIndex == primitiveRestartIndex;
#else
		return baseKeyMatches;
#endif
	});
	if (cacheEntry != LatteIndexCache.entry.end())
	{
		indexMax = cacheEntry->indexMax;
		renderIndexType = cacheEntry->renderIndexType;
		outputCount = cacheEntry->outputCount;
		indexAllocation = cacheEntry->indexAllocation;
		cacheEntry->lastUsed = LatteIndices_GetNextUsageIndex();
		return;
	}

	outputCount = 0;
	if (indexType == LatteIndexType::AUTO)
		renderIndexType = Renderer::INDEX_TYPE::NONE;
	else if (indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE)
		renderIndexType = promoteD3D12RestartToU32 ? Renderer::INDEX_TYPE::U32 : Renderer::INDEX_TYPE::U16;
	else if (indexType == LatteIndexType::U32_BE || indexType == LatteIndexType::U32_LE)
		renderIndexType = Renderer::INDEX_TYPE::U32;
	else
		cemu_assert_debug(false);

	// calculate index output size
	uint32 indexOutputSize = LatteIndices_calculateIndexOutputSize(primitiveMode, indexType, count, segmentedLineLoop, promoteD3D12RestartToU32);
	if (indexOutputSize == 0)
	{
		outputCount = count;
		indexMax = std::max(count, 1u)-1;
		renderIndexType = Renderer::INDEX_TYPE::NONE;
		indexAllocation = {};
		return; // no indices
	}
	// query index buffer from renderer
	indexAllocation = g_renderer->indexData_reserveIndexMemory(indexOutputSize);
	void* indexOutputPtr = indexAllocation.mem;
#if defined(CEMU_UWP)
	if (!indexOutputPtr || !indexAllocation.rendererInternal)
	{
		outputCount = 0;
		indexMax = 0;
		renderIndexType = Renderer::INDEX_TYPE::NONE;
		indexAllocation = {};
		return;
	}
#endif

	// decode indices
	indexMax = std::numeric_limits<uint32>::min();
	bool indexMaxIsAccurate = false;
	if (primitiveMode == LattePrimitiveMode::QUADS)
	{
		// unpack quads into triangles
		if (indexType == LatteIndexType::AUTO)
		{
			if (count <= 0xFFFF)
			{
				LatteIndices_generateAutoQuadIndices<uint16>(indexData, indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U16;
			}
			else
			{
				LatteIndices_generateAutoQuadIndices<uint32>(indexData, indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U32;
			}
			outputCount = count / 4 * 6;
		}
		else if (indexType == LatteIndexType::U16_BE)
			outputCount = LatteIndices_unpackQuadsAndConvert<uint16, true>(indexData, indexOutputPtr, count, d3d12PrimitiveRestart16Enabled, primitiveRestartIndex16, indexMax);
		else if (indexType == LatteIndexType::U16_LE)
			outputCount = LatteIndices_unpackQuadsAndConvert<uint16, false>(indexData, indexOutputPtr, count, d3d12PrimitiveRestart16Enabled, primitiveRestartIndex16, indexMax);
		else if (indexType == LatteIndexType::U32_BE)
			outputCount = LatteIndices_unpackQuadsAndConvert<uint32, true>(indexData, indexOutputPtr, count, d3d12PrimitiveRestartEnabled, primitiveRestartIndex, indexMax);
		else if (indexType == LatteIndexType::U32_LE)
			outputCount = LatteIndices_unpackQuadsAndConvert<uint32, false>(indexData, indexOutputPtr, count, d3d12PrimitiveRestartEnabled, primitiveRestartIndex, indexMax);
		else
			cemu_assert_debug(false);
		indexMaxIsAccurate = true;
	}
	else if (primitiveMode == LattePrimitiveMode::QUAD_STRIP)
	{
		// unpack quad strip into triangles
		if (indexType == LatteIndexType::AUTO)
		{
			if (count <= 0xFFFF)
			{
				LatteIndices_generateAutoQuadStripIndices<uint16>(indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U16;
			}
			else
			{
				LatteIndices_generateAutoQuadStripIndices<uint32>(indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U32;
			}
			outputCount = count >= 4 ? (count - 2) / 2 * 6 : 0;
		}
		else if (indexType == LatteIndexType::U16_BE)
			outputCount = LatteIndices_unpackQuadStripAndConvert<uint16, true>(indexData, indexOutputPtr, count, d3d12PrimitiveRestart16Enabled, primitiveRestartIndex16, indexMax);
		else if (indexType == LatteIndexType::U16_LE)
			outputCount = LatteIndices_unpackQuadStripAndConvert<uint16, false>(indexData, indexOutputPtr, count, d3d12PrimitiveRestart16Enabled, primitiveRestartIndex16, indexMax);
		else if (indexType == LatteIndexType::U32_BE)
			outputCount = LatteIndices_unpackQuadStripAndConvert<uint32, true>(indexData, indexOutputPtr, count, d3d12PrimitiveRestartEnabled, primitiveRestartIndex, indexMax);
		else if (indexType == LatteIndexType::U32_LE)
			outputCount = LatteIndices_unpackQuadStripAndConvert<uint32, false>(indexData, indexOutputPtr, count, d3d12PrimitiveRestartEnabled, primitiveRestartIndex, indexMax);
		else
			cemu_assert_debug(false);
		indexMaxIsAccurate = true;
	}
	else if (primitiveMode == LattePrimitiveMode::LINE_LOOP)
	{
		// unpack line loop into line strip with extra reconnecting vertex
		if (indexType == LatteIndexType::AUTO)
		{
			if (count <= 0xFFFF)
			{
				LatteIndices_generateAutoLineLoopIndices<uint16>(indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U16;
			}
			else
			{
				LatteIndices_generateAutoLineLoopIndices<uint32>(indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U32;
			}
			outputCount = count == 0 ? 0 : count + 1;
		}
		else if (indexType == LatteIndexType::U16_BE)
		{
			if (promoteD3D12RestartToU32)
				outputCount = LatteIndices_unpackLineLoopAndConvert<uint16, true, uint32>(indexData, indexOutputPtr, count, true, primitiveRestartIndex16, indexMax);
			else
				outputCount = LatteIndices_unpackLineLoopAndConvert<uint16, true>(indexData, indexOutputPtr, count, segmentedLineLoop && primitiveRestart16Enabled, primitiveRestartIndex16, indexMax);
		}
		else if (indexType == LatteIndexType::U16_LE)
		{
			if (promoteD3D12RestartToU32)
				outputCount = LatteIndices_unpackLineLoopAndConvert<uint16, false, uint32>(indexData, indexOutputPtr, count, true, primitiveRestartIndex16, indexMax);
			else
				outputCount = LatteIndices_unpackLineLoopAndConvert<uint16, false>(indexData, indexOutputPtr, count, segmentedLineLoop && primitiveRestart16Enabled, primitiveRestartIndex16, indexMax);
		}
		else if (indexType == LatteIndexType::U32_BE)
			outputCount = LatteIndices_unpackLineLoopAndConvert<uint32, true>(indexData, indexOutputPtr, count, segmentedLineLoop, primitiveRestartIndex, indexMax);
		else if (indexType == LatteIndexType::U32_LE)
			outputCount = LatteIndices_unpackLineLoopAndConvert<uint32, false>(indexData, indexOutputPtr, count, segmentedLineLoop, primitiveRestartIndex, indexMax);
		else
			cemu_assert_debug(false);
		indexMaxIsAccurate = true;
	}
	else if (primitiveMode == LattePrimitiveMode::TRIANGLE_FAN && (rendererType == RendererAPI::Metal || rendererType == RendererAPI::Direct3D12))
	{
		const bool remapFanRestart = isD3D12Renderer;
		if (indexType == LatteIndexType::AUTO)
		{
			if (count <= 0xFFFF)
			{
#if defined(CEMU_UWP)
				outputCount = LatteIndices_generateAutoTriangleFanList<uint16>(indexOutputPtr, count, indexMax);
#else
				LatteIndices_generateAutoTriangleFanIndices<uint16>(indexData, indexOutputPtr, count, indexMax);
#endif
				renderIndexType = Renderer::INDEX_TYPE::U16;
			}
			else
			{
#if defined(CEMU_UWP)
				outputCount = LatteIndices_generateAutoTriangleFanList<uint32>(indexOutputPtr, count, indexMax);
#else
				LatteIndices_generateAutoTriangleFanIndices<uint32>(indexData, indexOutputPtr, count, indexMax);
#endif
				renderIndexType = Renderer::INDEX_TYPE::U32;
			}
#if !defined(CEMU_UWP)
			outputCount = count;
#endif
		}
		else if (indexType == LatteIndexType::U16_BE)
		{
			if (promoteD3D12RestartToU32)
#if defined(CEMU_UWP)
				outputCount = LatteIndices_unpackTriangleFanToList<uint16, true, uint32>(indexData, indexOutputPtr, count, true, primitiveRestartIndex16, indexMax);
			else
				outputCount = LatteIndices_unpackTriangleFanToList<uint16, true>(indexData, indexOutputPtr, count, remapFanRestart && primitiveRestart16Enabled, primitiveRestartIndex16, indexMax);
#else
				outputCount = LatteIndices_unpackTriangleFanAndConvert<uint16, true, uint32>(indexData, indexOutputPtr, count, true, primitiveRestartIndex16, indexMax);
			else
				outputCount = LatteIndices_unpackTriangleFanAndConvert<uint16, true>(indexData, indexOutputPtr, count, remapFanRestart && primitiveRestart16Enabled, primitiveRestartIndex16, indexMax);
#endif
		}
		else if (indexType == LatteIndexType::U16_LE)
		{
			if (promoteD3D12RestartToU32)
#if defined(CEMU_UWP)
				outputCount = LatteIndices_unpackTriangleFanToList<uint16, false, uint32>(indexData, indexOutputPtr, count, true, primitiveRestartIndex16, indexMax);
			else
				outputCount = LatteIndices_unpackTriangleFanToList<uint16, false>(indexData, indexOutputPtr, count, remapFanRestart && primitiveRestart16Enabled, primitiveRestartIndex16, indexMax);
#else
				outputCount = LatteIndices_unpackTriangleFanAndConvert<uint16, false, uint32>(indexData, indexOutputPtr, count, true, primitiveRestartIndex16, indexMax);
			else
				outputCount = LatteIndices_unpackTriangleFanAndConvert<uint16, false>(indexData, indexOutputPtr, count, remapFanRestart && primitiveRestart16Enabled, primitiveRestartIndex16, indexMax);
#endif
		}
		else if (indexType == LatteIndexType::U32_BE)
#if defined(CEMU_UWP)
			outputCount = LatteIndices_unpackTriangleFanToList<uint32, true>(indexData, indexOutputPtr, count, remapFanRestart && primitiveRestartEnabled, primitiveRestartIndex, indexMax);
#else
			outputCount = LatteIndices_unpackTriangleFanAndConvert<uint32, true>(indexData, indexOutputPtr, count, remapFanRestart && primitiveRestartEnabled, primitiveRestartIndex, indexMax);
#endif
		else if (indexType == LatteIndexType::U32_LE)
#if defined(CEMU_UWP)
			outputCount = LatteIndices_unpackTriangleFanToList<uint32, false>(indexData, indexOutputPtr, count, remapFanRestart && primitiveRestartEnabled, primitiveRestartIndex, indexMax);
#else
			outputCount = LatteIndices_unpackTriangleFanAndConvert<uint32, false>(indexData, indexOutputPtr, count, remapFanRestart && primitiveRestartEnabled, primitiveRestartIndex, indexMax);
#endif
		else
			cemu_assert_debug(false);
		indexMaxIsAccurate = true;
	}
	else
	{
		const bool remapD3D12Restart = isD3D12Renderer &&
			primitiveRestartEnabled && LatteIndices_isD3D12NativeStripMode(primitiveMode);
		if (indexType == LatteIndexType::U16_BE)
		{
			if (remapD3D12Restart && primitiveRestart16Enabled)
			{
				if (promoteD3D12RestartToU32)
					LatteIndices_convertAndRemapRestart<uint16, true, uint32>(indexData, indexOutputPtr, count, primitiveRestartIndex16, indexMax);
				else
					LatteIndices_convertAndRemapRestart<uint16, true>(indexData, indexOutputPtr, count, primitiveRestartIndex16, indexMax);
				indexMaxIsAccurate = true;
			}
			else
			{
#if defined(ARCH_X86_64)
				if (g_CPUFeatures.x86.avx2)
					LatteIndices_fastConvertU16_AVX2(indexData, indexOutputPtr, count, indexMax);
				else if (g_CPUFeatures.x86.sse4_1 && g_CPUFeatures.x86.ssse3)
					LatteIndices_fastConvertU16_SSE41(indexData, indexOutputPtr, count, indexMax);
				else
					LatteIndices_convertBE<uint16>(indexData, indexOutputPtr, count, indexMax);
#elif defined(__aarch64__)
				LatteIndices_fastConvertU16_NEON(indexData, indexOutputPtr, count, indexMax);
#else
				LatteIndices_convertBE<uint16>(indexData, indexOutputPtr, count, indexMax);
#endif
			}
		}
		else if (indexType == LatteIndexType::U32_BE)
		{
			if (remapD3D12Restart)
			{
				LatteIndices_convertAndRemapRestart<uint32, true>(indexData, indexOutputPtr, count, primitiveRestartIndex, indexMax);
				indexMaxIsAccurate = true;
			}
			else
			{
#if defined(ARCH_X86_64)
				if (g_CPUFeatures.x86.avx2)
					LatteIndices_fastConvertU32_AVX2(indexData, indexOutputPtr, count, indexMax);
				else
					LatteIndices_convertBE<uint32>(indexData, indexOutputPtr, count, indexMax);
#elif defined(__aarch64__)
				LatteIndices_fastConvertU32_NEON(indexData, indexOutputPtr, count, indexMax);
#else
				LatteIndices_convertBE<uint32>(indexData, indexOutputPtr, count, indexMax);
#endif
			}
		}
		else if (indexType == LatteIndexType::U16_LE)
		{
			if (remapD3D12Restart && primitiveRestart16Enabled)
			{
				if (promoteD3D12RestartToU32)
					LatteIndices_convertAndRemapRestart<uint16, false, uint32>(indexData, indexOutputPtr, count, primitiveRestartIndex16, indexMax);
				else
					LatteIndices_convertAndRemapRestart<uint16, false>(indexData, indexOutputPtr, count, primitiveRestartIndex16, indexMax);
				indexMaxIsAccurate = true;
			}
			else
				LatteIndices_convertLE<uint16>(indexData, indexOutputPtr, count, indexMax);
		}
		else if (indexType == LatteIndexType::U32_LE)
		{
			if (remapD3D12Restart)
			{
				LatteIndices_convertAndRemapRestart<uint32, false>(indexData, indexOutputPtr, count, primitiveRestartIndex, indexMax);
				indexMaxIsAccurate = true;
			}
			else
				LatteIndices_convertLE<uint32>(indexData, indexOutputPtr, count, indexMax);
		}
		else
			cemu_assert_debug(false);
		outputCount = count;
	}
	// fast passthrough converters include the restart value in their maximum
	if (!indexMaxIsAccurate && primitiveRestartEnabled && primitiveRestartIndex == indexMax)
	{
		LatteIndices_alternativeCalculateIndexMax(indexData, indexType, count, primitiveRestartIndex, indexMax);
	}
	if (outputCount == 0)
		indexMax = 0;
	g_renderer->indexData_uploadIndexMemory(indexAllocation);
	performanceMonitor.cycle[performanceMonitor.cycleIndex].indexDataUploaded += indexOutputSize;
	// get least recently used cache entry
	auto lruEntry = std::min_element(LatteIndexCache.entry.begin(), LatteIndexCache.entry.end(), [](const auto& a, const auto& b)
	{
		return a.lastUsed < b.lastUsed;
	});
	// invalidate previous allocation
#if defined(CEMU_UWP)
	if (lruEntry->valid)
#else
	if(lruEntry->lastPtr != nullptr)
#endif
		g_renderer->indexData_releaseIndexMemory(lruEntry->indexAllocation);
	// update cache
#if defined(CEMU_UWP)
	lruEntry->valid = true;
#endif
	lruEntry->lastPtr = indexData;
	lruEntry->lastCount = count;
	lruEntry->lastPrimitiveMode = primitiveMode;
	lruEntry->lastIndexType = indexType;
#if defined(CEMU_UWP)
	lruEntry->lastRendererType = rendererType;
	lruEntry->lastPrimitiveRestartEnabled = primitiveRestartEnabled;
	lruEntry->lastPrimitiveRestartIndex = primitiveRestartIndex;
#endif
	lruEntry->indexMax = indexMax;
	lruEntry->renderIndexType = renderIndexType;
	lruEntry->outputCount = outputCount;
	lruEntry->indexAllocation = indexAllocation;
	lruEntry->lastUsed = LatteIndices_GetNextUsageIndex();
}
