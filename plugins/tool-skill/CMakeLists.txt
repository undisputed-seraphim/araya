# The model-facing `skill` loader and the session skill catalog section.
# araya::skill supplies the registry, araya::session the calling session's cwd,
# and araya::tools the tool registry (with the system-prompt registry).
araya_add_plugin(tool-skill
    SOURCES src/tool_skill.cpp
    DEPS araya::session araya::skill araya::tools
    TEST tests/tool_skill_test.cpp
)
