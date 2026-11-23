/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "PreRTS.h"
#include "GameNetwork/LANStutterLog.h"
#include "GameNetwork/NetworkDefs.h"
#include "gitinfo.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace LANStutterLog
{
namespace
{
using Clock = std::chrono::steady_clock;
using Micros = unsigned long long;
using Slots = std::array<unsigned, MAX_SLOTS>;
constexpr Micros StutterUs = 50000;
constexpr Micros SummaryUs = 1000000;

unsigned slotBit(int slot)
{
	return slot >= 0 && slot < MAX_SLOTS ? 1u << slot : 0;
}

struct NetworkSample
{
	unsigned frame = 0, infoMask = 0, commandsMask = 0;
	bool ready = false, overdue = false, disconnectHold = false;
	int router = -1, rate = 0, runAhead = 0, cushion = 0;
	float avgFrameAckMs = 0;
	Micros grantGap = 0;
};

struct Window
{
	unsigned renders = 0, slowFrames = 0, polls = 0, grants = 0, longGrantGaps = 0;
	unsigned resendTx = 0, resendRx = 0, resendTxMask = 0, resendRxMask = 0, lastResendFrame = 0;
	Slots packets{}, retries{};
	FrameSample peak{};
	Micros maxGrantGap = 0;
};

struct Record
{
	const char *event;
	Micros time, waitUs;
	unsigned frame, waitFrame, waitInfo, waitCommands;
	bool waitDisconnect;
	NetworkSample net;
	Window window;
	Slots highFrames;
	FrameSample sample;
	struct Routing
	{
		bool present = false;
		unsigned appliesAt = 0;
		int slowSlot = -1, otherHz = 0, slowHz = 0, runAhead = 0;
		std::array<int, MAX_SLOTS> reportedFps{};
		std::array<float, MAX_SLOTS> reportedLatencyMs{};
	} plan{};
};

void writeSlots(std::FILE *file, const char *name, const Slots &values)
{
	std::fprintf(file, " %s=", name);
	for (size_t i = 0; i < values.size(); ++i)
		std::fprintf(file, "%s%u", i ? "," : "", values[i]);
}

void writeTimes(std::FILE *file, const char *prefix, const FrameSample &s)
{
	std::fprintf(file, " %sframe_us=%llu %sgap_us=%llu %sclient_us=%llu %sdraw_us=%llu"
		" %snet_us=%llu %slogic_us=%llu %spacer_us=%llu", prefix, s.frameUs, prefix, s.betweenFramesUs,
		prefix, s.clientUs, prefix, s.drawUs, prefix, s.networkUs, prefix, s.logicUs, prefix, s.pacerUs);
}

void writeRecord(std::FILE *file, const Record &r)
{
	const auto &n = r.net;
	const auto &w = r.window;
	if (r.plan.present)
	{
		const auto &p = r.plan;
		std::fprintf(file, "router_plan t_us=%llu frame=%u applies_at=%u slow_slot=%d"
			" others_hz=%d slow_slot_hz=%d planned_run_ahead=%d reported_fps=",
			r.time, r.frame, p.appliesAt, p.slowSlot, p.otherHz, p.slowHz, p.runAhead);
		for (int i = 0; i < MAX_SLOTS; ++i)
			std::fprintf(file, "%s%d", i ? "," : "", p.reportedFps[i]);
		std::fputs(" reported_ack_ms=", file);
		for (int i = 0; i < MAX_SLOTS; ++i)
			std::fprintf(file, "%s%.1f", i ? "," : "", static_cast<double>(p.reportedLatencyMs[i]));
		std::fputc('\n', file);
		return;
	}
	std::fprintf(file, "%s t_us=%llu frame=%u poll_frame=%u ready=%d due_blocked=%d"
		" info_missing=0x%x commands_missing=0x%x disconnect_hold=%d router=%d target_hz=%d run_ahead=%d"
		" cushion=%d frame_ack_ms=%.1f grant_gap_us=%llu wait_us=%llu wait_frame=%u wait_info=0x%x wait_commands=0x%x"
		" wait_disconnect=%d renders=%u slow_frames=%u polls=%u grants=%u long_grant_gaps=%u max_grant_gap_us=%llu"
		" resend_tx=%u resend_rx=%u resend_tx_mask=0x%x resend_rx_mask=0x%x last_resend_frame=%u",
		r.event, r.time, r.frame, n.frame, int(n.ready), int(n.overdue), n.infoMask, n.commandsMask,
		int(n.disconnectHold), n.router, n.rate, n.runAhead, n.cushion, static_cast<double>(n.avgFrameAckMs),
		n.grantGap, r.waitUs, r.waitFrame, r.waitInfo, r.waitCommands, int(r.waitDisconnect), w.renders, w.slowFrames,
		w.polls, w.grants, w.longGrantGaps, w.maxGrantGap, w.resendTx, w.resendRx, w.resendTxMask, w.resendRxMask, w.lastResendFrame);
	writeSlots(file, "frame_info_high", r.highFrames);
	writeSlots(file, "queued_packets", w.packets);
	writeSlots(file, "queued_retries", w.retries);
	writeTimes(file, "peak_", w.peak);
	writeTimes(file, "", r.sample);
	std::fputc('\n', file);
}

// One producer (the game thread) and one disk writer. A slow disk can lose log
// records, but cannot hold up the match. end() drains the queue between games.
class Writer
{
public:
	explicit Writer(std::FILE *file) : m_file(file), m_thread(&Writer::run, this) {}

	~Writer()
	{
		{
			std::lock_guard<std::mutex> guard(m_mutex);
			m_stop = true;
		}
		m_wake.notify_one();
		m_thread.join();
	}

	void push(const Record &record)
	{
		const unsigned head = m_head.load(std::memory_order_relaxed);
		const unsigned next = (head + 1) % m_rows.size();
		if (next == m_tail.load(std::memory_order_acquire))
		{
			++m_dropped;
			return;
		}
		m_rows[head] = record;
		m_head.store(next, std::memory_order_release);
	}

private:
	void drain()
	{
		unsigned tail = m_tail.load(std::memory_order_relaxed);
		const unsigned head = m_head.load(std::memory_order_acquire);
		if (tail == head)
			return;
		while (tail != head)
		{
			writeRecord(m_file, m_rows[tail]);
			tail = (tail + 1) % m_rows.size();
		}
		m_tail.store(tail, std::memory_order_release);
		std::fflush(m_file);
	}

	void run()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		while (!m_wake.wait_for(lock, std::chrono::milliseconds(250), [this] { return m_stop; }))
		{
			lock.unlock();
			drain();
			lock.lock();
		}
		lock.unlock();
		drain();
		std::fprintf(m_file, "closed dropped_records=%u\n", m_dropped);
		if (std::fflush(m_file) != 0 || std::ferror(m_file))
			std::fprintf(stderr, "[LAN diagnostics] Could not finish writing the match log.\n");
	}

	std::FILE *m_file;
	std::array<Record, 128> m_rows{};
	std::atomic<unsigned> m_head{0}, m_tail{0};
	unsigned m_dropped = 0;
	std::mutex m_mutex;
	std::condition_variable m_wake;
	bool m_stop = false;
	std::thread m_thread;
};

using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

struct Session
{
	explicit Session(File file) : file(std::move(file)), writer(this->file.get()) {}
	File file;
	Writer writer;
	const Clock::time_point start = Clock::now();
	Micros lastSummary = 0, waitStart = 0, lastGrant = 0;
	unsigned frame = 0, waitFrame = 0, waitInfo = 0, waitCommands = 0;
	bool waiting = false, reported = false, waitDisconnect = false, haveGrant = false;
	NetworkSample net{};
	Window window{};
	Slots highFrames{};

	Micros elapsed() const
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
	}

	void emit(const char *event, Micros now, const FrameSample &sample = {})
	{
		writer.push({event, now, waiting ? now - waitStart : 0, frame, waitFrame, waitInfo,
			waitCommands, waitDisconnect, net, window, highFrames, sample});
	}

	void summarize(Micros now)
	{
		if (now - lastSummary >= SummaryUs)
		{
			emit("summary", now);
			window = {};
			lastSummary = now;
		}
	}
};
std::unique_ptr<Session> session;

