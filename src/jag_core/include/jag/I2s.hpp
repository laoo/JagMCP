#pragma once

#include <cstdint>

namespace jag
{

// Jerry's I2S interface: Jaguar_MiSTer rtl/Jerry/j_i2s.v, read at Jaguar_MiSTer 43a761d. It runs on
// the DSP's clock; only what decides the interrupt is kept (the serial clock divider, the word
// select, the bit counter of the receiver), not the data shifted in and out.
class I2s
{
public:
  // A write of the DSP served in the current cycle (i2s1w to i2s4w): LTXD, RTXD, SCLK, SMODE.
  void writeLtxd( std::uint16_t value );
  void writeRtxd( std::uint16_t value );
  void writeSclk( std::uint16_t value );
  void writeSmode( std::uint16_t value );

  // Whether the clock edge that ends the current cycle changes what the interface shows: it is
  // enabled, or a write of this cycle or the one before is being taken. Otherwise only the serial
  // clock generator runs, and it is caught up before the next write (`skip`).
  bool stepping() const
  {
    return mEnable || mSclkWrite || mSmodeWrite || mSettle;
  }

  // The clock edge that ends the current cycle.
  void step();

  void skip()
  {
    ++mSkipped;
    mPrev = mI2int;
  }

  // How many steps leave `interrupt()` as it is: up to the one before the next rising serial
  // clock, where the receiver moves; none while a write is being taken or while the interrupt is
  // still to take what the receiver's last move made of it.
  std::uint64_t quiet() const
  {
    if ( mSclkWrite || mSmodeWrite || mSettle || wsp() != mI2int )
      return 0;
    if ( !mEnable )
      return UINT64_MAX;
    return mSckout ? mT + 1 + mP : mT;
  }

  // Steps until `interrupt()` next rises (the request the DSP latches), UINT64_MAX while it cannot:
  // the receiver only moves at the rising serial clocks the interface itself generates. Found by
  // stepping a copy, once per rise.
  std::uint64_t interruptChangeIn( std::uint64_t now )
  {
    if ( mSclkWrite || mSmodeWrite || mSettle )
      return 0;
    if ( !mEnable )
      return UINT64_MAX;
    if ( mChangeKnown && now < mChangeAt )
      return mChangeAt - now;
    return findInterruptRise( now );
  }

  // `n` steps at once, no write among them: by the serial clock's toggles, not by the cycle.
  void advance( std::uint64_t n );

  // i2int: the interrupt line, as it is in the current cycle.
  bool interrupt() const
  {
    return mI2int;
  }

  // Nothing of the interface can move: disabled, no write being taken.
  bool idle() const
  {
    return !mEnable && !mSettle && !mSclkWrite && !mSmodeWrite;
  }

  // The line rose with the last step: the request the DSP latches (j_dsp.v, i2int & ~i2intd).
  bool rose() const
  {
    return mI2int && !mPrev;
  }

  std::uint16_t left() const
  {
    return mLeft;
  }

  std::uint16_t right() const
  {
    return mRight;
  }

  // Each frame the interface generates (its word select rises with the serial clock enabled): the
  // transmit latches as they stand, as the console's left and right outputs. Null to keep none.
  using SampleSink = void ( * )( void* context, std::uint16_t left, std::uint16_t right );

  void setSampleSink( SampleSink sink, void* context )
  {
    mSampleSink = sink;
    mSampleContext = context;
  }

  // The DSP cycles a frame takes: 32 serial clocks of two toggles, a toggle every SCLK + 1 cycles.
  std::uint64_t frameCycles() const
  {
    return 64 * ( static_cast<std::uint64_t>( mP ) + 1 );
  }

private:
  void generatorStep( bool sclkWrite );
  void toggle();
  void settle( std::uint64_t steps );
  std::uint64_t findInterruptRise( std::uint64_t now );

  // The word select as it was before a step of the generator: a rise starts a frame. In 16 bit mode
  // dpl (LTXD, $F1A148) is shifted out while the word select is low, and the board's DAC
  // (TDA1545A; Jaguar_MiSTer rtl/jaguar.v, rtl/tda1545a.v) puts that word on its right output: the
  // console's right channel is dpl, its left dpr (RTXD, $F1A14C).
  void framed( bool wsBefore ) const
  {
    if ( mSampleSink && mEnable && mWsout && !wsBefore )
      mSampleSink( mSampleContext, mRight, mLeft );
  }

  SampleSink mSampleSink = nullptr;
  void* mSampleContext = nullptr;

  // wsp: the interrupt condition of the receiver's state, taken by i2int at the next clock
  bool wsp() const
  {
    bool const ws0 = mWs & 1;
    bool const ws1 = ( mWs >> 1 ) & 1;
    return ( ws0 && !ws1 && mRising ) || ( !ws0 && ws1 && mFalling ) || ( mMsb && mEveryWord );
  }

  std::uint64_t mSkipped = 0;  // cycles the generator has not been stepped for
  std::uint64_t mChangeAt = 0; // the cycle `interrupt()` next rises, when known
  bool mChangeKnown = false;
  bool mSettle = false; // the cycle after a write

  // the writes of the current cycle
  bool mSclkWrite = false;
  std::uint16_t mSclkValue = 0;
  bool mSmodeWrite = false;
  std::uint16_t mSmodeValue = 0;

  std::uint16_t mLeft = 0;  // dpl
  std::uint16_t mRight = 0; // dpr

  // the serial clock and word select generator
  std::uint32_t mP = 0; // p: the divider
  std::uint32_t mT = 0; // t: counts down to 0, the serial clock toggles there
  bool mSckout = false; // sckout
  std::uint32_t mB = 0; // b: serial clock cycles, 5 bits
  bool mWsout = false;  // wsout
  bool mEnable = false; // i2sen: the clock and word select are generated here
  bool mMode32 = false; // SMODE bits
  bool mWsen = false;
  bool mRising = false;
  bool mFalling = false;
  bool mEveryWord = false;

  // the receiver, clocked by the rising serial clock
  std::uint32_t mWs = 0;  // ws[1:0]: the word select of the last two serial clocks
  std::uint32_t mBc = 0;  // bc: the bit counter
  bool mOv = false;       // ov
  bool mLeftWord = false; // left
  bool mMsb = false;      // msbi

  bool mI2int = false; // i2int
  bool mPrev = false;  // i2int before the last step (j_dsp.v i2intd)
};

} // namespace jag
