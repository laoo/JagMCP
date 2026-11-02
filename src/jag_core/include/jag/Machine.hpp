#pragma once

#include "jag/Debug.hpp"
#include "jag/Runner.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jag
{

class I2s;
class M68000;
class MainBus;
class Risc;
class Tom;
class TraceSink;

// The DSP cycles of a second: the system clock of a PAL console, as the simulation counts it.
inline constexpr double CYCLES_PER_SECOND = 26'593'900.0;

// A program on the console as the runner starts it (RunOptions): main RAM with what the options
// load into it, the bus, the processor the program runs on and, on the GPU, the DSP it may start;
// with --m68k the 68000 from reset, the OS ROM the simulation's boot program jumping to it.
// It runs in parts, so that a front end shows the frames and plays the sound as they are made; the
// parts give the run the runner gives in one.
class Machine
{
public:
  // Loads the program and sets its processor going; `trace`, if not null, takes both processors'
  // events. Throws std::runtime_error when a file cannot be read or does not fit.
  Machine( RunOptions const& options, TraceSink* trace );
  ~Machine();

  Machine( Machine const& ) = delete;
  Machine& operator=( Machine const& ) = delete;
  Machine( Machine&& ) = delete;
  Machine& operator=( Machine&& ) = delete;

  // The run ends at `cycle` at the latest: nothing planned for after it is made. Only lowered.
  void setLimit( std::uint64_t cycle );

  // Runs the processors by turns up to `cycle` (a node of the automaton may take one past it).
  // Returns whether the run goes on: the program's processor has not stopped, nor the limit come.
  bool runTo( std::uint64_t cycle );

  // The cycle the program's processor has reached.
  std::uint64_t now() const;

  // Ends the run where the program's processor ended it: what the bus has under way done, the
  // video output rendered up to there, the trace complete.
  void finish();

  // The state as the runner reports it, but for the trace: once finished.
  RunResult result() const;

  Tom& tom();

  // Jerry's I2S interface, the DSP's.
  I2s& i2s();

  // For a debugger, between runs: what the console's memory holds at `address`, read without a bus
  // cycle (main RAM, the OS ROM and the cartridge as the board maps them, the GPU's and the DSP's
  // local RAM), or none where none of them is; the same written (main RAM and the local RAMs),
  // false where nothing can be.
  std::optional<std::uint8_t> peek( std::uint32_t address ) const;
  bool writable( std::uint32_t address ) const;
  bool poke( std::uint32_t address, std::uint8_t value );

  // A debugger's probes of the processors, made and installed the first time it is asked for: from
  // then on a run stops short where one of them says (DebugStop, its `stop`), and runTo returns
  // there. A run without them checks nothing.
  Debugger& debugger();

  // A single step of the processor `probe` is the debugger's probe of: the runs from now stop
  // before its next instruction, once the one under way or about to issue (the RISC's pc) is
  // executed, until the step is cleared (Probe::step).
  void step( Probe& probe );

  // The reads of addresses no register answers, by processor, instruction and address, in the order
  // first made; the first UNDRIVEN_READS of them. Kept whether a debugger is installed or not.
  std::vector<UndrivenRead> const& undrivenReads() const
  {
    return mUndriven;
  }

  static constexpr std::size_t UNDRIVEN_READS = 64;

  // The 68000's bus cycle left waiting for the bus where the last run ended (another master keeps
  // it), if any. It goes on with the next run.
  std::optional<BusWait> busWait() const;

  // The buttons held on the pad in Jerry's port 0 or 1 (MainBus::setPad), with --m68k.
  void setPad( std::size_t port, std::uint32_t buttons );
  std::uint32_t pad( std::size_t port ) const;

  // The GPU and the DSP, null where the run has none.
  Risc* gpu();
  Risc* dsp();

  // The 68000's registers, with --m68k.
  struct M68kState
  {
    std::array<std::uint32_t, 8> d{};
    std::array<std::uint32_t, 8> a{};
    std::uint32_t pc = 0;
    std::uint32_t sr = 0;
    std::uint32_t usp = 0;
    std::uint32_t isp = 0;
    bool stopped = false; // a STOP waits for an interrupt
    bool halted = false;
  };

  std::optional<M68kState> m68kState() const;

  // For a debugger, between runs, with --m68k: the registers given, as the next instruction finds
  // them. SR is set first, as it may exchange the stack pointers; A7 is the one in use. PC starts
  // the instruction there, read without a bus cycle, and ends a STOP. Nothing is set, and the answer
  // is false, while a bus cycle waits for the bus (busWait): the 68000 is inside an instruction.
  struct M68kRegisters
  {
    std::array<std::optional<std::uint32_t>, 8> d{};
    std::array<std::optional<std::uint32_t>, 8> a{};
    std::optional<std::uint32_t> pc;
    std::optional<std::uint32_t> sr;
    std::optional<std::uint32_t> usp;
    std::optional<std::uint32_t> isp;
  };

  bool setM68kRegisters( M68kRegisters const& registers );

  // The 68000's instruction at `address` as text, and its length in bytes, with --m68k.
  std::optional<std::pair<std::string, std::uint32_t>> disassemble68k( std::uint32_t address ) const;

private:
  struct Ram;
  struct Outputs;

  RunOptions mOptions;
  std::unique_ptr<Ram> mRam;
  std::unique_ptr<Outputs> mOutputs; // the files the options write the frames and the sound to
  std::unique_ptr<MainBus> mBus;
  std::unique_ptr<Risc> mProgram; // the processor the program runs on, a RISC
  std::unique_ptr<Risc> mDsp;     // on the GPU: the DSP it may start
  std::unique_ptr<M68000> mM68k;  // or the 68000, beside the GPU and the DSP it may start
  std::unique_ptr<Risc> mGpu;
  std::unique_ptr<Debugger> mDebugger; // null until a debugger asks for it
  std::vector<UndrivenRead> mUndriven;
  std::uint64_t mLimit = UINT64_MAX;
  bool mPoweredOn = false;
  bool mRuns = true;
  bool mDspRuns = true;
  bool mGpuRuns = true;

  // Takes the bus's undriven reads into mUndriven.
  void noteUndrivenReads();

  // The RISCs the 68000 has started, by turns, up to `cycle` or until each waits for the 68000.
  void runRiscs( std::uint64_t cycle );
};

} // namespace jag