const char *platformName()
{
#if defined(_WIN32)
	return "Windows";
#elif defined(__linux__)
	return "Linux";
#elif defined(__APPLE__)
	return "Apple";
#else
	return "Other";
#endif
}
} // namespace

void begin(const char *userDataDir, unsigned mapCRC, unsigned seed, int slot, unsigned humanMask, int nativeRate)
{
	end();
	if (!userDataDir || !*userDataDir)
		return;
	try
	{
		const auto dir = std::filesystem::path(userDataDir) / "LANStutterLogs";
		std::filesystem::create_directories(dir);
		const auto wall = std::chrono::system_clock::now();
		const auto utcMs = std::chrono::duration_cast<std::chrono::milliseconds>(wall.time_since_epoch()).count();
		const std::time_t timestamp = std::chrono::system_clock::to_time_t(wall);
		char utc[32] = "unknown-date";
		if (const auto *tm = std::gmtime(&timestamp))
			std::strftime(utc, sizeof(utc), "%Y%m%d-%H%M%SZ", tm);
		File file(nullptr, &std::fclose);
		std::filesystem::path path;
		for (unsigned attempt = 0; attempt < 1000; ++attempt)
		{
			char name[120];
			std::snprintf(name, sizeof(name), "%s-%08x-slot%d-%u.log", utc, seed, slot, attempt);
			path = dir / name;
			file.reset(std::fopen(path.string().c_str(), "wx"));
			if (file || errno != EEXIST)
				break;
		}
		if (!file)
		{
			std::fprintf(stderr, "[LAN diagnostics] Could not create %s (errno %d).\n", path.string().c_str(), errno);
			return;
		}
#if RTS_ZEROHOUR
		const char *edition = "ZeroHour";
#else
		const char *edition = "Generals";
#endif
		std::fprintf(file.get(), "start format=1 utc_ms=%lld map_crc=%08x seed=%08x local_slot=%d human_mask=0x%x"
			" native_hz=%d edition=%s platform=%s build=%s dirty=%d threshold_us=%llu summary_us=%llu\n",
			static_cast<long long>(utcMs), mapCRC, seed, slot, humanMask, nativeRate, edition, platformName(),
			GitShortSHA1[0] ? GitShortSHA1 : "unknown", int(GitUncommittedChanges), StutterUs, SummaryUs);
		std::fflush(file.get());
		session = std::make_unique<Session>(std::move(file));
		std::fprintf(stderr, "[LAN diagnostics] %s\n", path.string().c_str());
	}
	catch (const std::exception &ex)
	{
		std::fprintf(stderr, "[LAN diagnostics] Could not start match log: %s\n", ex.what());
	}
}

