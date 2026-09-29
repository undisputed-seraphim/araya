#pragma once

#include <string_view>

namespace araya::workflow {

// The embedded Node bootstrap: framing, host calls, and the script globals.
std::string_view workflow_worker_source();

} // namespace araya::workflow
