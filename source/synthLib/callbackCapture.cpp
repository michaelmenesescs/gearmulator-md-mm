#include "callbackCapture.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

namespace synthLib::callbackCapture
{
	namespace
	{
		// 2^20 records = 24 MiB, about 90 minutes at 48 kHz / 256 frames
		constexpr size_t g_capacity = size_t(1) << 20;

		struct State
		{
			std::unique_ptr<Record[]> records;
			std::atomic<size_t> writeIndex{0};
			std::atomic<uint64_t> dropped{0};
			FILE* binFile = nullptr;
			FILE* infoFile = nullptr;
			std::thread writer;
		};

		State* createState()
		{
			const char* env = getenv("GEARMULATOR_RT_FULL_CAPTURE");
			if (!env || *env != '1')
				return nullptr;

			auto* s = new State();
			s->records.reset(new Record[g_capacity]);
			memset(s->records.get(), 0, sizeof(Record) * g_capacity);	// fault the pages in now, not on the audio thread

			std::string dir = ".";
			if (const char* home = getenv("HOME"))
				dir = std::string(home) + "/Documents";
			const auto base = dir + "/rtcapture-" + std::to_string(getpid());
			s->binFile = fopen((base + ".bin").c_str(), "wb");
			s->infoFile = fopen((base + ".txt").c_str(), "w");

			s->writer = std::thread([s]
			{
				size_t written = 0;
				while (true)
				{
					std::this_thread::sleep_for(std::chrono::seconds(2));
#ifdef __APPLE__
					task_vm_info_data_t vmInfo{};
					mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
					if (s->infoFile && task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vmInfo), &count) == KERN_SUCCESS)
					{
						fprintf(s->infoFile, "%llu footprint=%lluMB virtual=%lluMB records=%zu dropped=%llu\n", static_cast<unsigned long long>(now()),
							static_cast<unsigned long long>(vmInfo.phys_footprint >> 20), static_cast<unsigned long long>(vmInfo.virtual_size >> 20), s->writeIndex.load(), static_cast<unsigned long long>(s->dropped.load()));
						fflush(s->infoFile);
					}
#endif
					const size_t end = s->writeIndex.load(std::memory_order_acquire);
					if (end > written && s->binFile)
					{
						fwrite(&s->records[written], sizeof(Record), end - written, s->binFile);
						fflush(s->binFile);
						written = end;
					}
				}
			});
			s->writer.detach();
			return s;
		}

		State* state()
		{
			static State* s = createState();
			return s;
		}
	}

	bool isEnabled() noexcept
	{
		return state() != nullptr;
	}

	uint64_t now() noexcept
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	void record(const uint64_t _startNs, const uint64_t _endNs, const size_t _frames, const double _sampleRate) noexcept
	{
		auto* s = state();
		if (!s)
			return;
		const size_t i = s->writeIndex.load(std::memory_order_relaxed);
		if (i >= g_capacity)
		{
			s->dropped.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		auto& r = s->records[i];
		r.startNs = _startNs;
		r.durationNs = static_cast<uint32_t>(_endNs - _startNs);
		r.frames = static_cast<uint32_t>(_frames);
		r.sampleRate = static_cast<uint32_t>(_sampleRate + 0.5);
		r.reserved = 0;
		s->writeIndex.store(i + 1, std::memory_order_release);
	}

	void logInfo(const char* _text) noexcept
	{
		auto* s = state();
		if (!s || !s->infoFile)
			return;
		fprintf(s->infoFile, "%llu %s\n", static_cast<unsigned long long>(now()), _text);
		fflush(s->infoFile);
	}
}
