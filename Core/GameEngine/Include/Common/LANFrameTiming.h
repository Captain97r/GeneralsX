/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "GameNetwork/LANStutterLog.h"
#include <chrono>

// GeneralsX @feature Codex 23/11/2026 Capture wall time across the engine loop and its nested draw on the game thread.
class LANFrameTiming
{
public:
	using Sample = LANStutterLog::FrameSample;
	using Clock = std::chrono::steady_clock;

	static LANFrameTiming& get()
	{
		static LANFrameTiming timing;
		return timing;
	}

	void begin(unsigned frame)
	{
		m_collecting = LANStutterLog::active();
		if (!m_collecting)
		{
			m_previousEnd = {};
			return;
		}
		m_start = Clock::now();
		m_sample = {};
		if (m_previousEnd != Clock::time_point{} && frame >= m_lastFrame)
			m_sample.betweenFramesUs = microseconds(m_start - m_previousEnd);
		m_lastFrame = frame;
	}

	template<typename Operation>
	void measure(unsigned long long Sample::*field, Operation operation)
	{
		if (!m_collecting)
		{
			operation();
			return;
		}
		const auto drawBefore = m_sample.drawUs;
		const auto start = Clock::now();
		operation();
		auto elapsed = microseconds(Clock::now() - start);
		if (field == &Sample::clientUs)
		{
			// Report only the non-draw part of this client update. Other draws
			// in the same iteration do not belong to this interval.
			const auto nestedDraw = m_sample.drawUs - drawBefore;
			elapsed = elapsed >= nestedDraw ? elapsed - nestedDraw : 0;
		}
		m_sample.*field += elapsed;
	}

	void finish(unsigned frame)
	{
		if (!m_collecting)
			return;
		m_collecting = false;
		if (!LANStutterLog::active() || frame < m_lastFrame)
		{
			m_previousEnd = {};
			return;
		}
		m_previousEnd = Clock::now();
		m_lastFrame = frame;
		m_sample.frameUs = microseconds(m_previousEnd - m_start);
		LANStutterLog::recordFrame(frame, m_sample);
	}

private:
	static unsigned long long microseconds(Clock::duration duration)
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
	}

	Sample m_sample{};
	Clock::time_point m_start{};
	Clock::time_point m_previousEnd{};
	unsigned m_lastFrame = 0;
	bool m_collecting = false;
};
