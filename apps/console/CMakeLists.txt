add_executable(araya_console
    src/main.cpp
    src/demo_plugins.cpp
)

target_link_libraries(araya_console PRIVATE
    araya::araya
    araya::logger
    araya::session
    araya::timer
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(araya_console PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(CMAKE_TESTING_ENABLED)
    add_test(NAME console_demo
        COMMAND araya_console --script ${CMAKE_CURRENT_SOURCE_DIR}/demo/demo.txt)
    set_tests_properties(console_demo PROPERTIES TIMEOUT 60)
endif()
