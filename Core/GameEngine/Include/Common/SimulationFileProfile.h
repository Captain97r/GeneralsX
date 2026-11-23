/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstring>

// GeneralsX @feature Codex 23/11/2026 Frame-stamped files carry the clock used when their commands and deadlines were recorded.
namespace SimulationFileProfile
{
	constexpr char retailReplay[] = "GENREP";
	constexpr char nativeReplay[] = "GX60R2";
	constexpr int replaySignatureSize = sizeof(retailReplay) - 1;
	static_assert(sizeof(retailReplay) == sizeof(nativeReplay), "Replay signatures must have equal width");

	inline const char *replaySignature(int rate)
	{
		return rate == 60 ? nativeReplay : retailReplay;
	}

	inline int replayRate(const char *signature)
	{
		if (std::memcmp(signature, retailReplay, replaySignatureSize) == 0)
			return 30;
		if (std::memcmp(signature, nativeReplay, replaySignatureSize) == 0)
			return 60;
		return 0;
	}

	constexpr unsigned char nativeSaveVersion = 0x61;
	constexpr unsigned char saveVersion(int rate)
	{
		return rate == 60 ? nativeSaveVersion : 2;
	}

	constexpr int saveRate(unsigned char version)
	{
		if (version == nativeSaveVersion)
			return 60;
		return version == 1 || version == 2 ? 30 : 0;
	}
}
