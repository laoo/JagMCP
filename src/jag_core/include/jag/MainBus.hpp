#pragma once

#include "jag/Blitter.hpp"
#include "jag/Eeprom.hpp"
#include "jag/JerryInterrupts.hpp"
#include "jag/Timers.hpp"
#include "jag/Tom.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace jag
{

class ExternalBus;

// A load or store a RISC's memory controller serves outside its local space in the current cycle
// (gpu_mem.v datserv with external): what the gateway takes.
struct GatewayRequest
{
  bool valid = false;
  bool write = false;
  std::uint32_t address = 0;
  std::uint32_t size = 0; // msize: 0 a byte, 1 a word, 2 a long word, 3 a phrase
  std::uint32_t data = 0; // a store's
  bool program = false;   // a fetch of the program (progserv), a long word
  std::uint32_t pc = 0;   // a load's or store's instruction, for UndrivenRead
};

// The masters with a port on the bus: the DSP's gateway (through Jerry) and the GPU's (Tom's own).
enum class Master : std::uint8_t
{
  Dsp,
  Gpu
};

// The state the bus starts in: the DSP's or the GPU's GO as the reference boot program leaves the
// console (the 68000 at its last bus cycles, then STOP), or the end of reset, the 68000 emulated.
enum class Boot : std::uint8_t
{
  Dsp,
  Gpu,
  Reset
};

// A bus cycle of the 68000 (68k.v's pins): AS falls in `start`, at the 68000 clock's first half;
// a read's strobes fall with it, a write's two cycles later with the data. It ends three cycles
// after the first cycle at that phase, from two after `start` on, in which Tom gives DTACK; the
// strobes with it. It starts later, a clock at a time, while the bus is not the 68000's (w432).
struct M68kAccess
{
  std::uint64_t start = 0;
  bool read = true;
  std::uint32_t address = 0; // A1 to A23, bit 0 clear
  bool uds = true;           // the strobes asserted: the even byte, the odd one
  bool lds = true;
  std::uint32_t fc = 0;
  std::uint16_t data = 0; // a write's
  // TAS's read-modify-write: its read holds AS low after DTACK, its write starts with AS low
  bool holdAs = false;
  bool held = false;
};

// A RISC's slave side as the bus reaches it (gpu_cpu.v): a long word written into its local space
// by another master, served by its memory controller in cycle `cycle` (ioserv); a long word read
// from it as the bus latches it.
class RiscSlave
{
public:
  virtual ~RiscSlave() = default;
  virtual void slaveWrite( std::uint64_t cycle, std::uint32_t address, std::uint32_t value ) = 0;
  virtual std::uint32_t slaveRead( std::uint64_t cycle, std::uint32_t address ) = 0;
  // The slave asks for the local bus in `cycle` (ioreq): the processor's own requests wait.
  virtual void slaveRequest( std::uint64_t cycle ) = 0;
};

// The main bus as the RISCs meet it, stepped by RISC cycle: the gateways (gateway.v), Jerry's bus
// master and slave (j_jmem.v, j_jbus.v, gpu_cpu.v), Tom's arbiter, external master interface,
// memory controller, memory width sequencer, address decoder and refresh (arb.v, cpu.v, mem.v,
// memwidth.v, abus.v, misc.v), and the 68000 as the reference boot program leaves it
// (docs/architecture.md). Every signal is the netlist's of the same name. Data is moved at once,
// in the cycle the netlist moves it; only the control is stepped.
//
// Each master's processor hands its requests to the bus in its own cycles; the bus steps a cycle
// once every running processor has handed it what it serves in it (its horizon is past it), so that
// the two may run apart while they do not meet on the bus.
//
// Ports Jaguar_MiSTer rtl/jaguar_common/gateway.v, rtl/jaguar_common/gpu_cpu.v,
// rtl/Jerry/j_jmem.v, rtl/Jerry/j_jbus.v, rtl/Tom/arb.v, rtl/Tom/cpu.v, rtl/Tom/mem.v,
// rtl/Tom/memwidth.v, rtl/Tom/abus.v, rtl/Tom/misc.v, the bus arbitration of rtl/nuked-68k/68k.v
// and the xbgl flip-flop of rtl/jaguar.v, read at 43a761d.
class MainBus
{
public:
  // `boot`: the console's state in cycle 0. With Boot::Reset the OS ROM is `rom` and the
  // cartridge's ROM `cart` (none: pulled up), each mirrored through its size, a power of two, read
  // by the 68000's bus cycles (m68kAccess) and the other masters'.
  explicit MainBus( ExternalBus& memory,
                    Boot boot = Boot::Dsp,
                    std::vector<std::uint8_t> rom = {},
                    std::vector<std::uint8_t> cart = {} );
  ~MainBus();

  MainBus( MainBus const& ) = delete;
  MainBus& operator=( MainBus const& ) = delete;
  MainBus( MainBus&& ) = delete;
  MainBus& operator=( MainBus&& ) = delete;

  // What runs the RISCs up to a cycle (with the emulated 68000): a span of the 68000's bus cycle runs
  // them through it first, the bus held, to know that nothing of theirs meets it.
  void setOthers( std::function<void( std::uint64_t )> others )
  {
    mOthers = std::move( others );
  }

  // What takes a read of Tom's space that no register answers (UndrivenRead): its cpu, the
  // instruction's pc (a RISC's; the 68000's is the taker's to find), the address, the value read and
  // the cycle.
  using UndrivenReadSink = std::function<void(
      char const* cpu, std::uint32_t pc, std::uint32_t address, std::uint16_t value, std::uint64_t cycle )>;

  // The master the bus is granted to (arb.v ack_): the 68000, the blitter, the GPU, the DSP, the
  // object processor or the DRAM's refresh.
  char const* busHolder() const;

  void setUndrivenRead( UndrivenReadSink sink )
  {
    mUndrivenRead = std::move( sink );
  }

  // The control replayed from the transitions recorded of it, the bus's timing automaton
  // (docs/decisions/0020-the-bus-replays-its-control.md); off, worked out every cycle.
  void setAutomaton( bool on );

  // G_HIDATA, the GPU's gateway's hirdata: the high half of a phrase loaded or to store.
  std::uint32_t hidata() const
  {
    return mGw[GPU].hirdata;
  }

  void setHidata( std::uint32_t value )
  {
    mGw[GPU].hirdata = value;
  }

  // A gateway's outputs in the current cycle.
  bool gateActive( Master m ) const
  {
    return mCtl->gw[index( m )].active;
  }

  bool xldReady( Master m ) const
  {
    return mCtl->gw[index( m )].xldReady;
  }

  // The gateway hands the processor's local bus a program long word (gatereq) in the cycle just
  // stepped; the word (prog_data).
  bool programReady( Master m ) const
  {
    return mCtl->gw[index( m )].gatereq;
  }

  std::uint32_t programData( Master m ) const
  {
    return mGw[index( m )].progData;
  }

  std::uint32_t loadData( Master m ) const
  {
    return mGw[index( m )].loadData;
  }

  // Jerry's timers, which writes on the bus program.
  Timers& timers()
  {
    return mTimers;
  }

  // JOYSTICK's low byte as a read gives it (jaguar.v joy[7:0]): with the outputs enabled (JOYSTICK's
  // bit 15 written, joyen) the column latch written last (u374) in bits 7 to 1, else all set; bit 0
  // the cartridge's EEPROM's DO.
  std::uint8_t joystickLow() const
  {
    return static_cast<std::uint8_t>( ( mJoyEnable ? mJoyLatch & 0xfeu : 0xfeu ) | ( mEeprom.out() ? 1u : 0u ) );
  }

  // The buttons held on the pads in Jerry's two ports (0 and 1), a bit each in the order of the
  // MiSTer core's joystick inputs: right, left, down, up, A, B, C, Option, Pause, 1 to 9, 0, *, #.
  void setPad( std::size_t port, std::uint32_t buttons )
  {
    mPads[port & 1u] = buttons;
  }

  std::uint32_t pad( std::size_t port ) const
  {
    return mPads[port & 1u];
  }

  // JOYSTICK's high byte and JOYBUTS's low byte as a read gives them (jaguar.v joy[15:8], b[7:0]):
  // the rows of the column each port's half of the latch selects (jag_controller_mux.v), low for a
  // button held; JOYBUTS's high bits a PAL console's.
  std::uint8_t joystickHigh() const
  {
    return padHigh( padRows( 0 ), padRows( 1 ) );
  }

  std::uint8_t joystickButtons() const
  {
    return static_cast<std::uint8_t>( ( padRows( 0 ) & 3u ) | ( padRows( 1 ) & 3u ) << 2 | 0x60u );
  }

  // Tom's time base, PIT and interrupt control, which writes on the bus program.
  Tom& tom()
  {
    return mTom;
  }

  // D_FLAGS or G_FLAGS bit 15 (gateway.v dmaen): the master's bus requests are of the high
  // priority.
  void setDmaen( Master m, bool on )
  {
    mGw[index( m )].dmaen = on;
  }

  // Where another master's writes into the DSP's local space go.
  void setDspSlave( RiscSlave* slave )
  {
    mDspSlave = slave;
  }

  // Where the 68000's writes and reads of the GPU's local space go.
  void setGpuSlave( RiscSlave* slave )
  {
    mGpuSlave = slave;
  }

  // The master's processor runs from `cycle` on, or (`UINT64_MAX`) is stopped: the bus does not
  // wait for it.
  void attach( Master m, std::uint64_t cycle )
  {
    mHorizon[index( m )] = cycle;
  }

  // The object processor may run, and interrupt the GPU.
  bool objectsRun()
  {
    return mTom.videoOn() || !mTom.op().idle();
  }

  // The first cycle from `from` (the GPU's) the object processor may interrupt the GPU in: one the
  // bus has stepped already, ahead of the GPU; else, as far as it has stepped, while it walks its
  // list, from what it does, or that of the next line it may start.
  std::uint64_t objectQuietUntil( std::uint64_t from )
  {
    if ( mObjectGpuIrq != UINT64_MAX && mObjectGpuIrq >= from && mObjectGpuIrq < mCycle )
      return mObjectGpuIrq;
    if ( !mTom.op().idle() )
      return mCycle + mTom.op().gpuQuietCycles();
    return mTom.videoOn() ? mTom.nextLineStart( mCycle ) : UINT64_MAX;
  }

  // The object processor's interrupt to the GPU (grpintreq) in `cycle`, the bus stepped up to it:
  // through it, or (the DSP has not handed it over yet) to its start.
  bool objectGpuInterrupt( std::uint64_t cycle )
  {
    return mCycle > cycle ? mObjectGpuIrq == cycle : objectGpuInterruptAhead( cycle );
  }

  // The GPU's write of a blitter register on its local bus in `cycle` (gpu_memw with bliten), taken
  // as the bus steps the cycle.
  void blitterWrite( std::uint64_t cycle, std::uint32_t address, std::uint32_t value )
  {
    mBlitWrites.push_back( { cycle, address, value } );
  }

  // A read of the blitter's registers on the GPU's local bus, the bus stepped up to the cycle.
  std::uint32_t blitterRead( std::uint32_t address ) const
  {
    return mBlitter.read( address, blitterGranted() );
  }

  // The blitter works, has a write to take or still holds Tom's bus: what the GPU reads of it may
  // change from cycle to cycle, so that it steps the bus with itself.
  bool blitterBusy() const
  {
    return !mBlitter.settled() || !mBlitWrites.empty() || blitterGranted();
  }

  // The blitter at work may reach the RISCs' local memories (its A1 or A2 based in Tom's or
  // Jerry's space).
  bool blitterAtTomOrJerry() const
  {
    return mBlitter.basedAtTomOrJerry();
  }

  // The GPU's interrupt to the 68000 (gpu_ctrl.v cpu_int) in `cycle`, taken as the bus steps it.
  void gpuCpuInterrupt( std::uint64_t cycle )
  {
    mGpuCpuIrq = cycle;
  }

  // The DSP's interrupt to the CPU (cpu_int, Jerry's dint) in `cycle`, taken as the bus steps it.
  void dspCpuInterrupt( std::uint64_t cycle )
  {
    mDspCpuIrq = cycle;
  }

  // The blitter's interrupt to the GPU (blit_int, gpu_irq[4]) in `cycle`, the bus stepped up to it:
  // through it, or to its start, the interrupt then from the blitter's flip-flops before the cycle
  // (which stepping it would give).
  bool blitterGpuInterrupt( std::uint64_t cycle ) const
  {
    return mCycle > cycle ? mBlitGpuIrq == cycle : cycle == mCycle && mBlitter.interruptAhead();
  }

  // Both processors run: each may have to wait for the other to hand the bus a cycle.
  bool shared() const
  {
    return mHorizon[DSP] != UINT64_MAX && mHorizon[GPU] != UINT64_MAX;
  }

  // The end of a run: every cycle before `cycle` stepped, what is under way in them done.
  void runOut( std::uint64_t cycle )
  {
    mHorizon[DSP] = mHorizon[GPU] = mHorizon[M68K] = cycle;
    advance();
  }

  // The run ends at `cycle`: no cycle from it on is stepped, whoever asks.
  void setEnd( std::uint64_t cycle )
  {
    mEnd = cycle;
  }

  // The emulated 68000 (Boot::Reset) has handed over every cycle before `cycle`: it starts no bus
  // cycle before it. Steps the bus up to it as far as the RISCs' horizons let.
  void m68kHorizon( std::uint64_t cycle );

  // The 68000 starts a bus cycle (its horizon is `access.start`). Steps the bus through it as far
  // as the RISCs' horizons let; m68kEnd() says whether it ended.
  void m68kAccess( M68kAccess const& access );

  // Steps the 68000's bus cycle on as far as the RISCs' horizons let.
  void m68kContinue();

  // The 68000's bus cycle has ended: the cycle Tom's DTACK was taken in (AS rises three later),
  // and a read's data.
  struct M68kEnd
  {
    bool ended = false;
    std::uint64_t start = 0; // AS fell: later than asked while the bus was not the 68000's
    std::uint64_t dtack = 0;
    std::uint16_t data = 0;
  };

  // The 68000's bus cycle under way reaches only memory (the DRAM, the ROM, the cartridge): not
  // Tom's or Jerry's registers, nor the RISCs' spaces.
  bool m68kAtMemory() const;

  M68kEnd m68kEnd() const
  {
    return { !mM68k.pending, mM68k.access.start, mM68k.dtack, mM68k.readData };
  }

  // Tom's interrupt to the 68000 (xintl) in `cycle`, the bus stepped there.
  bool m68kInterrupt( std::uint64_t cycle )
  {
    return mTom.interrupt( cycle );
  }

  // What the 68000 reads at `address`, without a bus cycle.
  std::uint16_t m68kPeek( std::uint32_t address )
  {
    return m68kReadWord( address );
  }

  // The cycle the bus has stepped to: every cycle before it has been stepped.
  std::uint64_t now() const
  {
    return mCycle;
  }

  // Every cycle before `cycle` has been stepped.
  bool steppedTo( std::uint64_t cycle ) const
  {
    return mCycle >= cycle;
  }

  // The bus may step `cycle` as soon as `m` hands it over: the other RISC and the 68000 have, and
  // the run goes on through it.
  bool canStep( Master m, std::uint64_t cycle ) const;

  // The GPU has a load or store of Jerry's space under way: the DSP's slave or its timers may be
  // reached, and the DSP is to step the bus with itself.
  bool gpuAtJerry() const
  {
    return mGpuAtJerry;
  }

  // A 68000 program: the 68000 may reach into the RISCs' local spaces at any cycle.
  bool emulated68k() const
  {
    return mEmulated68k;
  }

  // The emulated 68000 has a bus cycle under way into the local space of `m` (its slave port), or,
  // for the DSP, into Jerry's registers (its timers, which interrupt it).
  bool slaveAccessPending( Master m ) const
  {
    std::uint32_t const a = mM68k.access.address;
    return mM68k.pending && ( m == Master::Gpu ? ( a & 0xffe000u ) == 0xf02000u : ( a & 0xff0000u ) == 0xf10000u );
  }

  // ... and it is a read of the RISC's FLAGS or CTRL, which the slave port serves as the RISC has
  // them in the cycle (Risc's status kept for it).
  bool statusReadPending( Master m ) const
  {
    std::uint32_t const a = mM68k.access.address & 0xfffffcu;
    std::uint32_t const flags = m == Master::Gpu ? 0xf02100u : 0xf1a100u;
    return slaveAccessPending( m ) && mM68k.access.read && ( a == flags || a == flags + 0x14u );
  }

  // Nothing of the master's is under way: its gateway is idle and no load waits to be written back.
  // Its processor may then leave the bus behind, up to the cycle it next hands it a request.
  bool idle( Master m ) const;

  // The cycles the bus may step for the master's processor before the others hold it back: up to the
  // first the others have not handed over (catchUp( m, cycle ) gets there for every cycle up to it).
  std::uint64_t reach( Master m ) const
  {
    std::uint64_t const other = mHorizon[m == Master::Gpu ? DSP : GPU];
    std::uint64_t const m68k = mM68k.pending ? UINT64_MAX : mHorizon[M68K];
    return std::min( { other, m68k, mEnd } );
  }

  // The master's processor is at the start of `cycle`, every request before it handed over: steps
  // the bus up to it as far as the other's horizon lets. Returns whether it got there.
  bool catchUp( Master m, std::uint64_t cycle );

  // What the master's memory controller serves in `cycle`; steps the bus through it as far as the
  // other's horizon lets.
  void clock( Master m, std::uint64_t cycle, GatewayRequest const& request );

private:
  static constexpr std::size_t DSP = 0;
  static constexpr std::size_t GPU = 1;
  static constexpr std::size_t M68K = 2;

  static constexpr std::size_t index( Master m )
  {
    return m == Master::Gpu ? GPU : DSP;
  }

  // blit_back: Tom's latched grant is the blitter's (ack_[1], ack_[6])
  bool blitterGranted() const
  {
    return ( mCtl->arbAck & 0x42u ) != 0;
  }

  // dbus.v dout: the data bus's low long word, moved down for a justified read
  std::uint32_t dataBusLow() const;

  // A cycle of the bus in three parts (MainBus.cpp): what its control reads besides its own state,
  // gathered first (Gathered); the next state of the control and what it tells the data side, worked
  // out from the two alone (Signals); the data moved as those say.
public:
  struct Control;

private:
  struct Gathered;
  struct Signals;
  void gather( std::uint64_t cycle, Gathered& g );
  void control( Control const& c, Gathered const& g, Signals& s, Control& n ) const;
  void data( std::uint64_t cycle, Gathered& g, Signals const& s, Control const& n, bool restful );
  // the control's part of a rest: the state of every latch that holds at rest
  static bool restful( Control const& k );

  // The timing automaton: the control state as a node, the cycle's inputs as a key
  struct Node;
  struct Automaton;
  std::uint64_t keyOf( Gathered const& g ) const;
  Node& nodeFor( Control const& state );
  void flushAutomaton();

  // A span: the 68000's bus cycle on a bus otherwise quiet, from the cycle it is asked in through
  // DTACK, replayed in one step once recorded (MainBus.cpp)
  struct Span;
  struct SpanGroup;

  struct Recording
  {
    bool on = false;
    std::uint64_t start = 0;
    std::uint64_t steadyUntil = 0; // Tom's ihandler holds before this cycle
    Node* node = nullptr;
    std::uint64_t key = 0;
    std::uint32_t toPen = 0;
    std::uint32_t ps = 0;
    std::uint32_t rp = 0;
    bool opRuns = false;
    std::uint32_t dw = 0; // the line buffers' bus as last handed to Tom
    bool dwKnown = false;
    bool pins = false; // the 68000's address is driven
    std::vector<std::uint32_t> actions;
  } mRec;
  // a span to compare with the cycles stepped, with JAG_BUS_CHECK
  struct SpanCheck;
  std::unique_ptr<SpanCheck> mSpanCheck;
  bool trySpan();
  void spanRecord( std::uint64_t cycle, std::uint32_t kind, std::uint32_t arg = 0 );
  void finishSpan();
  void spanEnds();
  bool quietCycle( std::uint64_t cycle ) const;
  void replaySpan( Span const& span, bool dry );
  void countRefresh( std::uint64_t n );
  // the control's part of a span's cycle: no master but the 68000 and the refresh at work
  static bool quiet( Control const& k );

  void advance();
  void step();
  void rest( std::uint64_t n );
  std::uint64_t restReach( std::uint64_t n ) const;
  std::uint32_t read( std::uint32_t address, std::uint32_t size );
  void write( std::uint32_t address, std::uint32_t size, std::uint32_t value );
  // Tom's registers in its internal I/O cycle, MEMCON1 and MEMCON2 among them, a word at a time
  void tomWrite( std::uint64_t cycle, std::uint32_t address, std::uint16_t value );
  // A long word through the 32 bit window onto the line buffers ($F08800 on), in its I/O cycle at
  // `address`, the data bus's lanes it names
  void lineBufferLongWrite( std::uint64_t cycle, std::uint32_t address );
  bool objectGpuInterruptAhead( std::uint64_t cycle );

public:
  // For a debugger: the byte the OS ROM or the cartridge holds at `address` as the board maps it, or
  // none where neither is.
  std::optional<std::uint8_t> peekBoard( std::uint32_t address ) const
  {
    if ( !boardAt( address ) )
      return std::nullopt;
    return static_cast<std::uint8_t>( boardBytes( address, 1 ) );
  }

private:
  // A read of Tom's registers by `cpu` (m68k, gpu or dsp), its instruction at `pc`; by the blitter
  // with a null `cpu`, an undriven read then not reported (it has no instruction).
  std::uint16_t tomRead( std::uint64_t cycle, std::uint32_t address, char const* cpu, std::uint32_t pc );
  // what the 68000 reads of a memory under its strobes: the DRAM, the OS ROM, the cartridge
  std::uint16_t m68kReadWord( std::uint32_t address, bool uds = true, bool lds = true );
  // the OS ROM or the cartridge (Boot::Reset) is where `address` decodes to
  bool boardAt( std::uint32_t address ) const;
  // the byte at `address` of a ROM cycle from `start` (the address Tom puts out), as the board drives
  // its lane of the external bus at MEMCON1's ROMWIDTH: the OS ROM, 8 bits wide, the low lane with
  // the byte at `start`; the cartridge, 32 bits wide and blind to A0 and A1, the lanes with the long
  // word at `start`; a lane nothing drives pulled up
  std::uint8_t boardByte( std::uint32_t start, std::uint32_t address ) const;
  // the bytes from `address` of an access `bytes` wide, in the ROM cycles memwidth.v splits it into
  std::uint32_t boardBytes( std::uint32_t address, std::uint32_t bytes ) const;

  ExternalBus& mMemory;
  RiscSlave* mDspSlave = nullptr;
  RiscSlave* mGpuSlave = nullptr;

  // gpu_cpu.v of the GPU (JERRY 0) as the 68000 reaches it through Tom's internal I/O cycles, a word
  // at a time: a write's high half latched (lodld), its low half served by the GPU's memory
  // controller in the next cycle (ioserv); a read's high half the long word's, its low half latched
  // for the read of it (latrdata)
  struct GpuPort
  {
    std::uint16_t lodata = 0;
    std::uint16_t latrdata = 0;
    std::uint32_t rddata = 0; // the long word a read's high half takes, read as the port serves it
    bool ioserv = false;
    std::uint32_t ioAddress = 0;
    std::uint32_t ioData = 0;
  } mGpuPort;

  Timers mTimers;
  JerryInterrupts mJerryInts{ mTimers };
  Eeprom mEeprom;
  std::uint8_t mJoyLatch = 0xff; // u374
  bool mJoyEnable = false;       // joyen

  std::array<std::uint32_t, 2> mPads{};

  // A port's six rows, row 5 the high bit, for the column its half of the latch selects.
  std::uint32_t padRows( std::size_t port ) const
  {
    std::uint32_t const latch = mJoyEnable ? mJoyLatch : 0xffu;
    // port 2's column select is the latch's high half reversed
    std::uint32_t const column =
        port == 0 ? latch & 0xfu
                  : ( latch >> 7 & 1u ) | ( latch >> 5 & 2u ) | ( latch >> 3 & 4u ) | ( latch >> 1 & 8u );
    std::uint32_t const b = mPads[port];
    auto const bit = [b]( int n ) { return b >> n & 1u; };
    std::uint32_t held = 0;
    if ( ( column & 8u ) == 0 ) // #, 9, 6, 3, Option
      held = bit( 20 ) << 5 | bit( 17 ) << 4 | bit( 14 ) << 3 | bit( 11 ) << 2 | bit( 7 ) << 1;
    else if ( ( column & 4u ) == 0 ) // 0, 8, 5, 2, C
      held = bit( 18 ) << 5 | bit( 16 ) << 4 | bit( 13 ) << 3 | bit( 10 ) << 2 | bit( 6 ) << 1;
    else if ( ( column & 2u ) == 0 ) // *, 7, 4, 1, B
      held = bit( 19 ) << 5 | bit( 15 ) << 4 | bit( 12 ) << 3 | bit( 9 ) << 2 | bit( 5 ) << 1;
    else if ( ( column & 1u ) == 0 ) // up, down, left, right, A, Pause
      held = bit( 3 ) << 5 | bit( 2 ) << 4 | bit( 1 ) << 3 | bit( 0 ) << 2 | bit( 4 ) << 1 | bit( 8 );
    return ~held & 0x3fu;
  }

  // joy[8 + n] is port 1's row 5 - n, joy[12 + n] port 2's
  static std::uint8_t padHigh( std::uint32_t one, std::uint32_t two )
  {
    std::uint32_t high = 0;
    for ( int n = 0; n < 4; ++n )
      high |= ( one >> ( 5 - n ) & 1u ) << n | ( two >> ( 5 - n ) & 1u ) << ( 4 + n );
    return static_cast<std::uint8_t>( high );
  }

  void joystickWrite( std::uint16_t value )
  {
    mJoyLatch = static_cast<std::uint8_t>( value );
    mJoyEnable = ( value & 0x8000u ) != 0;
  }

  Tom mTom;
  std::uint64_t mCycle = 0;        // the next cycle to step
  std::uint64_t mEnd = UINT64_MAX; // the run's end, with the emulated 68000
  std::uint32_t mRestSteps = 0;    // cycles stepped at rest in a row
  // by master: the first cycle its processor has not handed over, and its request not yet stepped;
  // the emulated 68000's horizon after the RISCs'
  std::uint64_t mHorizon[3] = { UINT64_MAX, UINT64_MAX, UINT64_MAX };
  std::uint64_t mPendingCycle[2] = { UINT64_MAX, UINT64_MAX };
  GatewayRequest mPending[2]{};
  bool mGpuAtJerry = false;
  std::uint64_t mObjectGpuIrq = UINT64_MAX;

  // the blitter, the GPU's writes to it waiting for their cycle, its interrupt's cycle
  struct BlitWrite
  {
    std::uint64_t cycle;
    std::uint32_t address;
    std::uint32_t value;
  };

  Blitter mBlitter;
  std::deque<BlitWrite> mBlitWrites;
  std::uint64_t mBlitGpuIrq = UINT64_MAX;
  std::uint64_t mGpuCpuIrq = UINT64_MAX;
  std::uint64_t mDspCpuIrq = UINT64_MAX;

  // Tom's data bus (dbus.v): its byte lanes as last latched (stdata), the high byte the first in
  // memory
  std::uint64_t mDataBus = 0;
  bool mGpuDrive = false; // the GPU's gateway drives it in the next cycle with this (wden)
  bool mOpDrive = false;  // the object processor drives it in the next cycle
  std::uint64_t mGpuDriveData = 0;

  // --- The control state: every flip-flop that steers the bus, apart from the data it moves and the
  // addresses it moves it at. Times in it are relative to the cycle the bus steps next. Bytes and one
  // half word, without padding, so that two states compare and hash as their bytes.
public:
  struct Control
  {
    std::uint16_t arbAck = 1; // arb.v ack_[9:0]: the master granted

    // the 68000's bus arbitration (68k.v w268, w269, w270, BG), the board's xbgl flip-flop, the
    // 68000's AS and R/W; the boot program's 68000's progress through its last bus cycles
    struct M68k
    {
      std::uint8_t phase = 0; // the cycle's half of the 68000 clock: 0 high (clk2), 1 low (clk1)
      std::uint8_t step = 0;
      std::uint8_t br0 = 0, br1 = 0, br2 = 0;
      std::uint8_t bga0 = 0, bga1 = 0, bga2 = 0;
      std::uint8_t st0 = 7, st1 = 7;
      bool bg = true;    // BG, active low
      bool xbgl = false; // the 68000 has granted the bus
      bool as = true;    // AS, active low
      bool read = false;
      bool asOff = false; // AS rises after DTACK, in `asLeft` cycles
      std::uint8_t asLeft = 0;
    } m68k;

    // j_jmem.v master side and j_jbus.v's latches (dsp16 and bigend set); the progress of a store
    struct Jerry
    {
      bool q0 = true;
      bool q1 = false;
      bool q2 = false;
      bool q3 = false;
      bool q4 = false;
      bool readsi = false;       // reads
      std::uint8_t ws = 1;       // width latch
      std::uint8_t dbrls = 3;    // bus requests as Tom sees them, active low
      bool storePending = false; // a store started, its data taken in the next cycle
      std::uint8_t storeSize = 0;
      std::uint8_t storeHalves = 0; // still to be written
    } jerry;

    std::uint8_t qs = 1; // j_jmem.v slave side, qs0 to qs4, one hot

    // arb.v (brs is always clear: Tom's own bus request is the only one on the line)
    struct Arbiter
    {
      bool q0 = true;
      bool q1 = false;
      bool q2 = false;
      bool intbms = false;
      bool dbg = false; // the DSP has the bus (dbgl low)
    } arb;

    // cpu.v
    struct CpuIf
    {
      bool q11 = false;
      bool q12 = false;
      bool q13 = false;
    } cpu;

    // mem.v; who the cycle under way was started for, its width, direction and whether its data is
    // justified (bus.v)
    struct Memory
    {
      bool ack = true; // mtb
      bool idle = true;
      bool q1a = false, q1b = false, q1c = false, q1d = false;
      bool q2a = false, q2b = false, q2c = false;
      bool q3a = false, q3b = false;
      bool q4a = false, q4b = false, q4c = false, q4d = false, q4e = false, q4f = false, q4g = false, q4h = false;
      bool q5a = false, q5b = false, q5c = false;
      bool q7a = false, q7b = false;
      bool q8a = false, q8b = false, q8c = false;
      bool readsi = false;
      std::uint8_t mws = 0;
      std::uint8_t wq = 0;
      bool gpuCycle = false; // the GPU (its grant may have moved on)
      bool opCycle = false;  // the object processor
      bool opRead = true;
      bool blitCycle = false; // the blitter
      bool m68kCycle = false; // the emulated 68000
      bool ourack = false;    // abus.v ourack: the 68000 acknowledges an interrupt (intd, latched)
      bool busRead = true;
      bool busJustify = false;
      std::uint8_t busWidth = 0;
    } mem;

    // memwidth.v
    struct Width
    {
      std::uint8_t pw = 0;
      std::uint8_t maskai = 0;
      std::uint8_t maskw = 0;
    } width;

    // abus.v: the banks an access selects, whether their open rows are valid
    struct Decoder
    {
      bool bs2 = false;
      bool bs3 = false;
      bool valid0 = false;
      bool valid1 = false;
    } dec;

    // misc.v refresh: the count of refreshes owed, the request
    struct Refresh
    {
      std::uint8_t rc = 0;
      bool rq = false;
    } ref;

    // gateway.v, by master (no program fetch from outside, no bus hog): a cycle pending, the access's
    // direction and size, a fetch of the program (progown, progownp; packwt, greqt2, gatereq)
    struct Gateway
    {
      bool cycpend = false;
      bool readp = false;
      bool reada = false;
      std::uint8_t msized = 0;
      bool active = false;
      bool dackwt = false;
      bool xldReady = false;
      bool progown = false;
      bool progownp = false;
      bool packwt = false;
      bool greqt2 = false;
      bool gatereq = false;
    } gw[2];
  };

private:
  // The state the bus is in: a node's of the automaton, or mOwn
  Control mOwn;
  Control const* mCtl = &mOwn;
  std::unique_ptr<Automaton> mAuto;
  std::unique_ptr<Gathered> mGathered; // a cycle's inputs, made again in place every cycle
  Node* mNode = nullptr;               // the node of the state, null where it is to be looked up
  bool mFlushWanted = false;
  std::function<void( std::uint64_t )> mOthers;
  UndrivenReadSink mUndrivenRead;
  bool mHold = false; // the RISCs run ahead: the bus steps nothing
  // Tom's interrupt handler as it holds before mIhandlerUntil (Tom::ihandlerSteady); 0 once a write
  // or an interrupt may have changed it
  std::uint64_t mIhandlerUntil = 0;
  bool mIhandler = false;
  // the state to change outside a cycle (a rest, the constructor): mOwn, the node left
  Control& ownState();

  // --- The data the control steers ------------------------------------------------------------------

  // gateway.v, by master: the access's address and data
  struct Gateway
  {
    bool dmaen = false;
    std::uint32_t datAddr = 0;
    std::uint32_t datPc = 0; // the instruction of the access at datAddr
    std::uint32_t loadData = 0;
    bool maskbN = false;
    bool maskwN = false;
    // the access the gateway serves, its data moved when the gateway takes it
    std::uint32_t pendData = 0;
    // the GPU's (JERRY 0): hirdata, the high half of a phrase; a store waiting for its memory cycle
    std::uint32_t hirdata = 0;
    std::uint32_t pendHi = 0;
    bool store = false;
    std::uint32_t storeAddress = 0;
    std::uint32_t storeSize = 0;
    std::uint32_t storeData = 0;
    std::uint32_t storeHi = 0;
    std::uint32_t storeHalves = 0; // a store to a 16 bit device: the cycles still to come
    std::uint32_t loadHalves = 0;  // a load of Tom's registers: the halves still to latch, its
    std::uint32_t loadAddress = 0; // address and size (the gateway may hold the next request's)
    std::uint32_t loadSize = 0;
    std::uint32_t loadPc = 0;
    std::uint32_t cpuAddr = 0; // a fetch's address (cpu_addr), apart from a load's or store's
    std::uint32_t progData = 0;
  } mGw[2];

  // j_jbus.v's address latch, a store's address and data, the low half of the data bus as Jerry
  // last drove it
  struct Jerry
  {
    std::uint32_t ad = 0;
    std::uint32_t adPc = 0; // the DSP's instruction of the access at ad
    std::uint32_t storeAddress = 0;
    std::uint32_t storeData = 0;
    std::uint16_t openLow = 1; // (the 68000's DSPGO)
  } mJerry;

  // j_jmem.v slave side and gpu_cpu.v of the DSP: Tom's I/O cycles to Jerry from another master, a
  // half at a time
  struct Slave
  {
    std::uint32_t address = 0; // the half on the bus: its address, its data, written
    std::uint16_t data = 0;
    bool write = false;
    std::uint16_t lodata = 0;           // the high half of a long word, taken first (lodld)
    std::uint16_t latrdata = 0;         // a read's low half, for the read of it
    std::uint64_t rdCycle = UINT64_MAX; // the cycle the DSP's memory controller serves a read in
    std::uint32_t rdAddress = 0;
    std::uint32_t rddata = 0; // the long word it read
    bool ioserv = false;      // the DSP's memory controller serves the write (ioservt)
    std::uint32_t ioAddress = 0;
    std::uint32_t ioData = 0;
  } mSlave;

  // MEMCON1 and MEMCON2 (abus.v): as the boot program leaves them, or as reset and the board's
  // configuration pins (jaguar.v xma_in: ROMHI, an 8 bit ROM, big endian) set them; mset once
  // MEMCON1 is written
  struct MemCon
  {
    bool mset = true;
    bool romhi = true;
    std::uint32_t romwid = 0;
    std::uint32_t romspd = 0;
    std::uint32_t dspd = 3;
    std::uint32_t iospd = 3;
    bool fastrom = false;
    std::uint32_t cols0 = 0;
    std::uint32_t dwid0 = 3;
    std::uint32_t cols1 = 0;
    std::uint32_t dwid1 = 3;
    std::uint32_t refrate = 5;
    bool hilo = false;
    bool bigend = true;
  } mCon;

  // the board's decoding of an address by its bits 15 to 23, as MEMCON stands (redecode)
  std::array<std::uint16_t, 512> mDecode{};
  void redecode();

  // The OS ROM and the cartridge's ROM, with Boot::Reset
  std::vector<std::uint8_t> mRom;
  std::vector<std::uint8_t> mCart;

  // The emulated 68000's bus cycle (Boot::Reset): what it asked for, whether it is under way, the
  // cycle Tom's DTACK was taken in and a read's data; whether a write has reached its memory
  struct M68kCycle
  {
    M68kAccess access;
    bool pending = false;
    std::uint64_t dtack = 0;
    std::uint16_t readData = 0;
    bool written = false;
    bool served = false; // Tom's memory controller has started a cycle for it
  } mM68k;

  bool mEmulated68k = false;

  // the address of the cycle under way, the object processor's phrase's (mem.v, bus.v)
  struct BusAddress
  {
    std::uint32_t bus = 0;
    std::uint32_t op = 0;
  } mAddr;

  // abus.v: the latched address (aouti and aout_obuf, bits 3 to 23), the open DRAM rows
  struct Rows
  {
    std::uint32_t aout = 0;
    std::uint32_t bank0 = 0;
    std::uint32_t bank1 = 0;
  } mRows;

  // misc.v refresh: the prescaler and the period counter, which count every cycle
  struct RefreshCount
  {
    std::uint32_t ps = 0;
    std::uint32_t rp = 0;
  } mRefCount;

  // the 68000's address lines; the boot program's 68000's prefetch start and the cycle it holds its
  // grant back in (w395)
  struct M68kPins
  {
    std::uint32_t address = 0;
    std::uint64_t readStart = 8;
    std::uint64_t holdBack = 7;
  } mPins;
};

} // namespace jag
