#include "jag/MainBus.hpp"

#include "jag/Risc.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <span>
#include <type_traits>
#include <unordered_map>

namespace jag
{

namespace
{

constexpr bool bit( std::uint32_t v, std::uint32_t n )
{
  return ( v >> n ) & 1u;
}

// The GPU's local space as Tom's internal I/O cycles reach it: its registers, the blitter's, its RAM.
// Jerry's slave asks the DSP's local bus for a read two cycles after Tom's I/O cycle starts (qs1)
constexpr std::uint64_t DSP_READ_IOREQ = 2;

// abus.v gpuadd: Tom's space with address bit 13 or 14, the GPU's local space; address bit 15 makes
// the port's 32 bit writes (gpu_cpu.v at[15]) and is not passed on: $F0B000 is $F03000
constexpr bool gpuSpace( std::uint32_t address )
{
  return ( address & 0xff0000u ) == 0xf00000u && ( address & 0x6000u ) != 0;
}

constexpr std::uint32_t gpuPortAddress( std::uint32_t address )
{
  return 0xf00000u | ( address & 0x7ffcu );
}

// abus.v's decoding of an address on the board's bus, by MEMCON1's ROMHI (the ROM and the
// devices, the cartridge and the two DRAM banks trade places) and, until MEMCON1 is first written
// (mset), everything the ROM but the devices; the memory's width (mw: 0 8 bits to 3 64 bits)
struct Decoded
{
  bool abs0;   // the ROM and the devices
  bool cart;   // abs1
  bool ram1;   // abs2
  bool ram0;   // abs3
  bool intdev; // Tom's registers, $F00000 to $F0FFFF
  bool extdev; // Jerry's, $F10000 to $F1FFFF
  bool from;   // the ROM or the cartridge
  std::uint32_t mw;
};

Decoded
decode( std::uint32_t ab, bool mset, bool romhi, std::uint32_t romwid, std::uint32_t dwid0, std::uint32_t dwid1 )
{
  std::uint32_t const top3 = ab >> 21;
  std::uint32_t const top2 = ab >> 22;
  bool const romlo = !romhi;
  bool const notdev = ( ( ab >> 17 ) & 0xf ) != 0x8;
  bool const dev32 = ( ( ab >> 15 ) & 0x3f ) == 0x21;
  bool const dev16 = ( ( ab >> 15 ) & 0x3f ) == 0x20 || ( ( ab >> 16 ) & 0x1f ) == 0x11;
  bool const low = ( romlo && top3 == 0 ) || ( romhi && top3 == 7 ); // the ROM's and the devices' half
  Decoded d{};
  d.abs0 = !mset || low;
  d.cart = mset && ( ( romlo && ( top3 == 1 || top2 == 1 ) ) || ( romhi && ( top3 == 6 || top2 == 2 ) ) );
  d.ram1 = mset && ( ( romlo && top2 == 2 ) || ( romhi && top2 == 1 ) );
  d.ram0 = mset && ( ( romlo && top2 == 3 ) || ( romhi && top2 == 0 ) );
  d.intdev = d.abs0 && ( ( ab >> 16 ) & 0x1f ) == 0x10;
  d.extdev = d.abs0 && ( ( ab >> 16 ) & 0x1f ) == 0x11;
  d.from = ( d.abs0 && notdev ) || d.cart;
  std::uint32_t const rom = d.abs0 && notdev ? romwid : 0u;
  std::uint32_t const dev = d.abs0 && dev16 ? 1u : d.abs0 && dev32 ? 2u : 0u;
  d.mw = rom | dev | ( d.cart ? romwid : 0u ) | ( d.ram1 ? dwid1 : 0u ) | ( d.ram0 ? dwid0 : 0u );
  return d;
}

// A decoding as MainBus::mDecode keeps it: the seven places a bit each, the width above them.
std::uint16_t pack( Decoded const& d )
{
  auto const b = []( bool v ) { return static_cast<std::uint32_t>( v ); };
  return static_cast<std::uint16_t>( b( d.abs0 ) | b( d.cart ) << 1 | b( d.ram1 ) << 2 | b( d.ram0 ) << 3 |
                                     b( d.intdev ) << 4 | b( d.extdev ) << 5 | b( d.from ) << 6 | ( d.mw & 3u ) << 7 );
}

Decoded unpack( std::uint16_t e )
{
  Decoded d{};
  d.abs0 = e & 1u;
  d.cart = ( e >> 1 ) & 1u;
  d.ram1 = ( e >> 2 ) & 1u;
  d.ram0 = ( e >> 3 ) & 1u;
  d.intdev = ( e >> 4 ) & 1u;
  d.extdev = ( e >> 5 ) & 1u;
  d.from = ( e >> 6 ) & 1u;
  d.mw = ( e >> 7 ) & 3u;
  return d;
}

// The decoding of the board's address `ab`, from the table MEMCON's last values made: it depends on
// address bits 15 to 23 alone.
Decoded lookup( std::array<std::uint16_t, 512> const& table, std::uint32_t ab )
{
  return unpack( table[( ab & 0xffffffu ) >> 15] );
}

// The 68000's last bus cycles after it sets DSPGO (sim/sim_main.cpp: the second word of
// move.l #1,D_CTRL is being written, then the prefetch of the word after STOP is read from ROM).
constexpr std::uint32_t M68K_ROM_READ = 0xe00060;

// w270: the 68000's bus arbitration state, from the one before it and BR, BGACK (68k.v
// br_fsm_cases). 7 is idle; BG is asserted (low) in the states with bit 0 clear.
constexpr std::uint32_t arbitrationLogic( std::uint32_t state, bool br, bool bgack, bool w395 )
{
  bool c[13];
  c[0] = state == 7 && !w395 && br && !bgack;
  c[1] = state == 1 && br;
  c[2] = state == 4;
  c[3] = state == 2 && br;
  c[4] = state == 6;
  c[5] = state == 0 && br && !bgack;
  c[6] = state == 7 && !w395 && bgack;
  c[7] = state == 5 && bgack;
  c[8] = state == 1 && !br && bgack;
  c[9] = state == 2 && !br && bgack;
  c[10] = state == 2 && br && !bgack;
  c[11] = state == 0 && bgack;
  c[12] = state == 5 && br;
  bool const b0 = !( c[0] || c[1] || c[2] || c[3] || c[4] || c[5] );
  bool const b1 = !( c[1] || c[4] || c[5] || c[6] || c[7] || c[8] || c[9] || c[10] || c[11] || c[12] );
  bool const b2 = !( c[2] || c[3] || c[4] || c[5] || c[6] || c[7] || c[8] || c[12] );
  return ( b0 ? 1u : 0u ) | ( b1 ? 2u : 0u ) | ( b2 ? 4u : 0u );
}

// the same for every state and input, worked out once
constexpr auto ARBITRATION = []
{
  std::array<std::uint8_t, 64> table{};
  for ( std::uint32_t k = 0; k < 64; ++k )
    table[k] =
        static_cast<std::uint8_t>( arbitrationLogic( k & 7u, ( k & 8u ) != 0, ( k & 16u ) != 0, ( k & 32u ) != 0 ) );
  return table;
}();

std::uint32_t arbitrationNext( std::uint32_t state, bool br, bool bgack, bool w395 )
{
  return ARBITRATION[( state & 7u ) | ( br ? 8u : 0u ) | ( bgack ? 16u : 0u ) | ( w395 ? 32u : 0u )];
}

// memwidth.v's byte mask on the 64 bit DRAM, as lanes of the data bus: the bytes from `offset` for
// `width`, within the phrase; the first byte in memory is the high lane (big endian).
std::uint64_t byteLanes( std::uint32_t offset, std::uint32_t width )
{
  std::uint64_t lanes = 0;
  for ( std::uint32_t b = offset; b < 8 && b < offset + width; ++b )
    lanes |= std::uint64_t{ 0xff } << ( 56 - 8 * b );
  return lanes;
}

// bus.v's pixel widths (pws8, pws16, pws64) latched as a cycle starts: a justified access of a
// byte, a word or a phrase, else of a long word; an access not justified is the phrase of an
// internal master.
struct PixelWidth
{
  bool w8, w16, w64;
};

PixelWidth pixelWidth( std::uint32_t width, bool justify )
{
  return { justify && ( width & 0xeu ) == 0, justify && width == 2, ( justify && bit( width, 3 ) ) || !justify };
}

// down.v: what a read of the 64 bit DRAM puts in the low long word of the data bus (dmuxd)
std::uint64_t downShift( std::uint64_t d, std::uint32_t offset, std::uint32_t width, bool justify )
{
  PixelWidth const p = pixelWidth( width, justify );
  std::uint32_t const ba = offset ^ 7u; // big endian
  std::uint32_t const dmuxd = ( !p.w64 && bit( ba, 2 ) ? 4u : 0u ) | ( ( p.w8 || p.w16 ) && bit( ba, 1 ) ? 2u : 0u ) |
                              ( p.w8 && bit( ba, 0 ) ? 1u : 0u );
  auto const field = [d]( std::uint32_t hi, std::uint32_t lo )
  {
    return static_cast<std::uint32_t>( d >> lo ) &
           static_cast<std::uint32_t>( ( std::uint64_t{ 1 } << ( hi - lo + 1 ) ) - 1 );
  };
  std::uint32_t low = 0;
  switch ( dmuxd )
  {
  case 0:
    low = field( 31, 0 );
    break;
  case 1:
    low = field( 31, 16 ) << 16 | field( 15, 8 ) << 8 | field( 15, 8 );
    break;
  case 2:
    low = field( 31, 16 ) << 16 | field( 31, 16 );
    break;
  case 3:
    low = field( 31, 16 ) << 16 | field( 31, 24 ) << 8 | field( 31, 24 );
    break;
  case 4:
    low = field( 63, 32 );
    break;
  case 5:
    low = field( 63, 48 ) << 16 | field( 47, 40 ) << 8 | field( 47, 40 );
    break;
  case 6:
    low = field( 63, 48 ) << 16 | field( 63, 48 );
    break;
  default:
    low = field( 63, 48 ) << 16 | field( 63, 56 ) << 8 | field( 63, 56 );
    break;
  }
  return ( d & 0xffffffff00000000u ) | low;
}

// up.v: a write's data on the data bus, its low bytes repeated in the lanes above (dmuxu)
std::uint64_t upShift( std::uint64_t d, std::uint32_t width, bool justify )
{
  PixelWidth const p = pixelWidth( width, justify );
  std::uint32_t const dmuxu = ( !p.w64 ? 4u : 0u ) | ( p.w8 || p.w16 ? 2u : 0u ) | ( p.w8 ? 1u : 0u );
  std::uint64_t const b0 = d & 0xff, w0 = d & 0xffff, l0 = d & 0xffffffffu;
  switch ( dmuxu )
  {
  case 0:
    return d;
  case 1:
    return ( d & 0xffffffffffff0000u ) | b0 << 8 | b0;
  case 2:
    return ( d & 0xffffffff00000000u ) | w0 << 16 | w0;
  case 3:
    return ( d & 0xffffffff00000000u ) | b0 * 0x01010101u;
  case 4:
    return l0 << 32 | l0;
  case 5:
    return ( ( d >> 16 ) & 0xffff ) << 48 | b0 << 40 | b0 << 32 | ( ( d >> 16 ) & 0xffff ) << 16 | b0 << 8 | b0;
  case 6:
    return w0 * 0x0001000100010001u;
  default:
    return b0 * 0x0101010101010101u;
  }
}

// memwidth.v: the `k`th 16 bit cycle of a long word at `address` to a 16 bit device, at the next
// word of its phrase each (maska + maskw, three bits: the phrase's end wraps to its start)
std::uint32_t longHalf( std::uint32_t address, std::uint32_t k )
{
  return ( address & ~7u ) | ( ( ( address & 6u ) + 2 * k ) & 6u );
}

// the word of a long word that a 16 bit cycle at `address` takes: the long word is on both halves
// of the data bus (up.v), so a word two past a long word's start has its low half
std::uint16_t longLane( std::uint32_t address, std::uint32_t data )
{
  return static_cast<std::uint16_t>( bit( address, 1 ) ? data : data >> 16 );
}

} // namespace

namespace
{

GatewayRequest const NO_REQUEST{};

} // namespace

// A span's data moves, in the order a cycle makes them: kind, the cycle's offset from the span's
// start (bits 8 to 15), an argument (from bit 16)
enum SpanAction : std::uint32_t
{
  S_DELAY,      // the 68000's S0 put off two cycles (w432)
  S_PINS,       // the 68000 drives its address
  S_DTACK,      // Tom's DTACK taken: the bus cycle ends
  S_LATCH_DRAM, // the data bus's lanes take a DRAM read (the width)
  S_LATCH_ROM,  // the data bus's lanes take a ROM read (maskai, maskw << 4)
  S_READ,       // the 68000's read takes its data
  S_WRITE,      // the 68000's write reaches the DRAM
  S_OP,         // the object processor clocked, the line buffers' bus handed to Tom
  S_OP_CLOCK,   // the object processor clocked
  S_ROWS,       // the open rows (bs3, bs2 << 1)
  S_AOUT,       // the address held (its source)
  S_START,      // a cycle started (the address's source, the 68000's cycle << 2)
};

// where the address on the board's bus comes from: the 68000, Tom's held address, nothing
enum AddressSource : std::uint8_t
{
  FROM_68000,
  FROM_AOUT,
  FROM_NONE,
  FROM_OTHER
};

struct MainBus::Span
{
  std::uint32_t length = 0;
  std::uint32_t restSteps = 0;
  // the refresh counters it started from: the cycles before the prescaler's end (pen), the period
  // counter at zero there; both, where it ends twice within the span
  std::uint32_t toPen = 0;
  bool rpZero = false;
  std::uint32_t ps = 0;
  std::uint32_t rp = 0;
  Node* end = nullptr;
  std::vector<std::uint32_t> actions;

  // The refresh counters now as good as those it started from: what they do within its length
  bool fits( std::uint32_t pen, std::uint32_t ps0, std::uint32_t rp0 ) const
  {
    if ( toPen >= length )
      return pen >= length;
    if ( toPen + 64 >= length )
      return pen == toPen && ( rp0 == 0 ) == rpZero;
    return ps0 == ps && rp0 == rp;
  }
};

// The spans recorded from a state for one set of the other inputs, told apart by the refresh
struct MainBus::SpanGroup
{
  std::uint64_t key = 0;
  std::vector<Span> spans;
};

// What a span's replay changes of the data, to compare with what the cycles stepped change
struct MainBus::SpanCheck
{
  std::uint64_t cycle;
  Node* node;
  std::uint64_t dataBus;
  BusAddress addr;
  Rows rows;
  RefreshCount refresh;
  bool pending, written, served;
  std::uint64_t dtack, start;
  std::uint16_t readData;
  std::uint32_t pins;
  std::uint64_t horizon;
  std::uint32_t restSteps;
};

// What a cycle's control reads besides the bus's own state: the requests the RISCs hand it, the
// 68000's arbitration and pins as the cycle leaves them (worked out first: what drives the board's
// bus follows from them), the object processor's and the blitter's outputs, Tom's interrupt
// handler, the refresh counters, and where the address on the bus decodes to.
struct MainBus::Gathered
{
  GatewayRequest const* request[2] = { &NO_REQUEST, &NO_REQUEST };
  bool dmaen[2] = {};

  // the 68000's as the cycle starts: its cycle under way, where the cycle is in it (the cycles from
  // its start: 4 or more before, 4 to 1 before, its first and second, then even or odd), what it
  // asks; the boot program's grant held back, its prefetch's start
  struct Raw
  {
    bool pending = false;
    std::uint8_t pos = 0;
    bool read = false;
    bool uds = false;
    bool lds = false;
    bool held = false;
    bool holdAs = false;
    bool holdBack = false;
    bool readStart = false;
  } raw;

  Control::M68k m68k;
  bool uds68 = true; // the emulated 68000's strobes, asserted
  bool lds68 = true;
  bool fc7 = false;
  bool opRuns = false;
  ObjectProcessor::Inputs oi{};
  ObjectProcessor::Outputs oo{};
  bool blitRuns = false;
  Blitter::Inputs bi{};
  Blitter::Outputs bo{};
  bool ihandler = false;
  bool pen = false;    // misc.v: the prescaler at its end
  bool rpZero = false; // the period counter at zero
  bool dreqin = false;
  bool rwin = false;
  std::uint32_t sizin = 0;
  std::uint32_t ain = 0xffffff; // the board's address lines, pulled up while nothing drives them
  std::uint8_t ainSource = FROM_NONE;
  std::uint32_t ab = 0xffffff;
  bool mreqb = false; // the internal bus's request: the external master's, or another's
  Decoded where{};
  bool row0 = false; // the address's row is bank 0's open one (with ack and mreqb)
  bool row1 = false;
};

// What the control tells the data side of a cycle, and its next state.
struct MainBus::Signals
{
  bool rwIn = false; // the direction on Tom's bus
  bool d7a = false;  // an internal I/O cycle starts
  bool readt = false;
  std::uint8_t ba1 = 0; // memwidth.v's byte address
  bool jack = false;    // Jerry's latches take the gateway's
  bool jerryStart = false;
  std::uint8_t jerryHalves = 0; // a Jerry store's halves still to write, before this cycle's
  bool d10 = false;             // an interrupt's acknowledge
  bool gateways = false;        // a gateway moves an address or data (Gateway below)

  struct Gateway
  {
    bool external = false;
    bool gpuMemw = false;
    bool gpuAck = false;
    bool ddatld = false;
    bool pdatld = false;
  } gw[2];
};

// --- the bus's timing automaton ---------------------------------------------------------------------
// The control state as a node; each transition recorded from it, for the inputs of a cycle (keyOf),
// to the next state, with what the control told the data side then. A cycle whose transition is
// known is the data side alone.
struct MainBus::Node
{
  struct Transition
  {
    std::uint64_t key = ~std::uint64_t{ 0 }; // no key: they have 53 bits
    Node* next = nullptr;
    Signals signals;
  };

