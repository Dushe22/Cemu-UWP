#include "Common/precompiled.h"
#include "GX2_State.h"
#include "GX2_Command.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/OS/common/OSCommon.h"

namespace GX2
{
	using namespace Latte;

	void GX2InitAlphaTestReg(GX2AlphaTestReg* reg, uint32 alphaTestEnable, GX2_ALPHAFUNC alphaFunc, float alphaRef)
	{
		Latte::LATTE_SX_ALPHA_TEST_CONTROL tmpRegCtrl;
		tmpRegCtrl.set_ALPHA_FUNC(alphaFunc);
		tmpRegCtrl.set_ALPHA_TEST_ENABLE(alphaTestEnable != 0);
		reg->regAlphaTestControl = tmpRegCtrl;

		Latte::LATTE_SX_ALPHA_REF tmpRegRef;
		tmpRegRef.set_ALPHA_TEST_REF(alphaRef);
		reg->regAlphaTestRef = tmpRegRef;
	}

	void GX2SetAlphaTestReg(GX2AlphaTestReg* reg)
	{
		GX2ReserveCmdSpace(3 + 3);
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::SX_ALPHA_TEST_CONTROL - 0xA000,
			reg->regAlphaTestControl,
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::SX_ALPHA_REF - 0xA000,
			reg->regAlphaTestRef);
	}

	void GX2SetAlphaTest(uint32 alphaTestEnable, GX2_ALPHAFUNC alphaFunc, float alphaRef)
	{
		GX2AlphaTestReg tmpReg;
		GX2InitAlphaTestReg(&tmpReg, alphaTestEnable, alphaFunc, alphaRef);
		GX2SetAlphaTestReg(&tmpReg);
	}

	void GX2InitAlphaToMaskReg(uint32be* reg, uint32 alphaToMaskEnable, uint32 alphaToMaskDitherEnable)
	{
		if (reg)
			*reg = (alphaToMaskEnable ? 1u : 0u) | (alphaToMaskDitherEnable ? 2u : 0u);
	}

	void GX2SetAlphaToMaskReg(uint32be* reg)
	{
		// todo - alpha-to-coverage, only relevant for MSAA render targets
		(void)reg;
	}

	void GX2InitColorControlReg(GX2ColorControlReg* reg, GX2_LOGICOP logicOp, uint32 blendMask, uint32 multiwriteEnable, uint32 colorBufferEnable)
	{
		Latte::LATTE_CB_COLOR_CONTROL colorControlReg2;
		colorControlReg2.set_MULTIWRITE_ENABLE(multiwriteEnable != 0);
		if (colorBufferEnable == 0)
			colorControlReg2.set_SPECIAL_OP(Latte::LATTE_CB_COLOR_CONTROL::E_SPECIALOP::DISABLE);
		else
			colorControlReg2.set_SPECIAL_OP(Latte::LATTE_CB_COLOR_CONTROL::E_SPECIALOP::NORMAL);
		colorControlReg2.set_BLEND_MASK(blendMask);
		colorControlReg2.set_ROP(logicOp);
		reg->reg = colorControlReg2;
	}

	void GX2SetColorControlReg(GX2ColorControlReg* reg)
	{
		GX2ReserveCmdSpace(3);
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::CB_COLOR_CONTROL - 0xA000,
			reg->reg);
	}

	void GX2SetColorControl(GX2_LOGICOP logicOp, uint32 blendMask, uint32 multiwriteEnable, uint32 colorBufferEnable)
	{
		GX2ColorControlReg colorControlReg;
		GX2InitColorControlReg(&colorControlReg, logicOp, blendMask, multiwriteEnable, colorBufferEnable);
		GX2SetColorControlReg(&colorControlReg);
	}


	void GX2InitPolygonControlReg(GX2PolygonControlReg* reg,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE frontFace,
		uint32 cullFront,
		uint32 cullBack,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_POLYGONMODE usePolygonMode,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE polyModeFront,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE polyModeBack,
		uint32 polygonOffsetFrontEnable,
		uint32 polygonOffsetBackEnable,
		uint32 paraOffsetEnable)
	{
		Latte::LATTE_PA_SU_SC_MODE_CNTL v;
		v.set_FRONT_FACE(frontFace);
		v.set_CULL_FRONT((cullFront & 1) != 0);
		v.set_CULL_BACK((cullBack & 1) != 0);
		v.set_POLYGON_MODE(usePolygonMode);
		v.set_FRONT_POLY_MODE(polyModeFront);
		v.set_BACK_POLY_MODE(polyModeBack);
		v.set_OFFSET_PARA_ENABLED((paraOffsetEnable & 1) != 0);
		v.set_OFFSET_FRONT_ENABLED((polygonOffsetFrontEnable & 1) != 0);
		v.set_OFFSET_BACK_ENABLED((polygonOffsetBackEnable & 1) != 0);
		reg->reg = v;
	}

	void GX2SetPolygonControlReg(GX2PolygonControlReg* reg)
	{
		GX2ReserveCmdSpace(3);
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::PA_SU_SC_MODE_CNTL - 0xA000,
			reg->reg);
	}

	void GX2SetPolygonControl(Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE frontFace,
		uint32 cullFront,
		uint32 cullBack,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_POLYGONMODE usePolygonMode,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE polyModeFront,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE polyModeBack,
		uint32 polygonOffsetFrontEnable,
		uint32 polygonOffsetBackEnable,
		uint32 paraOffsetEnable)
	{
		GX2PolygonControlReg reg{};
		GX2InitPolygonControlReg(&reg, frontFace, cullFront, cullBack, usePolygonMode, polyModeFront, polyModeBack, polygonOffsetFrontEnable, polygonOffsetBackEnable, paraOffsetEnable);
		GX2SetPolygonControlReg(&reg);
	}

	void GX2SetCullOnlyControl(Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE frontFace, uint32 cullFront, uint32 cullBack)
	{
		GX2PolygonControlReg reg{};
		GX2InitPolygonControlReg(&reg, frontFace, cullFront, cullBack, Latte::LATTE_PA_SU_SC_MODE_CNTL::E_POLYGONMODE::UKN0, Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::POINTS, Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::POINTS, 0, 0, 0);
		GX2SetPolygonControlReg(&reg);
	}

	void GX2InitPolygonOffsetReg(GX2PolygonOffsetReg* reg, float frontOffset, float frontScale, float backOffset, float backScale, float clampOffset)
	{
		frontScale *= 16.0;
		backScale *= 16.0;
		reg->regFrontScale = Latte::LATTE_PA_SU_POLY_OFFSET_FRONT_SCALE().set_SCALE(frontScale);
		reg->regFrontOffset = Latte::LATTE_PA_SU_POLY_OFFSET_FRONT_OFFSET().set_OFFSET(frontOffset);
		reg->regBackScale = Latte::LATTE_PA_SU_POLY_OFFSET_BACK_SCALE().set_SCALE(backScale);
		reg->regBackOffset = Latte::LATTE_PA_SU_POLY_OFFSET_BACK_OFFSET().set_OFFSET(backOffset);
		reg->regClamp = Latte::LATTE_PA_SU_POLY_OFFSET_CLAMP().set_CLAMP(clampOffset);
	}

	void GX2SetPolygonOffsetReg(GX2PolygonOffsetReg* reg)
	{
		GX2ReserveCmdSpace(6 + 3);

		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 4),
			Latte::REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE - 0xA000,
			reg->regFrontScale,
			reg->regFrontOffset,
			reg->regBackScale,
			reg->regBackOffset,

			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::PA_SU_POLY_OFFSET_CLAMP - 0xA000,
			reg->regClamp);
	}

	void GX2SetPolygonOffset(float frontOffset, float frontScale, float backOffset, float backScale, float clampOffset)
	{
		GX2PolygonOffsetReg tmpReg;
		GX2InitPolygonOffsetReg(&tmpReg, frontOffset, frontScale, backOffset, backScale, clampOffset);
		GX2SetPolygonOffsetReg(&tmpReg);
	}

	void GX2SetRasterizerClipControlEx(bool enableRasterizer, bool enableZClip, bool enableHalfZ)
	{
		GX2ReserveCmdSpace(3);

		//if (enableHalfZ)
		//{
		//	// Smash has a bug where it enables half space clipping during streamout drawcalls and shadowing and then doesn't turn it off until the next GX2SetRasterizerClipControl call
		//	// this leads to some stuff being rendered at the wrong z-plane (e.g. shields behind characters) if the game's default depth range -1 to 1 isn't supported (on OpenGL only Nvidia's glDepthRangedNV allows unclamped values)
		//	uint64 titleId = gameMeta_getTitleId();
		//	if (titleId == 0x0005000010144F00ULL ||
		//		titleId == 0x0005000010145000ULL ||
		//		titleId == 0x0005000010110E00ULL)
		//	{
		//		// force disable half space clipping
		//		if (g_renderer && g_renderer->GetType() == RendererAPI::OpenGL && LatteGPUState.glVendor != GLVENDOR_NVIDIA)
		//			enableHalfZ = false;
		//	}
		//}

		Latte::LATTE_PA_CL_CLIP_CNTL reg{};
		reg.set_ZCLIP_NEAR_DISABLE(!enableZClip).set_ZCLIP_FAR_DISABLE(!enableZClip);
		reg.set_DX_RASTERIZATION_KILL(!enableRasterizer);
		reg.set_DX_CLIP_SPACE_DEF(enableHalfZ);
		reg.set_DX_LINEAR_ATTR_CLIP_ENA(true);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::PA_CL_CLIP_CNTL - 0xA000,
			reg);
	}

	void GX2SetRasterizerClipControl(bool enableRasterizer, bool enableZClip)
	{
		GX2SetRasterizerClipControlEx(enableRasterizer, enableZClip, false);
	}

	void GX2SetRasterizerClipControlHalfZ(bool enableRasterizer, bool enableZClip, bool enableHalfZ)
	{
		GX2SetRasterizerClipControlEx(enableRasterizer, enableZClip, enableHalfZ);
	}

	void GX2InitViewportReg(GX2ViewportReg* viewportReg, float x, float y, float width, float height, float nearZ, float farZ)
	{
		// todo: set clipping registers and zMin/zMax registers
		viewportReg->xScale = Latte::LATTE_PA_CL_VPORT_XSCALE().set_SCALE(width * 0.5f);
		viewportReg->xOffset = Latte::LATTE_PA_CL_VPORT_XOFFSET().set_OFFSET(x + (width * 0.5f));
		viewportReg->yScale = Latte::LATTE_PA_CL_VPORT_YSCALE().set_SCALE(height * -0.5f);
		viewportReg->yOffset = Latte::LATTE_PA_CL_VPORT_YOFFSET().set_OFFSET(y + (height * 0.5f));
		viewportReg->zScale = Latte::LATTE_PA_CL_VPORT_ZSCALE().set_SCALE((farZ - nearZ) * 0.5f);
		viewportReg->zOffset = Latte::LATTE_PA_CL_VPORT_ZOFFSET().set_OFFSET((nearZ + farZ) * 0.5f);
	}

	void GX2SetViewportReg(GX2ViewportReg* viewportReg)
	{
		GX2ReserveCmdSpace(2 + 6);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 6),
			Latte::REGADDR::PA_CL_VPORT_XSCALE - 0xA000,
			viewportReg->xScale, viewportReg->xOffset,
			viewportReg->yScale, viewportReg->yOffset,
			viewportReg->zScale, viewportReg->zOffset);
	}

	void GX2SetViewport(float x, float y, float width, float height, float nearZ, float farZ)
	{
		GX2ViewportReg viewportReg;
		GX2InitViewportReg(&viewportReg, x, y, width, height, nearZ, farZ);
		GX2SetViewportReg(&viewportReg);
	}

	void GX2InitScissorReg(GX2ScissorReg* scissorReg, uint32 x, uint32 y, uint32 width, uint32 height)
	{
		uint32 tlx = x;
		uint32 tly = y;
		uint32 brx = x + width;
		uint32 bry = y + height;

		tlx = std::min(tlx, 8192u);
		tly = std::min(tly, 8192u);
		brx = std::min(brx, 8192u);
		bry = std::min(bry, 8192u);

		scissorReg->scissorTL = Latte::LATTE_PA_SC_GENERIC_SCISSOR_TL().set_TL_X(tlx).set_TL_Y(tly).set_WINDOW_OFFSET_DISABLE(true);
		scissorReg->scissorBR = Latte::LATTE_PA_SC_GENERIC_SCISSOR_BR().set_BR_X(brx).set_BR_Y(bry);
	}

	void GX2SetScissorReg(GX2ScissorReg* scissorReg)
	{
		GX2ReserveCmdSpace(4);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 2),
			Latte::REGADDR::PA_SC_GENERIC_SCISSOR_TL - 0xA000,
			scissorReg->scissorTL, scissorReg->scissorBR);
	}

	void GX2GetScissorReg(GX2ScissorReg* scissorReg, uint32be* x, uint32be* y, uint32be* width, uint32be* height)
	{
		*x = scissorReg->scissorTL.value().get_TL_X();
		*y = scissorReg->scissorTL.value().get_TL_Y();
		*width = scissorReg->scissorBR.value().get_BR_X() - scissorReg->scissorTL.value().get_TL_X();
		*height = scissorReg->scissorBR.value().get_BR_Y() - scissorReg->scissorTL.value().get_TL_Y();
	}

	void GX2SetScissor(uint32 x, uint32 y, uint32 width, uint32 height)
	{
		GX2ScissorReg scissorReg;
		GX2InitScissorReg(&scissorReg, x, y, width, height);
		GX2SetScissorReg(&scissorReg);
	}

	void GX2SetDepthOnlyControl(bool depthTestEnable, bool depthWriteEnable, LATTE_DB_DEPTH_CONTROL::E_ZFUNC depthFunction)
	{
		// disables any currently set stencil test
		GX2ReserveCmdSpace(3);

		Latte::LATTE_DB_DEPTH_CONTROL reg{};
		reg.set_Z_ENABLE(depthTestEnable);
		reg.set_Z_WRITE_ENABLE(depthWriteEnable);
		reg.set_Z_FUNC(depthFunction);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::DB_DEPTH_CONTROL - 0xA000,
			reg);
	}

	void GX2SetDepthStencilControl(
		bool depthTestEnable, bool depthWriteEnable, LATTE_DB_DEPTH_CONTROL::E_ZFUNC depthFunction,
		bool stencilTestEnable, bool backStencilTestEnable, 
		LATTE_DB_DEPTH_CONTROL::E_STENCILFUNC frontStencilFunction, 
		LATTE_DB_DEPTH_CONTROL::E_STENCILACTION frontStencilZPass, LATTE_DB_DEPTH_CONTROL::E_STENCILACTION frontStencilZFail, LATTE_DB_DEPTH_CONTROL::E_STENCILACTION frontStencilFail,
		LATTE_DB_DEPTH_CONTROL::E_STENCILFUNC backStencilFunction, 
		LATTE_DB_DEPTH_CONTROL::E_STENCILACTION backStencilZPass, LATTE_DB_DEPTH_CONTROL::E_STENCILACTION backStencilZFail, LATTE_DB_DEPTH_CONTROL::E_STENCILACTION backStencilFail
	)
	{
		GX2ReserveCmdSpace(3);

		Latte::LATTE_DB_DEPTH_CONTROL reg{};
		reg.set_Z_ENABLE(depthTestEnable).set_Z_WRITE_ENABLE(depthWriteEnable).set_Z_FUNC(depthFunction);
		reg.set_STENCIL_ENABLE(stencilTestEnable).set_BACK_STENCIL_ENABLE(backStencilTestEnable);
		reg.set_STENCIL_FUNC_F(frontStencilFunction).set_STENCIL_FUNC_B(backStencilFunction);
		reg.set_STENCIL_ZPASS_F(frontStencilZPass).set_STENCIL_ZFAIL_F(frontStencilZFail).set_STENCIL_FAIL_F(frontStencilFail);
		reg.set_STENCIL_ZPASS_B(backStencilZPass).set_STENCIL_ZFAIL_B(backStencilZFail).set_STENCIL_FAIL_B(backStencilFail);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::DB_DEPTH_CONTROL - 0xA000,
			reg);
	}

	void GX2InitDepthStencilControlReg(
		GX2DepthStencilControlReg* depthStencilControlReg,
		bool depthTestEnable, bool depthWriteEnable, LATTE_DB_DEPTH_CONTROL::E_ZFUNC depthFunction,
		bool stencilTestEnable, bool backStencilTestEnable,
		LATTE_DB_DEPTH_CONTROL::E_STENCILFUNC frontStencilFunction,
		LATTE_DB_DEPTH_CONTROL::E_STENCILACTION frontStencilZPass, LATTE_DB_DEPTH_CONTROL::E_STENCILACTION frontStencilZFail, LATTE_DB_DEPTH_CONTROL::E_STENCILACTION frontStencilFail,
		LATTE_DB_DEPTH_CONTROL::E_STENCILFUNC backStencilFunction,
		LATTE_DB_DEPTH_CONTROL::E_STENCILACTION backStencilZPass, LATTE_DB_DEPTH_CONTROL::E_STENCILACTION backStencilZFail, LATTE_DB_DEPTH_CONTROL::E_STENCILACTION backStencilFail)
	{
		Latte::LATTE_DB_DEPTH_CONTROL reg{};
		reg.set_Z_ENABLE(depthTestEnable).set_Z_WRITE_ENABLE(depthWriteEnable).set_Z_FUNC(depthFunction);
		reg.set_STENCIL_ENABLE(stencilTestEnable).set_BACK_STENCIL_ENABLE(backStencilTestEnable);
		reg.set_STENCIL_FUNC_F(frontStencilFunction).set_STENCIL_FUNC_B(backStencilFunction);
		reg.set_STENCIL_ZPASS_F(frontStencilZPass).set_STENCIL_ZFAIL_F(frontStencilZFail).set_STENCIL_FAIL_F(frontStencilFail);
		reg.set_STENCIL_ZPASS_B(backStencilZPass).set_STENCIL_ZFAIL_B(backStencilZFail).set_STENCIL_FAIL_B(backStencilFail);
		depthStencilControlReg->reg = reg;
	}

	void GX2SetDepthStencilControlReg(GX2DepthStencilControlReg* depthStencilControlReg)
	{
		GX2ReserveCmdSpace(3);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::DB_DEPTH_CONTROL - 0xA000,
			depthStencilControlReg->reg);
	}

	void GX2GetDepthStencilControlReg(
		GX2DepthStencilControlReg* depthStencilControlReg,
		uint32be* depthTestEnable, uint32be* depthWriteEnable, uint32be* depthFunction,
		uint32be* stencilTestEnable, uint32be* backStencilTestEnable,
		uint32be* frontStencilFunction,
		uint32be* frontStencilZPass, uint32be* frontStencilZFail, uint32be* frontStencilFail,
		uint32be* backStencilFunction,
		uint32be* backStencilZPass, uint32be* backStencilZFail, uint32be* backStencilFail)
	{
		// used by Hyrule Warriors

		*depthTestEnable = depthStencilControlReg->reg.value().get_Z_ENABLE();

		*depthWriteEnable = depthStencilControlReg->reg.value().get_Z_WRITE_ENABLE();
		*depthFunction = (uint32)depthStencilControlReg->reg.value().get_Z_FUNC();

		*stencilTestEnable = depthStencilControlReg->reg.value().get_STENCIL_ENABLE();
		*backStencilTestEnable = depthStencilControlReg->reg.value().get_BACK_STENCIL_ENABLE();
	
		*frontStencilFunction = (uint32)depthStencilControlReg->reg.value().get_STENCIL_FUNC_F();
		*backStencilFunction = (uint32)depthStencilControlReg->reg.value().get_STENCIL_FUNC_B();

		*frontStencilZPass = (uint32)depthStencilControlReg->reg.value().get_STENCIL_ZPASS_F();
		*frontStencilZFail = (uint32)depthStencilControlReg->reg.value().get_STENCIL_ZFAIL_F();
		*frontStencilFail = (uint32)depthStencilControlReg->reg.value().get_STENCIL_FAIL_F();

		*backStencilZPass = (uint32)depthStencilControlReg->reg.value().get_STENCIL_ZPASS_B();
		*backStencilZFail = (uint32)depthStencilControlReg->reg.value().get_STENCIL_ZFAIL_B();
		*backStencilFail = (uint32)depthStencilControlReg->reg.value().get_STENCIL_FAIL_B();
	}

	void GX2InitStencilMaskReg(GX2StencilMaskReg* stencilMaskReg, uint8 compareMaskFront, uint8 writeMaskFront, uint8 refFront, uint8 compareMaskBack, uint8 writeMaskBack, uint8 refBack)
	{
		stencilMaskReg->stencilRefMaskFrontReg = LATTE_DB_STENCILREFMASK().set_STENCILREF_F(refFront).set_STENCILMASK_F(compareMaskFront).set_STENCILWRITEMASK_F(writeMaskFront);
		stencilMaskReg->stencilRefMaskBackReg = LATTE_DB_STENCILREFMASK_BF().set_STENCILREF_B(refBack).set_STENCILMASK_B(compareMaskBack).set_STENCILWRITEMASK_B(writeMaskBack);
	}

	void GX2SetStencilMask(uint8 compareMaskFront, uint8 writeMaskFront, uint8 refFront, uint8 compareMaskBack, uint8 writeMaskBack, uint8 refBack)
	{
		GX2ReserveCmdSpace(3 + 3);

		LATTE_DB_STENCILREFMASK frontReg;
		frontReg.set_STENCILREF_F(refFront).set_STENCILMASK_F(compareMaskFront).set_STENCILWRITEMASK_F(writeMaskFront);
		LATTE_DB_STENCILREFMASK_BF backReg;
		backReg.set_STENCILREF_B(refBack).set_STENCILMASK_B(compareMaskBack).set_STENCILWRITEMASK_B(writeMaskBack);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
							REGADDR::DB_STENCILREFMASK - 0xA000,
							frontReg,
							pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
							REGADDR::DB_STENCILREFMASK_BF - 0xA000,
							backReg);
	}

	void GX2SetStencilMaskReg(GX2StencilMaskReg* stencilMaskReg)
	{
		GX2ReserveCmdSpace(3 + 3);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			REGADDR::DB_STENCILREFMASK - 0xA000,
			stencilMaskReg->stencilRefMaskFrontReg,
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			REGADDR::DB_STENCILREFMASK_BF - 0xA000,
			stencilMaskReg->stencilRefMaskBackReg);
	}

	void GX2SetPrimitiveRestartIndex(uint32 restartIndex)
	{
		GX2ReserveCmdSpace(3);
		Latte::LATTE_VGT_MULTI_PRIM_IB_RESET_INDX reg{};
		reg.set_RESTART_INDEX(restartIndex);
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX - 0xA000,
			reg);
	}

	void GX2InitTargetChannelMasksReg(GX2TargetChannelMaskReg* reg, GX2_CHANNELMASK t0, GX2_CHANNELMASK t1, GX2_CHANNELMASK t2, GX2_CHANNELMASK t3, GX2_CHANNELMASK t4, GX2_CHANNELMASK t5, GX2_CHANNELMASK t6, GX2_CHANNELMASK t7)
	{
		uint32 targetMask = 0;
		targetMask |= ((t0 & 0xF) << 0);
		targetMask |= ((t1 & 0xF) << 4);
		targetMask |= ((t2 & 0xF) << 8);
		targetMask |= ((t3 & 0xF) << 12);
		targetMask |= ((t4 & 0xF) << 16);
		targetMask |= ((t5 & 0xF) << 20);
		targetMask |= ((t6 & 0xF) << 24);
		targetMask |= ((t7 & 0xF) << 28);
		Latte::LATTE_CB_TARGET_MASK r;
		r.set_MASK(targetMask);
		reg->reg = r;
	}

	void GX2SetTargetChannelMasksReg(GX2TargetChannelMaskReg* reg)
	{
		GX2ReserveCmdSpace(3);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::CB_TARGET_MASK - 0xA000,
			reg->reg);
	}

	void GX2SetTargetChannelMasks(GX2_CHANNELMASK t0, GX2_CHANNELMASK t1, GX2_CHANNELMASK t2, GX2_CHANNELMASK t3, GX2_CHANNELMASK t4, GX2_CHANNELMASK t5, GX2_CHANNELMASK t6, GX2_CHANNELMASK t7)
	{
		GX2TargetChannelMaskReg tmpReg;
		GX2InitTargetChannelMasksReg(&tmpReg, t0, t1, t2, t3, t4, t5, t6, t7);
		GX2SetTargetChannelMasksReg(&tmpReg);
	}

	static_assert(sizeof(GX2_CHANNELMASK) == 4);

	void GX2GetTargetChannelMasksReg(GX2TargetChannelMaskReg* reg, betype<GX2_CHANNELMASK>* t0, betype<GX2_CHANNELMASK>* t1, betype<GX2_CHANNELMASK>* t2, betype<GX2_CHANNELMASK>* t3,
									betype<GX2_CHANNELMASK>* t4, betype<GX2_CHANNELMASK>* t5, betype<GX2_CHANNELMASK>* t6, betype<GX2_CHANNELMASK>* t7)
	{
		uint32 maskValue = reg->reg.value().get_MASK();
		*t0 = (maskValue >> 0) & 0xF;
		*t1 = (maskValue >> 4) & 0xF;
		*t2 = (maskValue >> 8) & 0xF;
		*t3 = (maskValue >> 12) & 0xF;
		*t4 = (maskValue >> 16) & 0xF;
		*t5 = (maskValue >> 20) & 0xF;
		*t6 = (maskValue >> 24) & 0xF;
		*t7 = (maskValue >> 28) & 0xF;
	}

	void GX2InitBlendControlReg(GX2BlendControlReg* reg, uint32 renderTargetIndex, GX2_BLENDFACTOR colorSrcFactor, GX2_BLENDFACTOR colorDstFactor, GX2_BLENDFUNC colorCombineFunc, uint32 separateAlphaBlend, GX2_BLENDFACTOR alphaSrcFactor, GX2_BLENDFACTOR alphaDstFactor, GX2_BLENDFUNC alphaCombineFunc)
	{
		Latte::LATTE_CB_BLENDN_CONTROL tmpReg;
		tmpReg.set_COLOR_SRCBLEND(colorSrcFactor);
		tmpReg.set_COLOR_DSTBLEND(colorDstFactor);
		tmpReg.set_COLOR_COMB_FCN(colorCombineFunc);
		tmpReg.set_ALPHA_SRCBLEND(alphaSrcFactor);
		tmpReg.set_ALPHA_DSTBLEND(alphaDstFactor);
		tmpReg.set_ALPHA_COMB_FCN(alphaCombineFunc);
		tmpReg.set_SEPARATE_ALPHA_BLEND(separateAlphaBlend != 0);

		reg->index = renderTargetIndex;
		reg->reg = tmpReg;
	}

	void GX2SetBlendControlReg(GX2BlendControlReg* reg)
	{
		GX2ReserveCmdSpace(3);
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			(Latte::REGADDR::CB_BLEND0_CONTROL + (uint32)reg->index) - 0xA000,
			reg->reg
		);
	}

	void GX2SetBlendControl(uint32 renderTargetIndex, GX2_BLENDFACTOR colorSrcFactor, GX2_BLENDFACTOR colorDstFactor, GX2_BLENDFUNC colorCombineFunc, uint32 separateAlphaBlend, GX2_BLENDFACTOR alphaSrcFactor, GX2_BLENDFACTOR alphaDstFactor, GX2_BLENDFUNC alphaCombineFunc)
	{
		GX2BlendControlReg tmpReg;
		GX2InitBlendControlReg(&tmpReg, renderTargetIndex, colorSrcFactor, colorDstFactor, colorCombineFunc, separateAlphaBlend, alphaSrcFactor, alphaDstFactor, alphaCombineFunc);
		GX2SetBlendControlReg(&tmpReg);
	}

	void GX2GetBlendControlReg(GX2BlendControlReg* reg, uint32be* renderTargetIndex, uint32be* colorSrcFactor, uint32be* colorDstFactor, uint32be* colorCombineFunc, uint32be* separateAlphaBlend, uint32be* alphaSrcFactor, uint32be* alphaDstFactor, uint32be* alphaCombineFunc)
	{
		const Latte::LATTE_CB_BLENDN_CONTROL blendControl = reg->reg.value();
		*renderTargetIndex = (uint32)reg->index;
		*colorSrcFactor = (uint32)blendControl.get_COLOR_SRCBLEND();
		*colorDstFactor = (uint32)blendControl.get_COLOR_DSTBLEND();
		*colorCombineFunc = (uint32)blendControl.get_COLOR_COMB_FCN();
		*separateAlphaBlend = blendControl.get_SEPARATE_ALPHA_BLEND() ? 1 : 0;
		*alphaSrcFactor = (uint32)blendControl.get_ALPHA_SRCBLEND();
		*alphaDstFactor = (uint32)blendControl.get_ALPHA_DSTBLEND();
		*alphaCombineFunc = (uint32)blendControl.get_ALPHA_COMB_FCN();
	}

	void GX2GetAlphaTestReg(GX2AlphaTestReg* reg, uint32be* alphaTestEnable, uint32be* alphaFunc, float32be* alphaRef)
	{
		*alphaTestEnable = reg->regAlphaTestControl.value().get_ALPHA_TEST_ENABLE() ? 1 : 0;
		*alphaFunc = (uint32)reg->regAlphaTestControl.value().get_ALPHA_FUNC();
		*alphaRef = reg->regAlphaTestRef.value().get_ALPHA_TEST_REF();
	}

	void GX2GetAlphaToMaskReg(uint32be* reg, uint32be* alphaToMaskEnable, uint32be* alphaToMaskDitherEnable)
	{
		const uint32 value = *reg;
		*alphaToMaskEnable = (value & 1) ? 1 : 0;
		*alphaToMaskDitherEnable = (value & 2) ? 1 : 0;
	}

	void GX2GetColorControlReg(GX2ColorControlReg* reg, uint32be* logicOp, uint8* blendMask, uint32be* multiwriteEnable, uint32be* colorBufferEnable)
	{
		const Latte::LATTE_CB_COLOR_CONTROL colorControl = reg->reg.value();
		*logicOp = (uint32)colorControl.get_ROP();
		*blendMask = (uint8)colorControl.get_BLEND_MASK();
		*multiwriteEnable = colorControl.get_MULTIWRITE_ENABLE() ? 1 : 0;
		*colorBufferEnable = colorControl.get_SPECIAL_OP() == Latte::LATTE_CB_COLOR_CONTROL::E_SPECIALOP::DISABLE ? 0 : 1;
	}

	void GX2GetPolygonControlReg(GX2PolygonControlReg* reg, uint32be* frontFace, uint32be* cullFront, uint32be* cullBack, uint32be* usePolygonMode, uint32be* polyModeFront, uint32be* polyModeBack, uint32be* polygonOffsetFrontEnable, uint32be* polygonOffsetBackEnable, uint32be* paraOffsetEnable)
	{
		const Latte::LATTE_PA_SU_SC_MODE_CNTL polygonControl = reg->reg.value();
		*frontFace = (uint32)polygonControl.get_FRONT_FACE();
		*cullFront = polygonControl.get_CULL_FRONT() ? 1 : 0;
		*cullBack = polygonControl.get_CULL_BACK() ? 1 : 0;
		*usePolygonMode = (uint32)polygonControl.get_POLYGON_MODE();
		*polyModeFront = (uint32)polygonControl.get_FRONT_POLY_MODE();
		*polyModeBack = (uint32)polygonControl.get_BACK_POLY_MODE();
		*polygonOffsetFrontEnable = polygonControl.get_OFFSET_FRONT_ENABLED() ? 1 : 0;
		*polygonOffsetBackEnable = polygonControl.get_OFFSET_BACK_ENABLED() ? 1 : 0;
		*paraOffsetEnable = polygonControl.get_OFFSET_PARA_ENABLED() ? 1 : 0;
	}

	void GX2GetPolygonOffsetReg(GX2PolygonOffsetReg* reg, float32be* frontOffset, float32be* frontScale, float32be* backOffset, float32be* backScale, float32be* clampOffset)
	{
		*frontOffset = reg->regFrontOffset.value().get_OFFSET();
		*frontScale = reg->regFrontScale.value().get_SCALE() / 16.0f;
		*backOffset = reg->regBackOffset.value().get_OFFSET();
		*backScale = reg->regBackScale.value().get_SCALE() / 16.0f;
		*clampOffset = reg->regClamp.value().get_CLAMP();
	}

	void GX2GetViewportReg(GX2ViewportReg* reg, float32be* x, float32be* y, float32be* width, float32be* height, float32be* nearZ, float32be* farZ)
	{
		const float xScale = reg->xScale.value().get_SCALE();
		const float yScale = reg->yScale.value().get_SCALE();
		const float zScale = reg->zScale.value().get_SCALE();
		const float zOffset = reg->zOffset.value().get_OFFSET();
		*x = reg->xOffset.value().get_OFFSET() - xScale;
		*y = reg->yOffset.value().get_OFFSET() + yScale;
		*width = xScale * 2.0f;
		*height = yScale * -2.0f;
		*nearZ = zOffset - zScale;
		*farZ = zOffset + zScale;
	}

	void GX2GetStencilMaskReg(GX2StencilMaskReg* reg, uint8* compareMaskFront, uint8* writeMaskFront, uint8* refFront, uint8* compareMaskBack, uint8* writeMaskBack, uint8* refBack)
	{
		const Latte::LATTE_DB_STENCILREFMASK front = reg->stencilRefMaskFrontReg.value();
		const Latte::LATTE_DB_STENCILREFMASK_BF back = reg->stencilRefMaskBackReg.value();
		*compareMaskFront = (uint8)front.get_STENCILMASK_F();
		*writeMaskFront = (uint8)front.get_STENCILWRITEMASK_F();
		*refFront = (uint8)front.get_STENCILREF_F();
		*compareMaskBack = (uint8)back.get_STENCILMASK_B();
		*writeMaskBack = (uint8)back.get_STENCILWRITEMASK_B();
		*refBack = (uint8)back.get_STENCILREF_B();
	}

	void GX2GetBlendConstantColorReg(GX2BlendConstantColorReg* reg, float32be* red, float32be* green, float32be* blue, float32be* alpha)
	{
		*red = reg->regRed.value().get_RED();
		*green = reg->regGreen.value().get_GREEN();
		*blue = reg->regBlue.value().get_BLUE();
		*alpha = reg->regAlpha.value().get_ALPHA();
	}

	void GX2GetPointSizeReg(GX2PointSizeReg* reg, float32be* width, float32be* height)
	{
		*width = (float)reg->reg.value().get_WIDTH() / 8.0f;
		*height = (float)reg->reg.value().get_HEIGHT() / 8.0f;
	}

	void GX2GetPointLimitsReg(GX2PointLimitsReg* reg, float32be* minSize, float32be* maxSize)
	{
		*minSize = (float)reg->reg.value().get_MIN_SIZE() / 8.0f;
		*maxSize = (float)reg->reg.value().get_MAX_SIZE() / 8.0f;
	}

	void GX2InitBlendConstantColorReg(GX2BlendConstantColorReg* reg, float red, float green, float blue, float alpha)
	{
		reg->regRed = Latte::LATTE_CB_BLEND_RED().set_RED(red);
		reg->regGreen = Latte::LATTE_CB_BLEND_GREEN().set_GREEN(green);
		reg->regBlue = Latte::LATTE_CB_BLEND_BLUE().set_BLUE(blue);
		reg->regAlpha = Latte::LATTE_CB_BLEND_ALPHA().set_ALPHA(alpha);
	}

	void GX2SetBlendConstantColorReg(GX2BlendConstantColorReg* reg)
	{
		GX2ReserveCmdSpace(6);
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 4),
			Latte::REGADDR::CB_BLEND_RED - 0xA000,
			reg->regRed,
			reg->regGreen,
			reg->regBlue,
			reg->regAlpha
		);
	}

	void GX2SetBlendConstantColor(float red, float green, float blue, float alpha)
	{
		GX2BlendConstantColorReg tmpReg;
		GX2InitBlendConstantColorReg(&tmpReg, red, green, blue, alpha);
		GX2SetBlendConstantColorReg(&tmpReg);
	}

	void GX2InitHiStencilInfoRegs(GX2HiStencilInfoReg* hiStencilInfo)
	{
		// seen in Color Splash
		// but the game never calls GX2SetHiStencilInfo thus this has no effect
	}

	void GX2InitPointSizeReg(GX2PointSizeReg* reg, float width, float height)
	{
		if (width < 0.0f || height < 0.0f)
		{
			cemu_assert_suspicious();
		}

		uint32 widthI = (uint32)(width * 8.0f);
		uint32 heightI = (uint32)(height * 8.0f);

		widthI = std::min<uint32>(widthI, 0xFFFF);
		heightI = std::min<uint32>(heightI, 0xFFFF);

		reg->reg = Latte::LATTE_PA_SU_POINT_SIZE().set_WIDTH(widthI).set_HEIGHT(heightI);
	}

	void GX2SetPointSizeReg(GX2PointSizeReg* reg)
	{
		GX2ReserveCmdSpace(3);
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::PA_SU_POINT_SIZE - 0xA000,
			reg->reg
		);
	}

	void GX2SetPointSize(float width, float height)
	{
		GX2PointSizeReg tmpReg;
		GX2InitPointSizeReg(&tmpReg, width, height);
		GX2SetPointSizeReg(&tmpReg);
	}

	void GX2InitPointLimitsReg(GX2PointLimitsReg* reg, float minSize, float maxSize)
	{
		if (minSize < 0.0f || maxSize < 0.0f)
		{
			cemu_assert_suspicious();
		}

		uint32 minSizeI = (uint32)(minSize * 8.0f);
		uint32 maxSizeI = (uint32)(maxSize * 8.0f);

		minSizeI = std::min<uint32>(minSizeI, 0xFFFF);
		maxSizeI = std::min<uint32>(maxSizeI, 0xFFFF);

		reg->reg = Latte::LATTE_PA_SU_POINT_MINMAX().set_MIN_SIZE(minSizeI).set_MAX_SIZE(maxSizeI);
	}

	void GX2SetPointLimitsReg(GX2PointLimitsReg* reg)
	{
		GX2ReserveCmdSpace(3);
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::PA_SU_POINT_MINMAX - 0xA000,
			reg->reg
		);
	}

	void GX2SetPointLimits(float minSize, float maxSize)
	{
		GX2PointLimitsReg tmpReg;
		GX2InitPointLimitsReg(&tmpReg, minSize, maxSize);
		GX2SetPointLimitsReg(&tmpReg);
	}

	enum class GX2_SPECIAL_STATE : uint32
	{
		FAST_CLEAR = 0,
		FAST_CLEAR_HIZ = 1,
	};

	void _setSpecialState0(bool isEnabled)
	{
		GX2ReserveCmdSpace(6);
		if (isEnabled)
		{
			// set PA_CL_VTE_CNTL to 0x300
			Latte::LATTE_PA_CL_VTE_CNTL regVTE{};
			regVTE.set_VTX_XY_FMT(true);
			regVTE.set_VTX_Z_FMT(true);
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
				Latte::REGADDR::PA_CL_VTE_CNTL - 0xA000,
				regVTE);
			// set PA_CL_CLIP_CNTL to 0x490000
			Latte::LATTE_PA_CL_CLIP_CNTL regClip{};
			regClip.set_CLIP_DISABLE(true); // 0x10000
			regClip.set_DX_CLIP_SPACE_DEF(true); // 0x80000
			regClip.set_DX_RASTERIZATION_KILL(true); // 0x400000
			
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
				Latte::REGADDR::PA_CL_CLIP_CNTL - 0xA000,
				regClip);
		}
		else
		{
			// set PA_CL_VTE_CNTL to 0x43F
			Latte::LATTE_PA_CL_VTE_CNTL reg{};
			reg.set_VPORT_X_OFFSET_ENA(true).set_VPORT_X_SCALE_ENA(true);
			reg.set_VPORT_Y_OFFSET_ENA(true).set_VPORT_Y_SCALE_ENA(true);
			reg.set_VPORT_Z_OFFSET_ENA(true).set_VPORT_Z_SCALE_ENA(true);
			reg.set_VTX_W0_FMT(true);
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
				Latte::REGADDR::PA_CL_VTE_CNTL - 0xA000,
				reg);
			// reset PA_CL_CLIP_CNTL
			GX2SetRasterizerClipControl(true, true);
		}
	}

	void GX2SetSpecialState(GX2_SPECIAL_STATE stateId, uint32 isEnabled)
	{
		if (stateId == GX2_SPECIAL_STATE::FAST_CLEAR)
		{
			_setSpecialState0(isEnabled != 0);
		}
		else if (stateId == GX2_SPECIAL_STATE::FAST_CLEAR_HIZ)
		{
			// todo
			// enables additional flags for special state 0
		}
		else
		{
			// legacy style
			GX2ReserveCmdSpace(3);
			gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_HLE_SPECIAL_STATE, 2));
			gx2WriteGather_submitU32AsBE((uint32)stateId); // state id
			gx2WriteGather_submitU32AsBE(isEnabled); // enable/disable bool
		}
	}

	void GX2StateInit()
	{
		cafeExportRegister("gx2", GX2InitAlphaTestReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetAlphaTestReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetAlphaTest, LogType::GX2);
		cafeExportRegister("gx2", GX2InitAlphaToMaskReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetAlphaToMaskReg, LogType::GX2);

		cafeExportRegister("gx2", GX2InitColorControlReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetColorControl, LogType::GX2);
		cafeExportRegister("gx2", GX2SetColorControlReg, LogType::GX2);

		cafeExportRegister("gx2", GX2InitPolygonControlReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetPolygonControlReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetPolygonControl, LogType::GX2);
		cafeExportRegister("gx2", GX2SetCullOnlyControl, LogType::GX2);

		cafeExportRegister("gx2", GX2InitPolygonOffsetReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetPolygonOffsetReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetPolygonOffset, LogType::GX2);

		cafeExportRegister("gx2", GX2SetRasterizerClipControl, LogType::GX2);
		cafeExportRegister("gx2", GX2SetRasterizerClipControlHalfZ, LogType::GX2);
		cafeExportRegister("gx2", GX2SetRasterizerClipControlEx, LogType::GX2);

		cafeExportRegister("gx2", GX2InitViewportReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetViewportReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetViewport, LogType::GX2);

		cafeExportRegister("gx2", GX2InitScissorReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetScissorReg, LogType::GX2);
		cafeExportRegister("gx2", GX2GetScissorReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetScissor, LogType::GX2);

		cafeExportRegister("gx2", GX2InitDepthStencilControlReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetDepthStencilControlReg, LogType::GX2);
		cafeExportRegister("gx2", GX2GetDepthStencilControlReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetDepthOnlyControl, LogType::GX2);
		cafeExportRegister("gx2", GX2SetDepthStencilControl, LogType::GX2);

		cafeExportRegister("gx2", GX2InitStencilMaskReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetStencilMask, LogType::GX2);
		cafeExportRegister("gx2", GX2SetStencilMaskReg, LogType::GX2);

		cafeExportRegister("gx2", GX2SetPrimitiveRestartIndex, LogType::GX2);

		cafeExportRegister("gx2", GX2InitTargetChannelMasksReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetTargetChannelMasksReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetTargetChannelMasks, LogType::GX2);
		cafeExportRegister("gx2", GX2GetTargetChannelMasksReg, LogType::GX2);

		cafeExportRegister("gx2", GX2InitBlendControlReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetBlendControlReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetBlendControl, LogType::GX2);
		cafeExportRegister("gx2", GX2GetBlendControlReg, LogType::GX2);
		cafeExportRegister("gx2", GX2GetAlphaTestReg, LogType::GX2);
		cafeExportRegister("gx2", GX2GetAlphaToMaskReg, LogType::GX2);
		cafeExportRegister("gx2", GX2GetColorControlReg, LogType::GX2);
		cafeExportRegister("gx2", GX2GetPolygonControlReg, LogType::GX2);
		cafeExportRegister("gx2", GX2GetPolygonOffsetReg, LogType::GX2);
		cafeExportRegister("gx2", GX2GetViewportReg, LogType::GX2);
		cafeExportRegister("gx2", GX2GetStencilMaskReg, LogType::GX2);
		cafeExportRegister("gx2", GX2GetBlendConstantColorReg, LogType::GX2);
		cafeExportRegister("gx2", GX2GetPointSizeReg, LogType::GX2);
		cafeExportRegister("gx2", GX2GetPointLimitsReg, LogType::GX2);

		cafeExportRegister("gx2", GX2InitBlendConstantColorReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetBlendConstantColorReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetBlendConstantColor, LogType::GX2);
		cafeExportRegister("gx2", GX2InitHiStencilInfoRegs, LogType::GX2);

		cafeExportRegister("gx2", GX2InitPointSizeReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetPointSizeReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetPointSize, LogType::GX2);
		cafeExportRegister("gx2", GX2InitPointLimitsReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetPointLimitsReg, LogType::GX2);
		cafeExportRegister("gx2", GX2SetPointLimits, LogType::GX2);

		cafeExportRegister("gx2", GX2SetSpecialState, LogType::GX2);
	}

}
