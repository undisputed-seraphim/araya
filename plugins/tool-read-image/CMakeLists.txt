# The model-facing read_image tool: read/validate an image, persist it as an
# attachment, and return the image block beside its text envelope.
araya_add_plugin(tool-read-image
    SOURCES src/tool_read_image.cpp
    DEPS araya::attachment araya::llm araya::session araya::tools
    TEST tests/tool_read_image_test.cpp
)
