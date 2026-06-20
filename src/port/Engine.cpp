#include "Engine.h"

#include "ship/utils/StringHelper.h"
#include "GameExtractor.h"
#include "mods/ModManager.h"
#include "ui/ImguiUI.h"
#include "ship/Context.h"
#include "ship/controller/controldevice/controller/mapping/ControllerDefaultMappings.h"
#include "resource/type/ResourceType.h"
#include "fast/resource/ResourceType.h"
#include "resource/importers/GenericArrayFactory.h"
#include "resource/importers/AudioBankFactory.h"
#include "resource/importers/AudioSampleFactory.h"
#include "resource/importers/AudioSequenceFactory.h"
#include "resource/importers/Vec3fFactory.h"
#include "resource/importers/Vec3sFactory.h"
#include "resource/importers/CPUFactory.h"
#include "resource/importers/CourseVtxFactory.h"
#include "resource/importers/TrackSectionsFactory.h"
#include "resource/importers/TrackPathPointFactory.h"
#include "resource/importers/ActorSpawnDataFactory.h"
#include "resource/importers/UnkActorSpawnDataFactory.h"
#include "resource/importers/ArrayFactory.h"
#include "resource/importers/MinimapFactory.h"
#include "resource/importers/BetterTextureFactory.h"
#include <ship/window/gui/Fonts.h>
#include "ship/window/gui/resource/Font.h"
#include "ship/window/gui/resource/FontFactory.h"
#include "libultraship/controller/controldeck/ControlDeck.h"
#include "SpaghettiGui.h"

#include "port/interpolation/FrameInterpolation.h"
#include "port/vr/vr.h"
#include <fast/Fast3dWindow.h>
#include <fast/interpreter.h>
// #include <Fast3D/gfx_rendering_api.h>
#include <SDL2/SDL.h>
#if defined(ENABLE_VR) && defined(_WIN32)
#include <imgui_impl_sdl2.h> // ImGui_ImplSDL2_SetGamepadMode - force the menu pad nav to (re)open the device
#endif

#include <utility>

#ifdef __SWITCH__
#include <ship/port/switch/SwitchImpl.h>
#endif

extern "C" {
bool prevAltAssets = false;
float gInterpolationStep = 0.0f;
#include <macros.h>
#include <fast/resource/factory/DisplayListFactory.h>
#include <fast/resource/factory/TextureFactory.h>
#include <fast/resource/factory/MatrixFactory.h>
#include <ship/resource/factory/BlobFactory.h>
#include <fast/resource/factory/VertexFactory.h>
#include <fast/resource/factory/LightFactory.h>
// #include <PngFactory.h>
#include "audio/internal.h"
#include "audio/GameAudio.h"
}

Fast::Interpreter* GetInterpreter() {
    return static_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow())
        ->GetInterpreterWeak()
        .lock()
        .get();
}

GameEngine* GameEngine::Instance;

bool CreateDirectoryRecursive(std::string const& dirName, std::error_code& err) {
    err.clear();
    if (!std::filesystem::create_directories(dirName, err)) {
        if (std::filesystem::exists(dirName)) {
            // The folder already exists:
            err.clear();
            return true;
        }
        return false;
    }
    return true;
}

