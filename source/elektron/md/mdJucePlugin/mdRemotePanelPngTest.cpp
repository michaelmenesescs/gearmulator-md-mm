// Remote panel v3 per-frame cost: correctness of the panel PNG writer, the pixel dedup and
// the adaptive capture policy, plus a micro-benchmark of the worker's per-capture work
// before (juce::PNGImageFormat on every capture) and after (pixel compare, then the panel
// writer only for changed pixels).
//
// Usage: mdRemotePanelPngTest [captured keyframe .png ...]
// Without arguments a synthetic panel-like raster is used. Real keyframes can be saved by
// any passive v3 viewer; they are panel pixels, never firmware.

#include "mdRemotePanelPng.h"
#include "mdRemotePanelCapturePolicy.h"
#include "mdRemotePanelWire.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace mdJucePlugin::remotePanel;

namespace
{
	int g_failures = 0;

	void check(const bool _ok, const std::string& _what)
	{
		std::printf("%s %s\n", _ok ? "PASS" : "FAIL", _what.c_str());
		if(!_ok)
			++g_failures;
	}

	struct Stats
	{
		double median = 0, p95 = 0, mean = 0;
	};

	Stats stats(std::vector<double> _v)
	{
		Stats s;
		if(_v.empty())
			return s;
		std::sort(_v.begin(), _v.end());
		s.median = _v[_v.size() / 2];
		s.p95 = _v[std::min(_v.size() - 1, _v.size() * 95 / 100)];
		for(const auto v : _v)
			s.mean += v;
		s.mean /= static_cast<double>(_v.size());
		return s;
	}

	Stats timeMs(const int _iterations, const std::function<void()>& _f)
	{
		std::vector<double> ms;
		_f();	// warm-up
		for(int i = 0; i < _iterations; ++i)
		{
			const auto t0 = std::chrono::steady_clock::now();
			_f();
			ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
		}
		return stats(ms);
	}

	std::vector<uint8_t> juceEncode(const juce::Image& _image)
	{
		juce::MemoryOutputStream out;
		juce::PNGImageFormat().writeImageToStream(_image, out);
		const auto* b = static_cast<const uint8_t*>(out.getData());
		return {b, b + out.getDataSize()};
	}

	juce::Image decode(const std::vector<uint8_t>& _png)
	{
		juce::MemoryInputStream in(_png.data(), _png.size(), false);
		return juce::PNGImageFormat().decodeImage(in);
	}

	// Compares straight-alpha colours; fully transparent pixels compare as equal.
	bool sameColours(const juce::Image& _a, const juce::Image& _b)
	{
		if(_a.getWidth() != _b.getWidth() || _a.getHeight() != _b.getHeight())
			return false;
		for(int y = 0; y < _a.getHeight(); ++y)
			for(int x = 0; x < _a.getWidth(); ++x)
			{
				const auto a = _a.getPixelAt(x, y), b = _b.getPixelAt(x, y);
				if(a.getAlpha() != b.getAlpha())
					return false;
				if(a.getAlpha() && a.getARGB() != b.getARGB())
					return false;
			}
		return true;
	}

	juce::Image syntheticPanel(const int _w, const int _h, const bool _opaque)
	{
		juce::Image img(juce::Image::ARGB, _w, _h, true);
		juce::Graphics g(img);
		if(_opaque)
			g.fillAll(juce::Colour(0xff2b2d30));
		g.setGradientFill(juce::ColourGradient(juce::Colour(0xff40444a), 0, 0, juce::Colour(0xff1c1d20), 0, static_cast<float>(_h), false));
		g.fillRect(0, 0, _w, _h / 3);
		std::mt19937 rng(7);
		for(int i = 0; i < 120; ++i)
		{
			const auto x = static_cast<float>(rng() % _w), y = static_cast<float>(rng() % _h);
			g.setColour(juce::Colour(0xff000000 | (rng() & 0xffffff)).withAlpha(_opaque ? 1.0f : 0.6f));
			g.fillEllipse(x, y, 22, 22);
			g.setColour(juce::Colours::white);
			g.drawText("FUNC", juce::Rectangle<float>(x, y + 24, 40, 12), juce::Justification::centred);
		}
		return img;
	}

