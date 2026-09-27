#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "mdRemotePanelWire.h"

namespace mdJucePlugin::remotePanel
{
	// Adaptive capture cadence for the v3 panel stream (message thread only).
	//
	// Fast while the panel is changing or a finger is down, idle otherwise. "Changing" is
	// fed by cheap signals that arrive before any pixels are read back: a handled touch, a
	// change of LCD or LED contents seen by the pump, or the worker reporting that the last
	// capture differed from the one before it. Idle captures are still taken (the
	// viewer's 750 ms freshness deadline needs them) but identical pixels are neither
	// encoded nor sent, so an unchanging panel still transmits zero PanelChunk bytes.
	//
	// Fast captures are also paced by a mirror of the per-client PanelBudget: a capture is
	// only requested once the link could send the previous keyframe's size in one go. Under
	// sustained change the budget, not the capture rate, bounds keyframes/s; capturing
	// faster than it drains would only make each frame older by the time its last byte left.
	class CapturePolicy
	{
	public:
		static constexpr uint64_t g_fastIntervalUs = 33333;		// ~30 captures/s
		static constexpr uint64_t g_idleIntervalUs = 200000;	// 5/s, well inside the 750 ms deadline
		static constexpr uint64_t g_holdFastUs = 600000;		// decay to idle after this long without activity
		static constexpr uint64_t g_captureLeadUs = 10000;		// request -> published keyframe (render + encode)
		static constexpr int g_fastTimerMs = 5;
		static constexpr int g_idleTimerMs = 20;

		static double wireBytes(const size_t _png)
		{
			return static_cast<double>(_png + 41 * ((_png + 16383) / 16384));
		}

		void activity(const uint64_t _nowUs) { m_lastActivityUs = _nowUs; m_haveActivity = true; }
		void setTouchActive(const bool _active) { m_touchActive = _active; }

		// A keyframe (or several, summed) was published for sending.
		void keyframePublished(const uint64_t _nowUs, const size_t _pngBytes, const size_t _lastPngBytes)
		{
			refill(_nowUs);
			m_tokens -= wireBytes(_pngBytes);
			m_lastWireBytes = wireBytes(_lastPngBytes);
		}

		bool isFast(const uint64_t _nowUs) const
		{
			return m_touchActive || (m_haveActivity && _nowUs - m_lastActivityUs < g_holdFastUs);
		}

		uint64_t intervalUs(const uint64_t _nowUs) const { return isFast(_nowUs) ? g_fastIntervalUs : g_idleIntervalUs; }
		int timerMs(const uint64_t _nowUs) const { return isFast(_nowUs) ? g_fastTimerMs : g_idleTimerMs; }

		// True when a capture should be requested now; records the request time.
		bool due(const uint64_t _nowUs)
		{
			if(m_haveCapture)
			{
				const auto elapsed = _nowUs - m_lastCaptureUs;
				if(elapsed < intervalUs(_nowUs))
					return false;
				// The budget can delay a capture, but never past the idle interval.
				refill(_nowUs);
				if(elapsed < g_idleIntervalUs
					&& m_tokens + g_captureLeadUs * g_panelBudgetBytesPerUs < m_lastWireBytes)
					return false;
			}
			m_lastCaptureUs = _nowUs;
			m_haveCapture = true;
			return true;
		}

	private:
		void refill(const uint64_t _nowUs)
		{
			if(m_haveRefill && _nowUs > m_lastRefillUs)
				m_tokens = std::min(g_panelBudgetBurst, m_tokens + static_cast<double>(_nowUs - m_lastRefillUs) * g_panelBudgetBytesPerUs);
			m_lastRefillUs = _nowUs;
			m_haveRefill = true;
		}

		uint64_t m_lastActivityUs = 0;
		uint64_t m_lastCaptureUs = 0;
		uint64_t m_lastRefillUs = 0;
		double m_tokens = g_panelBudgetBurst;
		double m_lastWireBytes = 0;
		bool m_haveActivity = false;
		bool m_haveCapture = false;
		bool m_haveRefill = false;
		bool m_touchActive = false;
	};
}
