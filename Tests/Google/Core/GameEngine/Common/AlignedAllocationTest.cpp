/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <new>

TEST(AlignedAllocation, AcceptsSmallAndOverAlignedRequests)
{
	const std::size_t alignments[] = {1, 2, 4, 8, 16, 64};
	for (std::size_t alignment : alignments)
	{
		SCOPED_TRACE(alignment);
		const auto requested = static_cast<std::align_val_t>(alignment);
		void *scalar = ::operator new(19, requested);
		EXPECT_EQ(reinterpret_cast<std::uintptr_t>(scalar) % alignment, 0u);
		::operator delete(scalar, requested);

		void *array = ::operator new[](19, requested);
		EXPECT_EQ(reinterpret_cast<std::uintptr_t>(array) % alignment, 0u);
		::operator delete[](array, requested);
	}
}
