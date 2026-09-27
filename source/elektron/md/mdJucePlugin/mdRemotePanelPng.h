#pragma once

#include <cstdint>
#include <vector>

#include "juce_graphics/juce_graphics.h"

namespace mdJucePlugin::remotePanel
{
	// Panel-stream PNG writer. Produces an ordinary, lossless PNG (any decoder reads it),
	// tuned for per-frame cost rather than size: an opaque capture is written as RGB8 with
	// no per-pixel unpremultiply, rows use the Up filter, and zlib runs at a low level.
	// A capture with any non-opaque pixel falls back to RGBA8 (unpremultiplied), so the
	// decoded pixels always equal what juce::PNGImageFormat would have produced.
	struct PanelPngOptions
	{
		int zlibLevel = 1;
	};

	bool encodePanelPng(const juce::Image& _image, std::vector<uint8_t>& _out, const PanelPngOptions& _options = {});

	// True if both images have identical dimensions and pixel bytes.
	bool samePixels(const juce::Image& _a, const juce::Image& _b);
}
