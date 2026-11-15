#pragma once

#include "swapchain_core.h"
#include "util/logs.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>

LOG_CHANNEL(uwp_present_log, "UWPPRESENT");

// UWP has no Vulkan window-system integration (Dozen only presents to HWNDs). Frames are
// copied to host memory like the other native swapchains and handed to the app shell,
// which shows them in its own CoreWindow swapchain.
extern void uwp_get_display_size(u32& width, u32& height);
extern void uwp_present_frame(const void* bgra_pixels, u32 width, u32 height);

namespace vk
{
	void acquire_global_submit_lock();
	void release_global_submit_lock();
	extern atomic_t<u64> g_uwp_present_wait_us, g_uwp_present_copy_us; // VKCommandStream.cpp
	// Off: the RSX thread copies each frame to the shell itself, as before (LocalState/sync_present_copy).
	extern bool g_uwp_async_present_copy; // VKCommandStream.cpp

	class swapchain_UWP : public native_swapchain_base
	{
		// present() runs right after the frame's commands are submitted, before the GPU has
		// copied the image to its host buffer. Reading it then shows an old or half-written
		// frame (judder). Instead each present queues a fence behind the frame and hands the
		// previous frame to the shell once its fence has signalled (one frame of latency).
		std::vector<VkFence> m_fences;
		s32 m_pending = -1;

		// The 8 MB copy of a finished frame into the shell's buffer runs on a thread of its own. On the
		// RSX thread it cost 1.3 ms a frame, and 3-4 ms in busy PS Home scenes where RSX was already the
		// limit. The image stays mapped and in use until the copy is done; the next hand_over frees it.
		std::thread m_copier;
		std::mutex m_copy_mutex;
		std::condition_variable m_copy_cv;
		const void* m_copy_src = nullptr; // set while the copier has a job (guarded by m_copy_mutex)
		u32 m_copy_width = 0, m_copy_height = 0;
		bool m_copier_exit = false;
		s32 m_copy_image = -1; // RSX thread: image mapped for the copier, freed by finish_copy()
		atomic_t<u64> m_copier_us = 0;

		// Frame pacing stats, logged every c_stats_frames presents.
		static constexpr u32 c_stats_frames = 300;
		u64 m_last_present_us = 0;
		u64 m_interval_sum_us = 0, m_interval_max_us = 0, m_wait_sum_us = 0, m_copy_sum_us = 0, m_copier_sum_us = 0;
		u32 m_stats_count = 0;
		u64 m_present_serial = 0, m_pending_serial = 0;
		u32 m_diagnostic_reports = 0, m_wait_timeouts = 0, m_wait_errors = 0, m_submit_errors = 0;
		static constexpr u32 c_diagnostic_reports = 8; // Bound log I/O during a sustained stall.
		static constexpr u64 c_slow_stage_us = 50'000;

		bool allow_diagnostic()
		{
			return m_diagnostic_reports++ < c_diagnostic_reports;
		}

