/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "Common/SimulationValue.h"
#include "Common/Xfer.h"

// Saves and CRCs use the selected game's native scalar representation.
template<typename T, int Power>
void xferSimulationValue(Xfer *xfer, SimulationValue<T, Power>& value)
{
	T scalar = value;
	if constexpr (std::is_same<T, Real>::value)
		xfer->xferReal(&scalar);
	else
		xfer->xferUnsignedInt(&scalar);
	if (xfer->getXferMode() == XFER_LOAD)
		value = scalar;
}