void end()
{
	if (session)
	{
		session->emit("end", session->elapsed());
		session.reset();
	}
}

bool active() { return session != nullptr; }

void recordFrame(unsigned frame, const FrameSample &sample)
{
	if (!session) return;
	Session &s = *session;
	s.frame = frame;
	const Micros now = s.elapsed();
	++s.window.renders;
	auto &peak = s.window.peak;
	peak.frameUs = (std::max)(peak.frameUs, sample.frameUs);
	peak.betweenFramesUs = (std::max)(peak.betweenFramesUs, sample.betweenFramesUs);
	peak.clientUs = (std::max)(peak.clientUs, sample.clientUs);
	peak.drawUs = (std::max)(peak.drawUs, sample.drawUs);
	peak.networkUs = (std::max)(peak.networkUs, sample.networkUs);
	peak.logicUs = (std::max)(peak.logicUs, sample.logicUs);
	peak.pacerUs = (std::max)(peak.pacerUs, sample.pacerUs);
	if (sample.frameUs + sample.betweenFramesUs >= StutterUs && ++s.window.slowFrames <= 4)
		s.emit("slow_loop", now, sample);
	s.summarize(now);
}

void networkPoll(unsigned frame, bool ready, bool overdue, unsigned missingFrameInfoMask,
	unsigned missingCommandsMask, bool disconnectHold, int router, int rate, int runAhead, int cushion,
	bool granted, float avgFrameAckMs)
{
	if (!session) return;
	Session &s = *session;
	const Micros now = s.elapsed();
	s.frame = frame;
	s.net = {frame, missingFrameInfoMask, missingCommandsMask, ready, overdue, disconnectHold,
		router, rate, runAhead, cushion, avgFrameAckMs};
	++s.window.polls;
	if (granted)
	{
		++s.window.grants;
		if (s.haveGrant)
		{
			const Micros gap = now - s.lastGrant;
			s.net.grantGap = gap;
			s.window.maxGrantGap = (std::max)(s.window.maxGrantGap, gap);
			if (gap >= StutterUs && ++s.window.longGrantGaps <= 4)
				s.emit("grant_gap", now);
		}
		s.haveGrant = true;
		s.lastGrant = now;
	}
	if (s.waiting && (ready || !overdue || frame != s.waitFrame))
	{
		if (s.reported || now - s.waitStart >= StutterUs)
			s.emit("wait_end", now);
		s.waiting = s.reported = s.waitDisconnect = false;
		s.waitInfo = s.waitCommands = s.waitFrame = 0;
	}
	if (!ready && overdue)
	{
		if (!s.waiting)
		{
			s.waiting = true;
			s.waitStart = now;
			s.waitFrame = frame;
		}
		s.waitInfo |= missingFrameInfoMask;
		s.waitCommands |= missingCommandsMask;
		s.waitDisconnect |= disconnectHold;
		if (!s.reported && now - s.waitStart >= StutterUs)
		{
			s.reported = true;
			s.emit("wait_open", now);
		}
	}
	s.summarize(now);
}

