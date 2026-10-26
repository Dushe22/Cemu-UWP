#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/h264_avc/parser/H264Parser.h"
#include "Cafe/OS/libs/h264_avc/H264DecInternal.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "Cafe/CafeSystem.h"

#include "h264dec.h"

enum class H264DEC_STATUS : uint32
{
	SUCCESS = 0x0,
	BAD_STREAM = 0x1000000,
	INVALID_PARAM = 0x1010000,
};

namespace H264
{
#if defined(CEMU_UWP)
	H264UwpStats g_h264UwpStats;

	static void H264UwpReportStats()
	{
		static auto s_lastReport = std::chrono::steady_clock::now();
		const auto now = std::chrono::steady_clock::now();
		if (now - s_lastReport < std::chrono::seconds(5))
			return;
		s_lastReport = now;
		auto& s = g_h264UwpStats;
		const uint32 submitted = s.submitted.exchange(0);
		const uint32 decoded = s.decodedFrames.exchange(0);
		const uint64 decodeUs = s.decodeUs.exchange(0);
		const uint32 decodeMax = s.decodeUsMax.exchange(0);
		const uint32 slow = s.slowDecodes.exchange(0);
		const uint32 outputs = s.outputFrames.exchange(0);
		const uint64 copyUs = s.copyUs.exchange(0);
		const uint32 waits = s.gameWaits.exchange(0);
		const uint64 waitUs = s.gameWaitUs.exchange(0);
		const uint32 waitMax = s.gameWaitUsMax.exchange(0);
		const uint32 queuedMax = s.queuedMax.exchange(0);
		cemuLog_log(LogType::Force, "UWP video: {}x{} {} mode, {} submitted, {} decoded (avg {:.1f} ms, max {:.1f} ms, {} over 33 ms), {} output (copy avg {:.2f} ms), game waited {} times ({:.0f} ms total, max {:.1f} ms), decoder queue max {}; begin {} end {}, refused {} while waiting for an IDR frame, recovered {}, longest gap between frames {:.0f} ms, timestamps going backwards {}",
			s.width.load(), s.height.load(), s.buffered.load() ? "buffered" : "per-frame", submitted, decoded,
			decoded ? decodeUs / 1000.0 / decoded : 0.0, decodeMax / 1000.0, slow, outputs, outputs ? copyUs / 1000.0 / outputs : 0.0,
			waits, waitUs / 1000.0, waitMax / 1000.0, queuedMax,
			s.begins.exchange(0), s.ends.exchange(0), s.rejectedRecovering.exchange(0), s.recoveredAtIdr.exchange(0),
			s.executeGapUsMax.exchange(0) / 1000.0, s.backwardTimestamps.exchange(0));
	}
#endif

#if defined(CEMU_UWP)
	// call-by-call trace of the video API: 400 lines per burst, a new burst after 2 s without a decode call
	static uint32 s_h264TraceLines = 0;
	static std::chrono::steady_clock::time_point s_h264TraceLastCall{};
	template<typename... Args>
	static void H264UwpTrace(fmt::format_string<Args...> format, Args&&... args)
	{
		const auto now = std::chrono::steady_clock::now();
		if (now - s_h264TraceLastCall > std::chrono::seconds(2))
			s_h264TraceLines = 0;
		s_h264TraceLastCall = now;
		if (s_h264TraceLines >= 400)
			return;
		if (++s_h264TraceLines == 400)
			cemuLog_log(LogType::Force, "UWP video trace: limit reached until the next pause in decode calls");
		cemuLog_log(LogType::Force, "UWP video trace: {}", fmt::format(format, std::forward<Args>(args)...));
	}
#endif

	bool H264_IsBotW()
	{
		// Cemuhook has a hack where it always returns a small size for H264DECMemoryRequirement (256 bytes)
		// it also outputs images pre-cropped instead of giving the game raw uncropped images
		// both of these are required to allow Breath of the Wild to playback the higher res (1080p) videos from the Switch version
		// we mirror these hacks for user convenience and because there are no downsides
		uint64 currentTitleId = CafeSystem::GetForegroundTitleId();
		if (currentTitleId == 0x00050000101c9500 || currentTitleId == 0x00050000101c9400 || currentTitleId == 0x00050000101c9300)
			return true;
		return false;
	}

	struct H264Context
	{
		struct
		{
			MEMPTR<void> ptr{ nullptr };
			uint32be length{ 0 };
			float64be timestamp;
		}BitStream;
		struct
		{
			MEMPTR<void> outputFunc{ nullptr };
			uint8be outputPerFrame{ 0 }; // default is 0
			MEMPTR<void> userMemoryParam{ nullptr };
		}Param;
		// misc
		uint32be sessionHandle;

		// decoder state
		struct
		{
			uint32 numFramesInFlight{0};
			bool isFirstBegin{true};
			bool isTryingToRecover{false};
		}decoderState;
	};