GameEngine::GameEngine() {
    // Initialize context properties early to recognize paths properly for non-portable builds
    this->context = Ship::Context::CreateUninitializedInstance("Spaghetti Kart", "spaghettify", "spaghettify.cfg.json");

#ifdef __SWITCH__
    Ship::Switch::Init(Ship::PreInitPhase);
    Ship::Switch::Init(Ship::PostInitPhase);
#endif

#ifdef _WIN32
    AllocConsole();
#endif

    this->context->InitConfiguration();    // without this line InitConsoleVariables fails at Config::Reload()
    this->context->InitConsoleVariables(); // without this line the controldeck constructor failes in
                                           // ShipDeviceIndexMappingManager::UpdateControllerNamesFromConfig()

    // Unlimited draw distance everywhere, flat and VR alike: nothing should fade out, LOD-swap or
    // pop in. No Culling + Disable LOD + Far Frustum forced on at every boot. The far plane is the
    // last thing that can still clip: at the old 10000 anything past it (hazard props scattered
    // across the track especially) appeared out of thin air, so it sits far beyond any course now.
    CVarSetInteger("gNoCulling", 1);
    CVarSetInteger("gDisableLod", 1);
    if (CVarGetFloat("gFarFrustrum", 0.0f) < 100000.0f) {
        CVarSetFloat("gFarFrustrum", 100000.0f);
    }

#if defined(ENABLE_VR) && defined(_WIN32)
    // VR: SDL_main decided whether to enable VR (--vr / --novr / headset auto-detect) and called
    // vr_request_enable(). OpenXR binds to the WGL context, so force the OpenGL backend before the
    // window is created (context->Init below reads Window.Backend.Id to choose the renderer).
    if (vr_is_requested()) {
        this->context->GetConfig()->SetInt("Window.Backend.Id", (int32_t)Ship::WindowBackend::FAST3D_SDL_OPENGL);
        this->context->GetConfig()->SetString("Window.Backend.Name", "OpenGL");
        SPDLOG_INFO("[VR] requested - forced OpenGL backend (OpenXR binds to WGL)");
        // Gamepad menu navigation - the mouse cursor isn't usable in the headset, so the menu must be
        // drivable with the controller.
        CVarSetInteger("gControlNav", 1);
        // VR steals OS focus to the compositor, so the desktop SDL window runs in the background. Without
        // these, SDL stops updating gamepad state for the unfocused window and the ImGui menu nav goes
        // dead. (Set before the joystick subsystem comes up; also belt-and-suspendered per-frame by an
        // explicit SDL_GameControllerUpdate() in the VR menu path.)
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        CVarSetInteger("gAllowBackgroundInputs", 1);
        CVarSave();
        SPDLOG_INFO("[VR] enabled VR render defaults: No Culling, Disable LOD, Far Frustum, Controller Nav");
    }
#endif

    auto defaultMappings = std::make_shared<Ship::ControllerDefaultMappings>(
        // KeyboardKeyToButtonMappings
        std::unordered_map<CONTROLLERBUTTONS_T, std::unordered_set<Ship::KbScancode>>{
            { BTN_A, { Ship::KbScancode::LUS_KB_SHIFT} },
            { BTN_B, { Ship::KbScancode::LUS_KB_CONTROL} },
            { BTN_L, { Ship::KbScancode::LUS_KB_Q} },
            { BTN_R, { Ship::KbScancode::LUS_KB_SPACE} },
            { BTN_Z, { Ship::KbScancode::LUS_KB_Z} },
            { BTN_START, { Ship::KbScancode::LUS_KB_ENTER} },
            { BTN_CUP, { Ship::KbScancode::LUS_KB_T} },
            { BTN_CDOWN, { Ship::KbScancode::LUS_KB_G} },
            { BTN_CLEFT, { Ship::KbScancode::LUS_KB_F} },
            { BTN_CRIGHT, { Ship::KbScancode::LUS_KB_H} },
            { BTN_DUP, { Ship::KbScancode::LUS_KB_NUMPAD8} },
            { BTN_DDOWN, { Ship::KbScancode::LUS_KB_NUMPAD2} },
            { BTN_DLEFT, { Ship::KbScancode::LUS_KB_NUMPAD4} },
            { BTN_DRIGHT, { Ship::KbScancode::LUS_KB_NUMPAD6} }
        },
        // KeyboardKeyToAxisDirectionMappings - use built-in LUS defaults
        std::unordered_map<Ship::StickIndex, std::vector<std::pair<Ship::Direction, Ship::KbScancode>>>{
            { Ship::StickIndex::LEFT_STICK, {
                { Ship::Direction::UP, Ship::KbScancode::LUS_KB_ARROWKEY_UP},
                { Ship::Direction::DOWN, Ship::KbScancode::LUS_KB_ARROWKEY_DOWN},
                { Ship::Direction::LEFT, Ship::KbScancode::LUS_KB_ARROWKEY_LEFT},
                { Ship::Direction::RIGHT, Ship::KbScancode::LUS_KB_ARROWKEY_RIGHT}
            }}
        },
        // SDLButtonToButtonMappings
        std::unordered_map<CONTROLLERBUTTONS_T, std::unordered_set<SDL_GameControllerButton>>{
            { BTN_A, { SDL_CONTROLLER_BUTTON_A } },
            { BTN_B, { SDL_CONTROLLER_BUTTON_X } },
            { BTN_START, { SDL_CONTROLLER_BUTTON_START } },
            { BTN_CLEFT, { SDL_CONTROLLER_BUTTON_Y } },
            { BTN_CDOWN, { SDL_CONTROLLER_BUTTON_B } },
            { BTN_DUP, { SDL_CONTROLLER_BUTTON_DPAD_UP } },
            { BTN_DDOWN, { SDL_CONTROLLER_BUTTON_DPAD_DOWN } },
            { BTN_DLEFT, { SDL_CONTROLLER_BUTTON_DPAD_LEFT } },
            { BTN_DRIGHT, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT } },
            { BTN_R, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER } },
            { BTN_L, { SDL_CONTROLLER_BUTTON_LEFTSHOULDER } }
        },
        // SDLButtonToAxisDirectionMappings - use built-in LUS defaults
        std::unordered_map<Ship::StickIndex, std::vector<std::pair<Ship::Direction, SDL_GameControllerButton>>>(),
        // SDLAxisDirectionToButtonMappings
        std::unordered_map<CONTROLLERBUTTONS_T, std::vector<std::pair<SDL_GameControllerAxis, int32_t>>>{
            { BTN_R, { { SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1 } } },
            { BTN_Z, { { SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1 } } },
            { BTN_CUP, { { SDL_CONTROLLER_AXIS_RIGHTY, -1 } } },
            { BTN_CRIGHT, { { SDL_CONTROLLER_AXIS_RIGHTX, 1 } } }
        },
        // SDLAxisDirectionToAxisDirectionMappings - use built-in LUS defaults
        std::unordered_map<Ship::StickIndex, std::vector<std::pair<Ship::Direction, std::pair<SDL_GameControllerAxis, int32_t>>>>()
    );

    auto buttonNames = std::unordered_map<CONTROLLERBUTTONS_T, std::string>({
                      { BTN_A, "A" },
                      { BTN_B, "B" },
                      { BTN_L, "L" },
                      { BTN_R, "R" },
                      { BTN_Z, "Z" },
                      { BTN_START, "Start" },
                      { BTN_CLEFT, "CLeft" },
                      { BTN_CRIGHT, "CRight" },
                      { BTN_CUP, "CUp" },
                      { BTN_CDOWN, "CDown" },
                      { BTN_DLEFT, "DLeft" },
                      { BTN_DRIGHT, "DRight" },
                      { BTN_DUP, "DUp" },
                      { BTN_DDOWN, "DDown" },
                  });
    auto controlDeck = std::make_shared<LUS::ControlDeck>(std::vector<CONTROLLERBUTTONS_T>(), defaultMappings, buttonNames);
    const std::string assets_path = Ship::Context::LocateFileAcrossAppDirs(engine_asset_file);
    this->context->InitResourceManager({assets_path}, {}, 3); // without this line InitWindow fails in Gui::Init()
    this->context->InitConsole(); // without this line the GuiWindow constructor fails in ConsoleWindow::InitElement()

    auto gui = std::make_shared<Ship::SpaghettiGui>(std::vector<std::shared_ptr<Ship::GuiWindow>>({}));
    auto wnd = std::make_shared<Fast::Fast3dWindow>(gui);

    // auto wnd = std::make_shared<Fast::Fast3dWindow>(std::vector<std::shared_ptr<Ship::GuiWindow>>({}));
    // auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());

    this->context->Init({assets_path}, {}, 3, { 26800, 512, 1100 }, wnd, controlDeck);

#ifndef __SWITCH__
    Ship::Context::GetInstance()->GetLogger()->set_level(
        (spdlog::level::level_enum) CVarGetInteger("gDeveloperTools.LogLevel", 1));
    Ship::Context::GetInstance()->GetLogger()->set_pattern("[%H:%M:%S.%e] [%s:%#] [%l] %v");
