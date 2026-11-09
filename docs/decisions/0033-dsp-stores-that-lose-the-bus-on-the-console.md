# 0033: A DSP store that loses the bus writes its high half twice on the console

**Status:** accepted

## Context

Jerry takes a store's data into its data latch the cycle after the store starts (`lwdl`). When
the DSP's grant is withdrawn in that cycle, the netlist's latch takes the bus instead: the high half
pulled up (`$FFFF`), the low half what the bus last carried
([main-bus.md](../hardware/main-bus.md), `random_external_0031`). The simulation does this often:
85 references (`random_external`, `random_video`) differ without it. A store after an external read
that has finished does not take it (of the stores in the references, 1074 of 1494 without such a
read took the open bus, 0 of 252 with one).

Atari documented the bug (Jaguar Developer Technical Notes, JDEV0002, "Jaguar Hardware Bugs &
Warnings", March 22, 1995, GPU/DSP item 7):

> The DSP must not do an external write unless it is preceded by an external read that will
> complete for the write starts. This problem is intermittent and could be missed by testing. Be
> careful in any DSP code that writes to external memory.

Its example: `load (r1),r2` / `or r10,r11` / `store r11,(r3)` will not work correctly,
`load (r1),r2` / `or r2,r11` / `store r11,(r3)` will (the `or` waits for the load).

A program on the console (`openbus_test`, 2026, through the BIOS from a cartridge) runs
`random_external_0110` and `_0195` 256 times each, after different delays, and lets the DSP store
2048 long words, 32 times, without and with a load before each store:

| | console | core, open bus | core, without |
|---|---|---|---|
| `random_external_0110`, runs wrong | 239 | 241 | 0 |
| `random_external_0195`, runs wrong | 232 | 225 | 0 |
| stores without a load, Blitter busy, longs wrong of 65536 | 1 | 3 | 0 |
| stores with a load first, Blitter busy | 0 | 0 | 0 |
| stores without a load, bus quiet | 3 | 2 | 0 |

The console fails as often as the netlist, but writes a different value: the store's high half,
then the high half again. `random_external_0110` leaves `$89BA89BA` where the store's data is
`$89BA23AF` (the netlist `$FFFF0000`), `_0195` `$00070007` for `$0007F000`, the stores
`$5A4A5A4A` for `$5A4A023C`. Never `$FFFF`. A likely cause: on the chip the undriven path keeps the
half driven on it last, the FPGA core resolves it to its pull-ups and the bus's last half word.

## Decision

- **An `openBus` option** in the core (`RunOptions::openBus`, `--open-bus`, `--no-open-bus`,
  `MainBus::setOpenBus`): on, the latch takes the open bus as the netlist does; off, a long word
  store takes its high half into both halves, as the console does. Only long word stores were
  measured: a word or a byte store takes the open bus either way.
- **It is off where a BIOS is given**, as games run, and on otherwise, as the references run: the
  references stay the netlist's, as with [0022](0022-mister-compatibility-fixes.md)'s `compat`.
- It changes only the data latched, not the bus's control, so the timing automaton is the same
  either way.

## Consequences

- A cartridge run through the BIOS fails where and as often as the netlist does, with the
  console's value. A program that keeps the technote's rule does not see it at all.
- A RISC program, a COFF program and a reference run take the netlist's open bus unless
  `--no-open-bus` is given; a COFF program started as a Skunkboard starts it gets the netlist's
  value, not the console's.
- What a word or a byte store writes on the console is not known; a test like `openbus_test` with
  `storew` would tell.
