/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "Common/GameCommon.h"
#include "WWMath/wwmath.h"

#include <cstdint>
#include <limits>
#include <type_traits>

// GeneralsX @feature Codex 23/11/2026 INI templates outlive a match. Keep their authored
// quantities independent of the clock used while the template or a map override was loaded.
template<typename T, int FramePower>
class SimulationValue
{
public:
	static constexpr Int storageRate = 60;
	SimulationValue() = default;
	explicit SimulationValue(T value) { *this = value; }

	SimulationValue& operator=(T value)
	{
		if constexpr (std::is_integral<T>::value)
		{
			static_assert(FramePower == 1, "integral simulation values are durations");
			if constexpr (std::is_signed<T>::value)
				if (value < 0)
				{
					m_value = value;
					return *this;
				}
			if (value == (std::numeric_limits<T>::max)())
				m_value = static_cast<Storage>(value) * 2;
			else
				m_value = static_cast<Storage>(value) * (getSimulationRate() == storageRate ? 1 : 2);
		}
		else
			m_value = value / readScale(getSimulationRate());
		return *this;
	}

	operator T() const { return atRate(getSimulationRate()); }

	// GeneralsX @feature Codex 23/11/2026 Authored weapon and particle durations can use the retail clock.
	T atRate(Int rate) const
	{
		if constexpr (std::is_integral<T>::value)
		{
			if constexpr (std::is_signed<T>::value)
				if (m_value < 0) return static_cast<T>(m_value);
			const Storage ticks = rate == storageRate ? m_value : (m_value + 1) / 2;
			const Storage maximum = (std::numeric_limits<T>::max)();
			return static_cast<T>(ticks > maximum ? maximum : ticks);
		}
		else
			return m_value * readScale(rate);
	}

	void setMilliseconds(Real milliseconds)
	{
		static_assert(FramePower == 1, "milliseconds specify a duration");
		const Real ticks = milliseconds * (Real(storageRate) / MSEC_PER_SECOND);
		if constexpr (std::is_integral<T>::value)
			m_value = static_cast<Storage>(WWMath::Ceilf(ticks));
		else
			m_value = ticks;
	}

	void setPerSecond(Real amount, Real unitScale = 1.0f)
	{
		static_assert(FramePower == -1 || FramePower == -2, "expected a rate or acceleration");
		constexpr Real secondsPerTick = 1.0f / storageRate;
		m_value = amount * (FramePower == -1 ? secondsPerTick * unitScale : secondsPerTick * secondsPerTick);
	}

private:
	static Real readScale(Int rate)
	{
		if (rate == storageRate) return 1.0f;
		if constexpr (FramePower == 1) return 0.5f;
		if constexpr (FramePower == -1) return 2.0f;
		return 4.0f;
	}

	// A valid 30 Hz integer duration can need one extra bit at the canonical 60 Hz.
	using Storage = typename std::conditional<std::is_integral<T>::value,
		typename std::conditional<std::is_signed<T>::value, int64_t, uint64_t>::type, T>::type;
	Storage m_value;
};

template<typename T> using SimulationFrameValue = SimulationValue<T, 1>;
template<typename T> using SimulationRateValue = SimulationValue<T, -1>;
template<typename T> using SimulationAccelerationValue = SimulationValue<T, -2>;