#endif

    SPDLOG_INFO("Spaghetti Kart " SPAGHETTI_VERSION);
    SPDLOG_INFO(CVarGetInteger("gEnableDebugMode", 0) == 0 ? "Debug Mode deactivated" : "Debug Mode activated");

    wnd->SetRendererUCode(ucode_f3dex);
    this->context->InitGfxDebugger();

    auto loader = context->GetResourceManager()->GetResourceLoader();
    loader->RegisterResourceFactory(std::make_shared<SM64::AudioBankFactoryV0>(), RESOURCE_FORMAT_BINARY, "AudioBank",
                                    static_cast<uint32_t>(SF64::ResourceType::Bank), 0);
    loader->RegisterResourceFactory(std::make_shared<SM64::AudioSampleFactoryV0>(), RESOURCE_FORMAT_BINARY,
                                    "AudioSample", static_cast<uint32_t>(SF64::ResourceType::Sample), 0);
    loader->RegisterResourceFactory(std::make_shared<SM64::AudioSequenceFactoryV0>(), RESOURCE_FORMAT_BINARY,
                                    "AudioSequence", static_cast<uint32_t>(SF64::ResourceType::Sequence), 0);
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryVec3fV0>(), RESOURCE_FORMAT_BINARY,
                                    "Vec3f", static_cast<uint32_t>(SF64::ResourceType::Vec3f), 0);
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryVec3sV0>(), RESOURCE_FORMAT_BINARY,
                                    "Vec3s", static_cast<uint32_t>(SF64::ResourceType::Vec3s), 0);
    loader->RegisterResourceFactory(std::make_shared<SF64::ResourceFactoryBinaryGenericArrayV0>(),
                                    RESOURCE_FORMAT_BINARY, "GenericArray",
                                    static_cast<uint32_t>(SF64::ResourceType::GenericArray), 0);
    // loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryTextureV0>(), RESOURCE_FORMAT_BINARY,
    //                                 "Texture", static_cast<uint32_t>(Fast::ResourceType::Texture), 0);
    // loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryTextureV1>(), RESOURCE_FORMAT_BINARY,
    //                                 "Texture", static_cast<uint32_t>(Fast::ResourceType::Texture), 1);
    loader->RegisterResourceFactory(std::make_shared<MK64::ResourceFactoryBinaryTextureV0>(), RESOURCE_FORMAT_BINARY,
                                    "Texture", static_cast<uint32_t>(Fast::ResourceType::Texture), 0);
    loader->RegisterResourceFactory(std::make_shared<MK64::ResourceFactoryBinaryTextureV1>(), RESOURCE_FORMAT_BINARY,
                                    "Texture", static_cast<uint32_t>(Fast::ResourceType::Texture), 1);
    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryVertexV0>(), RESOURCE_FORMAT_BINARY,
                                    "Vertex", static_cast<uint32_t>(Fast::ResourceType::Vertex), 0);
    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryXMLVertexV0>(), RESOURCE_FORMAT_XML, "Vertex",
                                    static_cast<uint32_t>(Fast::ResourceType::Vertex), 0);

    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryDisplayListV0>(),
                                    RESOURCE_FORMAT_BINARY, "DisplayList",
                                    static_cast<uint32_t>(Fast::ResourceType::DisplayList), 0);
    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryXMLDisplayListV0>(), RESOURCE_FORMAT_XML,
                                    "DisplayList", static_cast<uint32_t>(Fast::ResourceType::DisplayList), 0);

    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryMatrixV0>(), RESOURCE_FORMAT_BINARY,
                                    "Matrix", static_cast<uint32_t>(Fast::ResourceType::Matrix), 0);
    loader->RegisterResourceFactory(std::make_shared<Ship::ResourceFactoryBinaryBlobV0>(), RESOURCE_FORMAT_BINARY,
                                    "Blob", static_cast<uint32_t>(Ship::ResourceType::Blob), 0);
    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryLightV0>(), RESOURCE_FORMAT_BINARY,
                                    "Lights1", static_cast<uint32_t>(Fast::ResourceType::Light), 0);
    loader->RegisterResourceFactory(std::make_shared<MK64::ResourceFactoryBinaryArrayV0>(), RESOURCE_FORMAT_BINARY,
                                    "Array", static_cast<uint32_t>(MK64::ResourceType::MK_Array), 0);
    loader->RegisterResourceFactory(std::make_shared<MK64::ResourceFactoryBinaryCPUV0>(), RESOURCE_FORMAT_BINARY, "CPU",
                                    static_cast<uint32_t>(MK64::ResourceType::CPU), 0);
    loader->RegisterResourceFactory(std::make_shared<MK64::ResourceFactoryBinaryCourseVtxV0>(), RESOURCE_FORMAT_BINARY,
                                    "CourseVtx", static_cast<uint32_t>(MK64::ResourceType::CourseVertex), 0);
    loader->RegisterResourceFactory(std::make_shared<MK64::ResourceFactoryBinaryTrackSectionsV0>(),
                                    RESOURCE_FORMAT_BINARY, "TrackSections",
                                    static_cast<uint32_t>(MK64::ResourceType::TrackSection), 0);
    loader->RegisterResourceFactory(std::make_shared<MK64::ResourceFactoryXMLTrackSectionsV0>(),
                                    RESOURCE_FORMAT_XML, "TrackSections",
                                    static_cast<uint32_t>(MK64::ResourceType::TrackSection), 0);
    loader->RegisterResourceFactory(std::make_shared<MK64::ResourceFactoryBinaryTrackPathPointsV0>(),
                                    RESOURCE_FORMAT_BINARY, "Paths",
                                    static_cast<uint32_t>(MK64::ResourceType::Paths), 0);
    loader->RegisterResourceFactory(std::make_shared<MK64::ResourceFactoryXMLTrackPathPointsV0>(),
                                    RESOURCE_FORMAT_XML, "Paths",
                                    static_cast<uint32_t>(MK64::ResourceType::Paths), 0);
    loader->RegisterResourceFactory(std::make_shared<MK64::ResourceFactoryBinaryActorSpawnDataV0>(),
                                    RESOURCE_FORMAT_BINARY, "SpawnData",
                                    static_cast<uint32_t>(MK64::ResourceType::SpawnData), 0);
    loader->RegisterResourceFactory(std::make_shared<MK64::ResourceFactoryBinaryUnkActorSpawnDataV0>(),
                                    RESOURCE_FORMAT_BINARY, "UnkSpawnData",
                                    static_cast<uint32_t>(MK64::ResourceType::UnkSpawnData), 0);
    loader->RegisterResourceFactory(std::make_shared<MK64::ResourceFactoryBinaryMinimapV0>(), RESOURCE_FORMAT_BINARY,
                                    "Minimap", static_cast<uint32_t>(MK64::ResourceType::Minimap), 0);

    fontMono = CreateFontWithSize(16.0f, "fonts/Inconsolata-Regular.ttf");
    fontMonoLarger = CreateFontWithSize(20.0f, "fonts/Inconsolata-Regular.ttf");
    fontMonoLargest = CreateFontWithSize(24.0f, "fonts/Inconsolata-Regular.ttf");
    fontStandard = CreateFontWithSize(16.0f, "fonts/Montserrat-Regular.ttf");
    fontStandardLarger = CreateFontWithSize(20.0f, "fonts/Montserrat-Regular.ttf");
    fontStandardLargest = CreateFontWithSize(24.0f, "fonts/Montserrat-Regular.ttf");
    ImGui::GetIO().FontDefault = fontMono;
}

bool GameEngine::GenAssetFile() {
    auto extractor = new GameExtractor();

    if (!extractor->SelectGameFromUI()) {
        ShowMessage("Error", "No ROM selected.\n\nExiting...");
        exit(1);
    }

    auto game = extractor->ValidateChecksum();
    if (!game.has_value()) {
        ShowMessage("Unsupported ROM",
                    "The provided ROM is not supported.\n\nCheck the readme for a list of supported versions.");
        exit(1);
    }

    ShowMessage(("Found " + game.value()).c_str(),
                "The extraction process will now begin.\n\nThis may take a few minutes.", SDL_MESSAGEBOX_INFORMATION);

    return extractor->GenerateOTR();
}

uint32_t GameEngine::GetInterpolationFPS() {
#if defined(ENABLE_VR) && defined(_WIN32)
    // VR: target the headset's refresh so the per-eye render loop paces to the HMD (via xrWaitFrame)
    // while game logic stays ~30Hz. Using the monitor refresh here would desync game speed.
    if (vr_is_active()) {
        int hz = vr_display_refresh_hz();
        return (uint32_t)(hz >= 30 ? hz : 72);
    }
#endif
    if (CVarGetInteger("gMatchRefreshRate", 1)) { // default ON: uncap to the monitor's refresh rate
        return Ship::Context::GetInstance()->GetWindow()->GetCurrentRefreshRate();

    } else if (CVarGetInteger("gVsyncEnabled", 1) ||
               !Ship::Context::GetInstance()->GetWindow()->CanDisableVerticalSync()) {
        return std::min<uint32_t>(Ship::Context::GetInstance()->GetWindow()->GetCurrentRefreshRate(),
                                  CVarGetInteger("gInterpolationFPS", 30));
    }

    return CVarGetInteger("gInterpolationFPS", 30);
}

uint32_t GameEngine::GetInterpolationFrameCount() {
    return ceil((float) GetInterpolationFPS() / (60.0f / 2 /*gVIsPerFrame*/));
}

extern "C" uint32_t GameEngine_GetInterpolationFrameCount() {
    return GameEngine::GetInterpolationFrameCount();
}

// Clean app quit for game-side C code (the main menu's QUIT box - src/quit_menu.c). Same path as the
// window X button (SDL_QUIT -> Close()) and the ImGui power button: the backend flips mIsRunning = false,
// SDL_main's while (WindowIsRunning()) loop exits, then CustomEngineDestroy() + GameEngine::Destroy().
extern "C" void GameEngine_RequestQuit(void) {
    Ship::Context::GetInstance()->GetWindow()->Close();
}

