# The OpenAI-compatible chat-completions adapter. Depends on the llm
# seam (for the service, the HTTP client, and the SSE parser); Boost.JSON
# comes transitively through araya::llm, OpenSSL the same way.
araya_add_plugin(llm-openai
    SOURCES src/openai.cpp src/plugin.cpp src/translate.cpp
    DEPS araya::llm
)

# Two test executables: the pure translation suite and the in-process
# server integration suite; both need the private translate.hpp.
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
