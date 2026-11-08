# The main bus: what the port leaves out

`MainBus` ([0008](../decisions/0008-main-bus-by-cycle.md),
[0009](../decisions/0009-two-processors-by-turns.md)) follows the netlist's control signal for
signal; these are the places it knowingly does not, each with what the FPGA simulation does.

## Data

- **A word at an odd address outside the local space.** Jerry puts the addressed byte on the low
  lane only; the high byte of what the DSP loads is what the bus held before (`random_external`
  programs with an odd word pointer read `$27F0` where the word at the even address is `$DEF0`, the
  `$27` left by an earlier cycle). The core loads the word at the even address. Stores of a word at
  an odd address are likewise not modelled. The generator leaves both out.
- **Open bus.** When the DSP's grant is withdrawn the cycle after Jerry starts a store
  (`random_external_0031`), Jerry's data latch takes the bus: the high half pulled up (`$FFFF`), the
  low half what the bus last carried. The core keeps that low half as the last half word Jerry drove
  or received; the bus's other drivers (Tom for its own masters, the 68000) do not update it. The
  console keeps the store's data instead; with a BIOS the core does too
  ([0033](../decisions/0033-dsp-stores-keep-their-data-on-the-console.md)).
- **Main RAM** is the simulation's: 2 MB in bank 0 at $000000, mirrored up to $3FFFFF. Bank 1
  ($400000) is not fitted: nothing is written there, the blitter and the object processor read
  what Tom's data bus holds ([0012](../decisions/0012-blitter-and-tom-data-bus.md)), and the
  processors read zero where the simulation reads the bus too. Writes take effect as Tom strobes
  CAS; the DSP's read takes its data as Jerry starts the cycle, the GPU's as Tom takes it, which is
  the same unless the other processor writes the same phrase in between.
- **The OS ROM and the cartridge** are read by every master with a 68000 program, as the board
  drives the external bus's lanes: the OS ROM, 8 bits wide, the low lane with the byte at the
  address Tom puts out; the cartridge, a 32 bit ROM blind to A0 and A1 (the simulation's, see
  `sim/README.md`), the lanes with the long word there; a lane nothing drives pulled up. At a
  ROMWIDTH of 64 bits the simulation's high lanes keep what Tom's data bus held before; the core
  gives `$FF` there. With a RISC's program the core has no ROM and a RISC's read of it gives zero.
- **The cartridge's EEPROM** takes its clock, its data and its chip select as Tom's I/O cycle to
  Jerry's GPIO0 or GPIO1 ends, not as the simulation's a sys_clk or two later, and a write or an
  erase is done at once, not in the cycles of sys_clk the simulation's takes (DO low meanwhile):
  a program polls DO long after either.
- **Jerry's registers** other than the timers read as the simulation's: JOYSTICK's low byte the
  column latch JOYSTICK's last write left (with its bit 15 set) or $FF, bit 0 the EEPROM's DO; its
  high byte and JOYBUTS's low one the rows of the pads' columns the latch selects
  (`jag_controller_mux.v`), JOYBUTS's high bits those of a PAL console ($FF6F with the pads
  released, as the simulation's always are). The pads are two plain ones, held through the control
  protocol; there is no Team Tap, mouse, rotary controller or light gun, and with a RISC's program
  the ports read as released. JINTCTRL latches the DSP's interrupt to the
  CPU and the two timers' (the external, the UART's and the I2S interface's are not modelled); its
  line's rise is INT1's bit 4. The UART is not modelled, nor the DSP's reads of JINTCTRL.