// Final teardown on quit - SDL_main calls this AFTER the normal cleanup to GUARANTEE the app and its
// --console window actually terminate. vr_shutdown() releases OpenXR (it was never being called on quit,
// so the live session/instance kept runtime threads alive past window close, leaving the process - and
// its console - running). FreeConsole() drops the console window, and ExitProcess(0) hard-exits so no
// lingering VR / audio / SDL thread can keep the process up. Covers every quit path (menu QUIT box, the
// window X button, the ImGui power button) since they all funnel through SDL_main's loop exit.
extern "C" void GameEngine_TerminateProcess(void) {
    vr_shutdown(); // no-op when VR is off (all handles NULL-guarded)
#ifdef _WIN32
    FreeConsole();
    ExitProcess(0);
#endif
}

// The framebuffer-capture effects (the Luigi Raceway / Wario Stadium jumbotron screens,
// src/racing/framebuffer_effects.c) read the window backbuffer, which is stale while VR renders
// into the interpreter's managed eye framebuffer instead - hand them that fb to capture from.
// Returns -1 when VR isn't rendering (or hasn't created the fb yet), meaning use the backbuffer.
extern "C" int32_t GameEngine_GetVrEyeFb(void) {
    if (!vr_is_active()) {
        return -1;
    }
    return GetInterpreter()->GetVrFbId();
}

// Registers a framebuffer as a scaled-copy target: every gDPCopyFB into it scale-blits the whole
// source (the render dims at execution time - per-eye sizes in VR, the internal render size on
// flat) into dst rect [0,0,dstW,dstH]. Sticky so VR's per-eye display-list re-execution scales the
// copy in BOTH eyes; the default copy is a 1:1 region copy that clips at high render resolutions.
extern "C" void GameEngine_SetFbCopyScaledTarget(int32_t fbId, int32_t dstW, int32_t dstH) {
    GetInterpreter()->SetFbCopyScaledTarget(fbId, dstW, dstH);
}

// The VR eye render size, for effects that need the captured content's true aspect.
extern "C" void GameEngine_GetVrEyeDims(int32_t* width, int32_t* height) {
    if (width != nullptr) {
        *width = vr_eye_width(0);
    }
    if (height != nullptr) {
        *height = vr_eye_height(0);
    }
}

void GameEngine::ShowMessage(const char* title, const char* message, SDL_MessageBoxFlags type) {
#if defined(__SWITCH__)
    SPDLOG_ERROR(message);
#else
    SDL_ShowSimpleMessageBox(type, title, message, nullptr);
    SPDLOG_ERROR(message);
#endif
}

int GameEngine::ShowYesNoBox(const char* title, const char* box) {
    int ret;
#ifdef _WIN32
    ret = MessageBoxA(nullptr, box, title, MB_YESNO | MB_ICONQUESTION);
#elif defined(__SWITCH__)
    SPDLOG_ERROR(box);
    return IDYES;
#else
    SDL_MessageBoxData boxData = { 0 };
    SDL_MessageBoxButtonData buttons[2] = { { 0 } };

    buttons[0].buttonid = IDYES;
    buttons[0].text = "Yes";
    buttons[0].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
    buttons[1].buttonid = IDNO;
    buttons[1].text = "No";
    buttons[1].flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
    boxData.numbuttons = 2;
    boxData.flags = SDL_MESSAGEBOX_INFORMATION;
    boxData.message = box;
    boxData.title = title;
    boxData.buttons = buttons;
    SDL_ShowMessageBox(&boxData, &ret);
#endif
    return ret;
}

void GameEngine::Create() {
    const auto instance = Instance = new GameEngine();
    InitModsSystem();
    instance->gHMAS = new HMAS();
    instance->AudioInit();
    GameUI::SetupGuiElements();
#if defined(__SWITCH__) || defined(__WIIU__)
    CVarRegisterInteger("gControlNav", 1); // always enable controller nav on switch/wii u
#endif
}

void GameEngine::Destroy() {
    AudioExit();
#ifdef __SWITCH__
    Ship::Switch::Exit();
#endif
    UnloadMods();
    GameUI::Destroy();
    delete GameEngine::Instance;
    GameEngine::Instance = nullptr;
}

bool ShouldClearTextureCacheAtEndOfFrame = false;

void GameEngine::StartFrame() const {
    using Ship::KbScancode;
    const int32_t dwScancode = this->context->GetWindow()->GetLastScancode();
    this->context->GetWindow()->SetLastScancode(-1);

    switch (dwScancode) {
        case KbScancode::LUS_KB_TAB: {
            // Toggle HD Assets
            CVarSetInteger("gEnhancements.Mods.AlternateAssets", !CVarGetInteger("gEnhancements.Mods.AlternateAssets", 0));
            break;
        }
        case KbScancode::LUS_KB_P: {

            break;
        }
        default:
            break;
    }
}

// void GameEngine::ProcessFrame(void (*run_one_game_iter)()) const {
//     //this->context->GetWindow()->MainLoop(run_one_game_iter);
//     Instance->context->GetWindow()->MainLoop(run_one_game_iter);
// }

#if defined(ENABLE_VR) && defined(_WIN32)
extern "C" int gGamestate;             // src/main.c; RACING == 4 (defines.h). Else == non-gameplay screen.
extern "C" unsigned short gIsGamePaused; // src/code_800029B0.h; nonzero == paused (pause menu over a frozen race)
// VR sky dome (src/engine/sky/SkyDomeVR.cpp): a gradient sphere drawn at infinity so the sky anchors to head
// yaw + pitch. Rebuilt once per game frame; passed to RunVrEye with vr_sky_viewproj.
extern "C" Gfx* build_sky_dome_vr(void);
extern "C" Gfx* gVrSkyDomeGfx;
extern "C" unsigned int gVrSkyDomeFrame;
extern "C" int gGlobalTimer;

