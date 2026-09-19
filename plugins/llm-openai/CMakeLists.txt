# The OpenAI-compatible chat-completions adapter. Depends on the llm
# seam (for the service, the HTTP client, and the SSE parser); Boost.JSON
# comes transitively through araya::llm, OpenSSL the same way.
add_library(araya_llm_openai STATIC
    src/openai.cpp
    src/plugin.cpp
    src/translate.cpp
)
add_library(araya::llm-openai ALIAS araya_llm_openai)

set_target_properties(araya_llm_openai PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(araya_llm_openai PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(araya_llm_openai PUBLIC
    araya::llm
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(araya_llm_openai PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(Catch2_FOUND)
    add_executable(araya_llm_openai_translate_tests tests/translate_test.cpp)
    target_link_libraries(araya_llm_openai_translate_tests PRIVATE
        araya::llm-openai
        Catch2::Catch2WithMain
    )
    target_include_directories(araya_llm_openai_translate_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    include(Catch)
    catch_discover_tests(araya_llm_openai_translate_tests)

    add_executable(araya_llm_openai_adapter_tests tests/adapter_test.cpp)
    target_link_libraries(araya_llm_openai_adapter_tests PRIVATE
        araya::llm-openai
        Catch2::Catch2WithMain
    )
    target_include_directories(araya_llm_openai_adapter_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    include(Catch)
    catch_discover_tests(araya_llm_openai_adapter_tests)
endif()