  // the transition taken last, kept in the node: most cycles take the same again
  Transition last;
  Control state;
  bool restful = false; // MainBus::restful( state )
  bool quiet = false;   // MainBus::quiet( state )
  std::vector<Transition> transitions;
  std::vector<SpanGroup> spanGroups;
  std::size_t lastGroup = 0;

  Transition const* find( std::uint64_t key )
  {
    if ( last.key == key )
      return &last;
    for ( Transition const& t : transitions )
      if ( t.key == key )
      {
        last = t;
        return &last;
      }
    return nullptr;
  }
};

// The nodes the automaton keeps at most (some 300,000 for a game's first minute)
constexpr std::size_t MAX_NODES = std::size_t{ 1 } << 20;

struct MainBus::Automaton
{
  struct Hash
  {
    std::size_t operator()( Control const& c ) const
    {
      unsigned char const* p = reinterpret_cast<unsigned char const*>( &c );
      std::uint64_t h = 0x9e3779b97f4a7c15u;
      std::size_t i = 0;
      for ( ; i + 8 <= sizeof c; i += 8 )
      {
        std::uint64_t w;
        std::memcpy( &w, p + i, 8 );
        h = ( h ^ w ) * 0xff51afd7ed558ccdu;
        h ^= h >> 32;
      }
      std::uint64_t w = 0;
      std::memcpy( &w, p + i, sizeof c - i );
      h = ( h ^ w ) * 0xc4ceb9fe1a85ec53u;
      return static_cast<std::size_t>( h ^ ( h >> 29 ) );
    }
  };

  struct Equal
  {
    bool operator()( Control const& a, Control const& b ) const
    {
      return std::memcmp( &a, &b, sizeof a ) == 0;
    }
  };

