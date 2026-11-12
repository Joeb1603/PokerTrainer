// poker_trainer: the Dear ImGui desktop front end.
//
//   poker_trainer [--seed N] [--log-dir DIR] [--random-personalities on|off] [--settings PATH]
//                 [--mode freezeout|tournament] [--big-blind N] [--stack-bb N]
//                 [--level-hands N | --level-minutes N] [--preset standard|turbo|hyper]
//                 [--antes none|player|bb] [--skip-setup | --show-setup]
//
// The New Game screen opens first, filled in from the settings file
// (poker_trainer_settings.json, beside logs/, unless --settings names another);
// Start saves what the screen shows there. The game flags and
// --random-personalities fill in the screen, or with --skip-setup start that game;
// on their own they never save anything. In a game, the settings change only when
// the player changes one: the game on the New Game screen, or the checkbox.
//
// Developer checks, for running it unattended. --skip-setup, --exit-after-frames,
// --auto-shove, --stop-on and --screenshot skip the New Game screen, unless
// --show-setup is given too; the others leave it at launch.
//   --show-setup            Open the New Game screen even with the flags that skip it
//   --exit-after-frames N   Quit after N frames
//   --auto-shove            Hero moves all-in at every decision, with bots instant and
//                           hands and sessions advancing by themselves
//   --stop-on WHAT          With --auto-shove, hold the screen at: live-side-pot (Hero
//                           to act with side pots), side-pot (showdown with side
//                           pots), showdown, flop (Hero to act with a board), bust,
//                           or folded (Hero folds a hand that goes on; bots then pause)
//   --reveal                With --auto-shove, reveal the bots' hands at the held screen
//   --bot-delay MS          Start with this bot delay, to hold a "waiting" screen
//   --advice TYPE           Start the "Advice:" selector on baseline, nit, station or maniac
//   --hover X,Y             Hold the ImGui mouse at window point X,Y, to show a tooltip
//   --size WxH              Open the window at this size (unscaled), e.g. the 1180x760 minimum
//   --screenshot PATH       Save the last frame as a PNG (with --exit-after-frames, or
//                           when the window is closed)
//   --solver                Start with solver mode on
//   --solver-budget MS      Each solve's time budget (default 8000)
//   --solver-tier TIER      Use off, river, turn or flop instead of the detected tier

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "app/app_settings.h"
#include "app/screenshot.h"
#include "app/setup_panel.h"
#include "app/trainer_ui.h"
#include "controller/local_game_controller.h"

namespace {

void report_fatal(const std::string& message) {
#ifdef _WIN32
    MessageBoxA(nullptr, message.c_str(), "Poker Trainer", MB_OK | MB_ICONERROR);  // No console in this build
#else
    std::fprintf(stderr, "%s\n", message.c_str());
#endif
}

std::optional<std::filesystem::path> first_existing(std::initializer_list<const char*> candidates) {
    for (const char* candidate : candidates) {
        std::error_code ignored;
        if (std::filesystem::exists(candidate, ignored)) return candidate;
    }
    return std::nullopt;
}

poker::UiFonts load_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    const float size = 16.0f * scale;
    poker::UiFonts fonts;

