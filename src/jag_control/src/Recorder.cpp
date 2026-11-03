#include "Recorder.hpp"

#include "Process.hpp"

#include "jag/Machine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <span>
#include <utility>

#ifndef _WIN32
#include <csignal>
#include <pthread.h>
#endif

namespace jag::control
{

namespace
{

constexpr std::uint32_t SOUND_RATE = 48'000;
constexpr double SOUND_STEP = CYCLES_PER_SECOND / SOUND_RATE;
// I2S frames further than this from where the last ones put them follow a silence
constexpr double SOUND_GAP = CYCLES_PER_SECOND / 50;
// frames waiting for the encoder before the emulation waits for it
constexpr std::size_t QUEUE_FRAMES = 32;
// frames running at another rate that begin a segment at it
constexpr int RATE_FRAMES = 3;

// The encoder: the program JAGMCP_FFMPEG names, or ffmpeg on the PATH. Never one a request names.
std::string ffmpeg()
{
  char const* named = std::getenv( "JAGMCP_FFMPEG" ); // NOLINT(concurrency-mt-unsafe): read, never set
  return named != nullptr && *named != '\0' ? named : "ffmpeg";
}

std::string cyclesPerSecond()
{
  return std::to_string( static_cast<std::uint64_t>( CYCLES_PER_SECOND ) );
}

std::vector<std::string> ffmpegArgs( std::initializer_list<std::string> args )
{
  std::vector<std::string> all{ ffmpeg(), "-hide_banner", "-nostats", "-loglevel", "error", "-y" };
  all.insert( all.end(), args );
  return all;
}

// The end of the encoder's log, for an error's message.
std::string logTail( std::string const& path )
{
  std::ifstream f( path, std::ios::binary );
  std::string const text{ std::istreambuf_iterator<char>( f ), std::istreambuf_iterator<char>() };
  std::string tail = text.size() > 400 ? text.substr( text.size() - 400 ) : text;
  while ( !tail.empty() && ( tail.back() == '\n' || tail.back() == '\r' ) )
    tail.pop_back();
  return tail.empty() ? "" : ": " + tail;
}

void remove( std::string const& path )
{
  std::error_code ignored;
  std::filesystem::remove( path, ignored );
}

} // namespace

std::optional<std::string> encoderMissing( std::string const& log )
{
  auto process = Process::start( { ffmpeg(), "-hide_banner", "-version" }, false, log );
  std::optional<std::string> why;
  if ( !process )
    why = process.error() + " (set JAGMCP_FFMPEG to the ffmpeg program)";
  else if ( int const status = ( *process )->wait(); status != 0 )
    why = ffmpeg() + " does not run (exit status " + std::to_string( status ) + ")" + logTail( log );
  remove( log );
  return why;
}

std::expected<std::unique_ptr<Recorder>, std::string>
Recorder::start( std::string const& path, bool lossless, bool crisp, std::uint64_t cycle )
{
  std::unique_ptr<Recorder> recorder{ new Recorder };
  recorder->mPath = path;
  recorder->mSoundPath = path + ".sound.pcm";
  recorder->mLogPath = path + ".log";
  recorder->mLossless = lossless;
  recorder->mScale = crisp ? 2 : 1;
  recorder->mStart = cycle;
  recorder->mOutAt = static_cast<double>( cycle );
  recorder->mPointAt = static_cast<double>( cycle );
  if ( !std::ofstream( path, std::ios::binary ) )
    return std::unexpected( "Cannot write " + path );
  recorder->mSound.open( recorder->mSoundPath, std::ios::binary | std::ios::trunc );
  if ( !recorder->mSound )
    return std::unexpected( "Cannot write " + recorder->mSoundPath );
  remove( recorder->mLogPath );
  return recorder;
}

Recorder::~Recorder()
{
  if ( mWriter.joinable() )
  {
    {
      std::scoped_lock const lock( mMutex );
      mClosing = true;
    }
    mChanged.notify_all();
    mWriter.join();
  }
}

Recorder::Image Recorder::image( Session::Lines const& lines ) const
{
  auto pixels = std::make_shared<std::vector<std::uint8_t>>( std::size_t{ 3 } * mWidth * mHeight );
  std::size_t const stride = std::size_t{ 3 } * mWidth;
  for ( std::size_t y = 0; y < std::min<std::size_t>( lines.size(), mHeight ); ++y )
    std::copy_n( lines[y].begin(),
                 std::min( lines[y].size(), stride ),
                 pixels->begin() + static_cast<std::ptrdiff_t>( y * stride ) );
  return pixels;
}

void Recorder::frame( Session::Lines const& lines, std::uint64_t cycle )
{
  if ( mFailed )
    return;
  if ( mWidth == 0 )
  {
    // the first frame's size, made even for 4:2:0; later frames are cut or padded to it
    std::size_t width = 0;
    for ( auto const& line : lines )
      width = std::max( width, line.size() / 3 );
    if ( width == 0 )
      return;
    mWidth = static_cast<std::uint32_t>( ( width + 1 ) & ~std::size_t{ 1 } );
    mHeight = static_cast<std::uint32_t>( ( lines.size() + 1 ) & ~std::size_t{ 1 } );
  }
  Image const pixels = image( lines );
  if ( mPeriod == 0 )
  {
    if ( !mFirst )
    {
      mFirst.emplace( pixels, cycle );
      mLastAt = cycle;
      return;
    }
    if ( cycle <= mFirst->second )
      return;
    newSegment( cycle - mFirst->second, mStart );
    if ( mFailed )
      return;
    auto const [first, at] = *std::exchange( mFirst, std::nullopt );
    place( first, at );
  }
  else if ( std::uint64_t const period = cycle - mLastAt; period * 64 < mPeriod * 63 || period * 64 > mPeriod * 65 )
  {
    // another rate: one frame off it (the video was off a while) leaves the segment be
    mOtherFrames = period == mOtherPeriod ? mOtherFrames + 1 : 1;
    mOtherPeriod = period;
    if ( mOtherFrames >= RATE_FRAMES )
    {
      // the segment ends at the first of its frames that begins at or after this one's cycle
      std::uint64_t const ends = std::max( mSegmentFrames, ( cycle - mSegmentStart + mPeriod - 1 ) / mPeriod );
      for ( ; mSegmentFrames < ends; ++mRepeated )
        push( mLast );
      mFailed = finishEncoder();
      if ( mFailed )
        return;
      newSegment( period, mSegmentStart + ( ends * mPeriod ) );
      if ( mFailed )
        return;
    }
  }
  else
    mOtherFrames = 0;
  mLastAt = cycle;
  place( pixels, cycle );
}

void Recorder::newSegment( std::uint64_t period, std::uint64_t cycle )
{
  mPeriod = period;
  mSegmentStart = cycle;
  mSegmentFrames = 0;
  mOtherFrames = 0;
  mSegments.push_back( Segment{
      .path = mPath + ".video" + std::to_string( mSegments.size() ) + ".nut", .period = period, .frames = 0 } );
  mFailed = startEncoder();
}

std::optional<std::string> Recorder::startEncoder()
{
  std::vector<std::string> args = ffmpegArgs( { "-f",
                                                "rawvideo",
                                                "-pix_fmt",
                                                "rgb24",
                                                "-video_size",
                                                std::to_string( mWidth ) + "x" + std::to_string( mHeight ),
                                                "-framerate",
                                                cyclesPerSecond() + "/" + std::to_string( mPeriod ),
                                                "-i",
                                                "-" } );
  // no B-frames: NUT keeps no decoding times, and with frames out of order the MP4 made from it loses one
  if ( mLossless )
    args.insert( args.end(), { "-c:v", "libx264", "-preset", "veryfast", "-bf", "0", "-qp", "0" } );
  else
    args.insert( args.end(), { "-c:v", "libx264", "-preset", "veryfast", "-bf", "0", "-crf", "18" } );
  // YCbCr by BT.709, and the file says so: a player that is not told guesses, QuickTime BT.709 for a
  // large picture. Lossless in 4:4:4 of 10 bits: every RGB colour comes back exactly. RGB planes
  // (libx264rgb) QuickTime shows as YCbCr, in wrong colours.
  args.insert( args.end(), { "-colorspace", "bt709", "-color_range", "tv" } );
  std::string const convert =
      std::string{ "scale=out_color_matrix=bt709:out_range=limited:" } +
      "flags=accurate_rnd+full_chroma_int+bitexact,format=" + ( mLossless ? "yuv444p10le" : "yuv420p" );
  // shown 4:3 (0017): the pixels kept, made blocks of mScale by mScale, and the player told their shape
  std::string filter = convert + ",setdar=4/3";
  if ( mScale > 1 )
    filter = "scale=iw*" + std::to_string( mScale ) + ":ih*" + std::to_string( mScale ) + ":flags=neighbor," + filter;
  // timed in DSP cycles, the time base every segment shares, so that they join exactly
  args.insert( args.end(),
               { "-vf", filter, "-enc_time_base", "1:" + cyclesPerSecond(), "-f", "nut", mSegments.back().path } );
  auto encoder = Process::start( args, true, mLogPath );
  if ( !encoder )
    return encoder.error();
  mEncoder = std::move( *encoder );
  if ( !mLast )
    mLast = std::make_shared<std::vector<std::uint8_t> const>( std::size_t{ 3 } * mWidth * mHeight );
  mClosing = false;
  mBroken = false;
  mWriter = std::thread( &Recorder::write, this );
  return std::nullopt;
}

std::optional<std::string> Recorder::finishEncoder()
{
  if ( mWriter.joinable() )
  {
    {
      std::scoped_lock const lock( mMutex );
      mClosing = true;
    }
    mChanged.notify_all();
    mWriter.join();
  }
  int const status = mEncoder ? mEncoder->wait() : 0;
  mEncoder.reset();
  if ( mBroken || status != 0 )
    return "ffmpeg failed to encode the video" + logTail( mLogPath );
  return std::nullopt;
}

void Recorder::place( Image const& pixels, std::uint64_t cycle )
{
  std::uint64_t const index = cycle > mSegmentStart ? ( cycle - mSegmentStart ) / mPeriod : 0;
  if ( index < mSegmentFrames ) // its frame of the video has one already
  {
    ++mDropped;
    mLast = pixels;
    return;
  }
  for ( ; mSegmentFrames < index; ++mRepeated )
    push( mLast );
  push( pixels );
  mLast = pixels;
}

void Recorder::push( Image const& pixels )
{
  std::unique_lock lock( mMutex );
  mChanged.wait( lock, [this] { return mQueue.size() < QUEUE_FRAMES || mBroken; } );
  ++mFrames;
  ++mSegmentFrames;
  ++mSegments.back().frames;
  if ( mBroken )
    return;
  mQueue.push_back( pixels );
  mChanged.notify_all();
}

void Recorder::write()
{
#ifndef _WIN32
  // a write to an encoder that has ended fails here, rather than raising SIGPIPE in the process
  sigset_t pipeSignal;
  sigemptyset( &pipeSignal );
  sigaddset( &pipeSignal, SIGPIPE );
  pthread_sigmask( SIG_BLOCK, &pipeSignal, nullptr );
#endif
  for ( ;; )
  {
    Image pixels;
    {
      std::unique_lock lock( mMutex );
      mChanged.wait( lock, [this] { return mClosing || !mQueue.empty(); } );
      if ( mQueue.empty() )
        return;
      pixels = std::move( mQueue.front() );
      mQueue.pop_front();
    }
    mChanged.notify_all();
    if ( !mEncoder->write( std::span{ *pixels } ) )
    {
      std::scoped_lock const lock( mMutex );
      mBroken = true;
      mQueue.clear();
      mChanged.notify_all();
      return;
    }
  }
}

void Recorder::sample( std::int16_t left, std::int16_t right, std::uint64_t cycle, std::uint64_t period )
{
  if ( period == 0 )
    return;
  auto const heard = static_cast<double>( cycle );
  double at = mHeardAt ? *mHeardAt + static_cast<double>( period ) : heard;
  if ( !mHeardAt || heard > at + SOUND_GAP )
  {
    // after a silence: down to it a frame after the last, up from it a frame before this one
    if ( mHeardAt )
      soundTo( *mHeardAt + static_cast<double>( mHeardPeriod ), 0, 0 );
    at = std::max( heard, mPointAt );
    soundTo( std::max( mPointAt, at - static_cast<double>( period ) ), 0, 0 );
  }
  soundTo( at, left, right );
  mHeardAt = at;
  mHeardPeriod = period;
}

void Recorder::soundTo( double at, double left, double right )
{
  if ( at < mPointAt )
    return;
  auto const put = [this]( double v )
  {
    auto const s = static_cast<std::int16_t>( std::clamp( std::lround( v ), -32768L, 32767L ) );
    mSound.put( static_cast<char>( s & 0xff ) );
    mSound.put( static_cast<char>( ( s >> 8 ) & 0xff ) );
  };
  // the output's samples on the line from the last point to this one
  for ( ; mOutAt <= at; mOutAt += SOUND_STEP )
  {
    double const f = at > mPointAt ? ( mOutAt - mPointAt ) / ( at - mPointAt ) : 1.0;
    put( mPointLeft + ( f * ( left - mPointLeft ) ) );
    put( mPointRight + ( f * ( right - mPointRight ) ) );
  }
  mPointAt = at;
  mPointLeft = left;
  mPointRight = right;
}

std::expected<Session::Recorded, std::string> Recorder::stop( std::uint64_t cycle )
{
  auto const end = static_cast<double>( cycle );
  // the sound to the end: held where the I2S still runs, silent where it has stopped
  if ( mHeardAt && end <= *mHeardAt + SOUND_GAP )
  {
    soundTo( end, mPointLeft, mPointRight );
  }
  else
  {
    if ( mHeardAt )
      soundTo( *mHeardAt + static_cast<double>( mHeardPeriod ), 0, 0 );
    soundTo( end, 0, 0 );
  }
  mSound.close();
  if ( !mSound )
    mFailed = "Cannot write " + mSoundPath;

  // the video to the end; a single frame shown at 50 Hz
  if ( !mFailed && mPeriod == 0 )
  {
    if ( !mFirst )
    {
      mFailed = "No frame was completed during the record";
    }
    else
    {
      newSegment( static_cast<std::uint64_t>( CYCLES_PER_SECOND / 50 ), mStart );
      if ( !mFailed )
        place( mFirst->first, mFirst->second );
    }
  }
  if ( !mFailed )
    for ( std::uint64_t const index = cycle > mSegmentStart ? ( cycle - mSegmentStart ) / mPeriod : 0;
          mSegmentFrames < index;
          ++mRepeated )
      push( mLast );
  if ( auto const why = finishEncoder(); why && !mFailed )
    mFailed = why;

  // the segments one after the other, by ffmpeg's concat demuxer where there are more than one
  std::string const listPath = mPath + ".video.txt";
  std::vector<std::string> video;
  if ( mSegments.size() == 1 )
    video = { "-i", mSegments.front().path };
  else
  {
    std::ofstream list( listPath, std::ios::binary );
    for ( Segment const& segment : mSegments )
    {
      std::string quoted;
      for ( char const c : std::filesystem::absolute( segment.path ).string() )
        quoted += c == '\'' ? std::string{ "'\\''" } : std::string( 1, c );
      list << "file '" << quoted << "'\n";
    }
    if ( !list )
      mFailed = "Cannot write " + listPath;
    video = { "-f", "concat", "-safe", "0", "-i", listPath };
  }

  if ( !mFailed )
  {
    std::vector<std::string> args = ffmpegArgs( {} );
    args.insert( args.end(), video.begin(), video.end() );
    args.insert( args.end(),
                 { "-f",
                   "s16le",
                   "-ar",
                   std::to_string( SOUND_RATE ),
                   "-ac",
                   "2",
                   "-i",
                   mSoundPath,
                   "-map",
                   "0:v",
                   "-map",
                   "1:a",
                   "-c:v",
                   "copy",
                   "-c:a",
                   "aac",
                   "-b:a",
                   "192k" } );
    std::string const extension = std::filesystem::path( mPath ).extension().string();
    if ( extension == ".mp4" || extension == ".m4v" || extension == ".mov" )
      args.insert( args.end(), { "-movflags", "+faststart", "-video_track_timescale", cyclesPerSecond() } );
    args.push_back( mPath );
    auto muxer = Process::start( args, false, mLogPath );
    if ( !muxer )
      mFailed = muxer.error();
    else if ( ( *muxer )->wait() != 0 )
      mFailed = "ffmpeg failed to write " + mPath + logTail( mLogPath );
  }

  for ( Segment const& segment : mSegments )
    remove( segment.path );
  remove( listPath );
  remove( mSoundPath );
  if ( mFailed )
  {
    remove( mPath );
    return std::unexpected( *mFailed );
  }
  remove( mLogPath );

  Session::Recorded recorded;
  recorded.path = mPath;
  recorded.crisp = mScale > 1;
  recorded.width = mWidth * mScale;
  recorded.height = mHeight * mScale;
  // the rate most of the video has
  auto const most = std::ranges::max_element( mSegments, {}, &Segment::frames );
  recorded.frameRate = CYCLES_PER_SECOND / static_cast<double>( most->period );
  recorded.segments = mSegments.size();
  recorded.frames = mFrames;
  recorded.repeated = mRepeated;
  recorded.dropped = mDropped;
  recorded.sampleRate = SOUND_RATE;
  recorded.seconds = static_cast<double>( cycle - std::min( cycle, mStart ) ) / CYCLES_PER_SECOND;
  return recorded;
}

} // namespace jag::control
