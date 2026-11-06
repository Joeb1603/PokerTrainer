# Fails if code the players are built from can reach the engine or hidden state.
# The compile-time half of this is tests/player_boundary_test.cpp.
set(forbidden_patterns
    "game_engine\\.h"
    "game_types\\.h"
    "GameEngine"
    "GameState"
    "(^|[^A-Za-z_])Seat([^A-Za-z_]|$)"
    "\\.hole([^A-Za-z_]|$)"
    "showdown_ranks"
    "last_result")

function(first_forbidden text out)
    foreach(pattern IN LISTS forbidden_patterns)
        string(REGEX MATCH "${pattern}" found "${text}")
        if(found)
            set(${out} "${pattern}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out} "" PARENT_SCOPE)
endfunction()

# Negative controls: a check that accepts these would pass whatever the code did.
foreach(bad
        "#include \"engine/game_types.h\""
        "const GameState& state"
        "const Seat& s = seats[0];"
        "auto c = seat.hole;")
    first_forbidden("${bad}" hit)
    if(NOT hit)
        message(FATAL_ERROR "The check failed to catch: ${bad}")
    endif()
endforeach()
foreach(good "const PublicSeat& s" "view.own_hole" "const std::array<Card, 2>& hole")
    first_forbidden("${good}" hit)
    if(hit)
        message(FATAL_ERROR "The check wrongly rejects: ${good}")
    endif()
endforeach()

# A pattern for a missing folder (src/solver before it exists) matches nothing.
file(GLOB_RECURSE sources
     ${ROOT}/src/bots/* ${ROOT}/src/advisor/* ${ROOT}/src/strategy/* ${ROOT}/src/equity/*
     ${ROOT}/src/range/* ${ROOT}/src/solver/*)
if(NOT sources)
    message(FATAL_ERROR "No sources found under ${ROOT}: the check would pass vacuously")
endif()
foreach(source IN LISTS sources)
    file(READ ${source} text)
    first_forbidden("${text}" hit)
    if(hit)
        message(FATAL_ERROR "${source} matches ${hit}")
    endif()
endforeach()
