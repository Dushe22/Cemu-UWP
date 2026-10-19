#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace D3D12TypedSurfaceStats
{
constexpr std::size_t kMaxInspectedBytes = 64u * 1024u * 1024u;

enum class Status
{
	Ok,
	UnsupportedFormat,
	InvalidDimensions,
	SizeOverflow,
	InvalidRowLayout,
	NullData,
	ShortReadback,
};

constexpr const char* StatusName(Status status)
{
	switch (status)
	{
	case Status::Ok: return "ok";
	case Status::UnsupportedFormat: return "unsupported_format";
	case Status::InvalidDimensions: return "invalid_dimensions";
	case Status::SizeOverflow: return "size_overflow";
	case Status::InvalidRowLayout: return "invalid_row_layout";
	case Status::NullData: return "null_data";
	case Status::ShortReadback: return "short_readback";
	}
	return "unknown";
}

struct Component
{
	std::uint64_t finite{};
	std::uint64_t nan{};
	std::uint64_t inf{};
	// only meaningful when finite != 0
	double minimum{};
	double maximum{};
};

struct Summary
{
	Status status{Status::UnsupportedFormat};
	std::uint32_t format{};
	std::uint32_t componentCount{};
	std::uint32_t bytesPerPixel{};
	std::uint64_t expectedPixels{};
	std::uint64_t inspectedPixels{};
	bool capped{};
	std::array<Component, 4> components{};
	// RGBA16_FLOAT only: R/G treated as normal X/Y
	std::uint64_t xyFinite{};
	std::uint64_t xyNonFinite{};
	std::uint64_t xyRadiusSquaredAboveOne{};
	std::uint64_t xyRadiusSquaredAboveTolerance{};
};

constexpr std::uint32_t HalfToFloatBits(std::uint16_t half)
{
	const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16;
	std::uint32_t exponent = (half >> 10) & 0x1fu;
	std::uint32_t mantissa = half & 0x3ffu;
	if (exponent == 0)
	{
		if (mantissa == 0)
			return sign;
		// normalize the subnormal with integer ops so every payload survives exactly
		exponent = 113;
		while ((mantissa & 0x400u) == 0)
		{
			mantissa <<= 1;
			--exponent;
		}
		return sign | (exponent << 23) | ((mantissa & 0x3ffu) << 13);
	}
	if (exponent == 31)
		return sign | 0x7f800000u | (mantissa << 13);
	return sign | ((exponent + 112) << 23) | (mantissa << 13);
}

namespace Detail
{
constexpr std::uint64_t FloatToDoubleBits(std::uint32_t bits)
{
	const std::uint64_t sign = static_cast<std::uint64_t>(bits & 0x80000000u) << 32;
	std::uint64_t exponent = (bits >> 23) & 0xffu;
	std::uint64_t mantissa = bits & 0x7fffffu;
	if (exponent == 0)
	{
		if (mantissa == 0)
			return sign;
		exponent = 897;
		while ((mantissa & 0x800000u) == 0)
		{
			mantissa <<= 1;
			--exponent;
		}
		return sign | (exponent << 52) | ((mantissa & 0x7fffffu) << 29);
	}
	if (exponent == 255)
		return sign | 0x7ff0000000000000ull | (mantissa << 29);
	return sign | ((exponent + 896) << 52) | (mantissa << 29);
}

inline std::uint16_t Read16(const std::uint8_t* bytes)
{
	return static_cast<std::uint16_t>(static_cast<std::uint32_t>(bytes[0]) |
		(static_cast<std::uint32_t>(bytes[1]) << 8));
}

inline std::uint32_t Read32(const std::uint8_t* bytes)
{
	return static_cast<std::uint32_t>(bytes[0]) |
		(static_cast<std::uint32_t>(bytes[1]) << 8) |
		(static_cast<std::uint32_t>(bytes[2]) << 16) |
		(static_cast<std::uint32_t>(bytes[3]) << 24);
}

inline void AddFinite(Component& component, double value)
{
	if (component.finite == 0)
		component.minimum = component.maximum = value;
	else
	{
		component.minimum = std::min(component.minimum, value);
		component.maximum = std::max(component.maximum, value);
	}
	++component.finite;
}

inline bool AddFloatBits(Component& component, std::uint32_t bits, double& finiteValue)
{
	if ((bits & 0x7f800000u) == 0x7f800000u)
	{
		if ((bits & 0x7fffffu) != 0)
			++component.nan;
		else
			++component.inf;
		return false;
	}
	// integer widening avoids signaling-NaN arithmetic and flush-to-zero modes
	finiteValue = std::bit_cast<double>(FloatToDoubleBits(bits));
	AddFinite(component, finiteValue);
	return true;
}
}

// raw DXGI_FORMAT values: 10=RGBA16_FLOAT 28=RGBA8_UNORM 41=R32_FLOAT 45=D24S8 46=R24X8
inline Summary Analyze(const std::uint8_t* data, std::size_t dataBytes,
	std::uint32_t dxgiFormat, std::int64_t width, std::int64_t height,
	std::uint64_t tightRowBytes)
{
	Summary result;
	result.format = dxgiFormat;
	switch (dxgiFormat)
	{
	case 10: result.componentCount = 4; result.bytesPerPixel = 8; break;
	case 28: result.componentCount = 4; result.bytesPerPixel = 4; break;
	case 41:
	case 45:
	case 46: result.componentCount = 1; result.bytesPerPixel = 4; break;
	default: return result;
	}
	if (width <= 0 || height <= 0)
	{
		result.status = Status::InvalidDimensions;
		return result;
	}
	const auto unsignedWidth = static_cast<std::uint64_t>(width);
	const auto unsignedHeight = static_cast<std::uint64_t>(height);
	constexpr auto maxSize = std::numeric_limits<std::size_t>::max();
	if (unsignedWidth > maxSize / result.bytesPerPixel)
	{
		result.status = Status::SizeOverflow;
		return result;
	}
	const auto expectedRowBytes = static_cast<std::size_t>(unsignedWidth * result.bytesPerPixel);
	if (unsignedHeight > maxSize / expectedRowBytes)
	{
		result.status = Status::SizeOverflow;
		return result;
	}
	const auto expectedBytes = expectedRowBytes * static_cast<std::size_t>(unsignedHeight);
	result.expectedPixels = expectedBytes / result.bytesPerPixel;
	if (tightRowBytes != expectedRowBytes)
	{
		result.status = Status::InvalidRowLayout;
		return result;
	}
	if (!data)
	{
		result.status = Status::NullData;
		return result;
	}
	if (dataBytes < expectedBytes)
	{
		result.status = Status::ShortReadback;
		return result;
	}
	result.status = Status::Ok;
	const auto pixelCount = std::min(expectedBytes, kMaxInspectedBytes) / result.bytesPerPixel;
	result.inspectedPixels = pixelCount;
	result.capped = result.inspectedPixels < result.expectedPixels;
	for (std::size_t pixel = 0; pixel < pixelCount; ++pixel)
	{
		const auto* bytes = data + pixel * result.bytesPerPixel;
		if (dxgiFormat == 10)
		{
			std::array<double, 4> values{};
			std::array<bool, 4> finite{};
			for (std::size_t channel = 0; channel < 4; ++channel)
				finite[channel] = Detail::AddFloatBits(result.components[channel],
					HalfToFloatBits(Detail::Read16(bytes + channel * 2)), values[channel]);
			if (finite[0] && finite[1])
			{
				++result.xyFinite;
				// squared in double so half-precision differences near one survive
				const double radiusSquared = values[0] * values[0] + values[1] * values[1];
				result.xyRadiusSquaredAboveOne += radiusSquared > 1.0;
				result.xyRadiusSquaredAboveTolerance += radiusSquared > 1.0001;
			}
			else
				++result.xyNonFinite;
		}
		else if (dxgiFormat == 28)
		{
			for (std::size_t channel = 0; channel < 4; ++channel)
				Detail::AddFinite(result.components[channel], static_cast<double>(bytes[channel]) / 255.0);
		}
		else if (dxgiFormat == 41)
		{
			double value{};
			Detail::AddFloatBits(result.components[0], Detail::Read32(bytes), value);
		}
		else
			Detail::AddFinite(result.components[0], static_cast<double>(Detail::Read32(bytes) & 0xffffffu) / 16777215.0);
	}
	return result;
}
}
