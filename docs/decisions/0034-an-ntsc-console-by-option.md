# 0034: An NTSC console, by option

**Status:** proposed

## Context

The emulator was a PAL console (0028). Most consoles sold were NTSC, and a program timed by the
video beam, or by frames, gives other numbers on one: 524 values of VC a frame and 60 frames a
second instead of 624 and 50.

In the core, the region is one input, `ntsc`, which is JOYBUTS's bit 4 (`jaguar.v`, `b[4]`); Tom's
`ntsc` port is not used. Nothing else in the hardware changes: the BIOS reads the bit and writes
the NTSC video timing. The NTSC system clock (26.590906 MHz) differs from the PAL one by 0.01%.

## Decision

- **`--ntsc` sets JOYBUTS's bit 4**, in the runner and in the simulation (`jagsim --ntsc`, the
  core's `ntsc` input). JOYBUTS reads `$FF7F` with the pads released instead of `$FF6F`.
- **A COFF program on an NTSC console gets the NTSC video**: the boot program writes the registers
  that it writes for PAL with the values the BIOS writes when the bit is set, read from a run of
  the BIOS with watchpoints on Tom's registers, in the same order. VDE is `$FFFF` as for PAL, and VI
  the line a program's startup code sets on NTSC.
- **The clock stays the PAL one** (`CYCLES_PER_SECOND`): a second of NTSC console time is counted
  0.01% long.

## Consequences

- A cartridge booted with `--bios` and `--ntsc` runs as on an NTSC console: the BIOS sets the
  timing. A cartridge that times 68000 loops, blitter copies and a GPU decoder by the beam,
  measured on an NTSC console, agrees within 1% in 46 of its 48 tests without a RISC loading
  DRAM (within 2.6% in all), and within 10% with the GPU loading it. `m68k_ntsc` is the reference
  for the bit.
- Without `--ntsc` nothing changes.
