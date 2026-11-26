// jagsim: Verilator testbench of the Jaguar_MiSTer core, a drop-in replacement for JerryTest.
//
// The whole console (Tom, Jerry, 68000) is simulated. A small 68000 boot program in the OS ROM does
// what a game would do: configures MEMCON1/2 and D_END like the BIOS, writes JOYSTICK, D_FLAGS, D_PC,
// sets DSPGO and stops (STOP #$2700, so it leaves the bus alone); with --gpu the same with G_END
// (and D_END), G_FLAGS, G_PC and GPUGO. The program is written into the local RAM (or main RAM) by the
// testbench during reset, so that the boot program, and with it the state of the console at GO,
// is the same whatever the program. The trace and the state are of the processor started, and of
// the other one if the program starts it (its events and registers prefixed with its name).
// DRAM is ideal: data is ready at the CAS edge, ram_rdy is always 1; it is addressed as the MiSTer
// core's SDRAM controller addresses it. The cartridge's ROM gives its data at once (xwaitl high);
// its EEPROM's storage (cart_bram) is a RAM of 1024 words read a cycle later, erased ($FFFF).
//
// usage: jagsim [<file.o>] [options]     (same options and output as JerryTest)
//
//  --cycles N        bus cycle limit after DSPGO (default 10000000); bus cycle = 2 DSP cycles
//  --pc ADDR         start address (default: load address)
//  --flags VALUE     initial D_FLAGS (default $4000 = REGPAGE)
//  --wav FILE        wave output: L/R transmit latches sampled at every I2S frame
//  --mem ADDR:LEN    print FNV-1a hash of LEN bytes of main RAM at ADDR (repeatable)
//  --raw FILE        raw binary loaded at the local RAM instead of <file.o>
//  --gpu             run the program on the GPU (G_RAM $F03000, G_FLAGS, G_PC, GPUGO) and trace it
//  --m68k            run the program on the 68000: it goes into main RAM, the boot program jumps to
//                    it, and the run and the trace start with the end of reset
//  --cart FILE       the cartridge: FILE's bytes from $800000, mirrored to its size rounded up to a
//                    power of two; a 32 bit ROM, cart_q the long word at abus_out[22:2] (as a real
//                    cartridge, which leaves A0 and A1 alone, whatever MEMCON1's ROMWIDTH)
//  --bios FILE       the OS ROM is FILE in place of the boot program (with --m68k; <file.o> optional)
//  --compat          the MiSTer core's max_compat (decision 0022): vintbugfix, and the BIOS's
//  --no-compat       cartridge checksum check skipped (patch_checksums); on with --bios, else off
//  --ntsc            an NTSC console: the core's ntsc input set (JOYBUTS's bit 4)
//  --load FILE@ADDR  load a file into main RAM (repeatable)
//  --poke ADDR=VALUE write a big endian long into main RAM (repeatable)
//  --log FILE        trace: one line per event, prefixed with the cycle since GO
//  --log-lines N     stop writing the trace after N lines
//  --log-from N      write the trace's lines from cycle N on
//  --frames DIR      write each frame as DIR/frame_NNNN.ppm
//  --vcd FILE        waveform dump (needs a build with VCD=1)
//  --vcd-from N      the dump from DSP cycle N on (the cycle of the trace)
//
// Frames: the pixels of the video output (vga_r, vga_g, vga_b) at vid_ce, outside hblank and
// vblank, a line ending as hblank rises and a frame as vblank rises, or as VC wraps (vid.v
// nextfield) after a field without a vblank; each frame begun and completed after GO is printed in
// the state as "frame[n]: <FNV-1a of its lines> <width>x<height>".
//
// Trace lines:  <cycle> X <pc>                      instruction issued (exe), pc = program_count
//               <cycle> IRQ i2s                     rising edge of the I2S interrupt request
//               <cycle> F NCZ                       the flags changed (- for a clear flag)
//               <cycle> W<reg><bank>:<value> <port>  register file write, port A or B, bank ' = 0, " = 1
//               <cycle> L(<addr>) w<width>          external read request, width in bytes
//               <cycle> S(<addr>)=<value> w<width>  external write request
//               <cycle> dsp:<event>                 an event of the processor not started (or gpu:)
//               <cycle> A(<addr>) r|w fc<n>         the 68000 starts a bus cycle (AS falls), --m68k
//               <cycle> D=<data>                    it ends it (AS rises): the data read or written,
//                                                   -- for a byte its strobes left out (an
//                                                   interrupt's acknowledge: its vector, the low byte)

