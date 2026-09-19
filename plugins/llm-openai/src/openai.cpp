#include "araya/llm-openai/openai.hpp"
#include "araya/llm/http.hpp"
#include "araya/llm/sse.hpp"
#include "translate.hpp"

#include <boost/asio/this_coro.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace araya::llm_openai {
namespace {

using araya::llm::chunk_sink;
using araya::llm::finish_chunk;
using araya::llm::generate_options;
using araya::llm::llm_error;
using araya::llm::llm_error_code;
using araya::llm::llm_failure;
using araya::llm::stream_chunk;

std::optional<std::string>
header_lookup(std::vector<std::pair<std::string, std::string>> const& headers, std::string_view wanted) {
	for (auto const& [name, value] : headers) {
		if (name.size() == wanted.size() && std::equal(name.begin(), name.end(), wanted.begin(), [](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			})) {
			return value;
		}
	}
	return std::nullopt;
}

} // namespace

openai_config load_config(araya::plugin_config const& config) {
	openai_config result;
	std::string text;
	if (auto const found = config.find("config_file"); found != config.end()) {
		std::ifstream file(found->second);
		if (!file)
			throw std::invalid_argument("llm-openai: cannot open config_file '" + found->second + "'");
		std::ostringstream contents;
		contents << file.rdbuf();
		text = contents.str();
	} else if (auto const found = config.find("config"); found != config.end()) {
		text = found->second;
	}
	if (text.empty())
		return result;

	boost::json::value json;
	try {
		json = boost::json::parse(text);
	} catch (std::exception const& e) {
		throw std::invalid_argument(std::string("llm-openai: malformed config JSON: ") + e.what());
	}
	if (!json.is_object())
		throw std::invalid_argument("llm-openai: config JSON must be an object");
	auto const& object = json.as_object();
	auto take_string = [&](std::string_view key, std::string& target) {
		if (auto const* node = object.if_contains(key); node && node->is_string())
			target = std::string(node->as_string());
	};
	auto take_uint = [&](std::string_view key, std::uint64_t& target) {
		if (auto const* node = object.if_contains(key); node && node->is_uint64())
			target = node->as_uint64();
	};
	take_string("provider", result.provider);
	take_string("base_url", result.base_url);
	take_string("api_key_env", result.api_key_env);
	take_string("default_model", result.default_model);
	std::uint64_t idle_ms = 0;
	take_uint("idle_timeout_ms", idle_ms);
	if (idle_ms)
		result.idle_timeout = std::chrono::milliseconds(idle_ms);
	std::uint64_t connect_ms = 0;
	take_uint("connect_timeout_ms", connect_ms);
	if (connect_ms)
		result.connect_timeout = std::chrono::milliseconds(connect_ms);
	if (auto const* node = object.if_contains("verify_peer"); node && node->is_bool())
		result.verify_peer = node->as_bool();
	if (auto const* models = object.if_contains("models"); models && models->is_object()) {
		for (auto const& [id, entry] : models->as_object()) {
			openai_model model;
			model.id = std::string(id);
			model.name = std::string(id);
			if (entry.is_object()) {
				auto const& entry_object = entry.as_object();
				if (auto const* node = entry_object.if_contains("name"); node && node->is_string())
					model.name = std::string(node->as_string());
				if (auto const* node = entry_object.if_contains("context_window"); node && node->is_uint64())
					model.context_window = node->as_uint64();
				if (auto const* node = entry_object.if_contains("max_tokens"); node && node->is_uint64())
					model.default_max_tokens = node->as_uint64();
				if (auto const* efforts = entry_object.if_contains("reasoning_efforts");
					efforts && efforts->is_array()) {
					for (auto const& effort : efforts->as_array()) {
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
				header_lookup(response.headers, "retry-after").and_then([](std::string value) {
					return parse_retry_after(value);
				}),
				header_lookup(response.headers, "x-request-id"));
			terminal = finish_chunk{finish_chunk::reason::error, std::move(failure), std::nullopt};
		} else if (!done) {
			terminal = finish_chunk{
				finish_chunk::reason::error,
				llm_failure{llm_error_code::stream_closed, "SSE stream ended without [DONE]"},
				std::nullopt};
		} else {
			while (!pending.empty()) {
				auto chunk = std::move(pending.front());
				pending.pop_front();
				co_await sink(std::move(chunk));
			}
		}
	} catch (llm_error const& e) {
		finish_chunk finish;
		finish.why =
			e.failure().code == llm_error_code::aborted ? finish_chunk::reason::aborted : finish_chunk::reason::error;
		finish.failure = e.failure();
		terminal = std::move(finish);
	} catch (boost::system::system_error const& e) {
		terminal =
			finish_chunk{finish_chunk::reason::error, llm_failure{llm_error_code::transport, e.what()}, std::nullopt};
	} catch (std::exception const& e) {
		terminal = finish_chunk{
			finish_chunk::reason::error, llm_failure{llm_error_code::invalid_request, e.what()}, std::nullopt};
	}
	if (terminal)
		co_await sink(std::move(*terminal));
}

std::vector<araya::llm::model_info> openai_adapter::list_models(std::string_view provider) {
	std::vector<araya::llm::model_info> result;
	for (auto const& model : config_.models) {
		araya::llm::model_info info;
		info.provider = std::string(provider);
		info.model = model.id;
		info.name = model.name;
		info.context_window = model.context_window;
		info.default_max_tokens = model.default_max_tokens;
		info.reasoning_efforts = model.reasoning_efforts;
		result.push_back(std::move(info));
	}
	return result;
}

araya::llm::model_info openai_adapter::resolve_model(std::string_view provider, std::string_view model) {
	if (model.empty())
		model = config_.default_model;
	for (auto const& candidate : config_.models) {
		if (candidate.id == model) {
			araya::llm::model_info info;
			info.provider = std::string(provider);
			info.model = candidate.id;
			info.name = candidate.name;
			info.context_window = candidate.context_window;
			info.default_max_tokens = candidate.default_max_tokens;
			info.reasoning_efforts = candidate.reasoning_efforts;
			return info;
		}
	}
	return araya::llm::llm_adapter::resolve_model(provider, model);
}

araya::llm::provider_info openai_adapter::describe_provider(std::string_view provider) {
	return araya::llm::provider_info{std::string(provider), "OpenAI"};
}

} // namespace araya::llm_openai
