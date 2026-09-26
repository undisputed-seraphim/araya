#include "araya/llm-openai/openai.hpp"
#include "araya/llm/http.hpp"
#include "araya/llm/sse.hpp"
#include "araya/util/json.hpp"
#include "araya/util/plugin_config_json.hpp"
#include "translate.hpp"

#include <boost/asio/this_coro.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <cstdlib>
#include <deque>
#include <string>
#include <utility>

namespace araya::llm_openai {
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
araya::llm::model_info to_model_info(openai_model const& model, std::string_view provider) {
	araya::llm::model_info info;
	info.provider = std::string(provider);
	info.model = model.id;
	info.name = model.name;
	info.context_window = model.context_window;
	info.default_max_tokens = model.default_max_tokens;
	info.reasoning_efforts = model.reasoning_efforts;
	info.supports_image = model.supports_image;
	return info;
}

} // namespace

openai_config load_config(araya::plugin_config const& config) {
	openai_config result;
	auto const text = araya::util::read_config_text(config, "llm-openai");
	if (text.empty())
		return result;

	auto const object = araya::util::parse_config_json(text, "llm-openai").as_object();
	namespace json = araya::util::json;
	if (auto value = json::opt_string(object, "provider"))
		result.provider = *value;
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
	if (auto const* models = json::get_object(object, "models")) {
		for (auto const& [id, entry] : *models) {
			openai_model model;
			model.id = std::string(id);
			model.name = std::string(id);
			if (auto const* entry_object = json::as_object(entry)) {
				if (auto value = json::opt_string(*entry_object, "name"))
					model.name = *value;
				if (auto value = json::opt_uint(*entry_object, "context_window"))
					model.context_window = *value;
				if (auto value = json::opt_uint(*entry_object, "max_tokens"))
					model.default_max_tokens = *value;
				if (auto value = json::opt_bool(*entry_object, "supports_image"))
					model.supports_image = *value;
				if (auto const* efforts = json::get_array(*entry_object, "reasoning_efforts")) {
					for (auto const& effort : *efforts) {
						if (effort.is_string())
							model.reasoning_efforts.push_back(std::string(effort.as_string()));
					}
				}
			}
			result.models.push_back(std::move(model));
		}
	}
	return result;
}

openai_adapter::openai_adapter(openai_config config)
	: config_(std::move(config)) {}

araya::task<void> openai_adapter::stream(generate_options const& options, chunk_sink const& sink) {
	auto executor = co_await boost::asio::this_coro::executor;
	// The terminal chunk is emitted after the try/catch: the standard
	// forbids await-expressions in handlers.
	std::optional<finish_chunk> terminal;
	try {
		auto endpoint = araya::llm::http::parse_url(config_.base_url);
		araya::llm::http::request request;
		request.server = std::move(endpoint);
		request.target = request.server.path + "/chat/completions";
		request.method = boost::beast::http::verb::post;
		request.body = boost::json::serialize(build_request(options));
		if (auto const* key = std::getenv(config_.api_key_env.c_str()); key && *key)
			request.headers.emplace_back("Authorization", "Bearer " + std::string(key));

		araya::llm::http::request_options request_options;
		request_options.connect_timeout = config_.connect_timeout;
		request_options.idle_timeout = config_.idle_timeout;
		request_options.verify_peer = config_.verify_peer;

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
	if (terminal)
		co_await sink(std::move(*terminal));
}

std::vector<araya::llm::model_info> openai_adapter::list_models(std::string_view provider) {
	std::vector<araya::llm::model_info> result;
	result.reserve(config_.models.size());
	for (auto const& model : config_.models)
		result.push_back(to_model_info(model, provider));
	return result;
}

araya::llm::model_info openai_adapter::resolve_model(std::string_view provider, std::string_view model) {
	if (model.empty())
		model = config_.default_model;
	for (auto const& candidate : config_.models) {
		if (candidate.id == model)
			return to_model_info(candidate, provider);
	}
	return araya::llm::llm_adapter::resolve_model(provider, model);
}

araya::llm::provider_info openai_adapter::describe_provider(std::string_view provider) {
	return araya::llm::provider_info{std::string(provider), "OpenAI"};
}

} // namespace araya::llm_openai
