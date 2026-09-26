# The model-facing web_fetch/web_search tools and their prompt guidance.
# araya::web supplies the service, araya::tools the tool registry (with the
# system-prompt registry).
araya_add_plugin(tool-web
    SOURCES src/tool_web.cpp
    DEPS araya::tools araya::web
    TEST tests/tool_web_test.cpp
)
