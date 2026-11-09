#pragma once

#include "jag/Debug.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jag
{

// A DSP program run the way the FPGA simulation runs it (sim/sim_main.cpp): the same arguments and
// the same printed state, so that one is checked against the other line for line.
struct RunOptions
{
  std::string input;                 // BS94 or COFF file (ProgramFile), or empty with `raw`
  std::string raw;                   // raw DSP binary loaded at D_RAM
  std::uint64_t cycles = 10'000'000; // bus cycles; a bus cycle is two DSP cycles
  std::optional<std::uint32_t> pc;
  std::uint32_t flags = 0x4000;
  std::vector<std::pair<std::string, std::uint32_t>> loads;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> pokes;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> memHashes;
  std::string log;
  std::uint64_t logLines = UINT64_MAX;
  std::uint64_t logFrom = 0;   // the trace's events from this DSP cycle on
  bool step = false;           // no automatons: the stepped cores alone, the bus worked out every cycle
  bool gpu = false;            // the program runs on the GPU
  bool m68k = false;           // the program runs on the 68000, from reset
  std::string cart;            // the cartridge's ROM, from $800000
  std::string bios;            // the OS ROM in place of the boot program (with m68k; the program optional)
  std::optional<bool> compat;  // the MiSTer core's max_compat (decision 0022); left out, on with a BIOS
  std::optional<bool> openBus; // the netlist's open bus for DSP stores (decision 0033); left out, off with a BIOS
  std::string framesDir;       // each frame written there as frame_NNNN.ppm
  std::string wav;             // the I2S output written there, a frame a sample
  std::uint64_t part = 0;      // DSP cycles a part of the run, as a front end runs it; 0 for one part

  bool compatible() const
  {
    return compat.value_or( !bios.empty() );
  }

  bool openBusStores() const
  {
    return openBus.value_or( bios.empty() );
  }
};

struct TraceEvent
{
  std::uint64_t cycle;
  std::string text;
};

struct RunResult
{
  bool m68k = false; // the 68000's run: no RISC registers
  bool finished = false;
  std::uint64_t dspCycles = 0;
  std::array<std::uint32_t, 64> regs{};
  // the other processors' the program started: "gpu" or "dsp", their registers, in that order
  std::vector<std::pair<std::string, std::array<std::uint32_t, 64>>> others;
  std::vector<std::pair<std::string, std::uint32_t>> memHashes;
  std::vector<std::string> frames;    // the video output's frames, as the simulation prints them
  std::vector<TraceEvent> trace;      // sorted by cycle
  std::vector<UndrivenRead> undriven; // Machine::undrivenReads
};

// The options parseRunOptions takes, one to a line with what each does, for a front end's --help.
std::string_view runOptionsHelp();

// Parses the arguments after the program name; throws std::runtime_error on a bad one.
RunOptions parseRunOptions( std::span<std::string const> args );

// A tests/programs/*.run: the program its PROGRAM line names (BS94 or COFF, loaded where it says, or a raw
// binary loaded at the local RAM) with the arguments of its RUN lines, paths relative to the file's
// directory. Throws std::runtime_error when it cannot be read or names no program.
RunOptions readRunFile( std::string const& path );

RunResult runProgram( RunOptions const& options, bool keepTrace );

// The state as jagsim prints it: finished, cycles, dsp_cycles, the 64 registers, the other
// processor's when it ran, the memory hashes.
std::string formatState( RunResult const& result );

} // namespace jag