	H264DEC_STATUS H264DECMemoryRequirement(uint32 codecProfile, uint32 codecLevel, uint32 width, uint32 height, uint32be* sizeRequirementOut)
	{
		if (H264_IsBotW())
		{
			static_assert(sizeof(H264Context) < 256);
			*sizeRequirementOut = 256;
			return H264DEC_STATUS::SUCCESS;
		}

		// note: On console this seems to check if maxWidth or maxHeight < 64 but Pikmin 3 passes 32x32 and crashes if this function fails ?
		if (width < 32 || height < 32 || width > 2800 || height > 1408 || !sizeRequirementOut || codecLevel >= 52 || (codecProfile != 66 && codecProfile != 77 && codecProfile != 100))
			return H264DEC_STATUS::INVALID_PARAM;

		uint32 workbufferSize = 0;
		if (codecLevel < 0xB)
		{
			workbufferSize = 0x18C << 10;
		}
		else if (codecLevel == 0xB)
		{
			workbufferSize = 0x384 << 10;
		}
		else if (codecLevel >= 0xC && codecLevel <= 0x14)
		{
			workbufferSize = 0x948 << 10;
		}
		else if (codecLevel == 0x15)
		{
			workbufferSize = 0x1290 << 10;
		}
		else if (codecLevel >= 0x16 && codecLevel <= 0x1E)
		{
			workbufferSize = 0x1FA4 << 10;
		}
		else if (codecLevel == 0x1F)
		{
			workbufferSize = 0x4650 << 10;
		}
		else if (codecLevel == 0x20)
		{
			workbufferSize = 0x1400000;
		}
		else if (codecLevel >= 0x21 && codecLevel <= 0x29)
		{
			workbufferSize = 0x8000 << 10;
		}
		else if (codecLevel == 0x2A)
		{
			workbufferSize = 0x2200000;
		}
		else if (codecLevel >= 0x2B && codecLevel <= 0x32)
		{
			workbufferSize = 0x1AF40 << 10;
		}
		else if (codecLevel >= 0x33)
		{
			workbufferSize = 0x2D000 << 10;
		}
		workbufferSize += 0x447;
		*sizeRequirementOut = workbufferSize;
		return H264DEC_STATUS::SUCCESS;
	}

	uint32 H264DECCheckMemSegmentation(MPTR memory, uint32 size)
	{
		// return 0 for valid, 1 for invalid
		// currently we allow any range
		return 0;
	}

	H264DEC_STATUS H264DECFindDecstartpoint(uint8* ptr, uint32 length, uint32be* offsetOut)
	{
		if (!ptr || length < 4 || !offsetOut)
			return H264DEC_STATUS::INVALID_PARAM;
#if defined(CEMU_UWP)
		for (uint32 i = 0; i + 3 <= length; ++i)
		{
			const uint32 startCodeLength = NALInputBitstream::GetStartCodeLength(ptr + i, length - i);
			if (startCodeLength == 0 || i + startCodeLength >= length)
				continue;
			if ((ptr[i + startCodeLength] & 0x9F) != 7)
				continue;
			*offsetOut = i > 0 ? i - 1 : 0;
			return H264DEC_STATUS::SUCCESS;
		}
#else
		for (uint32 i = 0; i < length - 4; ++i)
		{
			uint8 b = ptr[i];
			if (b != 0)
				continue;

			b = ptr[i + 1];
			if (b != 0)
				continue;

			b = ptr[i + 2];
			if (b != 1)
				continue;

			b = ptr[i + 3];
			b &= 0x9F;
			if (b != 7) // check for NAL type SPS
				continue;

			if (i > 0)
				*offsetOut = i - 1;
			else
				*offsetOut = 0;

			return H264DEC_STATUS::SUCCESS;
		}
#endif
		return H264DEC_STATUS::BAD_STREAM;
	}

	H264DEC_STATUS H264DECFindIdrpoint(uint8* ptr, uint32 length, uint32be* offsetOut)
	{
		if (!ptr || length < 4 || !offsetOut)
			return H264DEC_STATUS::INVALID_PARAM;

#if defined(CEMU_UWP)
		for (uint32 i = 0; i + 3 <= length; ++i)
		{
			const uint32 startCodeLength = NALInputBitstream::GetStartCodeLength(ptr + i, length - i);
			if (startCodeLength == 0 || i + startCodeLength >= length)
				continue;
			const uint8 nalType = ptr[i + startCodeLength] & 0x9F;
			if (nalType != 5 && nalType != 7 && nalType != 8)
				continue;
			*offsetOut = i > 0 ? i - 1 : 0;
			return H264DEC_STATUS::SUCCESS;
		}
#else
		for (uint32 i = 0; i < length - 4; ++i)
		{
			uint8 b = ptr[i];
			if (b != 0)
				continue;

			b = ptr[i + 1];
			if (b != 0)
				continue;

			b = ptr[i + 2];
			if (b != 1)
				continue;

			b = ptr[i + 3];
			b &= 0x9F;
			if (b != 5 && b != 7 && b != 8) // check for NAL type IDR slice, but also accept SPS or PPS slices
				continue;

			if (i > 0)
				*offsetOut = i - 1;
			else
				*offsetOut = 0;

			return H264DEC_STATUS::SUCCESS;
		}
#endif
		return H264DEC_STATUS::BAD_STREAM;
	}

