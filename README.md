# JagMCP

JagMCP is an emulator of the Atari Jaguar. Its goal is to be as faithful to the hardware as
possible. Speed is not the goal.

The reference is the [Jaguar_MiSTer](https://github.com/MiSTer-devel/Jaguar_MiSTer) FPGA core. Its
Tom and Jerry chips are the original Atari netlists, translated to Verilog by Gregory Estrade
(Torlus). JagMCP ports this logic to C++. It is tested cycle by cycle against a simulation of the
core.

## Building

You need CMake 3.25 or later, Ninja and a C++23 compiler. The project is developed with Clang.

```sh
cmake --preset release && cmake --build --preset release && ctest --preset release
```

The window needs SDL3 and Dear ImGui. CMake downloads them when it configures. To build without
the window, add `-DJAG_BUILD_APP=OFF`.

`scripts/build-pgo.sh` makes a faster, profile-guided build in `build/pgo`. It needs Clang.

Each tagged release on GitHub has packages for macOS (arm64), Linux (x64) and Windows (x64).

## Running

JagMCP does not include a BIOS or any games. You need your own 128 KB boot ROM (for example
`jagboot.rom`) and cartridge images (`.j64` or `.jag`).

Both programs print their options with `--help`.

### The window: `jagmcp`

```sh
build/release/src/jag_app/jagmcp game.j64
```

You can also drop a cartridge onto the window, or use File > Open. The window looks for the BIOS
next to the cartridge, in a `bios` folder there, or one folder up. You can also give it with
`--bios`, or choose it with File > Choose BIOS. The window remembers it.

The window plays the video and the sound as they are made. It runs at the console's speed when the
machine is fast enough, and slower when it is not.

To record a video with sound, use File > Record video, or start the window with `--record`:

```sh
build/release/src/jag_app/jagmcp game.j64 --record game.mp4
```

The file is written when you choose File > Stop recording or close the window. It needs
[ffmpeg](https://ffmpeg.org) with libx264. The video plays at the console's speed, even when the
window ran slower. The window does not write `--frames`, `--wav` or `--log`. `jagmcp-cli` does.

The keyboard is the pad in port 1:

| Key | Pad |
|---|---|
| Arrows | Joypad |
| Z, X, C | A, B, C |
| Tab | Option |
| Return | Pause |
| 0 to 9 | Keypad |
| `-`, `=` | Star, hash |

### The command line: `jagmcp-cli`

```sh
build/release/src/jag_cli/jagmcp-cli --m68k --bios BIOS.rom --cart game.j64 --cycles 60000000
build/release/src/jag_cli/jagmcp-cli program.o --cycles 100000 --log trace.txt
```

`jagmcp-cli` has no window. It runs a program for a number of cycles. Then it prints the final
state of the processors, in the same format as the FPGA simulation in `sim/`.

Useful options:

- `--log FILE` writes a trace of every event.
- `--frames DIR` writes the video frames as images.
- `--wav FILE` writes the sound.
- `--check` compares a run with a recorded reference.

`program.o` is a DSP program in the format that [lyxass](https://github.com/42Bastian/lyxass)
writes (BS94). `--gpu` runs it on the GPU. `--m68k` runs a 68000 program, or boots a cartridge
with `--bios` and `--cart`.

A COFF file from Atari's linkers (`program.cof`, as `rln -a 4000` makes it) runs on the 68000
without a BIOS. It is started as a loader such as the Skunkboard starts it, after the BIOS has set
the video ([0028](docs/decisions/0028-a-coff-program-runs-as-a-loader-starts-it.md)).

## Remote control

Other programs can control a running emulator, with or without the window. They can load a
program, run it by frames or cycles, press buttons, read and write memory, read registers,
disassemble code, set breakpoints and watchpoints, take screenshots and record the sound.

There is one set of methods. It is available in two ways:

- **JSON lines**, on stdio or TCP. Each request is one line of JSON. Each answer is one line.
- **MCP** (Model Context Protocol), on stdio or HTTP. Each method is an MCP tool, so an AI agent
  can use the emulator directly. Screenshots come back as images.

The protocol is specified in [docs/spec/control-protocol.md](docs/spec/control-protocol.md).

### JSON lines

```sh
jagmcp-cli --server tcp:7710 --m68k --bios BIOS.rom --cart game.j64
```

Then send requests from another terminal:

```sh
echo '{"id":1,"method":"emu.run_frames","params":{"count":250}}' | nc -w 60 127.0.0.1 7710
```

```json
{"id":1,"ok":true,"result":{"reason":"completed","cycles_executed":145380614,"frames_executed":250,"running":true}}
```

`scripts/jagmcp.py` is a small client. It sends one request and prints the result:

```sh
scripts/jagmcp.py emu.run_frames count=250
scripts/jagmcp.py video.screenshot path=shot.png
```

Without `tcp:PORT`, `--server` reads requests on stdin and writes answers on stdout.

### MCP

```sh
jagmcp-cli --mcp              # MCP on stdio
jagmcp --mcp-http 7711        # the window, MCP on http://127.0.0.1:7711/mcp
```

To add it to Claude Code:

```sh
claude mcp add jagmcp -- /path/to/jagmcp-cli --mcp
```

### The window

The window serves the same protocol while you watch it:

```sh
jagmcp --server tcp:7710 --mcp-http 7711 game.j64
```

All clients control the same emulator. When one client loads a program, the others see it too.

The servers listen on 127.0.0.1 only. They have no authentication. Do not expose them to a
network.

## Tests

`ctest` runs about 3900 recorded references from `tests/reference`. Each reference is a run of the
FPGA simulation: its trace and its final state. Each one runs twice: once with the fast timing and
once with the plain step-by-step core. `ctest` also checks recorded sessions of the control
protocol. It needs only the build. It does not need a BIOS or games.

To record new references, you need Verilator, the `m68k-elf-` binutils, and checkouts of
Jaguar_MiSTer and lyxass next to this repository. [CLAUDE.md](CLAUDE.md) has the commands.

The design is in [docs/architecture.md](docs/architecture.md). The reasons for it are in
[docs/decisions/](docs/decisions/). [docs/README.md](docs/README.md) is the index.

## Licence

JagMCP is released under the GNU General Public License, version 2 only ([LICENSE](LICENSE)). This
is the licence of the Jaguar_MiSTer core. [THIRD_PARTY.md](THIRD_PARTY.md) lists the work it is
based on.
