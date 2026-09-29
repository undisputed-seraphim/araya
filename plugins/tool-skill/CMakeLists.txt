# The model-facing `skill` loader and the session skill catalog context
# producer. araya::skill supplies the registry, araya::session the calling
# session's cwd, araya::agent-loop the context-producer seam, and araya::tools
# the tool registry.
araya_add_plugin(tool-skill
    SOURCES src/tool_skill.cpp
    DEPS araya::session araya::skill araya::agent-loop araya::tools
    TEST tests/tool_skill_test.cpp
    TEST_DEPS araya::llm-mock
)
