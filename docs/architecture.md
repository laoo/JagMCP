# Architecture

This document describes how JagMCP is built: which parts of the hardware are modelled, at what
level, and how they fit together. The reasons for each choice are in [decisions/](decisions/).
What each port leaves out is in [hardware/](hardware/).

## The RISC core

The DSP in Jerry and the GPU in Tom are one core, `Risc`. In the netlist they are also one design
with a parameter. The GPU differs from the DSP in these points:

- It has its own microcode for six opcodes: SAT8, SAT16, SAT24, PACK and UNPACK, LOADP and
  STOREP. The DSP has SUBQMOD, SAT16S, SAT32S, MIRROR and ADDQMOD in their place.
- It has Tom's unsigned saturation and the pack units behind these opcodes.
- Its local space is $F02000 to $F07FFF: 4 KB of RAM at $F03000, and its registers at $F02100.
- Its interrupt vectors are at $F03000.

The datapath is instantiated once per chip, so neither chip pays for the other's opcodes.

A LOADP or STOREP in the local space moves a long word. G_HIDATA holds the other half of the
phrase.

MMULT is a sequence that the decode stage takes instead of the words in the queue (`systolic.v`):
IMULTN, then IMACN until the width of the matrix is used up, then RESMAC. The vector is read from
bank 1, half a register at a time. Each matrix word is read from local RAM through the data
request, in the cycle its instruction issues, by row or by column from D_MTXA.

The core is split by the size of the netlist's state. Where the state is small, it is stepped
every cycle. Where it is not, it is computed when an instruction issues
([0003](decisions/0003-risc-by-instruction.md), [0006](decisions/0006-front-end-stepped-by-cycle.md)).

### Stepped every cycle

- **The front end** is stepped every DSP cycle. It is:
  - the instruction queue of `prefetch.v`, as its registers (`insdata`, `pfdata`: the words as
    they were fetched). Its head is read even when the queue is empty.
  - the fetch address of `pc.v`;
  - the issue and MOVEI states of `execon.v`;
  - the jump latches;
  - the fetch requests of `gpu_mem.v`.

  A stall in which only the cycle count can change is counted out at once, up to the cycle that
  the scoreboard gives. In such a stall the queue is full, nothing is under way, and the I2S
  interface is waiting for a clock edge.

  The instruction in the decode stage issues in the cycle in which `exe` is set. The scoreboard
  checks the decode stage's microword in every cycle, valid or not. A wait that it finds there
  holds the decode stage back.

- **Interrupts insert instructions.** `interrupt.v` latches the requests that its enables let
  through. When nothing makes it wait, the decode stage takes an eight-word sequence instead of the
  queue's words (`intser`, `promold`): push the return address on R31, then jump to the vector.
  These make it wait (`atomic`): a MOVEI and its data words, a run of IMULTN and IMACN, and a jump
  that is about to be taken or is at the head of the queue.

  Jerry's I2S interface (`I2s`, `j_i2s.v`) runs on the same clock. It raises its interrupt at the
  word select edges that SMODE selects.

