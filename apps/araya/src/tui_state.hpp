#pragma once

#include "app_core.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
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
};

// An immutable UI snapshot: the engine builds a fresh one and swaps it
// in atomically; the UI thread copies the shared_ptr and renders.
struct snapshot {
	std::vector<component_row> components;
	std::vector<std::string> log;
	// The sidebar: the current session's id (the title placeholder until
	// titles exist), the collapsed working directory plus git branch,
	// and the araya version.
	std::string session;
	std::string cwd_branch;
	std::string version;
	// The working directory for the prompt hint line, unabridged.
	std::string cwd;
};

struct shared_state {
	std::atomic<std::shared_ptr<const snapshot>> snap{std::make_shared<const snapshot>()};
	std::mutex command_mutex;
	std::deque<std::string> commands;
	std::atomic<bool> quit{false};
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
};

// Runs the engine on the calling thread (the engine thread): boots the
// tree, drives commands and refresh, publishes snapshots, and returns
// once quit is signalled and the whole tree is retired on the strand.
void run_engine(shared_state& sh);

} // namespace araya::tui
