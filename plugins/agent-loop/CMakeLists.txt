# The agent loop: turns and steps over the llm seam with
# plugin-registered tools. Depends on the session store for durable
# history and the llm service for streaming; Boost.JSON comes
# transitively through both.
add_library(araya_agent_loop STATIC
    src/agent.cpp
    src/bridge.cpp
    src/plugin.cpp
)
add_library(araya::agent-loop ALIAS araya_agent_loop)

set_target_properties(araya_agent_loop PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(araya_agent_loop PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(araya_agent_loop PUBLIC
    araya::session
    araya::llm
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(araya_agent_loop PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(Catch2_FOUND)
    add_executable(araya_agent_loop_tests tests/agent_test.cpp)
    target_link_libraries(araya_agent_loop_tests PRIVATE
        araya::agent-loop
        Catch2::Catch2WithMain
    )
    include(Catch)
    catch_discover_tests(araya_agent_loop_tests)
endif()
