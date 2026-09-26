# The skill registry and its local filesystem provider: discovers directory
# (`<name>/SKILL.md`) and flat (`<name>.md`) skills under the project's
# `.dsh/skills` and `.agents/skills` roots, parses their frontmatter, and
# loads bodies on demand. Header-first; the one source implements discovery.
araya_add_plugin(skill
    SOURCES src/skill.cpp
    DEPS araya::araya
    TEST tests/skill_test.cpp
)