	void testCodec(const juce::Image& _src, const std::string& _name)
	{
		std::vector<uint8_t> png;
		check(encodePanelPng(_src, png), _name + ": panel writer encodes");
		const auto decoded = decode(png);
		check(!decoded.isNull(), _name + ": juce decodes panel PNG");
		const auto reference = decode(juceEncode(_src));
		check(sameColours(decoded, reference), _name + ": decoded pixels identical to juce::PNGImageFormat round trip");
	}

	void testDedup(const juce::Image& _src)
	{
		auto copy = _src.createCopy();
		check(samePixels(_src, copy), "dedup: identical copy compares equal");
		copy.setPixelAt(copy.getWidth() - 1, copy.getHeight() - 1, juce::Colour(0xff123456));
		check(!samePixels(_src, copy), "dedup: one changed pixel (last pixel) compares different");
		copy = _src.createCopy();
		copy.setPixelAt(0, 0, _src.getPixelAt(0, 0).withRotatedHue(0.5f).withAlpha(1.0f).withMultipliedBrightness(0.5f));
		check(!samePixels(_src, copy), "dedup: one changed pixel (first pixel) compares different");
		check(!samePixels(_src, _src.rescaled(_src.getWidth() / 2, _src.getHeight() / 2)), "dedup: different size compares different");
	}

	void testPolicy()
	{
		CapturePolicy p;
		uint64_t t = 1000000;
		int captures = 0;
		// 30 s idle on the idle timer: ~5 captures/s and never fast.
		bool everFast = false;
		for(uint64_t end = t + 30000000; t < end; t += CapturePolicy::g_idleTimerMs * 1000)
		{
			everFast |= p.isFast(t);
			captures += p.due(t);
		}
		check(!everFast && captures >= 145 && captures <= 151, "policy: idle 30 s -> " + std::to_string(captures) + " captures, never fast");

		// Activity: next fast-timer tick captures immediately, then ~30/s.
		p.activity(t);
		check(p.isFast(t) && p.timerMs(t) == CapturePolicy::g_fastTimerMs, "policy: activity switches to fast");
		check(p.due(t), "policy: first tick after activity captures immediately");
		captures = 1;
		const auto start = t;
		for(t += 5000; t < start + 1000000; t += CapturePolicy::g_fastTimerMs * 1000)
		{
			if(t - start < 500000)
				p.activity(t);	// keep changing for 0.5 s
			captures += p.due(t);
		}
		// 0.5 s changing + 0.6 s hold window -> fast for the whole first second.
		check(captures >= 27 && captures <= 30, "policy: changing panel -> " + std::to_string(captures) + " captures in 1 s (fast)");
		// Decay: 0.6 s after the last activity the policy is idle again.
		check(!p.isFast(start + 500000 + CapturePolicy::g_holdFastUs), "policy: decays to idle 600 ms after last activity");

		// A held finger keeps fast cadence without further activity.
		p.setTouchActive(true);
		check(p.isFast(t + 10000000), "policy: touch active keeps fast cadence");
		p.setTouchActive(false);
		check(!p.isFast(t + 10000000), "policy: touch released and no activity -> idle");

		// Budget pacing: sustained change with ~81 kB keyframes published right after each
		// capture settles at the budget's keyframe rate, never exceeds the idle gap.
		{
			CapturePolicy q;
			const size_t bytes = 81000;
			int n = 0;
			uint64_t last = 0, maxGap = 0;
			for(uint64_t u = 1000000; u < 11000000; u += CapturePolicy::g_fastTimerMs * 1000)
			{
				q.activity(u);
				if(!q.due(u))
					continue;
				if(last)
					maxGap = std::max(maxGap, u - last);
				last = u;
				++n;
				q.keyframePublished(u + CapturePolicy::g_captureLeadUs, bytes, bytes);
			}
			const double budgetRate = 10.0 * 1000000.0 * g_panelBudgetBytesPerUs / CapturePolicy::wireBytes(bytes);
			check(n <= budgetRate + 4 && n >= budgetRate * 0.9 && maxGap < CapturePolicy::g_idleIntervalUs,
				"policy: budget-paced sustained change -> " + std::to_string(n) + " captures in 10 s (budget allows "
				+ std::to_string(static_cast<int>(budgetRate)) + "), max gap " + std::to_string(maxGap / 1000) + " ms");
		}

		// Every idle interval stays inside the viewer's 750 ms freshness deadline.
		check(CapturePolicy::g_idleIntervalUs + CapturePolicy::g_idleTimerMs * 1000 < 750000, "policy: idle interval + timer slack < 750 ms freshness deadline");
	}

