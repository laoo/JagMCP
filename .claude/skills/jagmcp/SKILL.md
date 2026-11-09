---
name: jagmcp
description: Load, run, inspect and debug programs on JagMCP, the cycle-exact Atari Jaguar emulator in this repository, while they run. Use it to start a DSP, GPU or 68000 program or a cartridge through the BIOS headless, run frames or cycles, stop at breakpoints or watchpoints on the 68000, the GPU or the DSP, see the last instructions executed, read and write memory and registers, disassemble, press the pads' buttons, take screenshots, or capture the I2S sound.
---

# Driving JagMCP

JagMCP answers a JSON control protocol (`docs/spec/control-protocol.md`, every method with its
parameters). Run it headless as a server that keeps its state, and send it one request per
command with `scripts/jagmcp.py`. Work from the repository root.

## Start it

Build once, then start the server in the background (Bash with `run_in_background`):

```sh
cmake --build --preset release
build/release/src/jag_cli/jagmcp-cli --server tcp:7710
```

A program may follow the flags, loaded before the first request: `... --server tcp:7710
tests/programs/tj_julia.run`. When done, stop the one you started, by its port:
`pkill -f "jagmcp-cli --server tcp:7710"`.

A person may have the window open instead, started with `jagmcp --server tcp:7710` (and
`--mcp-http PORT` for MCP): then the same requests drive the program on their screen, and you must
not start a second server. Requests are answered between two parts of the run; the program runs
on between them, so read what you need right after the run that stops where you want it.

If 7710 is taken, pick another port and pass it to every request as `--port PORT`, written out on
each call (zsh does not split a command kept in a variable).

To have the emulator as MCP tools in your own sessions instead, with screenshots shown as images,
register it for yourself (it is not committed, as each session would then run the binary of its
start, and rebuilds are frequent here):
`claude mcp add --scope local jagmcp -- "$PWD/build/release/src/jag_cli/jagmcp-cli" --mcp`.
Reconnect it (`/mcp`) after a rebuild.

## Programs

- A `.run` file of `tests/programs` (the `tj_` intros, GPU and DSP, which set no video up and so make no
  frames).
- A reference's program with its arguments: `reference/op_bitmap.o --gpu` (paths are the server's,
  relative to where it was started).
- A COFF program (`GAME.cof`, homebrew linked at $4000): alone, without `--m68k` or a BIOS; it
  starts as a Skunkboard starts it (decision 0028). `--bios` with it is refused.
