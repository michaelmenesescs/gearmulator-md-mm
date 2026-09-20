// Standalone wall-clock performance benchmark for the Machinedrum emulation core.
//
// This is a manual measurement tool (EXCLUDE_FROM_ALL, not part of ctest). It
// boots real firmware, starts the sequencer playing a pattern, then sweeps a
// set of block sizes measuring hardware.advance() wall-clock cost per block.
//
// Usage:
//   mdPerfBenchmark [firmware.bin]
// Firmware path defaults to $GEARMULATOR_MD_FIRMWARE_BIN if omitted.
//
// Output is CSV-ish lines prefixed with "RESULT" for easy grep/parsing, plus
// human-readable summary lines.

#include "mdLib/mdhardware.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdtypes.h"

#include "baseLib/filesystem.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using Clock = std::chrono::steady_clock;

	void require(const bool _condition, const char* const _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	// Advance in fixed sub-blocks so boot-phase progress reporting stays granular,
	// regardless of the block size under test later.
	void advanceFrames(md::Hardware& _hardware, uint32_t _frames, const uint32_t _subBlock = 256)
	{
		while(_frames)
		{
			const auto n = std::min(_frames, _subBlock);
			_hardware.advance(n);
			_frames -= n;
		}
	}

	void tapPlay(md::Hardware& _hardware, md::MachineModel _model)
	{
		md::PanelRowState rows;
		const auto packet = md::panelPacket(_model, md::PanelControl::Play);
		require(packet.has_value(), "PanelControl::Play has no mapped packet for this model");
		const auto down = rows.press(*packet);
		require(_hardware.trySendPanelEvent(down.row, down.mask), "Play press rejected");
		advanceFrames(_hardware, md::g_samplerate / 8);
		const auto up = rows.release(*packet);
		require(_hardware.trySendPanelEvent(up.row, up.mask), "Play release rejected");
		advanceFrames(_hardware, md::g_samplerate / 8);
	}

	struct BlockStats
	{
		uint32_t blockSize = 0;
		size_t   sampleCount = 0;
		double   totalWallSeconds = 0.0;
		double   meanBlockUs = 0.0;
		double   medianBlockUs = 0.0;
		double   p95BlockUs = 0.0;
		double   maxBlockUs = 0.0;
		double   realtimeBudgetUs = 0.0;
		double   realtimeRatio = 0.0;     // meanBlockUs / realtimeBudgetUs
		double   perSampleNs = 0.0;       // totalWallSeconds / totalSamples * 1e9
	};

	BlockStats measureBlockSize(md::Hardware& _hardware, const uint32_t _blockSize,
		const uint32_t _sampleTarget)
	{
		std::vector<double> blockUs;
		blockUs.reserve(_sampleTarget / _blockSize + 1);

		uint64_t samplesDone = 0;
		while(samplesDone < _sampleTarget)
		{
			const auto t0 = Clock::now();
			_hardware.advance(_blockSize);
			const auto t1 = Clock::now();
			blockUs.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
			samplesDone += _blockSize;
		}

		BlockStats stats;
		stats.blockSize = _blockSize;
		stats.sampleCount = samplesDone;
		stats.totalWallSeconds = std::accumulate(blockUs.begin(), blockUs.end(), 0.0) / 1'000'000.0;
		stats.meanBlockUs = (stats.totalWallSeconds * 1'000'000.0) / static_cast<double>(blockUs.size());

		std::vector<double> sorted = blockUs;
		std::sort(sorted.begin(), sorted.end());
		stats.medianBlockUs = sorted[sorted.size() / 2];
		stats.p95BlockUs = sorted[static_cast<size_t>(sorted.size() * 0.95)];
		stats.maxBlockUs = sorted.back();

		stats.realtimeBudgetUs = (static_cast<double>(_blockSize) / md::g_samplerate) * 1'000'000.0;
		stats.realtimeRatio = stats.meanBlockUs / stats.realtimeBudgetUs;
		stats.perSampleNs = (stats.totalWallSeconds * 1'000'000'000.0) / static_cast<double>(stats.sampleCount);

		return stats;
	}
}

int main(int _argc, char** _argv)
{
	try
	{
		std::string path;
		std::string modelName = "MD";
		uint32_t blockSize = 256;
		uint32_t seconds = 4;
		const auto positive = [](const std::string& _value, const uint32_t _maximum)
		{
			require(!_value.empty() && _value.find_first_not_of("0123456789") == std::string::npos,
				"block and seconds must be positive integers");
			const auto value = std::stoull(_value);
			require(value > 0 && value <= _maximum, "block or seconds is out of range");
			return static_cast<uint32_t>(value);
		};
		for(int i = 1; i < _argc; ++i)
		{
			const std::string arg = _argv[i];
			if(arg == "--model" || arg == "--block" || arg == "--seconds")
			{
				require(i + 1 < _argc, "option requires a value");
				const std::string value = _argv[++i];
				if(arg == "--model")
				{
					require(value == "MD" || value == "MM", "model must be MD or MM");
					modelName = value;
				}
				else if(arg == "--block")
					blockSize = positive(value, 0xffffffffu);
				else
					seconds = positive(value, 0xffffffffu / md::g_samplerate);
			}
			else
			{
				require(arg.compare(0, 1, "-") != 0, "unknown option");
				require(path.empty(), "only one firmware path may be supplied");
				path = arg;
			}
		}
		const auto model = modelName == "MD" ? md::MachineModel::Machinedrum : md::MachineModel::Monomachine;
		const auto* envName = modelName == "MD" ? "GEARMULATOR_MD_FIRMWARE_BIN" : "GEARMULATOR_MM_FIRMWARE_BIN";
		if(path.empty())
			if(const auto* env = std::getenv(envName))
				path = env;
		if(path.empty())
		{
			std::cerr << "Usage: mdPerfBenchmark [firmware.bin] [--model MD|MM] [--block N] [--seconds N]\n"
				<< "Defaults: MD, 256 frames, 4 seconds; firmware from " << envName << '\n';
			return 77;
		}

		std::vector<uint8_t> rom;
		require(baseLib::filesystem::readFile(rom, path), "could not read firmware image");
		require(md::RomLoader::isRomForModel(rom, model),
			"firmware is not valid for the selected model");

		md::Hardware hardware(rom, path, model);
		require(hardware.isValid(), "hardware construction failed");

		std::cout << "Booting firmware...\n";
		const auto bootStart = Clock::now();

		// Boot until firmware MIDI-ready or a generous timeout (emulated time).
		constexpr uint32_t maxBootFrames = md::g_samplerate * 30;
		uint32_t bootedFrames = 0;
		while(bootedFrames < maxBootFrames && !hardware.isFirmwareMidiReady())
		{
			hardware.advance(256);
			bootedFrames += 256;
		}
		require(hardware.isFirmwareMidiReady(), "firmware did not become MIDI-ready within 30s emulated");

		const auto bootWall = std::chrono::duration<double>(Clock::now() - bootStart).count();
		std::cout << "Booted after " << (bootedFrames / static_cast<double>(md::g_samplerate))
			<< "s emulated, " << bootWall << "s wall (boot ratio "
			<< (bootedFrames / static_cast<double>(md::g_samplerate)) / bootWall << "x realtime)\n";

		// Request playback; representative audible voice load is not established by this probe.
		std::cout << "Pressing Play...\n";
		tapPlay(hardware, model);
		// Preserve the same post-Play settling interval for every invocation.
		advanceFrames(hardware, md::g_samplerate * 2);

		std::cout << "Warming up JIT block cache...\n";
		// A few seconds of steady playback lets the JIT populate its block cache so
		// we are not measuring initial cache population.
		advanceFrames(hardware, md::g_samplerate * 3);

		const uint32_t sampleTarget = md::g_samplerate * seconds;
		std::cout << "\nmodel,blockSize,samples,totalWallSec,meanBlockUs,medianBlockUs,p95BlockUs,maxBlockUs,"
			"realtimeBudgetUs,realtimeRatio,perSampleNs\n";
		std::cout << "MEASURE_BEGIN model=" << modelName << " block=" << blockSize
			<< " seconds=" << seconds << std::endl;
		const auto stats = measureBlockSize(hardware, blockSize, sampleTarget);
		std::cout << "RESULT " << modelName << ',' << stats.blockSize << ',' << stats.sampleCount << ','
			<< stats.totalWallSeconds << ',' << stats.meanBlockUs << ',' << stats.medianBlockUs << ','
			<< stats.p95BlockUs << ',' << stats.maxBlockUs << ',' << stats.realtimeBudgetUs << ','
			<< stats.realtimeRatio << ',' << stats.perSampleNs << '\n';

		return 0;
	}
	catch(const std::exception& e)
	{
		std::cerr << "mdPerfBenchmark: " << e.what() << '\n';
		return 1;
	}
}