// Feed ImGui's menu gamepad navigation directly from the first SDL controller, INDEPENDENT of OS window
// focus. In VR the headset compositor holds focus, so the desktop SDL window is "background" and ImGui's
// ImGui_ImplSDL2 auto-read (SDL_GameControllerGetButton) returns 0 for every button -> menu renders but
// gets no nav input. SDL_GameControllerUpdate() force-refreshes the pad state regardless of focus, then we
// push the exact ImGuiKey_Gamepad* nav events ImGui needs. Co-exists with the auto-read (same keys agree).
static void VrFeedImGuiGamepadNav() {
    SDL_GameControllerUpdate(); // refresh pad state even when the SDL window is not the focused window
    // Open + read ALL game controllers and MERGE their input. The diagnostic showed 2 joysticks; reading
    // only the first picked the wrong device (a phantom/virtual pad), so nav got no input. Merging means
    // whichever device the user actually holds drives the menu.
    SDL_GameController* pads[8];
    int padCount = 0;
    for (int i = 0, n = SDL_NumJoysticks(); i < n && padCount < 8; i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameController* gc = SDL_GameControllerOpen(i); // ref-counted; same handles ControlDeck holds
            if (gc) pads[padCount++] = gc;
        }
    }
    static bool sNavDiagLogged = false;
    if (!sNavDiagLogged) {
        sNavDiagLogged = true;
        ImGuiIO& dio = ImGui::GetIO();
        SPDLOG_INFO("[VR-NAV-DIAG] numJoysticks={} padsOpened={} NavEnableGamepad={} HasGamepad={}",
                    SDL_NumJoysticks(), padCount,
                    (bool)(dio.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad),
                    (bool)(dio.BackendFlags & ImGuiBackendFlags_HasGamepad));
    }
    if (padCount == 0) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    auto btn = [&](ImGuiKey key, SDL_GameControllerButton b) {
        bool down = false;
        for (int p = 0; p < padCount; p++) down |= SDL_GameControllerGetButton(pads[p], b) != 0;
        io.AddKeyEvent(key, down);
    };
    auto axis = [&](ImGuiKey key, SDL_GameControllerAxis a, int lo, int hi) {
        float best = 0.0f;
        for (int p = 0; p < padCount; p++) {
            float v = (float)(SDL_GameControllerGetAxis(pads[p], a) - lo) / (float)(hi - lo);
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            if (v > best) best = v;
        }
        io.AddKeyAnalogEvent(key, best > 0.1f, best);
    };
    const int dz = 8000; // stick dead zone (SDL's suggested value)
    btn(ImGuiKey_GamepadDpadLeft,  SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    btn(ImGuiKey_GamepadDpadRight, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    btn(ImGuiKey_GamepadDpadUp,    SDL_CONTROLLER_BUTTON_DPAD_UP);
    btn(ImGuiKey_GamepadDpadDown,  SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    btn(ImGuiKey_GamepadFaceDown,  SDL_CONTROLLER_BUTTON_A); // activate / select
    btn(ImGuiKey_GamepadFaceRight, SDL_CONTROLLER_BUTTON_B); // cancel / back
    btn(ImGuiKey_GamepadFaceLeft,  SDL_CONTROLLER_BUTTON_X);
    btn(ImGuiKey_GamepadFaceUp,    SDL_CONTROLLER_BUTTON_Y);
    btn(ImGuiKey_GamepadL1,        SDL_CONTROLLER_BUTTON_LEFTSHOULDER);  // prev sidebar section
    btn(ImGuiKey_GamepadR1,        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER); // next sidebar section
    axis(ImGuiKey_GamepadLStickLeft,  SDL_CONTROLLER_AXIS_LEFTX, -dz, -32768);
    axis(ImGuiKey_GamepadLStickRight, SDL_CONTROLLER_AXIS_LEFTX, +dz, +32767);
    axis(ImGuiKey_GamepadLStickUp,    SDL_CONTROLLER_AXIS_LEFTY, -dz, -32768);
    axis(ImGuiKey_GamepadLStickDown,  SDL_CONTROLLER_AXIS_LEFTY, +dz, +32767);
}

// Compute the mouse position ImGui should use, INDEPENDENT of OS window focus/hover, so the software
// cursor (io.MouseDrawCursor) always renders on the VR menu panel. ImGui's SDL2 backend only updates
// io.MousePos while the window is focused (global fallback) or hovered (SDL_MOUSEMOTION); in VR the
// headset compositor usually holds focus, so the moment the OS cursor drifts off the unfocused game
// window the backend pushes a mouse-leave (MousePos = -FLT_MAX) and ImGui stops drawing the cursor - the
// "mouse sometimes fails to render" bug. Read the global mouse and clamp it into the window rect so the
// cursor pins to the panel edge instead of vanishing. Coordinate space matters: with multi-viewports
// enabled (libultraship's default on Windows) ImGui expects ABSOLUTE desktop coordinates; without, it
// expects window-local - the SDL2 backend branches exactly the same way.
static bool VrComputeImGuiMousePos(float* outX, float* outY) {
    SDL_Window* win = SDL_GL_GetCurrentWindow();
    if (win == nullptr) {
        return false;
    }
    int gx = 0, gy = 0, wx = 0, wy = 0, ww = 0, wh = 0;
    SDL_GetGlobalMouseState(&gx, &gy);
    SDL_GetWindowPosition(win, &wx, &wy);
    SDL_GetWindowSize(win, &ww, &wh);
    int cx = gx, cy = gy; // clamp into the window rect, in global coords
    if (cx < wx) cx = wx; else if (ww > 0 && cx > wx + ww - 1) cx = wx + ww - 1;
    if (cy < wy) cy = wy; else if (wh > 0 && cy > wy + wh - 1) cy = wy + wh - 1;
    const bool multiViewport = (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0;
    *outX = (float)(multiViewport ? cx : cx - wx);
    *outY = (float)(multiViewport ? cy : cy - wy);
    return true;
}
#endif

void GameEngine::RunCommands(Gfx* pool, const std::vector<std::unordered_map<Mtx*, MtxF>>& mtx_replacements) {
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());

    if (wnd == nullptr) {
        return;
    }

    auto interpreter = wnd->GetInterpreterWeak().lock().get();

    // Process window events for resize, mouse, keyboard events
    wnd->HandleEvents();

    interpreter->mInterpolationIndex = 0;

#if defined(ENABLE_VR) && defined(_WIN32)
    const bool vrActive = vr_is_requested() && vr_is_active();
    if (vr_is_requested() && !vrActive) {
        // Booting: advance the OpenXR session and close any frame it begins (safe no-op otherwise) so
        // the active loop starts clean next frame; then fall through to the flat render so the desktop
        // shows the game while VR spins up.
        vr_begin_frame();
        vr_submit();
    }
    if (vrActive) {
        // The GUI cycle runs once so input + the controller device handler stay alive. When the ImGui
        // menu is open, present the desktop (with the menu) on the head-locked panel so it's usable in
        // the headset; otherwise render the stereo / panel frame(s).
        auto gui = wnd->GetGui();
        const bool menuOpen = gui->GetMenuOrMenubarVisible();
        // VR menu input: the mouse cursor can't be aimed in the headset, so the menu is driven with the
        // gamepad. Force ImGui gamepad navigation ON before StartDraw/NewFrame (so ImGui reads the pad
        // for nav THIS frame, instead of relying on menu-state callbacks that may not fire in this
        // path), and block game input while the menu is open so the stick drives the menu, not the kart.
        static const int32_t kVrMenuInputBlockId = 0x5652484E; // 'VRHN'
        static bool sVrMenuInputBlocked = false;
        float vrMouseX = 0.0f, vrMouseY = 0.0f;
        bool vrMouseValid = false;
        if (menuOpen) {
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
            // The OS cursor isn't visible inside the headset, so have ImGui draw its own software cursor
            // into the menu texture -> it shows on the panel and tracks the mouse (mouse becomes usable in VR).
            ImGui::GetIO().MouseDrawCursor = true;
            // Focus-independent pad feed (the real fix): refresh + push nav keys before StartDraw's
            // ImGui::NewFrame consumes them.
            VrFeedImGuiGamepadNav();
            // Focus-independent mouse feed: keep io.MousePos valid even when the OS cursor is off the
            // game window, so the software cursor never vanishes from the panel. Queued here so NewFrame
            // computes mouse deltas from it; ALSO written directly after StartDraw (below) because when
            // the window happens to keep OS focus, the backend queues its own UNCLAMPED position after
            // this event and would win the frame (and each window-leave queues a -FLT_MAX invalidation).
            vrMouseValid = VrComputeImGuiMousePos(&vrMouseX, &vrMouseY);
            if (vrMouseValid) {
                ImGui::GetIO().AddMousePosEvent(vrMouseX, vrMouseY);
            }
            if (!sVrMenuInputBlocked) {
                // On menu open, force ImGui's SDL2 backend to (re)scan + open ALL controllers and MERGE
                // their input (AutoAll). The diagnostic showed 2 joysticks present and AutoFirst was reading
                // the wrong one (a phantom/virtual device, not the pad in hand) - so nav got no input even
                // though HasGamepad was set. AutoAll ORs every device, so the real pad always drives nav.
                ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_AutoAll);
                Ship::Context::GetInstance()->GetControlDeck()->BlockGameInput(kVrMenuInputBlockId);
                sVrMenuInputBlocked = true;
            }
        } else if (sVrMenuInputBlocked) {
            ImGui::GetIO().MouseDrawCursor = false;
            Ship::Context::GetInstance()->GetControlDeck()->UnblockGameInput(kVrMenuInputBlockId);
            sVrMenuInputBlocked = false;
        }
        wnd->GetMouseStateManager()->StartFrame();
        gui->StartDraw();
        // Post-NewFrame override: ImGui::NewFrame (inside StartDraw) resolves the event queue with
        // last-write-wins, and the SDL2 backend's own events (unclamped global pos when focused, or the
        // window-leave -FLT_MAX) are queued AFTER our pre-StartDraw feed. Writing io.MousePos directly
        // here - after the queue is consumed, before any widgets are submitted in EndDraw - makes our
        // clamped position authoritative for both hit-testing and the software cursor, every frame.
        if (menuOpen && vrMouseValid) {
            ImGui::GetIO().MousePos = ImVec2(vrMouseX, vrMouseY);
        }
        interpreter->StartFrame();
        if (menuOpen) {
            uint32_t winW = 0, winH = 0; int32_t winX = 0, winY = 0;
            interpreter->GetDimensions(&winW, &winH, &winX, &winY);
            // Render the menu ONCE into its stable offscreen FBO.
            vr_menu_render_begin((int)winW, (int)winH);   // bind + clear the private FBO
            gui->EndDraw();                               // render ImGui INTO that FBO
            vr_menu_apply_opacity();                      // gVRMenuOpacity -> see-through panel (no-op if opaque)
            // CRITICAL: pace the SAME number of OpenXR frames as gameplay. One vr_begin_frame per tick
            // runs RunCommands (and thus the game logic + audio) at the headset's 90Hz instead of 30Hz,
            // which sped the game ~3x and garbled the audio. Looping `steps` xrWaitFrames restores the
            // 30Hz tick. The menu panel (already rendered) is just re-presented each paced frame.
            const size_t mSteps = mtx_replacements.empty() ? 1 : mtx_replacements.size();
            static const std::unordered_map<Mtx*, MtxF> kMenuEmptyMtx;
            // Live stereo world behind the menu - but not in Theater mode (it has no per-eye render).
            const bool showGameBehindMenu = (gGamestate == 4 /* RACING */) && (vr_get_view_mode() != VR_VIEW_THEATER);
            for (size_t s = 0; s < mSteps; s++) {
                const auto& mtx = mtx_replacements.empty() ? kMenuEmptyMtx : mtx_replacements[s];
                vr_begin_frame();
                if (showGameBehindMenu) {
                    // Render the game eyes so the world shows behind the menu (no black-out). Safe now: the
                    // loop is paced (mSteps xrWaitFrames) so the game tick stays 30Hz - no audio speed-up.
                    if (gVrSkyDomeFrame != (unsigned int)gGlobalTimer) build_sky_dome_vr();
                    const int eyes = vr_eye_count();
                    for (int e = 0; e < eyes; e++) {
                        interpreter->RunVrEye(pool, mtx, vr_eye_viewproj(e), vr_sky_viewproj(e),
                                              vr_hud_viewproj(e), gVrSkyDomeGfx,
                                              vr_eye_width(e), vr_eye_height(e));
                        vr_submit_eye_texture(e, interpreter->GetVrFbTextureId(), vr_eye_width(e), vr_eye_height(e));
                    }
                }
                vr_menu_render_present((int)winW, (int)winH); // present the menu texture on the panel (over the game)
                vr_submit();
                interpreter->mInterpolationIndex++;
            }
            vr_menu_mirror_desktop((int)winW, (int)winH); // mirror to the flatscreen ONCE (no per-step flicker)
        } else {
            // Render ONE OpenXR frame per frame-interpolation step. Each step re-locates the head pose
            // (vr_begin_frame -> xrWaitFrame), paced by the headset, so the HMD runs at its native
            // refresh with smooth head tracking while game logic stays ~30Hz.
            static const std::unordered_map<Mtx*, MtxF> kEmptyMtx;
            const size_t steps = mtx_replacements.empty() ? 1 : mtx_replacements.size();
            // Render stereo whenever racing - INCLUDING while paused - so the in-game VR menu shows over the
            // live stereo world and VR setting changes (view mode, stereo, etc.) apply instantly. (When paused
            // the game logic is frozen but the world still renders.)
            const bool gameplay = (gGamestate == 4 /* RACING */);
            // Stereo per-eye render for the in-world view modes (Third Person / First Person / Diorama).
            // Theater (and all non-gameplay screens) render the flat frame once onto the head-locked panel.
            const bool stereo = gameplay && (vr_get_view_mode() != VR_VIEW_THEATER);
            for (size_t s = 0; s < steps; s++) {
                const auto& mtx = mtx_replacements.empty() ? kEmptyMtx : mtx_replacements[s];
                vr_begin_frame();
                if (stereo) {
                    if (gVrSkyDomeFrame != (unsigned int)gGlobalTimer) build_sky_dome_vr();
                    const int eyes = vr_eye_count();
                    for (int e = 0; e < eyes; e++) {
                        interpreter->RunVrEye(pool, mtx, vr_eye_viewproj(e), vr_sky_viewproj(e),
                                              vr_hud_viewproj(e), gVrSkyDomeGfx,
                                              vr_eye_width(e), vr_eye_height(e));
                        vr_submit_eye_texture(e, interpreter->GetVrFbTextureId(), vr_eye_width(e), vr_eye_height(e));
                    }
                } else {
                    vr_set_panel_mode(true);
                    interpreter->RunVrPanel(pool, mtx, vr_overlay_width(), vr_overlay_height());
                    vr_submit_panel_texture(interpreter->GetVrFbTextureId(), vr_overlay_width(), vr_overlay_height());
                }
                vr_submit();
                interpreter->mInterpolationIndex++;
            }
            gui->EndDraw();
            // Menu closed: mirror the rendered VR frame onto the flat window as the LAST fb0 write before the
            // swap, so the desktop shows the game instead of flickering between stale back-buffers (the game
            // renders into the OpenXR swapchains + a stale mGameFb, never the live fb0). AFTER gui->EndDraw()
            // so the GUI composite can't overwrite it; BEFORE EndFrame()'s SwapBuffers. Mirrors the managed fb
            // (last eye in gameplay, panel otherwise) - same last-write-wins discipline as the menu mirror.
            {
                uint32_t mW = 0, mH = 0; int32_t mX = 0, mY = 0;
                interpreter->GetDimensions(&mW, &mH, &mX, &mY);
                const int sw = stereo ? vr_eye_width(0)  : vr_overlay_width();
                const int sh = stereo ? vr_eye_height(0) : vr_overlay_height();
                vr_mirror_game_desktop(interpreter->GetVrFbTextureId(), sw, sh, (int)mW, (int)mH);
            }
        }
        interpreter->EndFrame(); // present/swap the desktop window
    } else
#endif
    {
        for (const auto& mtxStack : mtx_replacements) {
            wnd->DrawAndRunGraphicsCommands(pool, mtxStack);
            interpreter->mInterpolationIndex++;
        }
    }

    bool curAltAssets = CVarGetInteger("gEnhancements.Mods.AlternateAssets", 0);
    if (prevAltAssets != curAltAssets) {
        prevAltAssets = curAltAssets;
        Ship::Context::GetInstance()->GetResourceManager()->SetAltAssetsEnabled(curAltAssets);
        gfx_texture_cache_clear();
    }
}

/**
 * During the draw phase, the gfx pool is filled with graphics commands.
 * At the end of the game loop, these commands are sent into lus and interpreted
 * or translated into modern graphics commands
 */
void GameEngine::ProcessGfxCommands(Gfx* pool) {
    std::vector<std::unordered_map<Mtx*, MtxF>> mtx_replacements;
    int target_fps = GameEngine::Instance->GetInterpolationFPS();
    if (CVarGetInteger("gModifyInterpolationTargetFPS", 0)) {
        target_fps = CVarGetInteger("gInterpolationTargetFPS", 60);
    }
    static int last_fps;
    static int last_update_rate;
    static int time;
    int fps = target_fps;
    int original_fps = 60 / 2 /*gVIsPerFrame*/;

    if (target_fps == 30 || original_fps > target_fps) {
        fps = original_fps;
    }

    if (last_fps != fps || last_update_rate != 2 /*gVIsPerFrame*/) {
        time = 0;
    }

    // time_base = fps * original_fps (one second)
    int next_original_frame = fps;

    // Get matrix replacements for intermediate frames
    while (time + original_fps <= next_original_frame) {
        time += original_fps;
        if (time != next_original_frame) {
            mtx_replacements.push_back(FrameInterpolation_Interpolate((float) time / next_original_frame));
        } else {
            mtx_replacements.emplace_back(); // No interpolation for key frames
        }
    }
    // printf("mtxf size: %d\n", mtx_replacements.size());

    time -= fps;

    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetInstance()->GetWindow());
    if (wnd != nullptr) {
        wnd->SetTargetFps(GetInterpolationFPS());
        wnd->SetMaximumFrameLatency(1);
    }
    RunCommands(pool, mtx_replacements);

    last_fps = fps;
    last_update_rate = 2;
}