- **The GPU's G_HIDATA** takes the high half of the bus with every load outside the local space;
  only a LOADP from a phrase boundary drives all of it. After a narrower load, or a LOADP from
  elsewhere in a phrase, it holds what the bus carried before in the lanes not driven (`$CAFE2700`
  after a word read of `$CAFE`, the `$2700` left by the 68000's last prefetch). The core gives the
  phrase's first long word. The generator reads G_HIDATA, and STOREPs, only where a LOADP from a
  phrase boundary or a write of G_HIDATA set it, waiting for the LOADP's register first.
- **A long word at an address two past a long word boundary, read by the GPU from Jerry's ports**
  is not modelled: the simulation gives `$FFFF0001` at `$F14002`. Tom's registers are
  ([tom.md](tom.md)), and its stores to either take the two words as Tom's do. Stored to the DRAM,
  such a long word writes its four bytes from its address on, both halves of it on the bus, up to
  the phrase's end: two past, its low half and then its high half; six past, its low half alone.
- **The GPU's reads of the DSP's space** (`gpu_cpu.v` `iord`, `latrdata`) take the long word as it
  stands when Tom takes the GPU's cycle, not in Jerry's I/O cycles; the cycle they take from the
  DSP's local bus (`ioreq`, two cycles after the I/O cycle starts) is given up.
  Its writes of a byte there are not modelled, nor its reads of Jerry's other registers but the
  timers (JOYSTICK, the interrupt control, the UART), nor its writes of the UART.
- **The GPU's writes into the DSP while it runs** land in the cycle the DSP's memory controller
  serves them, but the cycle the controller gives up for them (`ioreq` holding the DSP's own
  requests back) is not modelled, nor a D_CTRL written then. A RISC stopped and started again
  starts as at its first GO, its instruction queue, decode stage and memory controller empty: what
  its pipeline left from before its stop is not carried over.
- **A write through a slave port into a running RISC** (the 68000's, the GPU's into the DSP) is
  made in the cycle the RISC's memory controller serves it, before the rest of that cycle reads
  what it writes, whichever processor the bus steps the cycle for: the RISC waits at that cycle
  until the bus can step it with its own request (`m68k_gpu_flags`). A RISC whose GO is cleared
  so issues nothing from the cycle GO falls in, as when it clears GO itself, but the instructions
  issued before still write their results in the two cycles after; stopped by its own store,
  they do not (`m68k_risc_stop`, the `random_irq` references).
- **The GPU's own stores through Tom's window onto its local RAM** (`$F0B000`, the local RAM
  `$8000` above, in Tom's 32 bit wide space) go out through the gateway and come back through Tom's
  I/O cycles to its slave port, as the blitter's writes there do: a long word each I/O cycle, a
  phrase in two, each taking the GPU's local bus for a cycle (`gpu_store_window`; Ruiner Pinball
  loads its code modules so).
- **The I2S interface** runs while the DSP is stopped, as the simulation's does; its interrupt is
  then traced, not latched by the DSP.
- **The DSP's loads and stores of Tom's space** (the GPU's RAM and registers) are not modelled.

## The 68000

The emulated 68000 of a 68000 program is [hardware/m68000.md](m68000.md)'s. For a RISC's program
only what the reference boot program leaves it doing is modelled: the end of its write of DSPGO
(AS until the third cycle after DTACK), one prefetch from ROM from cycle 8 (8 bits wide, the
slowest ROM speed), then STOP. With GPUGO, written to one of Tom's own registers, the write is over
at GO, AS rises in cycle 4 and the prefetch starts in cycle 7, the 68000's clock at the other phase.
A request of the RISC before the prefetch starts would meet the 68000 first; what the 68000 does
then (whether it starts the prefetch with the bus granted away) is assumed, not checked: the
prefetch waits for BG to be released.

## Masters

Tom's masters are the GPU's gateway, the object processor and the blitter
([0010](../decisions/0010-video-by-cycle-frames-as-references.md),
[0012](../decisions/0012-blitter-and-tom-data-bus.md)); the 68000 as the boot program leaves it, or
emulated, alone on the bus ([0013](../decisions/0013-the-68000-is-moira.md)). With DMAEN set the DSP's gateway asks for the bus on `dbrl[1]`, which the
board leaves unconnected: the DSP never gets the bus, as in the simulation.

A processor that has run ahead of the other (in the automaton, while the bus does nothing for it)
does not see what the other does to it before the bus has stepped there: the GPU's writes to the
DSP or to the timers land as the bus steps, whatever the DSP's cycle by then.