		VkResult wait_for_frame(u32 image)
		{
			const VkResult result = vkWaitForFences(dev, 1, &m_fences[image], VK_TRUE, 1'000'000'000ull);
			m_wait_timeouts += result == VK_TIMEOUT;
			m_wait_errors += result != VK_SUCCESS && result != VK_TIMEOUT;
			return result;
		}

		static u64 now_us()
		{
			LARGE_INTEGER t, f;
			QueryPerformanceCounter(&t);
			QueryPerformanceFrequency(&f);
			return static_cast<u64>(t.QuadPart) * 1000000 / static_cast<u64>(f.QuadPart);
		}

		void copier_loop()
		{
			std::unique_lock lock(m_copy_mutex);
			for (;;)
			{
				m_copy_cv.wait(lock, [&] { return m_copy_src || m_copier_exit; });
				if (!m_copy_src)
					return;
				const void* src = m_copy_src;
				const u32 width = m_copy_width, height = m_copy_height;
				lock.unlock();
				const u64 t0 = now_us();
				uwp_present_frame(src, width, height);
				m_copier_us += now_us() - t0;
				lock.lock();
				m_copy_src = nullptr;
				m_copy_cv.notify_all();
			}
		}

		// Waits for the copier's current job, then unmaps and frees its image (RSX thread).
		void finish_copy()
		{
			if (m_copy_image < 0)
				return;
			{
				std::unique_lock lock(m_copy_mutex);
				m_copy_cv.wait(lock, [&] { return !m_copy_src; });
			}
			auto& img = swapchain_images[m_copy_image];
			img.second->free_pixels();
			img.first = false;
			m_copy_image = -1;
		}

		void hand_over(u32 image)
		{
			const u64 t0 = now_us();
			// Diagnostic only: retain the existing wait duration and hand-over behavior.
			const VkResult wait_result = wait_for_frame(image);
			const u64 t1 = now_us();
			finish_copy();
			const u64 t_copy_done = now_us();
			const void* pixels = swapchain_images[image].second->get_pixels();
			const u64 t_mapped = now_us();
			if (!g_uwp_async_present_copy)
			{
				uwp_present_frame(pixels, m_width, m_height);
				swapchain_images[image].second->free_pixels();
				swapchain_images[image].first = false;
			}
			else
			{
				if (!m_copier.joinable())
					m_copier = std::thread([this] { copier_loop(); });
				{
					std::lock_guard lock(m_copy_mutex);
					m_copy_src = pixels;
					m_copy_width = m_width;
					m_copy_height = m_height;
				}
				m_copy_cv.notify_all();
				m_copy_image = static_cast<s32>(image);
			}
			const u64 t2 = now_us();
			if ((t2 - t0 >= c_slow_stage_us || wait_result != VK_SUCCESS) && allow_diagnostic())
			{
				uwp_present_log.notice("Present diagnostic: frame %u image %u handover %.3f ms: fence %.3f, copier drain %.3f, map %.3f, publish %.3f ms; fence result %d",
					m_pending_serial, image, (t2 - t0) / 1000., (t1 - t0) / 1000., (t_copy_done - t1) / 1000.,
					(t_mapped - t_copy_done) / 1000., (t2 - t_mapped) / 1000., static_cast<s32>(wait_result));
			}
			m_wait_sum_us += t1 - t0;
			m_copy_sum_us += t2 - t1;
			g_uwp_present_wait_us += t1 - t0;
			g_uwp_present_copy_us += t2 - t1;
		}

		void release_fences()
		{
			finish_copy();
			if (m_pending >= 0)
			{
				const u64 begin = now_us();
				const VkResult result = wait_for_frame(static_cast<u32>(m_pending));
				const u64 elapsed = now_us() - begin;
				if ((result != VK_SUCCESS || elapsed >= c_slow_stage_us) && allow_diagnostic())
					uwp_present_log.notice("Present diagnostic: frame %u image %d release fence result %d, %.3f ms",
						m_pending_serial, m_pending, static_cast<s32>(result), elapsed / 1000.);
				swapchain_images[m_pending].first = false;
				m_pending = -1;
			}
			for (VkFence f : m_fences)
				vkDestroyFence(dev, f, nullptr);
			m_fences.clear();
		}

	public:
		using native_swapchain_base::native_swapchain_base;

		~swapchain_UWP() override
		{
			finish_copy();
			if (m_copier.joinable())
			{
				{
					std::lock_guard lock(m_copy_mutex);
					m_copier_exit = true;
				}
				m_copy_cv.notify_all();
				m_copier.join();
			}
		}

		bool init() override
		{
			if (!swapchain_images.empty())
				destroy(false);

			uwp_get_display_size(m_width, m_height);
			init_swapchain_images(dev, 3);

			m_fences.resize(swapchain_images.size());
			for (VkFence& f : m_fences)
			{
				const VkFenceCreateInfo info{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
				vkCreateFence(dev, &info, nullptr, &f);
			}
			uwp_present_log.notice("Present diagnostics v1: fence results, submit lock, copier drain and map timings; max %u details per %u presents; rendering behavior unchanged",
				c_diagnostic_reports, c_stats_frames);
			return true;
		}

		void create(display_handle_t& handle) override
		{
			window_handle = handle;
		}

		void destroy(bool full = true) override
		{
			release_fences();
			swapchain_images.clear();

			if (full)
				dev.destroy();
		}

		VkResult present(VkSemaphore /*semaphore*/, u32 image) override
		{
			// Fence behind this frame's work (queue order: signals once the copy is done).
			++m_present_serial;
			const VkResult reset_result = vkResetFences(dev, 1, &m_fences[image]);
			const u64 submit_begin = now_us();
			acquire_global_submit_lock();
			const u64 submit_locked = now_us();
			const VkResult submit_result = vkQueueSubmit(dev.get_graphics_queue(), 0, nullptr, m_fences[image]);
			release_global_submit_lock();
			const u64 submit_done = now_us();
			m_submit_errors += reset_result != VK_SUCCESS || submit_result != VK_SUCCESS;
			if ((reset_result != VK_SUCCESS || submit_result != VK_SUCCESS || submit_done - submit_begin >= c_slow_stage_us) && allow_diagnostic())
			{
				uwp_present_log.notice("Present diagnostic: frame %u image %u reset result %d, submit result %d, submit lock %.3f ms, submit/release %.3f ms",
					m_present_serial, image, static_cast<s32>(reset_result), static_cast<s32>(submit_result),
					(submit_locked - submit_begin) / 1000., (submit_done - submit_locked) / 1000.);
			}

			if (m_pending >= 0)
				hand_over(static_cast<u32>(m_pending));
			m_pending = static_cast<s32>(image);
			m_pending_serial = m_present_serial;

			const u64 now = now_us();
			if (!m_last_present_us)
				uwp_present_log.notice("First frame presented (frame copy on %s)", g_uwp_async_present_copy ? "its own thread" : "the RSX thread");
			if (m_last_present_us)
			{
				const u64 interval = now - m_last_present_us;
				if (interval >= 250'000)
					uwp_present_log.notice("Frame gap of %.3f s", interval / 1e6);
				m_interval_sum_us += interval;
				m_interval_max_us = std::max(m_interval_max_us, interval);
				if (++m_stats_count == c_stats_frames)
				{
					const u64 copier_us = m_copier_us.load();
					uwp_present_log.notice(" %.1f fps avg, frame interval avg %.1f ms max %.1f ms, GPU wait %.2f ms, copy %.2f ms (copy thread %.2f ms)",
						1e6 * m_stats_count / m_interval_sum_us, m_interval_sum_us / 1000.0 / m_stats_count, m_interval_max_us / 1000.0,
						m_wait_sum_us / 1000.0 / m_stats_count, m_copy_sum_us / 1000.0 / m_stats_count, (copier_us - m_copier_sum_us) / 1000.0 / m_stats_count);
					m_stats_count = 0;
					uwp_present_log.notice("Present diagnostic window: fence timeouts %u, fence errors %u, reset/submit errors %u, detail candidates %u (limit %u)",
						m_wait_timeouts, m_wait_errors, m_submit_errors, m_diagnostic_reports, c_diagnostic_reports);
					m_diagnostic_reports = m_wait_timeouts = m_wait_errors = m_submit_errors = 0;
					m_interval_sum_us = m_interval_max_us = m_wait_sum_us = m_copy_sum_us = 0;
					m_copier_sum_us = copier_us;
				}
			}
			m_last_present_us = now;
			return VK_SUCCESS;
		}
	};

	using swapchain_NATIVE = swapchain_UWP;

	[[maybe_unused]] static
	VkSurfaceKHR make_WSI_surface(VkInstance, display_handle_t, WSI_config*)
	{
		return VK_NULL_HANDLE;
	}
}
