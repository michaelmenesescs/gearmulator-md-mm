// One-off, throwaway measurement: isolates the JIT wall-clock cost of a fixed-count
// DO-loop whose body is only NOPs (the pattern found in the Machinedrum Producer DSP's
// idle time by pcHistogramProbe.cpp: `do #$32,>addr` / nop / nop).
//
// Not part of any product build, not part of ctest (EXCLUDE_FROM_ALL). Needs no
// firmware ROM: it builds the loop directly with the DSP56300 assembler and drives
// it purely through the normal exec path (dsp.execUntilCycles), i.e. exactly the JIT
// (or interpreter, depending on how this binary was built) that production uses.
//
// Method: build two DSP instances with identical programs except for the DO loop's
// iteration count (50, matching the real firmware, vs 1). Both loop back on
// themselves indefinitely (`jmp` to the DO instruction), so the JIT compiles the
// same three blocks once and re-enters them purely in native code afterwards - the
// closest a synthetic benchmark can get to how the real loop executes in situ.
// Measure wall-clock time to execute a large, equal number of DO-loop invocations
// for both, after a warm-up pass so JIT compilation cost is excluded. The
// per-invocation delta (50-iteration cost minus 1-iteration cost), divided by 49,
// is the marginal native wall-clock cost of one extra NOP-pair loop iteration.
//
// Usage: mdNopLoopMicrobench [invocations=2000000]

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"
#include "dsp56kEmu/assembler.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace dsp56k;

namespace
{
	using Clock = std::chrono::steady_clock;

	// Fixed layout for both variants. `do #$n,>addr` with an absolute (extended)
	// loop-end address is a 2-word instruction (opcode + extension word), matching
	// the real firmware's `do #<$32,>$100097` (PC=100093, extension at PC=100094,
	// body at PC=100095/100096):
	//   0x100: do #$<loopCount>,>$104   (2 words: 0x100, 0x101)
	//   0x102: nop
	//   0x103: nop
	//   0x104: jmp $100
	constexpr TWord g_start = 0x100;
	constexpr TWord g_bodyStart = 0x102;
	constexpr TWord g_loopEnd = 0x104;

	struct Harness
	{
		DefaultMemoryValidator validator;
		Peripherals56362 peripheralsX;
		Peripherals56367 peripheralsY;
		Memory mem;
		DSP dsp;

		explicit Harness(uint32_t _loopCount)
			: mem(validator, 0x080000, 0x800000, 0x200000)
			, dsp(mem, &peripheralsX, &peripheralsY)
		{
			Assembler assembler;

			char doText[64];
			std::snprintf(doText, sizeof(doText), "do #$%x,>$%x", _loopCount, g_loopEnd);

			write(assembler, doText, g_start);
			write(assembler, "nop", g_bodyStart);
			write(assembler, "nop", g_bodyStart + 1);

			char jmpText[64];
			std::snprintf(jmpText, sizeof(jmpText), "jmp $%x", g_start);
			write(assembler, jmpText, g_loopEnd);

			dsp.setPC(g_start);
		}

		void write(Assembler& _assembler, const char* _text, TWord _pc)
		{
			const auto result = _assembler.assemble(_text);
			if(!result.success())
				throw std::runtime_error(std::string("assembly failed for: ") + _text);
			dsp.memWriteP(_pc, result.word[0]);
			if(result.wordCount > 1)
				dsp.memWriteP(_pc + 1, result.word[1]);
		}
	};

