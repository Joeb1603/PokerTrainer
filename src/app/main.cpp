// poker_trainer: the Dear ImGui desktop front end.
//
//   poker_trainer [--seed N] [--log-dir DIR]
//
// Developer checks, for running it unattended:
//   --exit-after-frames N   Quit after N frames
//   --auto-shove            Hero moves all-in at every decision, with bots instant and
//                           hands and sessions advancing by themselves
//   --stop-on WHAT          With --auto-shove, hold the screen at: live-side-pot (Hero
//                           to act with side pots), side-pot (showdown with side
//                           pots), showdown, flop (Hero to act with a board), or bust
//   --reveal                With --auto-shove, reveal the bots' hands at the held screen
//   --bot-delay MS          Start with this bot delay, to hold a "waiting" screen
//   --advice TYPE           Start the "Advice:" selector on baseline, nit, station or maniac
//   --hover X,Y             Hold the ImGui mouse at window point X,Y, to show a tooltip
//   --size WxH              Open the window at this size (unscaled), e.g. the 1180x760 minimum
//   --screenshot PATH       Save the last frame as a PNG (with --exit-after-frames, or
//                           when the window is closed)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
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

#include "app/screenshot.h"
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
};

std::optional<poker::Archetype> parse_archetype(std::string_view text) {
    for (int i = 0; i < poker::kArchetypeCount; ++i) {
        const auto type = static_cast<poker::Archetype>(i);
        std::string name(poker::to_string(type));
        for (char& c : name) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (name == text) return type;
    }
    return std::nullopt;
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
            double x = 0.0;
            double y = 0.0;
            if (std::sscanf(argv[++i], "%lf,%lf", &x, &y) != 2) return std::nullopt;
            arguments.hover = std::pair{x, y};
        } else if (arg == "--size" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &arguments.width, &arguments.height) != 2 || arguments.width < 1180 ||
                arguments.height < 760) {
                return std::nullopt;
            }
        } else if (arg == "--screenshot" && i + 1 < argc) {
            arguments.screenshot = argv[++i];
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
    try {
        poker::ControllerOptions options;
        options.seed = arguments.seeded ? arguments.seed : (static_cast<std::uint64_t>(std::random_device{}()) << 32) ^
                                                               std::random_device{}();
        options.log_dir = arguments.log_dir;
        options.instant_play = arguments.auto_shove;
        if (arguments.bot_delay_ms) options.bot_delay_ms = *arguments.bot_delay_ms;
        poker::LocalGameController controller(options);
        poker::TrainerUi ui(controller, fonts, scale);
        ui.set_advice_focus(arguments.advice);

        long frames = 0;
        bool last_frame = false;
        while (!last_frame) {
            glfwPollEvents();
            last_frame = glfwWindowShouldClose(window) != 0 ||
                         (arguments.exit_after_frames && frames + 1 >= *arguments.exit_after_frames);
            controller.tick(static_cast<double>(io.DeltaTime) * 1000.0);

            if (arguments.auto_shove) auto_play(controller, arguments);

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            if (arguments.hover) {  // After the backend's own mouse update, so this one wins
                io.AddMousePosEvent(static_cast<float>(arguments.hover->first), static_cast<float>(arguments.hover->second));
            }
            ImGui::NewFrame();
            ui.draw();
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
        report_fatal("Usage: poker_trainer [--seed N] [--log-dir DIR] [--exit-after-frames N] [--auto-shove] [--stop-on WHAT] [--reveal] [--bot-delay MS] [--advice TYPE] [--hover X,Y] [--size WxH] [--screenshot PATH]");
        return 2;
    }
    return run(*arguments);
}
