# Working in this repository

## Language

Discussion with the user is in Polish. **All code, comments, documentation, commit messages and
identifiers are in English.**

## Build and test

```sh
cmake --preset release && cmake --build --preset release && ctest --preset release
build/release/src/jag_cli/jagmcp-cli --check-all tests/reference        # every reference, one line each
build/release/src/jag_cli/jagmcp-cli --check-all tests/reference --step # the same, without the automatons
build/release/src/jag_cli/jagmcp-cli --check tests/reference/abs        # one, with the first difference
./scripts/build-pgo.sh   # build/pgo, profile-guided (0016); extra runs to profile as arguments
```

Presets: `debug`, `release`, `asan`, and `pgo-gen` and `pgo` for the script. Never create build
directories by hand; the presets own `build/<preset>`.

The references are traces of the FPGA simulation (`sim/`). Recording them needs Verilator, the
Jaguar_MiSTer checkout and lyxass beside this repository, and the `m68k-elf-` binutils:

```sh
make -C sim                                                   # sim/obj_dir/jagsim
./scripts/make-references.py                                  # every tests/asm/*.js, tests/m68k/*.s and tests/programs/*.run
./scripts/make-references.py --no-asm --random alu:0:200      # generated programs of a class
./scripts/make-references.py --no-asm --out /tmp/wide --random gpu_blit:0:2000  # a wider run, not kept
./scripts/make-references.py --no-asm --out /tmp/irq --random m68k_irq:0:200     # a tool, not references
./scripts/disasm.py tests/reference/random_alu_0007.o f1b030 f1b040
```

The core's trace carries `I` events (opcode, registers, operands) that the comparison ignores; run
`jagmcp-cli <file.o> <args> --log FILE` to see them next to the reference trace, and `--frames DIR`
(either program) to see the frames as images, `--wav FILE` (either) to hear the I2S output.
`build/release/src/jag_app/jagmcp tests/programs/tj_julia.run` runs a program in the window.

A running program is driven through the control protocol
([docs/spec/control-protocol.md](docs/spec/control-protocol.md)); the `jagmcp` skill
(`.claude/skills/jagmcp`) says how an agent uses it:

```sh
build/release/src/jag_cli/jagmcp-cli --server tcp:7710 [program]   # then scripts/jagmcp.py METHOD k=v
build/release/src/jag_cli/jagmcp-cli --mcp [program]               # MCP on stdio
build/release/src/jag_app/jagmcp --server tcp:7710 --mcp-http 7711 [program]  # the window, served
cd tests && ../build/release/src/jag_cli/jagmcp-cli --batch control/lines.jsonl > control/lines.expect  # re-record a transcript
```

## Layout

| Path | Contents |
|---|---|
| `src/jag_core/` | The emulated hardware and the runner. All logic; deterministic. |
| `src/jag_control/` | The control API: the session that owns a machine, the dispatcher and its methods. |
| `src/jag_server/` | Its transports, PGMEmu's: JSON-lines on stdio and TCP, MCP on stdio and HTTP. |
| `src/jag_cli/` | `jagmcp-cli`: runs a program, checks the core against the references, serves the control API. |
| `src/jag_app/` | `jagmcp`: the window, as PGMEmu's; SDL3 and ImGui are fetched by `cmake/Dependencies.cmake`. |
| `sim/` | The Verilator simulation of the Jaguar_MiSTer core (`jagsim`), the reference. |
| `libextern/` | Third-party code carried unmodified (Moira, the 68000); see its `README.md`. |
| `tests/asm/` | lyxass test programs; only their `RUN` directive is read. |
| `tests/m68k/` | 68000 test programs (`--m68k`), assembled with `m68k-elf-as`; only their `RUN` directive (`\| RUN:`) is read. |
| `tests/programs/` | Binaries of real programs and their data; a `.run` names the binary and the arguments. |
| `tests/reference/` | Recorded runs of `jagsim`: `.o`, `.args`, `.trace`, `.state`. Each is a CTest test. |
| `tests/control/` | Transcripts of the control protocol (`.jsonl` requests, `.expect` answers the emulator wrote) and the network transports' test. |
| `tests/console/` | Sources of tests for a real console; their measurements are in the decisions. Not built, not run by CTest. |
| `.claude/skills/` | The `jagmcp` skill: driving the emulator as an agent. |
| `scripts/` | Recording references, disassembling, formatting; `jagmcp.py`, the control protocol's client. |
| `docs/` | See `docs/README.md`; it is the index and the rulebook. |

## The workspace around it

The repository builds and tests on its own. These checkouts beside it are optional, needed only to
record references, and are never edited from here:

- `../Jaguar_MiSTer` (https://github.com/MiSTer-devel/Jaguar_MiSTer): its `rtl/` is **the
  hardware reference** ([0002](docs/decisions/0002-the-fpga-core-is-the-reference.md)), Tom and
  Jerry translated from the original netlists. The DSP is `rtl/Jerry/j_dsp.v` and
  `rtl/jaguar_common/`. Read at `43a761d`.
- `../lyxass` (https://github.com/42Bastian/lyxass) assembles `tests/asm/`.
- `../SoftRef_V10_ext.pdf`, the Jaguar Software Reference Manual, for what the netlist does not
  name: register intent, addresses.
- `../TomNJerry`, the owner's earlier DSP simulator. A source of test programs, not of behaviour;
  its known differences from the FPGA core are in its `FPGA_DIFFERENCES.md`.

## Code style

Enforced by `.clang-format` and `.clang-tidy` (NGA's, as PGMEmu's): Allman braces, 2-space indent,
spaces inside parentheses, east const, `CamelCase` types, `camelBack` functions and variables,
`mMember` class members, `UPPER_CASE` constants. Emulated registers and buses are held in unsigned
fixed-width types. A netlist signal keeps its words in its C++ name: `alu_wback` becomes
`aluWback`. Every hardware module's header comment names the netlist files it ports and the
Jaguar_MiSTer commit they were read at.

```sh
./scripts/format.sh          # format in place
./scripts/format.sh --check  # fail on deviation
./scripts/tidy.sh            # clang-tidy against build/debug
```

## Documentation discipline

The rules are in `docs/README.md` and they are not optional: documentation carries **why** and
**contracts**, code carries **how**; one fact, one place; no status files; documentation changes in
the same commit as the code that invalidated it; a new document needs an entry in
`docs/README.md`.

## Never

- Commit before the owner has reviewed.
- Change a reference trace by hand. A reference is re-recorded from `jagsim`, or the simulation is
  fixed and every reference re-recorded.
