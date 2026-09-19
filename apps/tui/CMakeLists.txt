# The FTXUI terminal UI: the console's eventual successor. It boots the
# same desired tree (logger/timer/session/persistence + the demo
# components) on a background engine thread and renders live component
# state through FTXUI on the main thread - the UI never touches the
# runtime, it only reads immutable snapshots.
add_executable(araya_tui
    src/main.cpp
)

target_link_libraries(araya_tui PRIVATE
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
    target_compile_options(araya_tui PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
