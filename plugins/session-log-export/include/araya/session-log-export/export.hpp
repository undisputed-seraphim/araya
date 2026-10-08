#pragma once

#include "araya/plugin.hpp"
#include "araya/service.hpp"
#include "araya/session/session_types.hpp"

#include <string>

// Session log export: build a ZIP archive of one session's canonical JSONL
// log, optionally including its subagent descendants and referenced image
// attachments. The archive is the harness's export shape adapted to the
// TUI: the slash command writes it to the filesystem instead of streaming
// it to a browser.
namespace araya::session_export {

class session_export_service {
public:
	virtual ~session_export_service() = default;

	// The ZIP bytes for one session. Throws std::runtime_error when the
	// session is unknown or a referenced log cannot be read.
	virtual std::string build_zip(session::session_id const& session, bool include_descendants) const = 0;
};

inline constexpr araya::service_key<session_export_service> session_export_key{"session-export", 1};

// The download filename for a session: `araya-session-<sanitized>.zip`.
std::string session_export_filename(session::session_id const& session);

// The plugin descriptor: apply() resolves `sessions` (required) and
// `session.persistence`, `attachments`, and `session-query` (optional) and
// provides `session-export`.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::session_export
