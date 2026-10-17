# The compatibility LLM adapter: OpenAI Chat Completions and Anthropic
# Messages over one configurable HTTP client. The translation and in-process
# server integration suites both reach the private translate.hpp, so both add
# src/ to their include path.
araya_add_plugin(llm-compat
    SOURCES src/compat.cpp src/messages.cpp src/plugin.cpp src/translate.cpp
    DEPS araya::llm araya::logger
    TESTS tests/translate_test.cpp tests/messages_test.cpp tests/adapter_test.cpp
    TEST_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
)
