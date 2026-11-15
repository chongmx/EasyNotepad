# A third-party submodule locked to an exact commit (plan 22):
#
#   enp_pinned_source(<name> SOURCE <submodule dir> BASE <file>)
#
# <file> names the commit ("<hash> [tag]"). Configure fails unless the submodule is checked out at
# that commit, so `git submodule update --remote` or a stray checkout inside the submodule cannot
# change the version unnoticed. ENP_ALLOW_UNPINNED=ON turns the failure into a warning, for trying
# another version on purpose. Without git, or outside a git checkout (a source archive), nothing can
# be checked and nothing is. (enp_patched_source, for submodules with local patches, checks the same.)
#
# Upgrading: check out the new commit in the submodule, put its hash (and tag) in BASE, build and
# run the tests, and commit the submodule and BASE together.

find_package(Git QUIET)
option(ENP_ALLOW_UNPINNED "Build third-party code at a commit other than the one its BASE names (experiments)" OFF)

function(enp_pinned_source name)
    cmake_parse_arguments(P "" "SOURCE;BASE" "" ${ARGN})
    file(STRINGS "${P_BASE}" base LIMIT_COUNT 1)
    string(REGEX MATCH "^[0-9a-f]+" base_hash "${base}")
    if(NOT base_hash)
        message(FATAL_ERROR "${P_BASE} must start with the commit hash ${name} is locked to")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${P_BASE}")
    if(NOT GIT_FOUND OR NOT EXISTS "${P_SOURCE}/.git")
        return()
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${P_SOURCE}" rev-parse HEAD
                    OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc ERROR_QUIET)
    if(NOT rc EQUAL 0)
        message(WARNING "${name}: cannot ask git which commit ${P_SOURCE} is at; not checked against ${P_BASE}")
        return()
    endif()
    if(NOT head STREQUAL base_hash)
        set(msg "${name}: ${P_SOURCE} is at ${head}, but it is locked to ${base_hash} (${P_BASE}).\n"
                "Run: git submodule update --init ${P_SOURCE}\n"
                "(To build another version on purpose: -DENP_ALLOW_UNPINNED=ON, or move the lock: see ${P_BASE}.)")
        if(ENP_ALLOW_UNPINNED)
            message(WARNING ${msg})
        else()
            message(FATAL_ERROR ${msg})
        endif()
    endif()
endfunction()
