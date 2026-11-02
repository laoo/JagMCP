#pragma once

#include "jag/Debug.hpp"
#include "jag/I2s.hpp"
#include "jag/MainBus.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jag
{

// Receives the trace of a run, in the format of the FPGA simulation's trace (sim/sim_main.cpp):
// one event per call, prefixed by the caller with the DSP cycle counted from DSPGO.
class TraceSink
{
public:
  virtual ~TraceSink() = default;
  virtual void event( std::uint64_t cycle, std::string_view text ) = 0;
};

// Memory outside the DSP's local space, as the main bus moves its data (MainBus).
class ExternalBus
{
public:
  virtual ~ExternalBus() = default;
  virtual std::uint32_t readLong( std::uint32_t address ) = 0;
  virtual std::uint16_t readWord( std::uint32_t address ) = 0;
  virtual std::uint8_t readByte( std::uint32_t address ) = 0;
  virtual void writeLong( std::uint32_t address, std::uint32_t value ) = 0;
  virtual void writeWord( std::uint32_t address, std::uint16_t value ) = 0;
  virtual void writeByte( std::uint32_t address, std::uint8_t value ) = 0;

  // The phrase at `address` (a multiple of 8), the first byte the high one.
  virtual std::uint64_t readPhrase( std::uint32_t address )
  {
    return static_cast<std::uint64_t>( readLong( address ) ) << 32 | readLong( address + 4 );
  }
};

// The RISC of Jerry (the DSP) and of Tom (the GPU): the front end and the memory controller
// stepped every cycle as the netlist's, the scoreboard kept as the cycles at which registers, flags
// and write ports become free, the datapath run at once when an instruction issues
// (docs/architecture.md). The two are one netlist with a parameter (JERRY): the microcode of six
// opcodes, the result units behind them, the local space, the interrupt vectors.
class Risc : public RiscSlave
{
public:
  enum class Chip
  {
    Dsp,
    Gpu
  };

  explicit Risc( MainBus& bus, Chip chip = Chip::Dsp );

  // The local RAM: $F1B000, 8 KB (the DSP); $F03000, 4 KB (the GPU).
  std::uint32_t ramBase() const
  {
    return mRamBase;
  }

  std::uint32_t ramSize() const
  {
    return mRamSize;
  }

  ~Risc() override;

  // The trace's events, each prefixed with `prefix` (the processor the program did not start).
  void setTrace( TraceSink* trace, std::string prefix = {} );

  // Timing from the automaton the stepped core records (docs/decisions/0007-timing-automaton.md),
  // or, off, from stepping the core alone.
  void setAutomaton( bool on );

  // Writes into the local RAM, as an external master would before the DSP is started.
  void loadLocal( std::uint32_t address, std::span<std::uint8_t const> data );

  // D_FLAGS, D_PC and D_CTRL as written by the 68000 before the DSP starts: GO in cycle 0.
  void setFlags( std::uint32_t value );
  void setPc( std::uint32_t address );
  void go();

  // Runs until the DSP clears DSPGO or the cycle `limit` is reached. Returns whether it stopped by
  // itself.
  bool run( std::uint64_t limit );

  // A run in parts, beside the other processor on the bus: the cycle the run ends at, the part up
  // to `pause` (a node of the automaton may take it past), the end. A part also ends where the
  // processor waits for the other to hand the bus a cycle. runUntil returns whether the run goes on.
  void setLimit( std::uint64_t limit );
  bool runUntil( std::uint64_t pause );
  void finish();

  // The cycle the processor is at, counted as the bus counts them.
  std::uint64_t now() const
  {
    return mCycle;
  }

  // GO was set: by go(), or by another master's write of D_CTRL.
  bool started() const
  {
    return mStarted;
  }

  // The cycle the run ended in: the stop, or the limit.
  std::uint64_t cycles() const;

  // Where a part ended because the processor waits for the other: the cycle the other is to reach
  // for it to go on (past its own when its slave port serves a write in it).
  std::uint64_t waitsFor() const
  {
    return mServeWait ? mCycle + 1 : mCycle;
  }

  // The other processor cannot get on either: the write the slave port serves now is taken after
  // the cycle, as the bus steps it.
  void serveLate()
  {
    mServeLate = true;
  }

  // The processor cleared its GO before the limit.
  bool stopped() const
  {
    return mGoFalls <= mStopAt;
  }

  // Another master's write into the local space (the slave), served in `cycle`; a read of it.
  void slaveWrite( std::uint64_t cycle, std::uint32_t address, std::uint32_t value ) override;
  void slaveRequest( std::uint64_t cycle ) override;
  std::uint32_t slaveRead( std::uint64_t cycle, std::uint32_t address ) override;

  // Register `index` of the 64, bank 0 first.
  std::uint32_t reg( int index ) const;

  // For a debugger, between runs. The program counter as the core holds it (program_count, the
  // instruction issuing or about to), FLAGS and CTRL as a read of them gives them, a byte of the
  // local RAM; a write into it drops what the automaton knew of the code there.
  std::uint32_t pc() const;
  std::uint32_t flagsRegister();
  std::uint32_t controlRegister();

  // The bank the instructions name: 1 when REGPAGE selects bank 1 and IMASK does not override it.
  unsigned bank() const
  {
    return mBank / 32;
  }

  // For a debugger, between runs: register `index` of the 64 holds `value` from now on, and the
  // flags Z, C and N (FLAGS bits 0 to 2) `flags`. A result that an instruction already issued has
  // not written yet still writes after, as through the register file's port.
  void setReg( int index, std::uint32_t value );
  void setAluFlags( std::uint32_t flags );

  bool holdsLocal( std::uint32_t address ) const
  {
    return address - mRamBase < mRamSize;
  }

  std::uint8_t localByte( std::uint32_t address ) const;
  void setLocalByte( std::uint32_t address, std::uint8_t value );

  // A debugger's probe of this processor, and the stop it shares with the others; null for none.
  // While the probe has breakpoints or watchpoints the stepped core runs alone: a run stops before
  // an instruction at a breakpoint is issued, or once a load or store a watchpoint covers is served.
  void setDebug( Debugger* debugger, Probe* probe )
  {
    mDebugger = debugger;
    mProbe = probe;
  }

  // Jerry's I2S interface (the DSP's; the GPU's is never written).
  I2s& i2s()
  {
    return mI2s;
  }

private:
  // An instruction word decoded once, with the registers it names as the scoreboard compares them
  // in the current bank: MOVETA writes and MOVEFA reads the other bank, the indexed modes read R14
  // or R15 as their destination operand.
  struct Decoded
  {
    std::uint8_t op;
    std::uint8_t src;
    std::uint8_t dst;
    std::uint32_t imm;    // quick data: the source field, 0 meaning 32
    std::uint32_t s;      // source field, in the current bank
    std::uint32_t d;      // destination field, in the current bank
    std::uint32_t alt;    // the other bank
    std::uint32_t srcReg; // the source operand
    std::uint32_t dstReg; // the destination operand
  };

  // What the execution of an instruction tells the front end.
  struct Executed
  {
    bool immld = false;      // a MOVEI: its two data words follow
    bool jumpAbs = false;    // a JUMP whose condition holds (jumpabs)
    bool jumpRel = false;    // a JR whose condition holds (jumprel)
    std::uint32_t srcdp = 0; // its source operand, loaded into arith.v's srcdp at the end of the cycle
    bool memory = false;     // a load or a store (memrw)
    bool precomp = false;    // an indexed one, its address computed first (precomp)
    bool store = false;      // a store (datwe)
  };

  // The load or store the memory controller is serving.
  struct Access
  {
    std::uint8_t op = 0;
    std::uint32_t address = 0;
    std::uint32_t data = 0; // a store's
    std::uint32_t reg = 0;  // the register loaded or stored (ild_wbaddrl)
    std::uint32_t pc = 0;   // the instruction's, for a debugger
  };

  // Register file writes of one cycle: at most two, one per port (sboard.v, hwbaddr and swbaddr).
  struct WriteSlot
  {
    std::uint64_t cycle = UINT64_MAX;
    std::uint8_t count = 0;
    std::array<std::uint8_t, 2> reg{};
    std::array<std::uint8_t, 2> priority{};
    std::array<std::uint32_t, 2> value{};
  };

  static constexpr std::size_t WRITE_SLOTS = 64;

  // The timing automaton (RiscAutomaton.cpp).
  struct PipeState;
  struct Node;
  struct Variant;
  struct Automaton;
  struct LoopData;

  enum ActionKind : std::uint8_t
  {
    EXECUTE,    // the instruction's datapath
    ACCESS,     // the memory controller serves the load or store
    STORE_DATA, // an indexed store reads its data
    IMM_LO,     // a MOVEI's data words
    IMM_HI,
    ILD_WRITE,  // a loaded value takes a write port
    DIV_RUN,    // the divider loads its operands, for `word` steps
    DIV_WRITE,  // the quotient takes a write port
    ACCUMULATE, // macop_pp in a cycle no instruction issued in
    IRQ_TRACE   // the I2S interrupt request rises
  };

  bool issuing() const;
  bool busSafeNow() const;
  bool coupled() const;
  bool objectsNear() const;
  bool blitterNear() const;
  bool headIsMmult() const;
  void capture( PipeState& state ) const;
  void restore( PipeState const& state );
  Node& nodeFor( PipeState const& state );
  std::uint64_t outcomeOf( Node const& node );
  void advance( Node& node );
  void replay( Node const& node, Variant const& variant );
  void record( ActionKind kind, std::uint16_t word = 0 );
  void flushAutomaton();
  void captureLoop( LoopData& data );
  bool skipLoop( Node& node );
  void shiftTime( std::uint64_t delta );
  void flushTrace( std::uint64_t before );
  void loopDirty();
  void divideLoad( bool offset );
  void divideCatchUp( std::uint64_t cycle );

  static Decoded decode( std::uint16_t word, std::uint32_t bank );
  std::uint16_t fetch16( std::uint32_t address ) const;
  std::uint32_t readLong( std::uint32_t address );
  void writeLong( std::uint32_t address, std::uint32_t value );
  void writeIo( std::uint32_t address, std::uint32_t value );
  std::uint32_t readIo( std::uint32_t address );
  bool condition( std::uint32_t cc );

  std::uint32_t cycle();
  std::uint32_t quietCycles() const;
  std::uint32_t busWait( bool xldReady, bool ioreq );
  void access();
  void clockBus();
  void divide();
  void accumulate( std::uint16_t word );
  std::uint16_t insertedWord( std::uint32_t index ) const;
  std::uint32_t debugBits();
  std::uint32_t valueAt( std::uint32_t globalReg, std::uint64_t cycle ) const;
  bool canIssue( Decoded const& in, bool insexe, std::uint64_t t ) const;
  using Exec = Executed ( * )( Risc&, Decoded const&, std::uint32_t );
  template <std::uint8_t OP, bool GPU>
  Executed executeOp( Decoded const& in, std::uint32_t pc );
  template <std::uint8_t OP, bool GPU>
  static Executed thunk( Risc& core, Decoded const& in, std::uint32_t pc );

  template <bool GPU, std::size_t... I>
  static constexpr std::array<Exec, 64> execTable( std::index_sequence<I...> )
  {
    return { &Risc::thunk<static_cast<std::uint8_t>( I ), GPU>... };
  }

  static std::array<std::array<Exec, 64>, 2> const EXEC; // executeOp by chip (GPU 1) and opcode
  // A chain of simple nodes replayed by tail calls, one function an opcode so that each indirect
  // jump to the next node has a site of its own for the branch predictor (threaded code).
  using Chain = Node const* ( * )( Risc&, Node const* );
  template <std::uint8_t OP, bool GPU>
  static Node const* chain( Risc& core, Node const* node );

  template <bool GPU, std::size_t... I>
  static constexpr std::array<Chain, 64> chainTable( std::index_sequence<I...> )
  {
    return { &Risc::chain<static_cast<std::uint8_t>( I ), GPU>... };
  }

  static std::array<std::array<Chain, 64>, 2> const CHAIN;
  bool isLocal( std::uint32_t address ) const;
  std::uint8_t addressClass( std::uint32_t address ) const;
  Executed execute( Decoded const& in, std::uint32_t pc );
  std::uint32_t programCount() const;
  WriteSlot& slotAt( std::uint64_t cycle );
  WriteSlot const* writesAt( std::uint64_t cycle ) const;
  void writeReg( std::uint32_t globalReg, std::uint32_t value, std::uint64_t at, std::uint8_t priority );
  void writeFlags( std::uint32_t flags );
  void loadFlags( std::uint64_t at, std::uint32_t value, bool write );
  std::uint32_t commitFlags( std::uint64_t cycle );

  std::uint32_t flagsAt( std::uint64_t cycle )
  {
    return mFlagsPendingCount ? commitFlags( cycle ) : mFlags;
  }

  // An event of the trace, made before the processor stops (`end`, if given: the cycle events end
  // at instead).
  void emit( std::uint64_t cycle, std::string_view text, std::uint64_t end = 0 );

  // The cycle the register writes of what issued before the stop end at: with GO cleared through
  // the slave port, two after it falls (m68k_risc_stop), the run's end where it ends first; with
  // GO cleared by the processor's own store, as it falls (random_irq).
  std::uint64_t writesEnd() const
  {
    return mPortStop && mGoFalls <= mStopAt ? std::min( mGoFalls + 2, mLimit ) : mStopAt;
  }

  // A debugger's: the stop at a breakpoint before the instruction issuing now, or at a watchpoint
  // the access being served meets; whether the run stops here.
  bool breakHere();
  void watchAccess( Access const& a );
  void debugStop( DebugStop stop );

  void start( std::uint64_t cycle );
  void restart( std::uint64_t cycle );
  void i2sCatchUp( std::uint64_t cycle );

  MainBus& mBus;
  Timers& mTimers; // the timers that interrupt it: Jerry's for the DSP, Tom's PIT for the GPU
  TraceSink* mTrace = nullptr;
  Debugger* mDebugger = nullptr;
  Probe* mProbe = nullptr;
  std::string mTracePrefix;
  // events kept back from the sink until no waiting loop can be skipped across them
  std::vector<std::pair<std::uint64_t, std::string>> mTraceBuffer;
  std::unique_ptr<Automaton> mAuto; // null when stepping alone
  // the chip: the GPU's or the DSP's local space, its registers' and its interrupt vectors' place
  bool const mGpu;
  Master const mMaster;
  std::uint32_t const mRamBase;
  std::uint32_t const mRamSize;
  std::uint32_t const mIoBase;
  std::uint32_t const mIoEnd;
  static constexpr std::uint32_t RAM_MAX = 0x2000;

  std::array<std::uint8_t, RAM_MAX / 4> mCodeMark{}; // local RAM long words any node fetched

  std::array<std::uint8_t, RAM_MAX> mRam{};

  std::array<std::uint32_t, 64> mRegs{};
  bool mRegpage = false; // D_FLAGS REGPAGE
  bool mImask = false;
  std::uint32_t mBank = 0; // reghigh: 32 when REGPAGE selects bank 1 and IMASK does not override it
  // interrupt.v
  std::uint32_t mIntEna = 0;      // int_ena: D_FLAGS bits 4 to 8 and 16
  std::uint32_t mIntEnaNext = 0;  // as written to D_FLAGS, taken at the end of the cycle
  std::uint32_t mIlatch = 0;      // ilatch: the requests taken
  std::uint32_t mIlatchClear = 0; // the latches a write to D_FLAGS clears in this cycle
  bool mImaskClear = false;       // a write to D_FLAGS clears IMASK in this cycle
  bool mIntser = false;           // intser: the decode stage takes the inserted sequence
  std::uint32_t mIcount = 0;      // icount: the next word of it
  bool mCpuIrq = false;           // gpu_irq[0], from a write of D_CTRL bit 2
  bool mCpuIrqNext = false;
  // ins_exec.v, what makes an interrupt wait (atomic)
  bool mAwaitIdle = true; // idle, await1, await2: a MOVEI and its two data words being taken
  bool mAwait1 = false;
  bool mAwait2 = false;
  bool mMulta = false; // multa: within a run of IMULTN and IMACN
  I2s mI2s;
  bool mDmaen = false; // gateway.v dmaen: D_FLAGS bit 15

  // arith.v's flag registers (Z bit 0, C bit 1, N bit 2): the value visible now and the values
  // about to be, each from the cycle it is visible. An instruction's flags are visible two cycles
  // after it issues (flagldp), a write to D_FLAGS from the cycle after it is served (flagwr), and
  // wins over flags visible from the same cycle. Committed when read (`flagsAt`).
  struct FlagsPending
  {
    std::uint64_t at;
    std::uint32_t value;
    bool write;
  };

  std::uint32_t mFlags = 0;
  std::array<FlagsPending, 4> mFlagsPending{};
  std::uint32_t mFlagsPendingCount = 0;
  std::uint32_t mMod = 0;        // D_MOD
  bool mDivOffset = false;       // divider.v div_offset: D_DIVCTRL bit 0, the 16.16 divide
  bool mDivOffsetNext = false;   // as written, taken at the end of the cycle
  std::uint32_t mResult = 0;     // arith.v resreg: the last result loaded (resld, macop_pp), what an ALU
                                 // write-back writes, the accumulator's low 32 bits
  std::uint32_t mAccTop = 0;     // arith.v acctop: the accumulator's bits 32 to 39
  std::uint64_t mMacAt = 0;      // the cycle of macop_pp: an IMACN's product is accumulated in it
  std::uint32_t mMacProduct = 0; // mulqp: that product
  bool mGo = false;
  bool mStarted = false;
  bool mBlocked = false;               // the cycle waits for the other processor to hand the bus what it needs
  std::uint64_t mServeAt = UINT64_MAX; // the cycle the slave port serves a write in (ioserv)
  bool mServeWait = false;             // the cycle waits for the other to hand the bus that cycle
  bool mServeLate = false;             // ... which it cannot: the write is taken after it
  bool mSlaveWriting = false;          // a write through the slave port is being made
  bool mPortStop = false;              // GO was cleared through the slave port
  std::vector<std::uint64_t> mIoreqs;  // the cycles the slave takes the local bus in, to come

  // CTRL and FLAGS as another master's read sees them in a cycle, kept for the last few cycles while
  // the 68000 reaches for the local space: its read is served as the bus steps that cycle, which
  // may be in the middle of the processor's making it
  struct Status
  {
    std::uint64_t cycle = UINT64_MAX;
    std::uint32_t ctrl = 0;
    std::uint32_t flags = 0;
  };

  std::array<Status, 4> mStatus{};

  // Every time is a cycle as the bus counts them, from the GO of the processor the program runs on.
  std::uint64_t mCycle = 0;
  std::uint64_t mIssue = 0;               // cycle of the instruction being executed
  std::array<std::uint64_t, 64> mReady{}; // cycle from which a register reads its new value
  std::array<std::uint32_t, 64> mOld{};   // a register's value before its last write, read by an
                                          // operand the scoreboard does not wait for
  std::array<WriteSlot, WRITE_SLOTS> mWrites{};
  std::uint64_t mFlagsSet = 0;         // cycle of the last instruction that set the flags + 1
  std::uint64_t mGoFalls = UINT64_MAX; // DSPGO falls in this cycle
  std::uint64_t mStopAt = UINT64_MAX;  // the run ends in this cycle: DSPGO falls, or the limit
  std::uint64_t mLimit = UINT64_MAX;   // the limit
  std::uint64_t mPause = UINT64_MAX;   // where the part of the run under way ends (runUntil)
  std::uint64_t mI2sAt = 0;            // while stopped: the cycle the I2S interface has reached

  // The front end, stepped every cycle: the instruction queue of prefetch.v (its registers, the
  // words as they were read when fetched), pc.v, execon.v, and the program side of gpu_mem.v.
  std::uint32_t mInsdata = 0;   // insdata: the queue's head and the word after it, as fetched
  std::uint32_t mPfdata = 0;    // pfdata: the last long word fetched
  std::uint32_t mQueueSize = 0; // qs: the words queued
  std::uint32_t mFetchPc = 0;   // pc_obuf: the word address of the next fetch
  bool mProgServ = false;       // progserv: a fetch is served this cycle
  bool mXprog = false;          // xprog: a fetch outside the local space waits for the gateway
  bool mXpabort = false;        // xpabort: and a jump has made it void
  bool mGateServ = false;       // gateserv: the gateway hands the local bus a program long word
  std::uint32_t mProgData = 0;  // its prog_data
  bool mIdle = true;
  bool mExec = false;
  bool mVins = false; // the decode stage holds a valid instruction
  bool mImm1 = false;
  bool mImm2 = false;
  bool mInsrdyp = false;
  bool mJat2 = false; // a JUMP waits for its delay slot instruction
  bool mJrt2 = false; // a JR waits for its delay slot instruction
  bool mJrel = false;
  std::uint16_t mDecode = 0;   // the instruction in the decode stage
  Decoded mDecoded{};          // decoded, in the bank of the cycle (redone when the bank changes)
  bool mIoWrite = false;       // a write to an I/O register is served in this cycle
  std::uint32_t mImmCount = 0; // data words of a MOVEI still to come
  std::uint32_t mImmValue = 0;
  std::uint32_t mImmDst = 0;
  std::uint32_t mJumpValue = 0; // srcd of the JUMP taken, its register
  std::uint32_t mSrcdp = 0;     // registers.v srcdp: the source operand of the last instruction issued

  // The data side of the memory controller, stepped every cycle: the load and store states of
  // sboard.v, compdwait of execon.v, the data request of gpu_mem.v.
  Access mAccess{};
  bool mMemIdle = true;     // idle
  bool mComp1 = false;      // an indexed address is computed
  bool mComp2 = false;      // and requested
  bool mLoading = false;    // a request waits to be served
  bool mDrqt2 = false;      // drqt[2]: the request is held
  bool mDatServ = false;    // datserv: the request is served this cycle
  bool mCompdwait = false;  // an indexed store reads its data
  bool mIldPending = false; // ild_wback: a loaded value waits for a write port
  std::uint32_t mIldValue = 0;

  // Loads and stores outside the local space: the gateway's request of this cycle, and sboard.v's
  // external loads, two at most under way (oneld, twold), written back in the order they were
  // served (xlddst, xlddst2, xldd_sel).
  GatewayRequest mRequest{};
  bool mBusBusy = false;        // the main bus is clocked with the DSP: a request of it is under way
  bool mTimerWrite = false;     // and a write to the timers' chip's registers among it
  std::uint64_t mTimerNear = 0; // the cycle from which a timer may request within a span
  // the first cycle the object processor may interrupt the GPU in, as the bus last told it; the GPU
  // replays no span that reaches it
  mutable std::uint64_t mObjectQuiet = 0;

  bool objectsWithin( std::uint64_t span ) const
  {
    return mGpu && ( mIntEna & 8u ) && mCycle + span > mObjectQuiet;
  }

  bool mGateActive = false;
  bool mOneldi = false;
  bool mTwoldi = false;
  bool mOneld = false;
  bool mTwold = false;
  bool mXlddSel = false;
  bool mXldWbackl = false;
  bool mXldWback = false; // this cycle's xld_wback, which the divider gives way to
  std::uint32_t mXlddst = 0;
  std::uint32_t mXlddst2 = 0;
  std::uint32_t mXldData = 0;

  std::uint16_t mOpcodeWord = 0; // ins_exec.v opcode: the last word the decode stage took, a MOVEI's
                                 // data words as well

  // systolic.v: MMULT has the decode stage take IMULTN, IMACN until the count is out, RESMAC
  // (sysins), the vector in bank 1's registers a half at a time, the matrix read from local RAM
  // through the data request (mtx_mreq, mtx_dover) in the cycle each instruction issues.
  bool mSysIdle = true;
  bool mSysImultn = false;
  bool mSysImacn = false;
  bool mSysResmac = false;
  std::uint32_t mMcount = 0;  // the words still to insert
  std::uint32_t mSysR1 = 0;   // {the vector's register, reghalf}
  std::uint32_t mSysR2 = 0;   // the destination register
  std::uint32_t mMwidth = 0;  // D_MTXC bits 0 to 3: the matrix's width
  bool mMaddw = false;        // D_MTXC bit 4: the matrix read by column
  std::uint32_t mMtxAddr = 0; // D_MTXA bits 2 to 11
  bool mMtxDover = false;     // mtx_dover: a matrix word is requested
  bool mDatMtx = false;       // the request served this cycle is the matrix's (dataddr from mtx_addr)
  bool mMultsel = false;      // the vector's high half
  bool mDecodeMtx = false;    // the decode stage's instruction reads bank 1 (srctopi, mtx_doveri)
  std::uint32_t mMtxData = 0; // the matrix word served this cycle (mem_data)
  bool mMtxIssue = false;     // the instruction issuing takes it as its destination operand
  bool mMultselIssue = false;

  // divider.v, stepped every cycle it is active: two quotient bits a cycle, non restoring.
  std::uint32_t mDivisor = 0;
  std::uint32_t mDividendHi = 0; // the remainder as it is worked out, D_REMAIN
  std::uint32_t mDividendLo = 0;
  std::uint32_t mQuotient = 0;
  bool mLastNegN = true;
  std::uint32_t mDivStepsLeft = 0; // data steps still to take, each at the end of a cycle from
  std::uint64_t mDivStepAt = 0;    // this one on; taken when the data is read (`divideCatchUp`)
  std::uint32_t mDivCount = 0;
  bool mDivActive = false;
  bool mDivStart = false;    // a DIV issued this cycle
  bool mDivPending = false;  // div_wback: the quotient waits for a write port
  std::uint32_t mDivReg = 0; // div_wbaddr
  std::uint32_t mDivSrc = 0;
  std::uint32_t mDivDst = 0;
};

} // namespace jag
