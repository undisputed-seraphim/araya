#include "araya/llm-compat/compat.hpp"

#include "araya/llm/http.hpp"
#include "araya/llm/sse.hpp"
#include "araya/util/json.hpp"
#include "araya/util/plugin_config_json.hpp"
#include "logging.hpp"
#include "messages.hpp"
#include "translate.hpp"

#include <boost/asio/this_coro.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <cstdlib>
#include <deque>
#include <stdexcept>
#include <string>
#include <utility>

namespace araya::llm_compat {
namespace {

using araya::llm::chunk_sink;
using araya::llm::finish_chunk;
using araya::llm::finish_from;
using araya::llm::generate_options;
using araya::llm::llm_error;
using araya::llm::llm_error_code;
using araya::llm::llm_failure;
using araya::llm::stream_chunk;
using araya::llm::http::header_value;

// The assembled model_info for one configured model, shared by
// list_models and resolve_model.
araya::llm::model_info to_model_info(compat_model const& model, std::string_view provider) {
	araya::llm::model_info info;
	info.provider = std::string(provider);
	info.model = model.id;
	info.name = model.name;
	info.context_window = model.context_window;
	info.default_max_tokens = model.default_max_tokens;
	info.default_temperature = model.default_temperature;
	info.default_reasoning_effort = model.default_reasoning_effort;
	info.system_prompt_update = model.system_prompt_update;
	info.reasoning_efforts = model.reasoning_efforts;
	info.supports_image = model.supports_image;
	info.input_price = model.input_price;
	info.output_price = model.output_price;
	info.cache_read_price = model.cache_read_price;
	info.cache_write_price = model.cache_write_price;
	return info;
}

// Built-in advisory prices (USD per million tokens) for well-known DeepSeek
// model ids, applied only when the route declares none. Prices change; they
// are configuration, and this table is a convenience.
void apply_builtin_prices(compat_model& model) {
	struct entry {
		char const* id;
		double input;
		double output;
		double cache_read;
	};
	static constexpr entry entries[] = {
		{"deepseek-chat", 0.27, 1.10, 0.07},
		{"deepseek-reasoner", 0.55, 2.19, 0.14},
	};
	for (auto const& candidate : entries) {
		if (model.id != candidate.id)
			continue;
		if (!model.input_price)
			model.input_price = candidate.input;
		if (!model.output_price)
			model.output_price = candidate.output;
		if (!model.cache_read_price)
			model.cache_read_price = candidate.cache_read;
		break;
	}
}

} // namespace

namespace {

compat_config parse_profile(boost::json::object const& object) {
	compat_config result;
	namespace json = araya::util::json;
	if (auto value = json::opt_string(object, "provider"))
		result.provider = *value;
	if (auto value = json::opt_string(object, "protocol")) {
		if (*value == "messages")
			result.protocol = compat_protocol::messages;
		else if (*value == "chat-completions")
			result.protocol = compat_protocol::chat_completions;
		else
			throw std::invalid_argument("llm-compat: protocol must be \"chat-completions\" or \"messages\"");
	}
	if (auto value = json::opt_string(object, "base_url"))
		result.base_url = *value;
	if (auto value = json::opt_string(object, "api_key_env"))
		result.api_key_env = *value;
	if (auto value = json::opt_string(object, "default_model"))
		result.default_model = *value;
	if (auto value = json::opt_uint(object, "idle_timeout_ms"))
		result.idle_timeout = std::chrono::milliseconds(*value);
	if (auto value = json::opt_uint(object, "connect_timeout_ms"))
		result.connect_timeout = std::chrono::milliseconds(*value);
	if (auto value = json::opt_bool(object, "verify_peer"))
		result.verify_peer = *value;
	if (auto value = json::opt_uint(object, "fallback_max_tokens"))
		result.fallback_max_tokens = *value;
	if (auto value = json::opt_uint(object, "thinking_budget_tokens"))
		result.thinking_budget_tokens = *value;
	if (auto const* policy = json::get_object(object, "retryPolicy")) {
		araya::llm::retry_policy parsed;
		if (auto value = json::opt_string(*policy, "mode")) {
			if (*value == "always")
				parsed.mode = araya::llm::retry_mode::always;
			else if (*value == "normal")
				parsed.mode = araya::llm::retry_mode::normal;
			else
				throw std::invalid_argument("llm-compat: retryPolicy.mode must be \"normal\" or \"always\"");
		}
		if (auto value = json::opt_uint(*policy, "maxRetries"))
			parsed.max_retries = static_cast<std::uint32_t>(*value);
		if (auto const* codes = json::get_array(*policy, "retryableCodes")) {
			for (auto const& code : *codes) {
				if (code.is_string())
					parsed.retryable_codes.push_back(std::string(code.as_string()));
			}
		}
		if (auto const* backoff = json::get_object(*policy, "backoff")) {
			if (auto value = json::opt_uint(*backoff, "initialDelayMs"))
				parsed.initial_delay = std::chrono::milliseconds(*value);
			if (auto value = json::opt_uint(*backoff, "maxDelayMs"))
				parsed.max_delay = std::chrono::milliseconds(*value);
			if (auto value = json::opt_double(*backoff, "jitterRatio"))
				parsed.jitter_ratio = *value;
		}
		result.retry = std::move(parsed);
	}
	if (auto const* models = json::get_object(object, "models")) {
		for (auto const& [id, entry] : *models) {
			compat_model model;
			model.id = std::string(id);
			model.name = std::string(id);
			if (auto const* entry_object = json::as_object(entry)) {
				if (auto value = json::opt_string(*entry_object, "name"))
					model.name = *value;
				if (auto value = json::opt_uint(*entry_object, "context_window"))
					model.context_window = *value;
				if (auto value = json::opt_uint(*entry_object, "max_tokens"))
					model.default_max_tokens = *value;
				if (auto value = json::opt_double(*entry_object, "temperature"))
					model.default_temperature = *value;
				if (auto value = json::opt_string(*entry_object, "default_reasoning_effort"))
					model.default_reasoning_effort = *value;
				if (auto value = json::opt_string(*entry_object, "systemPromptUpdate"))
					model.system_prompt_update = *value;
				else if (auto value = json::opt_string(*entry_object, "system_prompt_update"))
					model.system_prompt_update = *value;
				if (auto value = json::opt_bool(*entry_object, "supports_image"))
					model.supports_image = *value;
				if (auto const* efforts = json::get_array(*entry_object, "reasoning_efforts")) {
					for (auto const& effort : *efforts) {
						if (effort.is_string())
							model.reasoning_efforts.push_back(std::string(effort.as_string()));
					}
				}
				if (auto value = json::opt_double(*entry_object, "input_price"))
					model.input_price = *value;
				if (auto value = json::opt_double(*entry_object, "output_price"))
					model.output_price = *value;
				if (auto value = json::opt_double(*entry_object, "cache_read_price"))
					model.cache_read_price = *value;
				if (auto value = json::opt_double(*entry_object, "cache_write_price"))
					model.cache_write_price = *value;
			}
			apply_builtin_prices(model);
			result.models.push_back(std::move(model));
		}
	}
	return result;
}

} // namespace

compat_config load_config(araya::plugin_config const& config) {
	std::string const text = araya::util::read_config_text(config, "llm-compat");
	if (text.empty())
		return compat_config{};
	auto const object = araya::util::parse_config_json(text, "llm-compat").as_object();
	if (auto const* providers = object.if_contains("providers"); providers && providers->is_object()) {
		auto const& map = providers->as_object();
		if (!map.empty()) {
			compat_config profile = parse_profile(map.begin()->value().as_object());
			profile.provider = std::string(map.begin()->key());
			return profile;
		}
	}
	return parse_profile(object);
}

compat_routes load_routes(araya::plugin_config const& config) {
	compat_routes routes;
	std::string const text = araya::util::read_config_text(config, "llm-compat");
	if (text.empty()) {
		routes.emplace_back("openai", compat_config{});
		return routes;
	}
	boost::json::value const root = araya::util::parse_config_json(text, "llm-compat");
	auto const& object = root.as_object();
	if (auto const* providers = object.if_contains("providers"); providers && providers->is_object()) {
		for (auto const& [id, entry] : providers->as_object()) {
			auto const* entry_object = entry.if_object();
			if (!entry_object)
				throw std::invalid_argument("llm-compat: providers.'" + std::string(id) + "' must be an object");
			compat_config profile = parse_profile(*entry_object);
			profile.provider = std::string(id);
			routes.emplace_back(std::string(id), std::move(profile));
		}
		return routes;
	}
	compat_config profile = parse_profile(object);
	routes.emplace_back(profile.provider, std::move(profile));
	return routes;
}

compat_adapter::compat_adapter(compat_config config, araya::logger::log_handle log)
	: log_(std::move(log)) {
	profiles_.emplace(config.provider, std::move(config));
}

compat_adapter::compat_adapter(compat_routes routes, araya::logger::log_handle log)
	: log_(std::move(log)) {
	for (auto& [id, profile] : routes)
		profiles_.emplace(id, std::move(profile));
}

compat_config const& compat_adapter::profile_for(std::string_view provider) const {
	auto const found = profiles_.find(provider);
	return found != profiles_.end() ? found->second : profiles_.begin()->second;
}

araya::task<void> compat_adapter::stream(generate_options const& options, chunk_sink const& sink) {
	auto executor = co_await boost::asio::this_coro::executor;
	compat_config const& config = profile_for(options.provider);
	auto const protocol = config.protocol == compat_protocol::messages ? "messages" : "chat-completions";
	log_request_start(log_, options.provider, options.model, protocol, config.base_url);
	if (config.protocol == compat_protocol::messages) {
		compat_model model;
		model.id = options.model;
		for (auto const& candidate : config.models) {
			if (candidate.id == options.model) {
				model = candidate;
				break;
			}
		}
		co_await run_messages_call(executor, config, model, options, sink, log_);
		co_return;
	}
	// The terminal chunk is emitted after the try/catch: the standard
	// forbids await-expressions in handlers.
	std::optional<finish_chunk> terminal;
	try {
		auto endpoint = araya::llm::http::parse_url(config.base_url);
		araya::llm::http::request request;
		request.server = std::move(endpoint);
		request.target = request.server.path + "/chat/completions";
		request.method = boost::beast::http::verb::post;
		request.body = boost::json::serialize(build_request(options));
		if (auto const* key = std::getenv(config.api_key_env.c_str()); key && *key)
			request.headers.emplace_back("Authorization", "Bearer " + std::string(key));

		araya::llm::http::request_options request_options;
		request_options.connect_timeout = config.connect_timeout;
		request_options.idle_timeout = config.idle_timeout;
		request_options.verify_peer = config.verify_peer;

		// The per-read idle timeout in the HTTP layer IS the watchdog:
		// every received byte (heartbeat comments included) re-arms it.
		araya::llm::sse_parser parser;
		chunk_translator translator;
		std::deque<stream_chunk> pending;
		bool done = false;
		parser.on_event = [&](std::string_view, std::string_view data) {
			if (done)
				throw llm_error(llm_failure{llm_error_code::malformed_response, "data after [DONE]"});
			if (data == "[DONE]") {
				done = true;
				for (auto& chunk : translator.finish())
					pending.push_back(std::move(chunk));
				return;
			}
			boost::json::value wire;
			try {
				wire = boost::json::parse(data);
			} catch (...) {
				throw llm_error(llm_failure{llm_error_code::malformed_response, "malformed SSE payload"});
			}
			for (auto& chunk : translator.feed(wire))
				pending.push_back(std::move(chunk));
		};
		auto on_body = [&](std::string_view bytes) -> araya::task<void> {
			parser.feed(bytes);
			while (!pending.empty()) {
				auto chunk = std::move(pending.front());
				pending.pop_front();
				co_await sink(std::move(chunk));
			}
			co_return;
		};

		auto const response =
			co_await araya::llm::http::stream_request(executor, request, on_body, request_options, options.stop_token);
		if (response.status < 200 || response.status >= 300) {
			auto failure = failure_for(
				response.status,
				response.body,
				parse_retry_after(header_value(response.headers, "retry-after")),
				header_value(response.headers, "x-request-id"));
			terminal = finish_from(std::move(failure));
		} else if (!done) {
			terminal = finish_from(llm_failure{llm_error_code::stream_closed, "SSE stream ended without [DONE]"});
		} else {
			while (!pending.empty()) {
				auto chunk = std::move(pending.front());
				pending.pop_front();
				co_await sink(std::move(chunk));
			}
		}
	} catch (llm_error const& e) {
		terminal = finish_from(e.failure());
	} catch (boost::system::system_error const& e) {
		terminal = finish_from(llm_failure{llm_error_code::transport, e.what()});
	} catch (std::exception const& e) {
		terminal = finish_from(llm_failure{llm_error_code::invalid_request, e.what()});
	}
	if (terminal && terminal->failure)
		log_request_failure(log_, options.provider, options.model, *terminal->failure);
	if (terminal)
		co_await sink(std::move(*terminal));
}

std::vector<araya::llm::model_info> compat_adapter::list_models(std::string_view provider) {
	compat_config const& config = profile_for(provider);
	std::vector<araya::llm::model_info> result;
	result.reserve(config.models.size());
	for (auto const& model : config.models)
		result.push_back(to_model_info(model, provider));
	return result;
}

araya::llm::model_info compat_adapter::resolve_model(std::string_view provider, std::string_view model) {
	compat_config const& config = profile_for(provider);
	if (model.empty())
		model = config.default_model;
	for (auto const& candidate : config.models) {
		if (candidate.id == model)
			return to_model_info(candidate, provider);
	}
	return araya::llm::llm_adapter::resolve_model(provider, model);
}

araya::llm::provider_info compat_adapter::describe_provider(std::string_view provider) {
	return araya::llm::provider_info{
		std::string(provider), profile_for(provider).protocol == compat_protocol::messages ? "Messages" : "OpenAI"};
}

std::optional<araya::llm::retry_policy> compat_adapter::provider_retry_policy(std::string_view provider) {
	return profile_for(provider).retry;
}

} // namespace araya::llm_compat
