#include "Emu/Cell/lv2/uwp_rsx_diagnostics.h"

#include <cassert>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
		assert(current_record == &record);
		{
			scoped_call nested(&record, {9, 0x102, 0, 0, 0, 0}, 150);
			record.set_phase(phase::display_flip);
			assert(record.read(sample) && sample.args[1] == 0x102);
		}
		assert(record.read(sample) && sample.args[1] == 0x103 && sample.started_us == 100);
		assert(sample.current == phase::event_scheduler_check);
	}
	assert(current_record == nullptr);
	assert(!record.read(sample));
	record.set_phase(phase::event_retry); // An unrelated send_event must not create an active call.
	assert(!record.read(sample));
	{
		scoped_call no_ppu(nullptr, {}, 0);
		set_current_phase(phase::event_full_wait);
		assert(current_record == nullptr);
	}
	// A different host sender must not overwrite the VBlank sender's record.
	{
		scoped_call vblank(&record, {1, 0xFED, 0, 0, 0, 0}, 300);
		set_current_phase(phase::event_full_wait);
		std::thread unrelated([] { set_current_phase(phase::event_retry); });
		unrelated.join();
		assert(record.read(sample) && sample.current == phase::event_full_wait);
		{
			scoped_call no_record(nullptr, {}, 0);
			set_current_phase(phase::event_retry);
		}
		assert(current_record == &record);
		assert(record.read(sample) && sample.current == phase::event_full_wait);
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
	// sys_rsx_mtx owner: published only while held, cleared before unlock.
	{
		lock_owner_record owner_record;
		owner_snapshot owner;
		assert(!owner_record.read(owner));
		std::mutex mtx;
		{
			std::lock_guard lock(mtx);
			scoped_owner held(&owner_record, {owner_kind::vblank, lock_site::context_attribute, 0, 0xFED, 500});
			assert(owner_record.read(owner));
			assert(owner.kind == owner_kind::vblank && owner.site == lock_site::context_attribute);
			assert(owner.package == 0xFED && owner.acquired_us == 500);
		}
		assert(!owner_record.read(owner));
		{
			scoped_owner exempt(nullptr, {owner_kind::other_cpu, lock_site::context_attribute, 7, 0xFEC, 1});
			assert(!owner_record.read(owner)); // RSX engine calls skip the lock and publish nothing.
		}

		// Writers serialized by a real mutex; an unlocked reader never sees mixed fields
		// and never sees an owner while the mutex is free.
		std::atomic<bool> owner_done{false};
		std::atomic<unsigned> owner_samples{0};
		std::thread owner_reader([&]
		{
			while (!owner_done.load())
			{
				owner_snapshot seen;
				if (owner_record.read(seen))
				{
					assert(seen.thread_id == seen.package && seen.acquired_us == seen.thread_id);
					assert(seen.site == (seen.thread_id % 2 ? lock_site::context_iomap : lock_site::context_attribute));
					++owner_samples;
				}
			}
		});
		std::vector<std::thread> writers;
		for (std::uint32_t w = 0; w < 4; ++w)
		{
			writers.emplace_back([&, w]
			{
				for (std::uint32_t i = 1 + w; i <= 40'000; i += 4)
				{
					std::lock_guard lock(mtx);
					scoped_owner held(&owner_record, {owner_kind::ppu, i % 2 ? lock_site::context_iomap : lock_site::context_attribute, i, i, i});
				}
			});
		}
		for (auto& writer : writers)
			writer.join();
		owner_done.store(true);
		owner_reader.join();
		assert(!owner_record.read(owner));
		assert(owner_samples.load() > 0);
		assert(std::string(owner_kind_name(owner_kind::vblank)) == "vblank");
		assert(std::string(lock_site_name(lock_site::device_map)) == "device_map");
	}
	std::cout << "RSX record coherence, lock-owner publication, scope cleanup and bounded late sampling passed\n";
}