    if (const auto path = first_existing({"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                          "/System/Library/Fonts/Supplemental/Arial.ttf"})) {
        fonts.regular = io.Fonts->AddFontFromFileTTF(path->string().c_str(), size);
    }
    if (const auto path = first_existing({"C:/Windows/Fonts/segoeuib.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                          "/System/Library/Fonts/Supplemental/Arial Bold.ttf"})) {
        fonts.bold = io.Fonts->AddFontFromFileTTF(path->string().c_str(), size);
    }
    if (!fonts.regular) {
        ImFontConfig config;
        config.SizePixels = size;
        fonts.regular = io.Fonts->AddFontDefault(&config);
    }
    if (!fonts.bold) fonts.bold = fonts.regular;
    return fonts;
}

void apply_style(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.ChildRounding = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.ItemSpacing = {8.0f, 7.0f};
    style.FramePadding = {8.0f, 5.0f};
    style.Colors[ImGuiCol_WindowBg] = {0.09f, 0.10f, 0.13f, 1.0f};
    style.ScaleAllSizes(scale);
}

struct Arguments {
    std::uint64_t seed = 0;
    bool seeded = false;
    std::filesystem::path log_dir = "logs";
    std::optional<long> exit_after_frames;
    bool auto_shove = false;
    bool reveal = false;
    std::optional<double> bot_delay_ms;
    std::string stop_on;
    poker::Archetype advice = poker::Archetype::Baseline;
    std::optional<std::pair<double, double>> hover;
    std::optional<std::filesystem::path> screenshot;
    int width = 1360;
    int height = 860;
    bool solver = false;
    std::optional<int> solver_budget_ms;
    std::optional<poker::SolverTier> solver_tier;
    std::optional<bool> random_personalities;
    std::filesystem::path settings = poker::kDefaultSettingsFile;
    // The game flags; each overrides the saved setup for this run.
    std::optional<poker::GameMode> mode;
    std::optional<int> big_blind;
    std::optional<int> stack_bb;
    std::optional<std::pair<poker::LevelUnit, int>> level;
    std::optional<poker::BlindPreset> preset;
    std::optional<poker::AnteType> antes;
    bool skip_setup = false;
    bool show_setup = false;
};

// The saved setup with the game flags applied. A flag that shapes the levels
// rebuilds them from the preset.
poker::GameSetup setup_from_flags(poker::GameSetup setup, const Arguments& arguments) {
    if (arguments.mode) setup.mode = *arguments.mode;
    if (arguments.big_blind) setup.big_blind = *arguments.big_blind;
    if (arguments.stack_bb) setup.starting_bb = *arguments.stack_bb;
    if (arguments.level) {
        setup.unit = arguments.level->first;
        setup.level_length = arguments.level->second;
    }
    if (arguments.preset) setup.preset = *arguments.preset;
    if (arguments.antes) setup.ante_type = *arguments.antes;
    if (arguments.big_blind || arguments.preset || arguments.antes) setup.levels.clear();
    return setup;
}

bool shows_setup_at_launch(const Arguments& arguments) {
    if (arguments.show_setup) return true;
    return !arguments.skip_setup && !arguments.exit_after_frames && !arguments.auto_shove && arguments.stop_on.empty() &&
           !arguments.screenshot;
}

std::optional<poker::SolverTier> parse_tier(std::string_view text) {
    if (text == "off") return poker::SolverTier::Unavailable;
    if (text == "river") return poker::SolverTier::River;
    if (text == "turn") return poker::SolverTier::Turn;
    if (text == "flop") return poker::SolverTier::Flop;
    return std::nullopt;
}

std::optional<poker::Archetype> parse_archetype(std::string_view text) {
    for (int i = 0; i < poker::kArchetypeCount; ++i) {
        const auto type = static_cast<poker::Archetype>(i);
        std::string name(poker::to_string(type));
        for (char& c : name) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (name == text) return type;
    }
    return std::nullopt;
}

// "A,B" or "AxB": two decimal numbers around `separator` and nothing else. Parsed
// with strtod rather than sscanf, which MSVC deprecates (C4996).
std::optional<std::pair<double, double>> parse_pair(const char* text, char separator) {
    const auto number = [](const char*& at) -> std::optional<double> {
        if (*at == '\0' || *at == ' ' || (at[0] == '0' && (at[1] == 'x' || at[1] == 'X'))) return std::nullopt;
        char* end = nullptr;
        const double value = std::strtod(at, &end);
        if (end == at || !std::isfinite(value)) return std::nullopt;
        at = end;
        return value;
    };
    const char* at = text;
    const auto first = number(at);
    if (!first || *at != separator) return std::nullopt;
    ++at;
    const auto second = number(at);
    if (!second || *at != '\0') return std::nullopt;
    return std::pair{*first, *second};
}

// Reads the back buffer before it is swapped, flipped to top-down rows.
void save_screenshot(const std::filesystem::path& path, int width, int height) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    const std::size_t row = static_cast<std::size_t>(width) * 3;
    std::vector<std::uint8_t> flipped(pixels.size());
    for (std::size_t y = 0; y < static_cast<std::size_t>(height); ++y) {
        std::copy_n(pixels.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(height) - 1 - y) * row), row,
                    flipped.begin() + static_cast<std::ptrdiff_t>(y * row));
    }
    const std::string error = poker::write_png_rgb(path, width, height, flipped);
    if (!error.empty()) std::fprintf(stderr, "Screenshot failed: %s\n", error.c_str());
}

// Developer autoplay, for reaching rare screens unattended. Returns nothing to do once
// the requested screen is up, so it stays there.
void auto_play(poker::IGameController& controller, const Arguments& arguments) {
    using poker::ControllerPhase;
    const poker::TableSnapshot& s = controller.snapshot();
    const bool side_pots = s.pots.pots.size() > 1;

    switch (s.phase) {
        case ControllerPhase::BotToAct:
            break;
        case ControllerPhase::HeroToAct: {
            if (arguments.stop_on == "folded" && s.legal.allows(poker::ActionType::Fold)) {
                // Pause the bots once Hero is out, so the rest of the hand holds still.
                (void)controller.submit_hero_action({poker::ActionType::Fold});
                if (controller.snapshot().phase == ControllerPhase::BotToAct) {
                    controller.set_instant_play(false);
                    controller.set_bot_delay_ms(1e12);
                }
                return;
            }
            if (arguments.stop_on == "live-side-pot" && side_pots) return;
            if (arguments.stop_on == "flop" && !s.board.empty()) return;
            // Calling keeps Hero in the hand with chips behind while bots go all-in.
            const bool shove = arguments.stop_on != "live-side-pot" && arguments.stop_on != "flop" &&
                               s.legal.allows(poker::ActionType::AllIn);
            const poker::ActionType fallback =
                s.legal.allows(poker::ActionType::Check) ? poker::ActionType::Check : poker::ActionType::Call;
            (void)controller.submit_hero_action({shove ? poker::ActionType::AllIn : fallback});
            break;
        }
        case ControllerPhase::HandComplete:
            if ((arguments.stop_on == "showdown" && s.showdown) || (arguments.stop_on == "side-pot" && s.showdown && side_pots)) {
                if (arguments.reveal) (void)controller.reveal_bot_hands();
                return;
            }
            (void)controller.next_hand();
            break;
        case ControllerPhase::SessionOver:
            if (arguments.stop_on == "bust" || (arguments.stop_on == "showdown" && s.showdown) ||
                (arguments.stop_on == "side-pot" && s.showdown && side_pots)) {
                if (arguments.reveal) (void)controller.reveal_bot_hands();
                return;
            }
            controller.new_session();
            break;
    }
}

std::optional<Arguments> parse_arguments(int argc, char** argv) {
    Arguments arguments;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--seed" && i + 1 < argc) {
            arguments.seed = std::strtoull(argv[++i], nullptr, 10);
            arguments.seeded = true;
        } else if (arg == "--log-dir" && i + 1 < argc) {
            arguments.log_dir = argv[++i];
        } else if (arg == "--exit-after-frames" && i + 1 < argc) {
            arguments.exit_after_frames = std::strtol(argv[++i], nullptr, 10);
        } else if (arg == "--auto-shove") {
            arguments.auto_shove = true;
        } else if (arg == "--reveal") {
            arguments.reveal = true;
        } else if (arg == "--bot-delay" && i + 1 < argc) {
            arguments.bot_delay_ms = std::strtod(argv[++i], nullptr);
        } else if (arg == "--stop-on" && i + 1 < argc) {
            arguments.stop_on = argv[++i];
        } else if (arg == "--advice" && i + 1 < argc) {
            const auto type = parse_archetype(argv[++i]);
            if (!type) return std::nullopt;
            arguments.advice = *type;
        } else if (arg == "--hover" && i + 1 < argc) {
            arguments.hover = parse_pair(argv[++i], ',');
            if (!arguments.hover) return std::nullopt;
        } else if (arg == "--size" && i + 1 < argc) {
            const auto size = parse_pair(argv[++i], 'x');
            if (!size || size->first < 1180.0 || size->second < 760.0 || size->first > 16384.0 || size->second > 16384.0 ||
                size->first != std::floor(size->first) || size->second != std::floor(size->second)) {
                return std::nullopt;
            }
            arguments.width = static_cast<int>(size->first);
            arguments.height = static_cast<int>(size->second);
        } else if (arg == "--screenshot" && i + 1 < argc) {
            arguments.screenshot = argv[++i];
        } else if (arg == "--solver") {
            arguments.solver = true;
        } else if (arg == "--solver-budget" && i + 1 < argc) {
            const long budget = std::strtol(argv[++i], nullptr, 10);
            if (budget < 1 || budget > 3'600'000) return std::nullopt;
            arguments.solver_budget_ms = static_cast<int>(budget);
        } else if (arg == "--solver-tier" && i + 1 < argc) {
            arguments.solver_tier = parse_tier(argv[++i]);
            if (!arguments.solver_tier) return std::nullopt;
        } else if (arg == "--random-personalities" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            if (value != "on" && value != "off") return std::nullopt;
            arguments.random_personalities = value == "on";
        } else if (arg == "--settings" && i + 1 < argc) {
            arguments.settings = argv[++i];
        } else if (arg == "--mode" && i + 1 < argc) {
            arguments.mode = poker::parse_game_mode(argv[++i]);
            if (!arguments.mode) return std::nullopt;
        } else if ((arg == "--big-blind" || arg == "--stack-bb" || arg == "--level-hands" || arg == "--level-minutes") &&
                   i + 1 < argc) {
            char* end = nullptr;
            const long value = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value < 1 || value > 1'000'000) return std::nullopt;
            const int n = static_cast<int>(value);
            if (arg == "--big-blind") arguments.big_blind = n;
            if (arg == "--stack-bb") arguments.stack_bb = n;
            if (arg == "--level-hands") arguments.level = std::pair{poker::LevelUnit::Hands, n};
            if (arg == "--level-minutes") arguments.level = std::pair{poker::LevelUnit::Minutes, n};
        } else if (arg == "--preset" && i + 1 < argc) {
            arguments.preset = poker::parse_blind_preset(argv[++i]);
            if (!arguments.preset) return std::nullopt;
        } else if (arg == "--antes" && i + 1 < argc) {
            arguments.antes = poker::parse_ante_type(argv[++i]);
            if (!arguments.antes) return std::nullopt;
        } else if (arg == "--skip-setup") {
            arguments.skip_setup = true;
        } else if (arg == "--show-setup") {
            arguments.show_setup = true;
        } else {
            return std::nullopt;
        }
    }
    return arguments;
}

