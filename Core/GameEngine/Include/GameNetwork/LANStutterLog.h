/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

// GeneralsX @feature Codex 23/11/2026 Observe local LAN lockstep and rendering on the game thread.
// All times are monotonic. No measurements are sent to other players.
namespace LANStutterLog
{
	struct FrameSample
	{
		unsigned long long frameUs, betweenFramesUs, clientUs, drawUs, networkUs, logicUs, pacerUs;
	};

	void begin(const char *userDataDir, unsigned mapCRC, unsigned seed, int slot, unsigned humanMask, int nativeRate);
	void end();
	bool active();
	void recordFrame(unsigned frame, const FrameSample &sample);
	void networkPoll(unsigned frame, bool ready, bool overdue, unsigned missingFrameInfoMask,
		unsigned missingCommandsMask, bool disconnectHold, int router, int rate, int runAhead, int cushion,
		bool granted, float avgFrameAckMs);
	void frameResend(unsigned frame, int slot, bool received);
	void packetSent(int peerSlot, bool repeated);
	void frameInfoReceived(int slot, unsigned executionFrame);
	void routerPlan(unsigned frame, unsigned appliesAt, int slowSlot, int otherTargetHz, int slowTargetHz,
		int plannedRunAhead, const int *reportedFps, const float *reportedLatencySeconds);
}
