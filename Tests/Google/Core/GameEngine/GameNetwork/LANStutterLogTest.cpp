/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "GameNetwork/LANStutterLog.h"
#include "../Common/FileSystemTestFixture.h"

class LANStutterLogTest : public FileSystemTestFixture
{
protected:
	void TearDown() override
	{
		LANStutterLog::end();
		FileSystemTestFixture::TearDown();
	}
};

TEST_F(LANStutterLogTest, DistinguishesPacingPeerWaitAndSlowDrawAcrossMatches)
{
	using namespace LANStutterLog;
	const char *dir = "LANStutterLogs";
	recordFrame(1, {});
	EXPECT_FALSE(active());
	EXPECT_FALSE(std::filesystem::exists(dir));
	begin(".", 123, 456, 1, 7, 60);
	ASSERT_TRUE(active());
	// Before the network deadline, incomplete input is ordinary pacing.
	networkPoll(80, false, false, 1, 4, false, 0, 60, 8, 2, false, 30.0f);
	std::this_thread::sleep_for(std::chrono::milliseconds(65));
	networkPoll(80, true, false, 0, 0, false, 0, 60, 8, 2, true, 31.0f);
	networkPoll(81, false, true, 4, 1, false, 0, 60, 8, 0, false, 31.0f);
	frameInfoReceived(2, 86);
	frameInfoReceived(2, 84); // Late duplicate must not lower the high-water mark.
	packetSent(2, true);
	frameResend(81, 0, false);
	const int fps[8] = {60, 58, 24, -1, -1, -1, -1, -1};
	const float ack[8] = {0.025f, 0.030f, 0.135f};
	routerPlan(81, 90, 2, 24, 26, 8, fps, ack);
	std::this_thread::sleep_for(std::chrono::milliseconds(65));
	networkPoll(81, false, true, 4, 1, false, 0, 60, 8, 0, false, 33.0f);
	networkPoll(81, true, false, 0, 0, false, 0, 60, 8, 2, true, 33.0f);
	recordFrame(82, {81000, 0, 1000, 80000, 0, 0, 0});
	end();
	ASSERT_FALSE(active());
	const auto first = std::filesystem::directory_iterator(dir)->path();
	std::ifstream source(first);
	const std::string log((std::istreambuf_iterator<char>(source)), {});
	EXPECT_NE(log.find("local_slot=1 human_mask=0x7 native_hz=60"), std::string::npos);
	EXPECT_NE(log.find("wait_open "), std::string::npos);
	EXPECT_NE(log.find("wait_end "), std::string::npos);
	EXPECT_NE(log.find("grant_gap "), std::string::npos);
	EXPECT_EQ(log.find("wait_frame=80"), std::string::npos);
	EXPECT_NE(log.find("wait_frame=81 wait_info=0x4 wait_commands=0x1"), std::string::npos);
	EXPECT_NE(log.find("frame_info_high=0,0,86,"), std::string::npos);
	EXPECT_NE(log.find("queued_retries=0,0,1,"), std::string::npos);
	EXPECT_NE(log.find("applies_at=90 slow_slot=2 others_hz=24 slow_slot_hz=26"), std::string::npos);
	EXPECT_NE(log.find("reported_fps=60,58,24,"), std::string::npos);
	EXPECT_NE(log.find("slow_loop "), std::string::npos);
	EXPECT_NE(log.find(" client_us=1000 draw_us=80000"), std::string::npos);
	EXPECT_NE(log.find("closed dropped_records=0"), std::string::npos);

	begin(".", 123, 456, 1, 7, 60);
	end();
	EXPECT_EQ(std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator()), 2);
}
