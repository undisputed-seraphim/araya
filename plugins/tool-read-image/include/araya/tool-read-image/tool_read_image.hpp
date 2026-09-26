#pragma once

#include "araya/plugin.hpp"

// The model-facing `read_image` tool: read a PNG/JPEG/GIF file, persist it as a
// durable attachment, and return the image itself beside a text envelope. A
// feature-replication of the deepseek-harness `read_image` (in `dsh-tool-fs`),
// trimmed to the local image formats and the inline image block.
namespace araya::tool_read_image {

araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::tool_read_image
