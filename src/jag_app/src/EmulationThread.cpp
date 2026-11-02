#include "EmulationThread.hpp"

#include "jag/I2s.hpp"
#include "jag/Tom.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>
#include <utility>

namespace jag::app
{

namespace
{

// A part: a quarter of a millisecond of the console's time, so that the sound is handed on often
// and a request waits little.
constexpr auto PART_CYCLES = static_cast<std::uint64_t>( CYCLES_PER_SECOND / 4000.0 );

// The most the machine runs to catch up with the host's clock: after a stall the rest is dropped
// rather than run at full speed.
constexpr double MAX_CYCLES_OWED = CYCLES_PER_SECOND / 10.0;

// How long the thread sleeps at most while it has nothing to run, so that it notices a program
// loaded or a pause lifted.
constexpr std::chrono::milliseconds IDLE_WAIT{ 20 };

// How long the speed is measured over.
constexpr std::chrono::milliseconds SPEED_WINDOW{ 500 };

} // namespace

EmulationThread::EmulationThread( AudioOutput* audio ) : mAudio{ audio }, mThread{ [this] { loop(); } }
{
  // the frames and the sound the session makes, on the emulation thread
  mSession.setObservers( [this]( control::Session::Lines const& lines ) { frame( lines ); },
                         [this]( std::uint16_t left, std::uint16_t right )
                         {
                           mSamples.push_back( static_cast<std::int16_t>( left ) );
                           mSamples.push_back( static_cast<std::int16_t>( right ) );
                         } );
}

EmulationThread::~EmulationThread()
{
  {
    std::scoped_lock const lock{ mMutex };
    mStop = true;
  }
  mWake.notify_all();
  mThread.join();
}

void EmulationThread::post( std::function<void()> job )
{
  std::promise<void> done;
  std::future<void> doneFuture = done.get_future();
  {
    std::scoped_lock const lock{ mMutex };
    if ( mStop )
      return;
    mJobs.emplace_back(
        [&job, &done]
        {
          job();
          done.set_value();
        } );
  }
  mWake.notify_all();
  doneFuture.wait();
}

std::optional<std::string> EmulationThread::load( RunOptions const& options )
{
  std::optional<std::string> failed = std::string{ "The emulator is closing" };
  post( [&] { failed = mSession.load( options ); } );
  return failed;
}

void EmulationThread::restart()
{
  post( [this] { static_cast<void>( mSession.restart() ); } );
}

control::Json EmulationThread::handle( control::Json const& request )
{
  control::Json answer{ { "id", 0 },
                        { "ok", false },
                        { "error", { { "code", "shutting_down" }, { "message", "The emulator is closing" } } } };
  post( [&] { answer = mDispatcher.handle( request ); } );
  return answer;
}

std::string EmulationThread::program()
{
  std::scoped_lock const lock{ mPictureMutex };
  return mProgram;
}

std::string EmulationThread::bios()
{
  std::scoped_lock const lock{ mPictureMutex };
  return mBios;
}

void EmulationThread::setPaused( bool paused )
{
  post( [this, paused] { mSession.setPaused( paused ); } );
  if ( paused && mAudio != nullptr )
    mAudio->clear(); // so that sound resumes at once, not after what was queued
  mWake.notify_all();
}

void EmulationThread::setPad( std::size_t port, std::uint32_t buttons )
{
  post( [this, port, buttons] { mSession.setPad( port, buttons ); } );
}

bool EmulationThread::takePicture( std::int64_t& picturesSeen, Picture& out )
{
  std::scoped_lock const lock{ mPictureMutex };
  if ( mPictures == picturesSeen || mPicture.rgba.empty() )
    return false;
  out = mPicture;
  picturesSeen = mPictures;
  return true;
}

void EmulationThread::frame( control::Session::Lines const& lines )
{
  Picture& picture = mBuilding;
  std::size_t width = 0;
  for ( auto const& line : lines )
    width = std::max( width, line.size() / 3 );
  picture.width = static_cast<std::uint32_t>( width );
  picture.height = static_cast<std::uint32_t>( lines.size() );
  picture.rgba.assign( width * lines.size() * 4, 0 );
  for ( std::size_t y = 0; y < lines.size(); ++y )
  {
    std::uint8_t* out = picture.rgba.data() + ( y * width * 4 );
    for ( std::size_t x = 0; x < width; ++x, out += 4 )
    {
      if ( ( 3 * x ) + 2 < lines[y].size() )
        std::copy_n( lines[y].begin() + static_cast<std::ptrdiff_t>( 3 * x ), 3, out );
      out[3] = 0xff;
    }
  }
  if ( width == 0 )
    return;
  std::scoped_lock const lock{ mPictureMutex };
  std::swap( mPicture, picture );
  ++mPictures;
}

void EmulationThread::runPart()
{
  static_cast<void>( mSession.runCycles( PART_CYCLES ) );
  if ( mAudio != nullptr && !mSamples.empty() )
    mAudio->push( mSamples, CYCLES_PER_SECOND / static_cast<double>( mSession.machine()->i2s().frameCycles() ) );
  mSamples.clear();
  publishState();
}

void EmulationThread::publishState()
{
  Machine* machine = mSession.machine();
  mLoaded = machine != nullptr;
  mRunning = mSession.running();
  mPaused = mSession.paused();
  mRecording = mSession.recording();
  mCycles = machine ? machine->now() : 0;
  std::string stop;
  if ( auto const& at = mSession.lastStop() )
  {
    char text[96];
    std::snprintf( text,
                   sizeof text,
                   "%s of the %s at $%06x",
                   at->kind == DebugStop::Kind::Breakpoint   ? "Breakpoint"
                   : at->kind == DebugStop::Kind::Watchpoint ? "Watchpoint"
                                                             : "Step",
                   at->cpu == "m68k"  ? "68000"
                   : at->cpu == "gpu" ? "GPU"
                                      : "DSP",
                   at->pc );
    stop = text;
  }
  // a 68000 bus cycle waiting a second or more: the console is likely locked
  else if ( auto const wait = machine ? machine->busWait() : std::nullopt;
            wait && machine->now() - wait->since >= static_cast<std::uint64_t>( CYCLES_PER_SECOND ) )
  {
    char text[160];
    std::snprintf( text,
                   sizeof text,
                   "The 68000 at $%06x waits for the bus since %.3f s, held by the %s",
                   wait->pc,
                   static_cast<double>( wait->since ) / CYCLES_PER_SECOND,
                   wait->holder.c_str() );
    stop = text;
  }
  std::vector<std::string> undriven;
  if ( machine )
    for ( UndrivenRead const& r : machine->undrivenReads() )
    {
      char const* name = Tom::registerName( r.address );
      char text[128];
      std::snprintf( text,
                     sizeof text,
                     "%s at $%06x reads $%06x%s%s%s: $%04x, %llu times",
                     r.cpu == "m68k"  ? "68000"
                     : r.cpu == "gpu" ? "GPU"
                                      : "DSP",
                     r.pc,
                     r.address,
                     name ? " (" : "",
                     name ? name : "",
                     name ? ")" : "",
                     r.value,
                     static_cast<unsigned long long>( r.count ) );
      undriven.emplace_back( text );
    }
  std::scoped_lock const lock{ mPictureMutex };
  mStopText = std::move( stop );
  mUndrivenText = std::move( undriven );
}

std::string EmulationThread::stop()
{
  std::scoped_lock const lock{ mPictureMutex };
  return mStopText;
}

std::vector<std::string> EmulationThread::undrivenReads()
{
  std::scoped_lock const lock{ mPictureMutex };
  return mUndrivenText;
}

void EmulationThread::serveJobs()
{
  bool served = false;
  for ( ;; )
  {
    std::function<void()> job;
    {
      std::scoped_lock const lock{ mMutex };
      if ( mJobs.empty() )
        break;
      job = std::move( mJobs.front() );
      mJobs.pop_front();
    }
    job();
    served = true;
  }
  if ( !served )
    return;
  // the sound of a run on request is not the console's time: it is not played
  mSamples.clear();
  publishState();
  RunOptions const* options = mSession.options();
  std::string program = options != nullptr ? mSession.program() : std::string{};
  std::string bios = options != nullptr ? options->bios : std::string{};
  std::scoped_lock const lock{ mPictureMutex };
  mProgram = std::move( program );
  mBios = std::move( bios );
}

void EmulationThread::loop()
{
  using Clock = std::chrono::steady_clock;
  auto last = Clock::now();
  double owed = 0.0; // the console's cycles the machine is behind the host's clock
  auto windowStart = last;
  std::uint64_t windowCycles = 0;
  for ( ;; )
  {
    // the jobs first, those posted before the end among them, so that no caller is left waiting
    serveJobs();
    {
      std::scoped_lock const lock{ mMutex };
      if ( mStop && mJobs.empty() )
        break;
    }

    // The host's clock paces the machine, so that it runs at the console's speed whatever the
    // display's refresh rate; the audio device's clock drifts from it, which AudioOutput absorbs.
    auto const now = Clock::now();
    double const elapsed = std::chrono::duration<double>( now - last ).count();
    last = now;
    if ( now - windowStart >= SPEED_WINDOW )
    {
      mSpeed = static_cast<double>( windowCycles ) / CYCLES_PER_SECOND /
               std::chrono::duration<double>( now - windowStart ).count();
      windowStart = now;
      windowCycles = 0;
    }

    auto wakeAt = now + IDLE_WAIT;
    if ( !mSession.machine() || mSession.paused() || !mRunning )
    {
      owed = 0.0;
    }
    else
    {
      owed = std::min( owed + ( elapsed * CYCLES_PER_SECOND ), MAX_CYCLES_OWED );
      if ( owed >= static_cast<double>( PART_CYCLES ) )
      {
        std::uint64_t const from = mSession.machine()->now();
        runPart();
        std::uint64_t const ran = mSession.machine()->now() - from;
        owed -= static_cast<double>( ran );
        windowCycles += ran;
        continue;
      }
      wakeAt = now + std::chrono::duration_cast<Clock::duration>( std::chrono::duration<double>(
                         ( static_cast<double>( PART_CYCLES ) - owed ) / CYCLES_PER_SECOND ) );
    }

    std::unique_lock lock{ mMutex };
    mWake.wait_until( lock, wakeAt, [this] { return mStop || !mJobs.empty(); } );
  }
}

} // namespace jag::app