- **The data side of the memory controller** is:
  - the load and store states of `sboard.v`: `idle`, `comp1` and `comp2` of the indexed modes,
    and `loading`;
  - `compdwait` of `execon.v`: an indexed store reads its data one cycle after it issues;
  - the data request of `gpu_mem.v`. It takes the local bus from the prefetcher for the cycle in
    which it is made.

  Its timing:
  - A load or store is served in the cycle after its request. The loaded value takes a write port
    in the cycle after that, or later if earlier sources in the scoreboard hold both ports.
  - A load or store is served at the start of its cycle, before that cycle's instruction issues.
    This is because what it writes reaches that instruction's ALU cycle.
  - A load or store outside the local space is handed to the gateway when it is served (see
    [The main bus](#the-main-bus)). Its value is written back when the main bus brings it. Up to
    two such loads can be under way. No load or store issues while the gateway is busy. No store
    issues while an external load is under way.
  - An indexed load or store has the ALU compute its address into `resreg` in the cycle after it
    issues. So right after an IMACN, the address is the accumulated sum.

  The local space is 32 bits wide. A byte or word access to it moves the whole long word. The wave
  table ROM reads as sign-extended words.

- **The flag registers and the divider:**
  - An instruction's flags are loaded at the end of the cycle after it issues.
  - A write to D_FLAGS takes effect at the end of the cycle in which it is served. It wins over the
    flags loaded in the same cycle. The carry of the instruction that issues then sees it.
  - The divider (`divider.v`) computes two quotient bits per cycle, for sixteen cycles. D_REMAIN
    reads its working register as it is.
  - The quotient takes a write port only next to an ALU, move or internal load write
    (`div_wbacks`).
  - A read of D_FLAGS or D_CTRL shows the pipeline in its upper half (`dbgrd`): the decode stage's
    opcode, the queue size, `sbwait`, `div_activei` and `jump_ena`.

### Computed when an instruction issues

- **The scoreboard is a set of times.** Whether the instruction can issue in a cycle (`sbwait`) is
  decided from the cycle in which each register and the flags become ready, and from the register
  file writes of that cycle. It waits for:
  - an operand that is still to be written (`aludwait`);
  - the flags of the instruction just before (`flagwait`);
  - two writes in the cycle (`mult_wback`);
  - one write, when the instruction needs both register file ports for its own operands
    (`wbwait`, `bothen`);
  - a load or store under way, when the instruction is another load or store or names its
    register (`ldwait`);
  - the divider's result register, while it divides (`divdwait`, `diviwait`).

  The indexed modes have their source field compared as a register, even where it is an offset.
  Register addresses use the bank (REGPAGE) of the cycle in which they issue.

- **Write-back is a calendar** of cycles. It allows at most two writes per cycle (the two ports),
  in the scoreboard's priority: ALU, move, MOVEI, internal load, external load, divide. It decides
  the waits above. It also decides which of two writes to one register survives (`rd64x32.v`,
  `dupwrite`), and what a destination operand holds when the instruction does not read it.

- **The datapath runs at once** when an instruction issues. It is `arith.v`, bit for bit, driven by
  the instruction's microword from the DSP's microcode ROM (`ra6032a.v`). It has the source operand
  (`srcdgen.v`), the adder (`alu32.v`), the barrel shifter (`brlshift.v`, `barrel32.v`), the
  multiplier, saturation (`j_saturate.v`), NORMI and MIRROR, the result multiplexer and the flags.

  The register file is written from the result register, `resreg`. Only instructions with `resld`
  load it; RESMAC writes what it holds. An IMACN's product is accumulated in the ALU cycle of the
  next instruction that issues (`macop_pp`), by that instruction's ALU function. When no
  instruction issues, it is accumulated by the decode stage's microword.

The code says, next to each rule, which netlist signal it follows.

The state that the datapath keeps between instructions is stored so that it does not need
stepping:

- The flags are kept as the value visible now and the values still to come, each with the cycle
  from which it is visible. They are committed when they are read.
- The divider is kept as its operands and the steps still to take. The steps are taken when the
  remainder or the quotient is read.
- The I2S interface is advanced by the toggles of its serial clock, not by the cycle. The receiver
  moves only on a rising clock. Nothing moves while the interface is disabled. Its generator is
  caught up before the next write.

## The main bus

The path between a RISC and main RAM is stepped by cycle, as the netlist's flip-flops and gates
([0008](decisions/0008-main-bus-by-cycle.md)). This is because the time an access takes depends on
all of it:

- the RISC's gateway;
- for the DSP, Jerry's bus master. It is 16 bits wide, so a long word takes two bus cycles.
- for the GPU, nothing between its gateway and Tom's own bus. That bus is 64 bits wide, so a
  phrase takes one cycle. The grant is latched in the arbiter.
- Tom's arbiter. It must ask the 68000 for the bus every time.
- the 68000's answer, at its half-rate clock;
- Tom's memory controller, with its DRAM, ROM and I/O states and their wait states;
- the open DRAM rows;
- the refresh. It takes the bus for eight cycles whenever eight are due.

Jerry's timers are registers on the bus. They raise the DSP's interrupts.

How the bus is modelled:

- **The bus is a component of its own**, `MainBus`. A RISC clocks it in every cycle in which one of
  the RISC's requests is under way. This happens after the memory controller has served the cycle's
  access, and before the interrupt requests are latched.

  When the RISC has nothing under way, the bus is left behind. It is brought forward when the RISC
  next hands it a request. At rest, only the refresh counters and the 68000's clock phase change,
  and they are moved by arithmetic. A rest ends at the next cycle for which a RISC has handed the
  bus something: a request, a write to the blitter's registers, or an interrupt to the 68000.

- **Data is moved at once**, in the cycle in which the netlist moves it, not bit by bit through the
  latches:
  - A load's data moves when Jerry starts its bus cycle.
  - A store's data moves into Jerry's data latch in the cycle after. It comes from the bus, not
    from the gateway, if the grant is withdrawn in that cycle.
  - Data moves into DRAM when Tom strobes CAS.
  - A timer read moves when Tom's I/O cycle latches it.

- **The GPU can fetch its program from outside its local space**, from main RAM or the cartridge.
  A cartridge's GPU code that the BIOS boots can run this way. The fetch is a long-word cycle of
  the gateway, as in `gpu_mem.v` and `gateway.v`:
  - It queues behind a load or store that is under way, and they queue behind it.
  - Its address is kept apart from theirs (`cpu_addr`).
  - Its long word is taken in the cycle after the cycle starts (`prog_data`). It is handed to the
    prefetch queue in the cycle after that (`gatereq`), when it takes the GPU's local bus.
  - A jump in the meantime leaves the word untaken (`xpabort`).

- **For a RISC program, the 68000 runs the reference boot program.** It ends its write of DSPGO
  or GPUGO, prefetches one word from ROM and stops. It answers bus requests through its
  arbitration logic. The core starts from the console's state at GO. The fixed boot program makes
  this state the same for every program. A 68000 program starts at reset instead (see
  [The 68000](#the-68000)).

- **Jerry's timers** are kept as counters at the start of a cycle, and brought forward by
  arithmetic. Their requests are the carries of the dividers.

- **The bus's control is replayed** ([0020](decisions/0020-the-bus-replays-its-control.md)). Its
  flip-flops are one value, and a cycle's inputs are a key. The next state, and what it tells the
  data side, are recorded the first time and replayed after that. The data is moved live. A 68000
  bus cycle with nothing else working on the bus is replayed whole, in one step
  ([0021](decisions/0021-a-68000-bus-cycle-in-one-step.md)).

What the port leaves out is in [hardware/main-bus.md](hardware/main-bus.md).

### Tom's registers

`Tom` holds Tom's video time base, its PIT and the 68000's interrupt control. They are reached
through Tom's internal I/O cycles on the bus, one word at a time. They are written when the cycle
strobes, and read when it latches.

They are kept in the same way as Jerry's timers:

- HC and VC are kept at the start of a cycle, and brought forward by arithmetic. HC counts to HP
  and wraps every half line. VC counts half lines to VP.
- The next vertical interrupt is computed from them.
- INT1's requests are taken when the reads and writes reach the cycles in which they rise.

The PIT is Jerry's timer circuit, but its prescaler register is clocked instead of latched. It
interrupts the GPU (`gpu_irq[2]`), as Jerry's timers interrupt the DSP.

When a request is pending in INT1, the 68000's interrupt handler gets the bus before the GPU, as
in the arbiter.

What the port leaves out is in [hardware/tom.md](hardware/tom.md).

### The video output and the object processor

Tom's video is stepped by cycle ([0010](decisions/0010-video-by-cycle-frames-as-references.md)).

The object processor is a master on Tom's bus. It is stepped with the bus while the time base
runs. At the start of each displayed line, it:

1. walks its list, one phrase at a time;
2. writes the pixels of the objects on the line into the line buffer that is not shown;
3. writes each header back;
4. starts a refresh at its stop object.

Between lines, when the list is walked and nothing else is under way, the bus rests until the
start of the next line.

The video output reads the other line buffer, one pixel at a time, at the pixel width. It converts
CRY or RGB, and clears what it read to the background colour. It produces the MiSTer core's
output: what is shown at each pixel enable outside the blanking, collected into frames.

The video output is brought forward:

- to each write of Tom's registers;
- to each cycle in which the object processor works;
- while it waits for a line: to the start of the line, and to each change of the data bus it reads;
- by arithmetic, while the time base is stopped.

**The object processor's interrupt.** A GPU with this interrupt enabled steps the bus with itself,
from the first cycle in which the interrupt can come. (The DSP does the same while the GPU
accesses Jerry.) The interrupt is a one-cycle pulse that the bus makes. That cycle is:

- while the object processor waits: the start of the next line (HC at HDB1 or HDB2, computed by
  arithmetic);
- while it reads an object's pixel data: the phrases left to read or the distance to the edge of
  the line, whichever is less, and then the states it passes.

Before that cycle, the GPU runs by itself, and its automaton replays what ends before it. The bus
may not have stepped the GPU's cycle yet, because the DSP has not handed it over. The object
processor's outputs in a cycle come from its state before that cycle, so the bus computes the
pulse ahead.

### The blitter and Tom's data bus

The blitter is Tom's other master ([0012](decisions/0012-blitter-and-tom-data-bus.md)). It is
ported signal for signal, and stepped with the bus while it works. Its latches take their values
in the middle of a cycle, and its flip-flops at the end.

The GPU writes and reads the blitter's registers on its local bus. A write is queued for the cycle
in which it is served, and taken when the bus steps to that cycle.

While the blitter works, the GPU steps the bus with itself:

- in a cycle that serves a read or write of the blitter's registers, so that it reads B_CMD as
  that cycle has it;
- all the time, while the blitter's interrupt is enabled, or while A1 or A2 points into Tom's or
  Jerry's space, where the blitter can reach the RISCs' memories.

The blitter's interrupt is a one-cycle pulse, like the object processor's. If the bus has not
stepped the GPU's cycle yet, the pulse is computed from the blitter's flip-flops before the cycle.
This happens when the 68000's horizon is short of the cycle, as at the end of a part of a run.

Tom's data bus is kept as eight byte lanes, as `dbus.v` latches them. A read replaces the lanes of
its width. A write's data takes all lanes in the cycle after Tom takes it. The blitter, the object
processor and the DRAM writes take their data from the lanes. So do the line buffer that is shown
and the CLUT's buses, while nothing else drives them. This makes generated blits and object lists
exact, including the bytes of a phrase that a narrower read did not touch.

### The 68000

A 68000 program runs from reset ([0013](decisions/0013-the-68000-is-moira.md)). The 68000 is
Moira, an instruction-level emulator. Moira calls its client at every bus access, at the 68000
clock at which it makes the access. The client, `M68000`, turns each access into a bus cycle of
the netlist's 68000 on `MainBus`:

- AS and the strobes are stepped through Tom's external master interface and memory controller.
- Tom's DTACK ends the cycle.
- Each clock that the cycle waits is added to Moira's clock.

The bus is kept three clocks behind the 68000. There, nothing that the 68000 does yet can change
it: the 68000 decides to start a cycle in the clock before its S0 (`w432`, the bus is its own),
and it holds a grant back in its S0 and S1 (`w395`).

Where the netlist's 68000 orders or addresses its cycles differently from Moira, the client
corrects them in Moira's times. Tom's interrupt line is given to Moira at each instruction
boundary, where the netlist's 68000 decides to take an interrupt.

Moira runs on a thread of its own, by turns with the caller of the run. A bus cycle that still
waits for the bus where a part of the run ends hands the turn back, and goes on with the next part
([0027](decisions/0027-a-run-ends-where-asked.md)). So a part ends where it was asked to, even
while another master keeps the bus.

The rest of the board:

- MEMCON1 and MEMCON2 are registers.
- The address decoding of `abus.v` is complete: ROMHI, and the ROM everywhere until MEMCON1 is
  written.
- The OS ROM is the simulation's boot program or the BIOS.
- The cartridge is its ROM, and its serial EEPROM on Jerry's chip selects.

What is corrected and what is left out is in [hardware/m68000.md](hardware/m68000.md).

A 68000 program starts the RISCs through their slave ports, and leads them
([0014](decisions/0014-three-processors-the-68000-leads.md)). A part of the run belongs to the
68000. The RISCs that it has started run up to it, in turns:

- When the RISCs hold back a 68000 bus cycle, they run until the bus can go on.
- Through a 68000 cycle to memory, they run on to where its next cycle could start
  ([0023](decisions/0023-the-riscs-run-through-the-68000s-memory-cycles.md)).
- When the 68000 is stopped by STOP, they run on to where an interrupt it takes could make its
  first bus cycle ([0024](decisions/0024-a-stopped-68000-waits-in-long-turns.md)).

While a 68000 cycle into a RISC's local space is under way, the RISC is stepped with the bus. This
way, the cycle in which the slave port takes the local bus (`ioreq`) reaches the RISC before it is
made, and the RISC's CTRL and FLAGS are kept for the cycle in which the read is served.

A RISC that has stopped can be started again. It starts as it did at its first GO.

### Two processors

A GPU program can start the DSP. Both then share the bus
([0009](decisions/0009-two-processors-by-turns.md)). Each master has its own port: the DSP's
gateway behind Jerry's bus master, and the GPU's on Tom's own bus. Both ask Tom's arbiter.

- **Each processor runs ahead on its own.** It hands the bus its cycles; how far it has got is its
  horizon. The bus steps a cycle when both processors have handed it over. A processor that needs
  the bus stepped further than the other has got waits at the start of a cycle, and its part of
  the run ends there. While the bus works for one processor and the other runs, the first one is
  stepped.
- **The runner runs the one that is behind.** In a cycle that both have reached, it runs the GPU
  first. This is because a DSP cycle needs the bus stepped through it before the DSP's interrupt
  requests are latched.
- **The GPU reaches the DSP through Jerry's slave.** Tom's I/O cycle is done half at a time,
  through Jerry's slave states. The DSP's slave port latches the high half and asks the DSP's
  memory controller for the low half. The write is served in the next cycle, into the DSP's RAM or
  registers. A GO starts the DSP in the cycle after that. While such an access is under way, the
  DSP steps the bus with the GPU.

## The timing automaton

Stepping the core costs the same in every cycle, whatever the program does. But a program does
the same things again and again. Over the whole of `mandel`, every instruction issues in one of 362
pipeline states ([0007](decisions/0007-timing-automaton.md)). So the stepped core records what it
computes, and replays it.

- A **node** is the control state of the pipeline in a cycle in which an instruction issues
  (`PipeState`). It holds the front end, the memory controller, the control of the divider and of
  the interrupt controller, the bank, the cycles in which the registers and the flags become ready,
  and the planned writes. All times are relative to that cycle. Nodes are looked up by this state.
  The first time the stepped core issues in a state, it creates the node.

- A node has **variants**: what followed in the stepped core, one for each **outcome** of the data
  that the timing depends on. An outcome is made of:
  - a jump's condition and target;
  - a load's or store's address class, and for an I/O register, the written bits that steer the
    pipeline;
  - the cycle in which the I2S interrupt line next rises;
  - the cycle in which each of Jerry's timers next requests.

  The last two count only within the 32 cycles that a variant can cover.

  A variant holds the cycles to the next issue, the **actions** of those cycles at their offsets,
  and the next node. The actions are: the instruction's datapath, the memory controller serving
  the access, a store's data read, a MOVEI's words, a load's or the divider's write-back, an
  accumulation, and the trace event of an interrupt request.

- **Replay** performs the actions at their offsets. They work on live data: the register file and
  its calendar, the flags, the divider and the I2S interface. An outcome without a variant is
  recorded: the node's state is put back into the live members, and the core steps to the next
  issue. A variant with an I/O read, an I2S control write or a span over 32 cycles is always
  stepped.

  A node whose timing depends on no data (no jump, no load or store) has one variant while the I2S
  interface is idle and no timer requests within its span. Runs of such nodes are replayed as a
  chain of tail calls, one function per opcode. This way, the branch predictor keeps one jump site
  per opcode (threaded code).

- The **datapath** is one function, templated on the opcode (`executeOp`). The microword is a
  constant of the template, so the branches that the opcode decides are folded away. These are the
  branches that the predictor cannot learn over a long run of instructions.

- A write into local RAM that holds words any node fetched **discards** the automaton. The queue's
  words are part of every node's state.

- MMULT's sequence, and the cycles before it, are stepped. The systolic unit's state is not in the
  nodes.

- The automaton holds **no bus state**:
  - A variant with a load or store outside the local space is always stepped.
  - While the bus works for the DSP, the bus is left behind. Only nodes whose timing cannot depend
    on it are replayed: no jump, no load or store, and none issuing next. A jump's delay slot is
    not replayed either, because its next node is the jump's target. The bus is brought forward
    when the core steps again.
  - The core is stepped while an external load waits for its write-back, while a write to Jerry's
    registers is under way, and while a timer's period ends within a span. It is also stepped for
    a processor that the bus works for while the other one runs, and for the DSP while the GPU
    accesses Jerry.

- If the replay finds a write into fetched code, it hands over the live state that the node
  replay has reached. Then the automaton is discarded.

- **Waiting loops are skipped.** At a jump node, the replay compares the core's data with what it
  held the last time it passed that node: the registers, the flags, the accumulator, the divider
  and the values of the planned writes. The node itself holds the control. If they are equal, and
  nothing in between changed memory, touched an I/O register or used the main bus, the loop
  repeats exactly until an event. Whole iterations are then skipped, up to the I2S line's next
  rise, a timer's next request or the end of the part of the run. Every time in the core is moved
  on by the skipped iterations.

  The trace is held back from its sink, so that an iteration's events can be repeated for the
  skipped iterations. A flags event is a change from the cycle before, so it is counted from the
  iteration's second cycle.

The stepped core is the only source of timing. The replay must write the same trace. Every
reference is checked in both modes. `--step` runs the stepped core alone, and works out the bus's
control in every cycle.

## Checking it

The core is checked against the Verilator simulation of the FPGA core (`sim/`,
[0002](decisions/0002-the-fpga-core-is-the-reference.md)), through recorded traces
([0004](decisions/0004-recorded-references.md), [spec/trace.md](spec/trace.md)). For every program
in `tests/reference`, the core must issue every instruction, write every register and change the
flags in the same cycles as the simulation. It must also end in the same state.

The programs come from:

- lyxass sources (`tests/asm`);
- 68000 sources (`tests/m68k`);
- binaries of real programs (`tests/programs`);
- a generator that writes random instruction sequences, one class at a time.

## The runner

`jagmcp-cli` takes the same arguments as the simulation, and prints its state in the same format.
`--check` compares a run with a reference.

The machine that it runs (`Machine`) holds main RAM, and Jerry's joystick ports as the main bus
reads and writes them.

- A GPU program runs next to the DSP, which the program may start. The run ends where the GPU's
  run ends.
- A 68000 program (`--m68k`) runs from reset, with the simulation's OS ROM. The DSP is not
  started. The run ends at its limit. For a COFF program, the OS ROM also sets the video as the
  BIOS leaves it ([0028](decisions/0028-a-coff-program-runs-as-a-loader-starts-it.md)).

The machine runs up to a cycle, and can go on from there later. A run in parts gives the same
result as a run in one piece.

## The window

`jagmcp` runs the runner's machine without a limit ([0011](decisions/0011-the-window-is-pgmemus.md)).
It runs one part at a time, on a thread of its own. It runs at the console's speed when the core is
fast enough. It shows each frame of the video output and plays the I2S output as they are made.

It reaches the core through the same interface as the runner. The only part of the core that
exists for the window alone is the sink for the I2S frames.

The emulation thread owns the machine as a session of the control API (see
[The control API](#the-control-api)). It answers the API's requests between two parts of the run.
So the window and the programs that it serves drive the same machine.

**Starting a cartridge.** A cartridge (`.j64`, `.jag`) is started when it is dropped onto the
window, chosen with File > Open, or given alone on the command line. It boots with a BIOS, as
games do in PGMEmu. The BIOS is chosen in this order:

1. one given with `--bios`, for this start only;
2. the one remembered in `settings.json`, in the preferences directory;
3. one found next to the cartridge, in a `bios` folder there, one folder up, or in a `bios`
   folder there. It must be 128 KB. The names are tried in this order: `jagboot.rom`,
   `jagboot_model_m.rom`, No-Intro's `[BIOS] Atari Jaguar (World).j64`, and similar names. A BIOS
   found this way is remembered.
4. one chosen with File > Choose BIOS. It is remembered.

Without a BIOS the start fails, and the status window says so.

**Recording.** File > Record video and `--record FILE` start a record through the same
`video.record_start` method that a client calls
([0031](decisions/0031-video-is-recorded-by-ffmpeg.md)). File > Stop recording ends it with
`video.record_stop`. A record that is still running when the window closes is written then, even
one a client started. The runner's outputs (`--frames`, `--wav`, `--log`) are not written: the
window runs without an end, and the session owns the frame and sound sinks. The window says so
when it gets them.

The status window shows the BIOS that the program on screen boots with. A client's `emu.load` can
give another one with `--bios`. That one is used for that load only. When the remembered BIOS is a
different one, the status window shows it on a second line, as the BIOS for cartridges.

**The keyboard** is the pad in port 1:

| Key | Pad |
|---|---|
| Arrows | Joypad |
| Z, X, C | A, B, C |
| Tab | Option |
| Return | Pause |
| Digits (top row or keypad) | Keypad |
| `-`, or `*` on the keypad | Star |
| `=`, or `+` on the keypad | Hash |

The keys' state is passed on when it changes, in the same way as `input.set` passes it. So
buttons held by a request stay held while no key changes. The keyboard does not drive ImGui's
navigation.

## The control API

A running program is driven and inspected through one dispatcher of methods, with JSON in and JSON
out ([0018](decisions/0018-one-control-api.md), [spec/control-protocol.md](spec/control-protocol.md)).
The dispatcher works on a session that owns the runner's machine. The machine is loaded without a
limit and run in parts. The session keeps the last frame and the I2S output.

`jagmcp-cli` serves the API on a thread of its own. The window serves it on its emulation thread.
The transports are PGMEmu's: JSON lines on stdio and TCP, and MCP on stdio and HTTP. Only the
transports and the dispatcher know about JSON or sockets.

The methods read the core between runs without a bus cycle, so looking at it changes nothing in
the run. They read memory as the masters see it, the processors' registers, and their code,
disassembled.

A run stops early at a debugger probe: a breakpoint or a watchpoint. While a probe is set on a
RISC, that RISC runs on its stepped core ([0019](decisions/0019-breakpoints-step-the-risc.md)).
