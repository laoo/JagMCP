#include "Methods.hpp"

#include "jag/Machine.hpp"
#include "jag/Risc.hpp"
#include "jag/RiscDisassembler.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jag::control
{

namespace
{

// The most instructions a disassembly answers.
constexpr std::uint64_t MAX_INSTRUCTIONS = 1000;

Risc* risc( Machine& machine, Cpu cpu )
{
  return cpu == Cpu::Gpu ? machine.gpu() : machine.dsp();
}

// A RISC's instruction at `address`, its words read as memory.read reads them; 0 where nothing is.
RiscInstruction riscInstruction( Machine const& machine, bool gpu, std::uint32_t address )
{
  return disassembleRisc( gpu,
                          address,
                          [&machine, address]( std::uint32_t n )
                          {
                            std::uint32_t const at = address + ( 2 * n );
                            return static_cast<std::uint16_t>( machine.peek( at ).value_or( 0 ) << 8 |
                                                               machine.peek( at + 1 ).value_or( 0 ) );
                          } );
}

// A processor's registers as cpu.get_state answers them.
Outcome stateJson( Machine& machine, Cpu cpu )
{
  if ( cpu == Cpu::M68k )
  {
    auto const state = machine.m68kState();
    if ( !state )
      return std::unexpected( noCpu( cpu ) );
    auto const disasm = machine.disassemble68k( state->pc );
    return Json{
      { "pc", state->pc },           { "d", state->d },           { "a", state->a },
      { "sr", state->sr },           { "usp", state->usp },       { "isp", state->isp },
      { "stopped", state->stopped }, { "halted", state->halted }, { "disasm", disasm ? disasm->first : "" }
    };
  }
  Risc* r = risc( machine, cpu );
  if ( !r )
    return std::unexpected( noCpu( cpu ) );
  Json registers = Json::array();
  for ( int i = 0; i < 64; ++i )
    registers.push_back( r->reg( i ) );
  std::uint32_t const pc = r->pc();
  return Json{ { "pc", pc },
               { "flags", r->flagsRegister() },
               { "control", r->controlRegister() },
               { "registers", std::move( registers ) },
               { "bank", r->bank() },
               { "started", r->started() },
               { "cycle", r->now() },
               { "disasm", riscInstruction( machine, cpu == Cpu::Gpu, pc ).text } };
}

// `name` as "<letter><number>" with the number below `count`, else nullopt: r5, d0, a7.
std::optional<std::size_t> numbered( std::string const& name, char letter, std::size_t count )
{
  if ( name.size() < 2 || name.size() > 3 || name[0] != letter || ( name.size() == 3 && name[1] == '0' ) )
    return std::nullopt;
  std::size_t n = 0;
  for ( char const c : name.substr( 1 ) )
  {
    if ( c < '0' || c > '9' )
      return std::nullopt;
    n = ( 10 * n ) + static_cast<std::size_t>( c - '0' );
  }
  return n < count ? std::optional{ n } : std::nullopt;
}

// The value of `registers[name]`, an integer or a string of hex digits, at most `max`.
std::expected<std::uint32_t, Error> registerValue( Json const& registers, std::string const& name, std::uint64_t max )
{
  auto const value = requireUnsigned( registers, name );
  if ( !value )
    return std::unexpected( value.error() );
  if ( *value > max )
    return std::unexpected( badRequest( "Register " + name + " is above " + std::to_string( max ) ) );
  return static_cast<std::uint32_t>( *value );
}

// The GPU's or the DSP's registers set from `registers`: r0 to r31 of `bank`, and flags.
std::optional<Error> setRiscRegisters( Risc& r, Json const& registers, unsigned bank )
{
  std::vector<std::pair<std::size_t, std::uint32_t>> writes;
  std::optional<std::uint32_t> flags;
  for ( auto const& item : registers.items() )
  {
    std::string const& name = item.key();
    if ( name == "flags" )
    {
      auto const value = registerValue( registers, name, 7 );
      if ( !value )
        return value.error();
      flags = *value;
    }
    else if ( auto const n = numbered( name, 'r', 32 ) )
    {
      auto const value = registerValue( registers, name, UINT32_MAX );
      if ( !value )
        return value.error();
      writes.emplace_back( ( std::size_t{ 32 } * bank ) + *n, *value );
    }
    else if ( name == "pc" )
    {
      return badRequest( "A RISC's PC cannot be set: its pipeline holds the instructions after it" );
    }
    else
    {
      return badRequest( "Unknown register " + name + ": r0 to r31 and flags" );
    }
  }
  for ( auto const& [index, value] : writes )
    r.setReg( static_cast<int>( index ), value );
  if ( flags )
    r.setAluFlags( *flags );
  return std::nullopt;
}

// The 68000's registers set from `registers`: d0 to d7, a0 to a7, pc, sr, usp and isp.
std::optional<Error> setM68kRegisters( Machine& machine, Json const& registers )
{
  Machine::M68kRegisters set;
  for ( auto const& item : registers.items() )
  {
    std::string const& name = item.key();
    std::optional<std::uint32_t>* target = nullptr;
    std::uint64_t max = UINT32_MAX;
    if ( auto const n = numbered( name, 'd', 8 ) )
    {
      target = &set.d[*n];
    }
    else if ( auto const m = numbered( name, 'a', 8 ) )
    {
      target = &set.a[*m];
    }
    else if ( name == "pc" )
    {
      target = &set.pc;
      max = 0xfffffe;
    }
    else if ( name == "sr" )
    {
      target = &set.sr;
      max = 0xffff;
    }
    else if ( name == "usp" )
    {
      target = &set.usp;
    }
    else if ( name == "isp" )
    {
      target = &set.isp;
    }
    else
    {
      return badRequest( "Unknown register " + name + ": d0 to d7, a0 to a7, pc, sr, usp and isp" );
    }
    auto const value = registerValue( registers, name, max );
    if ( !value )
      return value.error();
    if ( target == &set.pc && ( *value & 1u ) != 0 )
      return badRequest( "Register pc must be even" );
    *target = *value;
  }
  if ( !machine.setM68kRegisters( set ) )
    return Error{ .code = "cpu_busy",
                  .message = "The 68000 is inside an instruction, its bus cycle waiting for the bus: run on first" };
  return std::nullopt;
}

} // namespace

std::pair<std::string, std::uint32_t> instructionAt( Machine& machine, Cpu cpu, std::uint32_t address )
{
  if ( cpu == Cpu::M68k )
    return machine.disassemble68k( address ).value_or( std::pair<std::string, std::uint32_t>{} );
  RiscInstruction const instruction = riscInstruction( machine, cpu == Cpu::Gpu, address );
  return { instruction.text, 2 * instruction.words };
}

void addCpuMethods( Dispatcher& dispatcher, Session& session )
{
  dispatcher.add( "cpu.get_state",
                  info( "A processor's registers between runs: the GPU's or the DSP's 64 (bank 0, then bank 1), "
                        "the bank in use, PC, FLAGS and CTRL; or the 68000's. The instruction at PC is "
                        "disassembled.",
                        { { "cpu", "string", "m68k, gpu or dsp; the program's own when left out.", false } } ),
                  [&session]( Json const& params ) -> Outcome
                  {
                    auto const machine = loadedMachine( session );
                    if ( !machine )
                      return std::unexpected( machine.error() );
                    auto const cpu = cpuParam( params, session );
                    if ( !cpu )
                      return std::unexpected( cpu.error() );
                    return stateJson( **machine, *cpu );
                  } );

  dispatcher.add(
      "cpu.set_registers",
      info( "Sets a processor's registers between runs, and answers its state as cpu.get_state does. The GPU's or "
            "the DSP's: r0 to r31 and flags (Z, C, N: bits 0 to 2). A result an instruction already issued has "
            "not written yet still writes after. The 68000's: d0 to d7, a0 to a7 (a7 the stack pointer in use), "
            "pc, sr, usp and isp. Not a RISC's PC.",
            { { "registers", "object", "Register names and values: {\"r5\": 1, \"flags\": 0}, {\"d0\": \"0x10\"}." },
              { "cpu", "string", "m68k, gpu or dsp; the program's own when left out.", false },
              { "bank",
                "integer",
                "The GPU's or the DSP's bank r0 to r31 name, 0 or 1; the bank in use when left out.",
                false } } ),
      [&session]( Json const& params ) -> Outcome
      {
        auto const field = params.find( "registers" );
        if ( field == params.end() || !field->is_object() || field->empty() )
          return std::unexpected( badRequest( "Missing or invalid field: registers" ) );
        auto const machine = loadedMachine( session );
        if ( !machine )
          return std::unexpected( machine.error() );
        auto const cpu = cpuParam( params, session );
        if ( !cpu )
          return std::unexpected( cpu.error() );
        if ( *cpu == Cpu::M68k )
        {
          if ( !( *machine )->m68kState() )
            return std::unexpected( noCpu( *cpu ) );
          if ( params.contains( "bank" ) )
            return std::unexpected( badRequest( "Field bank is the GPU's or the DSP's" ) );
          if ( auto const failed = setM68kRegisters( **machine, *field ) )
            return std::unexpected( *failed );
          return stateJson( **machine, *cpu );
        }
        Risc* r = risc( **machine, *cpu );
        if ( !r )
          return std::unexpected( noCpu( *cpu ) );
        auto const bank = optionalUnsigned( params, "bank", r->bank() );
        if ( !bank )
          return std::unexpected( bank.error() );
        if ( *bank > 1 )
          return std::unexpected( badRequest( "Field bank must be 0 or 1" ) );
        if ( auto const failed = setRiscRegisters( *r, *field, static_cast<unsigned>( *bank ) ) )
          return std::unexpected( *failed );
        return stateJson( **machine, *cpu );
      } );

  dispatcher.add( "cpu.disassemble",
                  info( "Disassembles `count` instructions from `address`, read from memory without a bus cycle. "
                        "Each comes with its bytes, as memory.read gives them.",
                        { { "address", "integer", "Where to start (an integer, or a string \"0x...\")." },
                          { "count", "integer", "Instructions, at most 1000." },
                          { "cpu", "string", "m68k, gpu or dsp; the program's own when left out.", false } } ),
                  [&session]( Json const& params ) -> Outcome
                  {
                    auto const address = requireUnsigned( params, "address" );
                    if ( !address )
                      return std::unexpected( address.error() );
                    auto const count = requireUnsigned( params, "count" );
                    if ( !count )
                      return std::unexpected( count.error() );
                    if ( *count > MAX_INSTRUCTIONS )
                      return std::unexpected( badRequest( "Field count is above 1000" ) );
                    auto const machine = loadedMachine( session );
                    if ( !machine )
                      return std::unexpected( machine.error() );
                    auto const cpu = cpuParam( params, session );
                    if ( !cpu )
                      return std::unexpected( cpu.error() );
                    Json lines = Json::array();
                    auto at = static_cast<std::uint32_t>( *address & 0xffffffu );
                    for ( std::uint64_t i = 0; i < *count; ++i )
                    {
                      std::string text;
                      std::uint32_t length = 0;
                      if ( *cpu == Cpu::M68k )
                      {
                        auto const instruction = ( *machine )->disassemble68k( at );
                        if ( !instruction )
                          return std::unexpected( noCpu( *cpu ) );
                        text = instruction->first;
                        length = instruction->second;
                      }
                      else
                      {
                        RiscInstruction const instruction = riscInstruction( **machine, *cpu == Cpu::Gpu, at );
                        text = instruction.text;
                        length = 2 * instruction.words;
                      }
                      std::vector<std::uint8_t> bytes( length );
                      for ( std::uint32_t k = 0; k < length; ++k )
                        bytes[k] = ( *machine )->peek( ( at + k ) & 0xffffffu ).value_or( 0 );
                      lines.push_back( Json{ { "address", at },
                                             { "length", length },
                                             { "opcode_hex", toHex( bytes.data(), bytes.size() ) },
                                             { "text", text } } );
                      at = ( at + length ) & 0xffffffu;
                    }
                    return lines;
                  } );
}

} // namespace jag::control
