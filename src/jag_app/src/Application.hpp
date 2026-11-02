#pragma once

#include "AudioOutput.hpp"
#include "EmulationThread.hpp"
#include "GpuTexture.hpp"

#include "jag/server/McpHttpServer.hpp"
#include "jag/server/McpServer.hpp"
#include "jag/server/TcpLineServer.hpp"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace jag::app
{

// The desktop front end: an SDL3 window rendered through SDL_GPU, with Dear ImGui docked over it,
// as PGMEmu's (docs/decisions/0011-the-window-is-pgmemus.md). It shows the frames of the video
// output as the machine makes them and plays its I2S output.
class Application
{
public:
  // Opens the window and the GPU device and sets up ImGui. Answers why when any of them cannot be
  // had.
  static std::expected<std::unique_ptr<Application>, std::string> create();

  ~Application();

  Application( Application const& ) = delete;
  Application& operator=( Application const& ) = delete;
  Application( Application&& ) = delete;
  Application& operator=( Application&& ) = delete;

  // Starts a program: a .run of tests/programs, or a program and the runner's arguments, as
  // jagmcp-cli takes them, or a cartridge (.j64, .jag) booted by the BIOS, with the runner's
  // arguments or none (a --bios among them taken for this start alone). A failure is shown in the
  // status window.
  void load( std::vector<std::string> const& args );

  // Serves the control protocol to other programs while the window is open: JSON-lines on TCP
  // `linePort`, MCP over HTTP on `mcpPort`, each unless 0, on 127.0.0.1 (docs/spec/control-protocol.md).
  // Throws std::runtime_error when a port cannot be listened on.
  void serve( std::uint16_t linePort, std::uint16_t mcpPort );

  // Records the picture and the sound into `path` (.mp4, .mkv, .mov) from now on, through ffmpeg,
  // as video.record_start does. The record is written when it is stopped or the window closes. A
  // failure is shown in the status window and logged.
  void startRecord( std::string const& path );

  // Runs until the window is closed or Quit is chosen. A record still running is then written.
  void run();

private:
  Application( SDL_Window* window, SDL_GPUDevice* device );

  // Takes the last frame the emulation thread completed, the screen texture made anew when its
  // size changed.
  void updateEmulation();

  // Lays out one frame of the user interface.
  void drawInterface();
  void drawMenuBar();
  void drawScreenWindow();
  void drawStatusWindow();

  // Uploads what changed, then renders the interface into the swapchain.
  void renderFrame();

  // The BIOS a cartridge boots with: the one remembered, else one found beside the cartridge or a
  // folder up (as the files of a ROM collection are kept), which is then remembered. Empty with a
  // reason in mLastError when there is none.
  std::string biosFor( std::filesystem::path const& cartridge );
  // Takes `path` as the BIOS for the cartridges started from now on, remembered in settings.json
  // when `keep`. False, with the reason in mLastError, if it is not a BIOS image.
  bool setBios( std::string const& path, bool keep );
  void keepSetting( std::string const& key, control::Json value );

  // Ends the record, which a request may have started, and writes its file.
  void stopRecord();

  // File > Open, File > Choose BIOS and File > Record video: native dialogs that answer on a thread
  // of their own; the file chosen is taken on a later frame (takeChosen).
  void openFile();
  void chooseBios();
  void chooseRecord();
  void takeChosen();

  // The keyboard's keys as the pad in port 1, handed on as they change.
  void updatePad();

  SDL_Window* mWindow;
  SDL_GPUDevice* mDevice;
  std::unique_ptr<GpuTexture> mScreen; // null until the first frame
  Picture mPicture;
  bool mPictureChanged{};
  std::int64_t mPicturesShown{ -1 };
  // Null when no audio device could be opened: the emulation then runs silent, paced by the clock
  // alone.
  std::unique_ptr<AudioOutput> mAudio;
  std::unique_ptr<EmulationThread> mEmulation;
  std::unique_ptr<server::TcpLineServer> mLineServer;
  std::unique_ptr<server::McpServer> mMcp;
  std::unique_ptr<server::McpHttpServer> mMcpHttp;
  std::string mImguiIniPath;
  std::string mProgram; // the file loaded, as given; by the window or by a request
  std::string mLastError;
  std::string mRecordPath; // the record the window started; empty for none, or for a request's
  std::string mRecorded;   // what the last record the window stopped holds, for a person
  bool mQuit{};
  bool mPaused{};
  bool mShowStatus{ true };
  std::uint32_t mKeyPad{};   // the buttons the keyboard last held
  std::string mBios;         // the BIOS cartridges boot with; empty for none yet
  std::string mSettingsPath; // settings.json in the preferences' directory; empty if there is none
  control::Json mSettings = control::Json::object();
  std::mutex mChosenMutex;
  std::optional<std::string> mChosen;       // under mChosenMutex
  std::optional<std::string> mChosenBios;   // under mChosenMutex
  std::optional<std::string> mChosenRecord; // under mChosenMutex
};

} // namespace jag::app
