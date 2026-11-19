#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// One writer (the owning PPU), observed without locks by the UWP stall reporter.
// No logging, allocation, waiting or guest/scheduler state changes on the hot path.
namespace uwp_rsx_diagnostics
{
	enum class phase : std::uint32_t
	{
		idle,
		entry,
		context_lock,
		context_lookup,
		fifo_control,
		display_flip,
		frame_end,
		event_send,
		event_full_wait,
		event_scheduler_check,
		event_retry,
	};

	inline const char* phase_name(phase value)
	{
		switch (value)
		{
		case phase::idle: return "idle";
		case phase::entry: return "entry";
		case phase::context_lock: return "context-lock";
		case phase::context_lookup: return "context-lookup";
		case phase::fifo_control: return "fifo-control";
		case phase::display_flip: return "display-flip";
		case phase::frame_end: return "frame-end";
		case phase::event_send: return "event-send";
		case phase::event_full_wait: return "event-full-wait";
		case phase::event_scheduler_check: return "event-scheduler-check";
		case phase::event_retry: return "event-retry";
		}
		return "unknown";
	}

	struct snapshot
	{
		std::array<std::uint64_t, 6> args{};
		std::uint64_t started_us = 0;
		phase current = phase::idle;
	};

	class call_record
	{
		static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
		// All fields are atomic. Sequence validation rejects a concurrent begin/end;
		// phase updates during the same call do not invalidate its immutable arguments.
		std::atomic<std::uint64_t> m_sequence{0};
		std::array<std::atomic<std::uint64_t>, 6> m_args{};
		std::atomic<std::uint64_t> m_started_us{0};
		std::atomic<phase> m_phase{phase::idle};

	public:
		void begin(const std::array<std::uint64_t, 6>& args, std::uint64_t started_us, phase initial = phase::entry)
		{
			m_sequence.fetch_add(1);
			for (std::size_t i = 0; i < args.size(); ++i)
				m_args[i].store(args[i]);
			m_started_us.store(started_us);
			m_phase.store(initial);
			m_sequence.fetch_add(1);
		}

		void set_phase(phase value)
		{
			if (m_phase.load() != phase::idle)
				m_phase.store(value);
		}

		void finish()
		{
			m_sequence.fetch_add(1);
			m_phase.store(phase::idle);
			m_sequence.fetch_add(1);
		}

		bool read(snapshot& result) const
		{
			const auto before = m_sequence.load();
			if (before & 1)
				return false;
			result.current = m_phase.load();
			result.started_us = m_started_us.load();
			for (std::size_t i = 0; i < result.args.size(); ++i)
				result.args[i] = m_args[i].load();
			return before == m_sequence.load() && result.current != phase::idle;
		}
	};

	class scoped_call
	{
		call_record* m_record;
		snapshot m_previous;
		bool m_restore = false;

	public:
		scoped_call(call_record* record, const std::array<std::uint64_t, 6>& args, std::uint64_t started_us)
			: m_record(record)
		{
			if (m_record)
			{
				m_restore = m_record->read(m_previous);
				m_record->begin(args, started_us);
			}
		}
		~scoped_call()
		{
			if (m_record)
			{
				if (m_restore)
					m_record->begin(m_previous.args, m_previous.started_us, m_previous.current);
				else
					m_record->finish();
			}
		}
		scoped_call(const scoped_call&) = delete;
		scoped_call& operator=(const scoped_call&) = delete;
	};

	// Preserve early samples, then retain bounded observations after slow host I/O.
	class stall_schedule
	{
		static constexpr std::array<std::uint32_t, 25> thresholds = {
			100, 200, 300, 400, 500, 600, 700, 800, 900, 1000,
			1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800, 1900, 2000,
			3000, 6000, 12000, 30000, 60000};
		std::size_t m_next = 0;

	public:
		void reset() { m_next = 0; }
		bool should_report(std::uint32_t idle_ticks)
		{
			if (m_next == thresholds.size() || idle_ticks < thresholds[m_next])
				return false;
			while (m_next < thresholds.size() && idle_ticks >= thresholds[m_next])
				++m_next;
			return true;
		}
	};
}
