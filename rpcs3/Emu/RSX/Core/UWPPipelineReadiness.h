#pragma once

#include <cstdint>
#include <optional>

namespace rsx
{
	// Aggregate at flips, never format a log message on the per-draw path.
	// Counters describe batches reaching VKGSRender::end_draw(), not individual GPU draws.
	struct uwp_pipeline_readiness_sample
	{
		std::uint64_t interval_us;
		std::uint64_t attempted_batches;
		std::uint64_t unavailable_batches;
	};

	class uwp_pipeline_readiness_window
	{
		bool m_initialized = false;
		std::uint32_t m_run = 0;
		std::uint64_t m_started = 0;
		std::uint64_t m_attempted = 0;
		std::uint64_t m_unavailable = 0;

	public:
		std::optional<uwp_pipeline_readiness_sample> record(
			std::uint64_t now, std::uint32_t run, std::uint64_t attempted, std::uint64_t unavailable)
		{
			if (!m_initialized || run != m_run || now < m_started)
			{
				m_initialized = true;
				m_run = run;
				m_started = now;
				m_attempted = m_unavailable = 0;
			}

			m_attempted += attempted;
			m_unavailable += unavailable;
			if (now - m_started < 1'000'000)
				return std::nullopt;

			const uwp_pipeline_readiness_sample sample{now - m_started, m_attempted, m_unavailable};
			m_started = now;
			m_attempted = m_unavailable = 0;
			return sample.attempted_batches ? std::optional{sample} : std::nullopt;
		}
	};
}