	struct H264Dec_SeqParameterSet
	{
		// for now this contains only the fields we care about
		uint32 pic_width_in_luma_samples{};
		uint32 pic_height_in_map_units_x16{};
		uint8 frame_mbs_only_flag{};
	};

	void H264Dec_parseScalingList(RBSPInputBitstream& stream, sint32 size)
	{
		sint32 lastScale = 8;
		sint32 nextScale = 8;
		for (sint32 i = 0; i < size; i++)
		{
			if (nextScale != 0)
			{
				sint32 deltaScale = stream.readSV_E();
				nextScale = (lastScale + deltaScale + 256) % 256;
			}
			if (nextScale != 0)
				lastScale = nextScale;
		}
	}

	bool H264Dec_parseSequencePS(RBSPInputBitstream& stream, H264Dec_SeqParameterSet& ps)
	{
		ps = {};
		uint32 profileIdc = stream.readBits<8>();
		if (profileIdc != 66 && profileIdc != 77 && profileIdc != 100)
			return false;
		stream.readBits<8>(); // constraint_set flags
		stream.readBits<8>(); // level_idc
		uint32 seqParameterSetId = stream.readUV_E();
		if (seqParameterSetId >= 32)
			return false;
		if (profileIdc == 100)
		{
			uint32 chromaFormatIdc = stream.readUV_E();
			if (chromaFormatIdc > 1)
				return false;
			stream.readUV_E(); // bit_depth_luma_minus8
			stream.readUV_E(); // bit_depth_chroma_minus8
			stream.readBit(); // qpprime_y_zero_transform_bypass_flag
			if (stream.readBit()) // seq_scaling_matrix_present_flag
			{
				for (sint32 i = 0; i < 8; i++)
				{
					if (stream.readBit())
						H264Dec_parseScalingList(stream, i < 6 ? 16 : 64);
				}
			}
		}
		uint32 log2MaxFrameNumMinus4 = stream.readUV_E();
		if (log2MaxFrameNumMinus4 > 12)
			return false;
		uint32 picOrderCntType = stream.readUV_E();
		if (picOrderCntType > 2)
			return false;
		if (picOrderCntType == 0)
		{
			uint32 log2MaxPicOrderCntLsbMinus4 = stream.readUV_E();
			if (log2MaxPicOrderCntLsbMinus4 > 12)
				return false;
		}
		else if (picOrderCntType == 1)
		{
			stream.readBit(); // delta_pic_order_always_zero_flag
			stream.readSV_E(); // offset_for_non_ref_pic
			stream.readSV_E(); // offset_for_top_to_bottom_field
			uint32 numRefFramesInPicOrderCntCycle = stream.readUV_E();
			if (numRefFramesInPicOrderCntCycle > 0xFF)
				return false;
			for (uint32 i = 0; i < numRefFramesInPicOrderCntCycle; i++)
				stream.readSV_E();
		}
		uint32 maxNumRefFrames = stream.readUV_E();
		if (maxNumRefFrames > 16)
			return false;
		stream.readBit(); // gaps_in_frame_num_value_allowed_flag
		uint32 picWidthInMbsMinus1 = stream.readUV_E();
		uint32 width = 16 * (picWidthInMbsMinus1 + 1);
		if ((width & 0xFFF0) < 0x10)
			return false;
		uint32 picHeightInMapUnitsMinus1 = stream.readUV_E();
		uint32 heightInMapUnitsX16 = 16 * (picHeightInMapUnitsMinus1 + 1);
		if ((heightInMapUnitsX16 & 0xFFF0) < 0x10)
			return false;
		ps.pic_width_in_luma_samples = width;
		ps.pic_height_in_map_units_x16 = heightInMapUnitsX16;
		ps.frame_mbs_only_flag = stream.readBit();
		if (!ps.frame_mbs_only_flag)
			stream.readBit(); // mb_adaptive_frame_field_flag
		stream.readBit(); // direct_8x8_inference_flag
		if (stream.readBit()) // frame_cropping_flag
		{
			stream.readUV_E(); // frame_crop_left_offset
			stream.readUV_E(); // frame_crop_right_offset
			stream.readUV_E(); // frame_crop_top_offset
			stream.readUV_E(); // frame_crop_bottom_offset
		}
		stream.readBit(); // vui_parameters_present_flag
		// vui parsing unnecessary as we currently dont need any of the fields
		return true;
	}

