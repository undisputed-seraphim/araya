#pragma once

#include "araya/plugin.hpp"

namespace araya::session_query_sqlite {

// The plugin descriptor: an alternative provider of `session-query` backed by
// an FTS5 index. Depends on `sessions` and `session.persistence` (optional)
// plus the `session-query` core library for the non-search reads.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::session_query_sqlite
