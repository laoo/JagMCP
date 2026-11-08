#include "jag/Runner.hpp"

#include "jag/Machine.hpp"
#include "jag/ProgramFile.hpp"
#include "jag/Risc.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace jag
{

namespace
{

std::uint32_t parseNumber( std::string const& s )
{
  if ( !s.empty() && s[0] == '$' )
    return static_cast<std::uint32_t>( std::stoul( s.substr( 1 ), nullptr, 16 ) );
  return static_cast<std::uint32_t>( std::stoul( s, nullptr, 0 ) );
}

std::pair<std::string, std::string> split( std::string const& s, char sep )
{
  auto const pos = s.rfind( sep );
  if ( pos == std::string::npos )
    throw std::runtime_error( "expected '" + std::string( 1, sep ) + "' in " + s );
  return { s.substr( 0, pos ), s.substr( pos + 1 ) };
}

// The first bytes of `path`, as many as tell a program file's format; fewer when it is shorter or
// cannot be read.
std::vector<std::uint8_t> fileStart( std::string const& path )
{
  std::ifstream f( path, std::ios::binary );
  std::vector<std::uint8_t> start( 20 );
  f.read( reinterpret_cast<char*>( start.data() ), static_cast<std::streamsize>( start.size() ) );
  start.resize( static_cast<std::size_t>( f.gcount() ) );
  return start;
}

class Collector : public TraceSink
{
public:
  std::vector<TraceEvent> events;
  std::uint64_t from = 0;

  void event( std::uint64_t cycle, std::string_view text ) override
  {
    if ( cycle >= from )
      events.push_back( { cycle, std::string( text ) } );
  }
};

} // namespace

std::string_view runOptionsHelp()
{
  return "  --cycles N          Run for N bus cycles. Default: 10000000.\n"
         "  --pc ADDR           Start at ADDR instead of the program's start address.\n"
         "  --flags VALUE       Set the initial flags register. Default: 0x4000.\n"
         "  --raw FILE          Load a raw DSP binary into D_RAM instead of PROGRAM.\n"
         "  --load FILE@ADDR    Load FILE into memory at ADDR. Can be repeated.\n"
         "  --poke ADDR=VALUE   Write VALUE at ADDR before the run. Can be repeated.\n"
         "  --mem ADDR:LENGTH   Print a hash of LENGTH bytes at ADDR. Can be repeated.\n"
         "  --gpu               Run the program on the GPU.\n"
         "  --m68k              Run the program on the 68000, from reset. A COFF program\n"
         "                      runs there without this flag, unless --gpu is given.\n"
         "  --bios FILE         Use FILE as the boot ROM. Needs --m68k.\n"
         "  --cart FILE         Load a cartridge image at $800000.\n"
         "  --compat            Enable the MiSTer core's compatibility fixes. On with --bios.\n"
         "  --no-compat         Disable them.\n"
         "  --open-bus          Let a DSP store losing the bus take the open bus, as the\n"
         "                      netlist does. On without --bios.\n"
         "  --no-open-bus       Let it keep its data, as the console does.\n"
         "  --step              Step every cycle, without the fast timing. Slow; for checking.\n"
         "  --part N            Run in parts of N DSP cycles, as the window does.\n"
         "  --log FILE          Write a trace of every event to FILE.\n"
         "  --log-lines N       Write at most N lines of the trace.\n"
         "  --log-from N        Write the trace from DSP cycle N on.\n"
         "  --frames DIR        Write each video frame to DIR as frame_NNNN.ppm.\n"
         "  --wav FILE          Write the sound (I2S output) to FILE.\n";
}

RunOptions parseRunOptions( std::span<std::string const> args )
{
  RunOptions o;
  for ( std::size_t i = 0; i < args.size(); ++i )
  {
    std::string const& arg = args[i];
    auto const value = [&]() -> std::string const&
    {
      if ( ++i >= args.size() )
        throw std::runtime_error( "missing value for " + arg );
      return args[i];
    };
    if ( arg == "--cycles" )
      o.cycles = std::stoull( value() );
    else if ( arg == "--pc" )
      o.pc = parseNumber( value() );
    else if ( arg == "--flags" )
      o.flags = parseNumber( value() );
    else if ( arg == "--mem" )
    {
      auto const [address, size] = split( value(), ':' );
      o.memHashes.emplace_back( parseNumber( address ), parseNumber( size ) );
    }
    else if ( arg == "--raw" )
      o.raw = value();
    else if ( arg == "--load" )
    {
      auto const [path, address] = split( value(), '@' );
      o.loads.emplace_back( path, parseNumber( address ) );
    }
    else if ( arg == "--poke" )
    {
      auto const [address, v] = split( value(), '=' );
      o.pokes.emplace_back( parseNumber( address ), parseNumber( v ) );
    }
    else if ( arg == "--log" )
      o.log = value();
    else if ( arg == "--log-lines" )
      o.logLines = std::stoull( value() );
    else if ( arg == "--log-from" )
      o.logFrom = std::stoull( value() );
    else if ( arg == "--step" )
      o.step = true;
    else if ( arg == "--gpu" )
      o.gpu = true;
    else if ( arg == "--m68k" )
      o.m68k = true;
    else if ( arg == "--cart" )
      o.cart = value();
    else if ( arg == "--bios" )
      o.bios = value();
    else if ( arg == "--compat" )
      o.compat = true;
    else if ( arg == "--no-compat" )
      o.compat = false;
    else if ( arg == "--open-bus" )
      o.openBus = true;
    else if ( arg == "--no-open-bus" )
      o.openBus = false;
    else if ( arg == "--frames" )
      o.framesDir = value();
    else if ( arg == "--wav" )
      o.wav = value();
    else if ( arg == "--part" )
      o.part = std::stoull( value() );
    else if ( o.input.empty() && !arg.starts_with( "--" ) )
      o.input = arg;
    else
      throw std::runtime_error( "unknown argument " + arg );
  }
  // Atari's linkers make 68000 programs: a COFF one runs on the 68000 unless it is said otherwise
  bool const coff = !o.input.empty() && isCoff( fileStart( o.input ) );
  if ( coff && !o.gpu )
    o.m68k = true;
  if ( coff && !o.bios.empty() )
    throw std::runtime_error( "--bios with a COFF program: the BIOS does not start it; leave --bios out" );
  if ( !o.bios.empty() && !o.m68k )
    throw std::runtime_error( "--bios needs --m68k" );
  if ( ( !o.input.empty() && !o.raw.empty() ) || ( o.input.empty() && o.raw.empty() && o.bios.empty() ) )
    throw std::runtime_error( "a program is needed: <file.o> or --raw FILE" );
  return o;
}

RunOptions readRunFile( std::string const& path )
{
  std::ifstream f( path );
  if ( !f )
    throw std::runtime_error( "cannot open " + path );
  std::filesystem::path const dir = std::filesystem::path( path ).parent_path();
  std::string program;
  std::vector<std::string> args;
  for ( std::string line; std::getline( f, line ); )
  {
    // "; PROGRAM: file" and "; RUN: arguments"
    std::istringstream in( line );
    std::string semicolon, directive;
    if ( !( in >> semicolon >> directive ) || semicolon != ";" )
      continue;
    if ( directive == "PROGRAM:" )
      in >> program;
    else if ( directive == "RUN:" )
      for ( std::string word; in >> word; )
      {
        if ( !args.empty() && ( args.back() == "--load" || args.back() == "--cart" || args.back() == "--bios" ) )
          word = ( dir / word ).generic_string();
        args.push_back( word );
      }
  }
  if ( program.empty() )
    throw std::runtime_error( path + " names no PROGRAM" );
  std::string const file = ( dir / program ).generic_string();
  std::vector<std::uint8_t> const start = fileStart( file );
  bool const bs94 = start.size() >= 4 && std::string_view( reinterpret_cast<char const*>( start.data() ), 4 ) == "BS94";
  args.insert( args.begin(), file );
  if ( !bs94 && !isCoff( start ) )
    args.insert( args.begin(), "--raw" );
  return parseRunOptions( args );
}

RunResult runProgram( RunOptions const& options, bool keepTrace )
{
  Collector collector;
  collector.from = options.logFrom;
  bool const trace = keepTrace || !options.log.empty();
  Machine machine( options, trace ? &collector : nullptr );
  std::uint64_t const limit = 2 * options.cycles;
  machine.setLimit( limit );
  if ( options.part == 0 )
    machine.runTo( limit );
  else
    for ( std::uint64_t to = options.part; machine.runTo( std::min( to, limit ) ); to += options.part )
      ;
  machine.finish();

  RunResult result = machine.result();
  result.trace = std::move( collector.events );
  std::ranges::stable_sort( result.trace, {}, &TraceEvent::cycle );
  if ( !options.log.empty() )
  {
    std::ofstream log( options.log );
    std::uint64_t lines = 0;
    for ( auto const& e : result.trace )
    {
      if ( lines++ >= options.logLines )
        break;
      log << e.cycle << ' ' << e.text << '\n';
    }
  }
  return result;
}

std::string formatState( RunResult const& result )
{
  std::string out;
  char line[64];
  std::snprintf( line, sizeof line, "finished: %d\n", result.finished ? 1 : 0 );
  out += line;
  std::snprintf( line, sizeof line, "cycles: %llu\n", static_cast<unsigned long long>( result.dspCycles / 2 ) );
  out += line;
  std::snprintf( line, sizeof line, "dsp_cycles: %llu\n", static_cast<unsigned long long>( result.dspCycles ) );
  out += line;
  for ( int i = 0; i < 64 && !result.m68k; ++i )
  {
    std::snprintf( line, sizeof line, "b%d.r%d: %08x\n", i / 32, i % 32, result.regs[static_cast<std::size_t>( i )] );
    out += line;
  }
  for ( auto const& [name, regs] : result.others )
    for ( int i = 0; i < 64; ++i )
    {
      std::snprintf(
          line, sizeof line, "%s:b%d.r%d: %08x\n", name.c_str(), i / 32, i % 32, regs[static_cast<std::size_t>( i )] );
      out += line;
    }
  for ( auto const& [key, hash] : result.memHashes )
  {
    std::snprintf( line, sizeof line, "%s: %08x\n", key.c_str(), hash );
    out += line;
  }
  for ( std::size_t i = 0; i < result.frames.size(); ++i )
  {
    std::snprintf( line, sizeof line, "frame[%zu]: %s\n", i, result.frames[i].c_str() );
    out += line;
  }
  return out;
}

} // namespace jag