int run(const Arguments& arguments) {
    if (!glfwInit()) {
        report_fatal("Could not initialize GLFW.");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    float scale_x = 1.0f;
    float scale_y = 1.0f;
    glfwGetMonitorContentScale(glfwGetPrimaryMonitor(), &scale_x, &scale_y);
    const float scale = scale_x > 0.0f ? scale_x : 1.0f;

    GLFWwindow* window = glfwCreateWindow(static_cast<int>(static_cast<float>(arguments.width) * scale),
                                          static_cast<int>(static_cast<float>(arguments.height) * scale),
                                          "Poker Trainer", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        report_fatal("Could not create a window with OpenGL 3.0.");
        return 1;
    }
    glfwSetWindowSizeLimits(window, static_cast<int>(1180 * scale), static_cast<int>(760 * scale), GLFW_DONT_CARE,
                            GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    apply_style(scale);
    const poker::UiFonts fonts = load_fonts(scale);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    int exit_code = 0;
    long frames = 0;
    bool last_frame = false;
    // One frame: input, then `draw` between NewFrame and Render, then present.
    const auto frame = [&](const auto& draw) {
        glfwPollEvents();
        last_frame = glfwWindowShouldClose(window) != 0 ||
                     (arguments.exit_after_frames && frames + 1 >= *arguments.exit_after_frames);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        if (arguments.hover) {  // After the backend's own mouse update, so this one wins
            io.AddMousePosEvent(static_cast<float>(arguments.hover->first), static_cast<float>(arguments.hover->second));
        }
        ImGui::NewFrame();
        draw();
        ImGui::Render();

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.06f, 0.07f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (last_frame && arguments.screenshot) save_screenshot(*arguments.screenshot, width, height);
        glfwSwapBuffers(window);
        ++frames;
    };
    // Saves the fields given, leaving the file's other values exactly as they are.
    const auto save_settings = [&](const std::optional<poker::GameSetup>& setup, std::optional<bool> random) {
        if (!poker::update_app_settings(arguments.settings, poker::AppSettingsUpdate{random, setup})) {
            std::fprintf(stderr, "Could not save %s\n", arguments.settings.string().c_str());
        }
    };

    try {
        const poker::AppSettings settings = poker::load_app_settings(arguments.settings);
        poker::GameSetup setup = setup_from_flags(settings.game_setup, arguments);
        bool random = arguments.random_personalities.value_or(settings.random_personalities);
        // A bad flag value is shown on the New Game screen; without it, it stops here.
        const bool show_setup = shows_setup_at_launch(arguments);
        if (const auto problem = poker::validate(setup); problem && !show_setup) throw std::invalid_argument(*problem);

        // The New Game screen, before any hand is dealt: closing the window here quits
        // without starting a session.
        bool start = true;
        if (show_setup) {
            poker::SetupPanel panel;
            panel.open(setup, random, false);
            poker::SetupPanel::Result result = poker::SetupPanel::Result::None;
            while (!last_frame && panel.is_open()) {
                frame([&] { result = panel.draw(fonts.bold, scale); });
            }
            start = result == poker::SetupPanel::Result::Start;
            if (start) {
                setup = panel.setup();
                random = panel.random_personalities();
                save_settings(setup, random);  // What the player chose to start
            }
        }
        if (start) {
            poker::ControllerOptions options;
            options.seed = arguments.seeded ? arguments.seed : (static_cast<std::uint64_t>(std::random_device{}()) << 32) ^
                                                                   std::random_device{}();
            options.log_dir = arguments.log_dir;
            options.instant_play = arguments.auto_shove;
            if (arguments.bot_delay_ms) options.bot_delay_ms = *arguments.bot_delay_ms;
            options.solver_enabled = arguments.solver;
            if (arguments.solver_budget_ms) options.solver_budget_ms = *arguments.solver_budget_ms;
            options.solver_tier_override = arguments.solver_tier;
            options.setup = setup;
            options.random_personalities = random;
            // Each is saved only when the player changes it (Start on the New Game
            // screen, or the checkbox), so the flags alone never save.
            bool last_random = random;
            int last_started = 0;
            poker::LocalGameController controller(options);
            poker::TrainerUi ui(controller, fonts, scale);
            ui.set_advice_focus(arguments.advice);

            while (!last_frame) {
                // The New Game screen stops the game clock and the bots behind it.
                if (!ui.setup_open()) {
                    controller.tick(static_cast<double>(io.DeltaTime) * 1000.0);
                    if (arguments.auto_shove) auto_play(controller, arguments);
                }
                frame([&] { ui.draw(); });
                // Right after the frame that made the change, so a Start or a click on
                // the last frame (the window closing) is saved too.
                const bool started = ui.setups_started() != last_started;
                const bool random_changed = controller.random_personalities() != last_random;
                if (started || random_changed) {
                    last_started = ui.setups_started();
                    last_random = controller.random_personalities();
                    // Start saves the whole screen; the checkbox alone, only itself.
                    save_settings(started ? std::optional(controller.game_setup()) : std::nullopt, last_random);
                }
            }
        }
    } catch (const std::exception& error) {
        report_fatal(std::string("Poker Trainer stopped: ") + error.what());
        exit_code = 1;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
    const auto arguments = parse_arguments(argc, argv);
    if (!arguments) {
        report_fatal("Usage: poker_trainer [--seed N] [--log-dir DIR] [--random-personalities on|off] [--settings PATH] [--mode freezeout|tournament] [--big-blind N] [--stack-bb N] [--level-hands N | --level-minutes N] [--preset standard|turbo|hyper] [--antes none|player|bb] [--skip-setup | --show-setup] [--exit-after-frames N] [--auto-shove] [--stop-on WHAT] [--reveal] [--bot-delay MS] [--advice TYPE] [--hover X,Y] [--size WxH] [--screenshot PATH] [--solver] [--solver-budget MS] [--solver-tier off|river|turn|flop]");
        return 2;
    }
    return run(*arguments);
}
