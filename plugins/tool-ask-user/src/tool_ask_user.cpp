#include "araya/tool-ask-user/tool_ask_user.hpp"

#include "araya/plugin_context.hpp"
#include "araya/tools/tools.hpp"
#include "araya/user-questions/user_questions.hpp"
#include "araya/util/json.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace araya::tool_ask_user {
namespace {

using araya::tools::tool_context;
using araya::tools::tool_definition;
using araya::tools::tool_result;
using araya::tools::tools_key;
using araya::tools::tools_service;
using araya::user_questions::answer;
using araya::user_questions::ask_request;
using araya::user_questions::question;
using araya::user_questions::question_option;
using araya::user_questions::user_questions_key;
using araya::user_questions::user_questions_service;

tool_result text_result(std::string text, bool is_error = false) {
	return tool_result{boost::json::array{{{"type", "text"}, {"text", std::move(text)}}}, is_error};
}

tool_result error_result(std::string text) { return text_result(std::move(text), true); }

boost::json::value question_schema() {
	boost::json::object option;
	option["type"] = "object";
	option["additionalProperties"] = true;
	option["properties"] = boost::json::object{
		{"label",
		 boost::json::object{
			 {"type", "string"}, {"required", true}, {"description", "Short user-facing option label."}}},
		{"description",
		 boost::json::object{{"type", "string"}, {"description", "One sentence explaining the tradeoff."}}},
	};
	boost::json::object item;
	item["type"] = "object";
	item["additionalProperties"] = true;
	item["properties"] = boost::json::object{
		{"id",
		 boost::json::object{
			 {"type", "string"},
			 {"required", true},
			 {"description", "Stable id for this question; echoed in the answer."}}},
		{"question",
		 boost::json::object{
			 {"type", "string"}, {"required", true}, {"description", "The specific question to ask the user."}}},
		{"header",
		 boost::json::object{{"type", "string"}, {"description", "Optional short heading, such as \"Confirm\"."}}},
		{"detail",
		 boost::json::object{
			 {"type", "string"}, {"description", "Optional supporting text rendered with the question."}}},
		{"options",
		 boost::json::object{
			 {"type", "array"},
			 {"description", "Optional choices; put a recommended option first."},
			 {"items", std::move(option)}}},
		{"multi_select",
		 boost::json::object{
			 {"type", "boolean"}, {"description", "Whether the user may select more than one option."}}},
	};
	boost::json::object questions;
	questions["type"] = "array";
	questions["description"] = "Questions to ask the user before continuing.";
	questions["items"] = std::move(item);
	boost::json::object schema;
	schema["type"] = "object";
	schema["properties"] = boost::json::object{{"questions", std::move(questions)}};
	schema["required"] = boost::json::array{"questions"};
	return schema;
}

std::optional<question> parse_question(boost::json::value const& value, std::string& error) {
	auto const* object = value.if_object();
	if (!object) {
		error = "each question must be an object";
		return std::nullopt;
	}
	question parsed;
	parsed.id = araya::util::json::get_string(*object, "id");
	parsed.question = araya::util::json::get_string(*object, "question");
	if (parsed.id.empty() || parsed.question.empty()) {
		error = "each question requires a non-empty 'id' and 'question'";
		return std::nullopt;
	}
	if (auto header = araya::util::json::opt_string(*object, "header"))
		parsed.header = *header;
	if (auto detail = araya::util::json::opt_string(*object, "detail"))
		parsed.detail = *detail;
	if (auto multi = araya::util::json::opt_bool(*object, "multi_select"))
		parsed.multi_select = *multi;
	if (auto const* options = araya::util::json::get_array(*object, "options")) {
		for (auto const& entry : *options) {
			question_option option;
			if (auto const* option_object = entry.if_object()) {
				option.label = araya::util::json::get_string(*option_object, "label");
				if (auto description = araya::util::json::opt_string(*option_object, "description"))
					option.description = *description;
			} else if (entry.is_string()) {
				option.label = std::string(entry.as_string());
			}
			if (!option.label.empty())
				parsed.options.push_back(std::move(option));
		}
	}
	return parsed;
}

araya::task<tool_result> handle_ask(std::shared_ptr<user_questions_service> questions, tool_context const& ctx) {
	auto const* args = ctx.arguments.if_object();
	auto const* raw = args ? araya::util::json::get_array(*args, "questions") : nullptr;
	if (!raw || raw->empty())
		co_return error_result("Error: ask_user_question requires at least one question");

	ask_request request;
	request.stop = ctx.stop;
	for (auto const& value : *raw) {
		std::string error;
		auto parsed = parse_question(value, error);
		if (!parsed)
			co_return error_result("Error: " + error);
		request.questions.push_back(std::move(*parsed));
	}

	answer response;
	try {
		response = co_await questions->ask(std::move(request));
	} catch (std::exception const& e) {
		co_return error_result(std::string("Error: ") + e.what());
	}

	boost::json::array answers;
	for (auto const& item : response.answers) {
		boost::json::object entry;
		entry["id"] = item.id;
		boost::json::array selected;
		for (auto const& choice : item.selected)
			selected.emplace_back(choice);
		entry["selected"] = std::move(selected);
		if (item.custom)
			entry["custom"] = *item.custom;
		answers.push_back(std::move(entry));
	}
	co_return text_result(boost::json::serialize(boost::json::value{{"answers", std::move(answers)}}));
}

std::unique_ptr<araya::plugin> make_tool_ask_user(araya::plugin_config const& config) {
	(void)config;
	struct tool_ask_user_plugin : araya::plugin {
		araya::task<void> apply(araya::plugin_context& ctx) override {
			auto questions = ctx.require<user_questions_service>(user_questions_key).shared();
			auto tools = ctx.require<tools_service>(tools_key).shared();
			tools->register_tool(
				ctx,
				tool_definition{
					"ask_user_question",
					"Ask the user a concise question when you need confirmation, a choice, or missing information "
					"before proceeding. Send one or more questions, each with a stable id that will be echoed in the "
					"answer.",
					question_schema()},
				[questions](tool_context const& call) { return handle_ask(questions, call); });
			co_return;
		}
	};
	return std::make_unique<tool_ask_user_plugin>();
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"tools", 1}, true, {}},
	{araya::service_id{"user-questions", 1}, true, {}},
};
static constexpr std::span<araya::provision_spec const> g_provs{};
static const araya::plugin_descriptor g_descriptor{"tool-ask-user", g_deps, g_provs, &make_tool_ask_user};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::tool_ask_user
