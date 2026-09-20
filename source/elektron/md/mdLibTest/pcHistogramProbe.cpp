// Temporary, one-off measurement tool: quantifies the "idle-poll fraction" of
// DSP56300 instruction retirements during steady Machinedrum playback.
//
// Requires DSP56K_FORCE_INTERPRETER=ON (single-step interpreter, so every
// retired instruction is observable) AND -DMD_PC_HISTOGRAM_PROBE=1 in the
// compiler flags (a minimal, gated counter hook in dsp56kEmu/dsp.h; see there).
//
// Not part of any product build, not part of ctest. Boots real firmware,
// presses Play, lets the sequencer run past boot/settle, then tallies
// per-PC retirement counts for both DSPs over a measurement window and
// prints the hottest PCs (disassembled) plus their share of total
// instructions retired. A handful of PCs with counts vastly exceeding the
// rest, disassembling to a conditional-branch-to-self on a peripheral
// status bit (e.g. Brclr/Jclr on an HDI08/ESSI/PortC status register),
// are the idle-poll signature the emulator burns real CPU on without
// producing audio.
//
// Usage: mdPcHistogramProbe firmware.bin [settleSeconds=3] [measureSeconds=1]

#include "mdLib/mdhardware.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdpanel.h"
#include "baseLib/filesystem.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <vector>

int main(int _argc, char** _argv)
{
	if(_argc < 2)
	{
		std::cerr << "Usage: mdPcHistogramProbe firmware.bin [settleSeconds=3] [measureSeconds=1]\n";
		return 1;
	}
	if constexpr(dsp56k::g_useJIT)
	{
		std::cerr << "Build with DSP56K_FORCE_INTERPRETER=ON\n";
		return 1;
	}
	const double settleSeconds = _argc > 2 ? std::stod(_argv[2]) : 3.0;
	const double measureSeconds = _argc > 3 ? std::stod(_argv[3]) : 1.0;

	std::vector<uint8_t> rom;
	if(!baseLib::filesystem::readFile(rom, _argv[1])
		|| !md::RomLoader::isRomForModel(rom, md::MachineModel::Machinedrum))
	{
		std::cerr << "Failed to load firmware\n";
		return 1;
	}
	md::Hardware hardware(rom, _argv[1], md::MachineModel::Machinedrum);
	if(!hardware.isValid())
	{
		std::cerr << "Hardware init failed\n";
		return 1;
	}

	std::unordered_map<dsp56k::TWord, uint64_t> histMixer;
	std::unordered_map<dsp56k::TWord, uint64_t> histProducer;
#ifdef MD_PC_HISTOGRAM_PROBE
	histMixer.reserve(4096);
	histProducer.reserve(4096);
#else
	std::cerr << "Build with -DMD_PC_HISTOGRAM_PROBE=1\n";
	return 1;
#endif

	std::array<std::array<float, 256>, 6> samples{};
	synthLib::TAudioOutputs outputs{};
	for(size_t channel = 0; channel < samples.size(); ++channel)
		outputs[channel] = samples[channel].data();

	const auto playPacket = md::panelPacket(md::MachineModel::Machinedrum, md::PanelControl::Play);

	uint64_t frame = 0;
	bool pressedPlay = false;
	bool releasedPlay = false;
	uint64_t playFrame = 0;

	const uint64_t settleFrames = static_cast<uint64_t>(settleSeconds * 44100.0);
	const uint64_t measureFrames = static_cast<uint64_t>(measureSeconds * 44100.0);
	const uint64_t stopFrame = settleFrames + measureFrames;

	while(frame < stopFrame)
	{
		if(!pressedPlay && frame >= settleFrames / 2)
		{
			if(!hardware.trySendPanelEvent(playPacket->row, playPacket->mask))
				return 4;
			pressedPlay = true;
			playFrame = frame;
		}
		if(pressedPlay && !releasedPlay && frame >= playFrame + 128)
		{
			if(!hardware.trySendPanelEvent(playPacket->row, 0))
				return 4;
			releasedPlay = true;
		}

#ifdef MD_PC_HISTOGRAM_PROBE
		const bool measuring = frame >= settleFrames;
		hardware.getDspMixer().dsp().pcHistogram = measuring ? &histMixer : nullptr;
		hardware.getDspProducer().dsp().pcHistogram = measuring ? &histProducer : nullptr;
#endif

		hardware.processAudio(outputs, 256, 0);
		frame += 256;

		if((frame / 44100) != ((frame - 256) / 44100))
			std::cout << "machineSeconds=" << (frame / 44100.0) << std::endl;
	}

	auto report = [](const char* _name, std::unordered_map<dsp56k::TWord, uint64_t>& _hist, dsp56k::DSP& _dsp)
	{
		uint64_t total = 0;
		for(const auto& kv : _hist)
			total += kv.second;

		std::vector<std::pair<dsp56k::TWord, uint64_t>> sorted(_hist.begin(), _hist.end());
		std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

		std::cout << "\n=== " << _name << " === total retired instructions in window: " << total
			<< " (unique PCs: " << sorted.size() << ")\n";

		uint64_t top20Sum = 0;
		for(size_t i = 0; i < sorted.size() && i < 20; ++i)
		{
			const auto pc = sorted[i].first;
			const auto count = sorted[i].second;
			top20Sum += count;
			std::string instruction;
			_dsp.disassembler().disassemble(instruction,
				_dsp.memory().get(dsp56k::MemArea_P, pc), _dsp.memory().get(dsp56k::MemArea_P, pc + 1),
				0, 0, pc);
			std::cout << "  PC=" << std::hex << pc << std::dec << " count=" << count
				<< " (" << (100.0 * static_cast<double>(count) / static_cast<double>(total)) << "%) "
				<< instruction << "\n";
		}
		std::cout << _name << " top20Sum=" << top20Sum << " / total=" << total
			<< " = " << (100.0 * static_cast<double>(top20Sum) / static_cast<double>(total)) << "%\n";
	};

#ifdef MD_PC_HISTOGRAM_PROBE
	report("DSP Mixer (DSP1)", histMixer, hardware.getDspMixer().dsp());
	report("DSP Producer (DSP2)", histProducer, hardware.getDspProducer().dsp());
#endif

	return 0;
}
