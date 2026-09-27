#pragma once

#include <cstddef>
#include <cstdint>

namespace synthLib
{
	// Opt-in (GEARMULATOR_RT_FULL_CAPTURE=1) record of every host audio callback: start time,
	// duration, frame count and sample rate, into a fixed preallocated buffer. Unlike the sampled
	// RealtimeInstrumentation report this is unbiased, so true percentiles can be computed offline.
	// A background thread appends new records to <HOME>/Documents/rtcapture-<pid>.bin.
	namespace callbackCapture
	{
		struct Record
		{
			uint64_t startNs;
			uint32_t durationNs;
			uint32_t frames;
			uint32_t sampleRate;
			uint32_t reserved;
		};
		static_assert(sizeof(Record) == 24, "Record layout is read by the offline analysis");

		bool isEnabled() noexcept;
		uint64_t now() noexcept;
		void record(uint64_t _startNs, uint64_t _endNs, size_t _frames, double _sampleRate) noexcept;
		void logInfo(const char* _text) noexcept;

		class Scope final
		{
		public:
			Scope(size_t _frames, double _sampleRate) noexcept
				: m_frames(_frames), m_sampleRate(_sampleRate), m_start(isEnabled() ? now() : 0)
			{
			}
			~Scope()
			{
				if (m_start)
					record(m_start, now(), m_frames, m_sampleRate);
			}
			Scope(const Scope&) = delete;
			Scope& operator=(const Scope&) = delete;

		private:
			size_t m_frames;
			double m_sampleRate;
			uint64_t m_start;
		};
	}
}