	// Idle honesty: with a viewer attached and nothing changing for 30 s, the worker's
	// dedup must produce zero PanelChunk bytes.
	void testIdleBytes(const juce::Image& _src)
	{
		CapturePolicy p;
		juce::Image last;
		size_t bytes = 0;
		int encodes = 0, captures = 0;
		// The first keyframe after subscribe is expected; count from after it.
		last = _src.createCopy();
		for(uint64_t t = 0; t < 30000000; t += CapturePolicy::g_idleTimerMs * 1000)
		{
			if(!p.due(t))
				continue;
			++captures;
			const auto capture = _src.createCopy();
			if(samePixels(capture, last))
				continue;
			std::vector<uint8_t> png;
			encodePanelPng(capture, png);
			++encodes;
			bytes += png.size();
			last = capture;
		}
		check(bytes == 0 && encodes == 0, "idle 30 s: " + std::to_string(captures) + " captures, " + std::to_string(encodes)
			+ " encodes, " + std::to_string(bytes) + " PanelChunk bytes");
	}

	// Time for one keyframe of _bytes to clear the per-client PanelBudget, starting from a
	// quiet link (full burst credit), sending 16384-byte chunks + 41-byte headers.
	double budgetTransferMs(const size_t _bytes)
	{
		PanelBudget b;
		uint64_t t = 1;
		b.consume(t, 0);
		t += 1000000;	// quiet: credit refilled to cap
		size_t off = 0;
		const auto start = t;
		while(off < _bytes)
		{
			const auto n = std::min<size_t>(16384, _bytes - off);
			if(b.consume(t, n + 41))
				off += n;
			else
				t += 100;
		}
		return static_cast<double>(t - start) / 1000.0;
	}

	// Host-side latency MODEL, not an end-to-end measurement: it composes the encode times
	// measured above with the real capture cadence rules and the real token bucket. It
	// excludes Rml render/readback (identical before/after, needs the live GPU context),
	// Wi-Fi transit and iPad decode/display. 0.1 ms steps.
	struct PathModel
	{
		bool adaptive;			// false: 20 ms timer, >= 66.667 ms between capture requests
		double encodeMs;		// worker encode per changed capture
		double workerPollMs;	// before: worker slept 5 ms between polls; after: woken directly
		size_t bytes;			// keyframe size
		double burst;			// token bucket cap
	};

	struct Budget
	{
		double tokens = 16384, burst;
		explicit Budget(const double _burst) : burst(_burst) {}
		void tick(const double _us) { tokens = std::min(burst, tokens + _us * g_panelBudgetBytesPerUs); }
	};

	// Single change after >= 2 s quiet. _touch: the change is a handled touch (after: the fast
	// timer starts immediately); otherwise a firmware LED/LCD change seen at the next timer tick.
	Stats modelSingleChange(const PathModel& _m, const bool _touch, const int _runs, Stats* _captureWait = nullptr)
	{
		std::mt19937 rng(11);
		std::vector<double> out, wait;
		for(int r = 0; r < _runs; ++r)
		{
			const double step = 0.1;
			// Random phase of the timer and of the last idle capture relative to the change at t=0.
			const double timerPhase = std::uniform_real_distribution<double>(0, 20)(rng);
			double t = 0;
			double captureAt = -1;
			if(!_m.adaptive)
			{
				const double lastReq = timerPhase - 20.0 * std::uniform_int_distribution<int>(1, 4)(rng);	// idle cadence: every 4th 20 ms tick
				for(double tick = timerPhase; captureAt < 0; tick += 20)
					if(tick - lastReq >= 66.667) captureAt = tick;
			}
			else
			{
				CapturePolicy p;
				const double lastReq = timerPhase - 20.0 * std::uniform_int_distribution<int>(1, 10)(rng);	// idle cadence: every 10th 20 ms tick
				p.due(static_cast<uint64_t>((lastReq + 10000) * 1000));
				double tick = _touch ? CapturePolicy::g_fastTimerMs : timerPhase;
				for(; captureAt < 0; tick += p.timerMs(static_cast<uint64_t>((tick + 10000) * 1000)))
				{
					const auto us = static_cast<uint64_t>((tick + 10000) * 1000);
					p.activity(us);
					if(p.due(us)) captureAt = tick;
				}
			}
			wait.push_back(captureAt);
			t = captureAt + std::uniform_real_distribution<double>(0, _m.workerPollMs)(rng) + _m.encodeMs;
			Budget b(_m.burst);
			b.tokens = _m.burst;	// quiet link
			size_t off = 0;
			while(off < _m.bytes)
			{
				const auto n = std::min<size_t>(16384, _m.bytes - off);
				if(b.tokens >= static_cast<double>(n + 41)) { b.tokens -= static_cast<double>(n + 41); off += n; }
				else { b.tick(step * 1000); t += step; }
			}
			out.push_back(t);
		}
		if(_captureWait)
			*_captureWait = stats(wait);
		return stats(out);
	}

