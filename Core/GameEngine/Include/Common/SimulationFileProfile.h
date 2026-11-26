/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstring>

// GeneralsX @feature Captain97r 26/11/2026 Store frame-stamped replays and saves only under their matching simulation rules.
namespace SimulationFileProfile
{
#ifdef RTS_EXPERIMENTAL_60HZ_SIMULATION
#if RTS_ZEROHOUR
	constexpr char replaySignature[] = "GX60Z4";
	constexpr unsigned char saveVersion = 0x62;
#else
	constexpr char replaySignature[] = "GX60G4";
	constexpr unsigned char saveVersion = 0x63;
#endif
#else
	constexpr char replaySignature[] = "GENREP";
	constexpr unsigned char saveVersion = 2;
#endif
	constexpr int signatureLength = sizeof(replaySignature) - 1;

	inline bool acceptsReplay(const char *signature, int length)
	{
		return length == signatureLength && std::memcmp(signature, replaySignature, signatureLength) == 0;
	}

	constexpr bool acceptsSave(unsigned char version)
	{
#ifdef RTS_EXPERIMENTAL_60HZ_SIMULATION
		return version == saveVersion;
#else
		return version <= saveVersion;
#endif
	}
}
