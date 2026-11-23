/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../GameEngine/Common/FileSystemTestFixture.h"
#include "MiniAudioDevice/MiniAudioMusicStream.h"

#include <chrono>
#include <thread>

using MiniAudioMusicStreamTest = FileSystemTestFixture;

TEST_F(MiniAudioMusicStreamTest, ContinuesAcrossTrackBoundary)
{
	// An eight-sample PCM track. Read past EOF to exercise the producer's rewind.
	const unsigned char wave[] = {
		'R','I','F','F', 44,0,0,0, 'W','A','V','E', 'f','m','t',' ',
		16,0,0,0, 1,0, 1,0, 0x40,0x1f,0,0, 0x40,0x1f,0,0, 1,0, 8,0,
		'd','a','t','a', 8,0,0,0, 129,130,131,132,133,134,135,136
	};
	writeLooseFile("loop.wav", std::string(reinterpret_cast<const char *>(wave), sizeof(wave)));
	File *file = TheFileSystem->openFile("loop.wav");
	ASSERT_NE(file, nullptr);
	MiniAudioMusicStream stream;
	ASSERT_TRUE(stream.open(file));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	unsigned samples = 0;
	while (samples < 32 && std::chrono::steady_clock::now() < deadline) {
		unsigned char output[8];
		ma_uint64 read = 0;
		ASSERT_EQ(ma_data_source_read_pcm_frames(stream.source(), output, 8, &read), MA_SUCCESS);
		ASSERT_EQ(read, 8u);
		for (unsigned char sample : output) {
			if (sample == 128) continue; // The empty ring provides silence while the worker starts.
			ASSERT_EQ(sample, 129 + samples % 8);
			++samples;
		}
		ASSERT_FALSE(stream.failed());
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	EXPECT_GE(samples, 32u);
}

TEST_F(MiniAudioMusicStreamTest, RejectsAnInvalidTrack)
{
	writeLooseFile("invalid.mp3", "Invalid audio");
	File *file = TheFileSystem->openFile("invalid.mp3");
	ASSERT_NE(file, nullptr);
	MiniAudioMusicStream stream;
	EXPECT_FALSE(stream.open(file));
}