#include "Vsim_top.h"
#include "verilated.h"
#ifdef VCD
#include "verilated_vcd_c.h"
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
  constexpr uint32_t DRAM_SIZE = 2 * 1024 * 1024;
  constexpr uint32_t ROM_SIZE = 128 * 1024;
  constexpr uint32_t D_RAM = 0xf1b000;
  constexpr uint32_t D_RAM_SIZE = 0x2000;
  constexpr uint32_t G_RAM = 0xf03000;
  constexpr uint32_t G_RAM_SIZE = 0x1000;
  constexpr uint64_t BOOT_LIMIT = 20'000'000;  // sys_clk cycles to wait for DSPGO
  constexpr double DSP_CLOCK = 26593900.0;

  struct Error : std::runtime_error
  {
    using std::runtime_error::runtime_error;
  };

  uint32_t parseNumber( std::string const& s )
  {
    if ( !s.empty() && s[0] == '$' )
      return ( uint32_t )std::stoul( s.substr( 1 ), nullptr, 16 );
    return ( uint32_t )std::stoul( s, nullptr, 0 );
  }

  std::pair<std::string, std::string> split( std::string const& s, char sep )
  {
    auto pos = s.rfind( sep );
    if ( pos == std::string::npos )
      throw Error( std::string( "Expected '" ) + sep + "' in " + s );
    return { s.substr( 0, pos ), s.substr( pos + 1 ) };
  }

  std::vector<uint8_t> readFile( std::string const& path )
  {
    std::ifstream f( path, std::ios::binary );
    if ( !f )
      throw Error( "Cannot open " + path );
    return { std::istreambuf_iterator<char>( f ), {} };
  }

  uint32_t be32( uint8_t const* p )
  {
    return ( uint32_t )p[0] << 24 | ( uint32_t )p[1] << 16 | ( uint32_t )p[2] << 8 | p[3];
  }

  // 68000 boot program builder
  struct Code
  {
    std::vector<uint8_t>& rom;
    uint32_t pc;
    void w( uint16_t v ) { rom[pc++] = v >> 8; rom[pc++] = v & 0xff; }
    void l( uint32_t v ) { w( v >> 16 ); w( v & 0xffff ); }
  };

  class Wav
  {
  public:
    explicit Wav( std::string path ) : mPath{ std::move( path ) } {}
    void put( int16_t l, int16_t r ) { mData.push_back( l ); mData.push_back( r ); }
    void write( uint32_t rate ) const
    {
      std::ofstream f( mPath, std::ios::binary );
      if ( !f )
        throw Error( "Cannot open " + mPath );
      uint32_t bytes = ( uint32_t )mData.size() * 2;
      auto u32 = [&]( uint32_t v ) { f.write( ( char const* )&v, 4 ); };
      auto u16 = [&]( uint16_t v ) { f.write( ( char const* )&v, 2 ); };
      f.write( "RIFF", 4 ); u32( 36 + bytes ); f.write( "WAVEfmt ", 8 );
      u32( 16 ); u16( 1 ); u16( 2 ); u32( rate ); u32( rate * 4 ); u16( 4 ); u16( 16 );
      f.write( "data", 4 ); u32( bytes );
      f.write( ( char const* )mData.data(), bytes );
    }
  private:
    std::string mPath;
    std::vector<int16_t> mData;
  };
}

namespace
{
  // The video output collected into frames (see the header).
  struct Frames
  {
    std::vector<uint8_t> line;
    std::vector<std::vector<uint8_t>> lines;
    bool oldHblank = false;
    bool oldVblank = false;
    bool oldField = false;
    bool fieldBlanked = false; // vblank has risen since VC last wrapped
    bool started = false;      // a vblank has begun: frames are whole from it on
    std::vector<std::string> done;
    std::string dir;

