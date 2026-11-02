#include "Application.hpp"

#include "jag/Machine.hpp"
#include "jag/Runner.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace jag::app
{

namespace
{

// The window opens with room for the screen several times over and the status beside it, scaled
// for the display it opens on.
constexpr int INITIAL_WIDTH = 1280;
constexpr int INITIAL_HEIGHT = 800;

constexpr char const* SCREEN_WINDOW = "Screen";
constexpr char const* STATUS_WINDOW = "Status";

std::string sdlError( std::string const& what )
{
  return what + ": " + SDL_GetError();
}

// A BIOS image: the 68000's OS ROM, 128 KB.
constexpr std::uintmax_t BIOS_SIZE = 0x20000;

// The names a BIOS image goes by in a ROM collection, the one it is looked for by first: MAME's,
// the model M's and the model K's as dumps name them, No-Intro's. Matched ignoring case.
constexpr std::array<std::string_view, 6> BIOS_NAMES = {
  "jagboot.rom", "jagboot_model_m.rom", "[BIOS] Atari Jaguar (World).j64",
  "jaguar.rom",  "jagboot_model_k.rom", "K146005418.ROM"
};

std::string lowered( std::string_view text )
{
  std::string out{ text };
  std::ranges::transform( out, out.begin(), []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
  return out;
}

// A cartridge's image, booted by the BIOS, rather than a program for the runner
bool isCartridge( std::filesystem::path const& path )
{
  std::string const extension = lowered( path.extension().string() );
  return extension == ".j64" || extension == ".jag";
}

// A file of `folder` named as a BIOS image is, of its size; the first of BIOS_NAMES' order
std::optional<std::filesystem::path> biosIn( std::filesystem::path const& folder )
{
  std::error_code failed;
  if ( !std::filesystem::is_directory( folder, failed ) )
    return std::nullopt;
  std::optional<std::filesystem::path> best;
  std::size_t rank = BIOS_NAMES.size();
  for ( auto const& entry : std::filesystem::directory_iterator( folder, failed ) )
  {
    std::string const name = lowered( entry.path().filename().string() );
    for ( std::size_t i = 0; i < rank; ++i )
      if ( name == lowered( BIOS_NAMES[i] ) && entry.is_regular_file( failed ) &&
           entry.file_size( failed ) == BIOS_SIZE )
      {
        best = entry.path();
        rank = i;
        break;
      }
  }
  return best;
}

// The keyboard as the pad in port 1 (docs/architecture.md, The window): each key and the button it
// holds, as a bit of MainBus::setPad's (right, left, down, up, A, B, C, Option, Pause, 1 to 9, 0,
// star, hash)
struct KeyButton
{
  SDL_Scancode key;
  unsigned bit;
};

constexpr KeyButton KEY_BUTTONS[] = {
  { SDL_SCANCODE_RIGHT, 0 },   { SDL_SCANCODE_LEFT, 1 },
  { SDL_SCANCODE_DOWN, 2 },    { SDL_SCANCODE_UP, 3 },
  { SDL_SCANCODE_Z, 4 },       { SDL_SCANCODE_X, 5 },
  { SDL_SCANCODE_C, 6 },       { SDL_SCANCODE_TAB, 7 },
  { SDL_SCANCODE_RETURN, 8 },  { SDL_SCANCODE_KP_ENTER, 8 },
  { SDL_SCANCODE_1, 9 },       { SDL_SCANCODE_2, 10 },
  { SDL_SCANCODE_3, 11 },      { SDL_SCANCODE_4, 12 },
  { SDL_SCANCODE_5, 13 },      { SDL_SCANCODE_6, 14 },
  { SDL_SCANCODE_7, 15 },      { SDL_SCANCODE_8, 16 },
  { SDL_SCANCODE_9, 17 },      { SDL_SCANCODE_0, 18 },
  { SDL_SCANCODE_KP_1, 9 },    { SDL_SCANCODE_KP_2, 10 },
  { SDL_SCANCODE_KP_3, 11 },   { SDL_SCANCODE_KP_4, 12 },
  { SDL_SCANCODE_KP_5, 13 },   { SDL_SCANCODE_KP_6, 14 },
  { SDL_SCANCODE_KP_7, 15 },   { SDL_SCANCODE_KP_8, 16 },
  { SDL_SCANCODE_KP_9, 17 },   { SDL_SCANCODE_KP_0, 18 },
  { SDL_SCANCODE_MINUS, 19 },  { SDL_SCANCODE_KP_MULTIPLY, 19 },
  { SDL_SCANCODE_EQUALS, 20 }, { SDL_SCANCODE_KP_PLUS, 20 },
};

// The message of a control protocol's error answer
std::string errorMessage( control::Json const& answer )
{
  auto const error = answer.find( "error" );
  return error != answer.end() ? error->value( "message", std::string{ "unknown error" } ) : "unknown error";
}

} // namespace

std::expected<std::unique_ptr<Application>, std::string> Application::create()
{
  if ( !SDL_Init( SDL_INIT_VIDEO ) )
    return std::unexpected( sdlError( "SDL_Init" ) );

  float const scale = SDL_GetDisplayContentScale( SDL_GetPrimaryDisplay() );
  SDL_Window* const window = SDL_CreateWindow( "JagMCP",
                                               static_cast<int>( INITIAL_WIDTH * scale ),
                                               static_cast<int>( INITIAL_HEIGHT * scale ),
                                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY );
  if ( window == nullptr )
    return std::unexpected( sdlError( "SDL_CreateWindow" ) );

  // Every shader format the ImGui backend ships bytecode for, so that SDL may pick the native API:
  // Metal on macOS, Vulkan or D3D12 elsewhere.
  SDL_GPUDevice* const device = SDL_CreateGPUDevice(
      SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB, false, nullptr );
  if ( device == nullptr )
  {
    SDL_DestroyWindow( window );
    return std::unexpected( sdlError( "SDL_CreateGPUDevice" ) );
  }
  if ( !SDL_ClaimWindowForGPUDevice( device, window ) )
  {
    SDL_DestroyGPUDevice( device );
    SDL_DestroyWindow( window );
    return std::unexpected( sdlError( "SDL_ClaimWindowForGPUDevice" ) );
  }
  SDL_SetGPUSwapchainParameters( device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC );

  // From here on the destructor owns the window and the device.
  return std::unique_ptr<Application>{ new Application{ window, device } };
}

Application::Application( SDL_Window* window, SDL_GPUDevice* device )
    : mWindow{ window }, mDevice{ device }, mAudio{ AudioOutput::open() },
      mEmulation{ std::make_unique<EmulationThread>( mAudio.get() ) }
{
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // the keyboard is the pad's (updatePad), not ImGui's navigation's
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

  // The layout is kept with the user's preferences rather than in whatever directory the
  // application was started from.
  if ( char* const prefPath = SDL_GetPrefPath( "JagMCP", "jagmcp" ); prefPath != nullptr )
  {
    mImguiIniPath = std::string{ prefPath } + "imgui.ini";
    SDL_free( prefPath );
    io.IniFilename = mImguiIniPath.c_str();
  }
  else
  {
    io.IniFilename = nullptr;
  }

  // settings.json beside it: the BIOS remembered
  if ( char* const prefPath = SDL_GetPrefPath( "JagMCP", "jagmcp" ); prefPath != nullptr )
  {
    mSettingsPath = std::string{ prefPath } + "settings.json";
    SDL_free( prefPath );
    if ( std::ifstream in{ mSettingsPath }; in )
    {
      mSettings = control::Json::parse( in, nullptr, false );
      if ( !mSettings.is_object() )
        mSettings = control::Json::object();
    }
  }
  if ( auto const bios = mSettings.find( "bios" ); bios != mSettings.end() && bios->is_string() )
  {
    std::error_code failed;
    if ( std::filesystem::is_regular_file( bios->get<std::string>(), failed ) )
      mBios = bios->get<std::string>();
  }

  float const scale = SDL_GetDisplayContentScale( SDL_GetPrimaryDisplay() );
  ImGui::StyleColorsDark();
  ImGui::GetStyle().ScaleAllSizes( scale );
  ImGui::GetStyle().FontScaleDpi = scale;

  ImGui_ImplSDL3_InitForSDLGPU( mWindow );
  ImGui_ImplSDLGPU3_InitInfo initInfo{};
  initInfo.Device = mDevice;
  initInfo.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat( mDevice, mWindow );
  initInfo.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
  ImGui_ImplSDLGPU3_Init( &initInfo );
}

Application::~Application()
{
  SDL_WaitForGPUIdle( mDevice );
  ImGui_ImplSDL3_Shutdown();
  ImGui_ImplSDLGPU3_Shutdown();
  ImGui::DestroyContext();
  mScreen.reset();
  // The servers hand requests to the emulation thread, which feeds the audio stream: they go in
  // that order.
  mMcpHttp.reset();
  mMcp.reset();
  mLineServer.reset();
  mEmulation.reset();
  mAudio.reset();
  SDL_ReleaseWindowFromGPUDevice( mDevice, mWindow );
  SDL_DestroyGPUDevice( mDevice );
  SDL_DestroyWindow( mWindow );
  SDL_Quit();
}

void Application::load( std::vector<std::string> const& args )
{
  // a cartridge, alone or with the runner's arguments: booted by the BIOS given among them, else by
  // the one remembered or found beside it
  std::vector<std::string> runArgs = args;
  if ( auto const cart = std::ranges::find_if( runArgs, []( std::string const& a ) { return isCartridge( a ); } );
       cart != runArgs.end() && std::ranges::find( runArgs, "--cart" ) == runArgs.end() &&
       ( cart == runArgs.begin() || !( cart - 1 )->starts_with( "--" ) ) )
  {
    std::string const path = *cart;
    runArgs.erase( cart );
    if ( std::ranges::find( runArgs, "--bios" ) == runArgs.end() )
    {
      std::string const bios = biosFor( path );
      if ( bios.empty() )
        return;
      runArgs.insert( runArgs.begin(), { "--bios", bios } );
    }
    if ( std::ranges::find( runArgs, "--m68k" ) == runArgs.end() )
      runArgs.insert( runArgs.begin(), "--m68k" );
    runArgs.insert( runArgs.end(), { "--cart", path } );
  }
  RunOptions options;
  try
  {
    options =
        runArgs.size() == 1 && runArgs[0].ends_with( ".run" ) ? readRunFile( runArgs[0] ) : parseRunOptions( runArgs );
  }
  catch ( std::exception const& error )
  {
    mLastError = error.what();
    return;
  }
  if ( auto const failed = mEmulation->load( options ) )
  {
    mLastError = *failed;
    return;
  }
  mLastError.clear();
  // the session writes no runner outputs: the window runs without an end to write them at
  if ( !options.framesDir.empty() || !options.wav.empty() || !options.log.empty() )
  {
    mLastError = "--frames, --wav and --log are not written in the window: use jagmcp-cli for them, "
                 "or --record FILE.mp4 for a video";
    SDL_Log( "%s", mLastError.c_str() );
  }
  mPaused = false;
  mEmulation->setPaused( false );
}

void Application::startRecord( std::string const& path )
{
  control::Json const answer =
      mEmulation->handle( { { "id", 0 }, { "method", "video.record_start" }, { "params", { { "path", path } } } } );
  if ( !answer.value( "ok", false ) )
  {
    mLastError = "Cannot record " + path + ": " + errorMessage( answer );
    SDL_Log( "%s", mLastError.c_str() );
    return;
  }
  mRecordPath = path;
  mRecorded.clear();
  SDL_Log( "recording to %s", path.c_str() );
}

void Application::stopRecord()
{
  SDL_Log( "writing the record" );
  control::Json const answer = mEmulation->handle( { { "id", 0 }, { "method", "video.record_stop" } } );
  mRecordPath.clear();
  if ( !answer.value( "ok", false ) )
  {
    mLastError = "The record failed: " + errorMessage( answer );
    SDL_Log( "%s", mLastError.c_str() );
    return;
  }
  control::Json const& result = answer.at( "result" );
  char text[64];
  std::snprintf( text,
                 sizeof text,
                 ", %.1f s, %d x %d",
                 result.value( "seconds", 0.0 ),
                 result.value( "width", 0 ),
                 result.value( "height", 0 ) );
  mRecorded = result.value( "path", std::string{} ) + text;
  SDL_Log( "wrote %s", mRecorded.c_str() );
}

std::string Application::biosFor( std::filesystem::path const& cartridge )
{
  std::error_code failed;
  if ( !mBios.empty() && std::filesystem::is_regular_file( mBios, failed ) )
    return mBios;
  std::filesystem::path const folder = std::filesystem::absolute( cartridge, failed ).parent_path();
  for ( std::filesystem::path const& place :
        { folder, folder / "bios", folder.parent_path(), folder.parent_path() / "bios" } )
    if ( auto const found = biosIn( place ) )
    {
      if ( setBios( found->string(), true ) )
        return mBios;
    }
  mLastError = "No BIOS beside " + cartridge.filename().string() +
               " or a folder up (jagboot.rom and the like, 128 KB): choose one with File > Choose BIOS";
  return {};
}

bool Application::setBios( std::string const& path, bool keep )
{
  std::error_code failed;
  if ( !std::filesystem::is_regular_file( path, failed ) || std::filesystem::file_size( path, failed ) != BIOS_SIZE )
  {
    mLastError = path + " is not a BIOS image (128 KB)";
    return false;
  }
  mBios = path;
  mLastError.clear();
  if ( keep )
    keepSetting( "bios", path );
  return true;
}

void Application::keepSetting( std::string const& key, control::Json value )
{
  mSettings[key] = std::move( value );
  if ( mSettingsPath.empty() )
    return;
  std::ofstream out{ mSettingsPath };
  out << mSettings.dump( 2 ) << '\n';
  if ( !out )
    SDL_Log( "cannot write %s", mSettingsPath.c_str() );
}

void Application::openFile()
{
  static constexpr SDL_DialogFileFilter FILTERS[] = { { .name = "Jaguar cartridges", .pattern = "j64;jag" },
                                                      { .name = "Programs", .pattern = "run;o;cof" } };
  SDL_ShowOpenFileDialog(
      []( void* self, char const* const* files, int /*filter*/ )
      {
        // null on an error, empty when the dialog was cancelled
        auto* const application = static_cast<Application*>( self );
        if ( files != nullptr && files[0] != nullptr )
        {
          std::scoped_lock const lock{ application->mChosenMutex };
          application->mChosen = files[0];
        }
      },
      this,
      mWindow,
      FILTERS,
      2,
      nullptr,
      false );
}

void Application::chooseBios()
{
  static constexpr SDL_DialogFileFilter FILTERS[] = { { .name = "Jaguar BIOS (128 KB)", .pattern = "rom;bin;j64" } };
  SDL_ShowOpenFileDialog(
      []( void* self, char const* const* files, int /*filter*/ )
      {
        auto* const application = static_cast<Application*>( self );
        if ( files != nullptr && files[0] != nullptr )
        {
          std::scoped_lock const lock{ application->mChosenMutex };
          application->mChosenBios = files[0];
        }
      },
      this,
      mWindow,
      FILTERS,
      1,
      nullptr,
      false );
}

void Application::chooseRecord()
{
  static constexpr SDL_DialogFileFilter FILTERS[] = { { .name = "Video (MP4, MKV, MOV)", .pattern = "mp4;mkv;mov" } };
  SDL_ShowSaveFileDialog(
      []( void* self, char const* const* files, int /*filter*/ )
      {
        auto* const application = static_cast<Application*>( self );
        if ( files != nullptr && files[0] != nullptr )
        {
          std::scoped_lock const lock{ application->mChosenMutex };
          application->mChosenRecord = files[0];
        }
      },
      this,
      mWindow,
      FILTERS,
      1,
      nullptr );
}

void Application::takeChosen()
{
  std::optional<std::string> chosen;
  std::optional<std::string> bios;
  std::optional<std::string> record;
  {
    std::scoped_lock const lock{ mChosenMutex };
    chosen.swap( mChosen );
    bios.swap( mChosenBios );
    record.swap( mChosenRecord );
  }
  if ( bios )
    setBios( *bios, true );
  if ( chosen )
    load( { *chosen } );
  if ( record )
  {
    // the extension chooses the container
    if ( std::filesystem::path{ *record }.extension().empty() )
      *record += ".mp4";
    startRecord( *record );
  }
}

void Application::serve( std::uint16_t linePort, std::uint16_t mcpPort )
{
  auto const handler = [this]( control::Json const& request ) { return mEmulation->handle( request ); };
  if ( linePort != 0 )
  {
    mLineServer = std::make_unique<server::TcpLineServer>( linePort, handler );
    SDL_Log( "serving JSON-lines on 127.0.0.1:%u", static_cast<unsigned>( mLineServer->port() ) );
  }
  if ( mcpPort != 0 )
  {
    mMcp = std::make_unique<server::McpServer>( mEmulation->methods(), handler );
    mMcpHttp = std::make_unique<server::McpHttpServer>( mcpPort, *mMcp );
    SDL_Log( "serving MCP on http://127.0.0.1:%u/mcp", static_cast<unsigned>( mMcpHttp->port() ) );
  }
}

void Application::run()
{
  while ( !mQuit )
  {
    SDL_Event event;
    while ( SDL_PollEvent( &event ) )
    {
      ImGui_ImplSDL3_ProcessEvent( &event );
      if ( event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr )
        load( { event.drop.data } );
      if ( event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_O &&
           ( event.key.mod & ( SDL_KMOD_GUI | SDL_KMOD_CTRL ) ) != 0 )
        openFile();
      if ( event.type == SDL_EVENT_QUIT ||
           ( event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID( mWindow ) ) )
        mQuit = true;
    }

    // A minimised window has no swapchain to present to; waiting here keeps the loop from spinning
    // until it is restored.
    if ( ( SDL_GetWindowFlags( mWindow ) & SDL_WINDOW_MINIMIZED ) != 0 )
    {
      SDL_Delay( 10 );
      continue;
    }

    takeChosen();
    updateEmulation();
    updatePad();

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    drawInterface();
    ImGui::Render();
    renderFrame();
  }
  if ( mEmulation->recording() )
    stopRecord();
}

void Application::updateEmulation()
{
  // the program may be loaded by a request as well as by the window
  if ( std::string program = mEmulation->program(); program != mProgram )
  {
    mProgram = std::move( program );
    std::string const title = "JagMCP - " + std::filesystem::path{ mProgram }.filename().string();
    SDL_SetWindowTitle( mWindow, title.c_str() );
  }
  mPaused = mEmulation->paused(); // a request or a debugger's stop may have paused it
  if ( !mEmulation->recording() )
    mRecordPath.clear(); // a request may have stopped the record
  if ( !mEmulation->takePicture( mPicturesShown, mPicture ) )
    return;
  mPictureChanged = true;
  // Made anew before the interface draws it: SDL keeps the old one until the frames in flight that
  // draw it are done.
  if ( !mScreen || mScreen->width() != mPicture.width || mScreen->height() != mPicture.height )
  {
    mScreen = std::make_unique<GpuTexture>( mDevice, mPicture.width, mPicture.height );
    if ( !mScreen->valid() )
    {
      mLastError = sdlError( "creating the screen texture" );
      mScreen.reset();
    }
  }
}

void Application::updatePad()
{
  // nothing held while a text field of the interface takes the keys
  std::uint32_t buttons = 0;
  if ( !ImGui::GetIO().WantTextInput )
  {
    bool const* const keys = SDL_GetKeyboardState( nullptr );
    for ( KeyButton const& k : KEY_BUTTONS )
      if ( keys[k.key] )
        buttons |= 1u << k.bit;
  }
  if ( buttons == mKeyPad )
    return;
  mKeyPad = buttons;
  mEmulation->setPad( 0, buttons );
}

void Application::drawInterface()
{
  drawMenuBar();
  ImGuiID const dockspace = ImGui::DockSpaceOverViewport();

  // On the first run, before imgui.ini has a layout to restore, the screen fills the dockspace and
  // the status panel floats over it.
  ImGui::SetNextWindowDockID( dockspace, ImGuiCond_FirstUseEver );
  drawScreenWindow();
  drawStatusWindow();
}

void Application::drawMenuBar()
{
  if ( !ImGui::BeginMainMenuBar() )
    return;
  if ( ImGui::BeginMenu( "File" ) )
  {
    if ( ImGui::MenuItem( "Open...", SDL_GetPlatform() == std::string_view{ "macOS" } ? "Cmd+O" : "Ctrl+O" ) )
      openFile();
    if ( ImGui::MenuItem( "Choose BIOS..." ) )
      chooseBios();
    ImGui::Separator();
    if ( mEmulation->recording() )
    {
      if ( ImGui::MenuItem( "Stop recording" ) )
        stopRecord();
    }
    else if ( ImGui::MenuItem( "Record video...", nullptr, false, mEmulation->loaded() ) )
    {
      chooseRecord();
    }
    ImGui::Separator();
    if ( ImGui::MenuItem( "Quit" ) )
      mQuit = true;
    ImGui::EndMenu();
  }
  if ( ImGui::BeginMenu( "Emulation" ) )
  {
    if ( ImGui::MenuItem( "Pause", nullptr, &mPaused, mEmulation->loaded() ) )
      mEmulation->setPaused( mPaused );
    if ( ImGui::MenuItem( "Restart", nullptr, false, mEmulation->loaded() ) )
      mEmulation->restart();
    ImGui::EndMenu();
  }
  if ( ImGui::BeginMenu( "View" ) )
  {
    ImGui::MenuItem( STATUS_WINDOW, nullptr, &mShowStatus );
    ImGui::EndMenu();
  }
  ImGui::EndMainMenuBar();
}

void Application::drawScreenWindow()
{
  ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2{ 0.0F, 0.0F } );
  bool const open = ImGui::Begin( SCREEN_WINDOW, nullptr, ImGuiWindowFlags_NoScrollbar );
  ImGui::PopStyleVar();
  if ( open && mScreen )
  {
    // The frame is what the video output shows outside its blanking, the television's picture: it is
    // shown 4:3 (0017), as large as fits, its lines by whole multiples of the display's pixels where
    // it fits at least once, centred on a whole pixel.
    constexpr float ASPECT = 4.0F / 3.0F;
    float const pixelsPerPoint = ImGui::GetIO().DisplayFramebufferScale.y;
    ImVec2 const available = ImGui::GetContentRegionAvail();
    auto const height = static_cast<float>( mScreen->height() );
    float scale = std::min( available.x * pixelsPerPoint / ( height * ASPECT ), available.y * pixelsPerPoint / height );
    if ( scale >= 1.0F )
      scale = std::floor( scale );
    ImVec2 const size{ std::round( height * scale * ASPECT ) / pixelsPerPoint, height * scale / pixelsPerPoint };
    ImVec2 const cursor = ImGui::GetCursorScreenPos();
    auto const onPixel = [pixelsPerPoint]( float points )
    { return std::round( points * pixelsPerPoint ) / pixelsPerPoint; };
    ImGui::SetCursorScreenPos( ImVec2{ onPixel( cursor.x + std::max( 0.0F, ( available.x - size.x ) / 2.0F ) ),
                                       onPixel( cursor.y + std::max( 0.0F, ( available.y - size.y ) / 2.0F ) ) } );

    // Sampled nearest, the sampler restored for the rest of the interface.
    ImDrawList* const drawList = ImGui::GetWindowDrawList();
    ImGuiPlatformIO const& platform = ImGui::GetPlatformIO();
    drawList->AddCallback( platform.DrawCallback_SetSamplerNearest, nullptr );
    ImGui::Image( ImTextureRef{ reinterpret_cast<ImTextureID>( mScreen->texture() ) }, size );
    drawList->AddCallback( platform.DrawCallback_SetSamplerLinear, nullptr );
  }
  else if ( open )
  {
    ImGui::TextDisabled( mEmulation->loaded()
                             ? "No frame yet"
                             : "Drop a cartridge (.j64), a .run or a program onto the window, or use File > Open" );
  }
  ImGui::End();
}

void Application::drawStatusWindow()
{
  if ( !mShowStatus )
    return;
  if ( ImGui::Begin( STATUS_WINDOW, &mShowStatus ) )
  {
    if ( !mEmulation->loaded() )
    {
      ImGui::TextUnformatted( "No program" );
    }
    else
    {
      ImGui::TextUnformatted( std::filesystem::path{ mProgram }.filename().string().c_str() );
      char const* state = "running";
      if ( !mEmulation->running() )
        state = "stopped";
      else if ( mPaused )
        state = "paused";
      ImGui::Text( "State: %s", state );
      if ( std::string const stop = mEmulation->stop(); !stop.empty() )
        ImGui::TextUnformatted( stop.c_str() );
      ImGui::Text( "Time: %.3f s", static_cast<double>( mEmulation->cycles() ) / CYCLES_PER_SECOND );
      ImGui::Text( "Speed: %.0f%%", mEmulation->speed() * 100.0 );
      if ( mScreen )
        ImGui::Text( "Frame: %u x %u", mScreen->width(), mScreen->height() );
      if ( std::vector<std::string> const undriven = mEmulation->undrivenReads(); !undriven.empty() )
      {
        ImGui::Separator();
        ImGui::PushTextWrapPos( 0.0F );
        ImGui::TextColored( ImVec4{ 1.0F, 0.8F, 0.3F, 1.0F },
                            "Reads no register answers: they get what the bus held, which works by chance" );
        for ( std::string const& line : undriven )
          ImGui::BulletText( "%s", line.c_str() );
        ImGui::PopTextWrapPos();
      }
    }
    // the BIOS the program boots with, which a request may have given; and the one a cartridge
    // opened in the window boots with, where it is another
    std::string const running = mEmulation->loaded() ? mEmulation->bios() : std::string{};
    if ( mEmulation->loaded() )
      ImGui::Text( "BIOS: %s",
                   running.empty() ? "none" : std::filesystem::path{ running }.filename().string().c_str() );
    if ( !mEmulation->loaded() || mBios != running )
      ImGui::Text( "BIOS for cartridges: %s",
                   mBios.empty() ? "none chosen" : std::filesystem::path{ mBios }.filename().string().c_str() );
    if ( mAudio )
      ImGui::Text( "Sound: %d Hz, %.0f ms queued", mAudio->rate(), mAudio->queuedSeconds() * 1000.0 );
    else
      ImGui::TextUnformatted( "Sound: no device" );
    if ( mEmulation->recording() )
      ImGui::TextColored( ImVec4{ 1.0F, 0.4F, 0.4F, 1.0F },
                          "Recording: %s",
                          mRecordPath.empty() ? "started by a request" : mRecordPath.c_str() );
    else if ( !mRecorded.empty() )
      ImGui::Text( "Recorded: %s", mRecorded.c_str() );
    if ( !mLastError.empty() )
    {
      ImGui::Separator();
      ImGui::PushTextWrapPos( 0.0F );
      ImGui::TextColored( ImVec4{ 1.0F, 0.4F, 0.4F, 1.0F }, "%s", mLastError.c_str() );
      ImGui::PopTextWrapPos();
    }
  }
  ImGui::End();
}

void Application::renderFrame()
{
  ImDrawData* const drawData = ImGui::GetDrawData();
  bool const minimised = drawData->DisplaySize.x <= 0.0F || drawData->DisplaySize.y <= 0.0F;

  SDL_GPUCommandBuffer* const commands = SDL_AcquireGPUCommandBuffer( mDevice );
  if ( commands == nullptr )
    return;

  if ( mPictureChanged && mScreen )
    mScreen->upload( commands, mPicture.rgba );
  mPictureChanged = false;

  SDL_GPUTexture* swapchain = nullptr;
  if ( SDL_WaitAndAcquireGPUSwapchainTexture( commands, mWindow, &swapchain, nullptr, nullptr ) &&
       swapchain != nullptr && !minimised )
  {
    // The backend has to upload its vertices before the render pass begins.
    ImGui_ImplSDLGPU3_PrepareDrawData( drawData, commands );

    SDL_GPUColorTargetInfo target{};
    target.texture = swapchain;
    target.clear_color = SDL_FColor{ .r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F };
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* const pass = SDL_BeginGPURenderPass( commands, &target, 1, nullptr );
    ImGui_ImplSDLGPU3_RenderDrawData( drawData, commands, pass );
    SDL_EndGPURenderPass( pass );
  }
  SDL_SubmitGPUCommandBuffer( commands );
}

} // namespace jag::app
