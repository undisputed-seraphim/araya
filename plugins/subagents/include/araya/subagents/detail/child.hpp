#pragma once

#include "araya/session/store.hpp"

#include <memory>
#include <string>
#include <utility>

// Shared plumbing for the in-process subagent providers. Not part of the
// public delegation seam: providers use it to build a child's session options
// from the parent header and to enter the prepared child the same way.
namespace araya::subagents::detail {

// The common child options: subagent origin, parent link, depth, and (when the
// parent is present) its cwd and agent preset.
inline araya::session::create_session_options
child_session_options(std::string const& parent_id, std::uint32_t child_depth, araya::session::session const* parent) {
	araya::session::create_session_options options;
	options.origin = araya::session::session_origin::subagent;
	options.parent_session = araya::session::session_id{parent_id};
	options.delegation_depth = child_depth;
	if (parent) {
		options.cwd = parent->header().cwd;
		options.agent_preset = parent->header().agent_preset;
	}
	return options;
}

// Prepare, enter, and announce a child; returns the live child. Callers may
// have populated seed fields on `options` first.
inline std::shared_ptr<araya::session::session>
enter_child(araya::session::session_store& store, araya::session::create_session_options options) {
	auto child = store.prepare(store.mint_id(), std::move(options));
	store.enter(child);
	store.announce(*child);
	return child;
}

} // namespace araya::subagents::detail
