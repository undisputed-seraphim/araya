# Araya source formatting.
#
# Defines two convenience targets, available when clang-format is found:
#   format       - rewrites all project sources in place
#   format-check - fails with a per-file violation list if any source
#                  is not formatted (CI-style; exit code nonzero)
#
# The file list is a CONFIGURE_DEPENDS glob over the first-party source
# directories, so newly added files are picked up on the next build
# without reconfiguring by hand. Third-party code never enters the
# source tree, so nothing needs excluding.

find_program(ARAYA_CLANG_FORMAT NAMES clang-format clang-format-19 clang-format-18)

if(ARAYA_CLANG_FORMAT)
    set(ARAYA_FORMAT_DIRS
        apps
        include
        src
        tests
        bench
        plugins
        proof/oracle
        proof/conformance
    )

    # Build the pattern list per directory: expanding ARAYA_FORMAT_DIRS
    # inline would splice the list at the semicolons and silently turn
    # every directory except the last into a bare (non-glob) name.
    set(_araya_format_patterns)
    foreach(dir IN LISTS ARAYA_FORMAT_DIRS)
        list(APPEND _araya_format_patterns
            "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.hpp")
    endforeach()

    file(GLOB_RECURSE ARAYA_FORMAT_SOURCES CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        ${_araya_format_patterns}
    )

    if(NOT TARGET format)
        add_custom_target(format
            COMMAND ${ARAYA_CLANG_FORMAT} -i ${ARAYA_FORMAT_SOURCES}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Formatting Araya sources with clang-format"
            VERBATIM
        )
    endif()

    if(NOT TARGET format-check)
        add_custom_target(format-check
            COMMAND ${ARAYA_CLANG_FORMAT} --dry-run --Werror ${ARAYA_FORMAT_SOURCES}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Checking that sources are clang-formatted"
            VERBATIM
        )
    endif()
else()
    message(STATUS "clang-format not found: 'format' and 'format-check' targets unavailable")
endif()
