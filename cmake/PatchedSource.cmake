# Third-party sources with local patches, kept out of the submodule (plan 22 / plan 23).
#
#   enp_patched_source(<name> SOURCE <submodule dir> PATCHES <dir> OUT <var> [PATHS <paths>...])
#
# <dir>/BASE names the exact commit the patches are made for ("<hash> [tag]"). Configure fails
# unless the submodule is checked out at that commit. The commit's files (only PATHS, if given) are
# exported with `git archive` into the build directory, so line endings and local edits in the
# checkout do not matter, and <dir>/*.patch are applied there in name order with `git apply`. The
# result is redone only when BASE or a patch changes. <var> receives the patched source directory;
# <var>_TAG the tag from BASE (if any) and <var>_COMMIT the short hash.
#
# Upgrading: move the submodule, update BASE, re-make the patches against the new commit.

find_package(Git REQUIRED)

function(enp_patched_source name)
    cmake_parse_arguments(P "" "SOURCE;PATCHES;OUT" "PATHS" ${ARGN})
    file(STRINGS "${P_PATCHES}/BASE" base LIMIT_COUNT 1)
    string(REGEX MATCH "^[0-9a-f]+" base_hash "${base}")
    string(REGEX MATCH " +([^ ]+)" _ "${base}")
    set(base_tag "${CMAKE_MATCH_1}")
    if(NOT base_hash)
        message(FATAL_ERROR "${P_PATCHES}/BASE must start with the commit hash the patches are made for")
    endif()

    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${P_SOURCE}" rev-parse HEAD
                    OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc ERROR_QUIET)
    if(NOT rc EQUAL 0 OR NOT head STREQUAL base_hash)
        message(FATAL_ERROR "${name}: ${P_SOURCE} is at '${head}', but ${P_PATCHES}/BASE is ${base_hash}.\n"
                            "Run: git submodule update --init ${P_SOURCE}")
    endif()

    file(GLOB patches "${P_PATCHES}/*.patch")
    list(SORT patches)
    set(stamp "${base_hash}")
    foreach(patch IN LISTS patches)
        file(SHA256 "${patch}" sum)
        string(APPEND stamp " ${sum}")
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${P_PATCHES}/BASE" ${patches})

    set(out "${CMAKE_BINARY_DIR}/${name}-src")
    set(previous "")
    if(EXISTS "${out}/.enp-patched")
        file(READ "${out}/.enp-patched" previous)
    endif()
    if(NOT previous STREQUAL stamp)
        message(STATUS "${name}: exporting ${base_hash} and applying ${P_PATCHES}")
        file(REMOVE_RECURSE "${out}")
        file(MAKE_DIRECTORY "${out}")
        set(tar "${CMAKE_BINARY_DIR}/${name}-src.tar")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${P_SOURCE}" archive --format=tar "--output=${tar}" HEAD ${P_PATHS}
                        RESULT_VARIABLE rc ERROR_VARIABLE err)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "${name}: git archive failed: ${err}")
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${tar}" WORKING_DIRECTORY "${out}" RESULT_VARIABLE rc)
        file(REMOVE "${tar}")
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "${name}: could not unpack the sources")
        endif()
        foreach(patch IN LISTS patches)
            # The ceiling stops git from taking the enclosing repository for this one.
            execute_process(COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${CMAKE_BINARY_DIR}"
                                    "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${patch}"
                            WORKING_DIRECTORY "${out}" RESULT_VARIABLE rc ERROR_VARIABLE err)
            if(NOT rc EQUAL 0)
                message(FATAL_ERROR "${name}: ${patch} does not apply to ${base_hash}:\n${err}")
            endif()
        endforeach()
        file(WRITE "${out}/.enp-patched" "${stamp}")
    endif()

    string(SUBSTRING "${base_hash}" 0 7 short)
    set(${P_OUT} "${out}" PARENT_SCOPE)
    set(${P_OUT}_TAG "${base_tag}" PARENT_SCOPE)
    set(${P_OUT}_COMMIT "${short}" PARENT_SCOPE)
endfunction()
