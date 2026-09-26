#pragma once

#include "araya/effects.hpp"
#include "araya/plugin.hpp"
#include "araya/plugin_context.hpp"
#include "araya/service.hpp"
#include "araya/task.hpp"

#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

// The user-interaction seam: a model-facing tool (or permission plugin) pauses
// work and asks the human for a decision. A feature-replication of the
// deepseek-harness `@deepseek-ai/dsh-user-questions`, trimmed to one local
// answerer (the app's script/console prompt) instead of a scoped waterfall.
namespace araya::user_questions {

struct question_option {
	std::string label;
	std::string description;
};

struct question {
	std::string id;
	std::string question;
	std::optional<std::string> detail;
	std::optional<std::string> header;
	std::vector<question_option> options;
	bool multi_select = false;
};

struct ask_request {
	std::vector<question> questions;
	std::stop_token stop;
};

struct answer_item {
	std::string id;
	std::vector<std::string> selected;
	std::optional<std::string> custom;
};

struct answer {
	std::vector<answer_item> answers;
};

// A synchronous answerer: it renders the questions (through whatever surface
// it owns) and returns the human's answer. Absence of any answerer makes ask()
// fail rather than block.
using answerer_fn = std::function<answer(ask_request const&)>;

class user_questions_service {
public:
	virtual ~user_questions_service() = default;

	// Registers an answerer owned by `caller`; removed at teardown. The first
	// registered answerer serves ask().
	virtual araya::registration register_answerer(araya::plugin_context& caller, answerer_fn answerer) = 0;

	// Ask the registered answerer. Throws when no answerer is registered, when
	// there are no questions, or when `stop` is already requested.
	virtual araya::task<answer> ask(ask_request request) = 0;
};

inline constexpr araya::service_key<user_questions_service> user_questions_key{"user-questions", 1};

// The plugin descriptor: provides `user-questions`; no dependencies.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::user_questions
