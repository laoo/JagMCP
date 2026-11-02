#pragma once

#include "Fiber.hpp"
#include "Moira.h"
#include "jag/Debug.hpp"
#include "jag/MainBus.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace jag
{

class TraceSink;

// The 68000: Moira (libextern/Moira) with its bus cycles made on the main bus at the emulated time
// it makes them. Moira's clock counts the 68000's cycles, two of the bus's each, from the start of
// its reset; a bus cycle's S4 is where Moira calls for its data, its S2 (AS falling) two of the
// bus's cycles before. The bus inserts the wait states: DTACK taken later than S4 adds clocks.
//
// The trace takes its bus cycles: "A(addr) r|w fc<n>" as AS falls, "D=<data>" as it rises (the
// data under the strobes, -- for a byte left out), as the simulation writes them.
class M68000 : public moira::Moira
{
public:
  explicit M68000( MainBus& bus );

  void setTrace( TraceSink* trace )
  {
    mTrace = trace;
  }

  // What runs the other processors up to a cycle, when the bus cannot step there without them.
  void setOthers( std::function<void( std::uint64_t )> others )
  {
    mOthers = std::move( others );
  }

  // A debugger's probe of the 68000, and the stop it shares with the RISCs; null for none. A run
  // stops at an instruction's boundary: before an instruction at a breakpoint, after one whose read
  // or write of data a watchpoint covers, or after any once a RISC has stopped.
  void setDebug( Debugger* shared, Probe* probe )
  {
    mDebugger = shared;
    mProbe = probe;
  }

  // Resets it as reset ends in cycle 0 of the bus: it reads its vectors and fills its prefetch.
  void powerOn();

  // A STOP waits for an interrupt.
  bool stopped() const
  {
    return ( flags & moira::State::STOPPED ) != 0;
  }

  // For a debugger, at an instruction's boundary: the next instruction is the one at `pc`, its
  // prefetch read from memory without a bus cycle. A STOP waits no longer.
  void setNextPc( std::uint32_t pc );

  // The run ends at `cycle`: no bus cycle starts there or after, nor is traced.
  void setLimit( std::uint64_t cycle )
  {
    mLimit = cycle;
  }

  // Runs instructions until it reaches `cycle`: a STOP waits there for an interrupt. A bus cycle
  // still waiting for the bus there is left waiting, and goes on with the next run (waiting()).
  // The instructions run on a thread of their own, by turns with the caller (Fiber).
  void runUntil( std::uint64_t cycle );

  // The bus cycle left waiting for the bus where the last run ended, if any.
  std::optional<BusWait> const& waiting() const
  {
    return mWaiting;
  }

  // The cycle the run has reached: the bus's while a bus cycle is left waiting, else now().
  std::uint64_t reached() const
  {
    return mWaiting ? std::max( now(), mBus->now() ) : now();
  }

  // The cycle of the bus its clock has reached.
  std::uint64_t now() const
  {
    return cycleOf( clock );
  }

private:
  // the bus's cycle of the 68000's clock `c`: its first half (AS falls in it), as the simulation
  // starts it after reset
  static std::uint64_t cycleOf( std::int64_t c )
  {
    return static_cast<std::uint64_t>( std::max<std::int64_t>( 2 * c - RESET_OFFSET, 0 ) );
  }

  static constexpr std::int64_t RESET_OFFSET = 5;

  // where the 68000 samples Tom's interrupt line for an instruction boundary: cycles before the S0
  // of the instruction's last program read; while stopped, cycles back, once stopped for STOP_DWELL
  static constexpr std::uint64_t IPL_RUNNING = 4;
  static constexpr std::uint64_t IPL_STOPPED = 10;
  static constexpr std::uint64_t STOP_DWELL = 8;
  static constexpr std::uint64_t NEXT_START = 6; // DTACK to the next bus cycle's AS, at the least
  // a poll taking an interrupt while stopped to its first bus cycle's AS, the push of PC's low word
  // six clocks on (Moira's execInterrupt), at the least
  static constexpr std::uint64_t WAKE_START = IPL_STOPPED + 2 * 6 - 2;

  // the cycles of the bus nothing it does yet can change: all before this
  std::uint64_t horizon() const;

  // A bus cycle Moira calls for at its S4, now; returns a read's data.
  std::uint16_t access( bool read, std::uint32_t address, bool uds, bool lds, std::uint16_t data ) const;

  struct Done
  {
    std::uint16_t value;
    std::int64_t waits;
  };

  Done perform( M68kAccess a, std::int64_t s4 ) const;

  // the function code of an access of program space
  std::uint32_t programFc() const
  {
    return reg.sr.s ? 6u : 2u;
  }

  void sync( int cycles ) override;
  using moira::Moira::willExecute;
  void willExecute( char const* func, moira::Instr i, moira::Mode m, moira::Size s, moira::u16 opcode ) override;
  using moira::Moira::didExecute;
  void didExecute( char const* func, moira::Instr i, moira::Mode m, moira::Size s, moira::u16 opcode ) override;
  moira::u8 read8( moira::u32 addr ) const override;
  moira::u16 read16( moira::u32 addr ) const override;
  moira::u32 read32( moira::u32 addr ) const override;
  moira::u16 read16Dasm( moira::u32 addr ) const override;
  moira::u16 read16OnReset( moira::u32 addr ) const override;
  moira::u16 readIrqUserVector( moira::u8 level ) const override;
  void willInterrupt( moira::u8 level ) override;
  void write8( moira::u32 addr, moira::u8 val ) const override;
  void write16( moira::u32 addr, moira::u16 val ) const override;
  void write32( moira::u32 addr, moira::u32 val ) const override;

  MainBus* mBus;
  TraceSink* mTrace = nullptr;
  Debugger* mDebugger = nullptr;
  Probe* mProbe = nullptr;

  // A debugger's: whether the instruction at the boundary reached stops the run; a data access of
  // the instruction under way met against the watchpoints.
  bool breakHere();
  void watchAccess( std::uint32_t address, std::uint32_t bytes, bool write, std::uint32_t value ) const;
  std::function<void( std::uint64_t )> mOthers;

  // Steps the bus through `cycle`, the other processors run as far as it needs them, or on to
  // `reach`.
  void stepBusThrough( std::uint64_t cycle, std::uint64_t reach = 0 ) const;
  std::uint64_t mLimit = UINT64_MAX;
  std::uint64_t mTarget = 0; // where runUntil runs to
  std::optional<BusWait> mWaiting;
  M68000* mSelf; // what the const callbacks of Moira's interface change
  int mResetReads = 0;
  bool mStopFetch = false; // STOP's fetch of the word after it, which the 68000 does not make
  bool mStopOpen = false;  // the interrupt mask let level 2 through before STOP loaded SR
  bool mDbccDummy = false; // DBcc's count ended: its dummy read is from the branch's target
  std::uint32_t mDbccTarget = 0;
  bool mRefetch = false; // an instruction changing CCR or SR: its dummy read is of the word at PC
  int mTas = 0;          // TAS: 1 before its read, 2 between its read and its write
  std::uint64_t mTasStart = 0;
  int mSwapPairs = 0; // long words still to write low half first
  bool mJsr = false;  // JSR: the push waits for the target's fetch
  bool mRotateByZero = false;
  bool mAcknowledge = false;                 // an interrupt is taken: its acknowledge cycle is to come
  std::uint64_t mLastProgramS0 = UINT64_MAX; // the S0 of the instruction's last program read
  std::uint64_t mStopped = UINT64_MAX;       // the cycle it was first found stopped in
  int mMovemAhead = 0;                       // MOVEM.L from memory: 1 before its first read, 2 after
  int mMovemReads = 0;                       // its operand reads still to come, Moira's extra one first
  bool mMovemPc = false;                     // from a PC relative address: all of them in program space
  std::uint16_t mAhead = 0;
  int mSwapReads = 0;     // long words still to read low half first
  bool mSwapHigh = false; // the low half read, the high half's cycle to come
  std::uint16_t mSwapLow = 0;

  // writes waiting for the cycle they are made in, each with Moira's clock at its S4
  struct Deferred
  {
    M68kAccess access;
    std::int64_t clock;
  };

  std::vector<Deferred> mDeferred;

  // runUntil's instructions, on the fiber's thread
  void runToTarget();
  // last, so that its thread ends before the rest goes
  Fiber mFiber{ [this] { runToTarget(); } };
};

} // namespace jag
