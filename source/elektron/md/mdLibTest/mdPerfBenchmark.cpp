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
	std::string path;
	if(_argc > 1)
		path = _argv[1];
	else if(const auto* const env = std::getenv("GEARMULATOR_MD_FIRMWARE_BIN"))
		path = env;

	if(path.empty())
	{
		std::cerr << "Usage: mdPerfBenchmark [firmware.bin]  (or set GEARMULATOR_MD_FIRMWARE_BIN)\n";
		return 77;
	}

	try
	{
		std::vector<uint8_t> rom;
		require(baseLib::filesystem::readFile(rom, path), "could not read firmware image");
		require(md::RomLoader::isRomForModel(rom, md::MachineModel::Machinedrum),
			"firmware is not a valid Machinedrum image");

		md::Hardware hardware(rom, path, md::MachineModel::Machinedrum);
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

		// Start the sequencer so DSPs are doing real synthesis work, not idle polling.
		std::cout << "Pressing Play...\n";
		tapPlay(hardware, md::MachineModel::Machinedrum);
		// Let a pattern actually start producing triggered voices before measuring.
		advanceFrames(hardware, md::g_samplerate * 2);

		std::cout << "Warming up JIT block cache...\n";
		// A few seconds of steady playback lets the JIT populate its block cache so
		// we are not measuring first-execution compile cost during the sweep.
		advanceFrames(hardware, md::g_samplerate * 3);

		const std::vector<uint32_t> blockSizes{128, 256, 512, 1024};
		// ~4 emulated seconds per block size is enough to average out scheduler/OS
		// jitter while staying fast to run repeatedly.
		constexpr uint32_t sampleTarget = md::g_samplerate * 4;

		std::cout << "\nblockSize,samples,totalWallSec,meanBlockUs,medianBlockUs,p95BlockUs,maxBlockUs,"
			"realtimeBudgetUs,realtimeRatio,perSampleNs\n";
		for(const auto blockSize : blockSizes)
		{
			const auto stats = measureBlockSize(hardware, blockSize, sampleTarget);
			std::cout << "RESULT " << stats.blockSize << ',' << stats.sampleCount << ','
				<< stats.totalWallSeconds << ',' << stats.meanBlockUs << ',' << stats.medianBlockUs << ','
				<< stats.p95BlockUs << ',' << stats.maxBlockUs << ',' << stats.realtimeBudgetUs << ','
				<< stats.realtimeRatio << ',' << stats.perSampleNs << '\n';
		}

		return 0;
	}
	catch(const std::exception& e)
	{
		std::cerr << "mdPerfBenchmark: " << e.what() << '\n';
		return 1;
	}
}