// Audio
void GameEngine::HandleAudioThread() {
    while (audio.running) {
        {
            std::unique_lock<std::mutex> Lock(audio.mutex);
            while (!audio.processing && audio.running) {
                audio.cv_to_thread.wait(Lock);
            }

            if (!audio.running) {
                break;
            }
        }
        std::unique_lock<std::mutex> Lock(audio.mutex);

        int samples_left = AudioPlayerBuffered();
        u32 num_audio_samples = samples_left < AudioPlayerGetDesiredBuffered() ? SAMPLES_HIGH : SAMPLES_LOW;

        s16 nas_buffer[SAMPLES_PER_FRAME] = { 0 };
        f32 hmas_buffer[SAMPLES_PER_FRAME] = { 0 };
        s16 mix_buffer[SAMPLES_PER_FRAME] = { 0 };

        for (size_t i = 0; i < NUM_AUDIO_CHANNELS; i++) {
            create_next_audio_buffer(nas_buffer + i * (num_audio_samples * 2), num_audio_samples);
        }

        GameEngine::Instance->gHMAS->CreateBuffer((u8*)hmas_buffer, 4 * num_audio_samples * sizeof(float));

        float master_vol = CVarGetFloat("gGameMasterVolume", 1.0f);

        for (size_t i = 0; i < SAMPLES_PER_FRAME; i++) {
            mix_buffer[i] = nas_buffer[i] + ((int16_t)(hmas_buffer[i] * 32767.0f) * master_vol);
        }

        AudioPlayerPlayFrame((u8*) mix_buffer, 2 * num_audio_samples * 4);

        audio.processing = false;
        audio.cv_from_thread.notify_one();
    }
}

