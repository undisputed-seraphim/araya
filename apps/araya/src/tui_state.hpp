#pragma once

#include "app_core.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// The TUI's engine side: the cross-thread contract (immutable UI
// snapshots plus the command queue) and the engine thread that boots
// the shared app layer, drives commands, refreshes component state, and
// publishes snapshots. The UI thread only reads snapshots; it never
// touches the runtime.
namespace araya::tui {

struct component_row {
	std::string name;
	std::string state;
	std::string error;

	friend bool operator==(component_row const&, component_row const&) = default;
};

// One rendered conversation row, folded from the current session's
// surface on the engine side (the UI never touches the session).
struct feed_message {
	std::string role;
	std::string text;
};

// One stored-session row for the picker: the id, its derived title (or
// the id), and a formatted creation date. The engine enumerates these
// on demand (when the picker opens).
struct session_row {
	std::string id;
	std::string title;
	std::string date;
	std::int64_t created_at = 0;
};

// The latest folded model metrics: the most recent assistant/message's
// usage and the active model's advertised context window. `has_usage`
// is false until a settled assistant turn carries usage.
struct token_metrics {
	std::uint64_t input_tokens = 0;
	std::uint64_t output_tokens = 0;
	std::uint64_t context_window = 0;
	bool has_usage = false;

	friend bool operator==(token_metrics const&, token_metrics const&) = default;
};

// An immutable UI snapshot: the engine builds a fresh one and swaps it
// in atomically; the UI thread copies the shared_ptr and renders.
struct snapshot {
	std::vector<component_row> components;
	std::vector<std::string> log;
	// The conversation: the current session's folded messages.
	std::vector<feed_message> messages;
	// The stored sessions the picker offers (enumerated on demand).
	std::vector<session_row> sessions;
	// Whether the user has begun (submitted anything): the entry phase
	// gives way to the session view.
	bool started = false;
	// The sidebar: the current session's derived title (the first user
	// message, or the id when there is none), the session id, the
	// collapsed working directory plus git branch, and the araya version.
	std::string title;
	std::string session;
	std::string cwd_branch;
	std::string version;
	// The working directory for the prompt hint line, unabridged.
	std::string cwd;
	// The active llm route (empty provider when none is configured).
	std::string provider;
	std::string model;
	// The latest request's folded usage and the model's context window.
	token_metrics tokens;
};

struct shared_state {
	std::atomic<std::shared_ptr<const snapshot>> snap{std::make_shared<const snapshot>()};
	std::mutex command_mutex;
	std::deque<std::string> commands;
	std::atomic<bool> quit{false};
	// Set by the UI on the first submit so the entry view gives way
	// immediately, rather than waiting for the engine's next publish.
	std::atomic<bool> started_ui{false};
	// Set by the UI when the session picker opens; the engine enumerates
	// the stored sessions once, publishes, and clears it.
	std::atomic<bool> sessions_request{false};
	// Wakes the UI loop after a publish. Set once, before the engine
	// thread starts; read only from then on.
	std::function<void()> wake;
};

// The engine-side state: the shared app layer plus the UI-facing
// component list and log. Lives on the engine thread only.
struct engine_state {
	araya::app::app_context ctx;
	std::vector<component_row> components;
	std::deque<std::string> log;
	// The folded conversation of the current session, rebuilt on the
	// strand and copied into every snapshot.
	std::vector<feed_message> messages;
	// The derived session title (first user message, or the id).
	std::string title = "no session";
	// The stored sessions the picker offers, refreshed on demand.
	std::vector<session_row> sessions;
	// The active llm route (resolved on the strand) and the latest folded
	// model metrics.
	std::optional<araya::app::active_model> route;
	token_metrics tokens;
	// Set on the first submit; mirrored into every snapshot.
	bool started = false;
	// A monotonic counter bumped by every UI-visible mutation; the
	// refresh loop publishes only when it advanced (or the component
	// list changed), so an idle UI copies nothing.
	std::uint64_t revision = 0;
	std::uint64_t published_revision = 0;
	std::vector<component_row> published_components;
	// Fold tracking: the session and log size the messages were built
	// from, so an unchanged conversation is not rebuilt every refresh.
	std::string folded_session;
	std::size_t folded_log_size = 0;
};

// Runs the engine on the calling thread (the engine thread): boots the
// tree, optionally restores `resume_id` from disk, drives commands and
// refresh, publishes snapshots, and returns once quit is signalled and
// the whole tree is retired on the strand.
void run_engine(shared_state& sh, std::string resume_id = {});

} // namespace araya::tui
