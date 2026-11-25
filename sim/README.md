# jagsim: the FPGA reference

A [Verilator](https://www.veripool.org/verilator/) simulation of the
[Jaguar_MiSTer](https://github.com/MiSTer-devel/Jaguar_MiSTer) core: Tom, Jerry and the 68000 as
translated from the original netlists, running a DSP or GPU program and writing a trace of both. It
is JagMCP's reference ([0002](../docs/decisions/0002-the-fpga-core-is-the-reference.md)); the
same harness lives in TomNJerry's `fpga/`, where it is compared with TomNJerry's pipeline log.

## Build and run

```sh
make                                   # RTL=../../Jaguar_MiSTer/rtl by default; VCD=1 for --vcd, --vcd-from
./obj_dir/jagsim program.o --log trace.txt
```

JagMCP records its references with it (`scripts/make-references.py`); the trace format is
[docs/spec/trace.md](../docs/spec/trace.md).

Speed: about 150000 DSP cycles per second (0.6 MHz of `sys_clk`).

## What is simulated

The `jaguar` module of the core (everything below the MiSTer framework) with:

- **68000 boot program** in the OS ROM, doing what game code would do: `MEMCON1=$1861`,
  `MEMCON2=$35CC` and `D_END=$00070007` like the BIOS, write `JOYSTICK=$0100`, `D_FLAGS`, `D_PC`,
  set `DSPGO` and `stop #$2700`, so the 68000 stays off the bus while the DSP runs. It is the same
  for every program, so that the console is in the same state at DSPGO in every run
  ([0008](../docs/decisions/0008-main-bus-by-cycle.md)): the refresh counters, the 68000's last
  bus cycles (the end of its write of DSPGO, then one prefetch from ROM), the clock phases.
- **The DSP program** is written into the DSP RAM by the testbench while reset is held, through a
  port of `sim_top.sv` into the RAM's memory array; reset lasts as long whatever the program. A
  program loaded outside the DSP RAM goes into main RAM like `--load`.
- **`--gpu`** runs the program on the GPU instead: it goes into the GPU RAM ($F03000), the boot
  program writes G_END and D_END (both, as the BIOS), G_FLAGS, G_PC and sets GPUGO, and the trace
  and the state are the GPU's. The DSP's events are traced as well, prefixed with `dsp:`, for when
  the program starts it, and its registers are in the state if it ran
  ([0009](../docs/decisions/0009-two-processors-by-turns.md)).
- **`--m68k`** runs a 68000 program instead
  ([0013](../docs/decisions/0013-the-68000-is-moira.md)): the testbench loads it into main RAM
  during reset like `--load`, the boot program sets MEMCON1, MEMCON2, G_END and D_END and jumps to
  it, and the run and its cycles start as reset ends. The trace takes the 68000's bus cycles from
  its pins (AS, UDS, LDS, R/W, the function code, its data in as `Jaguar.sv` registers it and out);
  the state has no registers of the 68000's, only the RISCs' it starts.
- **Frames**: the video output (`vga_r`, `vga_g`, `vga_b`) is collected at `vid_ce` outside
  `hblank` and `vblank` into frames, whose hashes end the state; `--frames DIR` writes them as
  PPM images ([docs/spec/trace.md](../docs/spec/trace.md)).
- **Ideal DRAM**: a phrase is read or written at the falling CAS edge, `ram_rdy` is always 1. It is
  addressed as the MiSTer SDRAM controller addresses it: the row from Tom's multiplexed DRAM
  address as RAS falls, the column as CAS falls (`dram_a` for a write, Tom's held address
  `dram_addrp` for a read). The controller's latency workarounds are not part of the simulation.
- **`--cart FILE`** puts a cartridge in: FILE's bytes from $800000, a 32 bit ROM as every
  cartridge's is, blind to A0 and A1 (`cart_q` the long word at `abus_out[22:2]`) whatever
  MEMCON1's ROMWIDTH, so that at 8 bits Tom reads the low byte of each long word; its data is
  there at once (`xwaitl` high). The MiSTer framework's steering of images tagged 8 or 16 bits wide,
  and its SDRAM's latency, are not part of the simulation. The cartridge's EEPROM (`eeprom_93c46_x16.v`,
  in `jaguar.v`) keeps its words in a RAM of the testbench read a cycle later, erased at the start.
- **`--bios FILE`** puts FILE (128 KB) in the OS ROM in place of the boot program, with `--m68k`: the
  run is the console's from reset, the program optional.
- No video setup by the boot program (the object processor is idle), joypads released, PAL (NTSC with `--ntsc`: the core's `ntsc` input).

Differences from the Quartus build, all on the simulation side:

| file | why |
|------|-----|
| `altsyncram.v` | Behavioural model of the Altera RAM megafunction with the parameters the core uses: registered inputs, optional output register, new data on same port read during write, old data on the other port. |
| `jaguar_common/defs.v` left out | It defines `SIMULATION` under Verilator, which switches the RAMs (the DSP register file among them) to models with a different latency than the synthesized ones. |
| `Tom/abus.v` patched | `wire fcl[0];` is a one bit array for Quartus and an empty one for Verilator; becomes `wire [0:0] fcl;`. |
| `nuked-68k/68k.v` patched | Microcode files get absolute paths. The nuked 68000 is the CPU of the MiSTer build (`ACCURATE_CPU` in `jaguar.v`). |
| `Jerry/raa016a.v` patched | Its wave ROM prints every read with `$display`; the line is removed. |
| `spram.v` | Small RAM from the MiSTer framework, used by Butch only. |
| `sim_top.sv` | Passes the `jaguar` ports through, registers `m68k_di` like `Jaguar.sv` does, takes the trace signals from inside the DSP, the GPU and the 68000's pins by hierarchical references, and writes the RISCs' RAM through one (`ram_poke`). |
