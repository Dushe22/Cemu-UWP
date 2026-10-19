#pragma once

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

class StringBuf
{
public:
	StringBuf(uint32 bufferSize)
	{
		this->str = (uint8*)malloc(bufferSize + 4);
		this->allocated = true;
		this->length = 0;
#if defined(CEMU_UWP)
		if (!this->str)
			this->allocated = false;
		this->limit = this->str ? bufferSize : 0;
#else
		this->limit = bufferSize;
#endif
	}

	~StringBuf()
	{
		if (this->allocated)
			free(this->str);
	}

	template<typename TFmt, typename ... TArgs>
	void addFmt(const TFmt& format, TArgs&&... args)
	{
#if defined(CEMU_UWP)
		if (!this->str || this->length >= this->limit)
			_reserve(std::max<uint32>(this->length + 1024, this->limit + std::max<uint32>(this->limit / 2, 1024)));

		const auto formatView = fmt::detail::to_string_view(format);
		const size_t available = (size_t)(this->limit - this->length);
		auto formatArgs = fmt::make_format_args(args...);
		auto r = fmt::vformat_to_n((char*)(this->str + this->length), available, formatView, formatArgs);
		if (r.size + 1 > available)
		{
			const uint64 requiredLimit = (uint64)this->length + (uint64)r.size + 1;
			if (requiredLimit > std::numeric_limits<uint32>::max())
				throw std::length_error("StringBuf formatted output is too large");
			_reserve(std::max<uint32>((uint32)requiredLimit + 64, this->limit + std::max<uint32>(this->limit / 2, 1024)));
			auto retryArgs = fmt::make_format_args(args...);
			r = fmt::vformat_to_n((char*)(this->str + this->length), (size_t)(this->limit - this->length), formatView, retryArgs);
		}
		this->length += (uint32)r.size;
		this->str[this->length] = '\0';
#else
		auto r = fmt::vformat_to_n((char*)(this->str + this->length), (size_t)(this->limit - this->length), fmt::detail::to_string_view(format), fmt::make_format_args(args...));
		this->length += (uint32)r.size;
#endif
	}

	void add(const char* appendedStr)
	{
#if defined(CEMU_UWP)
		if (!appendedStr)
			return;
		add(std::string_view(appendedStr));
#else
		const char* outputStart = (char*)(this->str + this->length);
		char* output = (char*)outputStart;
		const char* outputEnd = (char*)(this->str + this->limit - 1);
		while (output < outputEnd)
		{
			char c = *appendedStr;
			if (c == '\0')
				break;
			*output = c;
			appendedStr++;
			output++;
		}
		this->length += (uint32)(output - outputStart);
		*output = '\0';
#endif
	}

	void add(std::string_view appendedStr)
	{
		size_t copyLen = appendedStr.size();
		if (this->length + copyLen + 1 >= this->limit)
			_reserve(std::max<uint32>(this->length + copyLen + 64, this->limit + this->limit / 2));
		char* outputStart = (char*)(this->str + this->length);
		std::copy(appendedStr.data(), appendedStr.data() + copyLen, outputStart);
		length += copyLen;
		outputStart[copyLen] = '\0';
	}

	void reset()
	{
		length = 0;
	}

	uint32 getLen() const
	{
		return length;
	}

	const char* c_str() const
	{
		str[length] = '\0';
		return (const char*)str;
	}

	void shrink_to_fit()
	{
		if (!this->allocated)
			return;
		uint32 newLimit = this->length;
#if defined(CEMU_UWP)
		if (newLimit == this->limit)
			return;
#endif
		this->str = (uint8*)realloc(this->str, newLimit + 4);
		this->limit = newLimit;
	}

private:
	void _reserve(uint32 newLimit)
	{
		cemu_assert_debug(newLimit > length);
#if defined(CEMU_UWP)
		uint8* newStr = (uint8*)realloc(this->str, newLimit + 4);
		if (!newStr)
			throw std::bad_alloc();
		this->str = newStr;
		this->allocated = true;
#else
		this->str = (uint8*)realloc(this->str, newLimit + 4);
#endif
		this->limit = newLimit;
	}

	uint8*	str;
	uint32	length; /* in bytes */
	uint32	limit; /* in bytes */
	bool	allocated;
};
