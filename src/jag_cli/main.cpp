// jagmcp-cli: runs a DSP or GPU program like the FPGA simulation does (sim/sim_main.cpp), or checks
// the core against the simulation's recorded traces (tests/reference).
//
//   jagmcp-cli <file.o> [--cycles N] [--pc A] [--flags V] [--mem A:L] [--raw F] [--load F@A]
//              [--poke A=V] [--log FILE] [--log-lines N] [--log-from N] [--gpu] [--m68k] [--cart F]
//              [--bios F] [--compat|--no-compat] [--open-bus|--no-open-bus] [--step] [--frames DIR]
//              [--wav FILE] [--part N]
//   jagmcp-cli --check <dir>/<name> [options]  compare with <name>.trace and <name>.state; 0 when equal
//   jagmcp-cli --check-all <dir> [options]     one line per reference, and a count
//   jagmcp-cli --server [tcp:PORT] [program]   the control protocol (docs/spec/control-protocol.md)
//                                              on stdin and stdout, or on TCP at 127.0.0.1:PORT
//   jagmcp-cli --mcp [program]                 the Model Context Protocol on stdio, for an agent
//   jagmcp-cli --batch FILE [--mcp] [--expect FILE] [program]
//                                              FILE's requests answered on stdout, or compared
//                                              with --expect's answers; 0 when equal
//
// --part N runs in parts of N DSP cycles, as the window does; --step without the automatons, the
// RISCs' stepped cores alone and the bus's control worked out every cycle. A served program, if
// given, is loaded as emu.load loads one: the program and its options, or a .run file.

#include "jag/Runner.hpp"
#include "jag/Tom.hpp"
#include "jag/control/Dispatcher.hpp"
#include "jag/control/Session.hpp"
#include "jag/server/JsonLinesServer.hpp"
#include "jag/server/McpServer.hpp"
#include "jag/server/TcpLineServer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

