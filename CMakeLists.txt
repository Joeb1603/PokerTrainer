cmake_minimum_required(VERSION 3.25)

project(PokerTrainer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Link the CRT statically so the final .exe is standalone.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(POKER_BUILD_TESTS "Build unit tests" ON)

include(FetchContent)

# --- Hand evaluator ----------------------------------------------------------
# Upstream's CMakeLists adds a PUBLIC -O3 flag (unknown to MSVC) and builds PLO,
# test and example targets we don't need. SOURCE_SUBDIR points at a directory
# with no CMakeLists.txt, so FetchContent only downloads the sources and the
# 5/6/7-card library is defined here instead.
FetchContent_Declare(
    phevaluator
    GIT_REPOSITORY https://github.com/HenryRLee/PokerHandEvaluator.git
    GIT_TAG        v0.6.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  cmake-no-subproject
)
FetchContent_MakeAvailable(phevaluator)

set(PHE_SRC ${phevaluator_SOURCE_DIR}/cpp/src)
add_library(phevaluator_lib STATIC
    ${PHE_SRC}/evaluator5.c
    ${PHE_SRC}/evaluator6.c
    ${PHE_SRC}/evaluator7.c
    ${PHE_SRC}/dptables.c
    ${PHE_SRC}/tables_bitwise.c
    ${PHE_SRC}/hash.c
    ${PHE_SRC}/hashtable.c
    ${PHE_SRC}/hashtable5.c
    ${PHE_SRC}/hashtable6.c
    ${PHE_SRC}/hashtable7.c
    ${PHE_SRC}/rank.c
    ${PHE_SRC}/7462.c
)
target_include_directories(phevaluator_lib SYSTEM PUBLIC ${phevaluator_SOURCE_DIR}/cpp/include)
if(MSVC)
    set(POKER_WARNINGS /W4 /permissive- /utf-8)
else()
    set(POKER_WARNINGS -Wall -Wextra -Wpedantic)
endif()

# --- JSON --------------------------------------------------------------------
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# --- Core library (no GUI dependencies) --------------------------------------
add_library(poker_core STATIC
    src/advisor/training_advisor.cpp
    src/bots/bots.cpp
    src/bots/gto_heuristic_engine.cpp
    src/core/card.cpp
    src/core/deck.cpp
    src/controller/local_game_controller.cpp
    src/core/hand_evaluator.cpp
    src/engine/game_engine.cpp
    src/engine/player_view.cpp
    src/engine/pots.cpp
    src/equity/equity.cpp
    src/logging/hand_logger.cpp
    src/range/combos.cpp
    src/range/generated/preflop_equity_data.cpp
    src/range/hand_vs_range.cpp
    src/range/preflop_matrix.cpp
    src/range/weighted_range.cpp
    src/solver/betting_tree.cpp
    src/solver/postflop_solver.cpp
    src/solver/showdown.cpp
    src/strategy/preflop_charts.cpp
)
find_package(Threads REQUIRED)
target_include_directories(poker_core PUBLIC src)
target_link_libraries(poker_core PRIVATE phevaluator_lib Threads::Threads nlohmann_json::nlohmann_json)
target_compile_options(poker_core PRIVATE ${POKER_WARNINGS})

# --- CLI test harness --------------------------------------------------------
add_executable(poker_cli tools/poker_cli.cpp)
target_link_libraries(poker_cli PRIVATE poker_core)
target_compile_options(poker_cli PRIVATE ${POKER_WARNINGS})

# Regenerates src/range/generated/preflop_equity_data.cpp; run from the repo root.
add_executable(gen_preflop_tables tools/gen_preflop_tables.cpp)
target_link_libraries(gen_preflop_tables PRIVATE poker_core)
target_compile_options(gen_preflop_tables PRIVATE ${POKER_WARNINGS})

# --- Desktop app (GLFW + OpenGL3 + Dear ImGui) ---------------------------------
option(POKER_BUILD_APP "Build the poker_trainer desktop app" ON)
if(POKER_BUILD_APP)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4
        GIT_SHALLOW    TRUE
    )
    # Upstream ships no CMakeLists; the target is defined below, as with phevaluator.
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.92.9b
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(glfw imgui)
    find_package(OpenGL REQUIRED)

    add_library(imgui_lib STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui_lib SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)

    # Separate from main.cpp so tests can drive the UI without a window.
    add_library(trainer_ui STATIC src/app/trainer_ui.cpp)
    target_include_directories(trainer_ui PUBLIC src)
    target_link_libraries(trainer_ui PUBLIC poker_core imgui_lib)
    target_compile_options(trainer_ui PRIVATE ${POKER_WARNINGS})

    add_executable(poker_trainer src/app/main.cpp)
    target_link_libraries(poker_trainer PRIVATE trainer_ui)
    target_compile_options(poker_trainer PRIVATE ${POKER_WARNINGS})
    if(MSVC)
        # A window with no console, keeping main() as the entry point.
        set_target_properties(poker_trainer PROPERTIES WIN32_EXECUTABLE TRUE)
        target_link_options(poker_trainer PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()

# --- Tests -------------------------------------------------------------------
if(POKER_BUILD_TESTS)
    enable_testing()

    set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.17.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(googletest)

    add_executable(core_tests
        tests/card_test.cpp
        tests/deck_test.cpp
        tests/hand_evaluator_test.cpp
        tests/pots_test.cpp
        tests/game_engine_test.cpp
        tests/player_view_test.cpp
        tests/player_boundary_test.cpp
        tests/equity_test.cpp
        tests/preflop_charts_test.cpp
        tests/range_test.cpp
        tests/solver_test.cpp
        tests/bots_test.cpp
        tests/training_advisor_test.cpp
        tests/hand_logger_test.cpp
        tests/game_controller_test.cpp
    )
    target_link_libraries(core_tests PRIVATE poker_core GTest::gtest_main nlohmann_json::nlohmann_json)
    target_compile_options(core_tests PRIVATE ${POKER_WARNINGS})
    if(POKER_BUILD_APP)
        target_sources(core_tests PRIVATE tests/trainer_ui_test.cpp)
        target_link_libraries(core_tests PRIVATE trainer_ui)
    endif()

    include(GoogleTest)
    # PRE_TEST: the default POST_BUILD list follows whichever config linked last,
    # so ctest -C Debug could silently run the Release binary.
    gtest_discover_tests(core_tests DISCOVERY_MODE PRE_TEST)

    add_test(NAME poker_cli_stress COMMAND poker_cli --games 500 --seed 1)

    # Bots and the advisor must see the table only through PlayerView.
    add_test(NAME no_engine_access_in_players
             COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/check_no_engine_access.cmake)
endif()
