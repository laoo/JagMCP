# The control protocol

This document specifies how a client controls the emulator: what it sends, and what it gets back.
Why there is one protocol, and why it is the same as PGMEmu's, is in
[0018](../decisions/0018-one-control-api.md).

There is one set of methods (§5). Two protocols carry them:

- **JSON lines**, in the format of §2 and §3, on stdio or TCP.
- **MCP** (Model Context Protocol), where each method is a tool, on stdio or HTTP.

A method takes the same parameters and gives the same result in both.

## 0. Quick start

Start a server on TCP, then send JSON lines from a shell. All connections control the same
emulator.

```sh
jagmcp-cli --server tcp:7710 --m68k --bios BIOS.rom --cart GAME.j64 &
printf '%s\n' '{"id":1,"method":"emu.run_frames","params":{"count":250}}' \
               '{"id":2,"method":"video.screenshot","params":{"path":"shot.png"}}' | nc -w 60 127.0.0.1 7710
```

```json
{"id":1,"ok":true,"result":{"reason":"completed","cycles_executed":145380614,"frames_executed":250,"running":true}}
{"id":2,"ok":true,"result":{"width":690,"height":283,"frame":250,"path":"shot.png"}}
```

These answers come from a cartridge booted with the BIOS. Other programs give other numbers.

`scripts/jagmcp.py` sends one request per call. It prints the `result` as indented JSON. On an
error, it prints the error on stderr and exits with status 1. It prints `cpu.disassemble` as a
listing, one instruction per line: the address and the bytes in hex, then the text. `--json`,
before the method, prints the JSON instead.

```sh
scripts/jagmcp.py --port 7710 emu.run_frames count=250
scripts/jagmcp.py --port 7710 memory.read address=0x4000 size=16
scripts/jagmcp.py --port 7710 cpu.disassemble cpu=m68k address=0xe00008 count=2
```

```
0xe00008  41f9 00f0 0000  lea     $f00000.l, A0
0xe0000e  30bc 1861       move.w  #$1861, (A0)
```

For an AI agent, use MCP. On stdio, the client starts the server itself. The window serves MCP on
HTTP, and the client connects to it.

```sh
claude mcp add jagmcp -- /path/to/jagmcp-cli --mcp                     # Claude Code, stdio
jagmcp --mcp-http 7711 &                                                # the window, then
claude mcp add --transport http jagmcp http://127.0.0.1:7711/mcp
```

Other MCP clients take a configuration like this one:

```json
{"mcpServers":{"jagmcp":{"command":"/path/to/jagmcp-cli","args":["--mcp"]}}}
```

## 1. Transports

### JSON lines on stdio

`jagmcp-cli --server` reads requests on stdin and writes responses on stdout.

- Each request is one line with one JSON object. Each response is one line with one JSON object.
- Responses are written in the order of the requests. Each response is flushed.
- Blank lines are skipped. They get no response.
- A line can end with `\n` or `\r\n`. There is no limit on its length. A newline inside a string
  must be escaped, as JSON requires.
- stdout carries only responses. The server writes one line about itself to stderr when it
  starts.
- The server stops when stdin ends.

### JSON lines on TCP

`jagmcp-cli --server tcp:PORT` serves the same lines on TCP. It listens on 127.0.0.1 only. It runs
until the process is stopped.

- Any number of connections can be open.
- Each connection has its own stream of lines. Its responses come in the order of its requests.
- A connection ends when the client closes it.
- All connections control **the same** emulator. A program that one client loads is the program
  that all clients see.
- Requests from all connections are answered one at a time. Each request is finished before the
  next one starts.

This lets a script or an agent keep one emulator across many short commands, as
`scripts/jagmcp.py` does.

There is no authentication. Anyone who can open a connection on this machine controls the
emulator. They can also make it read and write files, by the paths they give to `emu.load`,
`video.screenshot`, `video.record_start` and `audio.capture_start`. `video.record_start` also runs
ffmpeg, but a request cannot say which program that is ([0031](../decisions/0031-video-is-recorded-by-ffmpeg.md)).

### JSON lines from the window

`jagmcp --server tcp:PORT` serves the same lines from the window, while it is open. Requests from
all connections, and loads made in the window, are handled in turn on the emulation thread,
between two parts of the run. They act on the program on screen. The sound of a run made on
request is not played.

### MCP on stdio

`jagmcp-cli --mcp` serves the Model Context Protocol on stdio. Messages are JSON-RPC 2.0, one per
line, framed like the JSON lines above.

- The server answers `initialize`, `ping`, `tools/list` and `tools/call`.
- `initialize` gives the protocol revision the client asks for, if it is one of `2025-11-25`,
  `2025-06-18`, `2025-03-26` or `2024-11-05`. Otherwise it gives `2025-11-25`. The only capability
  is `tools`. The server's name is `jagmcp`, and its version is the one `emu.status` gives. The
  `instructions` are a short paragraph on how to begin.
