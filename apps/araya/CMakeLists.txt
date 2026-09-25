# The unified araya executable: the console and the TUI in one binary,
# everything first-party statically linked. Dispatches on the first
# argument (console | tui). The target is araya_app because the engine
# static library already owns the "araya" name; the binary installs as
# "araya".
add_executable(araya_app
    src/app_core.cpp
    src/commands_components.cpp
    src/commands_llm.cpp
    src/commands_session.cpp
    src/main.cpp
    src/run_main.cpp
    src/tui_entry.cpp
    src/tui_main.cpp
    src/tui_palette.cpp
    src/tui_picker.cpp
    src/tui_session.cpp
    src/tui_state.cpp
    src/tui_view.cpp
)
set_target_properties(araya_app PROPERTIES
    OUTPUT_NAME araya
)

target_link_libraries(araya_app PRIVATE
    araya::araya
    araya::agent-loop
    araya::coreutil
    araya::demo
    araya::fs-observation-policy
    araya::llm
    araya::llm-bridge
    araya::llm-mock
    araya::llm-openai
    araya::logger
    araya::persistence
    araya::session
    araya::shell
    araya::subagents
    araya::timer
    araya::tool-subagent
    ftxui::component
    ftxui::dom
    ftxui::screen
)

araya_enable_warnings(araya_app WERROR)

install(TARGETS araya_app
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(ARAYA_TESTING)
    add_test(NAME script_demo
        COMMAND araya run ${CMAKE_CURRENT_SOURCE_DIR}/demo/demo.txt)
    set_tests_properties(script_demo PROPERTIES TIMEOUT 60)
    add_test(NAME chat_demo
        COMMAND araya run ${CMAKE_CURRENT_SOURCE_DIR}/demo/chat.txt)
    set_tests_properties(chat_demo PROPERTIES TIMEOUT 60
        PASS_REGULAR_EXPRESSION "assistant: echo: hello world")

    add_test(NAME agent_demo
        COMMAND araya run ${CMAKE_CURRENT_SOURCE_DIR}/demo/agent.txt)
    set_tests_properties(agent_demo PROPERTIES TIMEOUT 60
        PASS_REGULAR_EXPRESSION "assistant: task done")

    add_test(NAME restart_cycle
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/demo/restart-cycle.sh
            $<TARGET_FILE:araya_app>
            ${CMAKE_CURRENT_SOURCE_DIR}/demo/save.txt
            ${CMAKE_CURRENT_SOURCE_DIR}/demo/load.txt)
    set_tests_properties(restart_cycle PROPERTIES TIMEOUT 60)

    add_test(NAME restore_cycle
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/demo/restore-cycle.sh
            $<TARGET_FILE:araya_app>
            ${CMAKE_CURRENT_SOURCE_DIR}/demo/save.txt
            ${CMAKE_CURRENT_SOURCE_DIR}/demo/restore.txt)
    set_tests_properties(restore_cycle PROPERTIES TIMEOUT 60)

    add_test(NAME prompt_cycle
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/demo/prompt-cycle.sh
            $<TARGET_FILE:araya_app>
            ${CMAKE_CURRENT_SOURCE_DIR}/demo/prompt.txt)
    set_tests_properties(prompt_cycle PROPERTIES TIMEOUT 60)

    # The pure input-routing helpers (no plugins, no app library).
    araya_add_test(araya_app_tests
        SOURCES tests/route_test.cpp
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
endif()
