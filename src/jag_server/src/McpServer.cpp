#include "jag/server/McpServer.hpp"

#include <algorithm>
#include <array>
#include <istream>
#include <ostream>
#include <string_view>
#include <utility>

namespace jag::server
{

namespace
{

using control::Json;

// The protocol revisions this server speaks, newest first. A client asking for
// one of them gets it; any other gets the newest.
constexpr std::array<std::string_view, 4> PROTOCOL_VERSIONS{ "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05" };

// JSON-RPC 2.0's error codes.
constexpr int PARSE_ERROR = -32700;
constexpr int INVALID_REQUEST = -32600;
constexpr int METHOD_NOT_FOUND = -32601;
constexpr int INVALID_PARAMS = -32602;

constexpr std::string_view INSTRUCTIONS =
    "JagMCP emulates the Atari Jaguar (PAL, or NTSC with --ntsc), cycle for cycle against its FPGA core. Load a program with "
    "emu_load (the arguments jagmcp-cli takes, or a .run file; --m68k --bios FILE --cart FILE for a cartridge), "
    "run it with emu_run_frames (50 frames is a second) or emu_run_cycles, and look at it with "
    "video_screenshot, memory_read, cpu_get_state and cpu_disassemble (cpu m68k, gpu or dsp). Runs stop "
    "early at breakpoints and watchpoints (debug_breakpoint_add, debug_watchpoint_add); debug_trace shows the "
    "last instructions; input_set and input_press hold the pads' buttons. Time is counted in DSP cycles of "
    "26.59 MHz. The tools are the methods of docs/spec/control-protocol.md.";

std::string toolName( std::string const& method )
{
  std::string name = method;
  std::ranges::replace( name, '.', '_' );
  return name;
}

Json errorMessage( Json const& id, int code, std::string message )
{
  return Json{ { "jsonrpc", "2.0" },
               { "id", id },
               { "error", { { "code", code }, { "message", std::move( message ) } } } };
}

Json resultMessage( Json const& id, Json result )
{
  return Json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move( result ) } };
}

Json textContent( std::string text )
{
  return Json{ { "type", "text" }, { "text", std::move( text ) } };
}

// What `initialize` answers: the revision spoken, and what the server is.
Json initializeResult( Json const& params )
{
  std::string version{ PROTOCOL_VERSIONS.front() };
  if ( params.contains( "protocolVersion" ) && params.at( "protocolVersion" ).is_string() )
  {
    auto const& asked = params.at( "protocolVersion" ).get_ref<std::string const&>();
    if ( std::ranges::find( PROTOCOL_VERSIONS, asked ) != PROTOCOL_VERSIONS.end() )
    {
      version = asked;
    }
  }
  return Json{ { "protocolVersion", version },
               { "capabilities", { { "tools", { { "listChanged", false } } } } },
               { "serverInfo", { { "name", "jagmcp" }, { "version", std::string{ control::version() } } } },
               { "instructions", INSTRUCTIONS } };
}

} // namespace

McpServer::McpServer( std::vector<control::Method> methods, RequestHandler handler )
    : mMethods{ std::move( methods ) }, mHandler{ std::move( handler ) }
{
  for ( control::Method const& method : mMethods )
  {
    mTools.emplace( toolName( method.name ), method.name );
  }
}

void McpServer::serve( std::istream& in, std::ostream& out )
{
  std::string line;
  while ( std::getline( in, line ) )
  {
    if ( line.find_first_not_of( " \t\r" ) == std::string::npos )
    {
      continue;
    }
    if ( auto const answer = handleLine( line ) )
    {
      out << *answer << '\n' << std::flush;
    }
  }
}

McpServer::McpServer( control::Dispatcher const& dispatcher )
    : McpServer{ dispatcher.methods(), directHandler( dispatcher ) }
{
}

