#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

using u32 = std::uint32_t;
using s32 = std::int32_t;
using u64 = std::uint64_t;
using usz = std::size_t;
enum class cpu_flag : u32 {};
template<class T> using bs_t = u32;
struct priority { s32 prio = 0; };
struct atomic_priority { priority value; priority load() const { return value; } };
struct ppu_stub
{
	u32 id = 0, state = 0;
	atomic_priority prio;
	bool ack_suspend = false;
	ppu_stub* next_ppu = nullptr;
};
struct lv2_obj
{
	static std::string uwp_scheduler_snapshot();
	static std::mutex g_mutex;
	static u32 g_pending;
	static ppu_stub* g_ppu;
};
std::mutex lv2_obj::g_mutex;
u32 lv2_obj::g_pending = 0;
ppu_stub* lv2_obj::g_ppu = nullptr;
static std::deque<std::pair<u64, ppu_stub*>> g_waiting;
static std::atomic<bool> g_scheduler_ready{false};
struct config { struct core { u32 ppu_threads = 2; } core; } g_cfg;
u64 get_guest_system_time() { return 1'000; }
static u32 runnable_rows = 0;
namespace fmt
{
	template<class... Args> std::string format(const char* pattern, Args&&...)
	{
		assert(lv2_obj::g_mutex.try_lock()); // No formatting under the production scheduler lock.
		lv2_obj::g_mutex.unlock();
		return pattern;
	}
	template<class... Args> void append(std::string& out, const char* pattern, Args&&...)
	{
		assert(lv2_obj::g_mutex.try_lock());
		lv2_obj::g_mutex.unlock();
		if (std::string(pattern).find("runnable[") != std::string::npos)
			++runnable_rows;
		out += pattern;
	}
}

// PRODUCTION_SCHEDULER_SNAPSHOT

int main()
{
	// A different thread owns the lock, as it would in a busy emulator.
	std::atomic<bool> held{false}, release{false};
	std::thread owner([&]
	{
		std::lock_guard lock(lv2_obj::g_mutex);
		held.store(true);
		while (!release.load()) {}
	});
	while (!held.load()) {}
	assert(lv2_obj::uwp_scheduler_snapshot() == "UWP scheduler: unavailable (scheduler mutex busy)");
	release.store(true);
	owner.join();
	assert(lv2_obj::uwp_scheduler_snapshot().find("timed waiters") != std::string::npos);
	assert(runnable_rows == 0);
	std::array<ppu_stub, 80> ppus{};
	for (u32 i = 0; i < ppus.size(); ++i)
	{
		ppus[i].id = i;
		ppus[i].ack_suspend = i == 0;
		ppus[i].next_ppu = i + 1 < ppus.size() ? &ppus[i + 1] : nullptr;
	}
	lv2_obj::g_ppu = ppus.data();
	lv2_obj::g_pending = 1;
	g_scheduler_ready.store(true);
	g_waiting.push_back({900, ppus.data()});
	assert(lv2_obj::uwp_scheduler_snapshot().find("oldest until") != std::string::npos);
	assert(runnable_rows == 64);
	assert(lv2_obj::g_pending == 1 && ppus[0].ack_suspend && g_waiting.size() == 1);
	assert(lv2_obj::g_mutex.try_lock());
	lv2_obj::g_mutex.unlock();
}