    void sample( bool ce, bool hblank, bool vblank, bool field, uint8_t r, uint8_t g, uint8_t b )
    {
      if ( ce && !hblank && !vblank && line.size() < 3 * 4096 )
      {
        line.push_back( r );
        line.push_back( g );
        line.push_back( b );
      }
      if ( hblank && !oldHblank && !line.empty() )
      {
        lines.push_back( std::move( line ) );
        line.clear();
      }
      if ( vblank && !oldVblank )
      {
        if ( started && !lines.empty() )
          finish();
        started = true;
        fieldBlanked = true;
        lines.clear();
        line.clear();
      }
      if ( field && !oldField )
      {
        // a field without a vertical blank (VBB past VP) ends its frame as VC wraps
        if ( !fieldBlanked && started && !lines.empty() )
        {
          finish();
          lines.clear();
          line.clear();
        }
        fieldBlanked = false;
      }
      oldHblank = hblank;
      oldVblank = vblank;
      oldField = field;
    }

    void finish()
    {
      uint32_t hash = 0x811c9dc5;
      size_t width = 0;
      for ( auto const& l : lines )
      {
        width = std::max( width, l.size() / 3 );
        uint32_t const n = ( uint32_t )l.size() / 3;
        for ( uint8_t byte : { ( uint8_t )( n >> 8 ), ( uint8_t )n } )
        {
          hash ^= byte;
          hash *= 0x01000193;
        }
        for ( uint8_t byte : l )
        {
          hash ^= byte;
          hash *= 0x01000193;
        }
      }
      char text[64];
      snprintf( text, sizeof text, "%08x %zux%zu", hash, width, lines.size() );
      if ( !dir.empty() )
      {
        char name[32];
        snprintf( name, sizeof name, "/frame_%04zu.ppm", done.size() );
        std::ofstream f( dir + name, std::ios::binary );
        f << "P6\n" << width << ' ' << lines.size() << "\n255\n";
        for ( auto const& l : lines )
        {
          f.write( ( char const* )l.data(), ( std::streamsize )l.size() );
          for ( size_t i = l.size(); i < 3 * width; ++i )
            f.put( 0 );
        }
      }
      done.emplace_back( text );
      lines.clear();
    }
  };

  // One RISC's trace signals in the current cycle.
  struct Probe
  {
    bool go, waN, wbN, exe, mreq, read;
    uint32_t waReg, waData, wbReg, wbData, pc, addr, width, wdata, flags;
  };

  Probe probe( Vsim_top const& t, bool gpu )
  {
    if ( gpu )
      return { ( bool )t.gpu_go, ( bool )t.gpu_wa_n, ( bool )t.gpu_wb_n, ( bool )t.gpu_exe, ( bool )t.gpu_mreq,
               ( bool )t.gpu_read, t.gpu_wa_reg, t.gpu_wa_data, t.gpu_wb_reg, t.gpu_wb_data, t.gpu_pc, t.gpu_addr,
               t.gpu_width, t.gpu_wdata, t.gpu_flags };
    return { ( bool )t.dsp_go, ( bool )t.dsp_wa_n, ( bool )t.dsp_wb_n, ( bool )t.dsp_exe, ( bool )t.dsp_mreq,
             ( bool )t.dsp_read, t.dsp_wa_reg, t.dsp_wa_data, t.dsp_wb_reg, t.dsp_wb_data, t.dsp_pc, t.dsp_addr,
             t.dsp_width, t.dsp_wdata, t.dsp_flags };
  }

  // What a RISC's trace remembers from the cycle before.
  struct TraceState
  {
    bool oldMreq = false;
    uint32_t oldFlags = 0;
    bool ran = false; // its GO has been set
  };
}

