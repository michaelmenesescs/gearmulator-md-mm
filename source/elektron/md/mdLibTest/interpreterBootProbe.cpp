#include "mdLib/mdhardware.h"
#include "mdLib/mdromloader.h"
#include "baseLib/filesystem.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

namespace
{
	using Clock = std::chrono::steady_clock;

	void writeLcd(const md::FrontPanel& _panel, const std::string& _path)
	{
		std::ofstream file(_path, std::ios::binary);
		file << "P6\n128 64\n255\n";
		for(uint32_t y = 0; y < 64; ++y)
			for(uint32_t x = 0; x < 128; ++x)
			{
				const unsigned char pixel = _panel.getLcdPixel(x, y) ? 20 : 220;
				for(unsigned channel = 0; channel < 3; ++channel)
					file.put(static_cast<char>(pixel));
			}
	}
}

int main(int _argc, char** _argv)
{
	if(_argc != 3)
	{
		std::cerr << "Usage: mdInterpreterBootProbe firmware.bin output-prefix\n";
		return 1;
	}
	if constexpr(dsp56k::g_useJIT)
	{
		std::cerr << "Build with DSP56K_FORCE_INTERPRETER=ON\n";
		return 1;
	}
	std::vector<uint8_t> rom;
	if(!baseLib::filesystem::readFile(rom, _argv[1])
		|| !md::RomLoader::isRomForModel(rom, md::MachineModel::Machinedrum))
		return 1;
	md::Hardware hardware(rom, _argv[1], md::MachineModel::Machinedrum);
	if(!hardware.isValid())
		return 1;

	std::atomic<uint64_t> frames{0};
	std::atomic<bool> stop{false};
	const auto start = Clock::now();
	std::thread watchdog([&]
	{
		uint64_t previous = 0;
		for(unsigned second = 1; second <= 65; ++second)
		{
			std::this_thread::sleep_until(start + std::chrono::seconds(second));
			if(second % 5 != 0)
				continue;
			const auto current = frames.load();
			const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
			std::cout << "wall=" << elapsed << " frames=" << current
				<< " totalRatio=" << current / (44100.0 * elapsed)
				<< " intervalRatio=" << (current - previous) / (44100.0 * 5.0) << std::endl;
			previous = current;
		}
		stop = true;
		std::this_thread::sleep_for(std::chrono::seconds(2));
		// A stuck advance() cannot join or destruct Hardware. Preserve the last
		// completed progress report and return failure instead of hanging forever.
		std::cerr << "advance() did not return within watchdog grace period\n";
		std::_Exit(2);
	});
	uint64_t nextReport = 0;
	while(!stop)
	{
		hardware.advance(512);
		const auto current = frames.fetch_add(512) + 512;
		if(current < 44100 || current >= nextReport)
		{
			const auto panel = hardware.getFrontPanelSnapshot();
			writeLcd(panel, std::string(_argv[2]) + "-lcd.ppm");
			std::cout << "machineSeconds=" << current / 44100.0
				<< " mixerPC=" << std::hex << hardware.getDspMixer().dsp().getPC().var
				<< " producerPC=" << hardware.getDspProducer().dsp().getPC().var << std::dec
				<< " midiReady=" << hardware.isFirmwareMidiReady()
				<< " panelBytes=" << panel.getByteCount() << std::endl;
			if(current >= 44100)
				for(auto* engine : {&hardware.getDspMixer(), &hardware.getDspProducer()})
				{
					auto& dsp = engine->dsp();
					const auto pc = dsp.getPC().var;
					std::string instruction;
					dsp.disassembler().disassemble(instruction,
						dsp.memory().get(dsp56k::MemArea_P, pc), dsp.memory().get(dsp56k::MemArea_P, pc + 1),
						0, 0, pc);
					std::cout << "mode=" << dsp.getProcessingMode() << " sr=" << std::hex
						<< dsp.getSR().var << " sc=" << unsigned(dsp.regs().sc.var) << std::dec
						<< " cycles=" << dsp.getCycles() << " instruction=" << instruction << std::endl;
				}
			nextReport = current + 44100;
		}
	}
	std::cout << "Completed 65-second interpreter probe\n" << std::flush;
	// Exit without waiting for the watchdog grace period.
	std::_Exit(hardware.isFirmwareMidiReady() ? 0 : 3);
}