- A message without an `id` is a notification. It gets no answer. `notifications/initialized` is
  one of these.
- Batches (JSON arrays) are not supported.
- Each method in §5 is a tool. The tool's name is the method's name with `_` in place of `.`:
  `emu.run_frames` becomes `emu_run_frames`. The tool's description and `inputSchema` are the
  method's. The tool's `arguments` are the method's `params`.
- A tool's answer is a `content` array. It holds the method's `result` as JSON text. For a
  screenshot, the PNG comes first, as an `image` of type `image/png`, and it is removed from the
  text.
- When a method fails, the tool result has `isError` set. Its one text is `CODE: message`, with
  the codes of §4.

Only these cases are JSON-RPC errors:

| Code | When |
|---|---|
| -32700 | The line is not JSON. The answer has `id` `null`. |
| -32600 | Not a JSON-RPC 2.0 request: not an object, `jsonrpc` is not `"2.0"`, or `method` is missing or not a string. |
| -32601 | A method other than the four above. |
| -32602 | `tools/call` without a `name`, or with a name that is not a tool. |

### MCP on HTTP, from the window

`jagmcp --mcp-http PORT` serves MCP's Streamable HTTP transport from the window, at
`http://127.0.0.1:PORT/mcp`.

- Each message is the body of a POST.
- The answer to a request is the body of the response, as `application/json`.
- A notification gets `202 Accepted`, with no body.
- A body that is not JSON gets `400`, with the -32700 error.
- The server does not stream. A GET gets `405`.
- The server does not issue an `Mcp-Session-Id`. All clients control the same emulator, as on TCP.
- A request with an `Origin` header that is not a page on `localhost` or `127.0.0.1` gets `403`. A
  request without `Origin` (from a program, not a browser) is served.

```sh
curl -s http://127.0.0.1:7711/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"emu_status","arguments":{}}}'
```

The window and its servers act on the program on screen. When a client loads a program, it
replaces the program in the window. When a file is dropped onto the window, it replaces what the
clients see.

### A program at start

Each server can be given a program after its flags. It is loaded before the first request, in the
same way as `emu.load` loads it:

```sh
jagmcp-cli --mcp --m68k --bios BIOS --cart GAME.j64
```

### Batch

`jagmcp-cli --batch FILE` answers the requests in FILE, one per line, and prints the answers on
stdout. A line that starts with `#` is a comment. With `--mcp`, the lines are MCP messages.

With `--expect ANSWERS`, the answers are compared line by line with the lines of ANSWERS, instead
of being printed. The first difference is printed. The exit status is 0 when all lines are equal.

A recorded transcript holds the version as `devel`. In the comparison, the build's own version is
read as `devel`. So any build checks a transcript in the same way.

## 2. Requests

```json
{"id":1,"method":"emu.status","params":{}}
```

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | A non-negative integer. The response repeats it. |
| `method` | yes | The name of the method. |
| `params` | no | An object. If it is left out, the method gets `{}`. `null` is not an object. |

Other fields are ignored. Parameters that a method does not use are ignored.

Names are case-sensitive. So are parameter values that are names (`cpu`, `access`, `buttons`).

An integer parameter (an address, a size, a count) can be a JSON integer, or a string of hex
digits after `0x` or `$`: `"0xf03000"`, `"$f03000"`. Answers always give integers.

## 3. Responses

```json
{"id":1,"ok":true,"result":{"version":"devel","program":null}}
{"id":1,"ok":false,"error":{"code":"unknown_method","message":"Unknown method: emu.nonsense"}}
```

- Keys come in the order shown.
- `error.code` is stable, for programs. `error.message` is for people, and can change.
- If the `id` cannot be read, the answer has `id` 0. This happens when the text is not JSON, the
  JSON is not an object, or the `id` is missing, negative or not an integer.
- Every request gets an answer. A malformed request never ends the session.

## 4. Error codes