std::string readText( std::filesystem::path const& path )
{
  std::ifstream f( path );
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::vector<std::string> splitWords( std::string const& text )
{
  std::istringstream in( text );
  std::vector<std::string> words;
  for ( std::string w; in >> w; )
    words.push_back( w );
  return words;
}

// The events both traces have: issues (X), register writes (W, without the port) and flags (F),
// either RISC's, and the 68000's bus cycles (A, D), sorted by cycle and, within a cycle, by text.
std::vector<jag::TraceEvent> comparable( std::vector<jag::TraceEvent> events )
{
  std::vector<jag::TraceEvent> out;
  bool pipelineRead = false; // the 68000's read of a RISC's CTRL or FLAGS, its high half
  for ( auto& e : events )
  {
    // the other processor's are prefixed with its name
    std::size_t const at = e.text.starts_with( "dsp:" ) || e.text.starts_with( "gpu:" ) ? 4 : 0;
    char const kind = e.text.size() > at ? e.text[at] : '\0';
    if ( kind != 'X' && kind != 'W' && kind != 'F' && kind != 'A' && kind != 'D' )
      continue;
    if ( kind == 'W' && e.text.size() > at + 13 )
      e.text.resize( at + 13 );
    // the high half of a RISC's CTRL or FLAGS is its pipeline's state where the read meets it, which
    // the core does not have (hardware/m68000.md): the data read is left out
    if ( at == 0 && kind == 'A' )
      pipelineRead = e.text.starts_with( "A(f02100) r" ) || e.text.starts_with( "A(f02114) r" ) ||
                     e.text.starts_with( "A(f1a100) r" ) || e.text.starts_with( "A(f1a114) r" );
    else if ( at == 0 && kind == 'D' && pipelineRead )
    {
      e.text = "D=????";
      pipelineRead = false;
    }
    out.push_back( std::move( e ) );
  }
  std::ranges::sort(
      out, []( auto const& a, auto const& b ) { return a.cycle != b.cycle ? a.cycle < b.cycle : a.text < b.text; } );
  return out;
}

std::vector<jag::TraceEvent> parseTrace( std::string const& text )
{
  std::vector<jag::TraceEvent> events;
  std::istringstream in( text );
  for ( std::string line; std::getline( in, line ); )
  {
    auto const space = line.find( ' ' );
    if ( space == std::string::npos )
      continue;
    events.push_back( { std::stoull( line.substr( 0, space ) ), line.substr( space + 1 ) } );
  }
  return events;
}

std::map<std::string, std::string> parseState( std::string const& text )
{
  std::map<std::string, std::string> state;
  std::istringstream in( text );
  for ( std::string line; std::getline( in, line ); )
  {
    auto const sep = line.rfind( ": " );
    if ( sep != std::string::npos )
      state[line.substr( 0, sep )] = line.substr( sep + 2 );
  }
  return state;
}

struct CheckResult
{
  bool equal = true;
  std::string summary;
  std::string detail;
};

CheckResult check( std::filesystem::path const& base, std::span<std::string const> extra )
{
  // files loaded are named relative to the reference's directory; a run of the BIOS may have no
  // program
  std::vector<std::string> args;
  if ( std::filesystem::exists( base.string() + ".o" ) )
    args.push_back( base.string() + ".o" );
  for ( auto& w : splitWords( readText( base.string() + ".args" ) ) )
  {
    bool const path =
        !args.empty() && ( args.back() == "--load" || args.back() == "--cart" || args.back() == "--bios" );
    if ( path && std::filesystem::path( w ).is_relative() )
      w = ( base.parent_path() / w ).string();
    args.push_back( w );
  }
  args.insert( args.end(), extra.begin(), extra.end() );
  jag::RunOptions const options = jag::parseRunOptions( args );
  jag::RunResult const result = jag::runProgram( options, true );

  CheckResult r;
  auto const expected = parseState( readText( base.string() + ".state" ) );
  auto const actual = parseState( jag::formatState( result ) );
  std::string stateDiff;
  for ( auto const& [key, value] : expected )
  {
    if ( key == "cycles" || key == "sys_cycles" || key == "log_lines" )
      continue;
    auto const it = actual.find( key );
    std::string const got = it == actual.end() ? "-" : it->second;
    if ( got != value )
      stateDiff += "  " + key + ": FPGA " + value + ", core " + got + "\n";
  }

  auto const want = comparable( parseTrace( readText( base.string() + ".trace" ) ) );
  auto const have = comparable( result.trace );
  std::size_t first = 0;
  while ( first < want.size() && first < have.size() && want[first].cycle == have[first].cycle &&
          want[first].text == have[first].text )
    ++first;
  bool const traceEqual = first == want.size() && first == have.size();

  r.equal = traceEqual && stateDiff.empty();
  if ( r.equal )
  {
    r.summary = "equal, " + std::to_string( want.size() ) + " events";
    return r;
  }
  if ( !traceEqual )
  {
    std::uint64_t const at = first < want.size() ? want[first].cycle : have[first].cycle;
    r.summary = "trace differs at cycle " + std::to_string( at ) + " (event " + std::to_string( first ) + " of " +
                std::to_string( want.size() ) + ")";
    r.detail += "  FPGA                          core\n";
    std::size_t const lo = first >= 6 ? first - 6 : 0;
    for ( std::size_t i = lo; i < first + 8; ++i )
    {
      char line[96];
      auto const cell = [&]( std::vector<jag::TraceEvent> const& v ) -> std::string
      {
        if ( i >= v.size() )
          return "";
        return std::to_string( v[i].cycle ) + " " + v[i].text;
      };
      std::snprintf(
          line, sizeof line, "%c %-30s %s\n", i == first ? '>' : ' ', cell( want ).c_str(), cell( have ).c_str() );
      r.detail += line;
    }
  }
  else
    r.summary = "state differs";
  if ( !stateDiff.empty() )
    r.detail += "state:\n" + stateDiff;
  return r;
}

int checkAll( std::filesystem::path const& dir, std::span<std::string const> extra )
{
  std::vector<std::filesystem::path> bases;
  for ( auto const& entry : std::filesystem::directory_iterator( dir ) )
    if ( entry.path().extension() == ".args" )
      bases.push_back( entry.path().parent_path() / entry.path().stem() );
  std::ranges::sort( bases );
  int equal = 0;
  for ( auto const& base : bases )
  {
    CheckResult const r = check( base, extra );
    equal += r.equal ? 1 : 0;
    std::printf( "%-24s %s\n", base.filename().string().c_str(), r.summary.c_str() );
  }
  std::printf( "%d of %zu equal\n", equal, bases.size() );
  return equal == static_cast<int>( bases.size() ) ? 0 : 1;
}

// The control protocol served, as the arguments after the program name ask (the header comment).
int serve( std::vector<std::string> args )
{
  std::string tcp; // the port, with --server tcp:PORT
  bool mcp = false;
  std::string batch;
  std::string expect;
  std::vector<std::string> program;
  for ( std::size_t i = 0; i < args.size(); ++i )
  {
    std::string const& arg = args[i];
    bool const valued = i + 1 < args.size();
    if ( arg == "--server" )
    {
      if ( valued && args[i + 1].starts_with( "tcp:" ) )
        tcp = args[++i].substr( 4 );
    }
    else if ( arg == "--mcp" )
      mcp = true;
    else if ( arg == "--batch" && valued )
      batch = args[++i];
    else if ( arg == "--expect" && valued )
      expect = args[++i];
    else
      program.push_back( arg );
  }

  jag::control::Session session;
  jag::control::Dispatcher const dispatcher{ session };
  if ( !program.empty() )
  {
    jag::control::Json const answer =
        dispatcher.handle( { { "id", 0 }, { "method", "emu.load" }, { "params", { { "args", program } } } } );
    if ( !answer["ok"].get<bool>() )
    {
      std::fprintf( stderr, "%s\n", answer["error"]["message"].get<std::string>().c_str() );
      return 2;
    }
  }

  jag::server::McpServer mcpServer{ dispatcher };
  jag::server::JsonLinesServer const linesServer{ dispatcher };
  if ( !batch.empty() )
  {
    // a line of the script answered as the transport answers it; a blank line or one beginning
    // with # is a comment
    std::ifstream in( batch );
    if ( !in )
      throw std::runtime_error( "cannot read " + batch );
    std::vector<std::string> answers;
    for ( std::string line; std::getline( in, line ); )
    {
      if ( line.find_first_not_of( " \t\r" ) == std::string::npos || line.starts_with( "#" ) )
        continue;
      if ( mcp )
      {
        if ( auto const answer = mcpServer.handleLine( line ) )
          answers.push_back( *answer );
      }
      else
        answers.push_back( linesServer.handleLine( line ) );
    }
    if ( expect.empty() )
    {
      for ( std::string const& answer : answers )
        std::printf( "%s\n", answer.c_str() );
      return 0;
    }
    std::ifstream expected( expect );
    if ( !expected )
      throw std::runtime_error( "cannot read " + expect );
    // a transcript holds the version as devel, so that it is the same whatever build checks it
    std::string const version = "\"version\":\"" + std::string{ jag::control::version() } + "\"";
    std::string const recorded = "\"version\":\"devel\"";
    for ( std::string& answer : answers )
      for ( std::size_t at = 0; ( at = answer.find( version, at ) ) != std::string::npos; at += recorded.size() )
        answer.replace( at, version.size(), recorded );
    std::size_t n = 0;
    for ( std::string line; std::getline( expected, line ); ++n )
    {
      if ( n >= answers.size() )
      {
        std::printf( "answer %zu missing; expected\n  %s\n", n + 1, line.c_str() );
        return 1;
      }
      if ( answers[n] != line )
      {
        std::printf( "answer %zu differs\n  expected %s\n  answered %s\n", n + 1, line.c_str(), answers[n].c_str() );
        return 1;
      }
    }
    if ( n < answers.size() )
    {
      std::printf( "answer %zu not expected\n  %s\n", n + 1, answers[n].c_str() );
      return 1;
    }
    std::printf( "%zu answers equal\n", n );
    return 0;
  }

  if ( mcp )
  {
    std::fprintf( stderr, "jagmcp-cli %s serving MCP on stdio\n", std::string{ jag::control::version() }.c_str() );
    mcpServer.serve( std::cin, std::cout );
    return 0;
  }
  if ( !tcp.empty() )
  {
    // connections are served on threads of their own; the dispatcher answers one request at a time
    std::mutex oneAtATime;
    jag::server::TcpLineServer const transport{ static_cast<std::uint16_t>( std::stoul( tcp ) ),
                                                [&]( jag::control::Json const& request )
                                                {
                                                  std::scoped_lock const lock{ oneAtATime };
                                                  return dispatcher.handle( request );
                                                } };
    std::fprintf( stderr,
                  "jagmcp-cli %s serving JSON-lines on 127.0.0.1:%u\n",
                  std::string{ jag::control::version() }.c_str(),
                  static_cast<unsigned>( transport.port() ) );
    for ( ;; )
      std::this_thread::sleep_for( std::chrono::hours{ 1 } );
  }
  std::fprintf( stderr, "jagmcp-cli %s serving JSON-lines on stdio\n", std::string{ jag::control::version() }.c_str() );
  linesServer.serve( std::cin, std::cout );
  return 0;
}

void printHelp()
{
  std::printf( "jagmcp-cli %s - Atari Jaguar emulator, command-line version\n"
               "\n"
               "Usage:\n"
               "  jagmcp-cli PROGRAM [OPTIONS]               Run a program and print the final CPU state.\n"
               "  jagmcp-cli --m68k --bios FILE --cart FILE [OPTIONS]\n"
               "                                             Boot a cartridge with the BIOS.\n"
               "  jagmcp-cli --check DIR/NAME [OPTIONS]      Compare a run with a recorded reference.\n"
               "  jagmcp-cli --check-all DIR [OPTIONS]       Compare all references in DIR.\n"
               "  jagmcp-cli --server [tcp:PORT] [PROGRAM]   Serve the control protocol as JSON lines,\n"
               "                                             on stdin/stdout or on 127.0.0.1:PORT.\n"
               "  jagmcp-cli --mcp [PROGRAM]                 Serve the control protocol as MCP on stdio.\n"
               "  jagmcp-cli --batch FILE [--mcp] [--expect FILE] [PROGRAM]\n"
               "                                             Answer the requests in FILE, or compare the\n"
               "                                             answers with the --expect file.\n"
               "  jagmcp-cli --help                          Show this help.\n"
               "  jagmcp-cli --version                       Show the version.\n"
               "\n"
               "PROGRAM is a BS94 object file (.o), a COFF file (.cof) or a .run file. --check,\n"
               "--check-all and --batch with --expect exit with 0 when everything matches.\n"
               "\n"
               "Options:\n"
               "%s"
               "\n"
               "The control protocol is described in docs/spec/control-protocol.md.\n"
               "The emulator with a window is jagmcp.\n",
               std::string{ jag::control::version() }.c_str(),
               std::string{ jag::runOptionsHelp() }.c_str() );
}

} // namespace