void frameResend(unsigned frame, int slot, bool received)
{
	if (!session) return;
	Window &w = session->window;
	if (received)
	{
		++w.resendRx;
		w.resendRxMask |= slotBit(slot);
	}
	else
	{
		++w.resendTx;
		w.resendTxMask |= slotBit(slot);
	}
	w.lastResendFrame = frame;
}

void packetSent(int peerSlot, bool repeated)
{
	if (!session || !slotBit(peerSlot)) return;
	++session->window.packets[peerSlot];
	if (repeated) ++session->window.retries[peerSlot];
}

void frameInfoReceived(int slot, unsigned executionFrame)
{
	if (!session || !slotBit(slot)) return;
	session->highFrames[slot] = (std::max)(session->highFrames[slot], executionFrame);
}

void routerPlan(unsigned frame, unsigned appliesAt, int slowSlot, int otherTargetHz, int slowTargetHz,
	int plannedRunAhead, const int *reportedFps, const float *reportedLatencySeconds)
{
	if (!session) return;
	Record record{};
	record.time = session->elapsed();
	record.frame = frame;
	auto &p = record.plan;
	p.present = true;
	p.appliesAt = appliesAt;
	p.slowSlot = slowSlot;
	p.otherHz = otherTargetHz;
	p.slowHz = slowTargetHz;
	p.runAhead = plannedRunAhead;
	for (int i = 0; i < MAX_SLOTS; ++i)
	{
		p.reportedFps[i] = reportedFps[i];
		p.reportedLatencyMs[i] = reportedLatencySeconds[i] * 1000.0f;
	}
	session->writer.push(record);
}
} // namespace LANStutterLog