	H264DEC_STATUS H264DECGetImageSize(uint8* stream, sint32 streamSize, sint32 offset, uint32be* outputWidth, uint32be* outputHeight)
	{
		if (!stream || streamSize < 4 || offset < 0 || !outputWidth || !outputHeight || offset >= streamSize)
			return H264DEC_STATUS::INVALID_PARAM;
		if ( (offset+4) >= streamSize )
			return H264DEC_STATUS::INVALID_PARAM;
		uint8* cur = stream + offset;
		uint8* end = stream + streamSize;
#if defined(CEMU_UWP)
		while (cur < end)
		{
			const uint32 startCodeLength = NALInputBitstream::GetStartCodeLength(cur, (uint32)(end - cur));
			if (startCodeLength == 0 || cur + startCodeLength >= end)
			{
				cur++;
				continue;
			}
			const uint8 nalHeader = cur[startCodeLength];
			if ((nalHeader & 0x1F) != 7)
			{
				cur++;
				continue;
			}
			uint8* spsStart = cur + startCodeLength + 1;
#else
		while (cur < end-2)
		{
			// check for start code
			if(*cur != 1)
			{
				cur++;
				continue;
			}
			// check if this is a valid NAL header
			if(cur[-2] != 0 || cur[-1] != 0 || cur[0] != 1) // if offset is < 2, this will read out of bounds. The console implementation has this behavior too so we have to replicate this bug
			{
				cur++;
				continue;
			}
			uint8 nalHeader = cur[1];
			if((nalHeader & 0x1F) != 7)
			{
				cur++;
				continue;
			}
			uint8* spsStart = cur + 2;
#endif
			uint32 spsLength = (uint32)(end - spsStart);
			H264Dec_SeqParameterSet psp;
			RBSPInputBitstream rbspStream(spsStart, spsLength, false);
			bool r = H264Dec_parseSequencePS(rbspStream, psp);
			if(!r)
			{
				cemuLog_log(LogType::Force, "H264DECGetImageSize: Invalid SPS data");
				cemu_assert_suspicious(); // should not happen
				return H264DEC_STATUS::BAD_STREAM;
			}
			*outputWidth = psp.pic_width_in_luma_samples;
			*outputHeight = psp.pic_height_in_map_units_x16;
			if (!psp.frame_mbs_only_flag)
				*outputHeight *= 2;
			if (!*outputHeight || !*outputWidth)
				return H264DEC_STATUS::BAD_STREAM;
			// BotW 1080p video support
			if (H264_IsBotW() && *outputWidth == 1920 && *outputHeight == 1088)
				*outputHeight = 1080;
			return H264DEC_STATUS::SUCCESS;
		}
		return H264DEC_STATUS::BAD_STREAM;
	}

	uint32 H264DECInitParam(uint32 workMemorySize, void* workMemory)
	{
		H264Context* ctx = (H264Context*)workMemory;
		*ctx = {};
		return 0;
	}

	std::unordered_map<uint32, H264DecoderBackend*> sDecoderSessions;
	std::mutex sDecoderSessionsMutex;
	std::atomic_uint32_t sCurrentSessionHandle{ 1 };

	H264DecoderBackend* CreateAVCDecoder();

	static H264DecoderBackend* _CreateDecoderSession(uint32& handleOut)
	{
		std::unique_lock _lock(sDecoderSessionsMutex);
		handleOut = sCurrentSessionHandle.fetch_add(1);
		H264DecoderBackend* session = CreateAVCDecoder();
		sDecoderSessions.try_emplace(handleOut, session);
		return session;
	}

	static H264DecoderBackend* _AcquireDecoderSession(uint32 handle)
	{
		std::unique_lock _lock(sDecoderSessionsMutex);
		auto it = sDecoderSessions.find(handle);
		if (it == sDecoderSessions.end())
			return nullptr;
		H264DecoderBackend* session = it->second;
		if (sDecoderSessions.size() >= 5)
		{
			cemuLog_log(LogType::Force, "H264: Warning - more than 5 active sessions");
			cemu_assert_suspicious();
		}
		return session;
	}

	static void _ReleaseDecoderSession(H264DecoderBackend* session)
	{
		std::unique_lock _lock(sDecoderSessionsMutex);

	}

	static void _DestroyDecoderSession(uint32 handle)
	{
		std::unique_lock _lock(sDecoderSessionsMutex);
		auto it = sDecoderSessions.find(handle);
		if (it == sDecoderSessions.end())
			return;
		H264DecoderBackend* session = it->second;
		session->Destroy();
		delete session;
		sDecoderSessions.erase(it);
	}

	uint32 H264DECOpen(void* workMemory)
	{
		H264Context* ctx = (H264Context*)workMemory;
		uint32 sessionHandle;
		_CreateDecoderSession(sessionHandle);
		ctx->sessionHandle = sessionHandle;
		return 0;
	}

	uint32 H264DECClose(void* workMemory)
	{
		if (workMemory)
		{
			H264Context* ctx = (H264Context*)workMemory;
			_DestroyDecoderSession(ctx->sessionHandle);
		}
		return 0;
	}

	uint32 H264DECBegin(void* workMemory)
	{
		H264Context* ctx = (H264Context*)workMemory;
		H264DecoderBackend* session = _AcquireDecoderSession(ctx->sessionHandle);
		if (!session)
		{
			cemuLog_log(LogType::Force, "H264DECBegin(): Invalid session");
			return 0;
		}
#if defined(CEMU_UWP)
		g_h264UwpStats.begins++;
#endif
#if defined(CEMU_UWP)
		H264UwpTrace("begin first={} inflight={} outputPerFrame={}", ctx->decoderState.isFirstBegin ? 1 : 0, ctx->decoderState.numFramesInFlight, (uint32)ctx->Param.outputPerFrame);
#endif
		if (ctx->decoderState.isFirstBegin)
		{
			session->Init(ctx->Param.outputPerFrame == 0);
			ctx->decoderState.isFirstBegin = false;
		}
		else
			ctx->decoderState.isTryingToRecover = true;
		//ctx->decoderState.numFramesInFlight = 0;
		_ReleaseDecoderSession(session);
		return 0;
	}

	void H264DoFrameOutputCallback(H264Context* ctx, H264DecoderBackend::DecodeResult& decodeResult);

	H264DEC_STATUS H264DECEnd(void* workMemory)
	{
		H264Context* ctx = (H264Context*)workMemory;
		H264DecoderBackend* session = _AcquireDecoderSession(ctx->sessionHandle);
		if (!session)
		{
			cemuLog_log(LogType::Force, "H264DECEnd(): Invalid session");
			return H264DEC_STATUS::SUCCESS;
		}
#if defined(CEMU_UWP)
		g_h264UwpStats.ends++;
#endif
		coreinit::OSEvent* flushEvt = &session->GetFlushEvent();
		coreinit::OSResetEvent(flushEvt);
		session->QueueFlush();
		coreinit::OSWaitEvent(flushEvt);
		while(true)
		{
			H264DecoderBackend::DecodeResult decodeResult;
			if( !session->GetFrameOutputIfReady(decodeResult) )
				break;
			// todo - output all frames in a single callback?
			H264DoFrameOutputCallback(ctx, decodeResult);
			ctx->decoderState.numFramesInFlight--;
		}
#if defined(CEMU_UWP)
		H264UwpTrace("end inflight after flush={}", (sint32)ctx->decoderState.numFramesInFlight);
#endif
		cemu_assert_debug(ctx->decoderState.numFramesInFlight == 0); // no frames should be in flight anymore. Exact behavior is not well understood but we may have to output dummy frames if necessary
		_ReleaseDecoderSession(session);
		// does not destroy decoder session, keeps it mostly intact so playback can resume after the next H264DECBegin call (potentially at a different location in the stream)
		return H264DEC_STATUS::SUCCESS;
	}

	H264DEC_STATUS H264DECSetParam_FPTR_OUTPUT(H264Context* ctx, void* outputFunc)
	{
		ctx->Param.outputFunc = outputFunc;
		return H264DEC_STATUS::SUCCESS;
	}

	H264DEC_STATUS H264DECSetParam_OUTPUT_PER_FRAME(H264Context* ctx, uint32 outputPerFrame)
	{
		ctx->Param.outputPerFrame = outputPerFrame != 0 ? 1 : 0;
		return H264DEC_STATUS::SUCCESS;
	}

	H264DEC_STATUS H264DECSetParam_USER_MEMORY(H264Context* ctx, MEMPTR<void*>* userMemoryParamPtr)
	{
		ctx->Param.userMemoryParam = *userMemoryParamPtr;
		return H264DEC_STATUS::SUCCESS;
	}

	H264DEC_STATUS H264DECSetParam(H264Context* ctx, uint32 paramId, void* paramValue)
	{
		const uint32 PARAMID_FPTR_OUTPUT = 0x1;
		const uint32 PARAMID_OUTPUT_PER_FRAME = 0x20000002;
		const uint32 PARAMID_USER_MEMORY = 0x70000001;
		const uint32 PARAMID_UKN = 0x20000030;

		if (paramId == PARAMID_FPTR_OUTPUT)
		{
			ctx->Param.outputFunc = paramValue;
		}
		else if (paramId == PARAMID_USER_MEMORY)
		{
			ctx->Param.userMemoryParam = paramValue;
		}
		else if (paramId == PARAMID_OUTPUT_PER_FRAME)
		{
			ctx->Param.outputPerFrame = *(uint8be*)paramValue != 0;
		}
		else if (paramId == PARAMID_UKN)
		{
			// unknown purpose, seen in MK8. paramValue points to a bool
		}
		else
		{
			cemuLog_log(LogType::Force, "h264Export_H264DECSetParam(): Unsupported parameterId 0x{:08x}\n", paramId);
			cemu_assert_unimplemented();
		}
		return H264DEC_STATUS::SUCCESS;
	}

	uint32 H264DECSetBitstream(void* workMemory, void* ptr, uint32 length, double timestamp)
	{
		H264Context* ctx = (H264Context*)workMemory;
		ctx->BitStream.ptr = ptr;
		ctx->BitStream.length = length;
		ctx->BitStream.timestamp = timestamp;
		return 0;
	}

	struct H264DECFrameOutput
	{
		/* +0x00 */ uint32be result;
		/* +0x04 */ uint32be padding04;
		/* +0x08 */ betype<double> timestamp;
		/* +0x10 */ uint32be frameWidth;
		/* +0x14 */ uint32be frameHeight;
		/* +0x18 */ uint32be bytesPerRow;
		/* +0x1C */ uint32be cropEnable;
		/* +0x20 */ uint32be cropTop;
		/* +0x24 */ uint32be cropBottom;
		/* +0x28 */ uint32be cropLeft;
		/* +0x2C */ uint32be cropRight;

		/* +0x30 */ uint32be ukn30;
		/* +0x34 */ uint32be ukn34;
		/* +0x38 */ uint32be ukn38;
		/* +0x3C */ uint32be ukn3C;
		/* +0x40 */ uint32be ukn40;

		/* +0x44 */ MEMPTR<uint8> imagePtr;

		/* +0x48 */ uint32 vuiEnable;
		/* +0x4C */ MPTR   vuiPtr;
		/* +0x50 */ sint32 unused[10];
	};

	struct H264OutputCBStruct
	{
		uint32be frameCount;
		MEMPTR<MEMPTR<H264DECFrameOutput>> resultArray;
		uint32be userParam;
	};

	static_assert(sizeof(H264OutputCBStruct) == 12);

	void H264DoFrameOutputCallback(H264Context* ctx, H264DecoderBackend::DecodeResult& decodeResult)
	{
		sint32 outputFrameCount = 1;

		cemu_assert(outputFrameCount < 8);

		StackAllocator<MEMPTR<void>, 8> stack_resultPtrArray;
		StackAllocator<H264DECFrameOutput, 8> stack_decodedFrameResult;

		for (sint32 i = 0; i < outputFrameCount; i++)
			stack_resultPtrArray[i] = &stack_decodedFrameResult + i;

		H264DECFrameOutput* frameOutput = &stack_decodedFrameResult + 0;
		memset(frameOutput, 0x00, sizeof(H264DECFrameOutput));
		frameOutput->imagePtr = (uint8*)decodeResult.imageOutput;
		frameOutput->result = 100;
		frameOutput->timestamp = decodeResult.timestamp;
#if defined(CEMU_UWP)
		{
			static double s_lastOutputTimestamp = -1.0;
			if (decodeResult.timestamp < s_lastOutputTimestamp)
				g_h264UwpStats.backwardTimestamps++;
			s_lastOutputTimestamp = decodeResult.timestamp;
		}
#endif
		frameOutput->frameWidth = decodeResult.frameWidth;
		frameOutput->frameHeight = decodeResult.frameHeight;
		frameOutput->bytesPerRow = decodeResult.bytesPerRow;
		frameOutput->cropEnable = decodeResult.cropEnable;
		frameOutput->cropTop = decodeResult.cropTop;
		frameOutput->cropBottom = decodeResult.cropBottom;
		frameOutput->cropLeft = decodeResult.cropLeft;
		frameOutput->cropRight = decodeResult.cropRight;

		StackAllocator<H264OutputCBStruct> stack_fptrOutputData;
		stack_fptrOutputData->frameCount = outputFrameCount;
		stack_fptrOutputData->resultArray = (MEMPTR<H264DECFrameOutput>*)stack_resultPtrArray.GetPointer();
		stack_fptrOutputData->userParam = ctx->Param.userMemoryParam.GetBEValue();

		// FPTR callback
		if (!ctx->Param.outputFunc.IsNull())
		{
			cemuLog_log(LogType::H264, "H264: Outputting frame via callback. Timestamp: {} Buffer 0x{:08x} UserParam 0x{:08x}", (double)decodeResult.timestamp, (uint32)frameOutput->imagePtr.GetMPTR(), ctx->Param.userMemoryParam.GetMPTR());
			PPCCoreCallback(ctx->Param.outputFunc.GetMPTR(), stack_fptrOutputData.GetMPTR());
		}
	}

	// find the first framedata slice (if there is any) and check if the type is idr
	bool IsIDRSlice(uint8* streamData, sint32 length)
	{
		NALInputBitstream nalStream(streamData, length);
		RBSPInputBitstream rbspStream;
		while (nalStream.getNextRBSP(rbspStream))
		{
			// parse NAL header
			uint8 nalHeaderByte = rbspStream.readU8();
			if ((nalHeaderByte & 0x80) == 0)
			{
				uint8 nal_unit_type = (nalHeaderByte >> 0) & 0x1f;
				if (nal_unit_type == 1) // non-idr
					return false;
				else if (nal_unit_type == 5) // idr
					return true;
			}
			else
			{
				cemu_assert_suspicious();
				return false; // corrupted stream?
			}
		}
		return false;
	}

#if defined(CEMU_UWP)
	// slice NAL count and first_mb_in_slice of the first slice: CheckDecunitLength assumes one slice per frame,
	// so an input with first_mb != 0 is a partial picture
	static std::string H264UwpDescribeSlices(uint8* streamData, sint32 length)
	{
		NALInputBitstream nalStream(streamData, length);
		RBSPInputBitstream rbspStream;
		uint32 slices = 0;
		sint64 firstMb = -1;
		std::string types;
		while (nalStream.getNextRBSP(rbspStream))
		{
			const uint8 header = rbspStream.readU8();
			const uint8 type = header & 0x1f;
			if (types.size() < 24)
				types += fmt::format("{}{}", types.empty() ? "" : ",", type);
			if (type == 1 || type == 5)
			{
				if (slices++ == 0)
					firstMb = rbspStream.readUV_E();
			}
		}
		return fmt::format("nal=[{}] slices={} firstMb={}", types, slices, firstMb);
	}
#endif

	uint32 H264DECExecute(void* workMemory, void* imageOutput)
	{
		cemuLog_log(LogType::H264, "H264DECExecute(): [BEGIN]");
		BenchmarkTimer bt;
		bt.Start();
		H264Context* ctx = (H264Context*)workMemory;
		H264DecoderBackend* session = _AcquireDecoderSession(ctx->sessionHandle);
		if (!session)
		{
			cemuLog_log(LogType::Force, "H264DECExecute(): Invalid session");
			return 0;
		}
#if defined(CEMU_UWP)
		{
			static std::chrono::steady_clock::time_point s_lastExecute{};
			const auto now = std::chrono::steady_clock::now();
			if (s_lastExecute.time_since_epoch().count() != 0)
				H264UwpStatsMax(g_h264UwpStats.executeGapUsMax, (uint32)std::min<sint64>(std::chrono::duration_cast<std::chrono::microseconds>(now - s_lastExecute).count(), 0xFFFFFFFF));
			s_lastExecute = now;
		}
#endif
		// if in recovery mode then return an error until we reach a IDR frame
		if (ctx->decoderState.isTryingToRecover)
		{
			bool isIDR = IsIDRSlice((uint8*)ctx->BitStream.ptr.GetPtr(), ctx->BitStream.length);
			if (isIDR)
			{
				ctx->decoderState.isTryingToRecover = false;
#if defined(CEMU_UWP)
				g_h264UwpStats.recoveredAtIdr++;
#endif
			}
			else
			{
#if defined(CEMU_UWP)
				g_h264UwpStats.rejectedRecovering++;
				H264UwpTrace("execute ts={:.3f} len={} {} refused (waiting for IDR) inflight={}", (double)ctx->BitStream.timestamp, (uint32)ctx->BitStream.length,
					H264UwpDescribeSlices((uint8*)ctx->BitStream.ptr.GetPtr(), ctx->BitStream.length), ctx->decoderState.numFramesInFlight);
				H264UwpReportStats();
#endif
				return 0x400; // error (figure out exact error code. Bit 0x80 indicates presence of frame?)
			}
		}
		// feed data to backend
		session->QueueForDecode((uint8*)ctx->BitStream.ptr.GetPtr(), ctx->BitStream.length, ctx->BitStream.timestamp, imageOutput);
		ctx->decoderState.numFramesInFlight++;
		// H264DECExecute is synchronous and will return a frame after either every call (non-buffered) or after 6 calls (buffered)
		// normally frame decoding happens only during H264DECExecute, but in order to hide the latency of our CPU decoder we will decode asynchronously in buffered mode
		uint32 numFramesToBuffer = (ctx->Param.outputPerFrame == 0) ? 5 : 0;
#if defined(CEMU_UWP)
		g_h264UwpStats.submitted++;
		uint32 waitedUs = 0;
		bool tracedExecute = false;
#endif
		if(ctx->decoderState.numFramesInFlight > numFramesToBuffer)
		{
			ctx->decoderState.numFramesInFlight--;
#if defined(CEMU_UWP)
			const auto waitStarted = std::chrono::steady_clock::now();
#endif
			while(true)
			{
				coreinit::OSEvent& evt = session->GetFrameOutputEvent();
				coreinit::OSWaitEvent(&evt);
				H264DecoderBackend::DecodeResult decodeResult;
				if( !session->GetFrameOutputIfReady(decodeResult) )
					continue;
#if defined(CEMU_UWP)
				// time to get the frame, before the game's output callback runs
				waitedUs = (uint32)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - waitStarted).count();
				const auto callbackStarted = std::chrono::steady_clock::now();
				const double outputTs = decodeResult.timestamp;
				const bool outputHasFrame = decodeResult.hasFrame;
#endif
				H264DoFrameOutputCallback(ctx, decodeResult);
#if defined(CEMU_UWP)
				H264UwpTrace("execute ts={:.3f} len={} {} queued, inflight={}, output ts={:.3f} frame={} after {:.1f} ms wait, game callback {:.1f} ms",
					(double)ctx->BitStream.timestamp, (uint32)ctx->BitStream.length, H264UwpDescribeSlices((uint8*)ctx->BitStream.ptr.GetPtr(), ctx->BitStream.length),
					ctx->decoderState.numFramesInFlight, outputTs, outputHasFrame ? 1 : 0, waitedUs / 1000.0,
					std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - callbackStarted).count() / 1000.0);
				tracedExecute = true;
#endif
				break;
			}
#if defined(CEMU_UWP)
			// under 2 ms the frame was already decoded
			if (waitedUs >= 2000)
			{
				const uint32 us = waitedUs;
				g_h264UwpStats.gameWaits++;
				g_h264UwpStats.gameWaitUs += us;
				H264UwpStatsMax(g_h264UwpStats.gameWaitUsMax, us);
			}
#endif
		}
#if defined(CEMU_UWP)
		if (!tracedExecute)
			H264UwpTrace("execute ts={:.3f} len={} {} queued, inflight={}, no output yet", (double)ctx->BitStream.timestamp, (uint32)ctx->BitStream.length,
				H264UwpDescribeSlices((uint8*)ctx->BitStream.ptr.GetPtr(), ctx->BitStream.length), ctx->decoderState.numFramesInFlight);
		H264UwpReportStats();
#endif
		_ReleaseDecoderSession(session);
		bt.Stop();
		double callTime = bt.GetElapsedMilliseconds();
		cemuLog_log(LogType::H264, "H264Bench | H264DECExecute took {}ms", callTime);
		return 0x80 | 100;
	}

	H264DEC_STATUS H264DECCheckDecunitLength(void* workMemory, uint8* data, uint32 maxLength, uint32 offset, uint32be* unitLengthOut)
	{
		// todo: our implementation for this currently doesn't parse slice headers and instead assumes that each frame is encoded into a single NAL slice. For all known cases this is sufficient but it doesn't match console behavior for cases where frames are split into multiple NALs
		if (offset >= maxLength || maxLength < 4)
		{
			return H264DEC_STATUS::INVALID_PARAM;
		}

		data += offset;
		maxLength -= offset;

		// search for start code
		sint32 startCodeOffset = 0;
		bool hasStartcode = false;
#if defined(CEMU_UWP)
		while (startCodeOffset + 3 <= (sint32)maxLength)
		{
			if (NALInputBitstream::GetStartCodeLength(data + startCodeOffset, maxLength - startCodeOffset) != 0)
			{
				hasStartcode = true;
				break;
			}
			startCodeOffset++;
		}
#else
		while (startCodeOffset < (sint32)(maxLength - 3))
		{
			if (data[startCodeOffset + 0] == 0x00 && data[startCodeOffset + 1] == 0x00 && data[startCodeOffset + 2] == 0x01)
			{
				hasStartcode = true;
				break;
			}
			startCodeOffset++;
		}
#endif
		if (hasStartcode == false)
			return H264DEC_STATUS::BAD_STREAM;
		data += startCodeOffset;
		maxLength -= startCodeOffset;

		NALInputBitstream nalStream(data, maxLength);
		if (nalStream.hasError())
			return H264DEC_STATUS::BAD_STREAM;

		// parse NAL data
		while (true)
		{
			if (nalStream.isEndOfStream())
				break;
			RBSPInputBitstream rbspStream;
			if (nalStream.getNextRBSP(rbspStream, true) == false)
				break;

			sint32 streamSubOffset = (sint32)(rbspStream.getBasePtr() - data);
			sint32 streamSubLength = rbspStream.getBaseLength();

			// parse NAL header
			uint8 nalHeaderByte = rbspStream.readU8();
			if ((nalHeaderByte & 0x80) != 0)
			{
				// MSB must be zero
				cemu_assert_debug(false);
				continue;
			}
			uint8 nal_ref_idc = (nalHeaderByte >> 5) & 0x3;
			uint8 nal_unit_type = (nalHeaderByte >> 0) & 0x1f;
			if (nal_unit_type == 14 || nal_unit_type == 20 || nal_unit_type == 21)
			{
				cemu_assert_debug(false);
				continue;
			}
			switch (nal_unit_type)
			{
			case 1:
			case 5:
			{
				*unitLengthOut = (sint32)((rbspStream.getBasePtr() + rbspStream.getBaseLength()) - data) + startCodeOffset;
				return H264DEC_STATUS::SUCCESS;
			}
			case 6:
				// SEI
				break;
			case 7:
				// SPS
				break;
			case 8:
				// PPS
				break;
			case 9:
				// access unit delimiter
				break;
			case 10:
				// end of sequence
				break;
			default:
				cemuLog_logDebug(LogType::Force, "Unsupported NAL unit type {}", nal_unit_type);
#if !defined(CEMU_UWP)
				cemu_assert_unimplemented();
				// todo
#endif
				break;
			}
		}
		return H264DEC_STATUS::BAD_STREAM;
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "h264";
		}

		void RPLMapped() override
		{
			cafeExportRegister("h264", H264DECCheckMemSegmentation, LogType::H264);
			cafeExportRegister("h264", H264DECMemoryRequirement, LogType::H264);
			cafeExportRegister("h264", H264DECFindDecstartpoint, LogType::H264);
			cafeExportRegister("h264", H264DECFindIdrpoint, LogType::H264);
			cafeExportRegister("h264", H264DECGetImageSize, LogType::H264);

			cafeExportRegister("h264", H264DECInitParam, LogType::H264);
			cafeExportRegister("h264", H264DECOpen, LogType::H264);
			cafeExportRegister("h264", H264DECClose, LogType::H264);
			cafeExportRegister("h264", H264DECBegin, LogType::H264);
			cafeExportRegister("h264", H264DECEnd, LogType::H264);

			cafeExportRegister("h264", H264DECSetParam_FPTR_OUTPUT, LogType::H264);
			cafeExportRegister("h264", H264DECSetParam_OUTPUT_PER_FRAME, LogType::H264);
			cafeExportRegister("h264", H264DECSetParam_USER_MEMORY, LogType::H264);
			cafeExportRegister("h264", H264DECSetParam, LogType::H264);

			cafeExportRegister("h264", H264DECSetBitstream, LogType::H264);
			cafeExportRegister("h264", H264DECExecute, LogType::H264);

			cafeExportRegister("h264", H264DECCheckDecunitLength, LogType::H264);
		};
	}s_COSh264Module;

	COSModule* GetModule()
	{
		return &s_COSh264Module;
	}
}
