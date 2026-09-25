# The OpenAI-compatible chat-completions adapter. The translation and
# in-process server integration suites both reach the private
# translate.hpp, so both add src/ to their include path.
araya_add_plugin(llm-openai
    SOURCES src/openai.cpp src/plugin.cpp src/translate.cpp
    DEPS araya::llm
    TESTS tests/translate_test.cpp tests/adapter_test.cpp
    TEST_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
)
