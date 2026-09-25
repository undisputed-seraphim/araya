#pragma once

#include "araya/events.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// The filesystem vocabulary: three events the fs tools dispatch and the
// observation policy listens for, so the emitter (coreutil) and the
// listener (fs-observation) share a vocabulary without the emitter
// depending on the policy plugin.
//
//   fs/write-intent (waterfall): the executor asks for the write guard.
//     No listener (or a listener returning the intent unchanged) means
//     "unconstrained" - the bare provider behavior, an unconditional
//     create-or-overwrite.
//   fs/edit-intent (waterfall): the executor asks for the edit guard.
//     A listener denies by setting `error` (the model-facing message) or
//     supplies an observed version for a guarded replacement.
//   fs/observed (emit): recorded presence/absence, emitted after a read
//     resolves the target and after a successful mutation.
//
// Faithful to the deepseek-harness `@deepseek-ai/dsh-fs` event gate: the
// executor holds no policy state and the policy performs no filesystem
// I/O.
namespace araya::fs {

// An opaque freshness token. The executor manufactures it from a stat;
// the policy only stores and compares it.
using fs_version = std::string;

// One resolved target. `owner` is the acting session id (empty when there
// is none); `path_key` is the canonical absolute path the policy keys on;
// `display_path` is the path rendered to the model.
struct fs_target {
	std::string owner;
	std::string path_key;
	std::string display_path;
};

enum class fs_write_decision {
	unconstrained,
	create_if_absent,
	replace_if_version,
};

// The write-intent message. `version` is meaningful only for
// replace_if_version.
struct fs_write_intent {
	fs_target target;
	fs_write_decision decision = fs_write_decision::unconstrained;
	fs_version version;
};

enum class fs_edit_decision {
	unconstrained,
	replace_if_version,
};

// The edit-intent message. A non-empty `error` denies the edit outright;
// otherwise `decision`/`version` carry the guard.
struct fs_edit_intent {
	fs_target target;
	fs_edit_decision decision = fs_edit_decision::unconstrained;
	fs_version version;
	std::string error;
};

// A recorded presence/absence. The executor emits one after a read
// resolves the target and after a successful mutation.
struct fs_observation {
	bool present = false;
	fs_version version;
};

// The fs/observed message: the target plus its observation.
struct fs_observed_msg {
	fs_target target;
	fs_observation observation;
};

inline constexpr araya::event_key<fs_write_intent, araya::dispatch_mode::waterfall> write_intent_key{
	"fs/write-intent",
	1};
inline constexpr araya::event_key<fs_edit_intent, araya::dispatch_mode::waterfall> edit_intent_key{"fs/edit-intent", 1};
inline constexpr araya::event_key<fs_observed_msg, araya::dispatch_mode::emit> observed_key{"fs/observed", 1};

// The target's freshness token (`<mtime>:<size>`), or nullopt when the
// path is not a regular file.
std::optional<fs_version> read_version(std::filesystem::path const& path);

} // namespace araya::fs
