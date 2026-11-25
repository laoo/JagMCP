// The timing automaton (docs/decisions/0007-timing-automaton.md): the pipeline's control state at
// each issue cycle as a node, what the stepped core did from there for each outcome of the data the
// timing depends on as the node's variants, replayed with the datapath and the data that keeps
// stepping (the register file and its calendar, the flags, the divider, the I2S interface).

#include "RiscAutomaton.hpp"

#include <algorithm>

namespace jag
{

using namespace risc;

Risc::Risc( MainBus& bus, Chip chip )
    : mBus{ bus }, mTimers{ chip == Chip::Gpu ? bus.tom().pit() : bus.timers() },
      mAuto{ std::make_unique<Automaton>() }, mGpu{ chip == Chip::Gpu }, mMaster{ mGpu ? Master::Gpu : Master::Dsp },
      mRamBase{ mGpu ? GPU_RAM : DSP_RAM }, mRamSize{ mGpu ? GPU_RAM_SIZE : DSP_RAM_SIZE },
      mIoBase{ mGpu ? GPU_IO : DSP_IO }, mIoEnd{ mGpu ? GPU_IO_END : DSP_IO_END }
{
}

// The address classes of a load or store, as the memory controller tells them apart.
std::uint8_t Risc::addressClass( std::uint32_t address ) const
{
  if ( address - mRamBase < mRamSize || ( !mGpu && address - ROM_BASE < 0x1000 ) )
    return CLASS_LOCAL;
  if ( address >= mIoBase && address < mIoEnd )
    return CLASS_IO;
  return CLASS_EXTERNAL;
}

Risc::~Risc() = default;

void Risc::setAutomaton( bool on )
{
  if ( on && !mAuto )
    mAuto = std::make_unique<Automaton>();
  else if ( !on )
    mAuto.reset();
}

// A write into code a node fetched: the automaton is discarded, once nothing is replaying or
// recording out of it.
void Risc::flushAutomaton()
{
  loopDirty();
  if ( mAuto )
    mAuto->flushWanted = true;
}

void Risc::record( ActionKind kind, std::uint16_t word )
{
  if ( mAuto && mAuto->recVariant )
  {
    std::uint64_t const offset = mCycle - mAuto->recStart;
    if ( offset < MAX_SPAN )
      mAuto->recVariant->actions.push_back( kind | static_cast<std::uint32_t>( offset ) << 8 |
                                            static_cast<std::uint32_t>( word ) << 16 );
  }
}

// The control state, times relative to this cycle. A field that steers nothing in the state (the
// register of an access while none is under way) is left zero, so that it does not tell states apart.
void Risc::capture( PipeState& s ) const
{
  s = PipeState{};
  std::uint64_t const c = mCycle;
  auto const rel = [c]( std::uint64_t t ) { return t > c ? t - c : 0ull; };
  s.fetchPc = mFetchPc;
  s.insdata = mInsdata;
  s.pfdata = mPfdata;
  s.jumpValue = mJat2 ? mJumpValue : 0;
  s.srcdp = mJrel ? mSrcdp : 0;
  s.ilatch = mIlatch;
  s.intEna = mIntEna;
  s.intEnaNext = mIntEnaNext;
  s.ilatchClear = mIlatchClear;
  s.immCount = mImmCount;
  s.immDst = mImmCount ? mImmDst : 0;
  bool const accessing = !mMemIdle || mIldPending || mDatServ || mCompdwait;
  s.accessReg = accessing ? mAccess.reg : 0;
  s.accessOp = accessing ? mAccess.op : 0;
  s.accessClass = accessing ? addressClass( mAccess.address ) : 0;
  s.divReg = mDivActive || mDivPending ? mDivReg : 0;
  s.decode = mDecode;
  s.opcodeWord = mOpcodeWord;
  s.queueSize = static_cast<std::uint8_t>( mQueueSize );
  s.icount = static_cast<std::uint8_t>( mIcount );
  s.divCount = static_cast<std::uint8_t>( mDivCount );
  s.bank = static_cast<std::uint8_t>( mBank );
  s.flagsSet = clamp8( rel( mFlagsSet + 1 ) );
  s.macAt = clamp8( rel( mMacAt ) );
  for ( std::size_t r = 0; r < 64; ++r )
    s.ready[r] = clamp8( rel( mReady[r] ) );
  for ( std::uint64_t t = c; t < c + CALENDAR_SPAN; ++t )
    if ( WriteSlot const* w = writesAt( t ) )
      for ( std::uint8_t i = 0; i < w->count; ++i )
      {
        if ( s.calendarCount == MAX_CALENDAR )
        {
          s.tooMany = 1;
          break;
        }
        s.calendar[s.calendarCount][0] = static_cast<std::uint8_t>( t - c );
        s.calendar[s.calendarCount][1] = w->reg[i];
        s.calendar[s.calendarCount][2] = w->priority[i];
        ++s.calendarCount;
      }
  s.progServ = mProgServ;
  s.exec = mExec;
  s.vins = mVins;
  s.imm1 = mImm1;
  s.imm2 = mImm2;
  s.insrdyp = mInsrdyp;
  s.jat2 = mJat2;
  s.jrt2 = mJrt2;
  s.jrel = mJrel;
  s.ioWrite = mIoWrite;
  s.memIdle = mMemIdle;
  s.comp1 = mComp1;
  s.comp2 = mComp2;
  s.loading = mLoading;
  s.drqt2 = mDrqt2;
  s.datServ = mDatServ;
  s.compdwait = mCompdwait;
  s.ildPending = mIldPending;
  s.divActive = mDivActive;
  s.divPending = mDivPending;
  s.divOffset = mDivOffset;
  s.divOffsetNext = mDivOffsetNext;
  s.intser = mIntser;
  s.imask = mImask;
  s.imaskClear = mImaskClear;
  s.cpuIrq = mCpuIrq;
  s.cpuIrqNext = mCpuIrqNext;
  s.awaitIdle = mAwaitIdle;
  s.await1 = mAwait1;
  s.await2 = mAwait2;
  s.multa = mMulta;
  s.regpage = mRegpage;
  s.pendingEdge = mI2s.rose();
  for ( std::uint32_t i = 0; i < mFlagsPendingCount; ++i )
    s.flagsPending[i] =
        static_cast<std::uint8_t>( clamp8( rel( mFlagsPending[i].at ) ) | ( mFlagsPending[i].write ? 0x80 : 0 ) );
}

// The control state back into the live members, for the stepped core to go on from. What the data
// side keeps live (the register file and its calendar, the flags, the divider's and the I2S
// interface's data, the accumulator) is not touched.
void Risc::restore( PipeState const& s )
{
  mFetchPc = s.fetchPc;
  mInsdata = s.insdata;
  mPfdata = s.pfdata;
  mJumpValue = s.jumpValue;
  mSrcdp = s.srcdp;
  mIlatch = s.ilatch;
  mIntEna = s.intEna;
  mIntEnaNext = s.intEnaNext;
  mIlatchClear = s.ilatchClear;
  mImmCount = s.immCount;
  mImmDst = s.immDst;
  mAccess.reg = s.accessReg;
  mAccess.op = s.accessOp;
  mDivReg = s.divReg;
  mDecode = s.decode;
  mOpcodeWord = s.opcodeWord;
  mQueueSize = s.queueSize;
  mIcount = s.icount;
  mDivCount = s.divCount;
  mBank = s.bank;
  mDecoded = decode( mDecode, mBank );
  mProgServ = s.progServ;
  mIdle = false;
  mExec = s.exec;
  mVins = s.vins;
  mImm1 = s.imm1;
  mImm2 = s.imm2;
  mInsrdyp = s.insrdyp;
  mJat2 = s.jat2;
  mJrt2 = s.jrt2;
  mJrel = s.jrel;
  mIoWrite = s.ioWrite;
  mMemIdle = s.memIdle;
  mComp1 = s.comp1;
  mComp2 = s.comp2;
  mLoading = s.loading;
  mDrqt2 = s.drqt2;
  mDatServ = s.datServ;
  mCompdwait = s.compdwait;
  mIldPending = s.ildPending;
  mDivActive = s.divActive;
  mDivStart = false;
  mDivPending = s.divPending;
  mDivOffset = s.divOffset;
  mDivOffsetNext = s.divOffsetNext;
  mIntser = s.intser;
  mImask = s.imask;
  mImaskClear = s.imaskClear;
  mCpuIrq = s.cpuIrq;
  mCpuIrqNext = s.cpuIrqNext;
  mAwaitIdle = s.awaitIdle;
  mAwait1 = s.await1;
  mAwait2 = s.await2;
  mMulta = s.multa;
  mRegpage = s.regpage;
  divideCatchUp( mCycle ? mCycle - 1 : 0 );
}

// --- waiting loops --------------------------------------------------------------------------------

namespace
{
constexpr std::uint64_t MAX_LOOP = 256; // cycles an iteration of a waiting loop may take
}

void Risc::loopDirty()
{
  if ( mAuto )
    mAuto->loopDirty = true;
}

void Risc::captureLoop( LoopData& d )
{
  d = LoopData{};
  divideCatchUp( mCycle ? mCycle - 1 : 0 );
  d.regs = mRegs;
  d.old = mOld;
  std::size_t n = 0;
  for ( std::uint64_t t = mCycle; t < mCycle + CALENDAR_SPAN && n < MAX_CALENDAR; ++t )
    if ( WriteSlot const* w = writesAt( t ) )
      for ( std::uint8_t i = 0; i < w->count && n < MAX_CALENDAR; ++i )
        d.calendar[n++] = w->value[i];
  for ( std::uint32_t i = 0; i < mFlagsPendingCount; ++i )
    d.flagsPending[i] = mFlagsPending[i].value;
  d.flags = mFlags;
  d.result = mResult;
  d.accTop = mAccTop;
  d.macProduct = mMacProduct;
  d.mod = mMod;
  d.divisor = mDivisor;
  d.dividendHi = mDividendHi;
  d.dividendLo = mDividendLo;
  d.quotient = mQuotient;
  d.divStepsLeft = mDivStepsLeft;
  d.ildValue = mIldValue;
  d.accessAddress = mAccess.address;
  d.accessData = mAccess.data;
  d.immValue = mImmValue;
  d.jumpValue = mJumpValue;
  d.srcdp = mSrcdp;
  d.lastNegN = mLastNegN ? 1u : 0u;
}

// Every time of the core `delta` cycles on, as the cycles between are skipped: the pipeline in the
// same state, relative to the new cycle, the I2S interface brought along.
void Risc::shiftTime( std::uint64_t delta )
{
  std::uint64_t const c = mCycle;
  auto const shift = [c, delta]( std::uint64_t& t )
  {
    if ( t >= c && t != UINT64_MAX )
      t += delta;
  };
  for ( std::uint64_t& t : mReady )
    shift( t );
  shift( mFlagsSet );
  for ( std::uint32_t i = 0; i < mFlagsPendingCount; ++i )
    shift( mFlagsPending[i].at );
  shift( mMacAt );
  shift( mDivStepAt );
  std::vector<WriteSlot> planned;
  for ( WriteSlot& slot : mWrites )
    if ( slot.cycle != UINT64_MAX && slot.cycle >= c )
    {
      planned.push_back( slot );
      slot.cycle = UINT64_MAX;
    }
  for ( WriteSlot slot : planned )
  {
    slot.cycle += delta;
    mWrites[slot.cycle % WRITE_SLOTS] = slot;
  }
  mI2s.advance( delta );
  mTimerNear = 0;
  mCycle += delta;
}

// At a jump node: a loop whose data comes back the same, with nothing written to memory, no I/O
// and nothing of the main bus's in an iteration, waits for an event; its iterations are skipped up to
// the next (the I2S line's next rise, a timer's request, the end of the run), their trace events
// repeated. Returns whether it skipped.
bool Risc::skipLoop( Node& node )
{
  Automaton& a = *mAuto;
  std::uint64_t const seen = node.loopSeen;
  node.loopSeen = mCycle;
  if ( a.loopHead == &node && !a.loopDirty && mCycle - a.loopCycle <= MAX_LOOP && mRegs == a.loopData.regs &&
       !mTimerWrite )
  {
    if ( mBusBusy )
    {
      // replay leaves the bus behind: it may have finished the DSP's work since
      mBusBusy = !mBus.catchUp( mMaster, mCycle ) || !mBus.idle( mMaster );
    }
    flagsAt( mCycle );
    // armed with the registers alone: the rest is taken now, and compared at the next visit
    if ( !a.loopFull )
    {
      a.loopCycle = mCycle;
      a.loopFull = true;
      captureLoop( a.loopData );
      return false;
    }
    LoopData now;
    captureLoop( now );
    if ( now == a.loopData && !mBusBusy ) // the interrupt controller is in the node
    {
      std::uint64_t const period = mCycle - a.loopCycle;
      std::uint64_t horizon = mStopAt;
      std::uint64_t const rise = mI2s.interruptChangeIn( mCycle );
      if ( rise != UINT64_MAX )
        horizon = std::min( horizon, mCycle + rise );
      horizon = std::min( horizon, mTimers.nextRequest( mCycle ) );
      if ( mGpu && ( mIntEna & 8u ) ) // up to the first cycle of the object processor's interrupt
        horizon = std::min( horizon, mObjectQuiet );
      // not past the part: the 68000 may reach into the local space after it, and a run without a
      // limit (the control API's, the window's) would skip for ever where nothing else ends the wait
      horizon = std::min( horizon, mPause );
      std::uint64_t const k = horizon > mCycle ? ( horizon - mCycle ) / period : 0;
      if ( k >= 2 )
      {
        std::uint64_t const delta = ( k - 1 ) * period;
        if ( mTrace )
        {
          // an iteration's events, repeated; those planned beyond it move with the time. A flags
          // event is a change from the cycle before: the iteration's are those after its first
          // cycle, up to the one it ends in (committed above)
          std::vector<std::pair<std::uint64_t, std::string>> once;
          for ( auto& e : mTraceBuffer )
          {
            bool const flags = e.second[0] == 'F';
            std::uint64_t const from = flags ? a.loopCycle + 1 : a.loopCycle;
            std::uint64_t const to = flags ? mCycle + 1 : mCycle;
            if ( e.first >= from && e.first < to )
              once.push_back( e );
            else if ( e.first >= to )
              e.first += delta;
          }
          for ( std::uint64_t j = 1; j < k; ++j )
            for ( auto const& e : once )
              if ( e.first + j * period < mStopAt )
                mTraceBuffer.emplace_back( e.first + j * period, e.second );
        }
        shiftTime( delta );
        a.loopCycle = mCycle;
        return true;
      }
    }
  }
  else if ( a.loopHead && a.loopHead != &node && !a.loopDirty && mCycle - a.loopCycle <= MAX_LOOP &&
            !( seen != UINT64_MAX && seen > a.loopCycle ) )
    return false; // another jump within the loop; one seen twice since the head is the loop's
  a.loopHead = &node;
  a.loopCycle = mCycle;
  a.loopDirty = false;
  a.loopFull = false;
  a.loopData.regs = mRegs;
  return false;
}

// An MMULT in the decode stage or the queue: the sequence it starts is stepped.
bool Risc::headIsMmult() const
{
  if ( mDecoded.op == MMULT )
    return true;
  std::uint32_t const words[4] = { mInsdata >> 16, mInsdata & 0xffffu, mPfdata >> 16, mPfdata & 0xffffu };
  for ( std::uint32_t i = 0; i < 4; ++i )
    if ( words[i] >> 10 == MMULT )
      return true;
  return false;
}

// Whether the instruction issuing now would make a node whose timing does not depend on the main
// bus (Node::busSafe), told from the live state.
bool Risc::busSafeNow() const
{
  std::uint8_t const op = mDecoded.op;
  if ( op == MOVEI || op == JR || op == JUMP || isMemory( op, mGpu ) || mImmCount != 0 || mJat2 || mJrt2 || mJrel )
    return false;
  if ( !mIntser && mQueueSize == 0 )
    return false;
  std::uint16_t const word =
      mIntser ? insertedWord( mIcount ) : static_cast<std::uint16_t>( ( mQueueSize & 1 ) ? mInsdata : mInsdata >> 16 );
  return !isMemory( word >> 10, mGpu );
}

Risc::Node& Risc::nodeFor( PipeState const& s )
{
  Node*& slot = mAuto->nodes[s];
  if ( !slot )
  {
    slot = &mAuto->nodeStore.emplace_back();
    slot->entry = s;
    slot->decoded = decode( s.decode, s.bank );
    slot->exec = EXEC[mGpu][slot->decoded.op];
    slot->chain = CHAIN[mGpu][slot->decoded.op];
    slot->pc = ( ( s.fetchPc - s.queueSize - 1 ) & 0x7fffff ) << 1;
    std::uint8_t const op = slot->decoded.op;
    slot->kind = op == JR ? KIND_JR : op == JUMP ? KIND_JUMP : isMemory( op, mGpu ) ? KIND_MEMORY : KIND_NONE;
    slot->mmult = op == MMULT || s.insdata >> 26 == MMULT || ( ( s.insdata >> 10 ) & 63u ) == MMULT ||
                  s.pfdata >> 26 == MMULT || ( ( s.pfdata >> 10 ) & 63u ) == MMULT;
    slot->simple = slot->kind == KIND_NONE && !slot->mmult;
    // the instruction that issues next: the word the decode stage takes now, the inserted
    // sequence's while an interrupt is entered; not known while a jump waits for its delay slot
    if ( slot->simple && op != MOVEI && s.immCount == 0 && ( s.intser || s.queueSize != 0 ) && !s.jat2 && !s.jrt2 &&
         !s.jrel )
    {
      std::uint16_t const word = s.intser
                                     ? insertedWord( s.icount )
                                     : static_cast<std::uint16_t>( ( s.queueSize & 1 ) ? s.insdata : s.insdata >> 16 );
      slot->busSafe = !isMemory( word >> 10, mGpu );
    }
  }
  return *slot;
}

// The data the timing of the instruction about to issue depends on, read as the datapath will: a
// jump's condition and target, a load or store's address class and, for an I/O register, the
// value written, and the cycle the I2S interrupt line next changes.
std::uint64_t Risc::outcomeOf( Node const& node )
{
  Decoded const& in = node.decoded;
  std::uint64_t o = 0;
  switch ( node.kind )
  {
  case KIND_JUMP:
    if ( condition( in.dst ) )
      o = 1 | static_cast<std::uint64_t>( mRegs[in.s] & 0xffffff ) << 8;
    break;
  case KIND_JR:
    o = condition( in.dst ) ? 1 : 0;
    break;
  case KIND_MEMORY:
  {
    bool const indexed = in.op == LOAD14N || in.op == LOAD15N || in.op == STORE14N || in.op == STORE15N ||
                         in.op == LOAD14R || in.op == LOAD15R || in.op == STORE14R || in.op == STORE15R;
    std::uint32_t address = mRegs[in.s];
    if ( indexed )
    {
      // the ALU adds the base and the offset; right after an IMACN it adds resreg and the product
      std::uint32_t const offset =
          in.op == LOAD14N || in.op == LOAD15N || in.op == STORE14N || in.op == STORE15N ? in.imm * 4 : mRegs[in.s];
      address = mMacAt == mCycle + 1 ? mResult + mMacProduct : mRegs[in.dstReg] + offset;
    }
    std::uint8_t const cls = addressClass( address );
    o = cls;
    if ( cls == CLASS_IO )
    {
      o |= static_cast<std::uint64_t>( ( address >> 2 ) & 0x3f ) << 8;
      if ( isStore( in.op, mGpu ) )
      {
        std::uint32_t const value = indexed ? valueAt( in.d, mCycle + 1 ) : mRegs[in.d];
        switch ( ( address & ~3u ) - mIoBase )
        {
        case REG_FLAGS:
          o |= static_cast<std::uint64_t>( value & 0x3ffff ) << 16;
          break;
        case REG_CTRL:
          o |= static_cast<std::uint64_t>( value & 5 ) << 16;
          break;
        case REG_REMAIN:
          o |= static_cast<std::uint64_t>( value & 1 ) << 16;
          break;
        default:
          break;
        }
      }
    }
    break;
  }
  default:
    break;
  }
  // the interrupt requests to come: the I2S line's next rise, each timer's next carry
  std::uint64_t const change = mI2s.interruptChangeIn( mCycle );
  o |= ( change < MAX_SPAN ? change : MAX_SPAN ) << RISE_SHIFT;
  for ( std::uint32_t timer = 0; timer < 2; ++timer )
  {
    std::uint64_t const next = mTimers.requestIn( mCycle, timer );
    o |= ( next < MAX_SPAN ? next : MAX_SPAN ) << ( TIMER_SHIFT + 8 * timer );
  }
  return o;
}

void Risc::replay( Node const& node, Variant const& variant )
{
  std::uint64_t const start = mCycle;
  for ( std::uint32_t const action : variant.actions )
  {
    mCycle = start + ( ( action >> 8 ) & 0xff );
    if ( mCycle >= mStopAt )
      break;
    std::uint16_t const word = static_cast<std::uint16_t>( action >> 16 );
    switch ( static_cast<ActionKind>( action & 0xff ) )
    {
    case EXECUTE:
      mIssue = mCycle;
      node.exec( *this, node.decoded, node.pc );
      break;
    case ACCESS:
      access();
      break;
    case STORE_DATA:
      mAccess.data = valueAt( mAccess.reg, mCycle );
      break;
    case IMM_LO:
      mImmValue = word;
      break;
    case IMM_HI:
      writeReg( mImmDst, mImmValue | static_cast<std::uint32_t>( word ) << 16, mCycle + 1, IMM_PRIORITY );
      break;
    case ILD_WRITE:
      writeReg( mAccess.reg, mIldValue, mCycle + 1, ILD_PRIORITY );
      mIldPending = false;
      break;
    case DIV_RUN:
      mDivStepsLeft = word & 0xff;
      divideLoad( word & 0x100 );
      break;
    case DIV_WRITE:
      divideCatchUp( mCycle );
      writeReg( mDivReg, mQuotient, mCycle + 1, DIV_PRIORITY );
      break;
    case ACCUMULATE:
      accumulate( word );
      break;
    case IRQ_TRACE:
      emit( mCycle, "IRQ i2s" );
      break;
    }
  }
  mI2s.advance( variant.span );
  mCycle = std::min( start + variant.span, mStopAt );
}

// From a node: replay the variant of this outcome, or record it, or step through it when it is
// not replayable. Leaves `current` at the node of the next issue, or null.
void Risc::advance( Node& node )
{
  std::uint64_t const outcome = outcomeOf( node );
  Variant* variant = node.last && node.last->outcome == outcome ? node.last : nullptr;
  if ( !variant )
    for ( Variant* v : node.variants )
      if ( v->outcome == outcome )
        variant = v;
  node.last = variant;
  if ( variant && !variant->stepOnly && !objectsWithin( variant->span ) )
  {
    replay( node, *variant );
    mAuto->current = variant->next;
    return;
  }

  // the stepped core takes over from the node's state
  restore( node.entry );
  bool const recording = variant == nullptr;
  if ( recording )
  {
    variant = &mAuto->variantStore.emplace_back();
    node.variants.push_back( variant );
    node.last = variant;
    variant->outcome = outcome;
    mAuto->recNode = &node;
    mAuto->recVariant = variant;
    mAuto->recStart = mCycle;
  }
  std::uint64_t const start = mCycle;
  bool stepOnly = false;
  do
  {
    // what is served in this cycle that the replay cannot follow
    if ( mDatServ )
    {
      std::uint32_t const a = mAccess.address & ~3u;
      bool const load = !isStore( mAccess.op, mGpu );
      bool const i2s = !mGpu && ( a == mIoBase + REG_SCLK || a == mIoBase + REG_SMODE );
      if ( ( addressClass( a ) == CLASS_IO && ( load || i2s ) ) || addressClass( a ) == CLASS_EXTERNAL )
        stepOnly = true;
    }
    mCycle += cycle();
  } while ( mCycle < mStopAt && !issuing() && !mBlocked );

  // waiting for the other processor in the middle of the node: what was recorded is not a variant
  if ( mBlocked )
  {
    if ( recording )
    {
      node.variants.pop_back();
      node.last = nullptr;
      mAuto->recVariant = nullptr;
      mAuto->recNode = nullptr;
    }
    mAuto->current = nullptr;
    return;
  }

  Node* next = nullptr;
  if ( mCycle < mStopAt )
  {
    PipeState s;
    capture( s );
    if ( !s.tooMany )
      next = &nodeFor( s );
  }
  if ( recording )
  {
    mAuto->recVariant = nullptr;
    mAuto->recNode = nullptr;
    std::uint64_t const span = mCycle - start;
    variant->span = static_cast<std::uint32_t>( span );
    variant->next = next;
    // cut by the limit rather than ended by the DSP: not a complete variant
    bool const cut = mCycle >= mStopAt && mGoFalls > mStopAt;
    // a span MMULT's sequence starts in (the decode stage taking it) is the stepped core's
    variant->stepOnly = stepOnly || span > MAX_SPAN || cut || ( !next && mCycle < mStopAt ) || !mSysIdle;
    variant->bare = variant->actions.size() == 1 && variant->actions[0] == EXECUTE;
    // the chain's variants: neither timer requests within the span
    if ( node.simple && !variant->stepOnly && outcome >> TIMER_SHIFT == TIMERS_FAR )
    {
      std::uint64_t const rise = ( outcome >> RISE_SHIFT ) & 63;
      node.byRise[rise] = variant;
      if ( rise == MAX_SPAN )
        node.idle = variant;
    }
  }
  mAuto->current = next;
}

bool Risc::run( std::uint64_t limit )
{
  setLimit( limit );
  runUntil( limit );
  finish();
  return mGoFalls <= limit;
}

void Risc::setLimit( std::uint64_t limit )
{
  // the run ends where DSPGO falls or at the limit; nothing planned for after that is made
  mLimit = std::min( mLimit, limit );
  mStopAt = std::min( mStopAt, limit );
}

bool Risc::runUntil( std::uint64_t pause )
{
  mPause = pause;
  while ( mCycle < mStopAt && mCycle < pause )
  {
    if ( mTraceBuffer.size() > 65536 )
      flushTrace( mCycle > 2 * MAX_LOOP + CALENDAR_SPAN ? mCycle - 2 * MAX_LOOP - CALENDAR_SPAN : 0 );
    // a debugger's stop: nothing runs on, unless the 68000 waits on the bus for this processor
    if ( mDebugger && mDebugger->stop && mDebugger->freezable )
      break;
    bool const probed = mProbe && mProbe->armed();
    // while the main bus works for the DSP the automaton replays only what does not depend on it;
    // nothing while an external load is to be written back or Jerry's timers run
    // a timer whose period is within a span requests more than once in it
    // MMULT's sequence too: its state is not in the nodes
    // nothing either while the bus is shared with the other processor and works for this one, or
    // the GPU reaches Jerry: a cycle may then wait for the other
    bool const coupled = ( mBusBusy && mBus.shared() ) || this->coupled();
    bool const xld = mOneld || mTwold || mXldWbackl || mTimerWrite || mTimers.shortestPeriod() <= MAX_SPAN ||
                     !mSysIdle || mMtxDover || mDatMtx || ( mAuto && mAuto->current && mAuto->current->mmult ) ||
                     mXprog || !isLocal( ( mFetchPc << 1 ) & 0xfffffcu ) || headIsMmult() || coupled ||
                     probed; // a debugger's breakpoints and watchpoints: the stepped core alone
    if ( mAuto && mAuto->current && ( xld || ( mBusBusy && !mAuto->current->busSafe ) ) )
    {
      restore( mAuto->current->entry ); // the stepped core takes over from the node's state
      mAuto->current = nullptr;
    }
    if ( mAuto && !xld )
    {
      if ( mAuto->flushWanted )
      {
        // the live state takes over from the node replay has reached, before the nodes go
        if ( mAuto->current )
          restore( mAuto->current->entry );
        mAuto->flushWanted = false;
        mAuto->nodes.clear();
        mAuto->nodeStore.clear();
        mAuto->variantStore.clear();
        mAuto->current = nullptr;
        mCodeMark.fill( 0 );
      }
      if ( Node* node = mAuto->current )
      {
        if ( node->kind == KIND_JUMP || node->kind == KIND_JR )
          skipLoop( *node );
        // simple nodes as a chain, their variants told apart by the I2S line alone; where it stops
        // on another node the loop judges that one, where the node has no variant it is recorded
        if ( node->simple )
        {
          Node const* next = node->chain( *this, node );
          while ( next )
            next = next->chain( *this, next );
          if ( mAuto->current != node )
            continue;
        }
        if ( mCycle < mStopAt )
          advance( *node );
        if ( mBlocked )
          break;
        continue;
      }
      if ( issuing() && ( !mBusBusy || busSafeNow() ) )
      {
        PipeState s;
        capture( s );
        if ( !s.tooMany )
        {
          Node& node = nodeFor( s );
          if ( !mBusBusy || node.busSafe )
          {
            mAuto->current = &node;
            continue;
          }
        }
      }
    }
    if ( probed && breakHere() )
      break;
    mCycle += cycle();
    if ( mBlocked )
      break;
  }
  mBlocked = false;
  if ( mCycle < mStopAt )
  {
    mBus.catchUp( mMaster, mCycle ); // every cycle before this one handed over
    return true;
  }
  mBus.attach( mMaster, UINT64_MAX ); // stopped: the bus no longer waits for it
  if ( mGoFalls <= mCycle )
  {
    // what another master writes now finds it stopped, the live state its own
    if ( mAuto && mAuto->current )
    {
      restore( mAuto->current->entry );
      mAuto->current = nullptr;
    }
    flagsAt( mGoFalls - 1 );
    mGo = false;
    mI2sAt = mCycle;
  }
  return false;
}

void Risc::finish()
{
  // the I2S interface up to the end of a run the processor stopped in, or did not start in
  if ( !mGo && mLimit != UINT64_MAX )
    i2sCatchUp( mLimit );
  lateLoads(); // the loads that ended after the stop
  // the flags due by the last cycle, for the trace
  flagsAt( mStopAt ? mStopAt - 1 : 0 );
  flushTrace( UINT64_MAX );
  mGo = false;
}

} // namespace jag
