#pragma once

#include "araya/plugin.hpp"

// The built-in file and search tools: `read`, `write`, `edit`, `glob`, and
// `grep`. A trimmed port of the harness's `dsh-tool-fs` and
// `dsh-tool-fs-search`: no sandbox, no read-before-write observation policy,
// and no spill store (native glob/grep over std::filesystem rather than a
// packaged ripgrep). `read_image` is omitted until the attachment/image
// subsystem exists.
namespace araya::coreutil {

araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::coreutil
