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

# --- Core library (no GUI dependencies) --------------------------------------
add_library(poker_core STATIC
    src/core/card.cpp
    src/core/deck.cpp
    src/core/hand_evaluator.cpp
    src/engine/game_engine.cpp
    src/engine/pots.cpp
)
target_include_directories(poker_core PUBLIC src)
target_link_libraries(poker_core PRIVATE phevaluator_lib)
target_compile_options(poker_core PRIVATE ${POKER_WARNINGS})

# --- CLI test harness --------------------------------------------------------
add_executable(poker_cli tools/poker_cli.cpp)
target_link_libraries(poker_cli PRIVATE poker_core)
target_compile_options(poker_cli PRIVATE ${POKER_WARNINGS})

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
    )
    target_link_libraries(core_tests PRIVATE poker_core GTest::gtest_main)
    target_compile_options(core_tests PRIVATE ${POKER_WARNINGS})

    include(GoogleTest)
    gtest_discover_tests(core_tests)

    add_test(NAME poker_cli_stress COMMAND poker_cli --games 500 --seed 1)
endif()
