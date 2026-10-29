# Tom: what the port leaves out

`Tom` ports the video time base and output (`vid.v`, `pix.v`, `cryrgb.v`), the object processor
(`ob.v`, `wbk.v`, `obdata.v`, `lbuf.v`) and the PIT and interrupt control of `misc.v`
([0010](../decisions/0010-video-by-cycle-frames-as-references.md)); `Blitter` the blitter, and
`MainBus` Tom's data bus ([0012](../decisions/0012-blitter-and-tom-data-bus.md)). These are the
places they knowingly do not follow the netlist, each with what the FPGA simulation does.

## The data bus

- **What reaches the lanes.** DRAM reads, reads of the OS ROM and the cartridge (as the board
  drives the external bus, [main-bus.md](main-bus.md)), the writes of the GPU, the blitter and the
  object processor, Jerry's writes of DRAM and the GPU's reads of Tom's registers. Reads of
  Jerry's ports and timers and of Tom's registers by the DSP, and Jerry's other writes, are not
  modelled: the lanes keep what they held. Nor are the 68000's cycles, `ourack`'s `$40` (the
  vector of an interrupt's acknowledge) among them.
- **Within a cycle** the lanes are what they hold at its end: a value driven and replaced in the
  same cycle (a read latched while a write's data is still on the bus) is not seen.

## The video output

- **The RISCs' accesses to the line buffers** ($F00800 to $F01FFF) are modelled a word at a time:
  A, B, or the buffer not shown, the word at an address two past a long word the high pixel; into
  the buffer shown while a line is displayed, at the output's read pointer, what its bus carries
  (the background colour while the pair is cleared, else zero); a read a cycle late (`lbd`). Where
  a write or a read meets the moment a buffer becomes shown, or a read is latched while the
  buffer's RAM does not drive its bus (the RAM enabled for half of the cycle, at `sys_clk`), the
  simulation differs: the pixel or the value read is zero. The test writes the buffers in the
  vertical blank. A GPU's phrase there is four words from the phrase's start, the high one first;
  through the 32 bit window ($F08800 on) a GPU's long word writes both its pixels at once, as the
  blitter's do; the DSP's writes through the window are not modelled. No reference records the
  GPU's phrases or its writes through the window: they are checked against the console (all four
  pixels of a phrase, both of a long word, in the console's cycles).
- **The CLUT's accesses** are modelled without the cycles the object processor's reading takes
  from them; a RISC's read of the CLUT while the pixel path reads it (`pden`) does not reach the
  CLUT's buses.
- **The light pen, HLOCK and VLOCK** (`vid.v` `lp`, `hlock`, `vlock`) are idle, and the MiSTer
  core's `active_video` option is off, as in the simulation.

## The object processor

- **The GPU object** (type 2) raises the GPU's interrupt 3 (`grpintreq`) and waits for OBF to be
  written, as in the netlist; the GPU steps the bus with itself while that interrupt is enabled
  and the object processor may run.

## The blitter

- **Its registers are written by the GPU and the 68000.** The 68000's reach them through the GPU's
  slave port, in the cycle the port serves the write
  ([0014](../decisions/0014-three-processors-the-68000-leads.md)); the DSP's are not modelled.
- **It reads DRAM, the OS ROM, the cartridge, the GPU's space and the rest of Tom's own space, and
  writes DRAM, the GPU's space, the DSP's and the rest of Tom's own space** (through `gpu_cpu.v`
  and Jerry's slave, as the 68000 does: the BIOS copies the GPU's code with it, a game the DSP's).
  Its reads and writes of Tom's registers, the CLUT and the line buffers are a word each I/O
  cycle, as the 68000's; a read takes the word of Tom's read bus into its lanes, as the GPU's
  (`blit_read_tom`; a program may use the CLUT as a buffer). An undriven read there gives MEMCON1
  ([0026](../decisions/0026-undriven-tom-reads-give-memcon1.md)) and is not reported: it has no
  instruction. Through the 32 bit window onto the line buffers ($F08800 on) a long word writes
  both its pixels at once (`lbuf.v` `wea12`), as a demo drawing a picture line by line from a GPU
  object's interrupt does; a read there, a read of the DSP's space, or a cycle at Jerry's other
  registers, runs on the bus as the netlist runs it but moves no data.
- **`big_pix`** (G_END bit 1) is set, as the boot program and the BIOS leave it; the GPU's writes
  of G_END are not modelled.
- **`indone`** goes through a delay of a fast clock (`sysclkdly`) in the netlist; it is taken as
  the cycle's own.

## The registers

- **The sync and equalisation registers** (HS, HVS, HEQ, VS, VEB, VEE) are written and have no
  effect, the video output collected outside the sync. The light pen registers, the test
  registers, OB0 to OB3 and LBRA read as 0 (the simulation drives what those registers hold).
- **A read that no register decodes** gives MEMCON1, as the chip's undriven read bus holds it. The
  simulation gives 0. Such reads are reported
  ([0026](../decisions/0026-undriven-tom-reads-give-memcon1.md)).
- **A long word two past a long word boundary**, the GPU's, is two 16 bit cycles from its address
  on within its phrase, the second wrapping to the phrase's start from its last word (`memwidth.v`
  adds the width on three bits). A store's long word is on both halves of the bus, so the word at
  its address takes its low half and the next its high half: a long word at BORD1 ($F0002A) writes
  BORD1 with its low half and BORD2 with its high half. A load's long word is the half of the bus
  the second cycle's address names, whatever the lanes it did not read hold: at $F00006, MEMCON1
  ($1861) in the high half and what the bus last carried in the low half. Jerry's ports are not
  modelled so ([main-bus.md](main-bus.md)); the generator reads those by word.
- **INT1's sources** are the vertical interrupt, the PIT, the object processor's stop object and
  the GPU's (`gpuint`); Jerry's is not modelled. A pending request is the 68000's interrupt when the
  68000 is emulated; with a RISC's program it stays in STOP with all levels masked. A pending
  request still gives it the bus before the GPU
  (`ihandler`, the arbiter's `req[3]`) until INT2 is written with none pending, as in the
  simulation, where a GPU that does not acknowledge waits for the bus for ever.
  With `compat` ([0022](../decisions/0022-mister-compatibility-fixes.md)) a write of INT1 that
  enables the vertical interrupt where it was disabled clears its latch (`vintbugfix`), as an
  acknowledge does (`m68k_vint_latch_compat`).
- **HLOCK, VLOCK and the light pen input** (`vid.v` `hlock`, `vlock`, `lp`) are idle.
