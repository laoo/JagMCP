// The DSP of Jerry: Jaguar_MiSTer rtl/Jerry/j_dsp.v and rtl/jaguar_common (ins_exec.v, sboard.v,
// prefetch.v, pc.v, execon.v, gpu_mem.v, arith.v, divider.v, interrupt.v, gpu_ctrl.v), read at
// Jaguar_MiSTer 43a761d, checked against the traces of the FPGA simulation (tests/reference).

#include "jag/Risc.hpp"

#include "RiscAutomaton.hpp"
#include "RiscInternal.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <utility>

namespace jag
{

using namespace risc;

namespace
{

// Latencies, in DSP cycles from issue to the register write.
constexpr std::uint64_t MOVE_LATENCY = 1;
constexpr std::uint64_t ALU_LATENCY = 2;
constexpr std::uint64_t STOP_DELAY = 2; // from the cycle the write to D_CTRL is served

std::int32_t signExtend5( std::uint32_t v )
{
  return static_cast<std::int32_t>( v << 27 ) >> 27;
}

// The microword fields the scoreboard and the control use, per opcode: the DSP's microcode ROM
// (jaguar_common/ra6032a.v with JERRY=1), decoded as ins_exec.v decodes it.
struct Micro
{
  bool srcRead;    // srcrrd: srcdat == 0, the source register is read
  bool dstRead;    // dstrrd, microword[18]
  bool aluWrite;   // reswr, microword[17]: the ALU result is written back two cycles later
  bool movWrite;   // dstwr, microword[25]: written back the next cycle
  bool flagLoad;   // flagld, microword[11]
  bool flagDepend; // alufunc 1 or 3: ADDC and SUBC (flag_depend); conditional jumps are added
  bool memory;     // memrw, microword[19]: a load or a store
  bool store;      // datwe, microword[20]
  bool precomp;    // microword[21]: indexed, the address computed by the ALU first
};

constexpr std::array<std::uint32_t, 64> MICROCODE = {
  0x0061800, 0x0061880, 0x0071800, 0x0071000, 0x0061900, 0x0061980, 0x0071900, 0x0071100, 0x0069900, 0x0061a00,
  0x0061a80, 0x0061b00, 0x006db00, 0x0053a00, 0x0073a80, 0x0075a00, 0x0061803, 0x0061813, 0x0041813, 0x0020000,
  0x0040050, 0x0040000, 0x0069b80, 0x0061801, 0x0067801, 0x0071801, 0x0061819, 0x0071819, 0x0061811, 0x0063811,
  0x0041900, 0x004b900, 0x0071906, 0x0069802, 0x2000000, 0x2002000, 0x2000002, 0x2000001, 0x0408000, 0x0080000,
  0x0080008, 0x0080010, 0x0069822, 0x02c5010, 0x02c5410, 0x01c0000, 0x01c0008, 0x01c0010, 0x0069807, 0x03c5010,
  0x03c5410, 0x200e000, 0x1000000, 0x080a000, 0x0008000, 0x0021804, 0x0021805, 0x0008000, 0x02c1010, 0x02c1410,
  0x03c1010, 0x03c1410, 0x0008000, 0x0071806
};

// The GPU's (JERRY 0): SAT8, SAT16, LOADP, STOREP, SAT24, PACK where the DSP has SUBQMOD, SAT16S,
// SAT32S, MIRROR, an illegal opcode, ADDQMOD.
constexpr std::array<std::uint32_t, 64> MICROCODE_GPU = []()
{
  std::array<std::uint32_t, 64> m = MICROCODE;
  m[32] = 0x0069802;
  m[33] = 0x0069822;
  m[42] = 0x0080018;
  m[48] = 0x01c0018;
  m[62] = 0x4069802;
  m[63] = 0x0069006;
  return m;
}();

constexpr std::array<std::array<std::uint32_t, 64>, 2> MICROCODES = { MICROCODE, MICROCODE_GPU };

constexpr Micro micro( std::uint32_t w )
{
  std::uint32_t const alufunc = ( w >> 7 ) & 7;
  return Micro{ .srcRead = ( ( w >> 13 ) & 15 ) == 0,
                .dstRead = ( ( w >> 18 ) & 1 ) != 0,
                .aluWrite = ( ( w >> 17 ) & 1 ) != 0,
                .movWrite = ( ( w >> 25 ) & 1 ) != 0,
                .flagLoad = ( ( w >> 11 ) & 1 ) != 0,
                .flagDepend = ( alufunc & 1 ) != 0 && ( alufunc & 4 ) == 0,
                .memory = ( ( w >> 19 ) & 1 ) != 0,
                .store = ( ( w >> 20 ) & 1 ) != 0,
                .precomp = ( ( w >> 21 ) & 1 ) != 0 };
}

constexpr std::array<std::array<Micro, 64>, 2> MICROS = []()
{
  std::array<std::array<Micro, 64>, 2> m{};
  for ( std::size_t c = 0; c < 2; ++c )
    for ( std::size_t i = 0; i < 64; ++i )
      m[c][i] = micro( MICROCODES[c][i] );
  return m;
}();

// --- the datapath of arith.v, bit for bit ---------------------------------------------------

// srcdgen.v: the source operand of an instruction that does not read its source register.
std::uint32_t sourceConstant( std::uint32_t srcdat, std::uint32_t srcop, std::uint32_t programCount )
{
  switch ( srcdat )
  {
  case 1: // RORQ, MOVEQ
    return srcop;
  case 2: // indexed offset in long words, 0 is 32
    return srcop ? srcop * 4 : 128;
  case 3: // SHLQ: the field holds 32 - n, used as a negative (left) shift
    return srcop | 0xffffffe0u;
  case 4: // NEG, ABS, SAT16S, SAT32S, MIRROR
    return 0;
  case 5: // CMPQ, JR
    return static_cast<std::uint32_t>( signExtend5( srcop ) );
  case 6: // NOT
    return 0xffffffffu;
  case 7: // MOVE PC
    return programCount;
  case 8: // quick data, 0 is 32
    return srcop ? srcop : 32;
  case 9: // BTST, BSET
    return 1u << srcop;
  case 10: // BCLR
    return ~( 1u << srcop );
  default:
    return 0;
  }
}

struct AluOut
{
  std::uint32_t q;
  bool carry;
};

// alu32.v
AluOut alu32( std::uint32_t a, std::uint32_t b, bool carryFlag, std::uint32_t func, bool revSub, bool dst31 )
{
  bool const subtract = ( func == 7 && dst31 ) || ( ( func >> 1 ) == 1 );
  std::uint32_t const adda = a ^ ( subtract && revSub ? 0xffffffffu : 0u );
  std::uint32_t const addb = b ^ ( subtract && !revSub ? 0xffffffffu : 0u );
  bool const cint = ( func & 1 ) && !( func & 4 ) && carryFlag;
  bool const cin = cint != subtract;
  std::uint64_t const sum = static_cast<std::uint64_t>( adda ) + addb + ( cin ? 1u : 0u );
  bool const sel0 = ( func & 1 ) || !( func & 4 );
  bool const sel1 = ( func & 2 ) || !( func & 4 );
  std::uint32_t const q = sel1 ? ( sel0 ? static_cast<std::uint32_t>( sum ) : a ^ b ) : ( sel0 ? a | b : a & b );
  return { q, ( ( sum >> 32 ) != 0 ) != subtract };
}

// barrel32.v
std::uint32_t barrel32( std::uint32_t mux, std::uint32_t sft, std::uint32_t a )
{
  std::uint64_t const t = static_cast<std::uint64_t>( a ) << sft;
  std::uint32_t const f = static_cast<std::uint32_t>( t ) | static_cast<std::uint32_t>( t >> 32 );
  // rmask: bit sft and up (a right shift clears what rotated in); lmask: the bits below sft
  std::uint32_t const rmask = 0xffffffffu << sft;
  std::uint32_t const lmask = ( 1u << sft ) - 1u;
  bool const lsl = mux == 0;
  bool const sr = mux & 1;
  bool const asrSign = mux == 3 && ( a >> 31 );
  std::uint32_t const opt0 = ~( ( lsl ? lmask : 0u ) | ( sr ? rmask : 0u ) );
  std::uint32_t const opt1 = asrSign ? rmask : 0u;
  return ( f & opt0 ) | opt1;
}

// brlshift.v: shift `brld` by the signed amount in `srcdp`, right when positive
AluOut brlshift( std::uint32_t brlmux, std::uint32_t srcdp, std::uint32_t brld )
{
  std::uint32_t const srcdpc = ( 0u - srcdp ) & 31;
  bool const rotate = brlmux == 2;
  bool const s31 = srcdp >> 31;
  std::uint32_t const b5to30 = ( srcdp >> 5 ) & 0x03ffffff;
  bool const uflow = !( s31 || b5to30 == 0 );
  bool const b5t30h = b5to30 == 0x03ffffff;
  bool const shzero = ( srcdp & 31 ) == 0;
  bool const oflow = s31 && ( ( b5t30h && shzero ) || !b5t30h );
  bool const outflow = ( oflow || uflow ) && !rotate;
  std::uint32_t const shiftcnt = outflow ? 0u : srcdpc;
  bool const mux0t = !( !s31 && !rotate && !shzero );
  bool const mux0 = !( mux0t && !outflow );
  bool const ashrN = !( !s31 && brlmux == 3 );
  bool const mux1 = !( ashrN && !rotate );
  std::uint32_t const mux = ( mux1 ? 2u : 0u ) | ( mux0 ? 1u : 0u );
  return { barrel32( mux, shiftcnt, brld ), mux0 ? ( brld & 1 ) != 0 : ( brld >> 31 ) != 0 };
}

// j_saturate.v
std::uint32_t saturate( std::uint32_t d, bool sat32, std::uint32_t accumTop )
{
  bool const sat16 = !sat32;
  bool const d31 = d >> 31;
  std::uint32_t const b15to30 = ( d >> 15 ) & 0xffff;
  bool const pos16 = sat16 && !d31 && b15to30 != 0;
  bool const neg16 = sat16 && d31 && b15to30 != 0xffff;
  bool const a39 = ( accumTop >> 7 ) & 1;
  bool const a32to38any = ( accumTop & 0x7f ) != 0;
  bool const a32to38all = ( accumTop & 0x7f ) == 0x7f;
  bool const pos32 = sat32 && !a39 && ( d31 || a32to38any );
  bool const neg32 = sat32 && a39 && !( d31 && a32to38all );
  bool const unch = ( sat16 && !pos16 && !neg16 ) || ( sat32 && !pos32 && !neg32 );
  if ( unch )
    return d;
  bool const bit0to14 = pos32 || pos16;
  bool const bit15to30 = pos32 || neg16;
  bool const bit31 = neg16 || neg32;
  return ( bit0to14 ? 0x7fffu : 0u ) | ( bit15to30 ? 0x7fff8000u : 0u ) | ( bit31 ? 0x80000000u : 0u );
}

// saturate.v (Tom's): an unsigned value of 8, 16 or 24 bits from a signed one
std::uint32_t saturateUnsigned( std::uint32_t d, bool sixteen, bool twentyfour )
{
  if ( d >> 31 )
    return 0;
  bool const oflow24 = ( d >> 24 ) & 0x7f;
  bool const oflow16 = ( d >> 16 ) & 0xff;
  bool const oflow8 = ( d >> 8 ) & 0xff;
  std::uint32_t const hi = twentyfour ? ( oflow24 ? 0xffu : ( d >> 16 ) & 0xff ) : 0u;
  bool const midAll = ( oflow16 && !twentyfour ) || oflow24;
  std::uint32_t const mid = twentyfour || sixteen ? ( midAll ? 0xffu : ( d >> 8 ) & 0xff ) : 0u;
  bool const loAll = ( !sixteen && !twentyfour && oflow8 ) || ( oflow16 && sixteen ) || oflow24;
  std::uint32_t const lo = loAll ? 0xffu : d & 0xff;
  return hi << 16 | mid << 8 | lo;
}

// arith.v pack and unpack: the GPU's CRY pixel to and from its three fields spread out
std::uint32_t pack( std::uint32_t d )
{
  return ( d & 0xff ) | ( ( d >> 13 ) & 0xf ) << 8 | ( ( d >> 22 ) & 0xf ) << 12;
}

std::uint32_t unpack( std::uint32_t d )
{
  return ( d & 0xff ) | ( ( d >> 8 ) & 0xf ) << 13 | ( ( d >> 12 ) & 0xf ) << 22;
}

// arith.v mirror: the bits in reverse order
std::uint32_t mirror( std::uint32_t v )
{
  std::uint32_t r = 0;
  for ( int i = 0; i < 32; ++i )
    r |= ( ( v >> i ) & 1u ) << ( 31 - i );
  return r;
}

// arith.v normi: the position of the highest set bit, relative to bit 23
std::uint32_t normalize( std::uint32_t srcdp )
{
  std::uint32_t const topset = srcdp ? 1u << ( 31 - std::countl_zero( srcdp ) ) : 0u;
  std::uint32_t r = 0;
  r |= ( topset & 0x55555555u ) ? 1u : 0u;
  r |= ( topset & 0x66666666u ) ? 2u : 0u;
  r |= ( topset & 0x78787878u ) ? 4u : 0u;
  r |= ( topset & 0x807F807Fu ) ? 8u : 0u;
  r |= ( topset & 0x007FFF80u ) ? 16u : 0u;
  if ( !( topset & 0xFF800000u ) )
    r |= 0xffffffe0u;
  return r;
}

} // namespace

void Risc::setTrace( TraceSink* trace, std::string prefix )
{
  mTrace = trace;
  mTracePrefix = std::move( prefix );
}

void Risc::loadLocal( std::uint32_t address, std::span<std::uint8_t const> data )
{
  for ( std::size_t i = 0; i < data.size(); ++i )
  {
    std::uint32_t const a = address + static_cast<std::uint32_t>( i );
    if ( a - mRamBase < mRamSize )
    {
      mRam[a - mRamBase] = data[i];
    }
  }
}

// gpu_mem.v localaddr: the DSP's f18000 to f1ffff, the GPU's f02000 to f07fff
bool Risc::isLocal( std::uint32_t address ) const
{
  return mGpu ? ( address & 0xff8000u ) == 0xf00000u && ( address & 0x6000u ) != 0
              : ( address & 0xff8000u ) == 0xf18000u;
}

void Risc::setFlags( std::uint32_t value )
{
  writeIo( mIoBase + REG_FLAGS, value );
}

void Risc::setPc( std::uint32_t address )
{
  mFetchPc = ( address >> 1 ) & 0x7fffff;
}

void Risc::go()
{
  start( 0 );
}

// GO set again by another master once the processor has stopped (a run ended where its GO fell):
// it starts afresh, its instruction queue empty, nothing in its decode stage or its memory
// controller, as at its first GO; its registers, flags, RAM and PC are as they were left. (A node
// of the automaton the stop cut leaves the state at its entry, the store that cleared GO still
// pending in it.)
void Risc::restart( std::uint64_t cycle )
{
  mAccess = Access{};
  mXprog = false;
  mXpabort = false;
  mMemIdle = true;
  mComp1 = false;
  mComp2 = false;
  mLoading = false;
  mDrqt2 = false;
  mDatServ = false;
  mCompdwait = false;
  mIldPending = false;
  mInsdata = 0;
  mPfdata = 0;
  mQueueSize = 0;
  mProgServ = false;
  mExec = false;
  mVins = false;
  mImm1 = false;
  mImm2 = false;
  mInsrdyp = false;
  mJat2 = false;
  mJrt2 = false;
  mJrel = false;
  mDecode = 0;
  mDecoded = Decoded{};
  mIoWrite = false;
  mImmCount = 0;
  mOpcodeWord = 0;
  mIntser = false;
  mIcount = 0;
  mAwaitIdle = true;
  mAwait1 = false;
  mAwait2 = false;
  mGoFalls = UINT64_MAX;
  mPortStop = false;
  mStopAt = mLimit;
  start( cycle );
}

// The I2S interface runs on the DSP's clock whether the DSP runs or not: while it is stopped the
// interface is stepped here up to `cycle`, the rises of its interrupt traced (not latched).
void Risc::i2sCatchUp( std::uint64_t cycle )
{
  while ( mI2sAt < cycle )
  {
    if ( mI2s.rose() )
      emit( mI2sAt, "IRQ i2s" );
    std::uint64_t const n = std::min( mI2s.quiet(), cycle - mI2sAt );
    if ( n > 1 && !mI2s.rose() )
    {
      mI2s.advance( n );
      mI2sAt += n;
      continue;
    }
    if ( mI2s.stepping() )
      mI2s.step();
    else
      mI2s.skip();
    ++mI2sAt;
  }
}

void Risc::start( std::uint64_t cycle )
{
  i2sCatchUp( cycle );
  mGo = true;
  mStarted = true;
  mCycle = cycle;
  mIdle = true;
  mBus.attach( mMaster, cycle );
}

// gpu_cpu.v and gpu_mem.v ioserv: the write of a long word into the local space. While the
// processor is stopped: its local RAM, its registers, D_CTRL's GO starting it in the next cycle.
// While it runs the write is made as it is served, in the cycle after the one its memory controller
// gives up for it (slaveRequest).
void Risc::slaveWrite( std::uint64_t cycle, std::uint32_t address, std::uint32_t value )
{
  bool const halted = mStarted && ( !mGo || mGoFalls <= cycle );
  if ( !mGo )
    i2sCatchUp( cycle );
  if ( ( !mGo || halted ) && ( address & ~3u ) == mIoBase + REG_CTRL )
  {
    if ( value & 2 ) // cpu_int, whether the processor runs or not
    {
      if ( mGpu )
        mBus.gpuCpuInterrupt( cycle + 1 );
      else
        mBus.dspCpuInterrupt( cycle + 1 );
    }
    if ( value & 1 )
    {
      if ( halted )
        restart( cycle + 1 );
      else
        start( cycle + 1 );
    }
    return;
  }
  if ( mGpu && ( address & 0xffff00u ) == BLITTER_BASE ) // the blitter, on the GPU's local bus
  {
    mBus.blitterWrite( cycle, address, value );
    return;
  }
  mSlaveWriting = true;
  writeLong( address, value );
  mSlaveWriting = false;
}

void Risc::slaveRequest( std::uint64_t cycle )
{
  if ( !mGo )
    return;
  mServeAt = cycle + 1;
  if ( cycle >= mCycle )
    mIoreqs.push_back( cycle );
  else if ( cycle + 1 == mCycle )
  {
    // the cycle has just been made, the bus stepping it after: what it would have served is served
    // in none (datreqa, progreqa), the requests stand
    mDatServ = false;
    mProgServ = false;
  }
}

// A read by another master. Once the processor has stopped, CTRL shows GO clear and the instruction
// queue empty, as the prefetcher leaves it.
std::uint32_t Risc::slaveRead( std::uint64_t cycle, std::uint32_t address )
{
  std::uint32_t const reg = ( address & ~3u ) - mIoBase;
  if ( reg == REG_CTRL || reg == REG_FLAGS )
    for ( Status const& s : mStatus )
      if ( s.cycle == cycle )
        return reg == REG_CTRL ? s.ctrl : s.flags;
  std::uint32_t v = readLong( address );
  if ( cycle >= mGoFalls && ( address & ~3u ) == mIoBase + REG_CTRL )
    v &= ~( 1u | 3u << 22 );
  return v;
}

std::uint64_t Risc::cycles() const
{
  return mStopAt;
}

std::uint32_t Risc::reg( int index ) const
{
  return mRegs[static_cast<std::size_t>( index & 63 )];
}

void Risc::setReg( int index, std::uint32_t value )
{
  loopDirty();
  auto const r = static_cast<std::size_t>( index & 63 );
  // a write planned for a later cycle keeps its value from that cycle; until then the register
  // reads as the one before it
  mOld[r] = value;
  if ( mReady[r] <= mCycle )
    mRegs[r] = value;
}

void Risc::setAluFlags( std::uint32_t flags )
{
  loopDirty();
  // the flags due by now committed, those planned for later left to come
  static_cast<void>( flagsAt( mCycle ) );
  mFlags = flags & 7;
}

void Risc::debugStop( DebugStop stop )
{
  stop.cpu = mProbe->cpu();
  stop.exact = mDebugger->freezable;
  std::array<std::uint32_t, 64> registers{};
  for ( std::size_t i = 0; i < registers.size(); ++i )
    registers[i] = valueAt( static_cast<std::uint32_t>( i ), mCycle );
  stop.registers = registers;
  mDebugger->stop = std::move( stop );
}

bool Risc::breakHere()
{
  if ( mDebugger->stop || ( mProbe->breakpoints.empty() && !mProbe->step ) || mProbe->passAt == mCycle || !issuing() )
    return false;
  std::uint32_t const pc = programCount();
  bool const breaks = mProbe->breaksAt( pc );
  if ( !breaks && mProbe->stepPasses )
  {
    mProbe->stepPasses = false; // the instruction the step executes
    mProbe->passAt = mCycle;
    return false;
  }
  if ( !breaks && !mProbe->step )
    return false;
  debugStop(
      DebugStop{ .kind = breaks ? DebugStop::Kind::Breakpoint : DebugStop::Kind::Step, .pc = pc, .cycle = mCycle } );
  if ( !mDebugger->freezable )
    return false;
  mProbe->passAt = mCycle;
  return true;
}

void Risc::watchAccess( Access const& a )
{
  if ( mDebugger->stop )
    return;
  bool const store = isStore( a.op, mGpu );
  std::uint32_t const bytes = a.op == LOADB || a.op == STOREB               ? 1u
                              : a.op == LOADW || a.op == STOREW             ? 2u
                              : mGpu && ( a.op == LOADP || a.op == STOREP ) ? 8u
                                                                            : 4u;
  std::uint32_t const address = a.address & 0xffffffu & ~( bytes - 1 );
  if ( !mProbe->watches( address, bytes, store ) )
    return;
  DebugStop stop{
    .kind = DebugStop::Kind::Watchpoint, .pc = a.pc, .cycle = mCycle, .address = address, .write = store, .bytes = bytes
  };
  if ( store )
    stop.value = bytes == 1 ? a.data & 0xffu : bytes == 2 ? a.data & 0xffffu : a.data;
  debugStop( std::move( stop ) );
}

std::uint32_t Risc::pc() const
{
  return programCount();
}

std::uint32_t Risc::flagsRegister()
{
  return readIo( mIoBase + REG_FLAGS );
}

std::uint32_t Risc::controlRegister()
{
  return readIo( mIoBase + REG_CTRL );
}

std::uint8_t Risc::localByte( std::uint32_t address ) const
{
  return holdsLocal( address ) ? mRam[address - mRamBase] : 0;
}

void Risc::setLocalByte( std::uint32_t address, std::uint8_t value )
{
  if ( !holdsLocal( address ) )
    return;
  std::uint32_t const a = address - mRamBase;
  if ( mCodeMark[a >> 2] )
    flushAutomaton();
  if ( mRam[a] != value )
    loopDirty();
  mRam[a] = value;
}

std::uint16_t Risc::fetch16( std::uint32_t address ) const
{
  std::uint32_t const a = ( address - mRamBase ) & ( mRamSize - 1 );
  return static_cast<std::uint16_t>( mRam[a] << 8 | mRam[a + 1] );
}

Risc::Decoded Risc::decode( std::uint16_t word, std::uint32_t bank )
{
  Decoded d{};
  d.op = static_cast<std::uint8_t>( word >> 10 );
  d.src = static_cast<std::uint8_t>( ( word >> 5 ) & 31 );
  d.dst = static_cast<std::uint8_t>( word & 31 );
  d.imm = d.src ? d.src : 32u;
  d.s = d.src + bank;
  d.d = d.dst + bank;
  d.alt = bank ^ 32u;
  d.srcReg = d.op == MOVEFA ? d.src + d.alt : d.s;
  d.dstReg = d.op == MOVETA ? d.dst + d.alt : d.d;
  if ( d.op == LOAD14N || d.op == LOAD14R || d.op == STORE14N || d.op == STORE14R )
    d.dstReg = bank + 14;
  else if ( d.op == LOAD15N || d.op == LOAD15R || d.op == STORE15N || d.op == STORE15R )
    d.dstReg = bank + 15;
  return d;
}

// pc.v: the address of the instruction at the head of the queue, less one word
std::uint32_t Risc::programCount() const
{
  return ( ( mFetchPc - mQueueSize - 1 ) & 0x7fffff ) << 1;
}

bool Risc::condition( std::uint32_t cc )
{
  std::uint32_t const flags = flagsAt( mCycle );
  bool const z = flags & 1;
  if ( ( cc & 1 ) && z )
    return false;
  if ( ( cc & 2 ) && !z )
    return false;
  bool const other = ( flags & ( ( cc & 16 ) ? 4u : 2u ) ) != 0;
  if ( ( cc & 4 ) && other )
    return false;
  if ( ( cc & 8 ) && !other )
    return false;
  return true;
}

// The bits a read of D_FLAGS or D_CTRL takes from the pipeline (ins_exec.v, dbgrd): the decode
// stage's opcode bits 5 to 2, the queue size, sbwait, div_activei, jump_ena, as they are before the
// cycle's instruction issues. external, gate_active, sysser and intser are clear on a local read.
std::uint32_t Risc::debugBits()
{
  bool const divActivei = mDivActive && mDivCount != 0; // a DIV issued in the cycle is added after
  return static_cast<std::uint32_t>( mOpcodeWord >> 12 ) << 18 | mQueueSize << 22 |
         ( canIssue( decode( mOpcodeWord, mBank ), mVins && mExec && !mCompdwait, mCycle ) ? 0u : 1u << 25 ) |
         ( divActivei ? 1u << 26 : 0u ) | ( mRequest.valid ? 1u << 27 : 0u ) | ( mGateActive ? 1u << 28 : 0u ) |
         ( condition( mOpcodeWord & 31u ) ? 1u << 29 : 0u ) | ( mSysIdle ? 0u : 1u << 30 ) |
         ( mIntser ? 1u << 31 : 0u );
}

std::uint32_t Risc::readIo( std::uint32_t address )
{
  loopDirty();
  switch ( ( address & ~3u ) - mIoBase )
  {
  case REG_FLAGS:
    // bits 16 and 17: the DSP's sixth interrupt enable, the GPU's decode stage opcode bits 1 and 0
    return flagsAt( mCycle ) | ( mImask ? IMASK : 0u ) | ( mIntEna & 31 ) << 4 | ( mRegpage ? REGPAGE : 0u ) |
           ( mDmaen ? 0x8000u : 0u ) | ( mGpu ? ( mOpcodeWord >> 10 & 3u ) << 16 : ( mIntEna >> 5 ) << 16 ) |
           debugBits();
  case REG_PC:
    return programCount();
  case REG_CTRL:
    return ( mGo ? 1u : 0u ) | ( mIlatch & 31 ) << 6 | 0x2000u |
           ( mGpu ? ( mOpcodeWord >> 10 & 3u ) << 16 : ( mIlatch >> 5 ) << 16 ) | debugBits();
  case REG_REMAIN:
    // the remainder as it stands: the step of this cycle comes after the read
    divideCatchUp( mCycle ? mCycle - 1 : 0 );
    return mDividendHi;
  case REG_MOD:
    return mGpu ? mBus.hidata() : 0u; // G_HIDATA reads back (gateway.v hirdata, hidrd)
  case REG_MACHI:
    // arith.v accum: the accumulator's top byte, sign extended
    if ( mGpu )
      return 0;
    return ( mAccTop & 0x80 ) ? 0xffffff00u | mAccTop : mAccTop;
  default:
    return 0;
  }
}

void Risc::writeIo( std::uint32_t address, std::uint32_t value )
{
  loopDirty();
  mIoWrite = true;
  std::uint32_t const reg = ( address & ~3u ) - mIoBase;
  if ( mGpu && reg >= REG_MACHI )
    return;
  switch ( reg )
  {
  case REG_FLAGS:
    if ( mGo )
      loadFlags( mCycle + 1, value & 7, true );
    else
      mFlags = value & 7;
    if ( !( value & IMASK ) )
      mImaskClear = true;
    mRegpage = value & REGPAGE;
    if ( !mGo )
      mBank = mRegpage && !mImask ? 32u : 0u;
    // the sixth interrupt is the DSP's (interrupt.v int_ena[5], JERRY)
    mIntEnaNext = ( ( value >> 4 ) & 31 ) | ( mGpu ? 0u : ( ( value >> 16 ) & 1 ) << 5 );
    mIlatchClear = ( ( value >> 9 ) & 31 ) | ( ( value >> 17 ) & 1 ) << 5;
    if ( !mGo )
    {
      mIntEna = mIntEnaNext;
      mIlatch &= ~mIlatchClear;
      mIlatchClear = 0;
      mImask = mImask && !mImaskClear;
      mImaskClear = false;
    }
    mDmaen = value & 0x8000;
    mBus.setDmaen( mMaster, mDmaen );
    break;
  case REG_MTXC:
    mMwidth = value & 15;
    mMaddw = ( value >> 4 ) & 1;
    break;
  case REG_MTXA:
    mMtxAddr = ( value >> 2 ) & 0x3ff;
    break;
  case REG_PC:
    mFetchPc = ( value >> 1 ) & 0x7fffff;
    break;
  case REG_CTRL:
    if ( !( value & 1 ) && mGo )
    {
      if ( mCycle + STOP_DELAY < mGoFalls )
        mPortStop = mSlaveWriting;
      mGoFalls = std::min( mGoFalls, mCycle + STOP_DELAY );
      mStopAt = std::min( mStopAt, mGoFalls );
    }
    mCpuIrqNext = ( value & 4 ) != 0; // gpu_ctrl.v gpu_intt: interrupt 0 from the next cycle
    if ( value & 2 )                  // cpu_int: the 68000's, a cycle after the write
    {
      if ( mGpu )
        mBus.gpuCpuInterrupt( mCycle + 1 );
      else
        mBus.dspCpuInterrupt( mCycle + 1 );
    }
    break;
  case REG_LTXD:
    mI2s.writeLtxd( static_cast<std::uint16_t>( value ) );
    break;
  case REG_RTXD:
    mI2s.writeRtxd( static_cast<std::uint16_t>( value ) );
    break;
  case REG_SCLK:
    mI2s.writeSclk( static_cast<std::uint16_t>( value ) );
    break;
  case REG_SMODE:
    mI2s.writeSmode( static_cast<std::uint16_t>( value ) );
    break;
  case REG_MOD:
    if ( mGpu )
      mBus.setHidata( value ); // G_HIDATA, the gateway's hirdata (hidwr)
    else
      mMod = value;
    break;
  case REG_REMAIN:
    mDivOffsetNext = value & 1;
    break;
  default:
    break;
  }
}

std::uint32_t Risc::readLong( std::uint32_t address )
{
  if ( address - mRamBase < mRamSize )
  {
    std::uint32_t const a = ( address - mRamBase ) & ~3u;
    return static_cast<std::uint32_t>( mRam[a] ) << 24 | static_cast<std::uint32_t>( mRam[a + 1] ) << 16 |
           static_cast<std::uint32_t>( mRam[a + 2] ) << 8 | mRam[a + 3];
  }
  if ( address >= mIoBase && address < mIoEnd )
    return readIo( address );
  if ( mGpu && ( address & 0xffff00u ) == BLITTER_BASE )
  {
    loopDirty();
    return mBus.blitterRead( address );
  }
  if ( !mGpu && address - ROM_BASE < 0x1000 ) // a word, its sign in the upper half (gpu_data_in_15)
    return static_cast<std::uint32_t>( static_cast<std::int16_t>( WAVE_ROM[( address - ROM_BASE ) >> 2] ) );
  return 0; // the rest of the local space reads nothing; outside it the gateway serves (access)
}

void Risc::writeLong( std::uint32_t address, std::uint32_t value )
{
  if ( address - mRamBase < mRamSize )
  {
    std::uint32_t const a = ( address - mRamBase ) & ~3u;
    if ( mCodeMark[a >> 2] )
      flushAutomaton();
    if ( mRam[a] != static_cast<std::uint8_t>( value >> 24 ) ||
         mRam[a + 1] != static_cast<std::uint8_t>( value >> 16 ) ||
         mRam[a + 2] != static_cast<std::uint8_t>( value >> 8 ) || mRam[a + 3] != static_cast<std::uint8_t>( value ) )
      loopDirty();
    mRam[a] = static_cast<std::uint8_t>( value >> 24 );
    mRam[a + 1] = static_cast<std::uint8_t>( value >> 16 );
    mRam[a + 2] = static_cast<std::uint8_t>( value >> 8 );
    mRam[a + 3] = static_cast<std::uint8_t>( value );
    return;
  }
  if ( address >= mIoBase && address < mIoEnd )
  {
    writeIo( address, value );
    return;
  }
  if ( mGpu && ( address & 0xffff00u ) == BLITTER_BASE )
  {
    loopDirty();
    mBus.blitterWrite( mCycle, address, value );
  }
}

void Risc::emit( std::uint64_t cycle, std::string_view text, std::uint64_t end )
{
  if ( mTrace && cycle < ( end != 0 ? end : mStopAt ) )
  {
    if ( mTracePrefix.empty() )
      mTraceBuffer.emplace_back( cycle, text );
    else
      mTraceBuffer.emplace_back( cycle, mTracePrefix + std::string( text ) );
  }
}

// Hands the sink the events before `before`, in the order they were made.
void Risc::flushTrace( std::uint64_t before )
{
  if ( !mTrace )
    return;
  auto const keep = std::stable_partition(
      mTraceBuffer.begin(), mTraceBuffer.end(), [before]( auto const& e ) { return e.first < before; } );
  for ( auto it = mTraceBuffer.begin(); it != keep; ++it )
    mTrace->event( it->first, it->second );
  mTraceBuffer.erase( mTraceBuffer.begin(), keep );
}

Risc::WriteSlot& Risc::slotAt( std::uint64_t cycle )
{
  WriteSlot& slot = mWrites[cycle % WRITE_SLOTS];
  if ( slot.cycle != cycle )
  {
    slot.cycle = cycle;
    slot.count = 0;
  }
  return slot;
}

Risc::WriteSlot const* Risc::writesAt( std::uint64_t cycle ) const
{
  WriteSlot const& slot = mWrites[cycle % WRITE_SLOTS];
  return slot.cycle == cycle ? &slot : nullptr;
}

void Risc::writeReg( std::uint32_t globalReg, std::uint32_t value, std::uint64_t at, std::uint8_t priority )
{
  WriteSlot& slot = mWrites[at % WRITE_SLOTS];
  if ( slot.cycle != at )
  {
    slot.cycle = at;
    slot.count = 0;
  }
  // Two writes of one register in one cycle: the destination port takes the one of the higher
  // priority and the register RAM ignores the source port's (rd64x32.v, dupwrite). Once GO has
  // fallen nothing issues, but what issued before writes its result in the two cycles after
  // (m68k_risc_stop); the divider's quotient is not written (div).
  std::uint64_t const end = priority == DIV_PRIORITY ? mStopAt : writesEnd();
  bool lost = at >= end;
  if ( slot.count )
    lost = lost || ( slot.reg[0] == globalReg && slot.priority[0] < priority ) ||
           ( slot.count == 2 && slot.reg[1] == globalReg && slot.priority[1] < priority );
  if ( slot.count < 2 )
  {
    slot.reg[slot.count] = static_cast<std::uint8_t>( globalReg );
    slot.priority[slot.count] = priority;
    slot.value[slot.count] = value;
    ++slot.count;
  }
  // Writes are planned out of their order in time (a load's after a later ALU result's): the last in
  // time is the register's value, one before it the value it replaces.
  std::uint64_t const ready = mReady[globalReg];
  std::uint32_t const current = mRegs[globalReg];
  bool const later = !lost && at >= ready;
  mOld[globalReg] = !lost && at < ready ? value : later && at > ready ? current : mOld[globalReg];
  mRegs[globalReg] = later ? value : current;
  mReady[globalReg] = at > ready ? at : ready;
  if ( mTrace )
  {
    char text[32];
    std::snprintf( text, sizeof text, "W%02u%c:%08x", globalReg & 31, globalReg >= 32 ? '"' : '\'', value );
    emit( at, text, end );
  }
}

void Risc::writeFlags( std::uint32_t flags )
{
  mFlagsSet = mIssue + 1;
  loadFlags( mIssue + 2, flags, false );
}

// A value the flags take from `at` on, kept in order of time, a write after flags loaded in the
// same cycle.
void Risc::loadFlags( std::uint64_t at, std::uint32_t value, bool write )
{
  if ( mFlagsPendingCount == mFlagsPending.size() )
    flagsAt( mFlagsPending[0].at );
  std::uint32_t i = mFlagsPendingCount++;
  while ( i > 0 && ( mFlagsPending[i - 1].at > at || ( mFlagsPending[i - 1].at == at && mFlagsPending[i - 1].write ) ) )
  {
    mFlagsPending[i] = mFlagsPending[i - 1];
    --i;
  }
  mFlagsPending[i] = FlagsPending{ .at = at, .value = value, .write = write };
}

// The flags visible in `cycle`: what is due by then is committed, the last value of a cycle shown.
std::uint32_t Risc::commitFlags( std::uint64_t cycle )
{
  if ( mFlagsPendingCount == 1 ) // the usual: the flags of the instruction before
  {
    FlagsPending const& p = mFlagsPending[0];
    if ( p.at > cycle )
      return mFlags;
    if ( mTrace && p.value != mFlags )
    {
      char text[8] = { 'F', ' ', ( p.value & 4 ) ? 'N' : '-', ( p.value & 2 ) ? 'C' : '-', ( p.value & 1 ) ? 'Z' : '-',
                       0 };
      emit( p.at, text );
    }
    mFlags = p.value;
    mFlagsPendingCount = 0;
    return mFlags;
  }
  std::uint32_t i = 0;
  while ( i < mFlagsPendingCount && mFlagsPending[i].at <= cycle )
  {
    std::uint64_t const at = mFlagsPending[i].at;
    std::uint32_t next = mFlagsPending[i].value;
    for ( ++i; i < mFlagsPendingCount && mFlagsPending[i].at == at; ++i )
      next = mFlagsPending[i].value;
    if ( mTrace && next != mFlags )
    {
      char text[8] = { 'F', ' ', ( next & 4 ) ? 'N' : '-', ( next & 2 ) ? 'C' : '-', ( next & 1 ) ? 'Z' : '-', 0 };
      emit( at, text );
    }
    mFlags = next;
  }
  if ( i )
  {
    for ( std::uint32_t j = i; j < mFlagsPendingCount; ++j )
      mFlagsPending[j - i] = mFlagsPending[j];
    mFlagsPendingCount -= i;
  }
  return mFlags;
}

// sboard.v: whether the instruction in the decode stage may issue in this cycle (no sbwait). The
// scoreboard compares the decode stage's microword even when it holds no valid instruction; only the
// terms gated by insexe (memrw, bothen) then drop out, and an indexed mode's destination operand is
// its destination field again, not R14 or R15 (ins_exec.v danwsel).
bool Risc::canIssue( Decoded const& in, bool insexe, std::uint64_t t ) const
{
  Micro const& mc = MICROS[mGpu][in.op];
  std::uint32_t const srcReg = in.srcReg;
  std::uint32_t const dstReg = mc.precomp && !insexe ? in.d : in.dstReg;
  // aludwait and friends: an operand still to be written
  if ( mc.srcRead && mReady[srcReg] > t )
    return false;
  if ( mc.dstRead && mReady[dstReg] > t )
    return false;
  // the indexed modes compare their source field as a register, even as an offset (srca_reg_ena)
  if ( mc.precomp && mReady[srcReg] > t )
    return false;
  // ldwait: while a load or store is under way, no other one, and no instruction that reads its
  // register
  if ( !mMemIdle && ( ( mc.memory && insexe ) || ( mc.dstRead && dstReg == mAccess.reg ) ||
                      ( ( mc.srcRead || mc.precomp ) && srcReg == mAccess.reg ) ) )
    return false;
  // xlddwait: an operand an external load will write; mbusywait: no load or store while the
  // gateway is busy or two external loads are under way, no store while any is
  if ( ( mOneld || mTwold ) &&
       ( ( mc.dstRead && dstReg == mXlddst ) || ( ( mc.srcRead || mc.precomp ) && srcReg == mXlddst ) ) )
    return false;
  if ( mTwold && ( ( mc.dstRead && dstReg == mXlddst2 ) || ( ( mc.srcRead || mc.precomp ) && srcReg == mXlddst2 ) ) )
    return false;
  if ( ( mc.memory && insexe && ( mGateActive || mTwoldi ) ) || ( mc.store && ( mOneldi || mTwoldi ) ) )
    return false;
  // divdwait: an operand the divider will write; diviwait: a DIV while the divider is busy
  if ( mDivActive && ( ( mc.dstRead && dstReg == mDivReg ) || ( ( mc.srcRead || mc.precomp ) && srcReg == mDivReg ) ) )
    return false;
  if ( in.op == DIV && ( mDivActive || mDivPending ) )
    return false;
  // flagwait: the flags of an instruction issued in the cycle before are not ready
  bool const conditionalJump = ( in.op == JUMP || in.op == JR ) && in.dst != 0;
  if ( ( mc.flagDepend || conditionalJump ) && mFlagsSet + 1 > t )
    return false;
  // wbwait: no issue in a cycle with two register writes (mult_wback), nor with one when the
  // instruction needs both ports and the write is to neither of its registers (bothen, wbdeq, wbseq)
  if ( WriteSlot const* slot = writesAt( t ) )
  {
    if ( slot->count >= 2 )
      return false;
    bool const bothPorts = insexe && mc.srcRead && ( mc.dstRead || mc.movWrite );
    if ( bothPorts && slot->count == 1 && slot->reg[0] != srcReg && slot->reg[0] != dstReg )
      return false;
  }
  return true;
}

// The datapath of one instruction, the microword a constant of the template: the compiler folds
// every branch the opcode decides, which the branch predictor cannot learn over a long run of
// instructions (0007). The generic `execute` dispatches through EXEC.
template <std::uint8_t OP, bool GPU>
Risc::Executed Risc::executeOp( Decoded const& in, std::uint32_t pc )
{
  std::uint32_t const s = in.s;
  std::uint32_t const d = in.d;
  std::uint32_t const alt = in.alt;
  std::uint32_t const srcReg = in.srcReg;
  std::uint32_t const dstReg = in.dstReg;
  constexpr std::uint32_t mw = MICROCODES[GPU ? 1 : 0][OP];
  constexpr Micro mc = micro( mw );
  Executed done{ .memory = mc.memory, .precomp = mc.precomp, .store = mc.store };

  if ( mTrace )
  {
    char text[80];
    std::snprintf( text, sizeof text, "X %06x", pc );
    emit( mIssue, text );
    std::snprintf( text, sizeof text, "I %02u r%u,r%u s=%08x d=%08x", OP, srcReg, dstReg, mRegs[srcReg], mRegs[d] );
    emit( mIssue, text );
  }
  if ( mProbe )
    mProbe->executed( pc, mIssue );

  std::uint32_t const vs = mRegs[s];
  std::uint32_t const vd = mRegs[d];
  done.srcdp = mc.srcRead ? mRegs[srcReg] : sourceConstant( ( mw >> 13 ) & 15, in.src, pc );
  // an IMACN issued in the cycle before has its product accumulated in this instruction's ALU cycle
  // (macop_pp): the ALU adds resreg and mulqp, by this instruction's function and result select
  bool const macpp = mMacAt == mIssue + 1;
  bool const resld = ( mw >> 12 ) & 1;
  bool const macop = ( mw >> 6 ) & 1;
  if ( mc.aluWrite || mc.flagLoad || resld || macop || macpp )
  {
    // arith.v: the operands, the units, the result multiplexer and the flags
    std::uint32_t const srcdat = ( mw >> 13 ) & 15;
    // UNPACK is PACK with the source field odd (ins_exec.v unpack)
    std::uint32_t const ressel = ( mw & 7 ) | ( GPU && OP == PACK && ( in.src & 1 ) ? 1u : 0u );
    std::uint32_t const brlmux = ( mw >> 3 ) & 3;
    bool const multsign = ( mw >> 4 ) & 1;
    bool const sat32 = ( mw >> 5 ) & 1;
    std::uint32_t alufunc = ( mw >> 7 ) & 7;
    std::uint32_t const srcdp = mc.srcRead ? mRegs[srcReg] : sourceConstant( srcdat, in.src, pc );
    // A destination operand the instruction does not read (NORMI) is what the destination port
    // holds: the value written in this cycle if there is one (registers.v, dstdsel), else the
    // register as it is now.
    // The indexed modes read R14 or R15 there, the ALU adding the offset (resld: resreg then holds
    // the address).
    std::uint32_t dstdp = mRegs[dstReg];
    if ( !mc.dstRead )
    {
      WriteSlot const& now = slotAt( mIssue );
      if ( now.count )
        dstdp = now.value[now.count == 2 && now.priority[1] < now.priority[0] ? 1 : 0];
      else if ( mReady[dstReg] > mIssue )
        dstdp = mOld[dstReg];
    }
    // MMULT's inserted words take the matrix word as their destination operand (mtx_doverp)
    if ( mMtxIssue )
      dstdp = mMtxData;
    bool const mantmode = ressel == 4;
    if ( mantmode && ( srcdp >> 31 ) )
      alufunc |= 2;
    bool const revSub = ( alufunc == 2 || alufunc == 7 ) && srcdat == 4;
    // the carry the ALU takes in the cycle after issue (ADDC, SUBC): a write to D_FLAGS served now is
    // seen
    bool const carry = mc.flagDepend && ( flagsAt( mIssue + 1 ) & 2 ) != 0;
    std::uint32_t const alua = mantmode ? 0u : macpp ? mResult : dstdp;
    std::uint32_t const alub = mantmode ? ( srcdp & 0x7fffffu ) | 0x800000u : macpp ? mMacProduct : srcdp;
    // only the units whose output is taken: the result select, the carry, the accumulator
    bool const needAlu = ressel == 0 || ressel == 4 || ressel == 6 || ( mc.flagLoad && !( ressel & 1 ) ) || macpp;
    bool const needBrl = ressel == 1 || ( mc.flagLoad && ( ressel & 1 ) );
    bool const needMul = ressel == 3 || macop;
    AluOut const a = needAlu ? alu32( alua, alub, carry, alufunc, revSub, ( dstdp >> 31 ) != 0 ) : AluOut{};
    AluOut const b = needBrl ? brlshift( brlmux, srcdp, dstdp ) : AluOut{};
    // MMULT's inserted words: the vector's half (multselp), the matrix word (mtx_doverp)
    std::uint32_t const mula = mMultselIssue ? srcdp >> 16 : srcdp;
    std::uint32_t const mulb = dstdp;
    std::uint32_t const mulq =
        !needMul   ? 0u
        : multsign ? static_cast<std::uint32_t>( static_cast<std::int16_t>( mula ) * static_cast<std::int16_t>( mulb ) )
                   : ( mula & 0xffff ) * ( mulb & 0xffff );
    std::uint32_t resd = 0;
    switch ( ressel )
    {
    case 0:
    case 4:
      resd = a.q;
      break;
    case 1:
      resd = b.q;
      break;
    case 2:
      resd = GPU ? saturateUnsigned( dstdp, sat32, ( mw >> 26 ) & 1 ) : saturate( dstdp, sat32, mAccTop );
      break;
    case 3:
      resd = mulq;
      break;
    case 5:
      resd = normalize( srcdp );
      break;
    case 6:
      resd = GPU ? pack( dstdp ) : ( mMod & dstdp ) | ( ~mMod & a.q );
      break;
    default:
      resd = GPU ? unpack( dstdp ) : mirror( dstdp );
      break;
    }
    if ( resld || macpp ) // resldt
      mResult = resd;
    if ( resld && ressel == 3 ) // accuminit
      mAccTop = macpp || !( mulq >> 31 ) ? 0u : 0xffu;
    else if ( macpp ) // accuma: the top byte takes the product's sign and the adder's carry
      mAccTop = ( mAccTop + ( ( mMacProduct >> 31 ) ? 0xffu : 0u ) + ( a.carry ? 1u : 0u ) ) & 0xff;
    if ( macop )
    {
      mMacAt = mIssue + 2;
      mMacProduct = mulq;
    }
    if ( mc.flagLoad )
    {
      bool const c = ( ressel & 1 ) ? b.carry : a.carry;
      writeFlags( ( resd == 0 ? 1u : 0u ) | ( c ? 2u : 0u ) | ( ( resd >> 31 ) ? 4u : 0u ) );
    }
    // the register file is written from resreg (sboard.v dwsel): RESMAC, which does not load it,
    // writes what it holds, the accumulated sum right after an IMACN
    if ( mc.aluWrite )
      writeReg( d, mResult, mIssue + ALU_LATENCY, ALU_PRIORITY );
  }
  switch ( OP )
  {
  case DIV:
    mDivStart = true;
    mDivSrc = vs;
    mDivDst = vd;
    mDivReg = d;
    break;
  case MOVE:
    writeReg( d, vs, mIssue + MOVE_LATENCY, MOV_PRIORITY );
    break;
  case MOVEQ:
    writeReg( d, in.src, mIssue + MOVE_LATENCY, MOV_PRIORITY );
    break;
  case MOVETA:
    writeReg( in.dst + alt, vs, mIssue + MOVE_LATENCY, MOV_PRIORITY );
    break;
  case MOVEFA:
    writeReg( d, mRegs[in.src + alt], mIssue + MOVE_LATENCY, MOV_PRIORITY );
    break;
  case MOVEI:
    // the data words follow through the queue (execon.v, imm1 and imm2)
    mImmCount = 2;
    mImmDst = d;
    done.immld = true;
    break;
  case MOVEPC:
    writeReg( d, pc, mIssue + MOVE_LATENCY, MOV_PRIORITY );
    break;
  case LOADB:
  case LOADW:
  case LOAD:
  case STOREB:
  case STOREW:
  case STORE:
    mAccess = Access{ .op = OP, .address = vs, .data = vd, .reg = in.d, .pc = pc };
    break;
  case LOADP: // the GPU's (the DSP's SAT32S, MIRROR are done above); the local space moves a long word
  case STOREP:
    if ( GPU )
      mAccess = Access{ .op = OP, .address = vs, .data = vd, .reg = in.d, .pc = pc };
    break;
  case LOAD14N:
  case LOAD15N:
  case STORE14N:
  case STORE15N:
  case LOAD14R:
  case LOAD15R:
  case STORE14R:
  case STORE15R:
    // The ALU adds the base and the offset in the cycle after issue, into resreg, the address then
    // (ins_exec.v dataddr = result): right after an IMACN it is the accumulated sum instead. A store
    // reads its data in that cycle.
    mAccess = Access{ .op = OP, .address = mResult, .data = 0, .reg = in.d, .pc = pc };
    break;
  case JUMP:
    if ( condition( in.dst ) )
    {
      done.jumpAbs = true;
      mJumpValue = vs;
    }
    break;
  case JR:
    if ( condition( in.dst ) )
      done.jumpRel = true;
    break;
  case NOP:
  default:
    break;
  }
  return done;
}

template <std::uint8_t OP, bool GPU>
Risc::Executed Risc::thunk( Risc& core, Decoded const& in, std::uint32_t pc )
{
  return core.executeOp<OP, GPU>( in, pc );
}

std::array<std::array<Risc::Exec, 64>, 2> const Risc::EXEC = { execTable<false>( std::make_index_sequence<64>{} ),
                                                               execTable<true>( std::make_index_sequence<64>{} ) };

Risc::Executed Risc::execute( Decoded const& in, std::uint32_t pc )
{
  return EXEC[mGpu][in.op]( *this, in, pc );
}

// Simple nodes replayed as a chain, until a node without the variant for the I2S line's next rise
// (`current`, for `advance` to record), a node that is not simple, or the end; the next node is
// reached by a tail call so that each opcode's function has a jump site of its own. Where the
// compiler cannot promise the tail call, the node is returned and the caller calls it.
template <std::uint8_t OP, bool GPU>
Risc::Node const* Risc::chain( Risc& core, Node const* node )
{
  // a timer requesting within the span: the node's variant is found by its whole outcome
  if ( core.mCycle >= core.mTimerNear )
  {
    std::uint64_t const next = core.mTimers.nextRequest( core.mCycle );
    core.mTimerNear = next == UINT64_MAX ? UINT64_MAX : next - std::min<std::uint64_t>( next, MAX_SPAN - 1 );
    if ( core.mCycle >= core.mTimerNear )
    {
      core.mTimerNear = 0;
      core.mAuto->current = const_cast<Node*>( node );
      return nullptr;
    }
  }
  Variant const* variant = node->idle;
  if ( !core.mI2s.idle() )
  {
    std::uint64_t const rise = core.mI2s.interruptChangeIn( core.mCycle );
    variant = node->byRise[rise < MAX_SPAN ? rise : MAX_SPAN];
  }
  if ( !variant || core.objectsWithin( variant->span ) )
  {
    core.mAuto->current = const_cast<Node*>( node );
    return nullptr;
  }
  Variant const& v = *variant;
  if ( v.bare )
  {
    core.mIssue = core.mCycle;
    core.executeOp<OP, GPU>( node->decoded, node->pc );
    core.mCycle += v.span;
    core.mI2s.advance( v.span );
  }
  else
    core.replay( *node, v );
  Node const* next = v.next;
  if ( !next || !next->simple || core.mCycle >= core.mStopAt || ( core.mBusBusy && !next->busSafe ) )
  {
    core.mAuto->current = const_cast<Node*>( next );
    return nullptr;
  }
#if __has_cpp_attribute( clang::musttail )
  [[clang::musttail]] return next->chain( core, next );
#else
  // no tail call is promised here (GCC 14, MSVC): the caller's loop calls the next node
  return next;
#endif
}

std::array<std::array<Risc::Chain, 64>, 2> const Risc::CHAIN = { chainTable<false>( std::make_index_sequence<64>{} ),
                                                                 chainTable<true>( std::make_index_sequence<64>{} ) };

// A register as the register file's ports read it in `cycle`: a write of that cycle is seen.
std::uint32_t Risc::valueAt( std::uint32_t globalReg, std::uint64_t cycle ) const
{
  return mReady[globalReg] <= cycle ? mRegs[globalReg] : mOld[globalReg];
}

// The load or store served in this cycle (datserv); a loaded value is written back from the next.
// The local space is 32 bits wide: a byte or word access to it reads or writes the whole long word,
// a store the whole register (random_narrow). Outside it (external) the gateway takes the access;
// a load is written back when the main bus brings its data (del_xld).
void Risc::access()
{
  Access const& a = mAccess;
  if ( mProbe && !mProbe->watchpoints.empty() )
    watchAccess( a );
  bool const local = isLocal( a.address );
  bool const store = isStore( a.op, mGpu );
  if ( !local )
  {
    loopDirty();
    mRequest.valid = true;
    mRequest.write = store;
    mRequest.address = a.address & 0xffffff;
    mRequest.size = a.op == LOADB || a.op == STOREB               ? 0u
                    : a.op == LOADW || a.op == STOREW             ? 1u
                    : mGpu && ( a.op == LOADP || a.op == STOREP ) ? 3u // a phrase, G_HIDATA the other half
                                                                  : 2u;
    mRequest.data = a.data;
    mRequest.pc = a.pc;
    return;
  }
  if ( store )
  {
    writeLong( a.address, a.data );
    return;
  }
  mIldValue = readLong( a.address );
  mIldPending = true;
}

// interrupt.v ins: the sequence an interrupt inserts, the vector after the highest latch set.
//   subqt #4,r31 / move pc,r30 / store r30,(r31) / movei #$f1b000+16*n,r30 / jump (r30) / nop
std::uint16_t Risc::insertedWord( std::uint32_t index ) const
{
  static constexpr std::array<std::uint16_t, 8> INS = {
    0x1c9f, 0xcc1e, 0xbffe, 0x981e, 0xb000, 0x00f1, 0xd3c0, 0xe400
  };
  // the vectors: the DSP's at $F1B000, the GPU's at $F03000
  if ( index == 5 )
    return mGpu ? 0x00f0 : 0x00f1;
  if ( index != 4 )
    return INS[index];
  std::uint32_t const n = mIlatch ? 31u - static_cast<std::uint32_t>( std::countl_zero( mIlatch ) ) : 0u;
  return static_cast<std::uint16_t>( ( mGpu ? 0x3000u : 0xb000u ) | n << 4 );
}

// macop_pp in a cycle no instruction issued before: the ALU still adds resreg and mulqp, by the
// function of the microword the decode stage held (arith.v alufnc, resselp).
void Risc::accumulate( std::uint16_t word )
{
  std::uint32_t const mw = MICROCODE[word >> 10];
  AluOut const a = alu32( mResult, mMacProduct, ( flagsAt( mCycle ) & 2 ) != 0, ( mw >> 7 ) & 7, false, false );
  mResult = a.q;
  mAccTop = ( mAccTop + ( ( mMacProduct >> 31 ) ? 0xffu : 0u ) + ( a.carry ? 1u : 0u ) ) & 0xff;
}

// divider.v: the end of a cycle. A DIV issued in it loads the operands; while active, two quotient
// bits a cycle, sixteen cycles; then the quotient takes a write port the scoreboard's sources before
// it leave free.
// divider.v, the data: a DIV loads the operands at the end of its cycle; then two quotient bits a
// cycle, non restoring, taken when the data is read.
void Risc::divideLoad( bool offset )
{
  mDivisor = mDivSrc;
  mDividendHi = offset ? mDivDst >> 16 : 0;
  mDividendLo = offset ? mDivDst << 16 : mDivDst;
  mLastNegN = true;
  mDivStart = false;
  mDivStepAt = mCycle + 1;
}

void Risc::divideCatchUp( std::uint64_t cycle )
{
  while ( mDivStepsLeft && mDivStepAt <= cycle )
  {
    std::uint32_t const dend1 = mDividendHi << 1 | mDividendLo >> 31;
    std::uint64_t const sum1 =
        static_cast<std::uint64_t>( dend1 ) + ( mDivisor ^ ( mLastNegN ? 0xffffffffu : 0u ) ) + ( mLastNegN ? 1u : 0u );
    bool const thisNeg = ( ( ( mDividendHi >> 31 ) != 0 ) != ( ( sum1 >> 32 ) != 0 ) ) != mLastNegN;
    std::uint32_t const addq1 = static_cast<std::uint32_t>( sum1 );
    std::uint32_t const dend2 = addq1 << 1 | ( ( mDividendLo >> 30 ) & 1 );
    std::uint64_t const sum2 =
        static_cast<std::uint64_t>( dend2 ) + ( mDivisor ^ ( thisNeg ? 0u : 0xffffffffu ) ) + ( thisNeg ? 0u : 1u );
    bool const nextNeg = ( ( ( addq1 >> 31 ) != 0 ) != ( ( sum2 >> 32 ) != 0 ) ) != !thisNeg;
    mDividendHi = static_cast<std::uint32_t>( sum2 );
    mDividendLo <<= 2;
    mQuotient = mQuotient << 2 | ( thisNeg ? 0u : 2u ) | ( nextNeg ? 0u : 1u );
    mLastNegN = !nextNeg;
    --mDivStepsLeft;
    ++mDivStepAt;
  }
}

// divider.v, the control, at the end of a cycle: the steps a divide takes, when its quotient takes
// a write port (the scoreboard's sources before it leaving one free).
void Risc::divide()
{
  if ( !mDivActive && !mDivStart && !mDivPending )
  {
    // count is 1 the cycle after a divide ends (cnten): a DIV issued then starts from 2 and runs
    // a step short, its quotient two bits down (random_io); otherwise it clears here
    mDivCount = 0;
    return;
  }
  bool const start = mDivStart;
  bool const activei = start || ( mDivActive && mDivCount != 0 );
  if ( start )
  {
    mDivStepsLeft = 16 - mDivCount;
    record( DIV_RUN, static_cast<std::uint16_t>( mDivStepsLeft | ( mDivOffset ? 0x100u : 0u ) ) );
    divideLoad( mDivOffset );
  }
  else
    divideCatchUp( mCycle );
  bool const dwbset = mDivActive && !activei;
  mDivCount = start || mDivActive ? ( mDivCount + 1 ) & 15 : 0;
  mDivActive = activei;
  if ( dwbset || mDivPending )
  {
    // the source port is the divider's only beside an ALU, move or internal load write (div_wbacks)
    WriteSlot const& next = slotAt( mCycle + 1 );
    mDivPending = next.count >= 2 || ( next.count == 1 && next.priority[0] == IMM_PRIORITY ) || mXldWback;
    if ( !mDivPending )
    {
      record( DIV_WRITE );
      divideCatchUp( mCycle );
      writeReg( mDivReg, mQuotient, mCycle + 1, DIV_PRIORITY );
    }
  }
}

// execon.v exe: whether the instruction in the decode stage issues in this cycle; nothing the cycle
// does before the issue changes it.
bool Risc::issuing() const
{
  if ( mCompdwait || !mVins || !mExec )
    return false;
  if ( mMtxDover && !mDatServ ) // mtx_wait: the matrix word is not served yet
    return false;
  return canIssue( mDecoded, true, mCycle );
}

// A stall in which nothing but the cycle count changes until the scoreboard lets the decode
// stage's instruction issue: the queue full enough that nothing is fetched, no jump or MOVEI data
// pending, nothing under way in the memory controller, the divider, the accumulator or the
// interrupt controller, and the I2S interface short of a serial clock edge. Returns how many
// cycles from this one may be counted out in one step, 0 if this one must be stepped.
std::uint32_t Risc::quietCycles() const
{
  if ( mQueueSize < 3 || mProgServ || mJat2 || mJrt2 || mJrel || mImm1 || mImm2 || !mInsrdyp )
    return 0;
  if ( mDatServ || !mMemIdle || mDrqt2 || mIldPending )
    return 0;
  if ( mBusBusy || mOneld || mTwold || mXldWbackl || coupled() )
    return 0;
  if ( !mSysIdle || mMtxDover || mDatMtx ) // MMULT's sequence requests its matrix every cycle
    return 0;
  if ( mDivActive || mDivPending || mDivCount != 0 || mMacAt == mCycle + 1 )
    return 0;
  if ( mIlatch != 0 || mIntser || mCpuIrq || mCpuIrqNext || mIoWrite || mI2s.rose() )
    return 0;
  // the cycle the instruction issues in; waits are shorter than the write calendar
  std::uint64_t limit = std::min( mStopAt, mCycle + WRITE_SLOTS );
  if ( mGpu && ( mIntEna & 8u ) ) // nothing within the margin of the object processor's interrupt
    limit = std::min( limit, mObjectQuiet );
  std::uint64_t t = mCycle + 1;
  while ( t < limit && !canIssue( mDecoded, true, t ) )
    ++t;
  std::uint64_t const n = std::min<std::uint64_t>( t - mCycle, mI2s.quiet() );
  return static_cast<std::uint32_t>( std::min( n, mTimers.requestIn( mCycle ) ) );
}

// A stall while the main bus serves the processor's load or store, as quietCycles' but for the bus:
// nothing changes in the processor until the bus hands it something (the load's data, a program
// word, the gateway's grant or its end), another master reaches its local space, or the
// instruction may issue. The bus is stepped a cycle at a time with the processor's horizon up to
// there; returns the cycles taken, this one with them, or 0.
std::uint32_t Risc::busWait( bool xldReady, bool ioreq )
{
  if ( !mBusBusy || xldReady || ioreq || mGateServ || mXlddSel || mXldWbackl || mRequest.valid )
    return 0;
  // as far as the other masters let the bus go, as they stand: the next cycle's start at least
  std::uint64_t const reach = mBus.reach( mMaster );
  if ( reach < mCycle + 2 )
    return 0;
  if ( mQueueSize < 3 || mProgServ || mJat2 || mJrt2 || mJrel || mImm1 || mImm2 || !mInsrdyp )
    return 0;
  if ( mDatServ || !mMemIdle || mDrqt2 || mIldPending || coupled() )
    return 0;
  if ( !mSysIdle || mMtxDover || mDatMtx )
    return 0;
  if ( mDivActive || mDivPending || mDivCount != 0 || mMacAt == mCycle + 1 )
    return 0;
  if ( mIlatch != 0 || mIntser || mCpuIrq || mCpuIrqNext || mIoWrite || mI2s.rose() )
    return 0;
  // the bus has stepped this cycle with the processor: an interrupt it brought is taken in it
  if ( mGpu && ( ( ( mIntEna & 8u ) && mBus.objectGpuInterrupt( mCycle ) ) || mBus.blitterGpuInterrupt( mCycle ) ) )
    return 0;
  // and no further than the run was asked to go: the 68000 may reach the local space after it
  std::uint64_t limit = std::min( { mStopAt, mPause, mCycle + WRITE_SLOTS, reach == UINT64_MAX ? reach : reach + 1 } );
  if ( mGpu && ( mIntEna & 8u ) )
    limit = std::min( limit, mObjectQuiet );
  std::uint64_t const end =
      mCycle + std::min( { limit > mCycle ? limit - mCycle : 0, mI2s.quiet(), mTimers.requestIn( mCycle ) } );
  std::uint64_t c = mCycle + 1;
  for ( ; c < end; ++c )
  {
    // the cycle the instruction may issue in is stepped as usual
    if ( canIssue( mDecoded, true, c ) )
      break;
    // the bus through the cycle before, as this one's start finds it (the 68000's DTACK may hold it
    // back sooner than it did)
    if ( !mBus.catchUp( mMaster, c ) )
      break;
    if ( !mIoreqs.empty() || coupled() || mBus.xldReady( mMaster ) || mBus.programReady( mMaster ) ||
         mBus.gateActive( mMaster ) != mGateActive || mBus.idle( mMaster ) )
      break;
  }
  if ( c == mCycle + 1 )
    return 0;
  mBusBusy = !mBus.steppedTo( c ) || !mBus.idle( mMaster );
  mTimerNear = 0;
  return static_cast<std::uint32_t>( c - mCycle );
}

// One DSP cycle of the front end (prefetch.v, execon.v, pc.v, gpu_mem.v) around the instruction it
// issues. Every signal is the netlist's of the same name; single stepping is left out. Returns the
// cycles taken: one, or a quiet stall at once.
std::uint32_t Risc::cycle()
{
  std::uint32_t const qs = mQueueSize;
  bool const insrdyt = qs != 0;
  bool const insrdy = insrdyt && !mJrel;

  // the bus stepped up to this cycle: the automaton may have replayed ahead of it, the other
  // processor may not have handed it over yet (the cycle then waits)
  if ( ( mBusBusy || coupled() ) && !mBus.catchUp( mMaster, mCycle ) )
  {
    mBlocked = true;
    return 0;
  }
  // the slave port serves a write in this cycle (ioserv), taken as the bus steps the cycle: the
  // bus is to step it with the processor's own request of it, before the rest of the cycle reads
  // what it writes
  mServeWait = mServeAt == mCycle && !mServeLate && !mBus.canStep( mMaster, mCycle );
  if ( mServeWait )
  {
    mBlocked = true;
    return 0;
  }
  if ( mServeAt <= mCycle )
  {
    mServeAt = UINT64_MAX;
    mServeLate = false;
  }
  if ( mBus.statusReadPending( mMaster ) )
  {
    Status& st = mStatus[mCycle & 3u];
    bool const dirty = mAuto && mAuto->loopDirty;
    st = { mCycle, readIo( mIoBase + REG_CTRL ), readIo( mIoBase + REG_FLAGS ) };
    if ( mAuto )
      mAuto->loopDirty = dirty;
  }

  // the gateway's outputs (gate_active, xld_ready), idle while the bus is not clocked with the
  // processor; sboard.v: an external load served now (del_xld) joins those under way
  mRequest = GatewayRequest{};
  bool const xldReady = mBusBusy && mBus.xldReady( mMaster );
  mGateServ = mBusBusy && mBus.programReady( mMaster ); // gatereq the cycle before
  if ( mGateServ )
    mProgData = mBus.programData( mMaster );
  mGateActive = mBusBusy && mBus.gateActive( mMaster );
  if ( xldReady )
    mXldData = mBus.loadData( mMaster ); // xld_data
  bool const datack = mDatServ;
  // gpu_mem.v decodes external from dataddr, MMULT's matrix address (always local) while the matrix
  // word is served: no external load then, whatever the last access's address was
  bool const delXld = datack && !mDatMtx && !isLocal( mAccess.address ) && !isStore( mAccess.op, mGpu );
  bool const ldidle = !( mOneld || mTwold );
  mOneldi = ( delXld && ldidle ) || ( mOneld && !delXld && !mXlddSel ) || ( mOneld && delXld && mXlddSel ) ||
            ( mTwold && mXlddSel );
  mTwoldi = ( mOneld && delXld && !mXlddSel ) || ( mTwold && !mXlddSel );

  bool const exe = issuing();
  // the load or store served in this cycle (datserv): before the instruction that issues in it, as
  // the flags and D_MOD it writes reach that instruction's ALU cycle
  if ( datack && mDatMtx )
  {
    // the matrix word (mem_data), at the vector's chip's RAM base and D_MTXA
    mMtxData = readLong( ( mGpu ? 0xf03000u : 0xf1b000u ) + ( mMtxAddr << 2 ) );
  }
  else if ( datack )
  {
    record( ACCESS );
    access();
    // A store served while MMULT's sequence requests its matrix (mtx_mreq masks datwe) counts as a
    // load to the scoreboard: the store's register is written back with what the bus carried, its
    // own data (ildwbset)
    if ( isStore( mAccess.op, mGpu ) && ( mSysImultn || mSysImacn ) && isLocal( mAccess.address ) )
    {
      mIldValue = mAccess.data;
      mIldPending = true;
    }
  }
  // gpu_mem.v: a fetch served outside the local space (progserv, external) is the gateway's to make,
  // a long word; a jump aborting it (pabort) leaves the gateway's word untaken
  std::uint32_t const fetchAddress = ( mFetchPc << 1 ) & 0xfffffcu;
  bool const fetchExternal = mGpu && mProgServ && !isLocal( fetchAddress );
  if ( fetchExternal )
  {
    mRequest.valid = true;
    mRequest.write = false;
    mRequest.address = fetchAddress;
    mRequest.size = 2;
    mRequest.program = true;
  }
  clockBus();
  // gpu_mem.v: the slave's request (ioreq), known once the bus has stepped this cycle, takes the
  // local bus before the processor's own
  bool ioreq = false;
  while ( !mIoreqs.empty() && mIoreqs.front() <= mCycle )
  {
    ioreq = ioreq || mIoreqs.front() == mCycle;
    mIoreqs.erase( mIoreqs.begin() );
  }

  // an indexed store reads its data in the cycle after it issues (compdwait)
  if ( mCompdwait )
  {
    record( STORE_DATA );
    mAccess.data = valueAt( mAccess.reg, mCycle );
  }

  std::uint16_t const microword = mOpcodeWord;
  // preinstr: the queue's head, valid or not, the half of insdata the queue size selects (ins_swap,
  // with big_instr set)
  std::uint16_t const preinstr = static_cast<std::uint16_t>( ( qs & 1 ) ? mInsdata : mInsdata >> 16 );
  std::uint32_t const headOp = preinstr >> 10u;
  std::uint32_t const microOp = microword >> 10u;
  bool const insexe = mVins && mExec && !mCompdwait;
  // execon.v wait_n: no sbwait, compdwait or mtx_wait (the matrix word not served yet)
  bool const waitN =
      !mCompdwait && !( mMtxDover && !mDatServ ) &&
      ( mVins ? canIssue( mDecoded, insexe, mCycle ) : canIssue( decode( microword, mBank ), insexe, mCycle ) );
  if ( !exe && mVins && mExec && !mCompdwait )
  {
    if ( std::uint32_t const n = quietCycles() )
    {
      mI2s.advance( n );
      return n;
    }
    if ( std::uint32_t const n = busWait( xldReady, ioreq ) )
    {
      mI2s.advance( n );
      return n;
    }
  }
  Executed done;
  if ( exe )
  {
    mIssue = mCycle;
    mMtxIssue = mMtxDover; // registers.v mtx_doverp: the matrix word as the destination operand
    mMultselIssue = mMultsel;
    record( EXECUTE );
    done = execute( mDecoded, programCount() );
    mMtxIssue = false;
    mMultselIssue = false;
  }

  // div_activei as a read of D_FLAGS or D_CTRL shows it
  if ( datack && mDivStart && mIldPending &&
       ( ( mAccess.address & ~3u ) == mIoBase + REG_FLAGS || ( mAccess.address & ~3u ) == mIoBase + REG_CTRL ) )
    mIldValue |= 1u << 26;

  // the data side of the memory controller (sboard.v, gpu_mem.v)
  bool const memExe = exe && done.memory;
  bool const drqt1 = ( mMemIdle && memExe && !done.precomp ) || mComp2;
  bool const sdatreq = ( drqt1 || mDrqt2 ) && !datack;
  bool const mtxMreq = ( mMtxDover && !datack ) || mSysImultn || mSysImacn; // systolic.v mtx_mreq
  bool const datreq = sdatreq || mtxMreq;
  bool const indexedStore = memExe && done.store && done.precomp;

  // romold: the decode stage takes the word at the head of the queue; an indexed store's successor
  // is taken in the cycle it reads its data
  bool const romold = ( ( mImm1 || mImm2 ) && insrdy ) || ( mExec && exe && insrdy && !indexedStore ) ||
                      ( mExec && !exe && mCompdwait ) || ( mExec && !exe && waitN && insrdy );
  // atomic (ins_exec.v): no interrupt is taken around a MOVEI and its data words, within a run of
  // IMULTN and IMACN, or about a jump
  bool const moveild = romold && headOp == MOVEI && mAwaitIdle;
  bool const headMult = headOp == IMACN || headOp == IMULTN;
  bool const multaset = romold && headMult && mAwaitIdle;
  bool const atomic = ( headOp == MOVEI && insrdy ) || ( mAwait2 && !romold ) || mAwait1 || mMulta || multaset ||
                      done.jumpAbs || done.jumpRel || mJat2 || mJrt2 || microOp == JUMP || microOp == JR ||
                      headOp == JUMP || headOp == JR;
  // systolic.v: MMULT taken from the queue starts the inserted sequence (mmult); the sequence is
  // atomic too
  bool const sromold = romold && !mIntser;
  bool const mmult = sromold && mImmCount == 0 && headOp == MMULT;
  bool const sysser = !mSysIdle;
  bool const isrset = mIlatch != 0 && !mIntser && !mImask && !atomic && !sysser && !mmult;

  // While an interrupt is being entered (intser) the decode stage takes the inserted sequence
  // (interrupt.v ins) and the queue is held (promold).
  bool const promold = romold && !mIntser && !sysser;
  bool immediate = false;
  bool jumped = false;
  // the instruction register takes the queue's head, valid or not (ins_exec.v mwordi, opcodein):
  // after an indexed store with the queue empty, a stale word whose registers the waits compare
  if ( romold )
  {
    std::uint16_t w = 0;
    if ( mIntser )
    {
      w = insertedWord( mIcount );
      jumped = mIcount == 7;
      mIcount = ( mIcount + 1 ) & 7;
    }
    else if ( sysser )
    {
      // sysins: 010, imacn, imultn or resmac, resmac, the vector's register, the destination
      w = static_cast<std::uint16_t>( 0x4000u | ( mSysImacn ? 0x1000u : 0u ) |
                                      ( mSysImultn || mSysResmac ? 0x800u : 0u ) | ( mSysResmac ? 0x400u : 0u ) |
                                      ( mSysR1 >> 1 ) << 5 | mSysR2 );
    }
    else
    {
      w = preinstr;
    }
    mOpcodeWord = w;
    if ( mImmCount )
    {
      immediate = true;
      record( mImmCount == 2 ? IMM_LO : IMM_HI, w );
      if ( mImmCount == 2 )
        mImmValue = w;
      else
        writeReg( mImmDst, mImmValue | static_cast<std::uint32_t>( w ) << 16, mCycle + 1, IMM_PRIORITY );
      --mImmCount;
    }
    else
    {
      mDecode = w;
      mDecodeMtx = sysser && mtxMreq;
      mDecoded = decode( w, mBank );
      if ( mDecodeMtx )
        mDecoded.s = mDecoded.srcReg = 32 + ( ( w >> 5 ) & 31u );
    }
  }

  // jumps take effect once their delay slot instruction is in the queue
  bool const jatPending = done.jumpAbs || mJat2;
  bool const jabs = jatPending && insrdyt;
  bool const jrtPending = done.jumpRel || mJrt2;
  bool const jreli = jrtPending && insrdyt;
  bool const jumpN = !( mJrel || jabs );
  // progack: a fetch served in the local space (progat0), or the gateway's word taken (progat1)
  bool const xprogset = fetchExternal && jumpN;
  bool const xprt = mXprog || xprogset;
  bool const viaGateway = xprt && mGateServ && !mXpabort;
  bool const progack = ( mProgServ && jumpN && !xprt ) || viaGateway;
  bool const prefetch = jumpN && ( qs == 0 || ( qs == 1 && !( progack && !promold ) ) || ( qs == 2 && !progack ) ||
                                   ( qs == 3 && promold ) );
  bool const progreq = !datreq && !ioreq && prefetch;

  std::uint32_t const pc = mFetchPc;
  if ( jabs )
    mFetchPc = ( mJumpValue >> 1 ) & 0x7fffff;
  else if ( mJrel )
    mFetchPc = ( pc - qs - 1 + mSrcdp ) & 0x7fffff; // pcadd: srcdp still holds the JR's offset
  else if ( progack )
    mFetchPc = ( ( pc & ~1u ) + 2 ) & 0x7fffff;

  // The queue: insdata holds the head and the word after it, pfdata the long word fetched after
  // those. A fetch is a long word; after a jump to an odd word only its second half is queued.
  std::uint32_t nextQs = qs;
  std::uint32_t gpuData = 0;
  if ( progack )
  {
    std::uint32_t const address = ( pc << 1 ) & 0xfffffc;
    if ( viaGateway )
      gpuData = mProgData;
    else
    {
      gpuData = static_cast<std::uint32_t>( fetch16( address ) ) << 16 | fetch16( address + 2 );
      mCodeMark[( ( address - mRamBase ) & ( mRamSize - 1 ) ) >> 2] = 1;
    }
    nextQs += qs == 0 && ( pc & 1 ) ? 1 : 2;
  }
  if ( promold && qs != 0 )
    --nextQs;
  bool const insld = ( qs == 0 && progack ) || ( qs == 1 && promold && progack ) || ( qs == 3 && promold );
  if ( insld )
    mInsdata = qs == 3 ? mPfdata : gpuData;
  if ( progack )
    mPfdata = gpuData;
  mQueueSize = jumpN ? nextQs : 0; // force0

  bool const nextExec = mIdle || ( mExec && !exe ) || ( mExec && exe && !done.immld ) || ( mImm2 && mInsrdyp );
  bool const nextImm1 = ( mExec && exe && done.immld ) || ( mImm1 && !mInsrdyp );
  bool const nextImm2 = ( mImm1 && mInsrdyp ) || ( mImm2 && !mInsrdyp );
  bool const vinsClear = ( mImm1 || mImm2 ) && mInsrdyp;
  bool const nextVins = ( romold && insrdy && !immediate ) || ( mVins && !exe && !vinsClear );

  mExec = nextExec;
  mImm1 = nextImm1;
  mImm2 = nextImm2;
  mVins = nextVins;
  mIdle = false;
  mInsrdyp = insrdy;
  mJat2 = jatPending && !insrdyt;
  mJrt2 = jrtPending && !insrdyt;
  mJrel = jreli;
  bool const xprogi = xprogset || ( mXprog && !mGateServ );
  mXpabort = ( mXprog && !prefetch && !mGateServ ) || ( mXpabort && !mGateServ ); // xpabortset
  mXprog = xprogi;
  mProgServ = progreq && !xprogi; // progreqa
  if ( exe )
    mSrcdp = done.srcdp;

  // a loaded value takes a write port that the scoreboard's sources before it leave free
  if ( mIldPending && slotAt( mCycle + 1 ).count < 2 )
  {
    record( ILD_WRITE );
    writeReg( mAccess.reg, mIldValue, mCycle + 1, ILD_PRIORITY );
    mIldPending = false;
  }

  // an external load's value (xld_data) takes a port after the internal one's and before the
  // divider's: not beside a MOVEI's data word nor beside two writes (xld_wbackh, xld_wbacks)
  mXldWback = mXldWbackl || xldReady;
  std::uint32_t const xldWbaddr = mXlddSel && mTwold                               ? mXlddst2
                                  : delXld && ( ldidle || ( mOneld && mXlddSel ) ) ? mAccess.reg
                                                                                   : mXlddst;
  bool xldWritten = false;
  if ( mXldWback )
  {
    WriteSlot const& next = slotAt( mCycle + 1 );
    if ( next.count == 0 || ( next.count == 1 && next.priority[0] != IMM_PRIORITY ) )
    {
      writeReg( xldWbaddr, mXldData, mCycle + 1, XLD_PRIORITY );
      xldWritten = true;
    }
  }
  mOneld = mOneldi;
  mTwold = mTwoldi;
  mXlddst = xldWbaddr;
  if ( delXld )
    mXlddst2 = mAccess.reg;
  mXlddSel = xldWritten;
  mXldWbackl = mXldWback && !xldWritten;

  if ( !exe && mMacAt == mCycle + 1 )
  {
    record( ACCUMULATE, microword );
    accumulate( microword );
  }
  divide();

  // interrupt.v: the latches take the requests the enables let through, the flags write clears
  // some; the interrupt mask is set as the inserted sequence is taken
  std::uint32_t const requests = ( mI2s.rose() ? 2u : 0u ) | ( mCpuIrq ? 1u : 0u ) | mTimers.requests( mCycle ) << 2 |
                                 ( mGpu && ( mIntEna & 8u ) && mBus.objectGpuInterrupt( mCycle ) ? 8u : 0u ) |
                                 ( mGpu && mBus.blitterGpuInterrupt( mCycle ) ? 16u : 0u );
  if ( requests || mIoWrite )
  {
    if ( requests & 2u )
    {
      record( IRQ_TRACE );
      emit( mCycle, "IRQ i2s" );
    }
    mIlatch = ( mIlatch & ~mIlatchClear ) | ( requests & mIntEna );
    mIlatchClear = 0;
    mIntEna = mIntEnaNext;
    mImask = mImask && !mImaskClear;
    mImaskClear = false;
    mDivOffset = mDivOffsetNext;
    mIoWrite = false;
  }
  mImask = mImask || ( romold && mIntser );
  mIntser = isrset || ( mIntser && !jumped );
  bool const nextAwait1 = moveild || ( mAwait1 && !romold );
  bool const nextAwait2 = ( mAwait1 && romold ) || ( mAwait2 && !romold );
  mAwaitIdle = ( mAwait2 && romold ) || ( mAwaitIdle && !moveild );
  mAwait1 = nextAwait1;
  mAwait2 = nextAwait2;
  mMulta = !( romold && !headMult ) && ( multaset || mMulta );
  if ( mCpuIrq || mCpuIrqNext )
  {
    mCpuIrq = mCpuIrqNext;
    mCpuIrqNext = false;
  }
  if ( mI2s.stepping() )
    mI2s.step();
  else
    mI2s.skip();

  std::uint32_t const bank = mRegpage && !mImask ? 32u : 0u;
  if ( bank != mBank )
  {
    mBank = bank;
    mDecoded = decode( mDecode, bank );
    if ( mDecodeMtx )
      mDecoded.s = mDecoded.srcReg = 32 + ( ( mDecode >> 5 ) & 31u );
  }

  // systolic.v next state
  {
    bool const active = !mSysIdle;
    bool const count1 = mMcount == 1;
    bool const nextIdle = ( mSysIdle && !mmult ) || ( mSysResmac && sromold );
    bool const nextImultn = ( mSysIdle && mmult ) || ( mSysImultn && !sromold );
    bool const nextImacn = ( mSysImultn && sromold ) || ( mSysImacn && !count1 ) || ( mSysImacn && !sromold );
    bool const nextResmac = ( mSysImacn && count1 && sromold ) || ( mSysResmac && !sromold );
    if ( mmult )
      mMcount = mMwidth;
    else if ( sromold && active )
      mMcount = ( mMcount - 1 ) & 15;
    if ( sromold )
      mMultsel = ( mSysR1 & 1 ) && active;
    if ( mmult )
    {
      mSysR1 = ( static_cast<std::uint32_t>( preinstr >> 5 ) & 31 ) << 1;
      mSysR2 = preinstr & 31u;
    }
    else if ( sromold )
      mSysR1 = ( mSysR1 + 1 ) & 63;
    if ( mMtxDover && datack ) // macnten
      mMtxAddr = ( mMtxAddr + ( mMaddw ? mMwidth : 1u ) ) & 0x3ff;
    mSysIdle = nextIdle;
    mSysImultn = nextImultn;
    mSysImacn = nextImacn;
    mSysResmac = nextResmac;
    mMtxDover = mtxMreq;
  }

  bool const nextIdle = ( mMemIdle && !memExe ) || ( mLoading && datack );
  mLoading = ( mMemIdle && memExe && !done.precomp ) || mComp2 || ( mLoading && !datack );
  mComp2 = mComp1;
  mComp1 = mMemIdle && memExe && done.precomp;
  mMemIdle = nextIdle;
  mDrqt2 = !datack && ( drqt1 || mDrqt2 );
  mDatServ = datreq && !ioreq; // gpu_mem.v datreqa
  mDatMtx = mtxMreq;           // dataddr: mtx_addr while mtx_mreq
  mCompdwait = indexedStore;
  return 1;
}

// The bus is stepped with the processor in every cycle, whether it asks for it or not, while
// something on it may reach the processor: for the DSP, the GPU's access to Jerry (its slave, its
// timers); for the GPU, the object processor's interrupt, while it is enabled, and the blitter while
// it works (its registers on the GPU's local bus, its interrupt).
bool Risc::coupled() const
{
  if ( !mIoreqs.empty() || mServeAt != UINT64_MAX || mBus.slaveAccessPending( mMaster ) )
    return true;
  return mGpu ? objectsNear() || blitterNear() : mBus.gpuAtJerry();
}

// The blitter at work, and something of it reaching the GPU in this cycle: its interrupt, while it
// is enabled; the read or write of a register of it served now (the access is never replayed: its
// address is not of the local class); the GPU's local RAM, which a blit based in Tom's space may
// read or write. Else the GPU runs by itself, its writes of the blitter's registers queued for
// their cycles.
bool Risc::blitterNear() const
{
  if ( !mBus.blitterBusy() )
    return false;
  return ( mIntEna & 16u ) || ( mDatServ && ( mAccess.address & 0xffff00u ) == BLITTER_BASE ) ||
         mBus.blitterAtTomOrJerry();
}

// The object processor's interrupt enabled, and this the first cycle it may come in or later: the
// bus is stepped with the GPU from then on. Before it, the GPU runs on by itself, replaying no
// variant that would reach it (objectsWithin).
bool Risc::objectsNear() const
{
  if ( !( mIntEna & 8u ) || !mBus.objectsRun() )
    return false;
  if ( mCycle < mObjectQuiet )
    return false;
  mObjectQuiet = mBus.objectQuietUntil( mCycle );
  return mCycle >= mObjectQuiet;
}

// The main bus, clocked with the DSP while a request of it is under way: in the cycle, after the
// gateway has taken what the memory controller serves, before the interrupt requests are latched
// (a timer written in the cycle may request in it).
void Risc::clockBus()
{
  if ( !mRequest.valid && !mBusBusy && !coupled() )
    return;
  if ( mRequest.valid )
  {
    // a write to Jerry's registers (Tom's, for the GPU) may start a timer: nothing is replayed until
    // it is made
    mTimerWrite = mTimerWrite || ( mRequest.write && ( mGpu ? Tom::decodes( mRequest.address )
                                                            : ( mRequest.address & 0xff0000u ) == 0xf10000u ) );
    if ( !mGateActive && mTrace )
    {
      char text[40];
      std::uint32_t const width = 1u << mRequest.size;
      if ( mRequest.write )
        std::snprintf( text, sizeof text, "S(%06x)=%08x w%u", mRequest.address, mRequest.data, width );
      else
        std::snprintf( text, sizeof text, "L(%06x) w%u", mRequest.address, width );
      emit( mCycle + 1, text );
    }
  }
  if ( mRequest.valid )
    mObjectQuiet = 0; // a write of Tom's registers may move the next line
  mBus.clock( mMaster, mCycle, mRequest );
  mBusBusy = !mBus.steppedTo( mCycle + 1 ) || !mBus.idle( mMaster );
  mTimerNear = 0; // a write may have changed the timers
  mTimerWrite = mTimerWrite && mBusBusy;
}

} // namespace jag
