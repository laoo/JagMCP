# Documentation map

This file is the index of every document in the project. **A document that is not listed here must
not exist.** If a new piece of writing cannot be classified into one of the buckets below, that is
the signal it should not be written. The regime is PGMEmu's (and NGA's), adopted whole:
[0001](decisions/0001-documentation-regime.md).

## The rule

> **Documentation carries *why* and *contracts*. Code carries *how*.**

1. **One fact, one place.** Every other place links to it instead of repeating it.
2. **No status files.** No `PROGRESS.md`, `TODO.md`, `CHANGES.md` or session notes. Git history
   covers that ground.

Documentation is updated in the **same commit** as the change that invalidated it.

## Buckets

| Location | Answers | Must not contain |
|---|---|---|
| `/README.md` | What JagMCP is, how to build and run it | architecture, internal APIs |
| `/THIRD_PARTY.md` | What JagMCP is derived from and the terms that work asks to be carried | anything of ours beyond the credits |
| `/CLAUDE.md` | How to work in this repository: commands, conventions, where things live | domain knowledge |
| `docs/architecture.md` | What the components are, how time flows through them, **why** the boundaries sit where they do | class or function listings |
| `docs/decisions/NNNN-*.md` | Why a decision was made and what it costs. Immutable once merged; a reversal is a new record | present-tense state |
| `docs/spec/*.md` | External contracts: the trace format, the runner's arguments, the control protocol | implementation detail |
| `docs/hardware/*.md` | Where the emulator knowingly differs from the netlist, and the evidence | anything the netlist already says |
| `docs/plans/*.md` | The order decided-but-unbuilt work is built in, each entry removed in the commit that lands it | status, progress |
| `sim/README.md` | How the FPGA simulation is built and what it changes of the core to run | the emulator |
| `libextern/README.md` | The third-party code carried in the tree, its versions, and how it is updated | anything of ours |
| `.claude/skills/*/SKILL.md` | How an agent drives the emulator: starting it, the requests, what trips it up | the protocol itself |

## Index

### Overview
- [Architecture](architecture.md): the RISC core, the main bus and its masters, the 68000, how time is kept, and how it is checked

### Specifications
- [Traces](spec/trace.md): the events of a run, as the simulation and the core write them
- [The control protocol](spec/control-protocol.md): the methods a client drives a running program with, and their transports

### Decision records
- [0001: Documentation regime](decisions/0001-documentation-regime.md)
- [0002: The FPGA core is the reference](decisions/0002-the-fpga-core-is-the-reference.md)
- [0003: The RISC is executed by instruction, its pipeline accounted for](decisions/0003-risc-by-instruction.md)
- [0004: References are recorded traces, generated programs among them](decisions/0004-recorded-references.md)
- [0005: Licence is GPL-2.0](decisions/0005-licence-gpl-2.md)
- [0006: The front end is stepped every cycle](decisions/0006-front-end-stepped-by-cycle.md)
- [0007: Timing is replayed from an automaton the stepped core builds](decisions/0007-timing-automaton.md)
- [0008: The main bus is stepped by DSP cycle, from a fixed state at DSPGO](decisions/0008-main-bus-by-cycle.md)
- [0009: The GPU and the DSP run by turns, the bus stepped as both hand it their cycles](decisions/0009-two-processors-by-turns.md)
- [0010: The video output and the object processor are stepped by cycle; frames are references](decisions/0010-video-by-cycle-frames-as-references.md)
- [0011: The window is PGMEmu's; the machine runs in parts on a thread of its own](decisions/0011-the-window-is-pgmemus.md)
- [0012: The blitter is stepped by cycle; Tom's data bus is its byte lanes](decisions/0012-blitter-and-tom-data-bus.md)
- [0013: The 68000 is Moira, its bus cycles made on the stepped bus; its references start at reset](decisions/0013-the-68000-is-moira.md)
- [0014: With the 68000, three processors take turns; the 68000 leads](decisions/0014-three-processors-the-68000-leads.md)
- [0015: The BIOS and games are checked outside the repository](decisions/0015-bios-and-games-checked-outside-the-repository.md)
- [0016: A release may be built profile-guided](decisions/0016-profile-guided-release.md)
- [0017: The window shows the picture at 4:3](decisions/0017-the-picture-is-shown-4-3.md)
- [0018: One control API, served as PGMEmu serves its own](decisions/0018-one-control-api.md)
- [0019: A debugger's breakpoints run a RISC on its stepped core; with the 68000 they stop it where the turns allow](decisions/0019-breakpoints-step-the-risc.md)
- [0020: The main bus replays its control from transitions it records](decisions/0020-the-bus-replays-its-control.md)
- [0021: A 68000 bus cycle on a quiet bus is replayed in one step](decisions/0021-a-68000-bus-cycle-in-one-step.md)
- [0022: Games run with the MiSTer core's compatibility fixes](decisions/0022-mister-compatibility-fixes.md)
- [0023: Through a 68000 bus cycle to memory the RISCs run on to where its next could start](decisions/0023-the-riscs-run-through-the-68000s-memory-cycles.md)
- [0024: A stopped 68000 lets the RISCs run on to where it could next make a bus cycle](decisions/0024-a-stopped-68000-waits-in-long-turns.md)
- [0025: CI builds three platforms, and a tag makes a release](decisions/0025-ci-and-releases.md)
- [0026: A read no Tom register answers gives MEMCON1, and is reported](decisions/0026-undriven-tom-reads-give-memcon1.md)
- [0027: A run ends where it was asked to, whatever the program does](decisions/0027-a-run-ends-where-asked.md)
- [0028: A COFF program runs on the 68000, started as a loader starts it](decisions/0028-a-coff-program-runs-as-a-loader-starts-it.md)
- [0029: CI runs on a tag or by hand, not on every push](decisions/0029-ci-runs-on-a-tag-or-by-hand.md)
- [0030: A single step runs the whole machine; no processor is held](decisions/0030-a-step-runs-the-whole-machine.md)
- [0031: Video is recorded by ffmpeg, run beside the emulator](decisions/0031-video-is-recorded-by-ffmpeg.md)
- [0032: A record's colours are YCbCr by BT.709, and the file says so](decisions/0032-video-colours-are-ycbcr-by-bt709.md)

### Hardware
- [The main bus](hardware/main-bus.md): what the port of Tom's and Jerry's bus logic leaves out
- [Tom](hardware/tom.md): what the port of the data bus, the video, the object processor, the blitter, the PIT and INT1 leaves out
- [The 68000](hardware/m68000.md): where Moira's bus cycles are corrected, and what is left out
