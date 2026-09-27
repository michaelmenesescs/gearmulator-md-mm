#include "mdRemotePanelPng.h"

#include <array>
#include <cstring>

namespace mdJucePlugin::remotePanel
{
	namespace
	{
		const std::array<uint32_t, 256>& crcTable()
		{
			static const auto table = []
			{
				std::array<uint32_t, 256> t{};
				for(uint32_t n = 0; n < 256; ++n)
				{
					uint32_t c = n;
					for(int k = 0; k < 8; ++k)
						c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
					t[n] = c;
				}
				return t;
			}();
			return table;
		}

		uint32_t crc(uint32_t _crc, const uint8_t* _data, const size_t _size)
		{
			const auto& t = crcTable();
			for(size_t i = 0; i < _size; ++i)
				_crc = t[(_crc ^ _data[i]) & 0xff] ^ (_crc >> 8);
			return _crc;
		}

		void be32(std::vector<uint8_t>& _out, const uint32_t _v)
		{
			for(int i = 3; i >= 0; --i)
				_out.push_back(static_cast<uint8_t>(_v >> (8 * i)));
		}

		void chunk(std::vector<uint8_t>& _out, const char* _type, const uint8_t* _data, const size_t _size)
		{
			be32(_out, static_cast<uint32_t>(_size));
			const auto start = _out.size();
			_out.insert(_out.end(), _type, _type + 4);
			if(_size)
				_out.insert(_out.end(), _data, _data + _size);
			be32(_out, crc(0xffffffffu, _out.data() + start, _size + 4) ^ 0xffffffffu);
		}

		bool isOpaque(const juce::Image::BitmapData& _src, const juce::Image& _image)
		{
			if(!_image.hasAlphaChannel())
				return true;
			if(_image.getFormat() != juce::Image::ARGB)
				return false;
			for(int y = 0; y < _src.height; ++y)
			{
				const auto* p = _src.getLinePointer(y);
				for(int x = 0; x < _src.width; ++x, p += _src.pixelStride)
					if(reinterpret_cast<const juce::PixelARGB*>(p)->getAlpha() != 0xff)
						return false;
			}
			return true;
		}
	}

	bool encodePanelPng(const juce::Image& _image, std::vector<uint8_t>& _out, const PanelPngOptions& _options)
	{
		_out.clear();
		if(_image.isNull())
			return false;

		const juce::Image::BitmapData src(_image, juce::Image::BitmapData::readOnly);
		const auto width = src.width;
		const auto height = src.height;
		const bool opaque = isOpaque(src, _image);
		const size_t channels = opaque ? 3 : 4;
		const size_t rowBytes = static_cast<size_t>(width) * channels;

		// Filtered scanlines: one filter byte (2 = Up) per row. The first row's Up
		// predictor is zero, which is identical to filter None.
		std::vector<uint8_t> prev(rowBytes, 0), cur(rowBytes), line(rowBytes + 1);
		juce::MemoryOutputStream idat(rowBytes * static_cast<size_t>(height) / 8 + 1024);
		{
			juce::GZIPCompressorOutputStream z(idat, _options.zlibLevel);
			for(int y = 0; y < height; ++y)
			{
				const auto* p = src.getLinePointer(y);
				auto* d = cur.data();
				if(_image.getFormat() == juce::Image::ARGB)
				{
					for(int x = 0; x < width; ++x, p += src.pixelStride)
					{
						auto px = *reinterpret_cast<const juce::PixelARGB*>(p);
						if(!opaque)
							px.unpremultiply();
						*d++ = px.getRed(); *d++ = px.getGreen(); *d++ = px.getBlue();
						if(!opaque)
							*d++ = px.getAlpha();
					}
				}
				else if(_image.getFormat() == juce::Image::RGB)
				{
					for(int x = 0; x < width; ++x, p += src.pixelStride)
					{
						const auto* px = reinterpret_cast<const juce::PixelRGB*>(p);
						*d++ = px->getRed(); *d++ = px->getGreen(); *d++ = px->getBlue();
					}
				}
				else
				{
					return false;
				}
				line[0] = 2;
				for(size_t i = 0; i < rowBytes; ++i)
					line[i + 1] = static_cast<uint8_t>(cur[i] - prev[i]);
				if(!z.write(line.data(), line.size()))
					return false;
				std::swap(prev, cur);
			}
			z.flush();
		}

		static constexpr uint8_t signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
		_out.reserve(idat.getDataSize() + 64);
		_out.insert(_out.end(), signature, signature + sizeof(signature));

		std::vector<uint8_t> ihdr;
		be32(ihdr, static_cast<uint32_t>(width));
		be32(ihdr, static_cast<uint32_t>(height));
		ihdr.push_back(8);						// bit depth
		ihdr.push_back(opaque ? 2 : 6);			// colour type: RGB / RGBA
		ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);	// deflate, adaptive filtering, no interlace
		chunk(_out, "IHDR", ihdr.data(), ihdr.size());
		chunk(_out, "IDAT", static_cast<const uint8_t*>(idat.getData()), idat.getDataSize());
		chunk(_out, "IEND", nullptr, 0);
		return true;
	}

	bool samePixels(const juce::Image& _a, const juce::Image& _b)
	{
		if(_a.isNull() || _b.isNull() || _a.getFormat() != _b.getFormat()
			|| _a.getWidth() != _b.getWidth() || _a.getHeight() != _b.getHeight())
			return false;
		const juce::Image::BitmapData a(_a, juce::Image::BitmapData::readOnly);
		const juce::Image::BitmapData b(_b, juce::Image::BitmapData::readOnly);
		const auto bytes = static_cast<size_t>(a.width) * static_cast<size_t>(a.pixelStride);
		for(int y = 0; y < a.height; ++y)
			if(std::memcmp(a.getLinePointer(y), b.getLinePointer(y), bytes) != 0)
				return false;
		return true;
	}
}