| Code | Meaning |
|---|---|
| `bad_request` | The request is malformed: not JSON, not an object, `id` or `method` missing or of the wrong type, `params` not an object, or a parameter missing, of the wrong type or out of range. |
| `unknown_method` | There is no method with that name. |
| `load_failed` | `emu.load` got arguments that the runner refuses, or a file that cannot be read or does not fit. The message says which. |
| `not_loaded` | The method needs a machine, and no program is loaded. |
| `no_cpu` | The requested processor is not in this run. Examples: the 68000 without `--m68k`, or the GPU in a DSP program. |
| `invalid_range` | Part of the range cannot be read (or written) without a bus cycle (`memory.read`, `memory.write`). The message gives the first such address. |
| `screenshot_failed` | No frame is complete yet, or the PNG cannot be written. |
| `capture_running` | `audio.capture_start` was called while a capture is running. |
| `capture_not_running` | `audio.capture_stop` was called with no capture running. |
| `capture_failed` | The capture's file cannot be written. |
| `recording_running` | `video.record_start` was called while a record is running. |
| `recording_not_running` | `video.record_stop` was called with no record running. |
| `encoder_missing` | ffmpeg cannot be run. The message says why. |
| `record_failed` | The record's files cannot be written, no frame was completed during it, or ffmpeg failed. The message says which, with the end of ffmpeg's log. |
| `invalid_input` | An `input.` method got a button name it does not know. |
| `cpu_busy` | `cpu.set_registers` on the 68000 while it is inside an instruction: a bus cycle waits for the bus. |

## 5. Methods

Time is counted in DSP cycles, as the runner counts them: 26,593,900 per second (PAL). 50 frames of
video are one second.

