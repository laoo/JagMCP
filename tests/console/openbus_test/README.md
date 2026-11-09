# openbus_test

A test for a real console, by 42Bastian (2026). It measures what a DSP store writes when the DSP
loses the bus the cycle after the store starts. Its results are in
[decision 0033](../../../docs/decisions/0033-dsp-stores-that-lose-the-bus-on-the-console.md).

It is not a reference and not a CTest test. It runs from a cartridge through the BIOS, and the
simulation shows the netlist's behaviour, not the console's.

Only the sources are here. To build it you also need:

- `ref0110.o` and `ref0195.o`: copies of `tests/reference/random_external_0110.o` and `_0195.o`.
- `exp0110.bin` and `exp0195.bin`: the expected memory. `make expected` writes them with a running
  JagMCP server; the Makefile's first comment says how `exp0195.bin` is made.
- `sbl.XXX`, the cartridge header of BJL, and `BJL_ROOT` (new_bjl), lyxass with its `js/` include
  files, and rmac.

The picture shows the results only at the end, after "done". In JagMCP that takes about
2,000,000,000 cycles through the BIOS. Run it with `--no-open-bus` (the default with a BIOS) for
the console's values and with `--open-bus` for the netlist's.
