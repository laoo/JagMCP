#include "M68000.hpp"

#include "jag/MainBus.hpp"
#include "jag/Risc.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <stdexcept>

#ifndef JAG_MOIRA_CONFIG
#error "Moira found a MoiraConfig.h other than src/jag_core/moira/MoiraConfig.h"
#endif

namespace jag
{

M68000::M68000( MainBus& bus ) : mBus( &bus ), mSelf( this )
{
  setModel( moira::Model::M68000 );
  irqMode = moira::IrqMode::USER; // the vector is read in the acknowledge cycle (VPA is tied high)
}

void M68000::powerOn()
{
  mResetReads = 0;
  reset();
}

void M68000::setNextPc( std::uint32_t pc )
{
  // as an instruction starts in Moira: IRD its first word, IRC the word after
  reg.pc = reg.pc0 = pc;
  queue.ird = read16Dasm( pc );
  queue.irc = read16Dasm( pc + 2 );
  flags &= ~moira::State::STOPPED;
}

void M68000::runUntil( std::uint64_t cycle )
{
  mTarget = cycle;
  mFiber.run();
}

void M68000::runToTarget()
{
  std::uint64_t const& cycle = mTarget; // a bus cycle left waiting goes on with the next run's
  while ( now() < cycle )
  {
    if ( mDebugger && breakHere() )
      return;
    if ( ( flags & moira::State::STOPPED ) && reg.sr.ipl >= 2 )
    {
      // Tom's interrupt, level 2, is masked: nothing ends the STOP
      clock = std::max<std::int64_t>( clock, ( static_cast<std::int64_t>( cycle ) + RESET_OFFSET + 1 ) / 2 );
      mBus->m68kHorizon( horizon() );
      return;
    }
    // the interrupt as the 68000 has taken it in for this boundary: Tom's line where it samples it,
    // four cycles before the S0 of the instruction's last program read as it was asked for; or,
    // stopped, ten cycles back: as it stops if the mask let the interrupt through before, else once
    // it has been stopped long enough (hardware/m68000.md)
    bool const stopped = ( flags & moira::State::STOPPED ) != 0;
    bool const stopping = stopped && mStopped == UINT64_MAX;
    if ( !stopped )
      mStopped = UINT64_MAX;
    else if ( stopping )
      mStopped = now();
    std::uint64_t sample = UINT64_MAX;
    if ( stopped && ( now() >= mStopped + STOP_DWELL || ( stopping && mStopOpen ) ) )
      sample = now() - IPL_STOPPED;
    else if ( !stopped && mLastProgramS0 != UINT64_MAX )
      sample = mLastProgramS0 > IPL_RUNNING ? mLastProgramS0 - IPL_RUNNING : 0;
    if ( stopped || mLastProgramS0 != UINT64_MAX )
    {
      // stopped, the RISCs run on to where the interrupt it may take in this poll makes its first bus
      // cycle, at the soonest: the polls before it then step the bus alone
      if ( sample != UINT64_MAX )
        stepBusThrough( sample, stopped ? sample + WAKE_START : 0 );
      reg.ipl = sample != UINT64_MAX && mBus->m68kInterrupt( sample ) ? 2 : 0;
      flags |= moira::State::CHECK_IRQ;
    }
    mLastProgramS0 = UINT64_MAX;
    // what willExecute sets for an instruction is its own
    mStopFetch = mDbccDummy = mRefetch = mJsr = mSwapHigh = false;
    mTas = mSwapPairs = mMovemAhead = mMovemReads = mSwapReads = 0;
    if ( mTrace )
    {
      // the core's view of the instruction, which the comparison leaves out
      char text[128];
      char line[96];
      disassemble( line, reg.pc );
      std::snprintf( text, sizeof text, "I %06x %s", reg.pc, line );
      mTrace->event( now(), text );
    }
    if ( mProbe && ( flags & moira::State::STOPPED ) == 0 )
      mProbe->executed( reg.pc, now() );
    execute();
  }
}

bool M68000::breakHere()
{
  if ( mDebugger->stop )
    return true;
  if ( ( flags & moira::State::STOPPED ) != 0 || mProbe->passAt == now() )
    return false;
  bool const breaks = mProbe->breaksAt( reg.pc );
  if ( !breaks && !mProbe->step )
    return false;
  mProbe->passAt = now();
  mDebugger->stop = DebugStop{
    .kind = breaks ? DebugStop::Kind::Breakpoint : DebugStop::Kind::Step, .cpu = "m68k", .pc = reg.pc, .cycle = now()
  };
  return true;
}

void M68000::watchAccess( std::uint32_t address, std::uint32_t bytes, bool write, std::uint32_t value ) const
{
  if ( mDebugger->stop || ( readFC() & 3u ) == 2u || !mProbe->watches( address, bytes, write ) )
    return;
  mDebugger->stop = DebugStop{ .kind = DebugStop::Kind::Watchpoint,
                               .cpu = "m68k",
                               .pc = reg.pc0,
                               .cycle = now(),
                               .address = address,
                               .write = write,
                               .value = value,
                               .bytes = bytes };
}

// Every bus cycle is called for at its S4, two clocks after its S0, its AS falling at S2; the bus
// decides on its S0 in the clock before (w432), and holds a grant back in its S0 and S1 (w395). So
// the bus may be stepped up to three clocks before the clock reached.
void M68000::sync( int cycles )
{
  clock += cycles;
  mBus->m68kHorizon( horizon() );
}

// The acknowledge cycle of an interrupt (FC 7, the level on A1 to A3, the vector in the low byte),
// its S4 two clocks before Moira asks for the vector.
void M68000::willInterrupt( moira::u8 /*level*/ )
{
  mAcknowledge = true;
}

moira::u16 M68000::readIrqUserVector( moira::u8 level ) const
{
  mSelf->mAcknowledge = false;
  M68kAccess a;
  a.read = true;
  a.address = 0xfffff0u | static_cast<std::uint32_t>( level ) << 1;
  a.uds = true; // both strobes, as the simulation's 68000 has them
  a.lds = true;
  a.fc = 7;
  Done const done = perform( a, clock - 2 );
  mSelf->clock += done.waits;
  return static_cast<moira::u16>( done.value & 0xffu );
}

// The RISCs run up to the cycle after the one the bus is to step, or on to `reach`, where nothing of
// the 68000's comes on the bus before.
void M68000::stepBusThrough( std::uint64_t cycle, std::uint64_t reach ) const
{
  while ( mBus->now() <= cycle && mBus->now() < mLimit )
  {
    std::uint64_t const before = mBus->now();
    if ( mOthers )
      mOthers( std::max( cycle + 1, std::min( reach, mLimit ) ) );
    mBus->m68kContinue();
    if ( mBus->now() == before )
      throw std::logic_error( "the 68000 and the RISCs wait for each other" );
  }
}

// A cycle waiting for the one after it holds the bus back: its S0 is still to be decided.
std::uint64_t M68000::horizon() const
{
  // the acknowledge of an interrupt has its S4 two clocks before Moira asks for the vector
  std::uint64_t const back = mAcknowledge ? 10 : 6;
  std::uint64_t const h = std::max<std::uint64_t>( now(), back ) - back;
  return mDeferred.empty() ? h : std::min( h, std::max<std::uint64_t>( cycleOf( mDeferred.front().clock ), 6 ) - 6 );
}

namespace
{

// The condition `cc` (an opcode's bits 8 to 11) of the status register.
bool condition( std::uint32_t cc, moira::StatusRegister const& sr )
{
  switch ( cc )
  {
  case 0:
    return true;
  case 1:
    return false;
  case 2:
    return !sr.c && !sr.z;
  case 3:
    return sr.c || sr.z;
  case 4:
    return !sr.c;
  case 5:
    return sr.c;
  case 6:
    return !sr.z;
  case 7:
    return sr.z;
  case 8:
    return !sr.v;
  case 9:
    return sr.v;
  case 10:
    return !sr.n;
  case 11:
    return sr.n;
  case 12:
    return sr.n == sr.v;
  case 13:
    return sr.n != sr.v;
  case 14:
    return !sr.z && sr.n == sr.v;
  default:
    return sr.z || sr.n != sr.v;
  }
}

} // namespace

// Where the 68000's bus cycles differ from Moira's, the times of its cycles being Moira's:
// - STOP: Moira fetches the word after it as it takes its operand; the 68000 makes no bus cycle.
// - DBcc whose count ends: the 68000's dummy read is of the word at the branch's target, Moira's
//   of the word after the displacement.
// - ANDI, ORI, EORI and MOVE to CCR or SR: the 68000's dummy read fetches the word at PC again
//   (program space), Moira's the word after it.
// - TAS of memory: the 68000 reads and writes in one bus cycle, AS held low.
// - ADD, SUB, ADDQ, SUBQ, ADDI, SUBI, ADDX and SUBX of a long word in memory, and MOVEM of long
//   words to memory predecremented: the 68000 writes a long word's low half first, Moira its high
//   half.
// - JSR: the 68000 fetches the word at the target before it pushes the return address, Moira
//   after.
// - MOVEM of long words from memory: Moira's extra read is of the first word, before the others;
//   the 68000's of the word after the last, after them.
// - ADDX and SUBX of long words predecremented: the 68000 reads each operand's low half first,
//   Moira its high half.
void M68000::willExecute( char const* /*func*/, moira::Instr i, moira::Mode m, moira::Size s, moira::u16 opcode )
{
  using moira::Instr;
  using moira::Mode;
  mStopFetch = i == Instr::STOP;
  if ( mStopFetch )
    mStopOpen = reg.sr.ipl < 2;
  mRefetch = i == Instr::ANDICCR || i == Instr::ANDISR || i == Instr::ORICCR || i == Instr::ORISR ||
             i == Instr::EORICCR || i == Instr::EORISR || i == Instr::MOVETCCR || i == Instr::MOVETSR;
  mTas = i == Instr::TAS ? 1 : 0;
  mDbccDummy = i >= Instr::DBCC && i <= Instr::DBT && !condition( ( opcode >> 8 ) & 0xfu, reg.sr ) &&
               ( reg.d[opcode & 7u] & 0xffffu ) == 0;
  if ( mDbccDummy )
    mDbccTarget = reg.pc + static_cast<std::uint32_t>( static_cast<std::int16_t>( queue.irc ) );
  bool const memory = m != Mode::DN && m != Mode::AN && m != Mode::IM;
  bool const movemOut = i == Instr::MOVEM && ( opcode & 0x400u ) == 0;
  bool const toMemory = ( ( i == Instr::ADD || i == Instr::SUB ) && ( opcode & 0x100u ) != 0 ) || i == Instr::ADDQ ||
                        i == Instr::SUBQ || i == Instr::ADDI || i == Instr::SUBI;
  if ( s == moira::Long && memory && toMemory )
    mSwapPairs = 1;
  if ( s == moira::Long && movemOut && m == Mode::PD )
    mSwapPairs = static_cast<int>( std::popcount( static_cast<unsigned>( queue.irc ) ) );
  mJsr = i == Instr::JSR;
  // a register's count (bit 5), of zero modulo 64
  mRotateByZero = ( i == Instr::ROXL || i == Instr::ROXR ) && m == Mode::DN && ( opcode & 0x20u ) != 0 &&
                  ( reg.d[( opcode >> 9 ) & 7u] & 63u ) == 0;
  mSwapReads = s == moira::Long && ( i == Instr::ADDX || i == Instr::SUBX ) && m == Mode::PD ? 2 : 0;
  mMovemAhead = i == Instr::MOVEM && !movemOut && s == moira::Long ? 1 : 0;
  mMovemReads = mMovemAhead != 0 ? 2 * std::popcount( static_cast<unsigned>( queue.irc ) ) + 1 : 0;
  mMovemPc = m == Mode::DIPC || m == Mode::IXPC;
}

// ROXL and ROXR by a register's count of zero clear C in the simulation's 68000, where Moira sets
// it to X (as the manual has it).
void M68000::didExecute(
    char const* /*func*/, moira::Instr i, moira::Mode /*m*/, moira::Size /*s*/, moira::u16 /*opcode*/ )
{
  mTas = 0;
  if ( ( i == moira::Instr::ROXL || i == moira::Instr::ROXR ) && mRotateByZero )
    reg.sr.c = false;
}

// A bus cycle made with its S4 at Moira's clock `s4`: its data, and the clocks it took more than
// four (a start held back, DTACK taken after S4).
M68000::Done M68000::perform( M68kAccess a, std::int64_t s4 ) const
{
  a.start = cycleOf( s4 ) - 2;                      // S2: AS falls
  if ( a.start >= mLimit || mBus->now() >= mLimit ) // the run has ended: nothing more reaches the bus
    return { a.read ? mBus->m68kPeek( a.address ) : a.data, 0 };
  mBus->m68kAccess( a );
  MainBus::M68kEnd end = mBus->m68kEnd();
  while ( !end.ended && mBus->now() < mLimit )
  {
    std::uint64_t const before = mBus->now();
    mBus->m68kContinue();
    // held back by the RISCs: they run up to it, or on to its start; through a cycle that reaches
    // only memory, on to where the next could start (AS six cycles after DTACK, not yet taken)
    if ( mBus->now() == before )
      stepBusThrough( before, mBus->m68kAtMemory() ? std::max( a.start, before + NEXT_START ) : a.start );
    end = mBus->m68kEnd();
    // the run ends while the cycle waits for the bus: its caller goes on, the cycle with the next run
    if ( !end.ended && mBus->now() >= mTarget && mBus->now() < mLimit && mFiber.inside() )
    {
      mSelf->mWaiting = BusWait{ reg.pc0, a.address, a.read, a.start, mBus->busHolder() };
      mSelf->mFiber.suspend();
      mSelf->mWaiting.reset();
    }
  }
  if ( !end.ended ) // the run ends first; AS has fallen if the bus got there
  {
    mSelf->clock = std::max<std::int64_t>( clock, ( static_cast<std::int64_t>( mLimit ) + RESET_OFFSET + 1 ) / 2 );
    if ( mTrace && mBus->now() > end.start && !a.held )
    {
      char text[48];
      std::snprintf( text, sizeof text, "A(%06x) %c fc%u", a.address, a.read || a.holdAs ? 'r' : 'w', a.fc );
      mTrace->event( end.start, text );
    }
    return { a.read ? mBus->m68kPeek( a.address ) : a.data, 0 };
  }
  if ( a.holdAs )
    mSelf->mTasStart = end.start;
  // a program read: where the 68000 samples its interrupt, at the S0 it was asked for
  if ( a.read && ( a.fc & 3u ) == 2u )
    mSelf->mLastProgramS0 = a.start - 2;
  Done done{ a.read ? end.data : a.data, static_cast<std::int64_t>( ( end.dtack - ( a.start + 2 ) ) / 2 ) };
  if ( mTrace && !a.holdAs ) // TAS's cycle is traced as its write ends, as one
  {
    char text[48];
    std::snprintf( text, sizeof text, "A(%06x) %c fc%u", a.address, a.read || a.held ? 'r' : 'w', a.fc );
    if ( end.start < mLimit )
      mTrace->event( a.held ? mTasStart : end.start, text );
    char hi[3] = "--";
    char lo[3] = "--";
    if ( a.uds && a.fc != 7 ) // an interrupt's vector is the low byte; the high one is the bus's
      std::snprintf( hi, sizeof hi, "%02x", done.value >> 8 );
    if ( a.lds )
      std::snprintf( lo, sizeof lo, "%02x", done.value & 0xffu );
    std::snprintf( text, sizeof text, "D=%s%s", hi, lo );
    if ( end.dtack + 3 < mLimit )
      mTrace->event( end.dtack + 3, text );
  }
  return done;
}

std::uint16_t M68000::access( bool read, std::uint32_t address, bool uds, bool lds, std::uint16_t data ) const
{
  M68000& self = *mSelf;
  M68kAccess a;
  a.read = read;
  a.address = address & 0xfffffeu;
  a.uds = uds;
  a.lds = lds;
  a.fc = readFC();
  a.data = data;
  bool const program = ( a.fc & 3u ) == 2u;
  if ( mStopFetch && read && program ) // not a privilege violation's frame
  {
    self.mStopFetch = false;
    return mBus->m68kPeek( address );
  }
  if ( mDbccDummy )
  {
    self.mDbccDummy = false;
    a.address = mDbccTarget;
  }
  if ( mRefetch && read && address == reg.pc + 2 && reg.pc0 != reg.pc ) // not the prefetch: its pc0 is pc
  {
    self.mRefetch = false;
    a.address = reg.pc;
    a.fc = programFc();
  }
  a.holdAs = mTas == 1 && read && !program;
  a.held = mTas == 2 && !read;
  if ( a.holdAs )
    self.mTas = 2;

  // MOVEM of long words from memory: each cycle reads the word after the one Moira asks for, which
  // the cycle before has read; Moira's extra read, the first, is in data space, the 68000's (the
  // last) in the operands' (program space from a PC relative address)
  if ( mMovemReads != 0 && read && ( mMovemAhead == 2 || !program ) ) // not the mask's prefetch
  {
    --self.mMovemReads;
    std::uint16_t const before = mAhead;
    if ( mMovemAhead == 2 )
      a.address += 2;
    if ( mMovemPc )
      a.fc = programFc();
    self.mMovemAhead = 2;
    Done const done = perform( a, clock );
    self.mAhead = done.value;
    self.clock += done.waits;
    return before;
  }

  // ADDX and SUBX of long words predecremented: each operand's low half read first; Moira gets the
  // high half it asks for first as it is in memory, the cycle reading it comes second
  if ( mSwapReads > 0 && read && !program )
  {
    if ( !mSwapHigh )
    {
      self.mSwapHigh = true;
      M68kAccess low = a;
      low.address += 2;
      Done const done = perform( low, clock );
      self.mSwapLow = done.value;
      self.clock += done.waits;
      return mBus->m68kPeek( a.address );
    }
    self.mSwapHigh = false;
    --self.mSwapReads;
    M68kAccess high = a;
    high.address -= 2;
    Done const done = perform( high, clock );
    self.clock += done.waits;
    return mSwapLow;
  }

  // a long word's halves written the other way round, the return address pushed after JSR's fetch
  // of its target: the writes wait for the cycle they are made in
  bool const defer = !read && ( ( mSwapPairs > 0 && mDeferred.empty() ) || ( mJsr && mDeferred.size() < 2 ) );
  if ( defer )
  {
    self.mDeferred.push_back( { a, clock } );
    return data;
  }
  if ( mDeferred.empty() )
  {
    Done const done = perform( a, clock );
    self.clock += done.waits;
    return done.value;
  }
  std::vector<Deferred> slots = std::move( self.mDeferred );
  self.mDeferred.clear();
  slots.push_back( { a, clock } );
  std::vector<M68kAccess> order;
  if ( mJsr )
  {
    self.mJsr = false;
    order = { slots[2].access, slots[0].access, slots[1].access };
  }

  else
  {
    --self.mSwapPairs;
    order = { slots[1].access, slots[0].access };
  }
  std::int64_t waits = 0;
  std::uint16_t value = data;
  for ( std::size_t k = 0; k < slots.size(); ++k )
  {
    Done const done = perform( order[k], slots[k].clock + waits );
    waits += done.waits;
    if ( k == 0 && read ) // the cycle Moira calls for now is made first
      value = done.value;
  }
  self.clock += waits;
  return value;
}

moira::u8 M68000::read8( moira::u32 addr ) const
{
  bool const odd = ( addr & 1u ) != 0;
  std::uint16_t const v = access( true, addr, !odd, odd, 0 );
  auto const byte = static_cast<moira::u8>( odd ? v : v >> 8 );
  if ( mProbe && !mProbe->watchpoints.empty() )
    watchAccess( addr & 0xffffffu, 1, false, byte );
  return byte;
}

moira::u16 M68000::read16( moira::u32 addr ) const
{
  std::uint16_t const v = access( true, addr, true, true, 0 );
  if ( mProbe && !mProbe->watchpoints.empty() )
    watchAccess( addr & 0xfffffeu, 2, false, v );
  return v;
}

moira::u32 M68000::read32( moira::u32 addr ) const
{
  std::uint32_t const hi = read16( addr );
  return hi << 16 | read16( addr + 2 );
}

// The reset reads the stack pointer, the program counter and the first word of the program; the
// 68000 then waits two clocks more than Moira before it fills the rest of its prefetch.
moira::u16 M68000::read16OnReset( moira::u32 addr ) const
{
  std::uint16_t const v = read16( addr );
  if ( ++mSelf->mResetReads == 5 )
    mSelf->clock += 2;
  return v;
}

moira::u16 M68000::read16Dasm( moira::u32 addr ) const
{
  return mBus->m68kPeek( addr );
}

// A byte is written on both halves of the data bus.
void M68000::write8( moira::u32 addr, moira::u8 val ) const
{
  bool const odd = ( addr & 1u ) != 0;
  access( false, addr, !odd, odd, static_cast<std::uint16_t>( val * 0x0101u ) );
  if ( mProbe && !mProbe->watchpoints.empty() )
    watchAccess( addr & 0xffffffu, 1, true, val );
}

void M68000::write16( moira::u32 addr, moira::u16 val ) const
{
  access( false, addr, true, true, val );
  if ( mProbe && !mProbe->watchpoints.empty() )
    watchAccess( addr & 0xfffffeu, 2, true, val );
}

void M68000::write32( moira::u32 addr, moira::u32 val ) const
{
  write16( addr, static_cast<moira::u16>( val >> 16 ) );
  write16( addr + 2, static_cast<moira::u16>( val ) );
}

} // namespace jag