void GameEngine::StartAudioFrame() {
    {
        std::unique_lock<std::mutex> Lock(audio.mutex);
        audio.processing = true;
    }

    audio.cv_to_thread.notify_one();
}

void GameEngine::EndAudioFrame() {
    {
        std::unique_lock<std::mutex> Lock(audio.mutex);
        while (audio.processing) {
            audio.cv_from_thread.wait(Lock);
        }
    }
}

void GameEngine::AudioInit() {
    const auto resourceMgr = Ship::Context::GetInstance()->GetResourceManager();
    resourceMgr->LoadResources("sound");
    const auto banksFiles = resourceMgr->GetArchiveManager()->ListFiles("sound/banks/*");
    const auto sequences_files = resourceMgr->GetArchiveManager()->ListFiles("sound/sequences/*");

    Instance->sequenceTable.resize(512);
    Instance->audioSequenceTable.resize(512);
    Instance->banksTable.resize(512);

    for (auto& bank : *banksFiles) {
        auto path = "__OTR__" + bank;
        const auto ctl = static_cast<CtlEntry*>(ResourceGetDataByName(path.c_str()));
        this->bankMapTable[bank] = ctl->bankId;
        SPDLOG_INFO("Loaded bank: {}", bank);
    }

    for (auto& sequence : *sequences_files) {
        if (sequence.find('.') != std::string::npos) {
            continue;
        }
        auto path = "__OTR__" + sequence;
        auto seq = static_cast<AudioSequenceData*>(ResourceGetDataByName(path.c_str()));
        Instance->sequenceTable[seq->id] = path;
        SPDLOG_INFO("Loaded sequence: {}", sequence);
    }

    if (!audio.running) {
        audio.running = true;
        audio.thread = std::thread(HandleAudioThread);
        SPDLOG_INFO("Audio thread started");
    }
}

void GameEngine::AudioExit() {
    {
        std::unique_lock lock(audio.mutex);
        audio.running = false;
    }
    audio.cv_to_thread.notify_all();

    // Wait until the audio thread quit
    audio.thread.join();
}

uint8_t GameEngine::GetBankIdByName(const std::string& name) {
    if (Instance->bankMapTable.contains(name)) {
        return Instance->bankMapTable[name];
    }
    return 0;
}

ImFont* GameEngine::CreateFontWithSize(float size, std::string fontPath) {
    auto mImGuiIo = &ImGui::GetIO();
    ImFont* font;
    if (fontPath == "") {
        ImFontConfig fontCfg = ImFontConfig();
        fontCfg.OversampleH = fontCfg.OversampleV = 1;
        fontCfg.PixelSnapH = true;
        fontCfg.SizePixels = size;
        font = mImGuiIo->Fonts->AddFontDefault(&fontCfg);
    } else {
        auto initData = std::make_shared<Ship::ResourceInitData>();
        initData->Format = RESOURCE_FORMAT_BINARY;
        initData->Type = static_cast<uint32_t>(RESOURCE_TYPE_FONT);
        initData->ResourceVersion = 0;
        initData->Path = fontPath;
        std::shared_ptr<Ship::Font> fontData = std::static_pointer_cast<Ship::Font>(
            Ship::Context::GetInstance()->GetResourceManager()->LoadResource(fontPath, false, initData));
        char* fontDataPtr = (char*) malloc(fontData->DataSize);
        memcpy(fontDataPtr, fontData->Data, fontData->DataSize);
        font = mImGuiIo->Fonts->AddFontFromMemoryTTF(fontDataPtr, fontData->DataSize, size);
    }
    // FontAwesome fonts need to have their sizes reduced by 2.0f/3.0f in order to align correctly
    float iconFontSize = size * 2.0f / 3.0f;
    static const ImWchar sIconsRanges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
    ImFontConfig iconsConfig;
    iconsConfig.MergeMode = true;
    iconsConfig.PixelSnapH = true;
    iconsConfig.GlyphMinAdvanceX = iconFontSize;
    mImGuiIo->Fonts->AddFontFromMemoryCompressedBase85TTF(fontawesome_compressed_data_base85, iconFontSize,
                                                          &iconsConfig, sIconsRanges);
    return font;
}