| Method | What it does |
|---|---|
| [`emu.status`](#emustatus) | The version, the program loaded, and where it is. |
| [`emu.load`](#emuload) | Load a program or a cartridge, from reset. |
| [`emu.reset`](#emureset) | Load the last program again. |
| [`emu.pause`, `emu.resume`](#emupause-emuresume) | Stop and restart the window's own run. |
| [`emu.run_cycles`](#emurun_cycles) | Run a number of cycles. |
| [`emu.run_frames`](#emurun_frames) | Run until a number of frames are complete. |
| [`debug.breakpoint.add`, `.remove`, `.list`](#debugbreakpointadd-debugbreakpointremove) | Stop a run before an instruction. |
| [`debug.watchpoint.add`, `.remove`, `.list`](#debugwatchpointadd-debugwatchpointremove) | Stop a run at an access to memory. |
| [`debug.singlestep`](#debugsinglestep) | Execute one instruction of a processor, or run to the next frame. |
| [`debug.trace`](#debugtrace) | The last instructions executed. |
| [`debug.diagnostics`](#debugdiagnostics) | What the program did that works only by chance. |
| [`input.set`, `input.clear`, `input.get_state`](#inputset-inputclear-inputget_state) | Hold and release the pads' buttons. |
| [`input.press`](#inputpress) | Press buttons for some frames, and run. |
| [`memory.read`, `memory.write`](#memoryread) | Read and write memory without a bus cycle. |
| [`cpu.get_state`](#cpuget_state) | A processor's registers. |
| [`cpu.set_registers`](#cpuset_registers) | Set a processor's registers. |
| [`cpu.disassemble`](#cpudisassemble) | Disassemble memory. |
| [`video.screenshot`](#videoscreenshot) | The last frame, as a PNG. |
| [`video.record_start`, `video.record_stop`](#videorecord_start) | Record the picture and the sound to a video file. |
| [`audio.capture_start`, `audio.capture_stop`](#audiocapture_start) | Record the sound to a WAV file. |

### `emu.status`

No parameters.

```json
{"id":1,"ok":true,"result":{"version":"devel","program":"reference/op_bitmap.o","cpu":"gpu",
  "cycles":20000,"seconds":0.00075,"frame":7,"running":true,"paused":false,"recording":false,
  "diagnostics":0}}
```

```json
{"id":4,"ok":true,"result":{"version":"devel","program":"reference/m68k_op_hog.o","cpu":"m68k",
  "cycles":120199,"seconds":0.0045,"frame":58,"running":true,"paused":false,"recording":false,
  "diagnostics":0,
  "bus_wait":{"cpu":"m68k","pc":16572,"address":16574,"access":"read","since":2487,
  "holder":"object processor"}}}
```

| Field | Meaning |
|---|---|
| `version` | The emulator's version. For a release, it is the tag without its `v` (`1.2.0`). For a build after a tag, it is `git describe`'s answer (`1.2.0-3-gabc1234`), with `-dirty` if there are uncommitted changes. Without a tag, it is `devel`. |
| `program` | The program loaded: its BS94, COFF or raw binary, or the cartridge or OS ROM that it boots from. It is `null` before a program is loaded. Then the other fields are absent. |
| `bios` | The BIOS file the program boots with, as `--bios` gave it. Only when there is one. |
| `cpu` | The processor that the program runs on: `m68k`, `gpu` or `dsp`. |
| `cycles`, `seconds` | How far the program's processor has run since the load. |
| `frame` | The number of video frames completed since the load. |
| `running` | True while the program's processor has not stopped. A RISC program ends when its processor clears GO. A 68000 program does not end. |
| `paused` | True when `emu.pause`, or a debugger stop, has paused the window's own run. |
| `recording` | True while a record (`video.record_start`) is running. |
| `diagnostics` | The number of entries that `debug.diagnostics` gives. |
| `bus_wait` | Only when a 68000 bus cycle still waits for the bus where the last run ended. The run ended on time, and the cycle goes on with the next run. `pc`: the instruction. `address`, `access`: the bus cycle's address, `read` or `write`. `since`: the cycle in which it started. `holder`: the master the bus is granted to, `68000`, `blitter`, `GPU`, `DSP`, `object processor` or `DRAM refresh`. A wait that never ends is a locked console, for example an object list that never ends ([0027](../decisions/0027-a-run-ends-where-asked.md)). |
| `stop` | Where the last run was stopped by a breakpoint, a watchpoint or a single step, in the same form as `emu.run_cycles` gives it. This covers runs on request and the window's own run. It is absent once a run continues. |

### `emu.load`

| Param | Meaning |
|---|---|
| `args` | The arguments, as strings, in the same form that `jagmcp-cli` takes them: the program and its options. A single `.run` file is read in the format of `tests/programs`. |

Loads the program in place of the current one, from reset, and powers the machine on.

- `--cycles` is ignored.
- The runner's outputs (`--frames`, `--wav`, `--log`) are ignored.
- Paths are relative to the server's working directory.

```json
{"id":2,"method":"emu.load","params":{"args":["--m68k","--bios","bios.rom","--cart","game.j64"]}}
{"id":2,"ok":true,"result":{"program":"game.j64","bios":"bios.rom","cpu":"m68k"}}
```

If the load fails, the current program keeps running.

Errors: `load_failed`, `bad_request`.

### `emu.reset`

Loads the last program again, from reset, with the same arguments that `emu.load` used. Answers
`{}`.

Errors: `not_loaded`, `load_failed`.

### `emu.pause`, `emu.resume`

`emu.pause` stops the window's own run. `emu.resume` starts it again. Both answer `{}`.

- Runs on request (`emu.run_cycles`, `emu.run_frames`, `input.press`) work in both states.
- `jagmcp-cli` has no own run. It runs only on request.
- A breakpoint or watchpoint that is hit also pauses the window's run. So does `debug.singlestep`.

### `emu.run_cycles`

| Param | Meaning |
|---|---|
| `count` | The number of DSP cycles to run. |

Runs until `count` more cycles have passed, or until the program's processor stops. A run ends
between instructions, so it can end a few cycles later than asked.

```json
{"id":4,"ok":true,"result":{"reason":"completed","cycles_executed":10000,"frames_executed":0,"running":true}}
```

| Field | Meaning |
|---|---|
| `reason` | Why the run ended. `completed`: all cycles ran. `halted`: the program's processor stopped first, or had already stopped. `breakpoint` or `watchpoint`: a debugger probe stopped it (see `debug.` below). `timeout`: only from `emu.run_frames` and `debug.singlestep`. `step` and `frame`: only from `debug.singlestep`. |
| `cycles_executed` | The DSP cycles that the run took, counted by the program's processor. |
| `frames_executed` | The frames that the run completed. |
| `running` | The same as in `emu.status`. |
| `stop` | Only when `reason` is `breakpoint`, `watchpoint` or `step`. Where the run stopped (below). |

```json
{"id":4,"ok":true,"result":{"reason":"watchpoint","cycles_executed":120,"frames_executed":0,"running":true,
  "stop":{"cpu":"m68k","pc":16434,"cycle":2119,"exact":true,"address":65540,"access":"write","bytes":2,"value":8249}}}
```

| `stop` field | Meaning |
|---|---|
| `cpu` | The processor that hit the probe, or that was stepped: `m68k`, `gpu` or `dsp`. |
| `pc` | For a breakpoint or a step, the instruction that is about to execute. For a watchpoint, the instruction whose access hit it. |
| `cycle` | The processor's cycle at the stop. |
| `exact` | True if the processor stopped exactly there. It is false for a RISC that the 68000 was waiting on in the middle of an instruction. That RISC ran on to the end of the instruction ([0019](../decisions/0019-breakpoints-step-the-risc.md)). |
| `registers` | Only when `exact` is false: the RISC's 64 registers as they were at the stop. |
| `address`, `access`, `bytes`, `value` | Only for a watchpoint: the address, `read` or `write`, the width in bytes, and the value written (`null` for a read). |

Errors: `not_loaded`, `bad_request`.

### `emu.run_frames`

| Param | Meaning |
|---|---|
| `count` | The number of video frames to complete. |
| `timeout_cycles` | The DSP cycles after which to give up. Default: 20 s of console time. |

Answers in the same form as `emu.run_cycles`. `reason` is `timeout` when the time runs out first.
For example, a program that does not set up video makes no frames.

The run is made in parts of 20,000 cycles. It ends with the part in which the last frame is
completed.

Errors: `not_loaded`, `bad_request`.

### `debug.breakpoint.add`, `debug.breakpoint.remove`

| Param | Meaning |
|---|---|
| `address` | The address of an instruction. |
| `cpu` | `m68k`, `gpu` or `dsp`. Default: the program's own processor. |

After `add`, later runs stop before the processor executes the instruction at `address`. A RISC
stops before the instruction issues. The 68000 stops at the instruction boundary. When the run
continues, it executes the instruction.

- The address is cut to 24 bits and rounded down to an even address, like instruction addresses.
- Adding the same address twice keeps one breakpoint.
- Breakpoints and watchpoints are kept by `emu.reset`. They are removed by `emu.load`.

While a GPU or DSP has breakpoints or watchpoints, it runs on its stepped core only. This is
slower, but the timing is the same, cycle for cycle. With the 68000, the machine then runs in steps
of 32 cycles. This way the 68000, which leads, is not further on when a RISC stops
([0019](../decisions/0019-breakpoints-step-the-risc.md)).

Answers `{"cpu", "breakpoints"}` for that processor. The addresses are in the order they were
added.

Errors: `not_loaded`, `no_cpu`, `bad_request`.

### `debug.breakpoint.list`

Lists the breakpoints of every processor in the run: `{"m68k": [...], "gpu": [...], "dsp": [...]}`.
A processor that is not in the run is left out.

Errors: `not_loaded`.

### `debug.watchpoint.add`, `debug.watchpoint.remove`

| Param | Meaning |
|---|---|
| `address` | The first byte watched. `remove` removes all watchpoints that start at this address. |
| `size` | The number of bytes watched, from 1 to 16,777,216. Default: 1. |
| `access` | `write`, `read`, or `access` (both). Default: `write`. |
| `cpu` | `m68k`, `gpu` or `dsp`. Default: the program's own processor. |

After `add`, later runs stop when the processor reads or writes a watched byte.

- A RISC stops when its memory controller has served the load or store. A load from outside its
  local RAM stops before the data has arrived, so the stop has no `value`.
- The 68000 stops after the instruction. Its watched accesses are its data reads and writes, one
  word at a time. Its program fetches are not watched.
- Accesses by the blitter and the object processor are not watched.

Answers `{"cpu", "watchpoints"}` for that processor. Each watchpoint is
`{"address", "size", "access"}`.

Errors: `not_loaded`, `no_cpu`, `bad_request`.

### `debug.watchpoint.list`

Lists the watchpoints of every processor in the run, in the same form as `debug.breakpoint.list`.

Errors: `not_loaded`.

### `debug.singlestep`

| Param | Meaning |
|---|---|
| `cpu` | `m68k`, `gpu` or `dsp`. Default: the program's own processor. |
| `frame` | `true`: run to the next frame instead. It cannot be given with `cpu`. Default: `false`. |
| `timeout_cycles` | The DSP cycles after which to give up. Default: 20 s of console time. |

Without `frame`, the processor executes one instruction: the one at the `pc` that `cpu.get_state`
gives. The run stops before the next instruction.

- The other processors run the same cycles beside it, as on the console. They are not held
  ([0030](../decisions/0030-a-step-runs-the-whole-machine.md)).
- The step stops as a breakpoint does. A RISC runs on its stepped core. With the 68000, the
  machine runs in steps of 32 cycles.
- If a breakpoint or a watchpoint is hit first, the step ends there, with that `reason`.
- A RISC that the 68000 waits on in the middle of an instruction cannot stop. The stop has `exact`
  false, as for a breakpoint. The RISC ran on to the end of the 68000's instruction. The next
  step starts where the RISC is now.
- Nothing runs, and `reason` is `halted`, when the processor cannot execute an instruction: a RISC
  whose GO is clear, a halted 68000, or a 68000 in a STOP whose mask holds back level 2. Level 2 is
  Tom's interrupt, the only one on the console.

With `frame`, the machine runs until the next frame of the video output is complete, as
`emu.run_frames` counts frames. That is where the vertical blank starts (VBB), or where VC wraps
in a field without one. Each processor then stops before its next instruction. The answer has no
`stop`. The vertical blank interrupt (VI) is another event: to stop there, set a breakpoint on its
handler.

The answer has the same form as `emu.run_cycles`. `cycles_executed` is counted by the program's
processor. For a step of another processor, it says how far the program's processor went. The
stepped processor's own cycle is the `cycle` of `stop`.

```json
{"id":5,"method":"debug.singlestep","params":{"cpu":"gpu"}}
{"id":5,"ok":true,"result":{"reason":"step","cycles_executed":6,"frames_executed":0,"running":true,
  "stop":{"cpu":"gpu","pc":15740934,"cycle":6,"exact":true}}}
{"id":6,"method":"debug.singlestep","params":{"frame":true}}
{"id":6,"ok":true,"result":{"reason":"frame","cycles_executed":6354,"frames_executed":1,"running":true}}
```

Errors: `not_loaded`, `no_cpu`, `bad_request`.

### `debug.trace`

| Param | Meaning |
|---|---|
| `count` | The number of instructions, at most 1024. Default: 32. |
| `cpu` | `m68k`, `gpu` or `dsp`. Default: the program's own processor. |

Gives the last instructions that the processor executed since the load, oldest first. Each is
`{"pc", "cycle", "text"}`:

- `cycle` is the cycle in which a RISC instruction issued, or in which a 68000 instruction began.
- `text` is disassembled from memory as it is now.

Errors: `not_loaded`, `no_cpu`, `bad_request`.

### `debug.diagnostics`

No parameters.

```json
{"id":1,"ok":true,"result":[{"kind":"undriven_read","cpu":"m68k","pc":278,
  "text":"move.w  ($2,A0), D0","address":15728866,"register":"INT2","value":6245,
  "cycle":14315818,"count":1}]}
```

Gives what the program did since the load that works only by chance, in the order it was first
done. There is one kind:

- `undriven_read`: a read of an address in Tom's space ($F00000 to $F01FFF) that no register
  answers, such as a write-only register. The value is what the bus held: MEMCON1 on the console
  ([0026](../decisions/0026-undriven-tom-reads-give-memcon1.md)). Other consoles or emulators may
  give another value.

Each entry is one processor, one instruction and one address:

| Field | Meaning |
|---|---|
| `kind` | `undriven_read`. |
| `cpu` | `m68k`, `gpu` or `dsp`. |
| `pc`, `text` | The instruction that read, disassembled from memory as it is now. |
| `address` | The word read. |
| `register` | The register's name in the Software Reference, or `null`. |
| `value` | The value read, the first time. |
| `cycle` | The cycle of the first read. |
| `count` | How many times the instruction read there. |

At most 64 entries are kept.

Errors: `not_loaded`.

### `input.set`, `input.clear`, `input.get_state`

| Param | Meaning |
|---|---|
| `buttons` | An array of lowercase names: `up`, `down`, `left`, `right`, `a`, `b`, `c`, `option`, `pause`, `0` to `9`, `star`, `hash`. `[]` releases all buttons. |
| `port` | 1 or 2. Default: 1. `input.clear` without `port` releases both pads. |

- `input.set` holds the given buttons on the pad, and releases the others. They stay this way
  until changed.
- `input.clear` releases the buttons.
- `input.get_state` gives the buttons held on each pad.

The program reads the buttons through Jerry's JOYSTICK and JOYBUTS, as the MiSTer core's two
plain pads give them ([hardware/main-bus.md](../hardware/main-bus.md)). This works with `--m68k`
only. The buttons are kept across loads.

All three answer in the same form:

```json
{"id":10,"ok":true,"result":{"port1":["up","a"],"port2":[]}}
```

Errors: `invalid_input`, `bad_request`.

### `input.press`

Takes `buttons` and `port`, as `input.set` does, and `frames` (default: 2).

1. Holds the buttons for `frames` frames.
2. Releases them.
3. Runs `frames` more frames.

Other buttons that were held stay held. The answer has the same form as `emu.run_frames`, for the
whole press.

- Each half gives up after 20 s of console time without its frames (`reason` `timeout`).
- If a probe is hit in the first half, the press ends there, with the buttons released.

Errors: `not_loaded`, `invalid_input`, `bad_request`.

### `memory.read`

| Param | Meaning |
|---|---|
| `address` | The address of the first byte, in the 24-bit address space. |
| `size` | The number of bytes to read, at most 1,048,576. |

Reads without a bus cycle. So a read never triggers what a read of a register would trigger. It
can read:

- main RAM (below $400000, mirrored);
- the OS ROM and the cartridge, where the board maps them;
- the GPU's and the DSP's local RAM.

It does not read registers.

```json
{"id":5,"method":"memory.read","params":{"address":"0x10000","size":4}}
{"id":5,"ok":true,"result":{"address":65536,"data_hex":"0000000a"}}
```

`data_hex` has two lowercase hex digits per byte, in address order.

Errors: `not_loaded`, `invalid_range`, `bad_request`.

### `memory.write`

| Param | Meaning |
|---|---|
| `address` | The address of the first byte. |
| `data_hex` | The bytes, two hex digits each. |

Writes without a bus cycle, to main RAM and the local RAMs. It cannot write the ROMs.

- The whole range is checked first. So the write happens completely or not at all.
- A write into a RISC's local RAM clears what the timing automaton knew about the code there.

Answers `{"address", "size"}`.

Errors: `not_loaded`, `invalid_range`, `bad_request`.

### `cpu.get_state`

| Param | Meaning |
|---|---|
| `cpu` | `m68k`, `gpu` or `dsp`. Default: the program's own processor. |

For the GPU or the DSP:

```json
{"id":6,"ok":true,"result":{"pc":15741038,"flags":321404928,"control":321396737,
  "registers":[0,0,"..."],"bank":0,"started":true,"cycle":20000,"disasm":"loadw (r6),r8"}}
```

| Field | Meaning |
|---|---|
| `pc` | The program counter as the core holds it: the instruction that is issuing or about to issue. |
| `flags`, `control` | G_FLAGS/D_FLAGS and G_CTRL/D_CTRL, as a read of them gives them. |
| `registers` | The 64 registers: bank 0, then bank 1. |
| `bank` | The bank that the instructions name: 1 when REGPAGE selects bank 1 and IMASK does not override it. |
| `started` | True when GO has been set, by the program's start or by another processor's write. |
| `cycle` | The cycle that the processor has reached. |
| `disasm` | The instruction at `pc`. |

For the 68000:

| Field | Meaning |
|---|---|
| `pc` | The address of the next instruction to execute. |
| `d`, `a` | The data and address registers. A7 is the stack pointer in use. |
| `sr`, `usp`, `isp` | The status register and the two stack pointers. |
| `stopped` | True when a STOP waits for an interrupt. |
| `halted` | True when the 68000 has halted on a double fault. |
| `disasm` | The instruction at `pc`. |

Errors: `not_loaded`, `no_cpu`, `bad_request`.

### `cpu.set_registers`

Sets registers of a processor between runs. Answers the processor's state after the write, as
`cpu.get_state` gives it.

| Param | Meaning |
|---|---|
| `registers` | An object: register names and their values. A value is an integer, or a string such as `"0x10"`. |
| `cpu` | `m68k`, `gpu` or `dsp`. Default: the program's own processor. |
| `bank` | GPU and DSP only: the bank that `r0` to `r31` name, 0 or 1. Default: the bank in use (`bank` of `cpu.get_state`). |

```json
{"id":7,"method":"cpu.set_registers","params":{"cpu":"gpu","registers":{"r1":"0x10000","flags":0}}}
{"id":8,"method":"cpu.set_registers","params":{"cpu":"m68k","registers":{"d0":1,"pc":"0x4032"}}}
```

The registers of the GPU and the DSP:

- `r0` to `r31` of the bank, and `flags`: Z, C and N, bits 0 to 2 of FLAGS (0 to 7).
- The PC cannot be set. The pipeline already holds the instructions after it.
- The new value is there from now on. An instruction that has already issued, but has not
  written its result yet, still writes it after. Examples: a load, a divide, the second word of
  a MOVEI.

The registers of the 68000:

- `d0` to `d7`, `a0` to `a7`, `pc`, `sr`, `usp` and `isp`. `a7` is the stack pointer in use.
- `sr` is set first, because it can exchange the stack pointers.
- `pc` must be even. The next instruction starts there. Its first words are read from memory
  without a bus cycle. A STOP ends.
- Between runs the 68000 is between two instructions, unless a bus cycle waits for the bus
  (`emu.status` shows it as `bus_wait`). Then nothing is set, and the answer is `cpu_busy`.

All the names and values are checked first. So all the registers are set, or none.

Errors: `not_loaded`, `no_cpu`, `cpu_busy`, `bad_request`.

### `cpu.disassemble`

| Param | Meaning |
|---|---|
| `address` | Where to start. |
| `count` | The number of instructions, at most 1000. |
| `cpu` | `m68k`, `gpu` or `dsp`. Default: the program's own processor. The GPU and the DSP differ in a few opcodes. |

Answers an array of `{"address", "length", "opcode_hex", "text"}`, one per instruction.
`opcode_hex` is the instruction's `length` bytes, in the same form as `data_hex` of
`memory.read`.

- Memory is read as `memory.read` reads it. A byte where there is no memory reads as 0.
- A RISC's MOVEI is 6 bytes long.
- Code for either RISC can be disassembled, whatever runs. `no_cpu` is only for the 68000,
  without `--m68k`.

Errors: `not_loaded`, `no_cpu`, `bad_request`.

### `video.screenshot`

Gives the last complete frame of the video output, in the same form as the runner's `--frames`
writes it. It is what is shown outside the blanking. Each pixel of the frame is one pixel of the
image. (The window shows it at 4:3, [0017](../decisions/0017-the-picture-is-shown-4-3.md).)

| Param | Meaning |
|---|---|
| `path` | Where to write the PNG (RGB pixels). If it is left out, the PNG comes back in the answer, in base64. |

```json
{"id":7,"ok":true,"result":{"width":690,"height":283,"frame":100,"path":"yarc.png"}}
{"id":8,"ok":true,"result":{"width":85,"height":7,"frame":7,"png_base64":"iVBORw0..."}}
```

- `frame` is the number of frames completed since the load.
- The path is relative to the server's working directory. An existing file is replaced.

Errors: `not_loaded`, `screenshot_failed`, `bad_request`.

### `video.record_start`

Records the video output and the sound from now on into a video file. ffmpeg encodes it
([0031](../decisions/0031-video-is-recorded-by-ffmpeg.md),
[0032](../decisions/0032-video-colours-are-ycbcr-by-bt709.md)). The file is written by
`video.record_stop`.

| Param | Meaning |
|---|---|
| `path` | Where to write the video. Its extension chooses the container: `.mp4`, `.mkv`, `.mov`. |
| `lossless` | `true`: keep the RGB pixels exactly, in H.264 4:4:4 of 10 bits. QuickTime, ffmpeg, VLC and mpv show it; browsers may not. `false` when left out. |
| `crisp` | `true`: make each pixel a block of 2 by 2 pixels. `true` when left out. |

Answers `{"path"}`.

- The picture is H.264, the sound AAC at 48 kHz.
- The colours are YCbCr by BT.709, limited range, and the file says so. ffmpeg gives back a
  lossless record's exact frames with
  `-vf scale=in_color_matrix=bt709:in_range=limited:flags=accurate_rnd+full_chroma_int+bitexact,format=rgb24`;
  without these flags, colours come 1 apart.
- H.264 keeps colour at half the resolution in each direction, and it blurs single pixels. With
  `crisp` a pixel is a whole block of colour, and the video is much closer to the frames: on a
  cartridge's title, 44 dB against 35 dB without it, for 1.5 times the size.
- The video's size is the first frame's, made even, doubled with `crisp`. Later frames of another
  size are cut or padded with black, at the right and at the bottom. The file tells the player to
  show it 4:3, as the window does ([0017](../decisions/0017-the-picture-is-shown-4-3.md)). The
  pixels are not resampled to 4:3.
- The video's frame rate is the rate the program's frames come at. Where it changes for three
  frames running, a new part of the video begins at the new rate. Native, for example, starts at
  50.08 Hz in the BIOS and goes on at 59.99 Hz.
- Each frame and each sound sample is placed by the cycle it was made in. Where no frame was
  completed (the video was off), the frame before is repeated, or black before the first. Where the
  I2S made no sound, the sound is silent.
- The record goes on across `emu.load` and `emu.reset`.
- The path is relative to the server's working directory. An existing file is replaced.
- ffmpeg is the program `JAGMCP_FFMPEG` names, or `ffmpeg` on the PATH. Beside `path`, the record
  keeps `PATH.videoN.nut` (a part each), `PATH.sound.pcm` and `PATH.log` while it runs.

Errors: `not_loaded`, `recording_running`, `encoder_missing`, `record_failed`, `bad_request`.

### `video.record_stop`

Ends the record, writes the file, and answers what it holds.

```json
{"id":9,"ok":true,"result":{"path":"native.mp4","crisp":true,"width":1380,"height":568,"frame_rate":59.990209878548356,
  "segments":2,"frames":3028,"repeated":25,"dropped":2,"sample_rate":48000,"seconds":53.22579426108995}}
```

| Field | Meaning |
|---|---|
| `crisp` | As `video.record_start` was given it. |
| `width`, `height` | The video's size, doubled with `crisp`. |
| `frame_rate` | Its frames per second, in the part that holds the most frames. |
| `segments` | The parts of the video, at a frame rate each. |
| `frames` | Its frames. |
| `repeated` | Of them, the frames that repeat the one before (or are black), as no frame was completed in time. |
| `dropped` | Frames completed that are not in the video: another frame was completed in the same frame of the video, for example while the rate changes. |
| `sample_rate` | The sound's samples per second. |
| `seconds` | How long the record is, in console time. |

On failure the file is removed. `PATH.log` is kept when ffmpeg failed.

Errors: `recording_not_running`, `record_failed`.

### `audio.capture_start`

Records the I2S output from now on, into a WAV file: 16-bit stereo, one sample per I2S frame, at
the rate the frames come. Nothing is resampled. The file is written by `audio.capture_stop`.

| Param | Meaning |
|---|---|
| `path` | Where to write the WAV file. |

Answers `{"path"}`.

Errors: `not_loaded`, `capture_running`, `capture_failed`, `bad_request`.

### `audio.capture_stop`

Ends the capture, writes the file, and answers what it holds.

```json
{"id":9,"ok":true,"result":{"path":"sound.wav","frames":639277,"sample_rate":31964}}
```

Errors: `capture_not_running`, `capture_failed`.
