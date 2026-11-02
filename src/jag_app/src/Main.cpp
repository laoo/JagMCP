// jagmcp: the desktop front end. Everything it does is Application's.
//
//   jagmcp                          an empty window; a program dropped onto it is started
//   jagmcp <file.run>               a program of tests/programs
//   jagmcp <file.o> [options]       a program and the runner's options, as jagmcp-cli takes them
//
//   --server tcp:PORT               serve the control protocol as JSON lines on 127.0.0.1:PORT
//   --mcp-http PORT                 serve it as MCP at http://127.0.0.1:PORT/mcp
//   --record FILE                   record the picture and the sound to FILE until the window closes
//
// The program runs without a limit: --cycles is not heeded, nor are the runner's outputs. The
// servers drive the program on screen (docs/spec/control-protocol.md).

#include "Application.hpp"

#include "jag/Runner.hpp"
#include "jag/control/Dispatcher.hpp"

#include <SDL3/SDL_main.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace
{

void printHelp()
{
  std::printf( "jagmcp %s - Atari Jaguar emulator\n"
               "\n"
               "Usage:\n"
               "  jagmcp [CARTRIDGE]                         Open the window. Boot CARTRIDGE if given.\n"
               "  jagmcp PROGRAM [OPTIONS]                   Run a program in the window.\n"
               "  jagmcp --m68k --bios FILE --cart FILE [OPTIONS]\n"
               "                                             Boot a cartridge with the given BIOS.\n"
               "  jagmcp --help                              Show this help.\n"
               "  jagmcp --version                           Show the version.\n"
               "\n"
               "Server options:\n"
               "  --server tcp:PORT   Serve the control protocol as JSON lines on 127.0.0.1:PORT.\n"
               "  --mcp-http PORT     Serve the control protocol as MCP on http://127.0.0.1:PORT/mcp.\n"
               "\n"
               "Recording:\n"
               "  --record FILE       Record the picture and the sound to FILE (.mp4, .mkv or .mov).\n"
               "                      The file is written when the window closes. Needs ffmpeg.\n"
               "                      File > Record video does the same from the menu.\n"
               "\n"
               "The BIOS is found next to the cartridge, in a bios folder there, or one folder up.\n"
               "You can also choose it with File > Choose BIOS.\n"
               "\n"
               "Program options (--cycles, --log, --frames and --wav are ignored in the window;\n"
               "use jagmcp-cli for them, or --record for a video):\n"
               "%s"
               "\n"
               "Keys (pad 1): arrows = joypad, Z X C = A B C, Tab = Option, Return = Pause,\n"
               "0-9 = keypad, - = star, = = hash.\n"
               "\n"
               "The control protocol is described in docs/spec/control-protocol.md.\n"
               "The emulator without a window is jagmcp-cli.\n",
               std::string{ jag::control::version() }.c_str(),
               std::string{ jag::runOptionsHelp() }.c_str() );
}

int run( int argc, char** argv )
{
  for ( int i = 1; i < argc; ++i )
  {
    std::string_view const arg = argv[i];
    if ( arg == "--help" || arg == "-h" )
    {
      printHelp();
      return 0;
    }
    if ( arg == "--version" )
    {
      std::printf( "jagmcp %s\n", std::string{ jag::control::version() }.c_str() );
      return 0;
    }
  }
  // the window's own flags, wherever they are; the rest is the program
  std::vector<std::string> args;
  std::uint16_t linePort = 0;
  std::uint16_t mcpPort = 0;
  std::string record;
  for ( int i = 1; i < argc; ++i )
  {
    std::string const arg = argv[i];
    if ( arg == "--server" && i + 1 < argc && std::string_view{ argv[i + 1] }.starts_with( "tcp:" ) )
      linePort = static_cast<std::uint16_t>( std::stoul( std::string{ argv[++i] }.substr( 4 ) ) );
    else if ( arg == "--mcp-http" && i + 1 < argc )
      mcpPort = static_cast<std::uint16_t>( std::stoul( argv[++i] ) );
    else if ( arg == "--record" && i + 1 < argc )
      record = argv[++i];
    else
      args.push_back( arg );
  }
  auto application = jag::app::Application::create();
  if ( !application )
  {
    std::fprintf( stderr, "jagmcp: cannot start: %s\n", application.error().c_str() );
    return 1;
  }
  ( *application )->serve( linePort, mcpPort );
  if ( !args.empty() )
    ( *application )->load( args );
  if ( !record.empty() )
    ( *application )->startRecord( record );
  ( *application )->run();
  return 0;
}

} // namespace

// Nothing may escape main: an exception escaping it ends the program without a word.
int main( int argc, char** argv )
{
  try
  {
    return run( argc, argv );
  }
  catch ( std::exception const& error )
  {
    std::fprintf( stderr, "jagmcp: %s\n", error.what() );
    return 2;
  }
  catch ( ... )
  {
    std::fputs( "jagmcp: internal error: unknown exception\n", stderr );
    return 2;
  }
}