int main( int argc, char** argv )
{
  try
  {
    std::vector<std::string> args( argv + 1, argv + argc );
    if ( args.empty() ||
         std::ranges::any_of( args, []( std::string const& a ) { return a == "--help" || a == "-h"; } ) )
    {
      printHelp();
      return args.empty() ? 2 : 0;
    }
    if ( std::ranges::find( args, "--version" ) != args.end() )
    {
      std::printf( "jagmcp-cli %s\n", std::string{ jag::control::version() }.c_str() );
      return 0;
    }
    // the arguments after the reference or the directory (--step) go to every run
    if ( args.size() >= 2 && args[0] == "--check" )
    {
      CheckResult const r = check( args[1], std::span( args ).subspan( 2 ) );
      std::printf( "%s\n%s", r.summary.c_str(), r.detail.c_str() );
      return r.equal ? 0 : 1;
    }
    if ( args.size() >= 2 && args[0] == "--check-all" )
      return checkAll( args[1], std::span( args ).subspan( 2 ) );
    if ( std::ranges::any_of(
             args, []( std::string const& a ) { return a == "--server" || a == "--mcp" || a == "--batch"; } ) )
      return serve( args );

    jag::RunOptions const options = jag::parseRunOptions( args );
    jag::RunResult const result = jag::runProgram( options, false );
    std::fputs( jag::formatState( result ).c_str(), stdout );
    // what worked by chance, apart from the state: on stderr
    for ( jag::UndrivenRead const& r : result.undriven )
    {
      char const* name = jag::Tom::registerName( r.address );
      std::fprintf(
          stderr,
          "undriven read: %s at $%06x read $%06x%s%s%s, which no register answers: $%04x, %llu times from cycle %llu\n",
          r.cpu.c_str(),
          r.pc,
          r.address,
          name ? " (" : "",
          name ? name : "",
          name ? ")" : "",
          r.value,
          static_cast<unsigned long long>( r.count ),
          static_cast<unsigned long long>( r.cycle ) );
    }
    return 0;
  }
  catch ( std::exception const& e )
  {
    std::fprintf( stderr, "%s\n", e.what() );
    return 2;
  }
}
