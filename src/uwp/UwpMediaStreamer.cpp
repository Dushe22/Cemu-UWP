#include "UwpMediaStreamer.h"

#if defined(CEMU_UWP)

#include "Cemu/Logging/CemuLogging.h"

#include <windows.h>
#include <dxgiformat.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace CemuUwp::MediaStreamer
{
namespace
{
class FfmpegApi
{
public:
	bool Load()
	{
		std::call_once(m_once, [this] { m_loaded = LoadImpl(); });
		return m_loaded;
	}

	std::string Error(int code) const
	{
		char text[AV_ERROR_MAX_STRING_SIZE]{};
		if (p_av_strerror && p_av_strerror(code, text, sizeof(text)) == 0)
			return text;
		return fmt::format("FFmpeg error {}", code);
	}

#define FFMPEG_PROC(name) decltype(&::name) p_##name = nullptr
	FFMPEG_PROC(av_channel_layout_copy);
	FFMPEG_PROC(av_channel_layout_default);
	FFMPEG_PROC(av_channel_layout_uninit);
	FFMPEG_PROC(av_dict_free);
	FFMPEG_PROC(av_dict_set);
	FFMPEG_PROC(av_frame_alloc);
	FFMPEG_PROC(av_frame_free);
	FFMPEG_PROC(av_frame_get_buffer);
	FFMPEG_PROC(av_frame_make_writable);
	FFMPEG_PROC(av_packet_alloc);
	FFMPEG_PROC(av_packet_free);
	FFMPEG_PROC(av_packet_rescale_ts);
	FFMPEG_PROC(av_packet_unref);
	FFMPEG_PROC(av_strerror);

	FFMPEG_PROC(avcodec_alloc_context3);
	FFMPEG_PROC(avcodec_find_encoder_by_name);
	FFMPEG_PROC(avcodec_free_context);
	FFMPEG_PROC(avcodec_open2);
	FFMPEG_PROC(avcodec_parameters_from_context);
	FFMPEG_PROC(avcodec_receive_packet);
	FFMPEG_PROC(avcodec_send_frame);

	FFMPEG_PROC(av_interleaved_write_frame);
	FFMPEG_PROC(av_write_trailer);
	FFMPEG_PROC(avformat_alloc_output_context2);
	FFMPEG_PROC(avformat_free_context);
	FFMPEG_PROC(avformat_network_init);
	FFMPEG_PROC(avformat_new_stream);
	FFMPEG_PROC(avformat_write_header);
	FFMPEG_PROC(avio_closep);
	FFMPEG_PROC(avio_open2);

	FFMPEG_PROC(swr_alloc_set_opts2);
	FFMPEG_PROC(swr_convert);
	FFMPEG_PROC(swr_free);
	FFMPEG_PROC(swr_init);

	FFMPEG_PROC(sws_freeContext);
	FFMPEG_PROC(sws_getContext);
	FFMPEG_PROC(sws_scale);
#undef FFMPEG_PROC

private:
	bool LoadImpl()
	{
		m_avutil = LoadPackagedLibrary(L"avutil-58.dll", 0);
		m_swresample = LoadPackagedLibrary(L"swresample-4.dll", 0);
		m_swscale = LoadPackagedLibrary(L"swscale-7.dll", 0);
		m_avcodec = LoadPackagedLibrary(L"avcodec-60.dll", 0);
		m_avformat = LoadPackagedLibrary(L"avformat-60.dll", 0);
		if (!m_avutil || !m_swresample || !m_swscale || !m_avcodec || !m_avformat)
		{
			cemuLog_log(LogType::Force, "Media streamer: could not load packaged FFmpeg runtime (error 0x{:08x})", GetLastError());
			return false;
		}

#define LOAD_FFMPEG(module, name) \
	p_##name = reinterpret_cast<decltype(p_##name)>(GetProcAddress(module, #name)); \
	if (!p_##name) { cemuLog_log(LogType::Force, "Media streamer: missing FFmpeg export {}", #name); return false; }
		LOAD_FFMPEG(m_avutil, av_channel_layout_copy);
		LOAD_FFMPEG(m_avutil, av_channel_layout_default);
		LOAD_FFMPEG(m_avutil, av_channel_layout_uninit);
		LOAD_FFMPEG(m_avutil, av_dict_free);
		LOAD_FFMPEG(m_avutil, av_dict_set);
		LOAD_FFMPEG(m_avutil, av_frame_alloc);
		LOAD_FFMPEG(m_avutil, av_frame_free);
		LOAD_FFMPEG(m_avutil, av_frame_get_buffer);
		LOAD_FFMPEG(m_avutil, av_frame_make_writable);
		LOAD_FFMPEG(m_avcodec, av_packet_alloc);
		LOAD_FFMPEG(m_avcodec, av_packet_free);
		LOAD_FFMPEG(m_avcodec, av_packet_rescale_ts);
		LOAD_FFMPEG(m_avcodec, av_packet_unref);
		LOAD_FFMPEG(m_avutil, av_strerror);

		LOAD_FFMPEG(m_avcodec, avcodec_alloc_context3);
		LOAD_FFMPEG(m_avcodec, avcodec_find_encoder_by_name);
		LOAD_FFMPEG(m_avcodec, avcodec_free_context);
		LOAD_FFMPEG(m_avcodec, avcodec_open2);
		LOAD_FFMPEG(m_avcodec, avcodec_parameters_from_context);
		LOAD_FFMPEG(m_avcodec, avcodec_receive_packet);
		LOAD_FFMPEG(m_avcodec, avcodec_send_frame);

		LOAD_FFMPEG(m_avformat, av_interleaved_write_frame);
		LOAD_FFMPEG(m_avformat, av_write_trailer);
		LOAD_FFMPEG(m_avformat, avformat_alloc_output_context2);
		LOAD_FFMPEG(m_avformat, avformat_free_context);
		LOAD_FFMPEG(m_avformat, avformat_network_init);
		LOAD_FFMPEG(m_avformat, avformat_new_stream);
		LOAD_FFMPEG(m_avformat, avformat_write_header);
		LOAD_FFMPEG(m_avformat, avio_closep);
		LOAD_FFMPEG(m_avformat, avio_open2);

		LOAD_FFMPEG(m_swresample, swr_alloc_set_opts2);
		LOAD_FFMPEG(m_swresample, swr_convert);
		LOAD_FFMPEG(m_swresample, swr_free);
		LOAD_FFMPEG(m_swresample, swr_init);

		LOAD_FFMPEG(m_swscale, sws_freeContext);
		LOAD_FFMPEG(m_swscale, sws_getContext);
		LOAD_FFMPEG(m_swscale, sws_scale);
#undef LOAD_FFMPEG

		p_avformat_network_init();
		cemuLog_logDebug(LogType::Force, "Companion stream: FFmpeg 6 UWP runtime loaded");
		return true;
	}

	std::once_flag m_once;
	bool m_loaded = false;
	HMODULE m_avutil = nullptr;
	HMODULE m_avcodec = nullptr;
	HMODULE m_avformat = nullptr;
	HMODULE m_swresample = nullptr;
	HMODULE m_swscale = nullptr;
};

FfmpegApi& GetFfmpegApi()
{
	static FfmpegApi api;
	return api;
}

struct RawFrame
{
	bool padView = false;
	uint32 width = 0;
	uint32 height = 0;
	uint32 rowBytes = 0;
	uint32 dxgiFormat = 0;
	std::vector<uint8> pixels;
};

uint8 ToUnorm8(float value)
{
	if (!(value > 0.0f))
		return 0;
	if (value >= 1.0f)
		return 255;
	return (uint8)std::lround(value * 255.0f);
}

// shared by half, float11 and float10, all of which use a 5 bit exponent
float DecodeSmallFloat(uint32 bits, uint32 mantissaBits)
{
	const uint32 mantissa = bits & ((1u << mantissaBits) - 1u);
	const uint32 exponent = (bits >> mantissaBits) & 0x1F;
	const float fraction = (float)mantissa / (float)(1u << mantissaBits);
	if (exponent == 0)
		return std::ldexp(fraction, -14);
	if (exponent == 0x1F)
		return mantissa ? 0.0f : std::numeric_limits<float>::infinity();
	return std::ldexp(1.0f + fraction, (int)exponent - 15);
}

float DecodeHalf(uint16 bits)
{
	const float magnitude = DecodeSmallFloat(bits & 0x7FFFu, 10);
	return (bits & 0x8000u) ? -magnitude : magnitude;
}

class Encoder
{
public:
	~Encoder() { Close(); }

	bool Open(const Request& request, ActiveInfo& info, std::string& error)
	{
		if (!GetFfmpegApi().Load())
		{
			error = "The packaged FFmpeg runtime could not be loaded";
			return false;
		}

		const uint32 sourceWidth = request.padView ? 854 : 1280;
		const uint32 sourceHeight = 720 - (request.padView ? 240 : 0);
		uint32 width = request.width ? request.width : sourceWidth;
		uint32 height = request.height ? request.height : sourceHeight;
		const double sourceAspect = (double)sourceWidth / sourceHeight;
		if (request.width && request.height)
		{
			uint32 fitHeight = (uint32)std::lround(width / sourceAspect);
			if (fitHeight <= height)
				height = fitHeight;
			else
				width = (uint32)std::lround(height * sourceAspect);
		}
		width = std::clamp(width & ~1u, 2u, 4096u);
		height = std::clamp(height & ~1u, 2u, 4096u);

		int result = GetFfmpegApi().p_avformat_alloc_output_context2(&m_format, nullptr, "mpegts", request.url.c_str());
		if (result < 0 || !m_format)
			return Fail("Could not create MPEG-TS output", result, error);
		m_format->flags |= AVFMT_FLAG_FLUSH_PACKETS;
		m_format->max_delay = 0;

		if (!OpenVideo(width, height, request, error))
			return false;
		if (request.audioEnabled && !OpenAudio(error))
			return false;

		if (!(m_format->oformat->flags & AVFMT_NOFILE))
		{
			AVDictionary* ioOptions = nullptr;
			GetFfmpegApi().p_av_dict_set(&ioOptions, "buffer_size", "65536", 0);
			result = GetFfmpegApi().p_avio_open2(&m_format->pb, request.url.c_str(), AVIO_FLAG_WRITE, nullptr, &ioOptions);
			GetFfmpegApi().p_av_dict_free(&ioOptions);
			if (result < 0)
				return Fail("Could not open UDP stream target", result, error);
		}

		AVDictionary* formatOptions = nullptr;
		GetFfmpegApi().p_av_dict_set(&formatOptions, "flush_packets", "1", 0);
		GetFfmpegApi().p_av_dict_set(&formatOptions, "mpegts_flags", "resend_headers", 0);
		result = GetFfmpegApi().p_avformat_write_header(m_format, &formatOptions);
		GetFfmpegApi().p_av_dict_free(&formatOptions);
		if (result < 0)
			return Fail("Could not start MPEG-TS stream", result, error);
		m_headerWritten = true;

		info = {
			.padView = request.padView,
			.streamType = request.streamType,
			.url = request.url,
			.width = (uint16)width,
			.height = (uint16)height,
			.fps = request.fps,
			.audioEnabled = request.audioEnabled,
		};
		return true;
	}

	bool EncodeVideo(const RawFrame& frame, std::string& error)
	{
		if (!m_videoContext || frame.width == 0 || frame.height == 0 || frame.pixels.empty())
			return true;

		AVPixelFormat sourceFormat = AV_PIX_FMT_NONE;
		const uint8* source = frame.pixels.data();
		uint32 sourceStride = frame.rowBytes;
		switch ((DXGI_FORMAT)frame.dxgiFormat)
		{
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_SNORM:
			sourceFormat = AV_PIX_FMT_RGBA;
			break;
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			sourceFormat = AV_PIX_FMT_BGRA;
			break;
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			m_normalizedPixels.resize((size_t)frame.width * frame.height * 4);
			for (uint32 y = 0; y < frame.height; ++y)
			{
				const uint8* row = source + (size_t)y * sourceStride;
				uint8* output = m_normalizedPixels.data() + (size_t)y * frame.width * 4;
				for (uint32 x = 0; x < frame.width; ++x)
				{
					uint32 value = 0;
					std::memcpy(&value, row + x * 4, 4);
					output[x * 4 + 0] = (uint8)((((value >> 0) & 1023u) * 255u + 511u) / 1023u);
					output[x * 4 + 1] = (uint8)((((value >> 10) & 1023u) * 255u + 511u) / 1023u);
					output[x * 4 + 2] = (uint8)((((value >> 20) & 1023u) * 255u + 511u) / 1023u);
					output[x * 4 + 3] = 255;
				}
			}
			source = m_normalizedPixels.data();
			sourceStride = frame.width * 4;
			sourceFormat = AV_PIX_FMT_RGBA;
			break;
		case DXGI_FORMAT_R16G16B16A16_UNORM:
			m_normalizedPixels.resize((size_t)frame.width * frame.height * 4);
			for (uint32 y = 0; y < frame.height; ++y)
			{
				const uint16* row = reinterpret_cast<const uint16*>(source + (size_t)y * sourceStride);
				uint8* output = m_normalizedPixels.data() + (size_t)y * frame.width * 4;
				for (uint32 x = 0; x < frame.width; ++x)
				{
					output[x * 4 + 0] = (uint8)((row[x * 4 + 0] * 255u + 32767u) / 65535u);
					output[x * 4 + 1] = (uint8)((row[x * 4 + 1] * 255u + 32767u) / 65535u);
					output[x * 4 + 2] = (uint8)((row[x * 4 + 2] * 255u + 32767u) / 65535u);
					output[x * 4 + 3] = 255;
				}
			}
			source = m_normalizedPixels.data();
			sourceStride = frame.width * 4;
			sourceFormat = AV_PIX_FMT_RGBA;
			break;
		case DXGI_FORMAT_R11G11B10_FLOAT:
			m_normalizedPixels.resize((size_t)frame.width * frame.height * 4);
			for (uint32 y = 0; y < frame.height; ++y)
			{
				const uint8* row = source + (size_t)y * sourceStride;
				uint8* output = m_normalizedPixels.data() + (size_t)y * frame.width * 4;
				for (uint32 x = 0; x < frame.width; ++x)
				{
					uint32 value = 0;
					std::memcpy(&value, row + x * 4, 4);
					output[x * 4 + 0] = ToUnorm8(DecodeSmallFloat(value & 0x7FFu, 6));
					output[x * 4 + 1] = ToUnorm8(DecodeSmallFloat((value >> 11) & 0x7FFu, 6));
					output[x * 4 + 2] = ToUnorm8(DecodeSmallFloat((value >> 22) & 0x3FFu, 5));
					output[x * 4 + 3] = 255;
				}
			}
			source = m_normalizedPixels.data();
			sourceStride = frame.width * 4;
			sourceFormat = AV_PIX_FMT_RGBA;
			break;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			m_normalizedPixels.resize((size_t)frame.width * frame.height * 4);
			for (uint32 y = 0; y < frame.height; ++y)
			{
				const uint16* row = reinterpret_cast<const uint16*>(source + (size_t)y * sourceStride);
				uint8* output = m_normalizedPixels.data() + (size_t)y * frame.width * 4;
				for (uint32 x = 0; x < frame.width; ++x)
				{
					output[x * 4 + 0] = ToUnorm8(DecodeHalf(row[x * 4 + 0]));
					output[x * 4 + 1] = ToUnorm8(DecodeHalf(row[x * 4 + 1]));
					output[x * 4 + 2] = ToUnorm8(DecodeHalf(row[x * 4 + 2]));
					output[x * 4 + 3] = 255;
				}
			}
			source = m_normalizedPixels.data();
			sourceStride = frame.width * 4;
			sourceFormat = AV_PIX_FMT_RGBA;
			break;
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
			m_normalizedPixels.resize((size_t)frame.width * frame.height * 4);
			for (uint32 y = 0; y < frame.height; ++y)
			{
				const float* row = reinterpret_cast<const float*>(source + (size_t)y * sourceStride);
				uint8* output = m_normalizedPixels.data() + (size_t)y * frame.width * 4;
				for (uint32 x = 0; x < frame.width; ++x)
				{
					output[x * 4 + 0] = ToUnorm8(row[x * 4 + 0]);
					output[x * 4 + 1] = ToUnorm8(row[x * 4 + 1]);
					output[x * 4 + 2] = ToUnorm8(row[x * 4 + 2]);
					output[x * 4 + 3] = 255;
				}
			}
			source = m_normalizedPixels.data();
			sourceStride = frame.width * 4;
			sourceFormat = AV_PIX_FMT_RGBA;
			break;
		default:
			error = fmt::format("Unsupported D3D12 stream texture format {}", frame.dxgiFormat);
			return false;
		}

		if (!m_scaler || m_sourceWidth != frame.width || m_sourceHeight != frame.height || m_sourceFormat != sourceFormat)
		{
			if (m_scaler)
				GetFfmpegApi().p_sws_freeContext(m_scaler);
			m_scaler = GetFfmpegApi().p_sws_getContext((int)frame.width, (int)frame.height, sourceFormat,
				m_videoContext->width, m_videoContext->height, m_videoContext->pix_fmt,
				SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
			m_sourceWidth = frame.width;
			m_sourceHeight = frame.height;
			m_sourceFormat = sourceFormat;
			if (!m_scaler)
			{
				error = "Could not create the video scaler";
				return false;
			}
		}

		int result = GetFfmpegApi().p_av_frame_make_writable(m_videoFrame);
		if (result < 0)
			return Fail("Could not acquire a video frame", result, error);
		const uint8* sourcePlanes[4]{ source, nullptr, nullptr, nullptr };
		int sourceStrides[4]{ (int)sourceStride, 0, 0, 0 };
		result = GetFfmpegApi().p_sws_scale(m_scaler, sourcePlanes, sourceStrides, 0, (int)frame.height,
			m_videoFrame->data, m_videoFrame->linesize);
		if (result <= 0)
		{
			error = "Video conversion produced no output";
			return false;
		}
		m_videoFrame->pts = m_videoPts++;
		return SendFrame(m_videoContext, m_videoStream, m_videoFrame, error);
	}

	bool EncodeAudio(const sint16* stereoSamples, uint32 frames, std::string& error)
	{
		if (!m_audioContext || !m_resampler || frames != (uint32)m_audioContext->frame_size)
			return true;
		int result = GetFfmpegApi().p_av_frame_make_writable(m_audioFrame);
		if (result < 0)
			return Fail("Could not acquire an audio frame", result, error);
		const uint8* source[1]{ reinterpret_cast<const uint8*>(stereoSamples) };
		result = GetFfmpegApi().p_swr_convert(m_resampler, m_audioFrame->data, (int)frames, source, (int)frames);
		if (result < 0)
			return Fail("Could not convert stream audio", result, error);
		m_audioFrame->nb_samples = result;
		m_audioFrame->pts = m_audioPts;
		m_audioPts += result;
		return SendFrame(m_audioContext, m_audioStream, m_audioFrame, error);
	}

	uint32 AudioFrameSize() const
	{
		return m_audioContext ? (uint32)std::max(m_audioContext->frame_size, 1) : 0;
	}

	void Close()
	{
		std::string ignored;
		if (m_headerWritten)
		{
			if (m_videoContext)
				SendFrame(m_videoContext, m_videoStream, nullptr, ignored);
			if (m_audioContext)
				SendFrame(m_audioContext, m_audioStream, nullptr, ignored);
			GetFfmpegApi().p_av_write_trailer(m_format);
		}
		m_headerWritten = false;
		if (m_format && m_format->pb)
			GetFfmpegApi().p_avio_closep(&m_format->pb);
		if (m_scaler)
			GetFfmpegApi().p_sws_freeContext(std::exchange(m_scaler, nullptr));
		if (m_resampler)
			GetFfmpegApi().p_swr_free(&m_resampler);
		if (m_videoFrame)
			GetFfmpegApi().p_av_frame_free(&m_videoFrame);
		if (m_audioFrame)
			GetFfmpegApi().p_av_frame_free(&m_audioFrame);
		if (m_packet)
			GetFfmpegApi().p_av_packet_free(&m_packet);
		if (m_videoContext)
			GetFfmpegApi().p_avcodec_free_context(&m_videoContext);
		if (m_audioContext)
			GetFfmpegApi().p_avcodec_free_context(&m_audioContext);
		if (m_format)
			GetFfmpegApi().p_avformat_free_context(std::exchange(m_format, nullptr));
		m_videoStream = nullptr;
		m_audioStream = nullptr;
	}

private:
	bool OpenVideo(uint32 width, uint32 height, const Request& request, std::string& error)
	{
		const AVCodec* codec = GetFfmpegApi().p_avcodec_find_encoder_by_name("h264_mf");
		if (!codec)
		{
			error = "The FFmpeg Media Foundation H.264 encoder is unavailable";
			return false;
		}
		m_videoContext = GetFfmpegApi().p_avcodec_alloc_context3(codec);
		if (!m_videoContext)
		{
			error = "Could not allocate the H.264 encoder";
			return false;
		}
		m_videoContext->codec_type = AVMEDIA_TYPE_VIDEO;
		m_videoContext->codec_id = codec->id;
		m_videoContext->width = (int)width;
		m_videoContext->height = (int)height;
		m_videoContext->pix_fmt = AV_PIX_FMT_NV12;
		m_videoContext->time_base = { 1, std::max<int>(request.fps, 1) };
		m_videoContext->framerate = { std::max<int>(request.fps, 1), 1 };
		m_videoContext->bit_rate = (int64_t)std::max<uint32>(request.bitrateKbps, 500u) * 1000;
		m_videoContext->gop_size = std::max<int>(request.fps, 1);
		m_videoContext->max_b_frames = 0;
		if (m_format->oformat->flags & AVFMT_GLOBALHEADER)
			m_videoContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

		AVDictionary* options = nullptr;
		GetFfmpegApi().p_av_dict_set(&options, "rate_control", "cbr", 0);
		GetFfmpegApi().p_av_dict_set(&options, "scenario", "live_streaming", 0);
		int result = GetFfmpegApi().p_avcodec_open2(m_videoContext, codec, &options);
		GetFfmpegApi().p_av_dict_free(&options);
		if (result < 0)
			return Fail("Could not open the Media Foundation H.264 encoder", result, error);

		m_videoStream = GetFfmpegApi().p_avformat_new_stream(m_format, nullptr);
		if (!m_videoStream)
		{
			error = "Could not create the MPEG-TS video stream";
			return false;
		}
		m_videoStream->time_base = m_videoContext->time_base;
		result = GetFfmpegApi().p_avcodec_parameters_from_context(m_videoStream->codecpar, m_videoContext);
		if (result < 0)
			return Fail("Could not publish H.264 stream parameters", result, error);

		m_videoFrame = GetFfmpegApi().p_av_frame_alloc();
		m_packet = GetFfmpegApi().p_av_packet_alloc();
		if (!m_videoFrame || !m_packet)
		{
			error = "Could not allocate FFmpeg video buffers";
			return false;
		}
		m_videoFrame->format = m_videoContext->pix_fmt;
		m_videoFrame->width = m_videoContext->width;
		m_videoFrame->height = m_videoContext->height;
		result = GetFfmpegApi().p_av_frame_get_buffer(m_videoFrame, 32);
		if (result < 0)
			return Fail("Could not allocate the NV12 video frame", result, error);
		return true;
	}

	bool OpenAudio(std::string& error)
	{
		const AVCodec* codec = GetFfmpegApi().p_avcodec_find_encoder_by_name("aac");
		if (!codec)
		{
			error = "The FFmpeg AAC encoder is unavailable";
			return false;
		}
		m_audioContext = GetFfmpegApi().p_avcodec_alloc_context3(codec);
		if (!m_audioContext)
		{
			error = "Could not allocate the AAC encoder";
			return false;
		}
		m_audioContext->codec_type = AVMEDIA_TYPE_AUDIO;
		m_audioContext->codec_id = codec->id;
		m_audioContext->sample_rate = 48000;
		m_audioContext->sample_fmt = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
		m_audioContext->time_base = { 1, m_audioContext->sample_rate };
		m_audioContext->bit_rate = 128000;
		GetFfmpegApi().p_av_channel_layout_default(&m_audioContext->ch_layout, 2);
		if (m_format->oformat->flags & AVFMT_GLOBALHEADER)
			m_audioContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		int result = GetFfmpegApi().p_avcodec_open2(m_audioContext, codec, nullptr);
		if (result < 0)
			return Fail("Could not open the AAC encoder", result, error);

		m_audioStream = GetFfmpegApi().p_avformat_new_stream(m_format, nullptr);
		if (!m_audioStream)
		{
			error = "Could not create the MPEG-TS audio stream";
			return false;
		}
		m_audioStream->time_base = m_audioContext->time_base;
		result = GetFfmpegApi().p_avcodec_parameters_from_context(m_audioStream->codecpar, m_audioContext);
		if (result < 0)
			return Fail("Could not publish AAC stream parameters", result, error);

		m_audioFrame = GetFfmpegApi().p_av_frame_alloc();
		if (!m_audioFrame)
		{
			error = "Could not allocate the AAC frame";
			return false;
		}
		m_audioFrame->format = m_audioContext->sample_fmt;
		m_audioFrame->sample_rate = m_audioContext->sample_rate;
		m_audioFrame->nb_samples = std::max(m_audioContext->frame_size, 1);
		result = GetFfmpegApi().p_av_channel_layout_copy(&m_audioFrame->ch_layout, &m_audioContext->ch_layout);
		if (result < 0)
			return Fail("Could not configure the AAC channel layout", result, error);
		result = GetFfmpegApi().p_av_frame_get_buffer(m_audioFrame, 0);
		if (result < 0)
			return Fail("Could not allocate the AAC sample frame", result, error);

		AVChannelLayout sourceLayout{};
		GetFfmpegApi().p_av_channel_layout_default(&sourceLayout, 2);
		result = GetFfmpegApi().p_swr_alloc_set_opts2(&m_resampler,
			&m_audioContext->ch_layout, m_audioContext->sample_fmt, m_audioContext->sample_rate,
			&sourceLayout, AV_SAMPLE_FMT_S16, 48000, 0, nullptr);
		GetFfmpegApi().p_av_channel_layout_uninit(&sourceLayout);
		if (result < 0 || !m_resampler)
			return Fail("Could not create the audio converter", result, error);
		result = GetFfmpegApi().p_swr_init(m_resampler);
		if (result < 0)
			return Fail("Could not initialize the audio converter", result, error);
		return true;
	}

	bool SendFrame(AVCodecContext* codec, AVStream* stream, AVFrame* frame, std::string& error)
	{
		int result = GetFfmpegApi().p_avcodec_send_frame(codec, frame);
		if (result < 0)
			return Fail("FFmpeg rejected an encoder frame", result, error);
		while (true)
		{
			result = GetFfmpegApi().p_avcodec_receive_packet(codec, m_packet);
			if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
				return true;
			if (result < 0)
				return Fail("FFmpeg could not receive an encoded packet", result, error);
			GetFfmpegApi().p_av_packet_rescale_ts(m_packet, codec->time_base, stream->time_base);
			m_packet->stream_index = stream->index;
			result = GetFfmpegApi().p_av_interleaved_write_frame(m_format, m_packet);
			GetFfmpegApi().p_av_packet_unref(m_packet);
			if (result < 0)
				return Fail("The UDP MPEG-TS write failed", result, error);
		}
	}

	bool Fail(std::string_view context, int code, std::string& error)
	{
		error = code < 0 ? fmt::format("{}: {}", context, GetFfmpegApi().Error(code)) : std::string(context);
		return false;
	}

	AVFormatContext* m_format = nullptr;
	AVCodecContext* m_videoContext = nullptr;
	AVCodecContext* m_audioContext = nullptr;
	AVStream* m_videoStream = nullptr;
	AVStream* m_audioStream = nullptr;
	AVFrame* m_videoFrame = nullptr;
	AVFrame* m_audioFrame = nullptr;
	AVPacket* m_packet = nullptr;
	SwsContext* m_scaler = nullptr;
	SwrContext* m_resampler = nullptr;
	bool m_headerWritten = false;
	uint32 m_sourceWidth = 0;
	uint32 m_sourceHeight = 0;
	AVPixelFormat m_sourceFormat = AV_PIX_FMT_NONE;
	std::vector<uint8> m_normalizedPixels;
	int64_t m_videoPts = 0;
	int64_t m_audioPts = 0;
};

std::mutex s_mutex;
std::condition_variable s_wake;
std::thread s_worker;
std::unique_ptr<Encoder> s_encoder;
std::optional<ActiveInfo> s_active;
std::optional<RawFrame> s_latestFrame;
std::vector<std::vector<uint8>> s_videoFramePool;
std::deque<sint16> s_audioSamples;
std::optional<std::string> s_failure;
bool s_stopRequested = false;
std::chrono::steady_clock::time_point s_nextCapture{};
constexpr size_t kMaxQueuedAudioSamples = 48000 * 2;
constexpr size_t kMaxPooledVideoFrameBuffers = 3;
constexpr size_t kMaxPooledVideoFrameBytes = 64u * 1024u * 1024u;

size_t GetPooledVideoFrameBytesLocked()
{
	size_t bytes = 0;
	for (const auto& buffer : s_videoFramePool)
		bytes += buffer.capacity();
	return bytes;
}

void RecycleVideoFrameBufferLocked(std::vector<uint8>&& buffer)
{
	buffer.clear();
	const size_t capacity = buffer.capacity();
	if (capacity == 0 || capacity > kMaxPooledVideoFrameBytes)
		return;

	size_t pooledBytes = GetPooledVideoFrameBytesLocked();
	while (!s_videoFramePool.empty() &&
		(s_videoFramePool.size() >= kMaxPooledVideoFrameBuffers ||
		 pooledBytes + capacity > kMaxPooledVideoFrameBytes))
	{
		auto smallest = std::min_element(s_videoFramePool.begin(), s_videoFramePool.end(),
			[](const auto& lhs, const auto& rhs) { return lhs.capacity() < rhs.capacity(); });
		if (capacity <= smallest->capacity())
			return;
		pooledBytes -= smallest->capacity();
		s_videoFramePool.erase(smallest);
	}

	if (s_videoFramePool.size() < kMaxPooledVideoFrameBuffers &&
		pooledBytes + capacity <= kMaxPooledVideoFrameBytes)
	{
		s_videoFramePool.emplace_back(std::move(buffer));
	}
}

void ReleaseVideoFramePoolLocked()
{
	s_videoFramePool.clear();
}

void WorkerMain()
{
	std::vector<sint16> audioFrame;
	while (true)
	{
		std::optional<RawFrame> video;
		uint32 audioFrames = 0;
		{
			std::unique_lock lock(s_mutex);
			const uint32 frameSize = s_encoder ? s_encoder->AudioFrameSize() : 0;
			s_wake.wait(lock, [frameSize] {
				return s_stopRequested || s_latestFrame.has_value() ||
					(frameSize != 0 && s_audioSamples.size() >= (size_t)frameSize * 2);
			});
			if (s_stopRequested)
				break;
			video = std::exchange(s_latestFrame, std::nullopt);
			if (frameSize != 0 && s_audioSamples.size() >= (size_t)frameSize * 2)
			{
				audioFrames = frameSize;
				audioFrame.resize((size_t)frameSize * 2);
				for (sint16& sample : audioFrame)
				{
					sample = s_audioSamples.front();
					s_audioSamples.pop_front();
				}
			}
		}

		std::string error;
		bool success = true;
		if (video)
			success = s_encoder->EncodeVideo(*video, error);
		if (success && audioFrames)
			success = s_encoder->EncodeAudio(audioFrame.data(), audioFrames, error);
		if (video)
		{
			std::lock_guard lock(s_mutex);
			RecycleVideoFrameBufferLocked(std::move(video->pixels));
		}
		if (!success)
		{
			cemuLog_log(LogType::Force, "Media stream stopped: {}", error);
			std::lock_guard lock(s_mutex);
			if (s_latestFrame)
				RecycleVideoFrameBufferLocked(std::move(s_latestFrame->pixels));
			s_latestFrame.reset();
			ReleaseVideoFramePoolLocked();
			s_failure = std::move(error);
			s_active.reset();
			s_stopRequested = true;
			break;
		}
	}
}
} // namespace

bool Start(const Request& request, ActiveInfo& activeInfo, std::string& error)
{
	Stop();
	if (!request.url.starts_with("udp://"))
	{
		error = "Only UDP stream targets are supported";
		return false;
	}
	if (request.fps == 0 || request.fps > 60 || request.width > 4096 || request.height > 4096)
	{
		error = "The requested stream dimensions or frame rate are invalid";
		return false;
	}

	auto encoder = std::make_unique<Encoder>();
	if (!encoder->Open(request, activeInfo, error))
		return false;
	{
		std::lock_guard lock(s_mutex);
		s_encoder = std::move(encoder);
		s_active = activeInfo;
		if (s_latestFrame)
			RecycleVideoFrameBufferLocked(std::move(s_latestFrame->pixels));
		s_latestFrame.reset();
		ReleaseVideoFramePoolLocked();
		s_audioSamples.clear();
		s_failure.reset();
		s_stopRequested = false;
		s_nextCapture = std::chrono::steady_clock::now();
	}
	s_worker = std::thread(WorkerMain);
	cemuLog_logDebug(LogType::Force, "Companion stream active: {} {}x{}@{} audio={} url={}",
		activeInfo.padView ? "GamePad" : "TV", activeInfo.width, activeInfo.height,
		activeInfo.fps, activeInfo.audioEnabled ? 1 : 0, activeInfo.url);
	return true;
}

void Stop()
{
	{
		std::lock_guard lock(s_mutex);
		s_stopRequested = true;
		s_active.reset();
	}
	s_wake.notify_all();
	if (s_worker.joinable() && s_worker.get_id() != std::this_thread::get_id())
		s_worker.join();
	{
		std::lock_guard lock(s_mutex);
		s_encoder.reset();
		if (s_latestFrame)
			RecycleVideoFrameBufferLocked(std::move(s_latestFrame->pixels));
		s_latestFrame.reset();
		ReleaseVideoFramePoolLocked();
		s_audioSamples.clear();
		s_stopRequested = false;
	}
}

bool IsActive()
{
	std::lock_guard lock(s_mutex);
	return s_active.has_value();
}

std::optional<ActiveInfo> GetActiveInfo()
{
	std::lock_guard lock(s_mutex);
	return s_active;
}

std::optional<std::string> TakeFailure()
{
	std::lock_guard lock(s_mutex);
	return std::exchange(s_failure, std::nullopt);
}

bool RequestVideoCapture(bool padView)
{
	std::lock_guard lock(s_mutex);
	if (!s_active || s_active->padView != padView || s_stopRequested)
		return false;
	const auto now = std::chrono::steady_clock::now();
	if (now < s_nextCapture)
		return false;
	const auto interval = std::chrono::microseconds(1000000 / std::max<uint8>(s_active->fps, 1));
	s_nextCapture = std::max(s_nextCapture + interval, now);
	return true;
}

std::vector<uint8> AcquireVideoFrameBuffer(std::size_t size)
{
	std::vector<uint8> buffer;
	{
		std::lock_guard lock(s_mutex);
		size_t bestIndex = s_videoFramePool.size();
		for (size_t index = 0; index < s_videoFramePool.size(); ++index)
		{
			const size_t capacity = s_videoFramePool[index].capacity();
			if (capacity >= size &&
				(bestIndex == s_videoFramePool.size() || capacity < s_videoFramePool[bestIndex].capacity()))
			{
				bestIndex = index;
			}
		}
		if (bestIndex == s_videoFramePool.size() && !s_videoFramePool.empty())
		{
			bestIndex = (size_t)std::distance(s_videoFramePool.begin(),
				std::max_element(s_videoFramePool.begin(), s_videoFramePool.end(),
					[](const auto& lhs, const auto& rhs) { return lhs.capacity() < rhs.capacity(); }));
		}
		if (bestIndex != s_videoFramePool.size())
		{
			buffer = std::move(s_videoFramePool[bestIndex]);
			s_videoFramePool.erase(s_videoFramePool.begin() + bestIndex);
		}
	}
	buffer.resize(size);
	return buffer;
}

void SubmitVideoFrame(bool padView, uint32 width, uint32 height, uint32 rowBytes,
	uint32 dxgiFormat, std::vector<uint8> pixels)
{
	std::lock_guard lock(s_mutex);
	if (!s_active || s_stopRequested)
		return;
	if (s_active->padView != padView)
	{
		RecycleVideoFrameBufferLocked(std::move(pixels));
		return;
	}
	if (s_latestFrame)
		RecycleVideoFrameBufferLocked(std::move(s_latestFrame->pixels));
	s_latestFrame = RawFrame{ padView, width, height, rowBytes, dxgiFormat, std::move(pixels) };
	s_wake.notify_one();
}

void SubmitAudioSamples(const sint16* samples, uint32 frames, uint32 channels, uint32 sampleRate)
{
	if (!samples || frames == 0 || channels == 0 || sampleRate != 48000)
		return;
	std::lock_guard lock(s_mutex);
	if (!s_active || !s_active->audioEnabled || s_stopRequested)
		return;
	for (uint32 frame = 0; frame < frames; ++frame)
	{
		const sint16 left = samples[(size_t)frame * channels];
		const sint16 right = channels > 1 ? samples[(size_t)frame * channels + 1] : left;
		s_audioSamples.push_back(left);
		s_audioSamples.push_back(right);
	}
	while (s_audioSamples.size() > kMaxQueuedAudioSamples)
		s_audioSamples.pop_front();
	s_wake.notify_one();
}
}

#endif
