#pragma once

// A record of the video output and the I2S output into a video file, made by ffmpeg: the frames
// piped to it as they come, the sound kept in a file beside the record, the two put together when
// the record ends. Both are placed by the DSP cycle they were made in: a frame takes the video's
// frames from the one its cycle falls in, the one before it repeated where none came (the video
// was off), and the sound is sampled at 48 kHz, silent where the I2S made none. The video is made
// in segments of a frame rate each: where the program's frames come at another rate for a few
// frames running, a new segment begins.

#include "jag/control/Session.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace jag::control
{

class Process;

// Why ffmpeg cannot be run (JAGMCP_FFMPEG, or ffmpeg on the PATH), its output written to `log` and
// removed; none when it can.
std::optional<std::string> encoderMissing( std::string const& log );

class Recorder
{
public:
  // A record into `path` from `cycle` on: H.264 in 4:2:0, or lossless H.264 in 4:4:4 of 10 bits; each
  // pixel made a block of 2 by 2 when `crisp`: H.264 keeps a colour for each such block, and keeps
  // its edges sharp where it blurs single pixels.
  static std::expected<std::unique_ptr<Recorder>, std::string>
  start( std::string const& path, bool lossless, bool crisp, std::uint64_t cycle );

  // Ends the encoder, the record left unfinished.
  ~Recorder();

  Recorder( Recorder const& ) = delete;
  Recorder& operator=( Recorder const& ) = delete;
  Recorder( Recorder&& ) = delete;
  Recorder& operator=( Recorder&& ) = delete;

  // A frame completed in `cycle`.
  void frame( Session::Lines const& lines, std::uint64_t cycle );

  // An I2S frame made near `cycle`; `period` the cycles between two.
  void sample( std::int16_t left, std::int16_t right, std::uint64_t cycle, std::uint64_t period );

  // Ends the record at `cycle` and writes the file.
  std::expected<Session::Recorded, std::string> stop( std::uint64_t cycle );

private:
  using Image = std::shared_ptr<std::vector<std::uint8_t> const>;

  Recorder() = default;
  Image image( Session::Lines const& lines ) const;
  std::optional<std::string> startEncoder();
  std::optional<std::string> finishEncoder();
  void newSegment( std::uint64_t period, std::uint64_t cycle );
  void place( Image const& pixels, std::uint64_t cycle );
  void push( Image const& pixels );
  void write();
  void soundTo( double at, double left, double right );

  std::string mPath;
  std::string mSoundPath;
  std::string mLogPath;
  bool mLossless = false;
  std::uint32_t mScale = 1;
  std::uint64_t mStart = 0;

  // the video: its size, the frames given it
  std::uint32_t mWidth = 0;
  std::uint32_t mHeight = 0;
  std::optional<std::pair<Image, std::uint64_t>> mFirst;
  Image mLast;
  std::uint64_t mLastAt = 0; // the cycle the last frame was completed in
  std::uint64_t mFrames = 0;
  std::uint64_t mRepeated = 0;
  std::uint64_t mDropped = 0;
  std::optional<std::string> mFailed;

  // the segment made now: the cycles of its frames, the cycle its first begins at, its frames; the
  // other rate the frames come at, and for how many running
  struct Segment
  {
    std::string path;
    std::uint64_t period = 0;
    std::uint64_t frames = 0;
  };

  std::vector<Segment> mSegments;
  std::uint64_t mPeriod = 0;
  std::uint64_t mSegmentStart = 0;
  std::uint64_t mSegmentFrames = 0;
  std::uint64_t mOtherPeriod = 0;
  int mOtherFrames = 0;

  // the encoder, fed by a thread of its own
  std::unique_ptr<Process> mEncoder;
  std::thread mWriter;
  std::mutex mMutex;
  std::condition_variable mChanged;
  std::deque<Image> mQueue;
  bool mClosing = false;
  bool mBroken = false;

  // the sound: the output's next sample, the last point of the line drawn to it, the last I2S frame
  std::ofstream mSound;
  double mOutAt = 0;
  double mPointAt = 0;
  double mPointLeft = 0;
  double mPointRight = 0;
  std::optional<double> mHeardAt;
  std::uint64_t mHeardPeriod = 0;
};

} // namespace jag::control