  std::unordered_map<Control, Node*, Hash, Equal> nodes;
  std::deque<Node> store;
};

static_assert( std::has_unique_object_representations_v<MainBus::Control>,
               "the control state compares and hashes as its bytes" );

MainBus::~MainBus() = default;

void MainBus::setAutomaton( bool on )
{
  static_assert( std::has_unique_object_representations_v<Signals>, "the signals compare as their bytes" );
  ownState();
  if ( on && !mAuto )
    mAuto = std::make_unique<Automaton>();
  else if ( !on )
    mAuto.reset();
}

bool MainBus::restful( Control const& k )
{
  auto const idle = []( Control::Gateway const& gw )
  { return !gw.active && !gw.dackwt && !gw.xldReady && !gw.cycpend && !gw.packwt && !gw.greqt2 && !gw.gatereq; };
  return idle( k.gw[DSP] ) && idle( k.gw[GPU] ) && k.qs == 1u && k.jerry.q0 && !k.jerry.storePending &&
         k.jerry.storeHalves == 0 && ( k.jerry.dbrls & 1u ) && k.arb.q0 && !k.arb.dbg && !k.cpu.q11 && !k.cpu.q12 &&
         !k.cpu.q13 && k.mem.idle && k.mem.ack && k.mem.wq == 0 && !k.ref.rq && !bit( k.ref.rc, 3 ) &&
         k.m68k.step >= 3 && k.m68k.st0 == 7 && k.m68k.st1 == 7 && k.m68k.br0 == 0 && k.m68k.br1 == 0 &&
         k.m68k.br2 == 0 && k.m68k.bga0 == 0 && k.m68k.bga1 == 0 && k.m68k.bga2 == 0 && k.m68k.bg && k.m68k.as;
}

// MEMCON1 or MEMCON2 written: the control's transitions depend on them. They are dropped as the
// next cycle starts, the state the cycle leaves taken out of its node first.
void MainBus::flushAutomaton()
{
  mFlushWanted = mAuto != nullptr;
}

MainBus::Node& MainBus::nodeFor( Control const& state )
{
  Node*& slot = mAuto->nodes[state];
  if ( !slot )
  {
    // a bound on the memory it takes: past it, the automaton starts again as the next cycle does
    if ( mAuto->store.size() >= MAX_NODES )
      mFlushWanted = true;
    slot = &mAuto->store.emplace_back();
    slot->state = state;
    slot->restful = restful( state );
    slot->quiet = quiet( state );
  }
  return *slot;
}

// The cycle's inputs as the control reads them: the RISCs' requests, the 68000's (its cycle under
// way, where the cycle is in it, its strobes, its function code; the boot program's times), Tom's
// interrupt handler, the object processor's and the blitter's requests, the refresh counters, and
// where the address on the bus decodes to: what it selects, its row, its low bits. Everything else
// the control reads follows from these and its own state.
std::uint64_t MainBus::keyOf( Gathered const& g ) const
{
  auto const b = []( bool v ) { return static_cast<std::uint64_t>( v ); };
  GatewayRequest const& d = *g.request[DSP];
  GatewayRequest const& p = *g.request[GPU];
  Gathered::Raw const& r = g.raw;
  return b( d.valid ) | b( d.write ) << 1 | b( d.program ) << 2 | std::uint64_t{ d.size & 3u } << 3 |
         b( g.dmaen[DSP] ) << 5 | b( p.valid ) << 6 | b( p.write ) << 7 | b( p.program ) << 8 |
         std::uint64_t{ p.size & 3u } << 9 | b( g.dmaen[GPU] ) << 11 | b( r.pending ) << 12 |
         std::uint64_t{ r.pos & 15u } << 13 | b( r.read ) << 17 | b( r.uds ) << 18 | b( r.lds ) << 19 |
         b( r.held ) << 20 | b( r.holdAs ) << 21 | b( g.fc7 ) << 22 | b( r.holdBack ) << 23 | b( r.readStart ) << 24 |
         b( g.ihandler ) << 25 | b( g.oo.obbreq ) << 26 | b( g.oo.mreq ) << 27 | b( g.oo.read ) << 28 |
         b( g.oo.startref ) << 29 | std::uint64_t{ g.bo.breq & 3u } << 30 | b( g.bo.mreq ) << 32 |
         b( g.bo.read ) << 33 | b( g.bo.justify ) << 34 | std::uint64_t{ g.bo.width & 15u } << 35 | b( g.pen ) << 39 |
         b( g.rpZero ) << 40 | b( g.where.ram0 ) << 41 | b( g.where.ram1 ) << 42 | b( g.where.intdev ) << 43 |
         b( g.where.extdev ) << 44 | b( g.where.from ) << 45 | std::uint64_t{ g.where.mw & 3u } << 46 |
         b( g.row0 ) << 48 | b( g.row1 ) << 49 | std::uint64_t{ mCtl->mem.ack ? g.ab & 7u : 0u } << 50;
}

void MainBus::redecode()
{
  for ( std::uint32_t i = 0; i < mDecode.size(); ++i )
    mDecode[i] = pack( decode( i << 15, mCon.mset, mCon.romhi, mCon.romwid, mCon.dwid0, mCon.dwid1 ) );
}

MainBus::MainBus( ExternalBus& memory, Boot boot, std::vector<std::uint8_t> rom, std::vector<std::uint8_t> cart )
    : mMemory( memory ), mRom( std::move( rom ) ), mCart( std::move( cart ) )
{
  setAutomaton( true );
  mTom.setJerry( &mJerryInts );
  mGathered = std::make_unique<Gathered>();
  redecode();
  if ( boot == Boot::Reset )
  {
    // MEMCON1 as the board's configuration pins load it during reset (ROMHI, an 8 bit ROM), the
    // rest of both clear, as the simulation starts them; nothing written yet (mset). The 68000 is
    // emulated: it starts no bus cycle before it says so.
    mCon = MemCon{};
    mCon.mset = false;
    mCon.dspd = 0;
    mCon.iospd = 0;
    mCon.dwid0 = 0;
    mCon.dwid1 = 0;
    mCon.refrate = 0;
    redecode();
    mEmulated68k = true;
    mHorizon[M68K] = 0;
    mOwn.m68k.step = 3;
    mPins.holdBack = 0;
    mOwn.m68k.phase = 1;
    return;
  }
  mOwn.width.pw = 2;
  mOwn.width.maskai = 6;
  mOwn.width.maskw = 2;
  mOwn.mem.mws = 1;
  mOwn.m68k.as = false;
  mOwn.m68k.read = false;
  if ( boot == Boot::Gpu )
  {
    // The state of the GPU's first cycle: the 68000's write of G_CTRL, an internal register, is
    // done and DTACK given; the 68000 lets go of AS in cycle 4, prefetches from cycle 7, at the
    // other phase of its clock than with the DSP.
    mOwn.cpu.q13 = true;
    mRows.aout = 0xf02110 >> 3;
    mRefCount.ps = 0x1a;
    mRefCount.rp = 3;
    mOwn.ref.rc = 3;
    mOwn.m68k.phase = 1;
    mOwn.m68k.asOff = true; // AS rises in cycle 4
    mOwn.m68k.asLeft = 4;
    mPins.address = 0xf02116;
    mPins.readStart = 7;
    mPins.holdBack = 6;
    return;
  }
  // The state of the DSP's first cycle (sim/sim_main.cpp's boot program): Tom ends the 68000's
  // write of D_CTRL (an I/O cycle, 16 bits wide), the 68000 waits for DTACK, the refresh counters
  // where the fixed boot leaves them.
  mOwn.mem.ack = false;
  mOwn.mem.idle = false;
  mOwn.mem.q8b = true;
  mOwn.cpu.q12 = true;
  mRows.aout = 0xf1a110 >> 3;
  mOwn.qs = 1u << 3; // Jerry's slave has passed the write on to the DSP (qs3)
  mRefCount.ps = 0xd;
  mRefCount.rp = 5;
  mOwn.ref.rc = 3;
  mPins.address = 0xf1a116;
}

std::uint32_t MainBus::dataBusLow() const
{
  Control::Memory const& m = mCtl->mem;
  std::uint64_t const d =
      m.busRead && m.busJustify ? downShift( mDataBus, mAddr.bus & 7u, m.busWidth, true ) : mDataBus;
  return static_cast<std::uint32_t>( d );
}

bool MainBus::idle( Master m ) const
{
  Control::Gateway const& g = mCtl->gw[index( m )];
  bool const gateway = !g.active && !g.dackwt && !g.xldReady && !g.cycpend && !mGw[index( m )].store && !g.packwt &&
                       !g.greqt2 && !g.gatereq;
  if ( m == Master::Gpu )
    return gateway;
  return gateway && mCtl->jerry.q0 && !mCtl->arb.dbg && ( mCtl->jerry.dbrls & 1u ) && !mCtl->arb.q1;
}

bool MainBus::m68kAtMemory() const
{
  std::uint32_t const ab = ( mM68k.access.address | ( mM68k.access.uds ? 0u : 1u ) ) & 0xffffffu;
  Decoded const d = lookup( mDecode, ab );
  return mM68k.access.fc != 7 && !d.intdev && !d.extdev;
}

bool MainBus::canStep( Master m, std::uint64_t cycle ) const
{
  std::uint64_t const other = mHorizon[index( m ) == GPU ? DSP : GPU];
  std::uint64_t const m68k = mM68k.pending ? UINT64_MAX : mHorizon[M68K];
  return !mHold && other > cycle && m68k > cycle && cycle < mEnd;
}

bool MainBus::catchUp( Master m, std::uint64_t cycle )
{
  std::uint64_t& horizon = mHorizon[index( m )];
  if ( horizon == UINT64_MAX || horizon < cycle )
    horizon = cycle;
  advance();
  return mCycle >= cycle;
}

void MainBus::clock( Master m, std::uint64_t cycle, GatewayRequest const& request )
{
  std::size_t const i = index( m );
  if ( request.valid )
  {
    mPending[i] = request;
    mPendingCycle[i] = cycle;
    if ( m == Master::Gpu && ( request.address & 0xff0000u ) == 0xf10000u )
      mGpuAtJerry = true;
  }
  if ( mHorizon[i] == UINT64_MAX || mHorizon[i] <= cycle )
    mHorizon[i] = cycle + 1;
  advance();
}

// Steps every cycle both processors have handed over; at rest, up to the next request handed.
void MainBus::advance()
{
  if ( mHold )
    return;
  for ( ;; )
  {
    // while the emulated 68000 waits for DTACK, its pins are the bus's to step
    std::uint64_t const m68kLimit = mM68k.pending ? UINT64_MAX : mHorizon[M68K];
    std::uint64_t limit = std::min( { mHorizon[DSP], mHorizon[GPU], m68kLimit } );
    if ( mCycle >= mEnd )
      return;
    if ( limit == UINT64_MAX && mM68k.pending ) // nothing else holds it back: a cycle, then back to it
    {
      step();
      return;
    }
    limit = std::min( limit, mEnd );
    if ( mCycle >= limit )
      return;
    if ( mRestSteps < 2 )
    {
      // not at rest: cycle by cycle, while what bounds them holds (DTACK moves the 68000's; a
      // cycle that reaches a RISC, a write that starts it, brings its horizon back)
      bool const pending = mM68k.pending;
      do
        step();
      while ( mCycle < std::min( { limit, mHorizon[DSP], mHorizon[GPU] } ) && mRestSteps < 2 &&
              mM68k.pending == pending );
      continue;
    }
    std::uint64_t until = std::min( { limit, mPendingCycle[DSP], mPendingCycle[GPU] } );
    // what a RISC handed for a cycle a rest would pass over: a write of the blitter's registers,
    // an interrupt to the 68000
    if ( !mBlitWrites.empty() )
      until = std::min( until, mBlitWrites.front().cycle );
    for ( std::uint64_t const irq : { mGpuCpuIrq, mDspCpuIrq } )
      if ( irq >= mCycle )
        until = std::min( until, irq );
    // a rest ends where the refresh count fills, and the object processor waits for a line through
    // it, which ends it where one starts: the video output is brought forward no further
    if ( mRestSteps >= 2 && until > mCycle )
      until = mCycle + restReach( until - mCycle );
    if ( mRestSteps >= 2 && until > mCycle && ( mTom.videoOn() || !mTom.op().idle() ) )
      until = mTom.renderToStart( mCycle, until );
    if ( mRestSteps >= 2 && until > mCycle )
      rest( until - mCycle );
    else
      step();
  }
}

// `n` cycles of the bus at rest, or fewer: up to the one in which the refresh count fills, which
// step() takes. At rest only the refresh counters and the 68000's clock change.
// The cycles of `n` a rest goes through: up to the one in which the refresh count fills.
std::uint64_t MainBus::restReach( std::uint64_t n ) const
{
  std::uint32_t ps = mRefCount.ps;
  std::uint32_t rp = mRefCount.rp;
  std::uint32_t rc = mCtl->ref.rc;
  std::uint64_t done = 0;
  while ( done < n )
  {
    std::uint64_t const toPen = ( 63u - ps ) & 63u;
    if ( n - done <= toPen )
      return n;
    done += toPen;
    bool const lrpc = rp == 0;
    bool const rpen = mCon.refrate != 0;
    if ( lrpc && rpen && rc == 7 )
      return done;
    if ( lrpc )
    {
      rp = mCon.refrate;
      if ( rpen )
        ++rc;
    }
    else
      rp = ( rp - 1 ) & 0xf;
    ps = 0;
    ++done;
  }
  return done;
}

void MainBus::rest( std::uint64_t n )
{
  RefreshCount& r = mRefCount;
  Control& own = ownState();
  std::uint8_t& rc = own.ref.rc;
  std::uint64_t done = 0;
  while ( done < n )
  {
    std::uint64_t const toPen = ( 63u - r.ps ) & 63u; // cycles before the one with ps 63 (pen)
    if ( n - done <= toPen )
    {
      r.ps = static_cast<std::uint32_t>( ( r.ps + ( n - done ) ) & 63u );
      done = n;
      break;
    }
    r.ps = 63;
    done += toPen;
    bool const lrpc = r.rp == 0;
    bool const rpen = mCon.refrate != 0;
    if ( lrpc && rpen && rc == 7 )
    {
      mRestSteps = 0;
      break;
    }
    if ( lrpc )
    {
      r.rp = mCon.refrate;
      if ( rpen )
        ++rc;
    }
    else
      r.rp = ( r.rp - 1 ) & 0xf;
    r.ps = 0;
    ++done;
  }
  mCycle += done;
  own.m68k.phase ^= static_cast<std::uint8_t>( done & 1 );
}

MainBus::Control& MainBus::ownState()
{
  if ( mCtl != &mOwn )
    mOwn = *mCtl;
  mCtl = &mOwn;
  mNode = nullptr;
  return mOwn;
}

// Jerry's bus is 16 bits wide: a long word is two word cycles, the second at the address with bit 1
// set (seta1), so that one at an address two past a long word boundary moves the same word twice.
// A word at an odd address is not modelled (its high byte is what the bus held before).
bool MainBus::boardAt( std::uint32_t address ) const
{
  if ( mRom.empty() )
    return false;
  return lookup( mDecode, address & 0xffffffu ).from;
}

std::uint8_t MainBus::boardByte( std::uint32_t start, std::uint32_t address ) const
{
  std::uint32_t const width = 1u << mCon.romwid;
  std::uint32_t const lane = width - 1 - ( address & ( width - 1 ) ); // 0: D[7:0]
  if ( !lookup( mDecode, start & 0xffffffu ).cart )
    return lane == 0 ? mRom[start & ( mRom.size() - 1 )] : 0xffu;
  if ( mCart.empty() || lane > 3 )
    return 0xffu;
  return mCart[( ( start & 0x7ffffcu ) + 3 - lane ) & ( mCart.size() - 1 )];
}

std::uint32_t MainBus::boardBytes( std::uint32_t address, std::uint32_t bytes ) const
{
  std::uint32_t const width = 1u << mCon.romwid;
  std::uint32_t v = 0;
  for ( std::uint32_t b = 0; b < bytes; ++b )
  {
    std::uint32_t const x = address + b;
    v = v << 8 | boardByte( std::max( address, x & ~( width - 1 ) ), x );
  }
  return v;
}

std::uint32_t MainBus::read( std::uint32_t address, std::uint32_t size )
{
  if ( boardAt( address ) ) // the OS ROM or the cartridge, as a RISC reads it
    return size == 0   ? boardBytes( address, 1 )
           : size == 1 ? boardBytes( address & ~1u, 2 )
                       : boardBytes( address & ~3u, 4 );
  switch ( size )
  {
  case 0:
    return mMemory.readByte( address );
  case 1:
    return mMemory.readWord( address & ~1u );
  default:
    return static_cast<std::uint32_t>( mMemory.readWord( address & ~1u ) ) << 16 |
           mMemory.readWord( ( address | 2u ) & ~1u );
  }
}

void MainBus::write( std::uint32_t address, std::uint32_t size, std::uint32_t value )
{
  switch ( size )
  {
  case 0:
    mMemory.writeByte( address, static_cast<std::uint8_t>( value ) );
    break;
  case 1:
    mMemory.writeWord( address & ~1u, static_cast<std::uint16_t>( value ) );
    break;
  default:
    mMemory.writeWord( address & ~1u, static_cast<std::uint16_t>( value >> 16 ) );
    mMemory.writeWord( ( address | 2u ) & ~1u, static_cast<std::uint16_t>( value ) );
    break;
  }
}

void MainBus::m68kHorizon( std::uint64_t cycle )
{
  mHorizon[M68K] = cycle;
  advance();
}

void MainBus::m68kAccess( M68kAccess const& access )
{
  mM68k.access = access;
  mM68k.pending = true;
  mM68k.written = false;
  mM68k.served = false;
  mM68k.readData = 0;
  mHorizon[M68K] = access.start;
  mRestSteps = 0; // the bus is stepped from S0 on: w395, then the cycle
  if ( mAuto && trySpan() )
    return;
  advance();
}

void MainBus::m68kContinue()
{
  advance();
}

// The object processor's interrupt to the GPU in the cycle the bus steps next: its outputs in a
// cycle come from its state and the bus's latches before it (step() works them out first), so
// they are what stepping the cycle will give.
bool MainBus::objectGpuInterruptAhead( std::uint64_t cycle )
{
  ObjectProcessor const& op = mTom.op();
  if ( cycle != mCycle || !( mTom.videoOn() || !op.idle() ) )
    return false;
  ObjectProcessor::Inputs oi = mTom.opInputs( cycle );
  oi.mack = mCtl->mem.ack;
  oi.obback = bit( mCtl->arbAck, 5 );
  return op.outputs( oi ).gpuInterrupt;
}

void MainBus::tomWrite( std::uint64_t cycle, std::uint32_t address, std::uint16_t value )
{
  mIhandlerUntil = 0;
  MemCon& c = mCon;
  switch ( address & 0xfffffeu )
  {
  case 0xf00000: // MEMCON1 (memc1w): ROMHI, ROMWIDTH, ROMSPEED, DRAMSPEED, FASTROM, IOSPEED
    c.mset = true;
    c.romhi = bit( value, 0 );
    c.romwid = ( value >> 1 ) & 3u;
    c.romspd = ( value >> 3 ) & 3u;
    c.dspd = ( value >> 5 ) & 3u;
    c.fastrom = bit( value, 7 );
    c.iospd = ( value >> 11 ) & 3u;
    redecode();
    flushAutomaton();
    break;
  case 0xf00002: // MEMCON2 (memc2w): COLS0, DWIDTH0, COLS1, DWIDTH1, REFRATE, BIGEND, HILO
    c.cols0 = value & 3u;
    c.dwid0 = ( value >> 2 ) & 3u;
    c.cols1 = ( value >> 4 ) & 3u;
    c.dwid1 = ( value >> 6 ) & 3u;
    c.refrate = ( value >> 8 ) & 0xfu;
    c.bigend = bit( value, 12 );
    c.hilo = bit( value, 13 );
    redecode();
    flushAutomaton();
    break;
  default:
    mTom.write( cycle, address, value );
    break;
  }
}

// lbuf.v aout[15] with a long word: both pixels at once (wea12, web12), the halves as the bus's
// endianness orders them (dw: down, up); a write there off the line buffers moves no data
void MainBus::lineBufferLongWrite( std::uint64_t cycle, std::uint32_t address )
{
  std::uint32_t const at = address & ~0x8000u;
  if ( !Tom::decodes( at ) || ( at & 0x1fffu ) < 0x800u || bit( address, 1 ) )
    return;
  std::uint32_t const dwrite = static_cast<std::uint32_t>( mDataBus >> ( bit( address, 2 ) ? 0 : 32 ) );
  mTom.write( cycle, at, static_cast<std::uint16_t>( mCon.bigend ? dwrite >> 16 : dwrite ) );
  mTom.write( cycle, at | 2u, static_cast<std::uint16_t>( mCon.bigend ? dwrite : dwrite >> 16 ) );
}

char const* MainBus::busHolder() const
{
  static constexpr std::array<char const*, 10> NAMES{ "68000", "blitter",          "GPU",     "68000",
                                                      "DSP",   "object processor", "blitter", "GPU",
                                                      "DSP",   "DRAM refresh" };
  for ( std::size_t i = 0; i < NAMES.size(); ++i )
    if ( bit( mCtl->arbAck, static_cast<std::uint32_t>( i ) ) )
      return NAMES[i];
  return "none";
}

std::uint16_t MainBus::tomRead( std::uint64_t cycle, std::uint32_t address, char const* cpu, std::uint32_t pc )
{
  mIhandlerUntil = 0;
  MemCon const& c = mCon;
  // dr_a0: CPU32 and NOCPU clear
  auto const memcon1 = [&c]
  {
    return static_cast<std::uint16_t>( c.iospd << 11 | ( c.fastrom ? 0x80u : 0u ) | c.dspd << 5 | c.romspd << 3 |
                                       c.romwid << 1 | ( c.romhi ? 1u : 0u ) );
  };
  if ( !Tom::readDecodes( address ) )
  {
    // no register drives Tom's read bus: it holds MEMCON1, which drives it between reads
    std::uint16_t const v = memcon1();
    if ( mUndrivenRead && cpu != nullptr )
      mUndrivenRead( cpu, pc, address & 0xfffffeu, v, cycle );
    return v;
  }
  switch ( address & 0xfffffeu )
  {
  case 0xf00000:
    return memcon1();
  case 0xf00002: // dr_a1
    return static_cast<std::uint16_t>( ( c.hilo ? 0x2000u : 0u ) | ( c.bigend ? 0x1000u : 0u ) | c.refrate << 8 |
                                       c.dwid1 << 6 | c.cols1 << 4 | c.dwid0 << 2 | c.cols0 );
  default:
    return mTom.read( cycle, address & ~1u );
  }
}

std::uint16_t MainBus::m68kReadWord( std::uint32_t address, bool uds, bool lds )
{
  Decoded const d = lookup( mDecode, address & 0xfffffeu );
  if ( d.ram0 )
    return mMemory.readWord( address & 0x3ffffeu );
  if ( d.from && !mRom.empty() )
  {
    // a byte's cycle is of its address alone
    std::uint32_t const a = address & 0xfffffeu;
    if ( uds && lds )
      return static_cast<std::uint16_t>( boardBytes( a, 2 ) );
    return static_cast<std::uint16_t>( uds ? boardBytes( a, 1 ) << 8 | 0xffu : 0xff00u | boardBytes( a + 1, 1 ) );
  }
  return 0xffff;
}

// --- spans ------------------------------------------------------------------------------------------

bool MainBus::quiet( Control const& k )
{
  auto const idle = []( Control::Gateway const& gw )
  { return !gw.active && !gw.dackwt && !gw.xldReady && !gw.cycpend && !gw.packwt && !gw.greqt2 && !gw.gatereq; };
  Control::Memory const& m = k.mem;
  bool const under = m.q1a || m.q1b || m.q1c || m.q1d || m.q2a || m.q2b || m.q2c || m.q3a || m.q3b || m.q4a || m.q4b ||
                     m.q4c || m.q4d || m.q4e || m.q4f || m.q4g || m.q4h || m.q5a || m.q5b || m.q5c;
  return idle( k.gw[DSP] ) && idle( k.gw[GPU] ) && k.qs == 1u && k.jerry.q0 && !k.jerry.storePending &&
         k.jerry.storeHalves == 0 && k.jerry.dbrls == 3u && !k.arb.dbg && ( k.arbAck & 0xe6u ) == 0 && !m.q7a &&
         !m.q7b && !m.q8a && !m.q8b && !m.q8c && !m.ourack && !m.busJustify &&
         !( ( m.gpuCycle || m.opCycle || m.blitCycle ) && under );
}

// Nothing but the 68000 and the refresh at work on the bus in `cycle`: no request handed for it, no
// interrupt to pass on, no data of another master's under way, the object processor waiting for a
// line with its inputs settled, the blitter settled.
bool MainBus::quietCycle( std::uint64_t cycle ) const
{
  if ( !mNode->quiet )
    return false;
  if ( mPendingCycle[DSP] == cycle || mPendingCycle[GPU] == cycle || mGpuCpuIrq == cycle || mDspCpuIrq == cycle ||
       mSlave.rdCycle == cycle )
    return false;
  if ( !mBlitWrites.empty() && mBlitWrites.front().cycle <= cycle )
    return false;
  if ( !mBlitter.settled() || mGpuDrive || mOpDrive || mGw[GPU].store || mGw[DSP].store || mGw[GPU].loadHalves != 0 ||
       mSlave.ioserv || mGpuPort.ioserv || mGpuAtJerry )
    return false;
  ObjectProcessor const& op = const_cast<Tom&>( mTom ).op();
  bool const opRuns = mTom.videoOn() || !op.idle();
  if ( opRuns != mRec.opRuns )
    return false;
  return !opRuns || ( op.idleSettled() && cycle < mTom.opWaitUntil() && mTom.obfwCycle() != cycle );
}

// misc.v's prescaler and period counter `n` cycles on, as the cycles would count them
void MainBus::countRefresh( std::uint64_t n )
{
  RefreshCount& r = mRefCount;
  while ( n > 0 )
  {
    std::uint64_t const toPen = ( 63u - r.ps ) & 63u;
    if ( n <= toPen )
    {
      r.ps = static_cast<std::uint32_t>( r.ps + n );
      return;
    }
    n -= toPen + 1; // through the cycle with ps 63 (pen)
    r.rp = r.rp == 0 ? mCon.refrate : ( r.rp - 1 ) & 0xf;
    r.ps = 0;
  }
}

void MainBus::spanRecord( std::uint64_t cycle, std::uint32_t kind, std::uint32_t arg )
{
  if ( kind == S_PINS )
  {
    if ( mRec.pins )
      return;
    mRec.pins = true;
  }
  mRec.actions.push_back( kind | static_cast<std::uint32_t>( cycle - mRec.start ) << 8 | arg << 16 );
}

void MainBus::finishSpan()
{
  mRec.on = false;
  Span span;
  span.length = static_cast<std::uint32_t>( mCycle - mRec.start );
  span.toPen = mRec.toPen;
  span.rpZero = mRec.rp == 0;
  span.ps = mRec.ps;
  span.rp = mRec.rp;
  span.restSteps = mRestSteps;
  span.end = mNode;
  span.actions = mRec.actions;
  // the object processor clocked as the span's last cycle clocks it, the line buffers as they are then
  if ( mRec.opRuns && ( span.actions.empty() || ( span.actions.back() & 0xffu ) != S_OP ||
                        ( ( span.actions.back() >> 8 ) & 0xffu ) != span.length - 1 ) )
    span.actions.push_back( S_OP_CLOCK | ( span.length - 1 ) << 8 );
  SpanGroup* group = nullptr;
  for ( SpanGroup& g : mRec.node->spanGroups )
    if ( g.key == mRec.key )
      group = &g;
  if ( !group )
  {
    group = &mRec.node->spanGroups.emplace_back();
    group->key = mRec.key;
  }
  group->spans.push_back( std::move( span ) );
}

void MainBus::spanEnds()
{
  if ( mRec.on )
    finishSpan();
#ifdef JAG_BUS_CHECK
  if ( mSpanCheck )
  {
    SpanCheck const& e = *mSpanCheck;
    M68kCycle const& mc = mM68k;
    bool const same = e.cycle == mCycle && e.node == mNode && e.dataBus == mDataBus && e.addr.bus == mAddr.bus &&
                      e.addr.op == mAddr.op && e.rows.aout == mRows.aout && e.rows.bank0 == mRows.bank0 &&
                      e.rows.bank1 == mRows.bank1 && e.refresh.ps == mRefCount.ps && e.refresh.rp == mRefCount.rp &&
                      e.pending == mc.pending && e.written == mc.written && e.served == mc.served &&
                      e.dtack == mc.dtack && e.start == mc.access.start && e.readData == mc.readData &&
                      e.pins == mPins.address && e.horizon == mHorizon[M68K] && e.restSteps == mRestSteps;
    if ( !same )
    {
      std::fprintf( stderr,
                    "bus span: cycle %llu: the replay differs (cycle %llu/%llu node %d dtack %llu/%llu readData "
                    "%04x/%04x refresh %u,%u/%u,%u aout %x/%x restSteps %u/%u)\n",
                    static_cast<unsigned long long>( mCycle ),
                    static_cast<unsigned long long>( e.cycle ),
                    static_cast<unsigned long long>( mCycle ),
                    e.node == mNode ? 1 : 0,
                    static_cast<unsigned long long>( e.dtack ),
                    static_cast<unsigned long long>( mc.dtack ),
                    e.readData,
                    mc.readData,
                    e.refresh.ps,
                    e.refresh.rp,
                    mRefCount.ps,
                    mRefCount.rp,
                    e.rows.aout,
                    mRows.aout,
                    e.restSteps,
                    mRestSteps );
      std::abort();
    }
    mSpanCheck.reset();
  }
#endif
}

void MainBus::replaySpan( Span const& span, bool dry )
{
  std::uint64_t const start = mCycle;
  M68kCycle& mc = mM68k;
  M68kAccess& acc = mc.access;
  auto const source = [this]( std::uint32_t from ) -> std::uint32_t
  {
    return from == FROM_68000 ? mPins.address : from == FROM_AOUT ? mRows.aout << 3 : 0xffffffu;
  };
  for ( std::uint32_t const action : span.actions )
  {
    std::uint64_t const cycle = start + ( ( action >> 8 ) & 0xffu );
    std::uint32_t const arg = action >> 16;
    switch ( action & 0xffu )
    {
    case S_DELAY:
      acc.start += 2;
      break;
    case S_PINS:
      mPins.address = acc.address | ( acc.uds ? 0u : 1u );
      break;
    case S_DTACK:
      if ( !mc.served && acc.read )
        mc.readData = mJerry.openLow;
      mc.pending = false;
      mc.dtack = cycle;
      mHorizon[M68K] = cycle + 2;
      break;
    case S_LATCH_DRAM:
    {
      std::uint64_t const lanes = byteLanes( mAddr.bus & 7u, arg );
      std::uint64_t const phrase = mMemory.readPhrase( mAddr.bus & ~7u );
      mDataBus = ( mDataBus & ~lanes ) | ( phrase & lanes );
      break;
    }
    case S_LATCH_ROM:
    {
      std::uint32_t const maskai = arg & 15u;
      std::uint32_t const maskw = arg >> 4;
      std::uint32_t const base = mAddr.bus & ~7u;
      for ( std::uint32_t b = maskai; b < 8 && b < maskai + maskw; ++b )
      {
        std::uint32_t const shift = 56 - 8 * b;
        mDataBus = ( mDataBus & ~( std::uint64_t{ 0xff } << shift ) ) |
                   static_cast<std::uint64_t>( boardByte( base + maskai, base + b ) ) << shift;
      }
      break;
    }
    case S_READ:
      mc.readData = m68kReadWord( acc.address, acc.uds, acc.lds );
      break;
    case S_WRITE:
      mc.written = true;
      mDataBus = acc.data * 0x0001000100010001u;
      if ( !dry && lookup( mDecode, acc.address ).ram0 )
      {
        if ( acc.uds )
          mMemory.writeByte( acc.address & 0x3fffffu, static_cast<std::uint8_t>( acc.data >> 8 ) );
        if ( acc.lds )
          mMemory.writeByte( ( acc.address & 0x3fffffu ) + 1, static_cast<std::uint8_t>( acc.data ) );
      }
      break;
    case S_OP:
    case S_OP_CLOCK:
      if ( !dry )
      {
        // the object processor waits: its inputs those it last took, the data bus its pixel register follows
        std::uint32_t const dout = static_cast<std::uint32_t>( mDataBus );
        ObjectProcessor::Inputs oi = mTom.opInputsWaiting( cycle );
        oi.dout = static_cast<std::uint16_t>( dout );
        mTom.op().clock( oi, mDataBus );
        if ( ( action & 0xffu ) == S_OP )
        {
          bool const down = bit( mRows.aout << 3, 15 );
          mTom.setDataBus( cycle, ( dout & 0xffffu ) << 16 | ( down ? dout >> 16 : dout & 0xffffu ) );
        }
      }
      break;
    case S_ROWS:
    {
      std::uint32_t const held = mRows.aout << 3;
      if ( arg & 1u )
        mRows.bank0 = ( held >> ( 8 + mCon.dwid0 + mCon.cols0 ) ) & 0x7ff;
      if ( arg & 2u )
        mRows.bank1 = ( held >> ( 8 + mCon.dwid1 + mCon.cols1 ) ) & 0x7ff;
      break;
    }
    case S_AOUT:
      mRows.aout = ( source( arg ) & 0xffffffu ) >> 3;
      break;
    case S_START:
    {
      std::uint32_t const ain = source( arg & 3u );
      mAddr.op = ain & 0xfffff8u;
      mAddr.bus = ain & 0xffffffu;
      mOpDrive = false;
      if ( arg & 4u )
        mc.served = true;
      break;
    }
    default:
      break;
    }
  }
  countRefresh( span.length );
  mCycle = start + span.length;
  mCtl = &span.end->state;
  mNode = span.end;
  mRestSteps = span.restSteps;
}

// A 68000 bus cycle just asked for: replayed in one step if a span of it is known from the state the
// bus is in, with the same inputs (the access, where it decodes to and the rows it meets, Tom's
// held address, the refresh counters, Tom's interrupt handler, the object processor at work or
// not) and nothing but it at work on the bus through its length; else recorded as the cycles are
// stepped, while they are so.
bool MainBus::trySpan()
{
  mRec.on = false;
  M68kAccess const& acc = mM68k.access;
  std::uint64_t const c = mCycle;
  if ( !mEmulated68k || acc.fc == 7 || acc.held || acc.holdAs || acc.start < c || acc.start - c > 63 || mFlushWanted ||
       c >= mEnd || mSpanCheck )
    return false;
  if ( !mNode )
    mNode = &nodeFor( *mCtl );
  Node& node = *mNode;
  if ( !node.quiet )
    return false;
  std::uint32_t const ab = ( acc.address | ( acc.uds ? 0u : 1u ) ) & 0xffffffu;
  Decoded const d = lookup( mDecode, ab );
  if ( !( d.ram0 || d.from ) || d.intdev || d.extdev )
    return false;
  std::uint32_t const aa = ( mRows.aout << 3 ) & 0xffffffu;
  Decoded const da = lookup( mDecode, aa );
  auto const row0 = [this]( std::uint32_t a ) { return ( a >> ( 8 + mCon.dwid0 + mCon.cols0 ) ) & 0x7ffu; };
  auto const row1 = [this]( std::uint32_t a ) { return ( a >> ( 8 + mCon.dwid1 + mCon.cols1 ) ) & 0x7ffu; };
  ObjectProcessor const& op = mTom.op();
  bool const opRuns = mTom.videoOn() || !op.idle();
  bool ihandler = false;
  std::uint64_t const steady = mTom.ihandlerSteady( c, ihandler );
  auto const b = []( bool v ) { return static_cast<std::uint64_t>( v ); };
  // with AS still low after the cycle before, its address on the bus until it rises
  std::uint64_t held = 0;
  if ( !node.state.m68k.as )
  {
    std::uint32_t const ob = mPins.address & 0xffffffu;
    Decoded const dp = lookup( mDecode, ob );
    held = 1u | b( dp.ram0 ) << 1 | b( dp.ram1 ) << 2 | b( dp.from ) << 3 | b( dp.intdev ) << 4 | b( dp.extdev ) << 5 |
           std::uint64_t{ dp.mw & 3u } << 6 | std::uint64_t{ ob & 7u } << 8 | b( mRows.bank0 == row0( ob ) ) << 11 |
           b( mRows.bank1 == row1( ob ) ) << 12 | b( row0( ob ) == row0( ab ) ) << 13 |
           b( row1( ob ) == row1( ab ) ) << 14 | b( row0( ob ) == row0( aa ) ) << 15 |
           b( row1( ob ) == row1( aa ) ) << 16;
  }
  std::uint64_t const key =
      b( acc.read ) | b( acc.uds ) << 1 | b( acc.lds ) << 2 | ( acc.start - c ) << 3 | held << 9 | b( ihandler ) << 26 |
      b( opRuns ) << 27 | b( d.ram0 ) << 28 | b( d.from ) << 29 | std::uint64_t{ d.mw & 3u } << 30 |
      std::uint64_t{ ab & 7u } << 32 | b( mRows.bank0 == row0( ab ) ) << 35 | b( mRows.bank1 == row1( ab ) ) << 36 |
      b( da.ram0 ) << 37 | b( da.ram1 ) << 38 | b( da.from ) << 39 | b( da.intdev ) << 40 | b( da.extdev ) << 41 |
      std::uint64_t{ da.mw & 3u } << 42 | b( mRows.bank0 == row0( aa ) ) << 44 | b( mRows.bank1 == row1( aa ) ) << 45 |
      b( row0( aa ) == row0( ab ) ) << 46 | b( row1( aa ) == row1( ab ) ) << 47 | b( mGw[DSP].dmaen ) << 48 |
      b( mGw[GPU].dmaen ) << 49;
  std::uint32_t const pen = ( 63u - mRefCount.ps ) & 63u;
  SpanGroup* group = nullptr;
  if ( node.lastGroup < node.spanGroups.size() && node.spanGroups[node.lastGroup].key == key )
    group = &node.spanGroups[node.lastGroup];
  else
    for ( std::size_t i = 0; i < node.spanGroups.size(); ++i )
      if ( node.spanGroups[i].key == key )
      {
        node.lastGroup = i;
        group = &node.spanGroups[i];
        break;
      }
  // the group's spans as they stand: a conditional with a temporary would copy them every attempt
  for ( Span const& span : group ? std::span<Span const>( group->spans ) : std::span<Span const>() )
  {
    if ( !span.fits( pen, mRefCount.ps, mRefCount.rp ) )
      continue;
    std::uint64_t const end = c + span.length;
    // the RISCs run up to its end first, the bus held: what they hand it meanwhile, it then sees
    if ( ( mHorizon[DSP] < end || mHorizon[GPU] < end ) && mOthers && end <= steady && end <= mEnd )
    {
      mHold = true;
      mOthers( end );
      mHold = false;
      // what they wrote meanwhile (a timer, an interrupt) may bring the handler's change nearer
      bool now = false;
      if ( mTom.ihandlerSteady( c, now ) < end || now != ihandler )
        return false;
    }
    // nothing else at work through it, and the bus free to go so far
    if ( mGw[DSP].dmaen != ( ( key >> 48 ) & 1u ) || mGw[GPU].dmaen != ( ( key >> 49 ) & 1u ) )
      return false;
    if ( end > steady || end > mEnd || mHorizon[DSP] < end || mHorizon[GPU] < end || mPendingCycle[DSP] < end ||
         mPendingCycle[GPU] < end )
      return false;
    for ( std::uint64_t const at : { mGpuCpuIrq, mDspCpuIrq, mSlave.rdCycle } )
      if ( at >= c && at < end )
        return false;
    if ( ( !mBlitWrites.empty() && mBlitWrites.front().cycle < end ) || !mBlitter.settled() || mGpuDrive || mOpDrive ||
         mGw[GPU].store || mGw[DSP].store || mGw[GPU].loadHalves != 0 || mSlave.ioserv || mGpuPort.ioserv ||
         mGpuAtJerry )
      return false;
    if ( opRuns &&
         ( !op.idleSettled() || end > mTom.opWaitUntil() || ( mTom.obfwCycle() >= c && mTom.obfwCycle() < end ) ) )
      return false;
#ifdef JAG_BUS_CHECK
    {
      // the replay's result, without what reaches outside the bus; then the cycles stepped, and the
      // two compared as the span ends
      std::uint64_t const dataBus = mDataBus;
      BusAddress const addr = mAddr;
      Rows const rows = mRows;
      RefreshCount const refresh = mRefCount;
      M68kCycle const m68k = mM68k;
      std::uint32_t const pins = mPins.address;
      std::uint64_t const horizon = mHorizon[M68K];
      std::uint32_t const restSteps = mRestSteps;
      Control const* ctl = mCtl;
      Node* nodeAt = mNode;
      bool const opDrive = mOpDrive;
      replaySpan( span, true );
      mSpanCheck = std::make_unique<SpanCheck>( SpanCheck{ mCycle,
                                                           mNode,
                                                           mDataBus,
                                                           mAddr,
                                                           mRows,
                                                           mRefCount,
                                                           mM68k.pending,
                                                           mM68k.written,
                                                           mM68k.served,
                                                           mM68k.dtack,
                                                           mM68k.access.start,
                                                           mM68k.readData,
                                                           mPins.address,
                                                           mHorizon[M68K],
                                                           mRestSteps } );
      mCycle = c;
      mDataBus = dataBus;
      mAddr = addr;
      mRows = rows;
      mRefCount = refresh;
      mM68k = m68k;
      mPins.address = pins;
      mHorizon[M68K] = horizon;
      mRestSteps = restSteps;
      mCtl = ctl;
      mNode = nodeAt;
      mOpDrive = opDrive;
      return false;
    }
#endif
    replaySpan( span, false );
    return true;
  }
  // a bound on the spans a state keeps
  if ( node.spanGroups.size() >= 256 || ( group && group->spans.size() >= 64 ) )
    return false;
  mRec.on = true;
  mRec.start = c;
  mRec.steadyUntil = steady;
  mRec.node = &node;
  mRec.key = key;
  mRec.toPen = pen;
  mRec.ps = mRefCount.ps;
  mRec.rp = mRefCount.rp;
  mRec.opRuns = opRuns;
  mRec.dwKnown = false;
  mRec.pins = false;
  mRec.actions.clear();
  return false;
}

void MainBus::step()
{
  std::uint64_t const cycle = mCycle++;
  if ( mGpuCpuIrq == cycle )
  {
    mTom.gpuInterrupt( cycle );
    mIhandlerUntil = 0;
  }
  if ( mDspCpuIrq == cycle )
  {
    mJerryInts.dspInterrupt( cycle );
    mIhandlerUntil = 0;
  }
  if ( mSlave.rdCycle == cycle && mDspSlave ) // the DSP's memory controller serves a read (ioserv)
    mSlave.rddata = mDspSlave->slaveRead( cycle, mSlave.rdAddress );

  if ( mFlushWanted )
  {
    mFlushWanted = false;
    mRec.on = false;
    mSpanCheck.reset();
    ownState();
    mAuto = std::make_unique<Automaton>();
  }
  // a span recorded goes on while its cycles are quiet, and ends with the step that takes DTACK
  if ( mRec.on && ( !mNode || !quietCycle( cycle ) || cycle >= mRec.steadyUntil || cycle - mRec.start > 250 ) )
    mRec.on = false;
  Gathered& g = *mGathered;
  gather( cycle, g );
  if ( mAuto )
  {
    std::uint64_t const key = keyOf( g );
    Node* node = mNode ? mNode : &nodeFor( *mCtl );
    if ( Node::Transition const* t = node->find( key ) )
    {
#ifdef JAG_BUS_CHECK
      {
        Signals cs;
        Control cn;
        control( *mCtl, g, cs, cn );
        if ( std::memcmp( &cn, &t->next->state, sizeof cn ) != 0 || std::memcmp( &cs, &t->signals, sizeof cs ) != 0 )
        {
          std::fprintf( stderr,
                        "bus automaton: cycle %llu, key %016llx: the transition differs\n",
                        static_cast<unsigned long long>( cycle ),
                        static_cast<unsigned long long>( key ) );
          auto const dump = []( char const* what, void const* a, void const* b, std::size_t n )
          {
            auto const* x = static_cast<unsigned char const*>( a );
            auto const* y = static_cast<unsigned char const*>( b );
            for ( std::size_t i = 0; i < n; ++i )
              if ( x[i] != y[i] )
                std::fprintf( stderr, "  %s byte %zu: worked out %u, recorded %u\n", what, i, x[i], y[i] );
          };
          dump( "state", &cn, &t->next->state, sizeof cn );
          dump( "signals", &cs, &t->signals, sizeof cs );
          std::abort();
        }
      }
#endif
      Node const& next = *t->next;
      data( cycle, g, t->signals, next.state, next.restful );
      mCtl = &next.state;
      mNode = t->next;
      if ( !mM68k.pending )
        spanEnds();
      return;
    }
    // a transition not recorded yet: the control worked out, and recorded
    Signals s;
    Control n;
    control( *mCtl, g, s, n );
    Node& next = nodeFor( n );
    node->transitions.push_back( { key, &next, s } );
    node->last = node->transitions.back();
    data( cycle, g, s, next.state, next.restful );
    mCtl = &next.state;
    mNode = &next;
    if ( !mM68k.pending )
      spanEnds();
    return;
  }
  Signals s;
  Control n;
  control( *mCtl, g, s, n );
  data( cycle, g, s, n, restful( n ) );
  mOwn = n;
  mCtl = &mOwn;
}

void MainBus::gather( std::uint64_t cycle, Gathered& g )
{
  Control const& c = *mCtl;
  M68kCycle& mc = mM68k;
  // g is the cycle before's: what is set below only in some cycles is reset to what it is without
  for ( std::size_t i : { DSP, GPU } )
  {
    g.request[i] = mPendingCycle[i] == cycle ? &mPending[i] : &NO_REQUEST;
    g.dmaen[i] = mGw[i].dmaen;
  }
  g.uds68 = true;
  g.lds68 = true;
  g.dreqin = false;
  g.rwin = false;
  g.sizin = 0;
  g.ain = 0xffffff;
  g.ainSource = FROM_NONE;
  g.bi.memw = false;
  g.bi.address = 0;
  g.bi.din = 0;
  g.fc7 = mc.access.fc == 7;
  if ( mEmulated68k )
  {
    M68kAccess const& acc = mc.access;
    std::int64_t const r = static_cast<std::int64_t>( cycle ) - static_cast<std::int64_t>( acc.start );
    g.raw.pending = mc.pending;
    g.raw.pos = r <= -5 ? 0 : r < 2 ? static_cast<std::uint8_t>( r + 5 ) : static_cast<std::uint8_t>( 7 + ( r & 1 ) );
    g.raw.read = acc.read;
    g.raw.uds = acc.uds;
    g.raw.lds = acc.lds;
    g.raw.held = acc.held;
    g.raw.holdAs = acc.holdAs;
  }
  else
  {
    g.raw.holdBack = cycle == mPins.holdBack;
    g.raw.readStart = cycle >= mPins.readStart;
  }

  // --- the 68000: its bus arbitration, the xbgl flip-flop, its pins --------------------------------
  Control::M68k& p = g.m68k;
  p = c.m68k;
  bool const ba = c.arb.q2; // Tom has the bus; BGACK
  bool const highPhase = p.phase == 0;
  p.phase ^= 1u;
  // w395: the 68000 is about to start a bus cycle (its S0 and S1), which holds a grant back
  bool const w395 = mEmulated68k
                        ? mc.pending && !mc.access.held && cycle + 2 >= mc.access.start && cycle < mc.access.start
                        : cycle == mPins.holdBack;
  if ( highPhase )
  {
    p.br0 = c.arb.q1 ? 1u : 0u; // BR, active
    p.bga0 = ba ? 1u : 0u;
    p.br2 = p.br1;
    p.bga2 = p.bga1;
    p.st1 = p.st0;
  }
  else
  {
    p.br1 = p.br0;
    p.bga1 = p.bga0;
    p.st0 = static_cast<std::uint8_t>( arbitrationNext( p.st1, p.br2 != 0, p.bga2 != 0, w395 ) );
  }
  // w432: the bus is the 68000's (BR and BGACK not seen, BG not given); a bus cycle's S0 is two
  // cycles after a cycle it holds in, at the earliest
  bool const w432 = p.bga2 == 0 && ( p.st0 & 1u ) != 0 && p.br2 == 0;
  if ( mEmulated68k && mc.pending && !mc.access.held && cycle + 4 == mc.access.start && !w432 )
  {
    mc.access.start += 2;
    if ( mRec.on )
      spanRecord( cycle, S_DELAY );
  }
  bool const oldBg = p.bg;
  if ( highPhase )
    p.bg = ( p.st0 & 1u ) != 0;
  if ( ba )
    p.xbgl = true;
  else if ( oldBg && !p.bg )
    p.xbgl = false;

  // AS rises the third cycle after DTACK is first seen; the prefetch starts in cycle 8 if the
  // 68000 has not granted the bus
  bool const dtack = c.cpu.q13;
  if ( mEmulated68k )
  {
    // AS falls in its start, the strobes with it for a read, two cycles later for a write; DTACK is
    // taken at the clock's first half from the third cycle on, AS rises three cycles later
    M68kAccess const& acc = mc.access;
    if ( mc.pending && cycle >= acc.start )
    {
      p.as = false;
      p.read = acc.read;
      mPins.address = acc.address | ( acc.uds ? 0u : 1u ); // abus.v ai: A0 is UDS high
      if ( mRec.on )
        spanRecord( cycle, S_PINS );
      bool const strobed = acc.read || cycle >= acc.start + 2;
      g.uds68 = strobed && acc.uds;
      g.lds68 = strobed && acc.lds;
      if ( dtack && cycle >= acc.start + 2 && ( ( cycle - acc.start ) & 1u ) == 0 )
      {
        // DTACK of another master's cycle, Tom not having seen the 68000's (its AS hidden while
        // Jerry drives the board's request, after reset while xbgl says the bus is granted): the
        // 68000 reads the data bus as Jerry left it, its write is lost
        if ( !mc.served && acc.read )
          mc.readData = mJerry.openLow;
        mc.pending = false;
        mc.dtack = cycle;
        if ( mRec.on )
          spanRecord( cycle, S_DTACK );
        p.asOff = true;
        p.asLeft = 3;
        mHorizon[M68K] = cycle + 2; // the next cycle's AS falls six later at the earliest, decided two before its S0
      }
    }
    else if ( p.asOff && p.asLeft > 0 )
    {
      g.uds68 = acc.uds && ( acc.read || cycle >= acc.start + 2 );
      g.lds68 = acc.lds && ( acc.read || cycle >= acc.start + 2 );
    }
    else if ( ( ( acc.holdAs && !mc.pending ) || ( mc.pending && acc.held ) ) && !p.as ) // TAS: AS low, no strobes
    {
      g.uds68 = false;
      g.lds68 = false;
    }
    else
    {
      p.as = true;
      p.asOff = false;
      p.asLeft = 0;
    }
  }
  else
    switch ( p.step )
    {
    case 0: // the write of D_CTRL
    case 2: // the prefetch
      if ( !p.as && dtack && !p.asOff )
      {
        p.asOff = true;
        p.asLeft = 3;
      }
      if ( p.asOff && p.asLeft == 0 )
      {
        p.as = true;
        p.asOff = false;
        ++p.step;
      }
      break;
    case 1:
      if ( cycle >= mPins.readStart && p.bg )
      {
        p.as = false;
        p.read = true;
        mPins.address = M68K_ROM_READ;
        ++p.step;
      }
      break;
    default:
      break;
    }
  if ( p.asOff && p.asLeft > 0 ) // AS's rise a cycle nearer
    --p.asLeft;

  // --- what drives Tom's bus lines: Jerry when granted, else the 68000 ----------------------
  Control::Jerry const& j = c.jerry;
  bool const dbg = c.arb.dbg;
  std::uint32_t const siz =
      ( bit( j.ws, 1 ) || bit( j.ws, 2 ) ? 2u : 0u ) | ( bit( j.ws, 0 ) ? 1u : 0u ); // long is clear
  if ( dbg )
  {
    g.dreqin = j.q1 || j.q3; // dreql
    g.rwin = j.readsi;
    g.sizin = siz;
    g.ain = mJerry.ad | ( j.q3 ? 2u : 0u ); // seta1
    g.ainSource = FROM_OTHER;
  }
  else if ( !p.as )
  {
    g.dreqin = true;
    g.rwin = p.read;
    g.sizin = mEmulated68k ? ( g.uds68 ? 0u : 2u ) | ( g.lds68 ? 0u : 1u ) : 0u; // UDS, LDS (active low)
    g.ain = mPins.address;
    g.ainSource = FROM_68000;
  }
  else if ( c.arb.intbms )
  {
    g.ain = mRows.aout << 3; // Tom holds the board's bus and drives its address lines with aout (bus.v aen)
    g.ainSource = FROM_AOUT;
  }
  // the GPU's gateway drives Tom's own bus while its grant is latched (gback: ack_[2], ack_[7]),
  // the object processor while its is (obback: ack_[5])
  Gateway const& gg = mGw[GPU];
  bool const gback = bit( c.arbAck, 2 ) || bit( c.arbAck, 7 );
  ObjectProcessor& op = mTom.op();
  g.opRuns = mTom.videoOn() || !op.idle();
  bool const obback = bit( c.arbAck, 5 );
  if ( g.opRuns )
  {
    g.oi = op.idleSettled() && !obback ? mTom.opInputsWaiting( cycle ) : mTom.opInputs( cycle );
    g.oi.mack = c.mem.ack;
    g.oi.obback = obback;
    g.oo = op.outputs( g.oi );
  }
  else
  {
    g.oi = {};
    g.oo = {};
  }
  // the blitter, with the GPU's write of its registers in the cycle; it drives the bus while its
  // grant is latched (bback: ack_[1], ack_[6])
  Blitter::Inputs& bi = g.bi;
  if ( !mBlitWrites.empty() && mBlitWrites.front().cycle <= cycle )
  {
    bi.memw = true;
    bi.address = mBlitWrites.front().address;
    bi.din = mBlitWrites.front().value;
    mBlitWrites.pop_front();
  }
  // the GPU's gateway drives the data bus with what it stores the cycle after Tom takes it (wden),
  // the object processor with the header it writes back
  if ( mGpuDrive )
  {
    mGpuDrive = false;
    mDataBus = mGpuDriveData;
  }
  if ( mOpDrive )
  {
    mOpDrive = false;
    mDataBus = mTom.op().writeData();
  }
  // Tom's data bus: a DRAM read is latched into its lanes as the cycle ends (q3b, dinlatch), the
  // bytes of its width, whoever reads. The blitter takes the bus at tlw, moved down where it reads a
  // pixel (justify: bus.v, down.v); it drives its write data while data_ena, moved up (up.v), and
  // the DRAM takes the bytes of its width as CAS falls (q3a)
  Control::Memory const& m = c.mem;
  if ( m.busRead && m.q3b && ( mAddr.bus & 0xc00000u ) == 0 ) // bank 1 has no DRAM to drive it
  {
    std::uint64_t const lanes = byteLanes( mAddr.bus & 7u, m.busWidth );
    std::uint64_t const phrase = mMemory.readPhrase( mAddr.bus & ~7u );
    mDataBus = ( mDataBus & ~lanes ) | ( phrase & lanes );
    if ( mRec.on )
      spanRecord( cycle, S_LATCH_DRAM, m.busWidth );
  }
  // the blitter's read of a word of the GPU's space is latched as its I/O cycle ends (q7b): the
  // high half of the long word the port read, its low half latched for the word after
  if ( m.blitCycle && m.busRead && m.q7b && mGpuSlave )
  {
    std::uint32_t const addr = ( mAddr.bus & ~7u ) + ( c.width.maskai & 6u );
    if ( gpuSpace( addr ) )
    {
      std::uint16_t v = mGpuPort.latrdata;
      if ( !bit( addr, 1 ) )
      {
        v = static_cast<std::uint16_t>( mGpuPort.rddata >> 16 );
        mGpuPort.latrdata = static_cast<std::uint16_t>( mGpuPort.rddata );
      }
      std::uint32_t const shift = 48 - 8 * ( addr & 6u );
      mDataBus = ( mDataBus & ~( std::uint64_t{ 0xffff } << shift ) ) | static_cast<std::uint64_t>( v ) << shift;
    }
  }
  // and of the rest of Tom's own space, its registers, the CLUT and the line buffers, as the GPU's
  // read of them: the word Tom's read bus carries, into its lanes (dbus.v dr, dinlatch at q7b)
  if ( m.blitCycle && m.busRead && m.q7b )
  {
    std::uint32_t const addr = ( mAddr.bus & ~7u ) + ( c.width.maskai & 6u );
    if ( Tom::decodes( addr ) )
    {
      std::uint16_t const v = tomRead( cycle, addr, nullptr, 0 );
      std::uint32_t const shift = 48 - 8 * ( addr & 6u );
      mDataBus = ( mDataBus & ~( std::uint64_t{ 0xffff } << shift ) ) | static_cast<std::uint64_t>( v ) << shift;
    }
  }
  // a ROM cycle's bytes are latched as it ends (q5c), the lanes of the part of the access it moves
  // (memwidth.v maskai, maskw)
  if ( m.readsi && m.q5c && boardAt( mAddr.bus ) )
  {
    if ( mRec.on )
      spanRecord( cycle, S_LATCH_ROM, c.width.maskai | std::uint32_t{ c.width.maskw } << 4 );
    std::uint32_t const base = mAddr.bus & ~7u;
    std::uint32_t const start = base + c.width.maskai;
    for ( std::uint32_t b = c.width.maskai; b < 8 && b < std::uint32_t{ c.width.maskai } + c.width.maskw; ++b )
    {
      std::uint32_t const shift = 56 - 8 * b;
      mDataBus = ( mDataBus & ~( std::uint64_t{ 0xff } << shift ) ) |
                 static_cast<std::uint64_t>( boardByte( start, base + b ) ) << shift;
    }
  }
  std::uint64_t blitData = mDataBus;
  if ( m.blitCycle && m.busRead )
    blitData = downShift( mDataBus, mAddr.bus & 7u, m.busWidth, m.busJustify );
  bool const bback = blitterGranted();
  g.blitRuns = !mBlitter.settled() || bi.memw || bback;
  bi.back = bback;
  bi.ack = m.ack;
  bi.data = blitData;
  if ( g.blitRuns )
    mBlitter.latch( bi ); // tlw: what the GPU writes reaches the width and justify it gives the bus
  if ( g.blitRuns )
    g.bo = mBlitter.outputs();
  else
    g.bo = {};
  if ( gback )
  {
    g.rwin = c.gw[GPU].reada;
    g.ain = c.gw[GPU].progown ? gg.cpuAddr : gg.datAddr; // out_addr
    g.ainSource = FROM_OTHER;
  }
  else if ( obback )
  {
    g.rwin = g.oo.read;
    g.ain = g.oo.address;
    g.ainSource = FROM_OTHER;
  }
  else if ( bback )
  {
    g.rwin = g.bo.read;
    g.ain = g.bo.address;
    g.ainSource = FROM_OTHER;
  }

  // req[3]: the 68000's interrupt handler, as Tom tells how long it holds
  if ( cycle >= mIhandlerUntil )
    mIhandlerUntil = std::max( mTom.ihandlerSteady( cycle, mIhandler ), cycle + 1 );
  g.ihandler = mIhandler;
  g.pen = mRefCount.ps == 63;
  g.rpZero = mRefCount.rp == 0;

  // the internal bus: the external master's request, or the refresh's, or the master granted's; the
  // address decoded, its row compared with the open ones, where a cycle may start
  bool const xp = ( c.arbAck & 0x2e6u ) == 0; // the external master interface drives the internal bus
  bool const refreq = c.ref.rq && c.ref.rc != 0;
  g.mreqb = ( xp && c.cpu.q11 ) || ( bit( c.arbAck, 9 ) && refreq ) || ( gback && c.gw[GPU].active ) ||
            ( obback && g.oo.mreq ) || ( bback && g.bo.mreq );
  g.ab = g.ain & 0xffffff;
  g.where = g.mreqb ? lookup( mDecode, g.ab ) : Decoded{};
  g.row0 = m.ack && g.mreqb && mRows.bank0 == ( ( g.ab >> ( 8 + mCon.dwid0 + mCon.cols0 ) ) & 0x7ff );
  g.row1 = m.ack && g.mreqb && mRows.bank1 == ( ( g.ab >> ( 8 + mCon.dwid1 + mCon.cols1 ) ) & 0x7ff );
}

void MainBus::control( Control const& c, Gathered const& g, Signals& s, Control& n ) const
{
  n = c;
  n.m68k = g.m68k;

  // --- the gateways' bus requests (gpu_breq, dma_breq) and Jerry's bus master (j_jmem.v) -----
  Control::Jerry const& j = c.jerry;
  Control::Gateway const& dg = c.gw[DSP];
  Control::Gateway const& gg = c.gw[GPU];
  bool const dbg = c.arb.dbg;
  bool const split = bit( j.ws, 2 ); // dsp16: a long word in two halves
  bool const gpuBreqG = !g.dmaen[GPU] && gg.active;
  bool const dmaBreqG = gg.active && g.dmaen[GPU];
  bool const mreq = dg.active;   // the DSP's gateway is Jerry's master's
  bool const dtackJ = c.cpu.q13; // dtackl, active
  bool const notmreq = !mreq;
  bool const jd02 = j.q1 && dbg && dtackJ && !split && notmreq;
  bool const jd03 = j.q3 && dbg && dtackJ && notmreq;
  bool const jack = j.q0 || j.q4 || jd02 || jd03;
  bool const jd0 = ( j.q0 && !dbg ) || ( j.q0 && notmreq ) || jd02 || jd03;
  bool const jd1 = ( j.q0 && dbg && mreq ) || ( j.q1 && !dbg ) || ( j.q1 && !dtackJ ) || ( j.q4 && dbg );
  bool const jd2 = ( j.q1 && dbg && dtackJ && split ) || ( j.q2 && dtackJ );
  bool const jd3 = ( j.q2 && !dtackJ ) || ( j.q3 && !dbg ) || ( j.q3 && !dtackJ );
  bool const jd4 = ( j.q1 && dbg && dtackJ && !split && mreq ) || ( j.q3 && dbg && dtackJ && mreq ) || ( j.q4 && !dbg );

  // --- Tom: the arbiter's requests (arb.v) -----------------------------------------------------
  Control::Arbiter const& a = c.arb;
  Control::Memory const& m = c.mem;
  bool const notempty = c.ref.rc != 0;
  bool const refreq = c.ref.rq && notempty;
  // req[9] refresh, req[8] the DSP once it has the bus (the board drives Tom's dbrl[1] with
  // Jerry's dbrl[0] OR dbgl; Jerry's dbrl[1] goes nowhere), req[4] the DSP, req[0] the 68000
  std::uint32_t req = 1u;
  if ( refreq )
    req |= 1u << 9;
  if ( !bit( j.dbrls, 0 ) && dbg )
    req |= 1u << 8;
  if ( !bit( j.dbrls, 0 ) )
    req |= 1u << 4;
  if ( g.ihandler ) // req[3] the 68000's interrupt handler
    req |= 1u << 3;
  if ( g.oo.obbreq ) // req[5] the object processor
    req |= 1u << 5;
  if ( bit( g.bo.breq, 0 ) ) // req[1] the blitter, req[6] the blitter with BUSHI
    req |= 1u << 1;
  if ( bit( g.bo.breq, 1 ) )
    req |= 1u << 6;
  if ( gpuBreqG ) // req[2] the GPU, req[7] the GPU with DMAEN
    req |= 1u << 2;
  if ( dmaBreqG )
    req |= 1u << 7;
  std::uint32_t pr = 0;
  for ( int k = 9; k >= 0; --k )
    if ( bit( req, static_cast<std::uint32_t>( k ) ) )
    {
      pr = 1u << k;
      break;
    }
  constexpr std::uint32_t LOCAL_MASTERS = 0x3f6u; // [9:4] and [2:1]
  bool const lbr = ( pr & LOCAL_MASTERS ) != 0;
  bool const lback = ( c.arbAck & LOCAL_MASTERS ) != 0;
  bool const intbm = ( c.arbAck & 0x2e6u ) != 0; // [2:1], [7:5], [9]
  bool const refack = bit( c.arbAck, 9 );
  bool const ack = m.ack;
  bool const xp = !intbm; // the external master interface drives the internal bus

  // --- cpu.v ---------------------------------------------------------------------------------
  bool const ba = a.q2;
  bool const bmcpu = !ba;
  bool const extbms = !a.intbms;
  bool const cpuIdle = !( c.cpu.q11 || c.cpu.q12 || c.cpu.q13 );
  bool const dreqin = g.dreqin;
  bool const lds = !bit( g.sizin, 0 );
  bool const uds = !bit( g.sizin, 1 );
  bool const cd11 = ( dbg && dreqin && cpuIdle ) || ( bmcpu && dreqin && lds && extbms && cpuIdle ) ||
                    ( bmcpu && dreqin && uds && extbms && cpuIdle ) || ( c.cpu.q11 && !ack );
  bool const cd12 = ( c.cpu.q11 && ack ) || ( c.cpu.q12 && !ack );
  bool const cd13 = ( c.cpu.q12 && ack ) || ( ba && dreqin && extbms && c.cpu.q13 ) ||
                    ( bmcpu && dreqin && lds && extbms && c.cpu.q13 ) ||
                    ( bmcpu && dreqin && uds && extbms && c.cpu.q13 );
  std::uint32_t xw = 0;
  if ( bmcpu ) // bm68k
    xw = g.sizin == 0 ? 2u : 1u;
  else
    xw = ( g.sizin == 0 ? 4u : 0u ) | g.sizin;

  // the internal bus: the external master's request, or the refresh's
  bool const gback = bit( c.arbAck, 2 ) || bit( c.arbAck, 7 );
  bool const obback = bit( c.arbAck, 5 );
  bool const bback = ( c.arbAck & 0x42u ) != 0;
  bool const mreqb = g.mreqb;
  std::uint32_t const wIn = gback ? ( gg.progown ? 4u : 1u << gg.msized ) : obback ? 8u : bback ? g.bo.width : xw;
  bool const rwIn = g.rwin;

  // --- abus.v: decoding the address --------------------------------------------------------------
  MemCon const& cf = mCon;
  bool const abs2 = mreqb && g.where.ram1;
  bool const abs3 = mreqb && g.where.ram0;
  bool const fdram = abs2 || abs3;
  bool const fintdev = mreqb && g.where.intdev;
  bool const fextdev = mreqb && g.where.extdev;
  bool const from = g.where.from;
  std::uint32_t const mw = g.where.mw;
  bool const match = ( g.row0 && c.dec.valid0 && abs3 ) || ( g.row1 && c.dec.valid1 && abs2 );

  // --- mem.v -----------------------------------------------------------------------------------
  bool const notrefack = !refack;
  bool const ourack = m.ourack;
  bool const notourack = !ourack;
  bool const readt = ack ? rwIn : m.readsi;
  std::uint32_t const mwt = ack ? mw : m.mws;

  // memwidth.v: the cycles an access of `w` bytes takes on memory `mwt` bytes wide
  Control::Width const& wd = c.width;
  std::uint32_t const rwRem = ( std::uint32_t{ wd.pw } - wd.maskw ) & 0x1f; // {rwc, rw}
  bool const rwc = wd.pw >= wd.maskw;
  bool const lastcycle = !( ( rwRem & 0xf ) != 0 && rwc );
  std::uint32_t const rw4 = rwRem & 0xf;
  std::uint32_t const rw1 = ack ? wIn : rw4;
  std::uint32_t const nba = ( std::uint32_t{ wd.maskai } + wd.maskw ) & 7;
  std::uint32_t const ba1 = ack ? ( g.ab & 7 ) : nba;
  bool const rwlte1 = ( rw1 >> 1 ) == 0;
  bool const rwlte2 = rw1 <= 2;
  bool const rwlte3 = rw1 <= 3;
  bool const rwlte4 = rw1 <= 4;
  bool const last16 = ( bit( ba1, 0 ) && rwlte1 ) || ( !bit( ba1, 0 ) && rwlte2 );
  std::uint32_t const ba2 = ba1 & 3;
  bool const last32 =
      ( ba2 == 3 && rwlte1 ) || ( ba2 == 2 && rwlte2 ) || ( ba2 == 1 && rwlte3 ) || ( ba2 == 0 && rwlte4 );
  bool const lastc = mwt == 3 ? true : mwt == 2 ? last32 : mwt == 1 ? last16 : rwlte1;

  std::uint32_t const dramspeed = 1u << cf.dspd;
  std::uint32_t const iospeed = 1u << cf.iospd;
  std::uint32_t const romspeed = 1u << cf.romspd;
  bool const ds0 = bit( dramspeed, 0 ), ds1 = bit( dramspeed, 1 ), ds2 = bit( dramspeed, 2 ), ds3 = bit( dramspeed, 3 );
  bool const slowrom = !cf.fastrom;
  bool const waitdone = m.wq == 0;
  bool const d0 = ack && !mreqb;
  bool const d1a = ack && mreqb && fdram && !match && notrefack && notourack;
  bool const d1b = m.q1a && ( ds0 || ds1 );
  bool const d1c = ( m.q1a && ds2 ) || m.q1b;
  bool const d1d = ( m.q1a && ds3 ) || m.q1c;
  bool const d2b = m.q2a && ( ds0 || ds1 );
  bool const d2c = ( m.q2a && ds2 ) || m.q2b;
  bool const d3a = ( ack && mreqb && fdram && match && notrefack && notourack ) || ( m.q2a && ds3 ) || m.q2c ||
                   ( m.q3b && !lastcycle );
  bool const d3b = m.q3a; // ram_rdy
  bool const d4a = ack && mreqb && refack;
  bool const d4b = m.q4a && ( ds0 || ds1 );
  bool const d4c = ( m.q4a && ds2 ) || m.q4b;
  bool const d4d = ( m.q4a && ds3 ) || m.q4c;
  bool const d4f = m.q4e && ds0;
  bool const d4g = ( m.q4e && ( ds1 || ds2 ) ) || m.q4f;
  bool const d4h = ( m.q4e && ds3 ) || m.q4g;
  bool const d5a = ( ack && mreqb && from && notrefack && notourack ) || ( m.q5c && !lastcycle );
  bool const d5b = ( m.q5a && slowrom ) || ( m.q5b && !waitdone );
  bool const d5c = ( m.q5a && !slowrom ) || ( m.q5b && waitdone );
  bool const d7a = ( ack && mreqb && fintdev && notrefack && notourack ) || ( m.q7a && !m.readsi && !lastcycle ) ||
                   ( m.q7b && !lastcycle );
  bool const d7b = m.q7a && m.readsi;
  bool const d8a = ( ack && mreqb && fextdev && notrefack && notourack ) || ( m.q8c && !lastcycle );
  bool const d10 = ack && mreqb && ourack && notrefack; // the acknowledge takes a cycle
  s.d7a = d7a;
  s.readt = readt;
  s.ba1 = static_cast<std::uint8_t>( ba1 );
  bool const d8b = m.q8a || ( m.q8b && !waitdone );
  bool const d8c = m.q8b && waitdone;
  bool const nextAck = d0 || ( d3b && lastcycle ) || m.q4h || ( d5c && lastcycle ) || ( d7a && lastc && !readt ) ||
                       ( d8c && lastcycle ) || ( d7b && lastcycle ) || d10;
  bool const nextc = m.q3b || m.q5c || ( m.q7a && !m.readsi ) || m.q7b || m.q8c || ack;

  // wait states (iospd, romspd)
  bool const q5s = m.q5a && slowrom;
  bool const wait1 = bit( iospeed, 2 ) && m.q8a;
  bool const wait2 = bit( romspeed, 3 ) && q5s;
  bool const wait3 = ( bit( romspeed, 2 ) && q5s ) || ( bit( iospeed, 3 ) && m.q8a );
  bool const wait5 = bit( romspeed, 1 ) && q5s;
  bool const wait7 = ( bit( romspeed, 0 ) && q5s ) || ( bit( iospeed, 1 ) && m.q8a );
  bool const wait15 = bit( iospeed, 0 ) && m.q8a;
  bool const wld = wait1 || wait2 || wait3 || wait5 || wait7 || wait15;
  std::uint32_t const wdv = ( ( wait1 || wait3 || wait5 || wait7 || wait15 ) ? 1u : 0u ) |
                            ( ( wait2 || wait3 || wait7 || wait15 ) ? 2u : 0u ) |
                            ( ( wait5 || wait7 || wait15 ) ? 4u : 0u );

  // memwidth.v next state
  std::uint32_t const negba = ( ( ~ba1 & 7 ) + 1 ) & 0xf;
  bool const d216 = !bit( ba1, 0 ) && !bit( ba1, 1 );
  std::uint32_t dW = 1;
  if ( mwt == 3 )
    dW = negba;
  else if ( mwt == 2 )
    dW = ( d216 ? 4u : 0u ) | ( negba & 3 );
  else if ( mwt == 1 )
    dW = ( !bit( ba1, 0 ) ? 2u : 0u ) | ( negba & 1 );
  bool const w32 = bit( rw1, 2 ) || bit( rw1, 3 );
  bool const w16 = bit( rw1, 1 ) || w32;
  bool const direct = ( mwt == 3 && bit( rw1, 3 ) ) || ( mwt == 2 && ( ba1 & 3 ) == 0 && w32 ) ||
                      ( mwt == 1 && !bit( ba1, 0 ) && w16 ) || mwt == 0;
  std::uint32_t const cw1 = lastc ? rw1 : dW;
  std::uint32_t const cw2 = direct ? ( 1u << mwt ) : cw1;

  // --- misc.v: refresh ----------------------------------------------------------------------------
  bool const rpcen = g.pen;
  bool const lrpc = g.rpZero && rpcen;
  bool const rpen = cf.refrate != 0;
  bool const dec = refack && ack && mreqb; // refack (misc) and mreq_in
  bool const refcount = ( lrpc && rpen && !dec ) || ( !lrpc && dec ) || ( !rpen && dec );
  bool const full = bit( c.ref.rc, 3 );

  // --- arb.v next state ---------------------------------------------------------------------------
  bool const bglin = n.m68k.xbgl;
  bool const bgack = ba;
  bool const ad0 = ( a.q0 && !lbr ) || ( a.q0 && bgack ) || ( a.q2 && !lback && !lbr && ack );
  bool const ad1 = ( a.q0 && lbr && !bgack ) || ( a.q1 && bglin ) || ( a.q1 && dreqin );
  bool const ad2 = ( a.q1 && !bglin && !dreqin ) || ( a.q2 && lback ) || ( a.q2 && lbr ) || ( a.q2 && !ack );
  bool const arben = a.q2 && ack;
  bool const ack48 = bit( c.arbAck, 4 ) || bit( c.arbAck, 8 );
  bool const pr48 = bit( pr, 4 ) || bit( pr, 8 );
  bool const dbgd = ( ack48 && ack && pr48 ) || ( a.dbg && pr48 );
  bool const intbmt = ack ? intbm : a.intbms;

  // --- gateway.v, the DSP's (JERRY = 1) and the GPU's ---------------------------------------
  // the gateway's grant and acknowledge: Jerry's grant and its bus master's for the DSP, Tom's
  // latched grant and the memory controller's for the GPU
  struct GatewayNext
  {
    bool gpuBreq, dmaBreq, readi, cycpend, active, dackwt, progown, progownp, packwt, greqt2, gatereq;
  };

  auto const gateway = []( Control::Gateway const& gw,
                           GatewayRequest const& request,
                           bool dmaen,
                           bool portBack,
                           bool portAck,
                           Signals::Gateway& f )
  {
    GatewayNext x{};
    // nothing asked and nothing under way: every signal holds, as below
    if ( !request.valid && !gw.active && !gw.cycpend && !gw.dackwt && !gw.packwt && !gw.greqt2 && !gw.gatereq )
    {
      x.readi = gw.reada;
      x.progown = gw.progown;
      x.progownp = gw.progownp;
      return x;
    }
    f.external = request.valid;
    f.gpuMemw = request.valid && request.write;
    bool const active = gw.active;
    bool const cycgo = f.external || gw.cycpend;
    x.gpuBreq = !dmaen && active;
    x.dmaBreq = active && dmaen;
    bool const busack = ( x.gpuBreq || x.dmaBreq ) && portBack;
    f.gpuAck = portAck && busack;
    bool const cycstart = ( active && f.gpuAck && cycgo ) || ( !active && f.external );
    x.readi = cycstart ? ( gw.cycpend ? gw.readp : !f.gpuMemw ) : gw.reada;
    bool const cyptset = f.external && active && !cycstart;
    x.cycpend = ( cyptset || gw.cycpend ) && !cycstart;
    x.active = ( !active && f.external ) || ( active && !f.gpuAck ) || ( active && f.gpuAck && cycgo );
    x.dackwt = ( active && !gw.progown && f.gpuAck && gw.reada ) || ( gw.dackwt && !portAck );
    f.ddatld = gw.dackwt && portAck;
    // a program fetch (progoi, pawset, pdatld); its data handed on the cycle after it is taken
    // (gatereq, until the processor's bus serves it: gateack, the cycle after)
    x.progown = cycstart ? ( gw.cycpend ? gw.progownp : request.program ) : gw.progown;
    x.progownp = f.external ? request.program : gw.progownp;
    x.packwt = ( active && gw.progown && f.gpuAck ) || ( gw.packwt && !portAck );
    f.pdatld = gw.packwt && portAck;
    bool const gateack = gw.gatereq;
    x.gatereq = ( f.pdatld || gw.greqt2 ) && !gateack;
    x.greqt2 = ( f.pdatld || gw.greqt2 ) && !gateack;
    return x;
  };
  GatewayNext const dn = gateway( dg, *g.request[DSP], g.dmaen[DSP], dbg, jack, s.gw[DSP] );
  GatewayNext const gn = gateway( gg, *g.request[GPU], g.dmaen[GPU], gback, ack, s.gw[GPU] );
  bool const gpuBreq = dn.gpuBreq;
  bool const dmaBreq = dn.dmaBreq;
  std::uint32_t const width = 1u << dg.msized; // width_out

  // --- Jerry's store: its data taken the cycle after it starts, its halves written as CAS falls
  // (startcas, q3a), or passed in I/O cycles; a new one started as Jerry starts a bus cycle
  std::uint8_t halves = j.storeHalves;
  bool storePending = j.storePending;
  if ( storePending )
  {
    storePending = false;
    halves = j.storeSize == 2 ? 2u : 1u;
  }
  s.jerryHalves = halves;
  if ( m.q3a && halves != 0 && dbg )
    --halves;
  else if ( halves != 0 && ( m.q8a || m.q7a || m.q5a ) && dbg ) // not memory
    --halves;
  s.jerryStart = ( j.q0 && dbg && mreq ) || ( j.q4 && dbg );
  std::uint8_t storeSize = j.storeSize;
  if ( s.jerryStart && !dg.reada )
  {
    storePending = true;
    storeSize = dg.msized;
  }
  n.jerry.storePending = storePending;
  n.jerry.storeHalves = halves;
  n.jerry.storeSize = storeSize;

  // --- Jerry's slave (j_jmem.v qs0 to qs4) follows Tom's I/O cycles to it (dspcsl, the strobe
  // while q8b holds)
  {
    bool const dspcs = m.q8a || m.q8b || m.q8c;
    bool const strobe = m.q8b;
    bool const qs0 = bit( c.qs, 0 ), qs1 = bit( c.qs, 1 ), qs2 = bit( c.qs, 2 ), qs3 = bit( c.qs, 3 ),
               qs4 = bit( c.qs, 4 );
    // ds0, ds1, ds4
    bool const next0 = ( qs0 && !dspcs ) || ( qs0 && !strobe ) || ( qs3 && !strobe ) || ( qs4 && !strobe );
    bool const next1 = qs0 && dspcs && strobe;
    bool const next4 = ( qs3 && strobe ) || ( qs4 && strobe );
    n.qs = static_cast<std::uint8_t>( ( next0 ? 1u : 0u ) | ( next1 ? 2u : 0u ) | ( qs1 ? 4u : 0u ) |
                                      ( qs2 ? 8u : 0u ) | ( next4 ? 16u : 0u ) );
  }

  // --- the clock edge ------------------------------------------------------------------------------
  // Jerry
  s.jack = jack;
  if ( jack )
  {
    n.jerry.ws = static_cast<std::uint8_t>( width );
    n.jerry.readsi = dg.reada;
    n.jerry.dbrls = static_cast<std::uint8_t>( ( gpuBreq ? 0u : 1u ) | ( dmaBreq ? 0u : 2u ) );
  }
  n.jerry.q0 = jd0;
  n.jerry.q1 = jd1;
  n.jerry.q2 = jd2;
  n.jerry.q3 = jd3;
  n.jerry.q4 = jd4;

  // the gateways: the direction and the size of an access asked for (dald), the fetch's
  auto const latch =
      []( Control::Gateway& gw, GatewayRequest const& request, GatewayNext const& next, Signals::Gateway const& f )
  {
    if ( f.external )
    {
      gw.readp = !f.gpuMemw;
      if ( !request.program )
        gw.msized = static_cast<std::uint8_t>( request.size );
    }
    gw.reada = next.readi;
    gw.cycpend = next.cycpend;
    gw.active = next.active;
    gw.dackwt = next.dackwt;
    gw.xldReady = f.ddatld;
    gw.progown = next.progown;
    gw.progownp = next.progownp;
    gw.packwt = next.packwt;
    gw.greqt2 = next.greqt2;
    gw.gatereq = next.gatereq;
  };
  latch( n.gw[DSP], *g.request[DSP], dn, s.gw[DSP] );
  latch( n.gw[GPU], *g.request[GPU], gn, s.gw[GPU] );

  // Tom's arbiter
  if ( arben )
    n.arbAck = static_cast<std::uint16_t>( pr );
  n.arb.q0 = ad0;
  n.arb.q1 = ad1;
  n.arb.q2 = ad2;
  n.arb.dbg = dbgd;
  n.arb.intbms = intbmt;

  // cpu.v
  n.cpu.q11 = cd11;
  n.cpu.q12 = cd12;
  n.cpu.q13 = cd13;

  // abus.v: the open rows' validity (newrow is q1a, at the falling edge of the cycle; resrow, q4a,
  // closes them), the banks the access selects
  if ( m.q1a )
  {
    if ( c.dec.bs3 )
      n.dec.valid0 = true;
    if ( c.dec.bs2 )
      n.dec.valid1 = true;
  }
  else if ( m.q4a )
  {
    n.dec.valid0 = false;
    n.dec.valid1 = false;
  }
  if ( ack )
  {
    n.dec.bs3 = abs3;
    n.dec.bs2 = abs2;
  }

  // mem.v
  Control::Memory& nm = n.mem;
  nm.idle = d0;
  nm.q1a = d1a;
  nm.q1b = d1b;
  nm.q1c = d1c;
  nm.q1d = d1d;
  nm.q2a = m.q1d;
  nm.q2b = d2b;
  nm.q2c = d2c;
  nm.q3a = d3a;
  nm.q3b = d3b;
  nm.q4a = d4a;
  nm.q4b = d4b;
  nm.q4c = d4c;
  nm.q4d = d4d;
  nm.q4e = m.q4d;
  nm.q4f = d4f;
  nm.q4g = d4g;
  nm.q4h = d4h;
  nm.q5a = d5a;
  nm.q5b = d5b;
  nm.q5c = d5c;
  nm.q7a = d7a;
  nm.q7b = d7b;
  nm.q8a = d8a;
  nm.q8b = d8b;
  nm.q8c = d8c;
  if ( ack && mreqb )
  {
    nm.gpuCycle = gback;
    nm.opCycle = obback;
    nm.opRead = rwIn;
    nm.blitCycle = bback;
    nm.m68kCycle = mEmulated68k && xp && !dbg && !refack;
    nm.busRead = rwIn;
    nm.busWidth = static_cast<std::uint8_t>( wIn );
    nm.busJustify = gback || ( bback && g.bo.justify ); // the GPU's gateway justifies, as the blitter's pixels
  }
  s.d10 = d10;
  nm.ack = nextAck;
  nm.readsi = readt;
  // abus.v intd: the 68000's cycle in function code 7 on Tom's external interface, latched
  nm.ourack = mEmulated68k && dreqin && g.fc7 && xp && !dbg;
  nm.mws = static_cast<std::uint8_t>( mwt );
  if ( wld )
    nm.wq = static_cast<std::uint8_t>( ( wait15 ? 8u : 0u ) | wdv );
  else if ( m.wq != 0 )
    nm.wq = static_cast<std::uint8_t>( m.wq - 1 );

  // memwidth.v
  std::uint32_t const atW = nextc ? ba1 : wd.maskai;
  if ( nextc )
  {
    n.width.pw = static_cast<std::uint8_t>( rw1 );
    n.width.maskw = static_cast<std::uint8_t>( cw2 );
  }
  n.width.maskai = static_cast<std::uint8_t>( atW );

  // misc.v
  bool const rq = c.ref.rq;
  n.ref.rq = ( !rq && full ) || ( !rq && g.oo.startref && notempty ) || ( rq && notempty );
  std::uint32_t rc = c.ref.rc;
  if ( lrpc && refcount )
    ++rc;
  else if ( !lrpc && refcount )
    --rc;
  n.ref.rc = static_cast<std::uint8_t>( rc & 0xf );

  s.rwIn = rwIn;
  for ( Signals::Gateway const& f : s.gw )
    s.gateways = s.gateways || f.external || f.gpuMemw || f.gpuAck || f.ddatld || f.pdatld;
}

void MainBus::data( std::uint64_t cycle, Gathered& g, Signals const& s, Control const& n, bool restful )
{
  Control const& c = *mCtl; // the state the cycle began in
  Control::Memory const& m = c.mem;
  M68kCycle& mc = mM68k;
  Jerry& j = mJerry;
  Gateway& dg = mGw[DSP];
  Gateway& gg = mGw[GPU];
  Slave& sl = mSlave;
  GatewayRequest const& dspRequest = *g.request[DSP];
  GatewayRequest const& gpuRequest = *g.request[GPU];
  bool const dbg = c.arb.dbg;
  bool const ack = m.ack;
  bool const waitdone = m.wq == 0;
  bool const obback = bit( c.arbAck, 5 );
  bool const gback = bit( c.arbAck, 2 ) || bit( c.arbAck, 7 );
  bool const bback = ( c.arbAck & 0x42u ) != 0;
  std::uint32_t const ain = g.ain;

  // gpu_cpu.v ioreq: an internal I/O cycle at the GPU's space starting (abus.v gpuread, gpuwrite,
  // with d7a), of a long word's high half read or its low half written, takes the GPU's local bus
  // in this cycle
  if ( s.d7a && mGpuSlave )
  {
    std::uint32_t const io = ( ( ack ? g.ab : mRows.aout << 3 ) & ~7u ) | s.ba1;
    if ( ( io & 0x6000u ) != 0 && ( s.readt ? !bit( io, 1 ) : bit( io, 1 ) || bit( io, 15 ) ) )
      mGpuSlave->slaveRequest( cycle );
  }

  // a read of Tom's registers is latched as its I/O cycle ends (dinlatch, q7b), as the gateway
  // takes it
  if ( gg.loadHalves != 0 && m.gpuCycle && m.q7b && m.readsi )
  {
    std::uint32_t const addr = gg.loadAddress;
    std::uint32_t const half = gg.loadSize == 2 ? longHalf( addr, 2u - gg.loadHalves ) : addr & ~1u;
    std::uint16_t const v = tomRead( cycle, half, "gpu", gg.loadPc );
    // the data bus's lanes of the word take it (dbus.v dr, dinlatch at q7b)
    std::uint32_t const shift = 48 - 8 * ( half & 6u );
    mDataBus = ( mDataBus & ~( std::uint64_t{ 0xffff } << shift ) ) | static_cast<std::uint64_t>( v ) << shift;
    // a long word is the bus's half the last word's address names, whatever the other lanes hold
    if ( gg.loadSize == 2 )
      gg.pendData = static_cast<std::uint32_t>( mDataBus >> ( bit( half, 2 ) ? 0 : 32 ) );
    else
      gg.pendData = gg.loadSize == 0 ? ( ( addr & 1u ) ? v & 0xffu : v >> 8 ) : v;
    --gg.loadHalves;
  }

  // the gateways' data as the clock edge takes it: a store's is in load_data from the cycle it is
  // served (lodwr), a load's is taken from the bus as the last half of it ends (ddatld); prog_data,
  // the long word a fetch read, taken before a cycle acknowledged with it reads
  std::uint32_t loadData[2] = {};
  std::uint32_t progData[2] = {};
  for ( std::size_t i : { DSP, GPU } )
  {
    if ( !s.gateways )
      break;
    Gateway const& gw = mGw[i];
    Signals::Gateway const& f = s.gw[i];
    loadData[i] = gw.loadData;
    if ( f.gpuMemw )
      loadData[i] = f.ddatld ? 0 : g.request[i]->data;
    else if ( f.ddatld )
    {
      std::uint32_t const v = gw.pendData;
      loadData[i] = ( v & 0xff ) | ( gw.maskbN ? v & 0xff00 : 0 ) | ( gw.maskwN ? v & 0xffff0000u : 0 );
    }
    progData[i] = f.pdatld ? gw.pendData : gw.progData;
  }

  // The GPU's cycle starts as Tom acknowledges it: a load's data moves now (64 bits: a phrase's
  // second long word is the register's, its first G_HIDATA's), a store waits for its memory cycle
  Control::Gateway const& ggc = c.gw[GPU];

  struct TakenStore
  {
    std::uint32_t address;
    std::uint32_t size;
    std::uint32_t data;
    std::uint32_t hi;
    std::uint32_t halves;
  };

  std::optional<TakenStore> takenStore;
  if ( ggc.active && s.gw[GPU].gpuAck && ggc.progown ) // a fetch of the program: the long word at cpu_addr
  {
    std::uint32_t const fetch = gg.cpuAddr & ~3u;
    gg.pendData = boardAt( fetch ) ? boardBytes( fetch, 4 ) : mMemory.readLong( fetch );
  }
  else if ( ggc.active && s.gw[GPU].gpuAck )
  {
    std::uint32_t const addr = gg.datAddr;
    std::uint32_t const msized = ggc.msized;
    if ( ggc.reada )
    {
      std::uint32_t const phrase = addr & ~7u;
      gg.pendHi = mMemory.readLong( phrase );
      if ( boardAt( addr ) ) // the OS ROM or the cartridge
      {
        gg.pendHi = boardBytes( phrase, 4 );
        gg.pendData =
            msized == 3 ? boardBytes( phrase + 4, 4 ) : boardBytes( addr & ~( ( 1u << msized ) - 1 ), 1u << msized );
      }
      else if ( ( addr & 0xff8000u ) == 0xf18000u && mDspSlave )
      {
        // the DSP's space through Jerry's slave (gpu_cpu.v of the DSP): a long word read as its
        // high half is, its low half latched with it
        std::uint32_t const v = mDspSlave->slaveRead( cycle, addr & ~3u );
        gg.pendHi = msized == 3 ? mDspSlave->slaveRead( cycle, phrase ) : v;
        if ( msized == 3 )
          gg.pendData = mDspSlave->slaveRead( cycle, phrase + 4 );
        else if ( msized == 2 )
          gg.pendData = v;
        else if ( msized == 1 )
          gg.pendData = bit( addr, 1 ) ? v & 0xffffu : v >> 16;
        else
          gg.pendData = ( v >> ( 24 - 8 * ( addr & 3u ) ) ) & 0xffu;
      }
      else
        switch ( msized )
        {
        case 0:
          gg.pendData = mMemory.readByte( addr );
          break;
        case 1:
          gg.pendData = mMemory.readWord( addr & ~1u );
          break;
        case 2:
          gg.pendData = mMemory.readLong( addr & ~3u );
          break;
        default:
          gg.pendData = mMemory.readLong( phrase + 4 );
          break;
        }
      // Tom's registers, 16 bits wide: latched a half at a time in its I/O cycles, below
      gg.loadHalves = Tom::decodes( addr ) ? ( msized == 2 ? 2u : 1u ) : 0u;
      gg.loadAddress = addr;
      gg.loadPc = gg.datPc;
      gg.loadSize = msized;
      if ( gg.loadHalves != 0 )
        gg.pendData = 0;
    }
    else
    {
      // Tom takes the data as it takes the cycle; the store waits below, for the cycle may be the
      // last of the store before (a 16 bit device's, the next acknowledged as it writes)
      takenStore = TakenStore{ .address = addr,
                               .size = msized,
                               .data = gg.loadData,
                               .hi = gg.hirdata,
                               .halves = msized >= 2 ? 1u << ( msized - 1 ) : 1u };
      // the data bus's lanes take it in the next cycle, justified ({hirdata, load_data}, wden)
      mGpuDrive = true;
      mGpuDriveData = upShift( static_cast<std::uint64_t>( gg.hirdata ) << 32 | gg.loadData, 1u << msized, true );
    }
  }
  // the object processor's header written back as CAS falls, from the data bus it drove
  if ( m.opCycle && !m.opRead && m.q3a )
  {
    std::uint64_t const v = mDataBus;
    for ( std::uint32_t b = 0; b < 8; ++b )
      mMemory.writeByte( mAddr.op + b, static_cast<std::uint8_t>( v >> ( 56 - 8 * b ) ) );
  }
  if ( g.blitRuns )
  {
    bool const ena = mBlitter.dataEna();
    std::uint64_t const wdata = mBlitter.clock( g.bi );
    if ( ena )
      mDataBus = upShift( wdata, m.busWidth, m.busJustify );
    if ( mBlitter.interrupt() )
      mBlitGpuIrq = cycle;
  }
  if ( m.blitCycle )
  {
    if ( m.blitCycle && !m.busRead && m.q3a )
    {
      std::uint32_t const o = mAddr.bus & 7u;
      for ( std::uint32_t b = o; b < 8 && b < o + m.busWidth; ++b )
        mMemory.writeByte( ( mAddr.bus & ~7u ) + b, static_cast<std::uint8_t>( mDataBus >> ( 56 - 8 * b ) ) );
    }
    // The blitter's cycles at the GPU's space, 16 bits wide: an internal I/O cycle a word, the part of
    // the access memwidth.v gives it (maskai), through gpu_cpu.v as the 68000's: a long word's high
    // half latched, its low half served by the GPU's memory controller in the next cycle; read, the
    // long word read as the port serves it (q7a), its lanes latched as the cycle ends (above)
    if ( m.blitCycle && m.busRead && m.q7a && mGpuSlave )
    {
      std::uint32_t const addr = ( mAddr.bus & ~7u ) + ( c.width.maskai & 6u );
      if ( gpuSpace( addr ) && !bit( addr, 1 ) )
        mGpuPort.rddata = mGpuSlave->slaveRead( cycle, gpuPortAddress( addr ) ); // the port serves it (ioserv)
    }
    if ( m.blitCycle && !m.busRead && m.q7a && mGpuSlave )
    {
      std::uint32_t const addr = ( mAddr.bus & ~7u ) + ( c.width.maskai & 6u );
      std::uint32_t const shift = 48 - 8 * ( c.width.maskai & 6u );
      if ( gpuSpace( addr ) && bit( addr, 15 ) )
      {
        // a 32 bit write (gpu_cpu.v at[15]): the long word of the data bus's lanes (dwrite, dbus.v
        // dout), in an I/O cycle of its own (Tom's space from $F08000 is 32 bits wide)
        std::uint32_t const dwrite = static_cast<std::uint32_t>( mDataBus >> ( bit( addr, 2 ) ? 0 : 32 ) );
        if ( !bit( addr, 1 ) )
          mGpuPort.lodata = static_cast<std::uint16_t>( dwrite );
        mGpuPort.ioserv = true;
        mGpuPort.ioAddress = gpuPortAddress( addr );
        mGpuPort.ioData = dwrite;
      }
      else if ( gpuSpace( addr ) )
      {
        std::uint16_t const v = static_cast<std::uint16_t>( mDataBus >> shift );
        if ( !bit( addr, 1 ) )
          mGpuPort.lodata = v;
        else
        {
          mGpuPort.ioserv = true;
          mGpuPort.ioAddress = gpuPortAddress( addr );
          mGpuPort.ioData = static_cast<std::uint32_t>( mGpuPort.lodata ) << 16 | v;
        }
      }
    }
    // and at the rest of Tom's own space, its registers, the CLUT and the line buffers: a word each
    // I/O cycle, as a 68000's write; through the 32 bit window onto the line buffers ($F08800 on,
    // lbuf.v aout[15] with a long word) both pixels of the long word at once (wea12, web12), the
    // halves as the bus's endianness orders them (dw: down, up)
    if ( m.blitCycle && !m.busRead && m.q7a )
    {
      std::uint32_t const addr = ( mAddr.bus & ~7u ) + ( c.width.maskai & 6u );
      std::uint32_t const at = addr & ~0x8000u;
      if ( Tom::decodes( at ) && !bit( addr, 15 ) )
        tomWrite( cycle, addr, static_cast<std::uint16_t>( mDataBus >> ( 48 - 8 * ( c.width.maskai & 6u ) ) ) );
      else
        lineBufferLongWrite( cycle, addr );
    }
  }

  // The emulated 68000's cycle: a read's data taken as Tom's memory cycle latches it (the DRAM's at
  // q3b, the ROM's at q5c, Tom's registers at q7b, Jerry's at the end of q8b), a write's put where
  // Tom's cycle puts it (the DRAM as CAS falls, Tom's registers at q7a, Jerry's slave and timers in
  // its I/O cycle)
  bool m68kHalf = false; // the I/O cycle starting is the 68000's write of a half to Jerry
  if ( m.m68kCycle )
  {
    M68kAccess const& acc = mc.access;
    std::uint32_t const addr = acc.address;
    if ( m.readsi )
    {
      if ( m.q3b || m.q5c )
      {
        mc.readData = m68kReadWord( addr, acc.uds, acc.lds );
        if ( mRec.on )
          spanRecord( cycle, S_READ );
      }
      else if ( m.q7a && gpuSpace( addr ) && !bit( addr, 1 ) && mGpuSlave )
        mGpuPort.rddata = mGpuSlave->slaveRead( cycle, gpuPortAddress( addr ) ); // the port serves it (ioserv)
      else if ( m.q7b && gpuSpace( addr ) && mGpuSlave )
      {
        // the high half the long word's as the port read it, the low half latched for its read
        if ( !bit( addr, 1 ) )
        {
          std::uint32_t const v = mGpuPort.rddata;
          mc.readData = static_cast<std::uint16_t>( v >> 16 );
          mGpuPort.latrdata = static_cast<std::uint16_t>( v );
        }
        else
          mc.readData = mGpuPort.latrdata;
      }
      else if ( m.q7b )
        mc.readData = tomRead( cycle, addr, "m68k", 0 );
      else if ( m.q8b && waitdone )
      {
        // the timers, the joystick ports, the DSP's space through Jerry's slave (as the GPU's: the
        // high half the long word's, the low half latched for its read); nothing else of Jerry's
        // drives the bus, pulled up
        if ( addr - 0xf10036u < 8 )
          mc.readData = static_cast<std::uint16_t>( mTimers.read( cycle, ( addr - 0xf10036u ) >> 1 ) );
        else if ( ( addr & 0xfffffeu ) == 0xf10020u )
          mc.readData = mJerryInts.read( cycle );
        else if ( ( addr & 0xff8000u ) == 0xf18000u && mDspSlave )
        {
          if ( !bit( addr, 1 ) )
          {
            std::uint32_t const v = mSlave.rddata;
            mc.readData = static_cast<std::uint16_t>( v >> 16 );
            mSlave.latrdata = static_cast<std::uint16_t>( v );
          }
          else
            mc.readData = mSlave.latrdata;
        }
        else
          mc.readData = ( addr & 0xfffffcu ) == 0xf14000u ? mMemory.readWord( addr ) : 0xffffu;
      }
    }
    else if ( m.q3a && !mc.written )
    {
      mc.written = true;
      if ( mRec.on )
        spanRecord( cycle, S_WRITE );
      // the data bus's lanes take the word the 68000 drives, repeated, as Jerry's
      mDataBus = acc.data * 0x0001000100010001u;
      if ( lookup( mDecode, addr ).ram0 ) // bank 1 is empty
      {
        if ( acc.uds )
          mMemory.writeByte( addr & 0x3fffffu, static_cast<std::uint8_t>( acc.data >> 8 ) );
        if ( acc.lds )
          mMemory.writeByte( ( addr & 0x3fffffu ) + 1, static_cast<std::uint8_t>( acc.data ) );
      }
    }
    else if ( m.q7a && !mc.written && gpuSpace( addr ) )
    {
      mc.written = true;
      if ( bit( addr, 15 ) )
      {
        // a 32 bit write (gpu_cpu.v at[15]): each half writes the long word the data bus's lanes
        // hold (dwrite, dbus.v dout), the word the 68000 drives repeated in them
        mDataBus = acc.data * 0x0001000100010001u;
        std::uint32_t const dwrite = static_cast<std::uint32_t>( mDataBus );
        if ( !bit( addr, 1 ) )
          mGpuPort.lodata = static_cast<std::uint16_t>( dwrite );
        mGpuPort.ioserv = true;
        mGpuPort.ioAddress = gpuPortAddress( addr );
        mGpuPort.ioData = dwrite;
      }
      else if ( !bit( addr, 1 ) )
        mGpuPort.lodata = acc.data;
      else
      {
        mGpuPort.ioserv = true;
        mGpuPort.ioAddress = gpuPortAddress( addr );
        mGpuPort.ioData = static_cast<std::uint32_t>( mGpuPort.lodata ) << 16 | acc.data;
      }
    }
    else if ( m.q7a && !mc.written )
    {
      mc.written = true;
      tomWrite( cycle, addr, acc.data );
    }
    else if ( m.q8a )
    {
      mSlave.address = addr | ( acc.uds ? 0u : 1u );
      mSlave.data = acc.data;
      mSlave.write = true;
      m68kHalf = true;
    }
    else if ( m.q8b && ( addr & 0xfffff8u ) == 0xf10000u )
    {
      // the strobe held through Tom's I/O cycle, the counter reloaded at the end of each
      mc.written = true;
      mTom.interrupt( cycle ); // what the timers requested before the write has reached INT1
      mIhandlerUntil = 0;
      mTimers.strobe( cycle, ( addr >> 1 ) & 3u, acc.data );
    }
    else if ( m.q8b && ( addr & 0xfffffeu ) == 0xf10020u )
    {
      mc.written = true;
      mTom.interrupt( cycle );
      mIhandlerUntil = 0;
      mJerryInts.write( cycle, acc.data );
    }
    else if ( m.q8b && !mc.written && ( addr & 0xfffffeu ) == 0xf14000u )
    {
      mc.written = true;
      joystickWrite( acc.data );
    }
  }

  // what the object processor latches (ack_pipe): the data bus
  std::uint64_t const opData = mDataBus;

  bool gpuHalf = false; // the I/O cycle starting is the GPU's write of a half to Jerry
  if ( gg.store && m.gpuCycle && ( m.q3a || m.q5a || m.q7a || m.q8a ) )
  {
    std::uint32_t const addr = gg.storeAddress;
    std::uint64_t const v = static_cast<std::uint64_t>( gg.storeHi ) << 32 | gg.storeData;
    if ( m.q7a && gpuSpace( addr ) && bit( addr, 15 ) )
    {
      // its own local RAM through Tom's 32 bit window onto it (gpu_cpu.v at[15]): a long word each
      // I/O cycle, the data bus's lanes the memory controller's address names, served by its slave
      // port as the blitter's writes there are
      std::uint32_t const at = ( mAddr.bus & ~7u ) + ( c.width.maskai & 6u );
      if ( mGpuSlave )
      {
        mGpuPort.ioserv = true;
        mGpuPort.ioAddress = gpuPortAddress( at );
        mGpuPort.ioData = static_cast<std::uint32_t>( mDataBus >> ( bit( at, 2 ) ? 0 : 32 ) );
      }
      gg.storeHalves = gg.storeHalves > 2 ? gg.storeHalves - 2 : 0;
      gg.store = gg.storeHalves != 0;
    }
    else if ( m.q7a && Tom::decodes( addr ) )
    {
      // Tom's registers, 16 bits wide: a half each I/O cycle, of a long word from its address on,
      // of a phrase from its start, the high one first
      std::uint32_t const bytes = 1u << gg.storeSize;
      std::uint32_t const k = ( bytes >= 2 ? bytes / 2 : 1u ) - gg.storeHalves;
      std::uint32_t half = addr & ~1u;
      std::uint16_t value = static_cast<std::uint16_t>( gg.storeData );
      if ( bytes == 4 )
      {
        half = longHalf( addr, k );
        value = longLane( half, gg.storeData );
      }
      else if ( bytes == 8 )
      {
        half = ( addr & ~7u ) + 2 * k;
        value = static_cast<std::uint16_t>( v >> ( 48 - 16 * k ) );
      }
      tomWrite( cycle, half, value );
      gg.store = --gg.storeHalves != 0;
    }
    else if ( m.q7a && Tom::decodes( addr & ~0x8000u ) )
    {
      // the 32 bit window onto the line buffers, as the blitter's writes there: a long word each I/O
      // cycle, the data bus's lanes the memory controller's address names
      lineBufferLongWrite( cycle, ( mAddr.bus & ~7u ) + ( c.width.maskai & 6u ) );
      gg.storeHalves = gg.storeHalves > 2 ? gg.storeHalves - 2 : 0;
      gg.store = gg.storeHalves != 0;
    }
    else if ( m.q8a && ( addr & 0xff0000u ) == 0xf10000u )
    {
      // Jerry, 16 bits wide: a half each I/O cycle, for its slave; a long word's as Tom's
      // registers take them, a phrase's the high one first
      std::uint32_t const bytes = 1u << gg.storeSize;
      std::uint32_t const k = ( bytes >= 2 ? bytes / 2 : 1u ) - gg.storeHalves;
      if ( bytes == 4 )
      {
        mSlave.address = longHalf( addr, k );
        mSlave.data = longLane( mSlave.address, gg.storeData );
      }
      else
      {
        mSlave.address = bytes >= 2 ? ( addr & ~( bytes - 1 ) ) + 2 * k : addr;
        std::uint32_t const shift = bytes >= 2 ? 8 * ( bytes - 2 - 2 * k ) : 0;
        mSlave.data = static_cast<std::uint16_t>( ( gg.storeSize == 3 ? v : gg.storeData ) >> shift );
      }
      mSlave.write = true;
      gpuHalf = true;
      gg.store = --gg.storeHalves != 0;
    }
    else
    {
      // the DRAM takes it as CAS falls; other memories are not modelled
      gg.store = false;
      if ( m.q3a )
        switch ( gg.storeSize )
        {
        case 0:
          mMemory.writeByte( addr, static_cast<std::uint8_t>( gg.storeData ) );
          break;
        case 1:
          mMemory.writeWord( addr & ~1u, static_cast<std::uint16_t>( gg.storeData ) );
          break;
        case 2:
          // the long word on both halves of the bus: its four bytes from its address, up to the
          // phrase's end
          for ( std::uint32_t b = addr & 7u; b < std::min( 8u, ( addr & 7u ) + 4 ); ++b )
            mMemory.writeByte( ( addr & ~7u ) + b,
                               static_cast<std::uint8_t>( gg.storeData >> ( 24 - 8 * ( b & 3u ) ) ) );
          break;
        default:
          // a phrase is not shifted: the bytes from its address to the phrase's end are written
          for ( std::uint32_t b = addr & 7u; b < 8; ++b )
            mMemory.writeByte( ( addr & ~7u ) + b, static_cast<std::uint8_t>( v >> ( 56 - 8 * b ) ) );
          break;
        }
    }
  }
  if ( takenStore )
  {
    gg.store = true;
    gg.storeAddress = takenStore->address;
    gg.storeSize = takenStore->size;
    gg.storeData = takenStore->data;
    gg.storeHi = takenStore->hi;
    gg.storeHalves = takenStore->halves;
  }
  if ( s.gw[GPU].ddatld )
    gg.hirdata = gg.pendHi;

  // Jerry's slave follows Tom's I/O cycles to it; in qs1 a write in the DSP's space (at[15]) reaches
  // the DSP (dspwrite): the high half of a long word is latched (lodld), the low half asks the DSP's
  // memory controller for its bus (ioreq), which serves the write in the next cycle (ioservt).
  // A read of a long word's high half in the DSP's space asks the DSP's local bus for a cycle
  // (gpu_cpu.v of the DSP, ioreq)
  if ( m.q8a && m.readsi && mDspSlave )
  {
    std::uint32_t const io = ( mAddr.bus & ~7u ) | c.width.maskai;
    if ( ( io & 0xff8000u ) == 0xf18000u && !bit( io, 1 ) )
    {
      mDspSlave->slaveRequest( cycle + DSP_READ_IOREQ );
      mSlave.rdCycle = cycle + DSP_READ_IOREQ + 1; // ioserv
      mSlave.rdAddress = io & ~3u;
    }
  }
  // the blitter's write of a half into the DSP's space: its I/O cycle to Jerry's slave, the part
  // of the access memwidth.v gives it, from the data bus's lanes
  bool blitHalf = false;
  if ( m.q8a && m.blitCycle && !m.busRead )
  {
    std::uint32_t const io = ( mAddr.bus & ~7u ) | ( c.width.maskai & 6u );
    if ( ( io & 0xff8000u ) == 0xf18000u )
    {
      mSlave.address = io;
      mSlave.data = static_cast<std::uint16_t>( mDataBus >> ( 48 - 8 * ( io & 6u ) ) );
      mSlave.write = true;
      blitHalf = true;
    }
  }
  if ( m.q8a && !gpuHalf && !m68kHalf && !blitHalf ) // another master's I/O cycle: nothing for the DSP
    sl.write = false;
  if ( mGpuPort.ioserv )
  {
    mGpuPort.ioserv = false;
    if ( mGpuSlave )
      mGpuSlave->slaveWrite( cycle, mGpuPort.ioAddress, mGpuPort.ioData );
  }
  if ( sl.ioserv )
  {
    sl.ioserv = false;
    if ( mDspSlave )
      mDspSlave->slaveWrite( cycle, sl.ioAddress, sl.ioData );
  }
  if ( ( c.qs & 2u ) && sl.write && ( sl.address & 0xff8000u ) == 0xf18000u )
  {
    if ( !bit( sl.address, 1 ) )
      sl.lodata = sl.data;
    else
    {
      sl.ioserv = true;
      sl.ioAddress = 0xf18000u | ( sl.address & 0x7ffcu );
      sl.ioData = static_cast<std::uint32_t>( sl.lodata ) << 16 | sl.data;
      if ( mDspSlave )
        mDspSlave->slaveRequest( cycle );
    }
  }

  // A store's data is taken into Jerry's data latch (d3) the cycle after it starts (lwdl): from the
  // gateway when Jerry has the bus then, else from the bus, its high half pulled up and its low
  // half what it last carried (open bus)
  Control::Jerry const& jc = c.jerry;
  if ( jc.storePending )
  {
    std::uint32_t data = dg.loadData;
    if ( !dbg )
      data = 0xffff0000u | j.openLow;
    if ( jc.storeSize == 0 )
      data = ( data & 0xff ) * 0x0101u;
    j.storeData = data;
    j.openLow = static_cast<std::uint16_t>( data );
  }
  // the DRAM takes a half of a store as CAS falls (startcas, q3a)
  if ( m.q3a && s.jerryHalves != 0 && dbg )
  {
    // the data bus's lanes take the half Jerry drives, repeated (pw16)
    std::uint32_t const half = jc.storeSize != 2 || s.jerryHalves == 1 ? j.storeData & 0xffff : j.storeData >> 16;
    mDataBus = ( jc.storeSize == 0 ? ( half & 0xff ) * 0x0101u : half ) * 0x0001000100010001u;
    if ( jc.storeSize != 2 )
      write( j.storeAddress, jc.storeSize, j.storeData );
    else if ( s.jerryHalves == 2 )
      write( j.storeAddress & ~1u, 1, j.storeData >> 16 );
    else
      write( ( j.storeAddress | 2u ) & ~1u, 1, j.storeData & 0xffff );
  }
  // Jerry's I/O cycles and Tom's registers on Jerry's grant
  if ( m.q8b || m.q8c || ( ( m.q7a || m.q7b ) && dbg ) )
  {
    // Jerry's own registers on the bus: a timer is written through Tom's I/O cycle, while q8b holds
    // the strobe (pit1w to pit4w), with the half of the store on the bus
    std::uint32_t const storeHalf = jc.storeSize == 2 ? ( jc.q3 ? j.storeData : j.storeData >> 16 ) : j.storeData;
    if ( m.q8b && !m.readsi && dbg && ( ain & 0xfffff8u ) == 0xf10000u )
    {
      mTom.interrupt( cycle );
      mIhandlerUntil = 0;
      mTimers.strobe( cycle, ( ain >> 1 ) & 3u, static_cast<std::uint16_t>( storeHalf ) );
    }
    else if ( m.q8b && !m.readsi && m.gpuCycle && sl.write && ( sl.address & 0xfffff8u ) == 0xf10000u ) // the GPU's
    {
      mTom.interrupt( cycle );
      mIhandlerUntil = 0;
      mTimers.strobe( cycle, ( sl.address >> 1 ) & 3u, sl.data );
    }
    // JINTCTRL, written the same way
    if ( m.q8b && !m.readsi && dbg && ( ain & 0xfffffeu ) == 0xf10020u )
    {
      mTom.interrupt( cycle );
      mIhandlerUntil = 0;
      mJerryInts.write( cycle, static_cast<std::uint16_t>( storeHalf ) );
    }
    else if ( m.q8b && !m.readsi && m.gpuCycle && sl.write && ( sl.address & 0xfffffeu ) == 0xf10020u )
    {
      mTom.interrupt( cycle );
      mIhandlerUntil = 0;
      mJerryInts.write( cycle, sl.data );
    }
    // JOYSTICK, written the same way
    if ( m.q8b && !m.readsi && dbg && ( ain & 0xfffffeu ) == 0xf14000u )
      joystickWrite( static_cast<std::uint16_t>( storeHalf ) );
    else if ( m.q8b && !m.readsi && m.gpuCycle && sl.write && ( sl.address & 0xfffffeu ) == 0xf14000u )
      joystickWrite( sl.data );
    // the cartridge's EEPROM on Jerry's chip selects, as the I/O cycle ends (q8c): an access to GPIO0
    // its clock, a write's bit 0 its data; one to GPIO1 its chip select dropped
    if ( m.q8c )
    {
      std::uint32_t const io = mAddr.bus;
      if ( ( io & 0xfff800u ) == 0xf14800u )
      {
        if ( !m.readsi )
        {
          std::uint32_t const half = dbg ? storeHalf : sl.data;
          mEeprom.data( ( half & 1u ) != 0 );
        }
        mEeprom.clock();
      }
      else if ( ( io & 0xfff000u ) == 0xf15000u )
        mEeprom.deselect();
    }

    // Tom's registers on the bus: written by the half Jerry has on the bus in Tom's internal I/O cycle
    // (q7a), read as the cycle ends (q7b)
    if ( m.q7a && !m.readsi && dbg && Tom::decodes( ain ) )
      tomWrite( cycle, ain & ~1u, static_cast<std::uint16_t>( storeHalf ) );
    if ( m.q7b && m.readsi && dbg && Tom::decodes( ain ) )
    {
      std::uint32_t const v = tomRead( cycle, ain & ~1u, "dsp", j.adPc );
      if ( !bit( jc.ws, 2 ) )
        dg.pendData = jc.ws == 1 ? ( ( ain & 1u ) ? v & 0xffu : v >> 8 ) : v;
      else if ( jc.q3 )
        dg.pendData = ( dg.pendData & 0xffff0000u ) | v;
      else
        dg.pendData = v << 16;
      j.openLow = static_cast<std::uint16_t>( v );
    }

    // a timer is read as it counts, in the last cycle of q8b (d8c, dinlatch), a half at a time
    if ( m.q8b && waitdone && m.readsi && dbg && ain - 0xf10036u < 8 )
    {
      std::uint32_t const v = mTimers.read( cycle, ( ain - 0xf10036u ) >> 1 );
      if ( !bit( jc.ws, 2 ) )
        dg.pendData = v;
      else if ( jc.q3 )
        dg.pendData = ( dg.pendData & 0xffff0000u ) | v;
      else
        dg.pendData = v << 16;
      j.openLow = static_cast<std::uint16_t>( v );
    }
  }

  // Jerry starts a bus cycle with what the gateway presents (ack latches ad, ws, reads); a load's
  // data moves now
  if ( s.jerryStart )
  {
    Control::Gateway const& dgc = c.gw[DSP];
    std::uint32_t const size = dgc.msized;
    if ( dgc.reada && ( ( dg.datAddr & 0xffffffu ) - 0xf10036u < 8 || Tom::decodes( dg.datAddr ) ) )
      dg.pendData = 0; // a timer or Tom's register: read as Tom's I/O cycle latches it, above
    else if ( dgc.reada )
    {
      dg.pendData = read( dg.datAddr, size );
      j.openLow = static_cast<std::uint16_t>( size == 0 ? read( dg.datAddr & ~1u, 1 ) : dg.pendData );
    }
    else
      j.storeAddress = dg.datAddr;
  }

  // --- the clock edge ------------------------------------------------------------------------------
  if ( s.jack )
  {
    j.ad = dg.datAddr;
    j.adPc = dg.datPc;
  }
  // the gateways: cpu_addr for a fetch (cald), dat_addr for a load or store (dald); the masks
  // (maskld); the data
  for ( std::size_t i : { DSP, GPU } )
  {
    if ( !s.gateways )
      break;
    Gateway& gw = mGw[i];
    Signals::Gateway const& f = s.gw[i];
    GatewayRequest const& request = i == DSP ? dspRequest : gpuRequest;
    if ( f.external )
    {
      if ( request.program )
        gw.cpuAddr = request.address & 0xffffff;
      else
      {
        gw.datAddr = request.address & 0xffffff;
        gw.datPc = request.pc;
      }
    }
    if ( c.gw[i].active && f.gpuAck )
    {
      gw.maskbN = ( n.gw[i].msized & 3 ) != 0;
      gw.maskwN = bit( n.gw[i].msized, 1 );
    }
    gw.loadData = loadData[i];
    gw.progData = progData[i];
  }
  // gpu_mem.v: the gateway handing a program long word (gatereq) takes the GPU's local bus in the
  // cycle, as the slave's request does (datreqa, progreqa)
  if ( n.gw[GPU].gatereq && mGpuSlave )
    mGpuSlave->slaveRequest( cycle );

  // the object processor; OBF may have been written in the cycle
  ObjectProcessor& op = mTom.op();
  if ( g.opRuns )
  {
    ObjectProcessor::Inputs& oi = g.oi;
    oi.obfw = mTom.obfWritten( cycle );
    oi.ewr = ( gback || bback ) && !s.rwIn && ack && g.mreqb; // ob.v eback: the GPU's or the blitter's
    oi.dout = static_cast<std::uint16_t>( dataBusLow() );
    if ( g.oo.interrupt )
    {
      mTom.objectInterrupt( cycle );
      mIhandlerUntil = 0;
    }
    if ( g.oo.gpuInterrupt )
      mObjectGpuIrq = cycle;
    op.clock( oi, opData );
  }

  // the data bus as the line buffers' write path takes it (dbus.v dout: its low long word moved
  // down for a justified read; lbuf.v dw: the low word, or with address bit 15 the high, in the
  // low half, big endian)
  if ( g.opRuns )
  {
    std::uint32_t const dout = dataBusLow();
    bool const down = bit( mRows.aout << 3, 15 );
    std::uint32_t const dw = ( dout & 0xffffu ) << 16 | ( down ? dout >> 16 : dout & 0xffffu );
    mTom.setDataBus( cycle, dw );
    // a span hands the line buffers' bus on where it changes, and clocks the object processor there
    if ( mRec.on && ( !mRec.dwKnown || dw != mRec.dw ) )
    {
      mRec.dw = dw;
      mRec.dwKnown = true;
      spanRecord( cycle, S_OP );
    }
  }

  // abus.v: the open rows (newrow is q1a, at the falling edge of the cycle), the address held
  if ( m.q1a )
  {
    std::uint32_t const held = mRows.aout << 3;
    if ( c.dec.bs3 )
      mRows.bank0 = ( held >> ( 8 + mCon.dwid0 + mCon.cols0 ) ) & 0x7ff;
    if ( c.dec.bs2 )
      mRows.bank1 = ( held >> ( 8 + mCon.dwid1 + mCon.cols1 ) ) & 0x7ff;
    if ( mRec.on && ( c.dec.bs3 || c.dec.bs2 ) )
      spanRecord( cycle, S_ROWS, ( c.dec.bs3 ? 1u : 0u ) | ( c.dec.bs2 ? 2u : 0u ) );
  }
  mRows.aout = ( ack ? g.ab : mRows.aout << 3 ) >> 3;
  if ( mRec.on && ack )
    spanRecord( cycle, S_AOUT, g.ainSource );

  // a cycle started: its address, the object processor's phrase's; the object processor drives the
  // header it writes back in the next cycle; the 68000's cycle is Tom's to make
  if ( ack && g.mreqb )
  {
    mAddr.op = ain & 0xfffff8u;
    mAddr.bus = ain & 0xffffffu;
    mOpDrive = obback && !s.rwIn;
    if ( n.mem.m68kCycle )
      mc.served = true;
    if ( mRec.on )
      spanRecord( cycle, S_START, g.ainSource | ( n.mem.m68kCycle ? 4u : 0u ) );
  }
  if ( s.d10 && mEmulated68k ) // the acknowledge of an interrupt: Tom gives vector 64 (dbus.v vd)
    mc.readData = 0x4040;

  // misc.v: the refresh prescaler and period counter
  bool const lrpc = g.rpZero && g.pen;
  if ( lrpc )
    mRefCount.rp = mCon.refrate;
  else if ( g.pen )
    mRefCount.rp = ( mRefCount.rp - 1 ) & 0xf;
  mRefCount.ps = ( mRefCount.ps + 1 ) & 63;

  // at rest: nothing under way, the 68000 stopped; from the second such cycle on every latch holds
  // (the object processor and the blitter first: at work they are what keeps the bus from rest)
  bool const atRest = restful && !g.blitRuns && ( !g.opRuns || ( op.idleSettled() && !obback ) ) &&
                      mBlitWrites.empty() && !s.gw[DSP].external && !s.gw[GPU].external && !dg.store && !gg.store &&
                      !sl.ioserv && cycle > mPins.holdBack && !mc.pending;
  mRestSteps = atRest ? mRestSteps + 1 : 0;
  if ( mGpuAtJerry )
  {
    Control::Gateway const& k = n.gw[GPU];
    if ( !s.gw[GPU].external && !k.active && !k.dackwt && !k.xldReady && !k.cycpend && !gg.store && !k.packwt &&
         !k.greqt2 && !k.gatereq && n.qs == 1u && !sl.ioserv &&
         ( mPendingCycle[GPU] == UINT64_MAX || mPendingCycle[GPU] <= cycle ) )
      mGpuAtJerry = false;
  }
  if ( mPendingCycle[DSP] == cycle )
    mPendingCycle[DSP] = UINT64_MAX;
  if ( mPendingCycle[GPU] == cycle )
    mPendingCycle[GPU] = UINT64_MAX;
}

} // namespace jag
