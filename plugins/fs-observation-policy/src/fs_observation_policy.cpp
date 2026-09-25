#include "araya/fs-observation-policy/fs_observation_policy.hpp"

#include "araya/fs/events.hpp"
#include "araya/plugin_context.hpp"

#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace araya::fs_observation_policy {
namespace {

using araya::fs::fs_edit_intent;
using araya::fs::fs_observation;
using araya::fs::fs_observed_msg;
using araya::fs::fs_target;
using araya::fs::fs_write_intent;

// Recorded presence/absence, keyed first by owner (the acting session)
// then by the target's canonical path. A missing entry means unseen; the
// observation's discriminant keeps confirmed absence distinct.
using observed_map = std::map<std::string, std::map<std::string, fs_observation>>;

// The stable model-facing diagnostic for an unobserved edit (matching the
// harness's remediated FS_NOT_OBSERVED text).
std::string edit_not_read_message(std::string const& path) {
	return "cannot modify \"" + path + "\": file has not been read — read the file, then retry";
}

// Per-context observed state and the two intent decisions over it. One
// instance is created per apply(), so disposal drops all state.
struct observation_gate {
	observed_map observed;

	// Decide the write intent: unseen or confirmed absent => createIfAbsent;
	// confirmed present => replaceIfVersion at the observed version.
	fs_write_intent decide_write(fs_write_intent intent) {
		auto const* prior = lookup(intent.target);
		if (prior && prior->present) {
			intent.decision = araya::fs::fs_write_decision::replace_if_version;
			intent.version = prior->version;
		} else {
			intent.decision = araya::fs::fs_write_decision::create_if_absent;
		}
		return intent;
	}

	// Decide the edit guard: unseen rejects, confirmed absence rejects, and
	// presence supplies the observed version as the guard basis.
	fs_edit_intent decide_edit(fs_edit_intent intent) {
		auto const* prior = lookup(intent.target);
		if (!prior) {
			intent.error = edit_not_read_message(intent.target.display_path);
			return intent;
		}
		if (!prior->present) {
			intent.error = "cannot edit \"" + intent.target.display_path + "\": not found";
			return intent;
		}
		intent.decision = araya::fs::fs_edit_decision::replace_if_version;
		intent.version = prior->version;
		return intent;
	}

	// Record an authoritative present/absent observation. A target with no
	// owner (a direct tool call with no session) cannot be keyed, so it is
	// dropped - such calls read freely but cannot satisfy the policy.
	void observe(fs_target const& target, fs_observation const& observation) {
		if (target.owner.empty())
			return;
		observed[target.owner][target.path_key] = observation;
	}

private:
	fs_observation const* lookup(fs_target const& target) {
		if (target.owner.empty())
			return nullptr;
		auto owner = observed.find(target.owner);
		if (owner == observed.end())
			return nullptr;
		auto entry = owner->second.find(target.path_key);
		if (entry == owner->second.end())
			return nullptr;
		return &entry->second;
	}
};

struct fs_observation_policy_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto gate = std::make_shared<observation_gate>();
		// Each intent waterfall occupies the single decision slot: the
		// listener returns its decision and does not chain.
		ctx.on(
			araya::fs::write_intent_key,
			[gate](fs_write_intent const& intent, araya::waterfall_continuation<fs_write_intent>) -> fs_write_intent {
				return gate->decide_write(intent);
			});
		ctx.on(
			araya::fs::edit_intent_key,
			[gate](fs_edit_intent const& intent, araya::waterfall_continuation<fs_edit_intent>) -> fs_edit_intent {
				return gate->decide_edit(intent);
			});
		// fs/observed is emit-mode: synchronous, non-throwing recording.
		ctx.on(araya::fs::observed_key, [gate](fs_observed_msg const& msg) {
			gate->observe(msg.target, msg.observation);
		});
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_fs_observation_policy(araya::plugin_config const&) {
	return std::make_unique<fs_observation_policy_plugin>();
}

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::plugin_descriptor g_descriptor{
	"fs-observation-policy",
	g_no_deps,
	g_no_provs,
	&make_fs_observation_policy};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::fs_observation_policy
