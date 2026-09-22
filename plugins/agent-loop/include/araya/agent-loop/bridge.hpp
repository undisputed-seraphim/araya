#pragma once

#include "araya/llm/llm.hpp"
#include "araya/session/session_types.hpp"

#include <boost/json/value.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The session surface <-> llm message bridge, owned by the agent loop.
// The session fold reads only the "message" member of the built-in
// envelopes; usage/replay/turn/step ride along as siblings.
namespace araya::agent {

// The session surface -> llm message bridge. Tool results project onto
// the user role (the session's own projection), and unknown block types
// are dropped.
araya::llm::llm_message to_llm_message(araya::session::session_message const& message);

// llm content blocks -> the session content block array.
boost::json::value blocks_to_json(std::vector<araya::llm::content_block> const& blocks);

// The assistant/message envelope (built-in surface shape).
boost::json::value assistant_message_data(
	std::string_view id,
	std::vector<araya::llm::content_block> const& blocks,
	araya::llm::token_usage const& usage,
	std::optional<boost::json::value> replay,
	std::uint64_t turn,
	std::uint64_t step,
	bool interrupted);

// The system/message envelope (built-in surface shape: source.kind must
// be "plugin").
boost::json::value system_message_data(std::string_view id, std::string_view plugin, std::string_view text);

// The user/message envelope (built-in surface shape).
boost::json::value user_message_data(std::string_view id, std::string_view text);

// The tool/result envelope: the data IS the message, exactly one content
// block carrying the call id, and source.call_id matching it (the
// store's validated shape).
boost::json::value
tool_result_data(std::string_view id, std::string_view call_id, boost::json::value const& content, bool is_error);

} // namespace araya::agent
