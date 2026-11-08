# 0033: A DSP store keeps its data on the console; the open bus only for the references

**Status:** accepted

## Context

Jerry takes a store's data into its data latch the cycle after the store starts (`lwdl`). When
the DSP's grant is withdrawn in that cycle, the netlist's latch takes the bus instead: the high half
pulled up (`$FFFF`), the low half what the bus last carried
([main-bus.md](../hardware/main-bus.md), `random_external_0031`). The simulation does this often:
85 references (`random_external`, `random_video`) differ without it.

A real program shows it as wrong pixels. `fact_slope` (branch `julia`, 2026) draws a Julia set
from the top with the DSP while the GPU draws it from the bottom and builds object lists, with
the blitter copying them. The DSP stores two 16-bit pixels at a time to DRAM. Compared with a
reference picture, JagMCP has about two longs per picture with `$FFFF` in the high half and the
previous long's low half in the low half: bright yellow dots in the black areas. Storing one
pixel at a time (`storew`), a pixel now and then holds its left neighbour's value. On the console
the same program shows no such dots. With the latch kept from the gateway, the pictures in JagMCP
match the reference to the pixel, and the timing is unchanged.

So the chip does not do what the translated netlist does here. A likely cause is the kind
[0026](0026-undriven-tom-reads-give-memcon1.md) describes: on the chip the bus keeps its charge,
the core resolves an undriven bus to a fixed value. That is not proven.

## Decision

- **An `openBus` option** in the core (`RunOptions::openBus`, `--open-bus`, `--no-open-bus`,
  `MainBus::setOpenBus`): on, the latch takes the open bus as the netlist does; off, it keeps the
  store's data from the gateway.
- **It is off where a BIOS is given**, as games run, and on otherwise, as the references run: the
  references stay the netlist's, as with [0022](0022-mister-compatibility-fixes.md)'s `compat`.
- It changes only the data latched, not the bus's control, so the timing automaton is the same
  either way.

## Consequences

- A cartridge run through the BIOS writes what the DSP stores, as the console does. A RISC program,
  a COFF program and a reference run take the open bus unless `--no-open-bus` is given.
- No reference shows the console's behaviour: it rests on one program seen on one console. A test
  on the console that finds the open bus after all reverses this.