	// Estimates the cycle cost of one DO-loop invocation (start -> jmp back to
	// start). NOTE: in a g_useJIT=true build, DSP::execOp() only updates m_cycles
	// `if constexpr(!g_useJIT)` - cycle bookkeeping in that build is exclusively
	// done by the JIT-emitted increaseCycleCount() code inside compiled blocks (see
	// jitblock.cpp), so calibration must go through execUntilCycles (JIT), not
	// execInterpreter, or m_cycles never advances at all.
	//
	// This only needs to be a good-enough estimate to size the timed run's cycle
	// target: a branch to a compile-time-known address (our `jmp` back to the DO
	// instruction) chains directly into the next block without returning to C++,
	// so execUntilCycles can land anywhere at or past a requested target, never
	// exactly on an invocation boundary. The actual timed measurement normalizes
	// by instructions actually retired (exact, always), not by this estimate.
	uint64_t calibrateCyclesPerInvocation(uint32_t _loopCount)
	{
		Harness h(_loopCount);
		const uint64_t instructionsPerInvocation = 2 + 2ull * _loopCount;

		// Comfortably more than one invocation regardless of loopCount (1 or 50).
		h.dsp.execUntilCycles(h.dsp.getCycles() + 10000);

		const auto instructions = h.dsp.getInstructionCounter();
		const auto invocations = std::max<uint64_t>(1,
			(instructions + instructionsPerInvocation / 2) / instructionsPerInvocation);
		return std::max<uint64_t>(1, h.dsp.getCycles() / invocations);
	}

	// Measures wall time to execute approximately _invocations DO-loop entries,
	// having already warmed up (forced JIT compilation of all three blocks)
	// beforehand. execUntilCycles only guarantees running until the first block
	// that reaches the target cycle count has completed - chained blocks (our
	// `jmp` back to a compile-time-known target chains directly into the next
	// block, see calibrateCyclesPerInvocation) mean it can land anywhere at or
	// past the requested target, not exactly on it. Rather than fight that,
	// request a generous cycle target and report wall time per *actual*
	// invocation executed (from the instruction counter, which is exact).
	double measure(uint32_t _loopCount, uint64_t _invocations, double& _nsPerInvocation)
	{
		const auto cyclesPerInvocation = calibrateCyclesPerInvocation(_loopCount);
		const uint64_t instructionsPerInvocation = 2 + 2ull * _loopCount;

		Harness h(_loopCount);

		// Warm up generously so JIT compilation (a one-time cost per block, not
		// per invocation) happens outside the timed region.
		h.dsp.execUntilCycles(h.dsp.getCycles() + cyclesPerInvocation * 10);

		const auto instructionsBeforeTimedRun = h.dsp.getInstructionCounter();
		const auto targetCycles = h.dsp.getCycles() + cyclesPerInvocation * _invocations;

		const auto t0 = Clock::now();
		h.dsp.execUntilCycles(targetCycles);
		const auto t1 = Clock::now();

		const auto instructionsExecuted = h.dsp.getInstructionCounter() - instructionsBeforeTimedRun;
		const auto actualInvocations = instructionsExecuted / instructionsPerInvocation;
		if(actualInvocations == 0)
			throw std::runtime_error("timed run executed zero invocations");

		const double wallSeconds = std::chrono::duration<double>(t1 - t0).count();
		_nsPerInvocation = (wallSeconds / static_cast<double>(actualInvocations)) * 1e9;
		return wallSeconds;
	}
}

int main(int _argc, char** _argv)
{
	uint64_t invocations = 2'000'000;
	if(_argc > 1)
		invocations = std::strtoull(_argv[1], nullptr, 10);

	try
	{
		std::cout << "JIT mode: " << (dsp56k::g_useJIT ? "JIT" : "interpreter") << "\n";
		std::cout << "invocations per measurement: " << invocations << "\n";

		double perInvocation50Ns = 0;
		double perInvocation1Ns = 0;
		const double wall50 = measure(50, invocations, perInvocation50Ns);
		const double wall1 = measure(1, invocations, perInvocation1Ns);

		const double marginalPerIterationNs = (perInvocation50Ns - perInvocation1Ns) / 49.0;

		std::cout << "RESULT wall50Sec=" << wall50 << " wall1Sec=" << wall1 << "\n";
		std::cout << "RESULT perInvocation50Ns=" << perInvocation50Ns
			<< " perInvocation1Ns=" << perInvocation1Ns << "\n";
		std::cout << "RESULT marginalPerBodyIterationNs=" << marginalPerIterationNs << "\n";
		std::cout << "RESULT extra49IterationsCostFractionOfTotal="
			<< ((perInvocation50Ns - perInvocation1Ns) / perInvocation50Ns) << "\n";

		return 0;
	}
	catch(const std::exception& e)
	{
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
