# The unified araya executable: the console and the TUI in one binary,
# everything first-party statically linked. Dispatches on the first
# argument (console | tui). The target is araya_app because the engine
# static library already owns the "araya" name; the binary installs as
# "araya".
add_executable(araya_app
    src/app_core.cpp
    src/main.cpp
    src/run_main.cpp
    src/tui_main.cpp
    src/tui_state.cpp
    src/tui_view.cpp
)
set_target_properties(araya_app PROPERTIES
    OUTPUT_NAME araya
)

target_link_libraries(araya_app PRIVATE
    araya::araya
    araya::demo
    araya::logger
    araya::persistence
    araya::session
    araya::timer
    ftxui::component
    ftxui::dom
    ftxui::screen
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(araya_app PRIVATE -Wall -Wextra -Wpedantic -Werror)
    # GCC 14 false positive (same as the engine library): awaitable
    # frames are allocated through Boost.Asio's aligned_new
    # (aligned_alloc) and released through the matching
    # awaitable_frame_base::operator delete, which the mismatch
    # detector cannot see across the inlining boundary at -O2.
    target_compile_options(araya_app PRIVATE -Wno-mismatched-new-delete)
endif()

install(TARGETS araya_app
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(CMAKE_TESTING_ENABLED)
    add_test(NAME script_demo
        COMMAND araya run ${CMAKE_CURRENT_SOURCE_DIR}/demo/demo.txt)
    set_tests_properties(script_demo PROPERTIES TIMEOUT 60)

    add_test(NAME restart_cycle
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/demo/restart-cycle.sh
            $<TARGET_FILE:araya_app>
            ${CMAKE_CURRENT_SOURCE_DIR}/demo/save.txt
            ${CMAKE_CURRENT_SOURCE_DIR}/demo/load.txt)
    set_tests_properties(restart_cycle PROPERTIES TIMEOUT 60)
endif()
