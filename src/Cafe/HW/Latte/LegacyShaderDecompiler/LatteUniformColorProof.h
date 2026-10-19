#pragma once

#include <cstddef>
#include <cstdint>

// one ALU clause of four uniform MOVs followed by a single RGBA color export
namespace LatteUniformColorProof
{
	inline std::uint32_t ReadWord(const std::uint8_t* bytes)
	{
		return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
			(std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
	}

	inline bool IsUniformColorProgram(const std::uint8_t* program, std::size_t size)
	{
		if (!program || size < 48 || (size & 7) != 0)
			return false;

		const std::uint32_t aluCF0 = ReadWord(program);
		const std::uint32_t aluCF1 = ReadWord(program + 4);
		const std::uint32_t exportCF0 = ReadWord(program + 8);
		const std::uint32_t exportCF1 = ReadWord(program + 12);

		// CF_ALU x4, barrier, bank0 LOCK_1; only clause address and bank0 base may vary
		if ((aluCF0 & 0xfc000000u) != 0x40000000u ||
			(aluCF1 & ~0x000003fcu) != 0xa00c0000u)
			return false;

		// EXPORT_DONE, EOP, identity swizzle; only export index and source GPR may vary
		if ((exportCF0 & ~0x003f8007u) != 0 || exportCF1 != 0x94200688u)
			return false;

		const std::size_t aluOffset = std::size_t(aluCF0 & 0x003fffffu) * 8;
		if (aluOffset < 16 || aluOffset > size - 32)
			return false;
		const std::uint32_t outputGpr = (exportCF0 >> 15) & 0x7f;
		std::uint32_t writtenChannels = 0;
		for (std::size_t index = 0; index < 4; ++index)
		{
			const std::uint32_t word0 = ReadWord(program + aluOffset + index * 8);
			const std::uint32_t word1 = ReadWord(program + aluOffset + index * 8 + 4);
			// bank0 direct source, any swizzle/neg; no literal, rel, index mode or pred
			const std::uint32_t source = word0 & 0x1ff;
			if (source < 128 || source > 159 ||
				(word0 & ~0x80001dffu) != 0x001f0000u ||
				((word0 >> 31) != (index == 3 ? 1u : 0u)))
				return false;
			// OP2 MOV, write enabled; abs/clamp ok, no dst index, pred update or omod
			if ((word1 & ~0xefe00001u) != 0x00000c90u ||
				((word1 >> 21) & 0x7f) != outputGpr)
				return false;
			const std::uint32_t channelBit = 1u << ((word1 >> 29) & 3);
			if ((writtenChannels & channelBit) != 0)
				return false;
			writtenChannels |= channelBit;
		}
		return writtenChannels == 0xf;
	}
}
