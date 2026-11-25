# 0028: A COFF program runs on the 68000, started as a loader starts it

**Status:** accepted

## Context

Homebrew for the Jaguar is often linked twice. `rln -a 802000` makes a cartridge image. `rln -a 4000`
makes a COFF file (`.cof`) that is loaded into RAM. A developer sends the COFF file to the console
over a cable (Skunkboard, BJL, JaggD). The console has booted with its BIOS before that. The loader
copies the sections to their addresses and jumps to the entry point. Virtual Jaguar runs a COFF
file without a BIOS.

The runner read only BS94, lyxass's format, and raw binaries. A COFF file needed a converter.

A COFF program does not set all of the video itself. The demos of `../JaguarDemos` set VMODE, HDB,
HDE, VDB, VDE, OLP and the colours. They take the timing as the BIOS leaves it: HP, VP, the blanking
and the syncs. With the simulation's boot program, which does not set the timing, Tom completes no
frame.

## Decision

- **A program file is BS94 or COFF.** The format is known from the first bytes, not from the name.
  From a COFF file, each section with bytes in the file is loaded at its physical address. `.bss`
  is not loaded: RAM is zero at power-on. The program starts at the entry point of the a.out
  header.
- **A COFF program runs on the 68000** without `--m68k`. Atari's linkers make 68000 programs.
  `--gpu` runs it on the GPU.
- **The boot program sets the video as the BIOS leaves it** before it jumps to a COFF program. It
  writes BG, BORD1 and the timing registers with the values that the model M BIOS writes on a PAL
  console, in the same order. Then it clears the two line buffers, as the BIOS does. The values
  were read from a run of the BIOS, with watchpoints on Tom's registers.
- **RAM holds the object list the BIOS leaves, and OLP points to it.** Before it starts a
  cartridge, the BIOS writes a STOP object at $1000 and sets OLP to $1000. Some programs turn the
  video on before they set OLP. With OLP at zero, the object processor reads what is at $0 as a
  list. That list may not stop, and the object processor keeps the bus: the 68000 waits for ever. The STOP object is put in RAM
  before the program is loaded, so a program loaded over $1000 replaces it, as the loader would.
- **`--bios` with a COFF program is refused.** The BIOS boots a cartridge. It does not start a
  program in RAM.

## Consequences

- yarc, jag_ball, jagniccc, hirez_slideshow and waves run from their `.cof` files without a BIOS.
  waves had no cartridge image.
- The boot program of a BS94 program is not changed. It is the simulation's
  (`sim/sim_main.cpp`), and the 68000 references start with it.
- What else the BIOS leaves is not set: its exception vectors in RAM, the clock dividers of Jerry,
  the GPU it has stopped. A program that depends on these runs only from a cartridge image.
- The console is PAL. With `--ntsc` it is NTSC, and the video the BIOS writes on one (0034).
- A COFF file with relocations is loaded as it is. The linker's absolute output has none.
