#include "Emu/Cell/lv2/uwp_rsx_diagnostics.h"

#include <cassert>
#include <iostream>
#include <thread>

using namespace uwp_rsx_diagnostics;

int main()
{
	call_record record;
	snapshot sample;
	assert(!record.read(sample));
	{
		scoped_call call(&record, {1, 0x103, 3, 4, 5, 6}, 100);
		record.set_phase(phase::event_scheduler_check);
		assert(record.read(sample));
		assert(sample.args[1] == 0x103 && sample.started_us == 100);
		assert(sample.current == phase::event_scheduler_check);
		{
			scoped_call nested(&record, {9, 0x102, 0, 0, 0, 0}, 150);
			record.set_phase(phase::display_flip);
			assert(record.read(sample) && sample.args[1] == 0x102);
		}
		assert(record.read(sample) && sample.args[1] == 0x103 && sample.started_us == 100);
		assert(sample.current == phase::event_scheduler_check);
	}
	assert(!record.read(sample));
	record.set_phase(phase::event_retry); // An unrelated send_event must not create an active call.
	assert(!record.read(sample));
	{
		scoped_call no_ppu(nullptr, {}, 0);
	}
	const auto early_return = [&]
	{
		scoped_call call(&record, {2, 0x102, 0, 0, 0, 0}, 200);
		return;
	};
	early_return();
	assert(!record.read(sample));

	// A reader must never combine argument words or timestamps from different calls.
	std::atomic<bool> start{false}, done{false};
	std::atomic<unsigned> valid_samples{0};
	std::thread reader([&]
	{
		while (!start.load()) {}
		while (!done.load())
		{
			snapshot concurrent;
			if (record.read(concurrent))
			{
				for (const auto arg : concurrent.args)
					assert(arg == concurrent.started_us);
				++valid_samples;
			}
		}
	});
	start.store(true);
	for (std::uint64_t i = 1; i <= 100'000; ++i)
	{
		scoped_call call(&record, {i, i, i, i, i, i}, i);
		record.set_phase(phase::context_lock);
		if (i % 100 == 0)
			std::this_thread::yield();
	}
	done.store(true);
	reader.join();
	assert(!record.read(sample));
	assert(valid_samples.load() > 0);

	stall_schedule schedule;
	unsigned reports = 0;
	for (std::uint32_t tick = 0; tick <= 60'000; ++tick)
		reports += schedule.should_report(tick);
	assert(reports == 25);
	assert(!schedule.should_report(600'000));
	schedule.reset();
	assert(!schedule.should_report(99));
	assert(schedule.should_report(100));
	assert(!schedule.should_report(100));
	assert(schedule.should_report(2'500)); // A busy UI skips missed samples without a log burst.
	assert(!schedule.should_report(2'500));
	assert(schedule.should_report(3'000));
	assert(schedule.should_report(6'000)); // Old twenty-sample cap missed this post-install observation.
	std::cout << "RSX record coherence, scope cleanup and bounded late sampling passed\n";
}
