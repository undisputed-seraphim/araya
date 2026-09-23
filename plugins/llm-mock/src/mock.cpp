#include "araya/llm-mock/mock.hpp"

#include "araya/config.hpp"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json/parse.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace araya::llm_mock {
namespace {

using araya::llm::chunk_sink;
using araya::llm::content_block_type;
using araya::llm::finish_chunk;
using araya::llm::generate_options;
using araya::llm::llm_error_code;

std::string last_user_text(generate_options const& options) {
	std::string text;
	for (auto it = options.messages.rbegin(); it != options.messages.rend(); ++it) {
		if (it->role != araya::llm::message_role::user)
			continue;
		for (auto const& block : it->content) {
			if (auto const* t = std::get_if<araya::llm::text_block>(&block))
				text += t->text;
		}
		return text;
	}
	return text;
}

llm_error_code parse_fail_code(std::string_view name) {
	for (int code = static_cast<int>(llm_error_code::no_adapter);
		 code <= static_cast<int>(llm_error_code::malformed_response);
		 ++code) {
		auto const candidate = static_cast<llm_error_code>(code);
		if (name == araya::llm::llm_error::code_name(candidate))
			return candidate;
	}
	throw std::invalid_argument("llm-mock: unknown fail_code '" + std::string(name) + "'");
}

std::vector<mock_step> parse_script(std::string_view json) {
	boost::system::error_code ec;
	auto value = boost::json::parse(json, ec);
	if (ec || !value.is_array())
		throw std::invalid_argument("llm-mock: script must be a JSON array");
	std::vector<mock_step> steps;
	for (auto const& entry : value.as_array()) {
		auto const* object = entry.if_object();
		if (!object)
			throw std::invalid_argument("llm-mock: script steps must be objects");
		if (auto const* call = object->if_contains("tool_call")) {
			auto const* spec = call->if_object();
			auto const* name = spec ? spec->if_contains("name") : nullptr;
			auto const* arguments = spec ? spec->if_contains("arguments") : nullptr;
			if (!name || !name->is_string() || !arguments || !arguments->is_string())
				throw std::invalid_argument("llm-mock: tool_call steps need string name and arguments");
			mock_step step;
			step.is_tool_call = true;
			step.text = std::string(name->as_string());
			step.arguments = std::string(arguments->as_string());
			steps.push_back(std::move(step));
		} else if (auto const* text = object->if_contains("text"); text && text->is_string()) {
			mock_step step;
			step.text = std::string(text->as_string());
			steps.push_back(std::move(step));
		} else {
			throw std::invalid_argument("llm-mock: script steps need \"text\" or \"tool_call\"");
		}
	}
	return steps;
}

} // namespace

inline constexpr araya::config_key<std::string> provider_key{"provider"};
inline constexpr araya::config_key<std::string> model_key{"model"};
inline constexpr araya::config_key<std::string> response_key{"response"};
inline constexpr araya::config_key<bool> fail_key{"fail"};
inline constexpr araya::config_key<std::string> fail_code_key{"fail_code"};
inline constexpr araya::config_key<std::string> fail_message_key{"fail_message"};
inline constexpr araya::config_key<std::uint64_t> input_tokens_key{"input_tokens"};
inline constexpr araya::config_key<std::uint64_t> output_tokens_key{"output_tokens"};
inline constexpr araya::config_key<std::int64_t> delay_ms_key{"delay_ms"};
inline constexpr araya::config_key<std::string> script_key{"script"};

mock_config load_config(araya::plugin_config const& config) {
	// The typed accessors throw config_error on malformed values (a
	// missing key is still the default).
	araya::plugin_config_view view(config);
	mock_config result;
	if (auto value = view.try_get(provider_key))
		result.provider = *value;
	if (auto value = view.try_get(model_key))
		result.model = *value;
	if (auto value = view.try_get(response_key))
		result.response = *value;
	if (auto value = view.try_get(fail_key))
		result.fail = *value;
	if (auto value = view.try_get(fail_code_key))
		result.fail_code = parse_fail_code(*value);
	if (auto value = view.try_get(fail_message_key))
		result.fail_message = *value;
	if (auto value = view.try_get(input_tokens_key))
		result.input_tokens = *value;
	if (auto value = view.try_get(output_tokens_key))
		result.output_tokens = *value;
	if (auto value = view.try_get(delay_ms_key))
		result.delay = std::chrono::milliseconds(*value);
	if (auto value = view.try_get(script_key))
		result.script = parse_script(*value);
	return result;
}

mock_adapter::mock_adapter(mock_config config)
	: config_(std::move(config)) {}

araya::task<void> mock_adapter::stream(generate_options const& options, chunk_sink const& sink) {
	if (config_.delay.count() > 0) {
		auto executor = co_await boost::asio::this_coro::executor;
		boost::asio::steady_timer timer(executor, config_.delay);
		co_await timer.async_wait(boost::asio::use_awaitable);
	}
	if (config_.fail) {
		co_await sink(finish_chunk{
			finish_chunk::reason::error,
			araya::llm::llm_failure{config_.fail_code, config_.fail_message},
			std::nullopt});
		co_return;
	}

	// Script steps win while they last; the canned response takes over
	// afterwards.
	mock_step step;
	bool scripted = false;
	auto const index = script_index_++;
	if (index < config_.script.size()) {
		step = config_.script[index];
		scripted = true;
	}

	if (scripted && step.is_tool_call) {
		auto const call_id = "call-" + std::to_string(index);
		co_await sink(araya::llm::block_start_chunk{0, content_block_type::tool_call});
		co_await sink(araya::llm::tool_call_delta_chunk{0, call_id, step.text, step.arguments});
		co_await sink(araya::llm::block_end_chunk{
			0, araya::llm::content_block{araya::llm::tool_call_block{call_id, step.text, step.arguments}}});
		araya::llm::token_usage usage;
		usage.input_tokens = config_.input_tokens;
		usage.output_tokens = config_.output_tokens;
		usage.total_tokens = config_.input_tokens + config_.output_tokens;
		co_await sink(araya::llm::usage_chunk{usage});
		co_await sink(finish_chunk{finish_chunk::reason::tool_calls, std::nullopt, std::nullopt});
		co_return;
	}

	auto response = scripted ? step.text : config_.response;
	auto const user_text = last_user_text(options);
	auto const placeholder = response.find("{user}");
	if (placeholder != std::string::npos)
		response.replace(placeholder, 6, user_text);

	co_await sink(araya::llm::block_start_chunk{0, content_block_type::text});
	co_await sink(araya::llm::text_delta_chunk{0, response});
	co_await sink(araya::llm::block_end_chunk{0, araya::llm::content_block{araya::llm::text_block{response}}});
	araya::llm::token_usage usage;
	usage.input_tokens = config_.input_tokens;
	usage.output_tokens = config_.output_tokens;
	usage.total_tokens = config_.input_tokens + config_.output_tokens;
	co_await sink(araya::llm::usage_chunk{usage});
	co_await sink(finish_chunk{finish_chunk::reason::stop, std::nullopt, std::nullopt});
}

araya::llm::model_info mock_adapter::resolve_model(std::string_view provider, std::string_view) {
	araya::llm::model_info info;
	info.provider = std::string(provider);
	info.model = config_.model;
	info.name = config_.model;
	return info;
}

araya::llm::provider_info mock_adapter::describe_provider(std::string_view provider) {
	return araya::llm::provider_info{std::string(provider), "mock"};
}

} // namespace araya::llm_mock
