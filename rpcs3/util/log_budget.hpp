#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace logs
{
	// Reserve complete records before concurrent producers enqueue them. A severe
	// record can use the final reserve; ordinary traffic cannot exhaust it.
	class log_budget
	{
		const std::uint64_t m_limit;
		const std::uint64_t m_ordinary_limit;
		std::atomic<std::uint64_t> m_accepted;

	public:
		log_budget(std::uint64_t limit, std::uint64_t reserve, std::uint64_t initial = 0)
			: m_limit(limit)
			, m_ordinary_limit(limit - std::min(limit, reserve))
			, m_accepted(initial)
		{
		}

		bool reserve_line(std::uint64_t size, bool severe)
		{
			const auto limit = severe ? m_limit : m_ordinary_limit;
			auto accepted = m_accepted.load(std::memory_order_relaxed);
			for (;;)
			{
				if (accepted > limit || size > limit - accepted)
					return false;
				if (m_accepted.compare_exchange_weak(accepted, accepted + size, std::memory_order_relaxed))
					return true;
			}
		}
	};
}