	// Sustained change (every capture differs), 20 s. Returns delivered keyframes/s and the
	// age of each delivered keyframe (change captured -> last byte released).
	std::pair<double, Stats> modelSustained(const PathModel& _m)
	{
		const double step = 0.1, end = 20000;
		Budget b(_m.burst);
		b.tokens = _m.burst;	// viewer connected a while ago: link quiet, credit full
		CapturePolicy p;
		double lastReq = -1e9, nextTick = 0, encodeDone = -1, pendingCapture = -1;
		double newestReady = -1, sendingCapture = -1;
		size_t sendingOff = 0;
		bool sending = false;
		int delivered = 0;
		std::vector<double> age;
		for(double t = 0; t < end; t += step)
		{
			b.tick(step * 1000);
			if(t >= nextTick)
			{
				const auto us = static_cast<uint64_t>((t + 10000) * 1000);
				bool due;
				if(_m.adaptive) { p.activity(us); due = p.due(us); nextTick = t + p.timerMs(us); }
				else { due = t - lastReq >= 66.667; if(due) lastReq = t; nextTick = t + 20; }
				if(due && encodeDone < 0) { pendingCapture = t; encodeDone = t + _m.workerPollMs / 2 + _m.encodeMs; }
			}
			if(encodeDone >= 0 && t >= encodeDone)
			{
				newestReady = pendingCapture;
				encodeDone = -1;
				if(_m.adaptive)
					p.keyframePublished(static_cast<uint64_t>((t + 10000) * 1000), _m.bytes, _m.bytes);
			}
			if(!sending && newestReady >= 0 && newestReady != sendingCapture) { sending = true; sendingCapture = newestReady; sendingOff = 0; }
			if(sending)
			{
				const auto n = std::min<size_t>(16384, _m.bytes - sendingOff);
				if(b.tokens >= static_cast<double>(n + 41))
				{
					b.tokens -= static_cast<double>(n + 41);
					sendingOff += n;
					if(sendingOff == _m.bytes) { sending = false; ++delivered; if(t > 2000) age.push_back(t - sendingCapture); }
				}
			}
		}
		return {delivered / (end / 1000.0), stats(age)};
	}

	void model(const double _oldEncodeMs, const size_t _oldBytes, const double _newEncodeMs, const size_t _newBytes)
	{
		const PathModel before{false, _oldEncodeMs, 5.0, _oldBytes, 32768};
		const PathModel after{true, _newEncodeMs, 0.0, _newBytes, g_panelBudgetBurst};
		std::printf("\nMODEL host-side change -> last PanelChunk byte released (ms median / p95 / mean)\n");
		std::printf("  excludes render/readback, Wi-Fi, iPad decode; uses encode medians measured above\n");
		for(const bool touch : {true, false})
		{
			Stats wa, wc;
			const auto a = modelSingleChange(before, touch, 2000, &wa), c = modelSingleChange(after, touch, 2000, &wc);
			const char* what = touch ? "touch (pressed state)" : "firmware LED/LCD";
			std::printf("  single %-22s before %6.1f / %6.1f / %6.1f   after %6.1f / %6.1f / %6.1f\n",
				what, a.median, a.p95, a.mean, c.median, c.p95, c.mean);
			std::printf("    of which change -> capture request      before %6.1f / %6.1f / %6.1f   after %6.1f / %6.1f / %6.1f\n",
				wa.median, wa.p95, wa.mean, wc.median, wc.p95, wc.mean);
		}
		const auto sb = modelSustained(before), sa = modelSustained(after);
		std::printf("  sustained change: before %.1f keyframes/s, age %6.1f / %6.1f / %6.1f;  after %.1f keyframes/s, age %6.1f / %6.1f / %6.1f\n",
			sb.first, sb.second.median, sb.second.p95, sb.second.mean, sa.first, sa.second.median, sa.second.p95, sa.second.mean);
	}

