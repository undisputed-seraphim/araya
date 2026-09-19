# The deterministic mock adapter for tests and demos: canned chunks and
# injectable failures, zero network. Depends on the llm seam only.
add_library(araya_llm_mock STATIC
    src/mock.cpp
    src/plugin.cpp
)
add_library(araya::llm-mock ALIAS araya_llm_mock)

set_target_properties(araya_llm_mock PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(araya_llm_mock PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(araya_llm_mock PUBLIC
    araya::llm
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(araya_llm_mock PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
