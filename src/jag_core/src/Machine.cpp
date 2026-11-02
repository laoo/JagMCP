#include "jag/Machine.hpp"

#include "M68000.hpp"
#include "jag/I2s.hpp"
#include "jag/MainBus.hpp"
#include "jag/ProgramFile.hpp"
#include "jag/Risc.hpp"
#include "jag/Tom.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace jag
{

namespace
{

constexpr std::uint32_t DRAM_SIZE = 2 * 1024 * 1024;
constexpr std::uint32_t ROM_SIZE = 128 * 1024;

// The video registers the BIOS (model M) sets on a PAL console before it starts a cartridge, in
// its order: BG and BORD1, then the timing: HP, HBB, HBE, HS, HVS, HEQ, HDE, HDB1, HDB2, VP, VEE,
// VBE, VDB, VDE, VBB, VEB, VS, VI.
constexpr std::array<std::pair<std::uint32_t, std::uint16_t>, 20> BIOS_VIDEO = { {
    { 0xf00058, 0x0000 }, { 0xf0002a, 0x0000 }, { 0xf0002e, 0x0352 }, { 0xf00030, 0x06af }, { 0xf00032, 0x009e },
    { 0xf00034, 0x06d5 }, { 0xf00036, 0x0259 }, { 0xf00054, 0x0313 }, { 0xf0003c, 0x06a0 }, { 0xf00038, 0x00a6 },
    { 0xf0003a, 0x00a6 }, { 0xf0003e, 0x026f }, { 0xf0004c, 0x0006 }, { 0xf00042, 0x0022 }, { 0xf00046, 0x002e },
    { 0xf00048, 0xffff }, { 0xf00040, 0x0258 }, { 0xf0004a, 0x0265 }, { 0xf00044, 0x026a }, { 0xf0004e, 0x0261 },
} };

// The object list the BIOS (model M) leaves when it starts a cartridge: a STOP object at $1000,
// which OLP points to.
constexpr std::uint32_t BIOS_OBJECT_LIST = 0x1000;

// The OS ROM of the simulation's --m68k (sim/sim_main.cpp): its vectors, then MEMCON1, MEMCON2,
// G_END and D_END as the BIOS sets them, JOYSTICK, and a jump to the program. With `biosVideo`, the
// video as the BIOS leaves it too, before the jump: a COFF program is uploaded by a loader after
// the BIOS has booted, and sets only part of the video itself (decision 0028). OLP is set to the
// BIOS's object list, which the Machine puts in RAM.
std::vector<std::uint8_t> bootRom( std::uint32_t pc, bool biosVideo )
{
  std::vector<std::uint8_t> rom( ROM_SIZE, 0xff );
  std::size_t at = 0;
  auto const w = [&]( std::uint32_t v )
  {
    rom[at++] = static_cast<std::uint8_t>( v >> 8 );
    rom[at++] = static_cast<std::uint8_t>( v );
  };
  auto const l = [&]( std::uint32_t v )
  {
    w( v >> 16 );
    w( v & 0xffff );
  };
  l( 0x00004000 ); // SSP
  l( 0x00e00008 ); // PC
  w( 0x33fc );     // move.w #$1861,MEMCON1
  w( 0x1861 );
  l( 0xf00000 );
  w( 0x33fc ); // move.w #$35cc,MEMCON2
  w( 0x35cc );
  l( 0xf00002 );
  w( 0x23fc ); // move.l #$70007,G_END
  l( 0x00070007 );
  l( 0xf0210c );
  w( 0x23fc ); // move.l #$70007,D_END
  l( 0x00070007 );
  l( 0xf1a10c );
  w( 0x33fc ); // move.w #$0100,JOYSTICK
  w( 0x0100 );
  l( 0xf14000 );
  if ( biosVideo )
  {
    for ( auto const& [address, value] : BIOS_VIDEO )
    {
      w( 0x33fc ); // move.w #value,address
      w( value );
      l( address );
    }
    // the two line buffers cleared, as the BIOS clears them: a long of each in turn
    w( 0x41f9 ); // lea $f00800,A0
    l( 0xf00800 );
    w( 0x43f9 ); // lea $f01000,A1
    l( 0xf01000 );
    w( 0x303c ); // move.w #359,D0
    w( 359 );
    w( 0x4298 ); // clr.l (A0)+
    w( 0x4299 ); // clr.l (A1)+
    w( 0x51c8 ); // dbra D0,clr.l (A0)+
    w( 0xfffa );
    w( 0x23fc ); // move.l #BIOS_OBJECT_LIST,OLP: its low word first
    w( BIOS_OBJECT_LIST & 0xffff );
    w( BIOS_OBJECT_LIST >> 16 );
    l( 0xf00020 );
  }
  w( 0x4ef9 ); // jmp program
  l( pc );
  return rom;
}

std::vector<std::uint8_t> readFile( std::string const& path )
{
  std::ifstream f( path, std::ios::binary );
  if ( !f )
    throw std::runtime_error( "cannot open " + path );
  return { std::istreambuf_iterator<char>( f ), {} };
}

// patch_checksums (the MiSTer core's Jaguar.sv): the BEQ that fails a cartridge's checksum, at
// $19C6 in the model M's BIOS and $136E in the others', made a BRA.
void skipChecksum( std::vector<std::uint8_t>& rom )
{
  if ( rom[0x19c6] == 0x67 )
    rom[0x19c6] = 0x60;
  else if ( rom[0x136e] == 0x67 )
    rom[0x136e] = 0x60;
}

} // namespace

// Main RAM in the byte order the 68000 sees it, and Jerry's joystick ports with the pads released
// (JOYBUTS reads $FF6F in the FPGA simulation). The 2 MB are DRAM bank 0, repeated through
// $3FFFFF; bank 1 ($400000 to $7FFFFF) has no DRAM, as the console and the MiSTer core have none
// (only RAS and CAS 0 are wired). Everything else outside the DSP reads as zero.
struct Machine::Ram : ExternalBus
{
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>( DRAM_SIZE );

  std::uint32_t readLong( std::uint32_t address ) override
  {
    return static_cast<std::uint32_t>( readWord( address ) ) << 16 | readWord( address + 2 );
  }

  std::uint16_t readWord( std::uint32_t address ) override
  {
    return static_cast<std::uint16_t>( readByte( address ) << 8 | readByte( address + 1 ) );
  }

  std::uint8_t readByte( std::uint32_t address ) override
  {
    if ( address < 0x400000 )
      return bytes[address & ( DRAM_SIZE - 1 )];
    if ( ( address & ~3u ) == 0xf14000 )
    {
      // JOYSTICK's low byte: the column latch and the cartridge's EEPROM's DO; the pads' rows in
      // JOYSTICK's high byte and JOYBUTS's low one
      if ( bus )
      {
        if ( address == 0xf14000 )
          return bus->joystickHigh();
        if ( address == 0xf14001 )
          return bus->joystickLow();
        if ( address == 0xf14003 )
          return bus->joystickButtons();
      }
      return JOYSTICK_PORTS[address & 3];
    }
    return 0;
  }

  std::uint64_t readPhrase( std::uint32_t address ) override
  {
    if ( address >= 0x400000 )
      return ExternalBus::readPhrase( address );
    std::uint8_t const* p = bytes.data() + ( address & ( DRAM_SIZE - 1 ) );
    std::uint64_t v = 0;
    for ( std::uint32_t b = 0; b < 8; ++b )
      v = v << 8 | p[b];
    return v;
  }

  static constexpr std::array<std::uint8_t, 4> JOYSTICK_PORTS = { 0xff, 0xff, 0xff, 0x6f };
  MainBus const* bus = nullptr;

  void writeLong( std::uint32_t address, std::uint32_t value ) override
  {
    writeWord( address, static_cast<std::uint16_t>( value >> 16 ) );
    writeWord( address + 2, static_cast<std::uint16_t>( value ) );
  }

  void writeWord( std::uint32_t address, std::uint16_t value ) override
  {
    writeByte( address, static_cast<std::uint8_t>( value >> 8 ) );
    writeByte( address + 1, static_cast<std::uint8_t>( value ) );
  }

  void writeByte( std::uint32_t address, std::uint8_t value ) override
  {
    if ( address < 0x400000 )
      bytes[address & ( DRAM_SIZE - 1 )] = value;
  }
};

// The frames as images in a directory, and the sound as a WAV file, as the simulation writes them.
struct Machine::Outputs
{
  std::string framesDir;
  int frameCount = 0;
  std::vector<std::int16_t> samples; // left and right
  std::uint64_t frameCycles = 0;     // the I2S frame's period, as last set

  static void frame( void* context, std::vector<std::vector<std::uint8_t>> const& lines )
  {
    auto& outputs = *static_cast<Outputs*>( context );
    std::size_t width = 0;
    for ( auto const& line : lines )
      width = std::max( width, line.size() / 3 );
    char name[32];
    std::snprintf( name, sizeof name, "/frame_%04d.ppm", outputs.frameCount++ );
    std::ofstream f( outputs.framesDir + name, std::ios::binary );
    f << "P6\n" << width << ' ' << lines.size() << "\n255\n";
    for ( auto const& line : lines )
    {
      f.write( reinterpret_cast<char const*>( line.data() ), static_cast<std::streamsize>( line.size() ) );
      for ( std::size_t i = line.size(); i < 3 * width; ++i )
        f.put( 0 );
    }
  }

  static void sample( void* context, std::uint16_t left, std::uint16_t right )
  {
    auto& outputs = *static_cast<Outputs*>( context );
    outputs.samples.push_back( static_cast<std::int16_t>( left ) );
    outputs.samples.push_back( static_cast<std::int16_t>( right ) );
  }

  void writeWav( std::string const& path ) const
  {
    std::ofstream f( path, std::ios::binary );
    if ( !f )
      throw std::runtime_error( "cannot open " + path );
    // the simulation measures the period between two frames, and says 32000 without them
    auto const rate = static_cast<std::uint32_t>(
        samples.size() >= 4 ? CYCLES_PER_SECOND / static_cast<double>( frameCycles ) + 0.5 : 32000 );
    auto const bytes = static_cast<std::uint32_t>( samples.size() * 2 );
    auto const u32 = [&]( std::uint32_t v )
    {
      for ( int i = 0; i < 4; ++i )
        f.put( static_cast<char>( v >> ( 8 * i ) ) );
    };
    auto const u16 = [&]( std::uint32_t v )
    {
      f.put( static_cast<char>( v ) );
      f.put( static_cast<char>( v >> 8 ) );
    };
    f.write( "RIFF", 4 );
    u32( 36 + bytes );
    f.write( "WAVEfmt ", 8 );
    u32( 16 );
    u16( 1 );
    u16( 2 );
    u32( rate );
    u32( rate * 4 );
    u16( 4 );
    u16( 16 );
    f.write( "data", 4 );
    u32( bytes );
    for ( std::int16_t const s : samples )
      u16( static_cast<std::uint16_t>( s ) );
  }
};

Machine::Machine( RunOptions const& options, TraceSink* trace )
    : mOptions( options ), mRam( std::make_unique<Ram>() ), mOutputs( std::make_unique<Outputs>() )
{
  // the program in main RAM (optional with the BIOS, and with the 68000 only)
  ProgramFile m68kProgram{};
  if ( options.m68k && !options.input.empty() )
    m68kProgram = parseProgramFile( readFile( options.input ) );
  // the BIOS's object list, before anything is loaded: the loader of a COFF program runs after the
  // BIOS, and what it loads over the list replaces it (decision 0028)
  if ( m68kProgram.coff )
    mRam->writeLong( BIOS_OBJECT_LIST + 4, 4 ); // a STOP object
  for ( auto const& [path, address] : options.loads )
  {
    auto const data = readFile( path );
    if ( address + data.size() > DRAM_SIZE )
      throw std::runtime_error( path + " does not fit in RAM" );
    std::ranges::copy( data, mRam->bytes.begin() + address );
  }
  for ( auto const& [address, v] : options.pokes )
    mRam->writeLong( address, v );

  mOutputs->framesDir = options.framesDir;
  if ( options.m68k )
  {
    // the program's sections, the OS ROM, the cartridge
    for ( auto const& [address, data] : m68kProgram.sections )
    {
      if ( address + data.size() > DRAM_SIZE )
        throw std::runtime_error( options.input + " does not fit in RAM" );
      std::ranges::copy( data, mRam->bytes.begin() + address );
    }
    std::vector<std::uint8_t> rom = bootRom( options.pc.value_or( m68kProgram.entry ), m68kProgram.coff );
    if ( !options.bios.empty() )
    {
      rom = readFile( options.bios );
      if ( rom.size() != ROM_SIZE )
        throw std::runtime_error( options.bios + " is not 128 KB" );
      if ( options.compatible() )
        skipChecksum( rom );
    }
    std::vector<std::uint8_t> cart;
    if ( !options.cart.empty() )
    {
      cart = readFile( options.cart );
      std::size_t size = 4;
      while ( size < cart.size() )
        size *= 2;
      if ( size > 0x800000 )
        throw std::runtime_error( options.cart + " is too big for a cartridge" );
      cart.resize( size, 0xff );
    }
    mBus = std::make_unique<MainBus>( *mRam, Boot::Reset, std::move( rom ), std::move( cart ) );
    noteUndrivenReads();
    mBus->setAutomaton( !options.step );
    mBus->tom().setVintFix( options.compatible() );
    mRam->bus = mBus.get();
    if ( !options.framesDir.empty() )
      mBus->tom().setFrameSink( &Outputs::frame, mOutputs.get() );
    mM68k = std::make_unique<M68000>( *mBus );
    if ( trace )
      mM68k->setTrace( trace );
    // the GPU and the DSP, not started: the slaves the 68000 reaches, the DSP's I2S interface
    mGpu = std::make_unique<Risc>( *mBus, Risc::Chip::Gpu );
    mDsp = std::make_unique<Risc>( *mBus, Risc::Chip::Dsp );
    for ( auto* r : { mGpu.get(), mDsp.get() } )
      r->setAutomaton( !options.step );
    if ( trace )
    {
      mGpu->setTrace( trace, "gpu:" );
      mDsp->setTrace( trace, "dsp:" );
    }
    mBus->setGpuSlave( mGpu.get() );
    mBus->setDspSlave( mDsp.get() );
    auto others = [this]( std::uint64_t cycle )
    {
      // the 68000 waits on the bus for the RISCs in the middle of an instruction: a RISC cannot
      // stop for a debugger there
      if ( !mDebugger )
      {
        runRiscs( cycle );
        return;
      }
      bool const freezable = std::exchange( mDebugger->freezable, false );
      runRiscs( cycle );
      mDebugger->freezable = freezable;
    };
    mM68k->setOthers( others );
    mBus->setOthers( others );
    if ( !options.wav.empty() )
      i2s().setSampleSink( &Outputs::sample, mOutputs.get() );
    return;
  }

  mBus = std::make_unique<MainBus>( *mRam, options.gpu ? Boot::Gpu : Boot::Dsp );
  noteUndrivenReads();
  mBus->setAutomaton( !options.step );
  mBus->tom().setVintFix( options.compatible() );
  if ( !options.framesDir.empty() )
    mBus->tom().setFrameSink( &Outputs::frame, mOutputs.get() );

  // the processor the program runs on, and the other, which it may start: the DSP, by the GPU
  mProgram = std::make_unique<Risc>( *mBus, options.gpu ? Risc::Chip::Gpu : Risc::Chip::Dsp );
  if ( options.gpu )
  {
    mBus->setGpuSlave( mProgram.get() ); // its own writes through Tom's window onto its RAM
    mDsp = std::make_unique<Risc>( *mBus, Risc::Chip::Dsp );
    mDsp->setAutomaton( !options.step );
    if ( trace )
      mDsp->setTrace( trace, "dsp:" );
    mBus->setDspSlave( mDsp.get() );
  }
  else
    mBus->setDspSlave( mProgram.get() );
  if ( !options.wav.empty() )
    i2s().setSampleSink( &Outputs::sample, mOutputs.get() );

  ProgramFile program;
  if ( options.raw.empty() )
  {
    program = parseProgramFile( readFile( options.input ) );
  }
  else
  {
    program.entry = mProgram->ramBase();
    program.sections.push_back( { .address = program.entry, .data = readFile( options.raw ) } );
  }
  mProgram->setAutomaton( !options.step );
  if ( trace )
    mProgram->setTrace( trace );
  // a section outside the local RAM goes into main RAM, as the simulation loads it
  for ( auto const& [address, data] : program.sections )
  {
    bool const local =
        address >= mProgram->ramBase() && address + data.size() <= mProgram->ramBase() + mProgram->ramSize();
    if ( local )
    {
      mProgram->loadLocal( address, data );
    }
    else
    {
      if ( address + data.size() > DRAM_SIZE )
        throw std::runtime_error( options.input + " does not fit in RAM" );
      std::ranges::copy( data, mRam->bytes.begin() + address );
    }
  }
  mProgram->setFlags( options.flags );
  mProgram->setPc( options.pc.value_or( program.entry ) );
  mProgram->go();
}

void Machine::setLimit( std::uint64_t cycle )
{
  mLimit = std::min( mLimit, cycle );
  if ( mM68k )
  {
    mM68k->setLimit( mLimit );
    mBus->setEnd( mLimit );
    mGpu->setLimit( mLimit );
    mDsp->setLimit( mLimit );
    return;
  }
  mProgram->setLimit( cycle );
  if ( mDsp )
    mDsp->setLimit( cycle );
}

bool Machine::runTo( std::uint64_t cycle )
{
  if ( mM68k )
  {
    if ( !mPoweredOn )
    {
      mPoweredOn = true;
      mM68k->powerOn();
    }
    std::uint64_t const to = std::min( cycle, mLimit );
    mM68k->runUntil( to );
    // the RISCs no further than the 68000, which a debugger's stop may have ended short
    runRiscs( std::min( to, mM68k->reached() ) );
    return mM68k->reached() < mLimit;
  }
  if ( !mDsp )
  {
    mRuns = mRuns && mProgram->runUntil( cycle );
    return mRuns;
  }

  // The two by turns, the one behind first, the GPU first in a cycle both are at: a cycle of the
  // DSP may need the bus stepped through it, which needs the GPU's request of it. The GPU runs
  // until it waits for the DSP or reaches `cycle`; the DSP up to the GPU, or to `cycle` once the
  // GPU has stopped.
  for ( ;; )
  {
    if ( mDebugger && mDebugger->stop )
      break;
    mDspRuns = mDspRuns && ( !mDsp->started() || mDsp->now() < mDsp->cycles() );
    bool const dspActive = mDspRuns && mDsp->started();
    std::uint64_t const gpuAt = mProgram->now();
    if ( mRuns && gpuAt < cycle && ( !dspActive || gpuAt <= mDsp->now() ) )
    {
      mRuns = mProgram->runUntil( cycle );
      mDsp->setLimit( mProgram->cycles() ); // the run ends where the GPU stops
      if ( !mRuns && !mDsp->started() )
        mDspRuns = false;
    }
    else if ( std::uint64_t const to = mRuns ? std::min( mProgram->waitsFor(), cycle ) : cycle;
              dspActive && mDsp->now() < to )
    {
      std::uint64_t const before = mDsp->now();
      mDspRuns = mDsp->runUntil( to );
      if ( mDspRuns && mDsp->now() == before )
      {
        // each waits for the other: the write the slave port serves is taken late
        mProgram->serveLate();
        mDsp->serveLate();
      }
    }
    else
      break;
  }
  return mRuns || mDspRuns;
}

// The GPU first in a cycle both are at, as with a GPU program; one that cannot get on (it waits for
// the 68000) leaves the turn.
void Machine::runRiscs( std::uint64_t cycle )
{
  for ( ;; )
  {
    if ( mDebugger && mDebugger->stop && mDebugger->freezable )
      return;
    // a RISC stopped may be started again by the 68000
    mGpuRuns = !mGpu->started() || mGpu->now() < mGpu->cycles();
    mDspRuns = !mDsp->started() || mDsp->now() < mDsp->cycles();
    bool const gpuActive = mGpuRuns && mGpu->started() && mGpu->now() < cycle;
    bool const dspActive = mDspRuns && mDsp->started() && mDsp->now() < cycle;
    Risc* r = nullptr;
    if ( gpuActive && ( !dspActive || mGpu->now() <= mDsp->now() ) )
      r = mGpu.get();
    else if ( dspActive )
      r = mDsp.get();
    if ( !r )
      return;
    std::uint64_t const before = r->now();
    bool const runs = r->runUntil( cycle );
    ( r == mGpu.get() ? mGpuRuns : mDspRuns ) = runs;
    if ( runs && r->now() == before )
    {
      // it waits: for the other RISC if it is behind (or not past the cycle its slave port serves
      // a write in), else for the 68000
      Risc* other = r == mGpu.get() ? mDsp.get() : mGpu.get();
      bool const otherRuns = r == mGpu.get() ? mDspRuns : mGpuRuns;
      std::uint64_t const target = r->waitsFor();
      if ( !otherRuns || !other->started() || other->now() >= target )
        return;
      std::uint64_t const otherBefore = other->now();
      bool const oruns = other->runUntil( target );
      ( other == mGpu.get() ? mGpuRuns : mDspRuns ) = oruns;
      if ( oruns && other->now() == otherBefore )
      {
        if ( target == before )
          return;
        r->serveLate(); // the other waits for it too
      }
    }
  }
}

std::uint64_t Machine::now() const
{
  return mM68k ? mM68k->reached() : mProgram->now();
}

void Machine::finish()
{
  if ( mM68k )
  {
    // the run ends at the limit; a bus cycle under way then is left as it is
    runRiscs( mLimit );
    mBus->runOut( mLimit );
    mBus->tom().renderTo( mLimit );
    mGpu->finish();
    mDsp->finish();
    if ( !mOptions.wav.empty() )
    {
      mOutputs->frameCycles = i2s().frameCycles();
      mOutputs->writeWav( mOptions.wav );
    }
    return;
  }
  std::uint64_t const end = mProgram->cycles();
  mBus->runOut( end );
  mBus->tom().renderTo( end );
  mProgram->finish();
  if ( mDsp )
    mDsp->finish();
  if ( !mOptions.wav.empty() )
  {
    mOutputs->frameCycles = i2s().frameCycles();
    mOutputs->writeWav( mOptions.wav );
  }
}

RunResult Machine::result() const
{
  RunResult result;
  if ( mM68k )
  {
    result.m68k = true;
    result.dspCycles = mLimit;
    if ( mGpu->started() )
    {
      auto& [name, regs] = result.others.emplace_back( "gpu", std::array<std::uint32_t, 64>{} );
      for ( int i = 0; i < 64; ++i )
        regs[static_cast<std::size_t>( i )] = mGpu->reg( i );
    }
  }
  else
  {
    result.finished = mProgram->stopped();
    result.dspCycles = mProgram->cycles();
    for ( int i = 0; i < 64; ++i )
      result.regs[static_cast<std::size_t>( i )] = mProgram->reg( i );
  }
  if ( mDsp && mDsp->started() )
  {
    auto& [name, regs] = result.others.emplace_back( "dsp", std::array<std::uint32_t, 64>{} );
    for ( int i = 0; i < 64; ++i )
      regs[static_cast<std::size_t>( i )] = mDsp->reg( i );
  }
  for ( auto const& [address, size] : mOptions.memHashes )
  {
    std::uint32_t hash = 0x811c9dc5;
    for ( std::uint32_t i = 0; i < size; ++i )
    {
      hash ^= mRam->bytes[( address + i ) & ( DRAM_SIZE - 1 )];
      hash *= 0x01000193;
    }
    char key[32];
    std::snprintf( key, sizeof key, "mem[%x:%x]", address, size );
    result.memHashes.emplace_back( key, hash );
  }
  result.frames = mBus->tom().frames();
  result.undriven = mUndriven;
  return result;
}

std::optional<std::uint8_t> Machine::peek( std::uint32_t address ) const
{
  address &= 0xffffffu;
  if ( address < 0x400000u && !mBus->peekBoard( address ) )
    return mRam->bytes[address & ( DRAM_SIZE - 1 )];
  if ( auto const rom = mBus->peekBoard( address ) )
    return rom;
  for ( Risc const* r : { mGpu.get(), mDsp.get(), mProgram.get() } )
    if ( r && r->holdsLocal( address ) )
      return r->localByte( address );
  return std::nullopt;
}

bool Machine::writable( std::uint32_t address ) const
{
  address &= 0xffffffu;
  if ( address < 0x400000u && !mBus->peekBoard( address ) )
    return true;
  for ( Risc const* r : { mGpu.get(), mDsp.get(), mProgram.get() } )
    if ( r && r->holdsLocal( address ) )
      return true;
  return false;
}

bool Machine::poke( std::uint32_t address, std::uint8_t value )
{
  address &= 0xffffffu;
  if ( address < 0x400000u && !mBus->peekBoard( address ) )
  {
    mRam->bytes[address & ( DRAM_SIZE - 1 )] = value;
    return true;
  }
  for ( Risc* r : { mGpu.get(), mDsp.get(), mProgram.get() } )
    if ( r && r->holdsLocal( address ) )
    {
      r->setLocalByte( address, value );
      return true;
    }
  return false;
}

std::optional<BusWait> Machine::busWait() const
{
  if ( !mM68k )
    return std::nullopt;
  return mM68k->waiting();
}

void Machine::noteUndrivenReads()
{
  mBus->setUndrivenRead(
      [this]( char const* cpu, std::uint32_t pc, std::uint32_t address, std::uint16_t value, std::uint64_t cycle )
      {
        // the 68000 is in the middle of the instruction that makes the read
        if ( mM68k && std::string_view{ cpu } == "m68k" )
          pc = mM68k->getPC0();
        for ( UndrivenRead& r : mUndriven )
          if ( r.pc == pc && r.address == address && r.cpu == cpu )
          {
            ++r.count;
            return;
          }
        if ( mUndriven.size() < UNDRIVEN_READS )
          mUndriven.push_back( UndrivenRead{ cpu, pc, address, value, cycle, 1 } );
      } );
}

Debugger& Machine::debugger()
{
  if ( !mDebugger )
  {
    mDebugger = std::make_unique<Debugger>();
    if ( mM68k )
      mM68k->setDebug( mDebugger.get(), &mDebugger->m68k );
    if ( Risc* r = gpu() )
      r->setDebug( mDebugger.get(), &mDebugger->gpu );
    if ( Risc* r = dsp() )
      r->setDebug( mDebugger.get(), &mDebugger->dsp );
  }
  return *mDebugger;
}

void Machine::step( Probe& probe )
{
  Debugger const& debugger = this->debugger();
  probe.step = true;
  probe.stepPasses = false;
  if ( &probe == &debugger.m68k )
  {
    // at an instruction boundary, or in the middle of the instruction a bus cycle waits in
    probe.passAt = mM68k->now();
    return;
  }
  // stopped before an instruction by a probe, it issues in this cycle; else the RISC issues it
  // later, or is past its issue: the next to issue is the instruction the step executes
  if ( std::uint64_t const now = ( &probe == &debugger.gpu ? gpu() : dsp() )->now(); probe.passAt != now )
    probe.stepPasses = true;
}

void Machine::setPad( std::size_t port, std::uint32_t buttons )
{
  mBus->setPad( port, buttons );
}

std::uint32_t Machine::pad( std::size_t port ) const
{
  return mBus->pad( port );
}

Risc* Machine::gpu()
{
  if ( mGpu )
    return mGpu.get();
  return mOptions.gpu ? mProgram.get() : nullptr;
}

Risc* Machine::dsp()
{
  if ( mDsp )
    return mDsp.get();
  return mOptions.gpu || mOptions.m68k ? nullptr : mProgram.get();
}

std::optional<Machine::M68kState> Machine::m68kState() const
{
  if ( !mM68k )
    return std::nullopt;
  M68kState s;
  for ( int n = 0; n < 8; ++n )
  {
    s.d[static_cast<std::size_t>( n )] = mM68k->getD( n );
    s.a[static_cast<std::size_t>( n )] = mM68k->getA( n );
  }
  s.pc = mM68k->getPC();
  s.sr = mM68k->getSR();
  s.usp = mM68k->getUSP();
  s.isp = mM68k->getISP();
  s.stopped = mM68k->stopped();
  s.halted = mM68k->isHalted();
  return s;
}

bool Machine::setM68kRegisters( M68kRegisters const& registers )
{
  if ( !mM68k || mM68k->waiting() )
    return false;
  if ( registers.sr )
    mM68k->setSR( static_cast<std::uint16_t>( *registers.sr ) );
  if ( registers.usp )
    mM68k->setUSP( *registers.usp );
  if ( registers.isp )
    mM68k->setISP( *registers.isp );
  for ( int n = 0; n < 8; ++n )
  {
    if ( auto const d = registers.d[static_cast<std::size_t>( n )] )
      mM68k->setD( n, *d );
    if ( auto const a = registers.a[static_cast<std::size_t>( n )] )
      mM68k->setA( n, *a );
  }
  if ( registers.pc )
    mM68k->setNextPc( *registers.pc );
  return true;
}

std::optional<std::pair<std::string, std::uint32_t>> Machine::disassemble68k( std::uint32_t address ) const
{
  if ( !mM68k )
    return std::nullopt;
  char text[128];
  int const length = mM68k->disassemble( text, address );
  return std::pair<std::string, std::uint32_t>{ text, static_cast<std::uint32_t>( length ) };
}

Tom& Machine::tom()
{
  return mBus->tom();
}

I2s& Machine::i2s()
{
  return ( mDsp ? mDsp : mProgram )->i2s();
}

Machine::~Machine() = default;

} // namespace jag