// End

extern "C" uint32_t GameEngine_GetSampleRate() {
    auto player = Ship::Context::GetInstance()->GetAudio()->GetAudioPlayer();
    if (player == nullptr) {
        return 0;
    }

    if (!player->IsInitialized()) {
        return 0;
    }

    return player->GetSampleRate();
}

extern "C" uint32_t GameEngine_GetSamplesPerFrame() {
    return SAMPLES_PER_FRAME;
}

extern "C" CtlEntry* GameEngine_LoadBank(const uint8_t bankId) {
    const auto engine = GameEngine::Instance;

    if (bankId >= engine->bankMapTable.size()) {
        return nullptr;
    }

    if (engine->banksTable[bankId] != nullptr) {
        return engine->banksTable[bankId];
    }

    for (auto& bank : engine->bankMapTable) {
        if (bank.second == bankId) {
            const auto ctl = static_cast<CtlEntry*>(ResourceGetDataByName(("__OTR__" + bank.first).c_str()));
            engine->banksTable[bankId] = ctl;
            return ctl;
        }
    }
    return nullptr;
}

extern "C" uint8_t GameEngine_IsBankLoaded(const uint8_t bankId) {
    const auto engine = GameEngine::Instance;
    GameEngine_LoadBank(bankId);
    return engine->banksTable[bankId] != nullptr;
}

extern "C" void GameEngine_UnloadBank(const uint8_t bankId) {
    const auto engine = GameEngine::Instance;
    engine->banksTable[bankId] = nullptr;
}

extern "C" AudioSequenceData* GameEngine_LoadSequence(const uint8_t seqId) {
    auto engine = GameEngine::Instance;

    if (engine->sequenceTable[seqId].empty()) {
        return nullptr;
    }

    if (engine->audioSequenceTable[seqId] != nullptr) {
        return engine->audioSequenceTable[seqId];
    }

    auto sequences = static_cast<AudioSequenceData*>(ResourceGetDataByName(engine->sequenceTable[seqId].c_str()));
    engine->audioSequenceTable[seqId] = sequences;
    return sequences;
}

extern "C" uint32_t GameEngine_GetSequenceCount() {
    auto engine = GameEngine::Instance;
    return engine->sequenceTable.size();
}

extern "C" uint8_t GameEngine_IsSequenceLoaded(const uint8_t seqId) {
    return GameEngine_LoadSequence(seqId) != nullptr;
}

extern "C" void GameEngine_UnloadSequence(const uint8_t seqId) {
    const auto engine = GameEngine::Instance;
    engine->audioSequenceTable[seqId] = nullptr;
}

extern "C" float GameEngine_GetAspectRatio() {
    auto gfx_current_dimensions = GetInterpreter()->mCurDimensions;
    return gfx_current_dimensions.aspect_ratio;
}

extern "C" uint32_t GameEngine_GetGameVersion() {
    return 0x00000001;
}

static const char* const sOtrSignature = "__OTR__";

extern "C" bool GameEngine_OTRSigCheck(const char* data) {
    return strncmp(data, sOtrSignature, strlen(sOtrSignature)) == 0;
}

extern "C" int32_t GameEngine_ResourceGetTexTypeByName(const char* name) {
    const auto res = std::static_pointer_cast<Fast::Texture>(ResourceLoad(name));

    if (res != nullptr) {
        return (int16_t) res->Type;
    }

    SPDLOG_ERROR("Given texture path {} is a non-existent resource", name);
    return -1;
}

// struct TimedEntry {
//     uint64_t duration;
//     TimerAction action;
//     int32_t* address;
//     int32_t value;
//     bool active;
// };

// std::vector<TimedEntry> gTimerTasks;

// uint64_t Timer_GetCurrentMillis() {
//     return SDL_GetTicks();
// }

// extern "C" s32 Timer_CreateTask(u64 time, TimerAction action, s32* address, s32 value) {
//     const auto millis = Timer_GetCurrentMillis();
//     TimedEntry entry = {
//         .duration = millis + CYCLES_TO_MSEC_PC(time),
//         .action = action,
//         .address = address,
//         .value = value,
//         .active = true,
//     };

//     gTimerTasks.push_back(entry);

//     return gTimerTasks.size() - 1;
// }

extern "C" void Timer_Increment(int32_t* address, int32_t value) {
    *address += value;
}

extern "C" void Timer_SetValue(int32_t* address, int32_t value) {
    *address = value;
}

extern "C" float OTRGetAspectRatio() {
    // In VR the frame is rendered to a fixed-aspect eye/panel target (1920x1080 panel in Theater),
    // NOT the desktop window. 2D widescreen sizing - the sky's right-edge extension
    // (OTRGetDimensionFromRightEdge) and HUD edge anchoring - must match that target's aspect, or
    // the game (which builds its display list while mCurDimensions still holds the window size)
    // sizes the 2D sky to the window. Rendered onto the 16:9 panel that leaves black gaps whenever
    // the window isn't 16:9 (e.g. not maximized). Use the VR target's aspect so it always fills.
    if (vr_is_active()) {
        return (float) vr_overlay_width() / (float) vr_overlay_height();
    }
    return GetInterpreter()->mCurDimensions.aspect_ratio;
}

extern "C" float OTRGetDimensionFromLeftEdge(float v) {
    return (SCREEN_WIDTH / 2 - SCREEN_HEIGHT / 2 * OTRGetAspectRatio() + (v));
}

extern "C" int16_t OTRGetRectDimensionFromLeftEdge(float v) {
    return ((int) floorf(OTRGetDimensionFromLeftEdge(v)));
}

extern "C" float OTRGetDimensionFromRightEdge(float v) {
    return (SCREEN_WIDTH / 2 + SCREEN_HEIGHT / 2 * OTRGetAspectRatio() - (SCREEN_WIDTH - v));
}

extern "C" int16_t OTRGetRectDimensionFromRightEdge(float v) {
    return ((int) ceilf(OTRGetDimensionFromRightEdge(v)));
}

/**
 * Centers an item in a given area.
 *
 * Adds the number of extended screen pixels to the location to center.
 * This allows stretching the game window really wide, and the item will stay in-place.
 *
 * This is not for centering in the direct center of the screen.
 *
 * How to use:
 *
 * s32 center = OTRCalculateCenterOfAreaFromRightEdge((SCREEN_WIDTH / 4) + (SCREEN_WIDTH / 2));
 * x = center - (texWidth / 2)
 * x2 = center + (texWidth / 2)
 */
extern "C" uint32_t OTRCalculateCenterOfAreaFromRightEdge(int32_t center) {
    return ((OTRGetDimensionFromRightEdge(SCREEN_WIDTH) - SCREEN_WIDTH) / 2) + center;
}

extern "C" uint32_t OTRCalculateCenterOfAreaFromLeftEdge(int32_t center) {
    return ((OTRGetDimensionFromLeftEdge(0) - SCREEN_WIDTH) / 2) + center;
}

// Gets the width of the current render target area
extern "C" uint32_t OTRGetGameRenderWidth() {
    return GetInterpreter()->mCurDimensions.width;
}

// Gets the height of the current render target area
extern "C" uint32_t OTRGetGameRenderHeight() {
    return GetInterpreter()->mCurDimensions.height;
}

extern "C" uint32_t OTRGetGameViewportWidth() {
    return GetInterpreter()->mGameWindowViewport.width;
}

extern "C" uint32_t OTRGetGameViewportHeight() {
    return GetInterpreter()->mGameWindowViewport.height;
}