int main( int argc, char** argv )
{
  try
  {
    std::string input, raw, wavOut, logOut, vcdOut, framesDir, cartPath, biosPath;
    int compat = -1; // --compat 1, --no-compat 0, else with --bios
    uint64_t logLines = UINT64_MAX;
    uint64_t logFrom = 0;
    uint64_t vcdFrom = 0;
    uint64_t dspCycles = 0; // the run's cycles, counted as the trace counts them
    int64_t maxCycles = 10'000'000;
    std::optional<uint32_t> pc;
    uint32_t flags = 0x4000;
    bool gpu = false;
    bool m68k = false;
    bool ntsc = false; // --ntsc: the core's ntsc input, JOYBUTS's bit 4
    std::vector<std::pair<uint32_t, uint32_t>> memHashes;
    std::vector<std::pair<std::string, uint32_t>> loads;
    std::vector<std::pair<uint32_t, uint32_t>> pokes;

    for ( int i = 1; i < argc; ++i )
    {
      std::string arg = argv[i];
      auto value = [&]() -> std::string
      {
        if ( ++i >= argc )
          throw Error( "Missing value for " + arg );
        return argv[i];
      };

      if ( arg == "--cycles" )
        maxCycles = std::stoll( value() );
      else if ( arg == "--pc" )
        pc = parseNumber( value() );
      else if ( arg == "--flags" )
        flags = parseNumber( value() );
      else if ( arg == "--wav" )
        wavOut = value();
      else if ( arg == "--mem" )
      {
        auto [address, size] = split( value(), ':' );
        memHashes.emplace_back( parseNumber( address ), parseNumber( size ) );
      }
      else if ( arg == "--raw" )
        raw = value();
      else if ( arg == "--load" )
      {
        auto [path, address] = split( value(), '@' );
        loads.emplace_back( path, parseNumber( address ) );
      }
      else if ( arg == "--poke" )
      {
        auto [address, v] = split( value(), '=' );
        pokes.emplace_back( parseNumber( address ), parseNumber( v ) );
      }
      else if ( arg == "--log" )
        logOut = value();
      else if ( arg == "--log-lines" )
        logLines = std::stoull( value() );
      else if ( arg == "--log-from" )
        logFrom = std::stoull( value() );
      else if ( arg == "--vcd" )
        vcdOut = value();
      else if ( arg == "--vcd-from" )
        vcdFrom = std::stoull( value() );
      else if ( arg == "--frames" )
        framesDir = value();
      else if ( arg == "--gpu" )
        gpu = true;
      else if ( arg == "--m68k" )
        m68k = true;
      else if ( arg == "--cart" )
        cartPath = value();
      else if ( arg == "--bios" )
        biosPath = value();
      else if ( arg == "--ntsc" )
        ntsc = true;
      else if ( arg == "--compat" )
        compat = 1;
      else if ( arg == "--no-compat" )
        compat = 0;
      else if ( arg.rfind( "+verilator", 0 ) == 0 )
        ;
      else if ( input.empty() && arg.rfind( "--", 0 ) != 0 )
        input = arg;
      else
        throw Error( "Unknown argument " + arg );
    }

    if ( !input.empty() && !raw.empty() )
      throw Error( "usage: jagsim <file.o> | --raw FILE [options], see sim_main.cpp" );
    if ( !biosPath.empty() && !m68k )
      throw Error( "--bios needs --m68k" );
    if ( input.empty() && raw.empty() && biosPath.empty() )
      throw Error( "usage: jagsim <file.o> | --raw FILE [options], see sim_main.cpp" );
    bool const hasProgram = !input.empty() || !raw.empty();

    // the program
    uint32_t const ramBase = gpu ? G_RAM : D_RAM;
    uint32_t const ramSize = gpu ? G_RAM_SIZE : D_RAM_SIZE;
    uint32_t const regBase = gpu || m68k ? 0xf02100 : 0xf1a100; // FLAGS; END +$C, PC +$10, CTRL +$14
    uint32_t loadAddress = ramBase;
    std::vector<uint8_t> program;
    if ( !input.empty() )
    {
      auto f = readFile( input );
      if ( f.size() < 12 || std::memcmp( f.data(), "BS94", 4 ) != 0 )
        throw Error( "File " + input + " is not a valid BS94 file" );
      loadAddress = be32( &f[4] );
      program.assign( f.begin() + 12, f.end() );
    }
    else if ( !raw.empty() )
    {
      program = readFile( raw );
    }
    program.resize( ( program.size() + 3 ) & ~3u, 0 );
    bool const local = !m68k && loadAddress >= ramBase && loadAddress + program.size() <= ramBase + ramSize;
    if ( hasProgram && ( program.empty() || ( !local && loadAddress + program.size() > DRAM_SIZE ) ) )
      throw Error( "Bad program size or load address" );

    // the cartridge, its size a power of two
    std::vector<uint8_t> cart;
    if ( !cartPath.empty() )
    {
      cart = readFile( cartPath );
      size_t size = 4;
      while ( size < cart.size() )
        size *= 2;
      if ( size > 0x800000 )
        throw Error( "Cartridge " + cartPath + " is too big" );
      cart.resize( size, 0xff );
    }

    // main RAM, byte address order (as the 68000 sees it)
    std::vector<uint8_t> dram( DRAM_SIZE, 0 );
    for ( auto const& [path, address] : loads )
    {
      auto f = readFile( path );
      if ( address + f.size() > DRAM_SIZE )
        throw Error( "File " + path + " is too big for RAM" );
      std::copy( f.begin(), f.end(), dram.begin() + address );
    }
    for ( auto [address, v] : pokes )
    {
      for ( int b = 0; b < 4; ++b )
        dram[address + b] = v >> ( 24 - 8 * b );
    }
    if ( hasProgram && !local )
      std::copy( program.begin(), program.end(), dram.begin() + loadAddress );

    // OS ROM with the boot program, or the BIOS
    std::vector<uint8_t> rom( ROM_SIZE, 0xff );
    if ( !biosPath.empty() )
    {
      auto f = readFile( biosPath );
      if ( f.size() != ROM_SIZE )
        throw Error( "BIOS " + biosPath + " is not 128 KB" );
      rom = f;
      // patch_checksums (Jaguar.sv): the BEQ that fails a cartridge's checksum, at $19C6 in the
      // model M's BIOS and $136E in the others', made a BRA
      if ( compat != 0 ) // on with a BIOS unless --no-compat
      {
        if ( rom[0x19c6] == 0x67 )
          rom[0x19c6] = 0x60;
        else if ( rom[0x136e] == 0x67 )
          rom[0x136e] = 0x60;
      }
    }
    else
    {
      Code c{ rom, 0 };
      c.l( 0x00004000 );                                        // SSP
      c.l( 0x00e00008 );                                        // PC
      c.w( 0x33fc ); c.w( 0x1861 ); c.l( 0xf00000 );            // move.w #$1861,MEMCON1
      c.w( 0x33fc ); c.w( 0x35cc ); c.l( 0xf00002 );            // move.w #$35cc,MEMCON2
      c.w( 0x23fc ); c.l( 0x00070007 ); c.l( regBase + 0xc );   // move.l #$70007,D_END (big endian, as the BIOS)
      if ( gpu )
        { c.w( 0x23fc ); c.l( 0x00070007 ); c.l( 0xf1a10c ); }  // move.l #$70007,D_END: the DSP too, as the BIOS
      if ( m68k )
        { c.w( 0x23fc ); c.l( 0x00070007 ); c.l( 0xf1a10c ); }  // move.l #$70007,D_END: the DSP too, as the BIOS
      c.w( 0x33fc ); c.w( 0x0100 ); c.l( 0xf14000 );            // move.w #$0100,JOYSTICK
      if ( m68k )
        { c.w( 0x4ef9 ); c.l( pc.value_or( loadAddress ) ); }   // jmp program
      else
      {
        c.w( 0x23fc ); c.l( flags ); c.l( regBase );            // move.l #flags,D_FLAGS
        c.w( 0x23fc ); c.l( pc.value_or( loadAddress ) ); c.l( regBase + 0x10 ); // move.l #pc,D_PC
        c.w( 0x23fc ); c.l( 1 ); c.l( regBase + 0x14 );         // move.l #DSPGO,D_CTRL
        c.w( 0x4e72 ); c.w( 0x2700 );                           // .s: stop #$2700
        c.w( 0x60fa );                                          // bra.s .s
      }
    }

    std::unique_ptr<std::ofstream> log;
    uint64_t logCount = 0;
    if ( !logOut.empty() )
    {
      log = std::make_unique<std::ofstream>( logOut, std::ios::binary );
      if ( !*log )
        throw Error( "Cannot open " + logOut );
    }
    std::unique_ptr<Wav> wav;
    if ( !wavOut.empty() )
      wav = std::make_unique<Wav>( wavOut );

    auto ctx = std::make_unique<VerilatedContext>();
    ctx->commandArgs( argc, argv );
    auto top = std::make_unique<Vsim_top>( ctx.get() );

#ifdef VCD
    std::unique_ptr<VerilatedVcdC> vcd;
    if ( !vcdOut.empty() )
    {
      ctx->traceEverOn( true );
      vcd = std::make_unique<VerilatedVcdC>();
      top->trace( vcd.get(), 99 );
      vcd->open( vcdOut.c_str() );
    }
#else
    if ( !vcdOut.empty() )
      throw Error( "--vcd needs a build with VCD=1" );
#endif

    // static inputs
    top->xresetl_in = 0;
    top->cold_reset = 0;
    top->ram_rdy = 1;
    top->cart_q = 0xffffffff;
    top->xwaitl = 1;
    top->joystick_0 = top->joystick_1 = top->joystick_2 = top->joystick_3 = top->joystick_4 = 0;
    top->analog_0 = top->analog_1 = top->analog_2 = top->analog_3 = 0x80;
    top->comlynx_rx = 1;
    top->audwaitl = 1;
    top->ntsc = ntsc;
    top->vintbugfix = compat == 1 || ( compat < 0 && !biosPath.empty() );
    top->gpu = gpu;

    // ideal DRAM, addressed as the MiSTer core's SDRAM controller (Jaguar.sv, rtl/mem/sdram_dual.sv)
    // addresses it: the row taken from Tom's multiplexed address (dram_a) as RAS falls, the column
    // as CAS falls, from dram_a for a write and from Tom's held address (dram_addrp) for a read; with
    // the boot program's MEMCON2 (64 bits, the first column width) the row is A[20:11] and the
    // column A[10:3]. The phrase is latched, or the byte enables written, at the falling CAS edge.
    bool oldCas = true;
    bool oldRas = true;
    uint32_t row = 0;
    uint64_t dramQ = 0;
    auto phraseByte = []( uint32_t lane16, bool upper ) { return ( 3 - lane16 ) * 2 + ( upper ? 0 : 1 ); };

    // returns true when an input changed and the model has to be evaluated again
    auto updateInputs = [&]() -> bool
    {
      bool cas = top->dram_cas_n;
      bool ras = top->dram_ras_n;
      if ( oldRas && !ras )
        row = top->dram_a & 0x3ff;
      oldRas = ras;
      if ( oldCas && !cas && !ras )
      {
        bool write = ( ( ~top->dram_uw_n | ~top->dram_lw_n ) & 0xf ) != 0;
        uint32_t const column = write ? top->dram_a & 0xff : top->dram_addrp & 0xff;
        uint32_t base = ( row << 11 | column << 3 ) & ( DRAM_SIZE - 1 );
        if ( write )
        {
          uint64_t d = top->dram_d;
          for ( uint32_t lane = 0; lane < 4; ++lane )
          {
            uint16_t v = d >> ( 16 * lane );
            if ( !( top->dram_uw_n >> lane & 1 ) )
              dram[base + phraseByte( lane, true )] = v >> 8;
            if ( !( top->dram_lw_n >> lane & 1 ) )
              dram[base + phraseByte( lane, false )] = v & 0xff;
          }
        }
        else
        {
          dramQ = 0;
          for ( uint32_t lane = 0; lane < 4; ++lane )
          {
            uint16_t v = dram[base + phraseByte( lane, true )] << 8 | dram[base + phraseByte( lane, false )];
            dramQ |= ( uint64_t )v << ( 16 * lane );
          }
        }
      }
      oldCas = cas;
      uint8_t oe = cas ? 0 : ( ~top->dram_oe_n & 0xf );
      uint8_t romQ = top->os_rom_ce_n ? 0xff : rom[top->abus_out & ( ROM_SIZE - 1 )];
      uint32_t const cartQ = cart.empty() ? 0xffffffffu : be32( &cart[top->abus_out & 0x7ffffcu & ( cart.size() - 1 )] );
      bool changed = top->dram_q != dramQ || top->dram_oe != oe || top->os_rom_q != romQ || top->cart_q != cartQ;
      top->dram_q = dramQ;
      top->dram_oe = oe;
      top->os_rom_q = romQ;
      top->cart_q = cartQ;
      return changed;
    };

    // the cartridge's EEPROM storage, read a cycle later
    std::vector<uint16_t> eeprom( 1024, 0xffff );
    top->cart_bram_q = 0xffff;

    uint64_t sysCycles = 0;
    Frames frames;
    frames.dir = framesDir;
    bool capturing = false; // frames are collected from GO on
    auto tick = [&]()
    {
      top->sys_clk = 0;
      top->eval();
      uint32_t const eeAddress = top->cart_bram_addr & 0x3ff;
      if ( top->cart_bram_wr )
        eeprom[eeAddress] = top->cart_bram_data;
      top->sys_clk = 1;
      top->eval();
      top->cart_bram_q = eeprom[eeAddress];
      if ( updateInputs() )
        top->eval();
      if ( capturing )
        frames.sample( top->vid_ce, top->hblank, top->vblank, top->nextfield, top->vga_r, top->vga_g, top->vga_b );
#ifdef VCD
      if ( vcd )
        if ( dspCycles >= vcdFrom )
          vcd->dump( sysCycles * 2 );
#endif
      ++sysCycles;
    };

    auto logLine = [&]( uint64_t cycle, char const* text )
    {
      if ( log && logCount < logLines && cycle >= logFrom )
      {
        *log << cycle << ' ' << text << '\n';
        ++logCount;
      }
    };

    // reset, as long whatever the program, which is written into the DSP RAM meanwhile
    for ( uint32_t i = 0; i < 64 + D_RAM_SIZE / 4; ++i )
    {
      top->ram_poke = local && 4 * i < program.size();
      if ( top->ram_poke )
      {
        top->ram_poke_addr = ( loadAddress - ramBase ) / 4 + i;
        top->ram_poke_data = be32( &program[4 * i] );
      }
      tick();
    }
    top->ram_poke = 0;
    top->xresetl_in = 1;

    // boot until DSPGO (with --m68k, the run starts here)
    while ( !m68k && !probe( *top, gpu ).go )
    {
      if ( sysCycles > BOOT_LIMIT )
        throw Error( "DSP was not started by the boot program" );
      tick();
    }

    capturing = true;

    // run; the other processor's events are traced too, prefixed with its name, when the program
    // starts it
    uint64_t const startSysCycles = sysCycles;
    uint64_t const maxDspCycles = 2 * ( uint64_t )maxCycles;
    bool finished = false;
    bool oldI2int = false;
    TraceState traces[2];
    for ( int c = 0; c < 2; ++c )
      traces[c].oldFlags = probe( *top, c == 1 ).flags;
    bool oldWs = top->i2s_ws;
    uint64_t lastFrame = 0, framePeriod = 0;
    char buf[128];
    auto traceRisc = [&]( bool chipGpu )
    {
      Probe const p = probe( *top, chipGpu );
      TraceState& t = traces[chipGpu ? 1 : 0];
      char const* prefix = !m68k && chipGpu == gpu ? "" : chipGpu ? "gpu:" : "dsp:";
      t.ran = t.ran || p.go;
      if ( p.flags != t.oldFlags )
      {
        snprintf( buf, sizeof buf, "%sF %c%c%c", prefix, p.flags & 4 ? 'N' : '-', p.flags & 2 ? 'C' : '-', p.flags & 1 ? 'Z' : '-' );
        logLine( dspCycles, buf );
        t.oldFlags = p.flags;
      }
      if ( p.exe )
      {
        snprintf( buf, sizeof buf, "%sX %06x", prefix, p.pc );
        logLine( dspCycles, buf );
      }
      if ( !p.waN )
      {
        snprintf( buf, sizeof buf, "%sW%02d%c:%08x A", prefix, p.waReg & 31, p.waReg >= 32 ? '"' : '\'', p.waData );
        logLine( dspCycles, buf );
      }
      if ( !p.wbN )
      {
        snprintf( buf, sizeof buf, "%sW%02d%c:%08x B", prefix, p.wbReg & 31, p.wbReg >= 32 ? '"' : '\'', p.wbData );
        logLine( dspCycles, buf );
      }
      if ( p.mreq && !t.oldMreq )
      {
        if ( p.read )
          snprintf( buf, sizeof buf, "%sL(%06x) w%u", prefix, p.addr, p.width );
        else
          snprintf( buf, sizeof buf, "%sS(%06x)=%08x w%u", prefix, p.addr, p.wdata, p.width );
        logLine( dspCycles, buf );
      }
      t.oldMreq = p.mreq;
    };
    // the 68000's bus cycles: AS falling, AS rising with the data under the strobes seen
    bool oldAs = true;
    uint32_t strobes = 0;
    uint32_t fc = 0;
    auto trace68k = [&]()
    {
      bool const as = top->m68k_as_n;
      if ( !as )
        strobes |= ( top->m68k_uds_n ? 0 : 2 ) | ( top->m68k_lds_n ? 0 : 1 );
      if ( oldAs && !as )
      {
        fc = top->m68k_fc;
        snprintf( buf, sizeof buf, "A(%06x) %c fc%u", ( top->m68k_addr << 1 ) & 0xffffff, top->m68k_rw ? 'r' : 'w',
                  top->m68k_fc );
        logLine( dspCycles, buf );
        strobes = ( top->m68k_uds_n ? 0 : 2 ) | ( top->m68k_lds_n ? 0 : 1 );
      }
      else if ( !oldAs && as )
      {
        uint32_t const d = top->m68k_rw ? top->m68k_din : top->m68k_dout;
        char hi[3] = "--", lo[3] = "--";
        if ( strobes & 2 && fc != 7 ) // an interrupt's vector is the low byte; the high one is the bus's
          snprintf( hi, sizeof hi, "%02x", d >> 8 );
        if ( strobes & 1 )
          snprintf( lo, sizeof lo, "%02x", d & 0xff );
        snprintf( buf, sizeof buf, "D=%s%s", hi, lo );
        logLine( dspCycles, buf );
      }
      oldAs = as;
    };
    while ( dspCycles < maxDspCycles )
    {
      tick();
      if ( wav && top->i2s_ws && !oldWs )
      {
        framePeriod = sysCycles - lastFrame;
        lastFrame = sysCycles;
        wav->put( ( int16_t )top->snd_l, ( int16_t )top->snd_r );
      }
      oldWs = top->i2s_ws;

      if ( !top->risc_clk )
        continue;
      // values sampled here are the ones written at the next sys_clk edge
      if ( top->dsp_i2int && !oldI2int )
        logLine( dspCycles, gpu || m68k ? "dsp:IRQ i2s" : "IRQ i2s" ); // the DSP's
      oldI2int = top->dsp_i2int;
      traceRisc( gpu );
      traceRisc( !gpu );
      if ( m68k )
        trace68k();
      ++dspCycles;
      if ( !m68k && !probe( *top, gpu ).go )
      {
        finished = true;
        break;
      }
    }
    // the sys_clk edge that makes the writes sampled in the last DSP cycle, so that the state holds
    // what the trace shows
    tick();

    printf( "finished: %d\n", finished ? 1 : 0 );
    printf( "cycles: %llu\n", ( unsigned long long )( dspCycles / 2 ) );
    printf( "dsp_cycles: %llu\n", ( unsigned long long )dspCycles );
    printf( "sys_cycles: %llu\n", ( unsigned long long )( sysCycles - startSysCycles ) );
    if ( log )
      printf( "log_lines: %llu\n", ( unsigned long long )logCount );
    for ( int i = 0; i < 64 && !m68k; ++i )
    {
      top->dbg_reg_addr = i;
      top->eval();
      printf( "b%d.r%d: %08x\n", i / 32, i % 32, gpu ? top->gpu_reg_data : top->dsp_reg_data );
    }
    if ( traces[gpu ? 0 : 1].ran ) // the other processor's registers, when the program started it
      for ( int i = 0; i < 64; ++i )
      {
        top->dbg_reg_addr = i;
        top->eval();
        printf( "%s:b%d.r%d: %08x\n", gpu ? "dsp" : "gpu", i / 32, i % 32, gpu ? top->dsp_reg_data : top->gpu_reg_data );
      }
    if ( m68k && traces[0].ran ) // with --m68k, the DSP's as well when it was started
      for ( int i = 0; i < 64; ++i )
      {
        top->dbg_reg_addr = i;
        top->eval();
        printf( "dsp:b%d.r%d: %08x\n", i / 32, i % 32, top->dsp_reg_data );
      }
    for ( auto [address, size] : memHashes )
    {
      uint32_t hash = 0x811c9dc5;
      for ( uint32_t i = 0; i < size; ++i )
      {
        hash ^= dram[( address + i ) & ( DRAM_SIZE - 1 )];
        hash *= 0x01000193;
      }
      printf( "mem[%x:%x]: %08x\n", address, size, hash );
    }
    for ( size_t i = 0; i < frames.done.size(); ++i )
      printf( "frame[%zu]: %s\n", i, frames.done[i].c_str() );
    if ( wav )
      wav->write( framePeriod ? ( uint32_t )( DSP_CLOCK * 4 / framePeriod + 0.5 ) : 32000 );

#ifdef VCD
    if ( vcd )
      vcd->close();
#endif
    top->final();
  }
  catch ( std::exception const& e )
  {
    fprintf( stderr, "%s\n", e.what() );
    return 2;
  }
  return 0;
}