	void bench(const juce::Image& _src, const std::string& _name)
	{
		const int n = 60;
		std::vector<uint8_t> png;
		const auto oldBytes = juceEncode(_src).size();
		encodePanelPng(_src, png);
		const auto newBytes = png.size();
		std::vector<uint8_t> png3;
		encodePanelPng(_src, png3, {3});

		const auto copy = _src.createCopy();
		const auto tCopy = timeMs(n, [&] { (void)_src.createCopy(); });
		const auto tOld = timeMs(n, [&] { (void)juceEncode(_src); });
		const auto tNew = timeMs(n, [&] { encodePanelPng(_src, png); });
		const auto tNew3 = timeMs(n, [&] { encodePanelPng(_src, png3, {3}); });
		const auto tCmp = timeMs(n, [&] { (void)samePixels(_src, copy); });

		std::printf("\nBENCH %s  %dx%d, %d iterations, ms median / p95 / mean\n", _name.c_str(), _src.getWidth(), _src.getHeight(), n);
		std::printf("  callback createCopy                 %7.3f / %7.3f / %7.3f\n", tCopy.median, tCopy.p95, tCopy.mean);
		std::printf("  BEFORE juce::PNGImageFormat (lvl 6)  %7.3f / %7.3f / %7.3f   %zu bytes\n", tOld.median, tOld.p95, tOld.mean, oldBytes);
		std::printf("  AFTER  panel PNG zlib 3, Up (shipped) %7.3f / %7.3f / %7.3f   %zu bytes\n", tNew3.median, tNew3.p95, tNew3.mean, png3.size());
		std::printf("  (alt)  panel PNG zlib 1, Up filter   %7.3f / %7.3f / %7.3f   %zu bytes\n", tNew.median, tNew.p95, tNew.mean, newBytes);
		std::printf("  AFTER  unchanged-frame pixel compare %7.3f / %7.3f / %7.3f   0 bytes (encode skipped)\n", tCmp.median, tCmp.p95, tCmp.mean);
		std::printf("  worker cost per CHANGED capture: before %.3f ms, after %.3f ms (compare + encode)\n", tOld.median, tCmp.median + tNew3.median);
		std::printf("  worker cost per UNCHANGED capture: before %.3f ms, after %.3f ms\n", tOld.median, tCmp.median);
		std::printf("  PanelBudget transfer of one keyframe from quiet link (current burst cap %.0f B): %.1f ms (%zu B, zlib 1), %.1f ms (%zu B, zlib 3)\n",
			g_panelBudgetBurst, budgetTransferMs(newBytes), newBytes, budgetTransferMs(png3.size()), png3.size());
		model(tOld.median, oldBytes, tNew3.median + tCmp.median, png3.size());
	}
}

int main(int _argc, char** _argv)
{
	juce::ScopedJuceInitialiser_GUI juce;

	std::vector<std::pair<std::string, juce::Image>> images;
	for(int i = 1; i < _argc; ++i)
	{
		const juce::File f(juce::File::getCurrentWorkingDirectory().getChildFile(_argv[i]));
		auto img = juce::ImageFileFormat::loadFrom(f);
		if(img.isNull())
		{
			std::printf("FAIL cannot load %s\n", _argv[i]);
			return 1;
		}
		// Screenshots arrive as ARGB; benchmark exactly that format.
		images.emplace_back(f.getFileName().toStdString(), img.convertedToFormat(juce::Image::ARGB));
	}
	if(images.empty())
		images.emplace_back("synthetic-1032x535", syntheticPanel(1032, 535, true));

	for(const auto& [name, img] : images)
		testCodec(img, name);
	testCodec(syntheticPanel(320, 200, false), "synthetic translucent (RGBA fallback)");
	testCodec(syntheticPanel(1032, 535, true).convertedToFormat(juce::Image::RGB), "synthetic RGB-format image");
	testDedup(images.front().second);
	testPolicy();
	testIdleBytes(images.front().second);

	for(const auto& [name, img] : images)
		bench(img, name);

	std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
	return g_failures ? 1 : 0;
}
