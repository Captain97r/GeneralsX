/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////


#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/AudioRequest.h"
#include "Common/AudioEventRTS.h"
#include "Common/GameCommon.h"


AudioRequest::~AudioRequest()
{
}

// GeneralsX @bugfix Codex 26/11/2026 Audio is polled once per render, including renders with multiple simulation ticks.
Bool AudioRequest::isReadyToProcess(UnsignedInt now)
{
	if (!m_pendingEvent)
		return true;

#ifdef RTS_EXPERIMENTAL_60HZ_SIMULATION
	const Real delay = m_pendingEvent->getDelay();
	if (now - m_queuedAtMs >= delay)
	{
		// Miles reads any remaining delay when it configures its sample filter.
		m_pendingEvent->decrementDelay(delay);
		return true;
	}
#else
	if (m_pendingEvent->getDelay() < MSEC_PER_LOGICFRAME_REAL)
		return true;
	m_pendingEvent->decrementDelay(MSEC_PER_LOGICFRAME_REAL);
#endif
	m_requiresCheckForSample = true;
	return false;
}

// Keep the retail cutoff for restarting a loop immediately on the audio device.
Bool AudioRequest::shouldQueueLoop(Real delayMs)
{
	return delayMs > static_cast<Real>(MSEC_PER_SECOND) / BaseFps;
}