std::optional<std::string> McpServer::handleLine( std::string const& line )
{
  auto const message = Json::parse( line, nullptr, false );
  if ( message.is_discarded() )
  {
    return errorMessage( nullptr, PARSE_ERROR, "The message is not valid JSON" ).dump();
  }
  auto answer = handle( message );
  return answer ? std::optional<std::string>{ answer->dump() } : std::nullopt;
}

std::optional<Json> McpServer::handle( Json const& message )
{
  if ( !message.is_object() || message.value( "jsonrpc", "" ) != "2.0" || !message.contains( "method" ) ||
       !message.at( "method" ).is_string() )
  {
    return errorMessage(
        message.is_object() ? message.value( "id", Json{} ) : Json{}, INVALID_REQUEST, "Not a JSON-RPC 2.0 request" );
  }

  // A message without an id is a notification, answered by nothing; the
  // client's `notifications/initialized` is one.
  if ( !message.contains( "id" ) )
  {
    return std::nullopt;
  }
  Json const& id = message.at( "id" );
  auto const& method = message.at( "method" ).get_ref<std::string const&>();
  static Json const NO_PARAMS = Json::object();
  Json const& params = message.contains( "params" ) ? message.at( "params" ) : NO_PARAMS;

  if ( method == "initialize" )
  {
    return resultMessage( id, initializeResult( params ) );
  }
  if ( method == "ping" )
  {
    return resultMessage( id, Json::object() );
  }
  if ( method == "tools/list" )
  {
    return resultMessage( id, listTools() );
  }
  if ( method == "tools/call" )
  {
    Json error;
    auto result = callTool( params, error );
    return result ? resultMessage( id, std::move( *result ) )
                  : errorMessage( id, error.at( "code" ).get<int>(), error.at( "message" ).get<std::string>() );
  }
  return errorMessage( id, METHOD_NOT_FOUND, "Unknown method: " + method );
}

Json McpServer::listTools() const
{
  Json tools = Json::array();
  for ( control::Method const& method : mMethods )
  {
    tools.push_back( Json{ { "name", toolName( method.name ) },
                           { "description", method.info.description },
                           { "inputSchema", method.info.params } } );
  }
  return Json{ { "tools", std::move( tools ) } };
}

std::optional<Json> McpServer::callTool( Json const& params, Json& error ) const
{
  if ( !params.contains( "name" ) || !params.at( "name" ).is_string() )
  {
    error = Json{ { "code", INVALID_PARAMS }, { "message", "tools/call needs a tool name" } };
    return std::nullopt;
  }
  auto const tool = mTools.find( params.at( "name" ).get<std::string>() );
  if ( tool == mTools.end() )
  {
    error =
        Json{ { "code", INVALID_PARAMS }, { "message", "Unknown tool: " + params.at( "name" ).get<std::string>() } };
    return std::nullopt;
  }
  Json arguments = params.contains( "arguments" ) ? params.at( "arguments" ) : Json::object();
  if ( arguments.is_null() )
  {
    arguments = Json::object();
  }

  // A tool's failure is its result, flagged, so that the agent reads why; only
  // a call that names no tool is a protocol error.
  Json const response =
      mHandler( Json{ { "id", 1 }, { "method", tool->second }, { "params", std::move( arguments ) } } );
  if ( response.at( "ok" ) != true )
  {
    auto const& failure = response.at( "error" );
    return Json{ { "content",
                   Json::array( { textContent( failure.at( "code" ).get<std::string>() + ": " +
                                               failure.at( "message" ).get<std::string>() ) } ) },
                 { "isError", true } };
  }

  Json result = response.at( "result" );
  Json content = Json::array();
  // A picture is shown, not spelled out in base64.
  if ( result.is_object() && result.contains( "png_base64" ) )
  {
    content.push_back(
        Json{ { "type", "image" }, { "data", result.at( "png_base64" ) }, { "mimeType", "image/png" } } );
    result.erase( "png_base64" );
  }
  content.push_back( textContent( result.dump() ) );
  return Json{ { "content", std::move( content ) }, { "isError", false } };
}

} // namespace jag::server
