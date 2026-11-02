#pragma once

#include "AudioOutput.hpp"

#include "jag/Runner.hpp"
#include "jag/control/Dispatcher.hpp"
#include "jag/control/Session.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace jag::app
{

// A frame of the video output: RGBA, width by height, its lines as the MiSTer core's output gives
// them, a shorter line padded with black.
struct Picture
{
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;
};

// The machine on a thread of its own (docs/decisions/0011-the-window-is-pgmemus.md). It runs the
// program in parts at the console's speed, paced by the host's clock, or as fast as it can where it
// cannot keep up; each frame the video output completes is handed to the interface, and the I2S
// frames of each part to the sound. Between two parts it answers the requests of the control
// protocol (docs/decisions/0018-one-control-api.md), so that the interface and every transport
// attached to it drive the one machine without touching it.
class EmulationThread
{
public:
  // Starts the thread, with no program loaded. `audio`, if not null, is fed the sound, and must
  // outlive the thread.
  explicit EmulationThread( AudioOutput* audio );
  ~EmulationThread();

  EmulationThread( EmulationThread const& ) = delete;
  EmulationThread& operator=( EmulationThread const& ) = delete;
  EmulationThread( EmulationThread&& ) = delete;
  EmulationThread& operator=( EmulationThread&& ) = delete;

  // Starts `options`' program in place of what runs, as the runner starts it but without a limit.
  // Returns why not when it cannot be loaded; what runs then runs on.
  std::optional<std::string> load( RunOptions const& options );

  // Starts the program loaded last again.
  void restart();

  // Answers a request of the control protocol, on the emulation thread between two parts; waits
  // for the answer. Safe from any thread.
  [[nodiscard]] control::Json handle( control::Json const& request );

  // The dispatcher's methods, which do not change once it is made.
  [[nodiscard]] std::vector<control::Method> const& methods() const
  {
    return mMethods;
  }

  // Stops running parts, or starts again; requests are answered either way. A debugger's stop
  // pauses it too.
  void setPaused( bool paused );

  // Holds `buttons` on the pad in port 0 or 1 (MainBus::setPad's bits), as input.set does.
  void setPad( std::size_t port, std::uint32_t buttons );

  [[nodiscard]] bool paused() const
  {
    return mPaused;
  }

  // Where a debugger's breakpoint or watchpoint stopped the program last, for a person; empty for
  // none.
  [[nodiscard]] std::string stop();

  // The program's reads no register answers (Machine::undrivenReads), a line each for a person.
  [[nodiscard]] std::vector<std::string> undrivenReads();

  // Copies the last complete frame into `out` if it is newer than `picturesSeen`, and updates that;
  // false if there is none newer.
  bool takePicture( std::int64_t& picturesSeen, Picture& out );

  // A program is loaded, and its processor has not stopped.
  [[nodiscard]] bool loaded() const
  {
    return mLoaded;
  }

  [[nodiscard]] bool running() const
  {
    return mRunning;
  }

  // A record (video.record_start) is running, started by the window or by a request.
  [[nodiscard]] bool recording() const
  {
    return mRecording;
  }

  // The cycle the program has reached.
  [[nodiscard]] std::uint64_t cycles() const
  {
    return mCycles;
  }

  // The console's seconds run in a second of the host's, lately.
  [[nodiscard]] double speed() const
  {
    return mSpeed;
  }

  // The file of the program loaded, as it was given; empty before one is.
  [[nodiscard]] std::string program();

  // The BIOS the program loaded boots with, as it was given; empty for none.
  [[nodiscard]] std::string bios();

private:
  void loop();
  // Runs `job` on the emulation thread between two parts, and waits for it.
  void post( std::function<void()> job );
  // Runs every job waiting, and takes on what they changed.
  void serveJobs();
  // Runs a part, and hands its sound on.
  void runPart();
  // What the interface reads of the session, as it is now.
  void publishState();

  void frame( control::Session::Lines const& lines );

  AudioOutput* mAudio;

  // The thread's own, but for the dispatcher's method table.
  control::Session mSession;
  control::Dispatcher mDispatcher{ mSession };
  std::vector<control::Method> const mMethods{ mDispatcher.methods() };
  std::vector<std::int16_t> mSamples; // the part's, left and right
  Picture mBuilding;

  // The jobs waiting, and the thread's end.
  std::mutex mMutex;
  std::condition_variable mWake;
  std::deque<std::function<void()>> mJobs;
  bool mStop{};

  std::atomic<bool> mPaused{};
  std::atomic<bool> mLoaded{};
  std::atomic<bool> mRunning{};
  std::atomic<bool> mRecording{};
  std::atomic<std::uint64_t> mCycles{};
  std::atomic<double> mSpeed{};

  std::mutex mPictureMutex;
  Picture mPicture;
  std::int64_t mPictures{ -1 };
  std::string mProgram;                   // under mPictureMutex
  std::string mBios;                      // under mPictureMutex
  std::string mStopText;                  // under mPictureMutex
  std::vector<std::string> mUndrivenText; // under mPictureMutex

  std::thread mThread;
};

} // namespace jag::app
