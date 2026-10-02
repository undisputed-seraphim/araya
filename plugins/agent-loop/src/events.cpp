#include "araya/agent-loop/events.hpp"

#include <boost/json/object.hpp>

#include <optional>

namespace araya::agent {

namespace {

std::optional<std::uint64_t> turn_of(boost::json::value const& data) {
	auto const* object = data.if_object();
	if (!object)
		return std::nullopt;
	auto it = object->find("turn");
	if (it == object->end())
		return std::nullopt;
	if (it->value().is_uint64())
		return it->value().as_uint64();
	if (it->value().is_int64() && it->value().as_int64() >= 0)
		return static_cast<std::uint64_t>(it->value().as_int64());
	return std::nullopt;
}

} // namespace

araya::session::event_projection<turn_state> turn_boundary_projection() {
	return araya::session::event_projection<turn_state>{
		.name = "agent-turn-boundary",
		.types = {"turn/start", "turn/end", "step/start", "step/end"},
		.init = [](araya::session::session_header const&) { return turn_state{}; },
		.apply =
			[](turn_state& state, araya::session::session_event const& event) {
				if (event.type == "turn/start") {
					state.open_turn_start_seq = event.seq;
					if (auto turn = turn_of(event.data))
						state.last_turn = *turn;
				} else if (event.type == "turn/end") {
					state.open_turn_start_seq.reset();
				} else if (event.type == "step/start") {
					state.last_step_start_seq = event.seq;
					state.last_step_boundary = step_boundary{true, event.seq};
				} else if (event.type == "step/end") {
					state.last_step_boundary = step_boundary{false, event.seq};
				}
			}};
}

} // namespace araya::agent