- A cartridge through the BIOS: `--m68k --bios BIOS --cart GAME.j64`, with the MiSTer core's
  compatibility fixes on (`--no-compat` for the bare netlist; decision 0022), and a DSP store that
  loses the bus does as on the console (`--open-bus` for the netlist's; decision 0033). The BIOS and games are never
  in the repository ([0015](../../../docs/decisions/0015-bios-and-games-checked-outside-the-repository.md));
  the owner keeps them beside it: the BIOS is a 128 KB `jagboot*.rom`, the games `*.j64`.
  Never copy them in, and never commit a screenshot or a capture of one.

## Send requests

```sh
scripts/jagmcp.py emu.load --m68k --bios BIOS.rom --cart GAME.j64
scripts/jagmcp.py emu.run_frames count=250                  # 50 frames is a second (PAL)
scripts/jagmcp.py video.screenshot path=build/shot.png      # then Read build/shot.png to see it
```

Parameters are `key=value` (JSON values; `0x...` is hex) or one JSON object; `emu.load` takes the
program and its options as `jagmcp-cli` does. The answer is the result as JSON; an error goes to
stderr with exit status 1. Numbers in answers are decimal: `"pc": 15740928` is 0xf03000.

## What to ask for

| To | Methods |
|---|---|
| Load, reset, see where it is | `emu.load {args}`, `emu.reset`, `emu.status` |
| Run | `emu.run_frames {count, timeout_cycles}`, `emu.run_cycles {count}` (DSP cycles, 26.59 MHz) |
| Processors | `cpu.get_state {cpu}` and `cpu.disassemble {cpu, address, count}`, `cpu` one of `m68k`, `gpu`, `dsp` (the program's own when left out) |
| Change registers | `cpu.set_registers {cpu, registers, bank?}`: `registers='{"r1":"0x10000","flags":0}'` (GPU, DSP: r0 to r31 of the bank in use, flags Z C N; no PC) or `'{"d0":1,"a7":"0x8000","pc":"0x4032"}'` (68000); answers the new state. A load or divide already issued still writes its result after |
| Memory | `memory.read {address, size}`, `memory.write {address, data_hex}`: main RAM, the ROMs (read only), the GPU's RAM at 0xf03000 and the DSP's at 0xf1b000; not registers |
| Picture | `video.screenshot {path}`: the last frame, a pixel a pixel (the window stretches it to 4:3) |
| Sound | `audio.capture_start {path}` / `audio.capture_stop` (a WAV at the I2S frame rate) |
| Video | `video.record_start {path, lossless?, crisp?}` / `video.record_stop` (picture and sound, by ffmpeg; an absolute path) |
| Stop at code | `debug.breakpoint.add/remove {address, cpu}`, `debug.breakpoint.list`: a run stops before that instruction |
| Stop at data | `debug.watchpoint.add {address, size, access: read/write/access, cpu}`, `remove`, `list`: a run stops once the access is made, and says what and from where |
| Step | `debug.singlestep {cpu}`: one instruction of that processor, the others running beside it; `debug.singlestep {frame: true}`: to the next frame (the vertical blank) |
| See what ran | `debug.trace {cpu, count}`: the last instructions (up to 1024), disassembled |
| Bugs that work by chance | `debug.diagnostics`: reads of Tom addresses no register answers (a write-only register such as INT2), each with the instruction; `emu.status` counts them |
| Pads | `input.set {buttons, port}` (held until changed), `input.press {buttons, port, frames}` (held, released, run), `input.clear`, `input.get_state`; with `--m68k` |
| Window | `emu.pause`, `emu.resume`: the program on screen stops running by itself (a stop pauses it too) |

A run answers `reason`: `completed`, `halted` (a RISC program's processor cleared GO), `timeout`
(`emu.run_frames` saw too few frames), or `breakpoint` / `watchpoint` / `step` with a `stop` saying
which processor, where and at which cycle; `debug.singlestep {frame: true}` answers `frame`. A step
of a processor that cannot run (GO clear, or a 68000 STOP no interrupt ends) answers `halted` at
once. The run that goes on executes the instruction stopped at; a
breakpoint in a loop stops every run within it, so remove it to run on.

While a GPU or DSP has breakpoints or watchpoints it runs on its stepped core: slower, the same
cycles. With the 68000, a stop in RISC code that shares the bus with the 68000 may say
`"exact": false`: the RISC ran on to the end of the 68000's instruction, and the stop carries its
registers as they were at the breakpoint.

### Finding an address to stop at

Run some frames and read `debug.trace` for the processor (`cpu=gpu`, `cpu=dsp`, `cpu=m68k`) to see
the code that is running, then `cpu.disassemble` around it. The GPU's code runs from its RAM at
0xf03000, the DSP's from 0xf1b000. `scripts/jagmcp.py` prints a disassembly as a listing (address,
bytes, text); `--json` before the method gives the JSON.

## Things that trip people up

- Time is counted in DSP cycles: 26,593,900 a second, about 532,000 a PAL frame.
- `video.screenshot` without `path` returns base64; give a path and Read the file to see it.
- Memory reads in the console's byte order, big-endian.
- A RISC's `pc` is the instruction issuing or about to; MOVEI takes 6 bytes.
- The emulator is slower than real time on some programs (a 68000 game with the bus busy): a
  long `emu.run_frames` holds the other requests back, so run in steps of a few hundred frames.
- A 68000 program whose picture runs but whose 68000 does nothing may have locked the bus: see
  `bus_wait` in `emu.status`, which says which master keeps it (an object list that never ends
  keeps it for the object processor).
- A program that hangs or misbehaves only here may read a write-only register. Check
  `debug.diagnostics`: the console gives MEMCON1 there, and other emulators may not.
- Writing a RISC's local RAM while it runs drops what the timing automaton knew of the code there;
  it is correct, only slower for a while.
