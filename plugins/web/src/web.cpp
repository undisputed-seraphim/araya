#include "araya/web/web.hpp"

#include "araya/config.hpp"
#include "araya/llm/http.hpp"
#include "araya/plugin_context.hpp"

#include <boost/beast/http/verb.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::web {
namespace {

constexpr araya::config_key<std::uint64_t> fetch_max_bytes_key{"fetch_max_bytes"};
constexpr araya::config_key<std::uint64_t> timeout_ms_key{"timeout_ms"};
constexpr araya::config_key<std::string> search_endpoint_key{"search_endpoint"};
constexpr araya::config_key<std::string> search_api_key_key{"search_api_key"};

struct web_config {
	std::size_t fetch_max_bytes = 200'000;
	std::uint64_t timeout_ms = 30'000;
	std::string search_endpoint;
	std::string search_api_key;
};

web_config parse_config(araya::plugin_config const& config) {
	araya::plugin_config_view const view(config);
	web_config out;
	if (auto value = view.try_get(fetch_max_bytes_key))
		out.fetch_max_bytes = static_cast<std::size_t>(*value);
	if (auto value = view.try_get(timeout_ms_key))
		out.timeout_ms = *value;
	if (auto value = view.try_get(search_endpoint_key))
		out.search_endpoint = *value;
	if (auto value = view.try_get(search_api_key_key))
		out.search_api_key = *value;
	return out;
}

std::string percent_encode(std::string_view text) {
	static char const* hex = "0123456789ABCDEF";
	std::string out;
	out.reserve(text.size());
	for (unsigned char const c : text) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out.push_back(static_cast<char>(c));
		} else {
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 0x0F]);
		}
	}
	return out;
}

bool is_html(std::string_view content_type) {
	std::string lowered;
	lowered.reserve(content_type.size());
	for (char const c : content_type)
		lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	return lowered.find("html") != std::string::npos;
}

std::string decode_entity(std::string_view name) {
	if (name == "amp")
		return "&";
	if (name == "lt")
		return "<";
	if (name == "gt")
		return ">";
	if (name == "quot")
		return "\"";
	if (name == "apos" || name == "#39")
		return "'";
	if (name == "nbsp")
		return " ";
	return {};
}

// Strip tags, drop script/style/noscript bodies, decode the common entities,
// and collapse runs of whitespace. A plain-text projection, not markdown.
std::string strip_html(std::string_view html) {
	std::string out;
	out.reserve(html.size());
	bool in_tag = false;
	bool in_script = false;
	bool in_entity = false;
	std::string entity;
	for (std::size_t i = 0; i < html.size(); ++i) {
		char const c = html[i];
		if (in_script) {
			if (c == '<' && html.compare(i, 8, "</script") == 0) {
				in_script = false;
			} else if (c == '<' && html.compare(i, 7, "</style") == 0) {
				in_script = false;
			}
			continue;
		}
		if (in_entity) {
			if (c == ';') {
				out += decode_entity(entity);
				entity.clear();
				in_entity = false;
			} else if (entity.size() > 10 || (!std::isalnum(static_cast<unsigned char>(c)) && c != '#')) {
				out += '&';
				out += entity;
				out.push_back(c);
				entity.clear();
				in_entity = false;
			} else {
				entity.push_back(c);
			}
			continue;
		}
		if (in_tag) {
			if (c == '>')
				in_tag = false;
			continue;
		}
		if (c == '<') {
			std::size_t const next = i + 1;
			if (html.compare(next, 7, "script>") == 0 || html.compare(next, 8, "script ") == 0 ||
				html.compare(next, 6, "style>") == 0 || html.compare(next, 7, "style ") == 0) {
				in_script = true;
			}
			in_tag = true;
			out.push_back(' ');
			continue;
		}
		if (c == '&') {
			in_entity = true;
			entity.clear();
			continue;
		}
		out.push_back(c);
	}
	std::string collapsed;
	collapsed.reserve(out.size());
	bool pending_space = false;
	for (char const c : out) {
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			pending_space = true;
			continue;
		}
		if (pending_space && !collapsed.empty())
			collapsed.push_back(' ');
		pending_space = false;
		collapsed.push_back(c);
	}
	return collapsed;
}

std::optional<search_source> source_from_json(boost::json::value const& value) {
	auto const* object = value.if_object();
	if (!object)
		return std::nullopt;
	auto const* url = object->if_contains("url");
	if (!url || !url->is_string() || url->as_string().empty())
		return std::nullopt;
	search_source source;
	source.url = std::string(url->as_string());
	if (auto const* title = object->if_contains("title"); title && title->is_string())
		source.title = std::string(title->as_string());
	if (auto const* snippet = object->if_contains("snippet"); snippet && snippet->is_string())
		source.snippet = std::string(snippet->as_string());
	return source;
}

class http_web : public web_service {
public:
	http_web(boost::asio::any_io_executor executor, web_config config)
		: executor_(std::move(executor))
		, config_(std::move(config)) {}

	bool has_search() const override { return !config_.search_endpoint.empty(); }

	araya::task<fetch_result> fetch(fetch_request request, std::stop_token stop) override {
		auto endpoint = araya::llm::http::parse_url(request.url);
		araya::llm::http::request http_request;
		http_request.server = endpoint;
		http_request.target = endpoint.path.empty() ? std::string{"/"} : endpoint.path;
		http_request.method = boost::beast::http::verb::get;
		http_request.headers.push_back({"accept", "text/html, text/plain, application/json, */*"});
		http_request.headers.push_back({"user-agent", "araya-web/1.0"});

		std::size_t const cap = request.max_bytes == 0 ? config_.fetch_max_bytes : request.max_bytes;
		auto body = std::make_shared<std::string>();
		auto truncated = std::make_shared<bool>(false);
		auto options = araya::llm::http::request_options{
			.connect_timeout = std::chrono::milliseconds(config_.timeout_ms),
			.idle_timeout = std::chrono::milliseconds(config_.timeout_ms),
		};
		auto response = co_await araya::llm::http::stream_request(
			executor_,
			http_request,
			[body, truncated, cap](std::string_view chunk) -> araya::task<void> {
				if (body->size() < cap) {
					auto const keep = std::min<std::size_t>(chunk.size(), cap - body->size());
					body->append(chunk.data(), keep);
					if (keep < chunk.size())
						*truncated = true;
				} else if (!chunk.empty()) {
					*truncated = true;
				}
				co_return;
			},
			options,
			stop);

		fetch_result result;
		result.url = request.url;
		result.status = response.status;
		result.content_type = araya::llm::http::header_value(response.headers, "content-type");
		// A non-2xx response is buffered by the client (on_body never ran).
		std::string const raw = response.status >= 200 && response.status < 300 ? *body : response.body;
		result.truncated = *truncated;
		result.text = is_html(result.content_type) ? strip_html(raw) : raw;
		co_return result;
	}

	araya::task<search_result> search(search_request request, std::stop_token stop) override {
		if (!has_search())
			throw std::runtime_error("no web search provider is configured");
		auto endpoint = araya::llm::http::parse_url(config_.search_endpoint);
		araya::llm::http::request http_request;
		http_request.server = endpoint;
		http_request.method = boost::beast::http::verb::get;
		std::string target = endpoint.path.empty() ? std::string{"/"} : endpoint.path;
		target += endpoint.path.find('?') == std::string::npos ? "?" : "&";
		target += "q=" + percent_encode(request.query);
		target += "&count=" + std::to_string(request.max_results);
		http_request.target = std::move(target);
		http_request.headers.push_back({"accept", "application/json"});
		http_request.headers.push_back({"user-agent", "araya-web/1.0"});
		if (!config_.search_api_key.empty())
			http_request.headers.push_back({"authorization", "Bearer " + config_.search_api_key});

		auto body = std::make_shared<std::string>();
		auto options = araya::llm::http::request_options{
			.connect_timeout = std::chrono::milliseconds(config_.timeout_ms),
			.idle_timeout = std::chrono::milliseconds(config_.timeout_ms),
		};
		auto response = co_await araya::llm::http::stream_request(
			executor_,
			http_request,
			[body](std::string_view chunk) -> araya::task<void> {
				body->append(chunk.data(), chunk.size());
				co_return;
			},
			options,
			stop);
		if (response.status < 200 || response.status >= 300)
			throw std::runtime_error(
				"web search failed: HTTP " + std::to_string(response.status) + " " + response.reason);

		boost::system::error_code ec;
		auto parsed = boost::json::parse(*body, ec);
		if (ec)
			throw std::runtime_error("web search returned invalid JSON");
		auto const* object = parsed.if_object();
		if (!object)
			throw std::runtime_error("web search returned an unexpected payload");
		boost::json::array const* array =
			object->if_contains("results") ? object->if_contains("results")->if_array() : nullptr;
		if (!array) {
			if (auto const* sources = object->if_contains("sources"))
				array = sources->if_array();
		}
		if (!array)
			throw std::runtime_error("web search returned no results array");

		search_result result;
		for (auto const& value : *array) {
			if (auto source = source_from_json(value))
				result.sources.push_back(std::move(*source));
		}
		if (result.sources.size() > request.max_results) {
			result.sources.resize(request.max_results);
			result.truncated = true;
		}
		co_return result;
	}

private:
	boost::asio::any_io_executor executor_;
	web_config config_;
};

std::unique_ptr<araya::plugin> make_web(araya::plugin_config const& config) {
	struct web_plugin : araya::plugin {
		explicit web_plugin(araya::plugin_config const& cfg)
			: config(parse_config(cfg)) {}

		araya::task<void> apply(araya::plugin_context& ctx) override {
			std::shared_ptr<web_service> impl = std::make_shared<http_web>(ctx.executor(), config);
			ctx.provide(web_key, std::move(impl));
			co_return;
		}

		web_config config;
	};
	return std::make_unique<web_plugin>(config);
}

static constexpr std::span<araya::dependency_spec const> g_deps{};
static const araya::provision_spec g_provs[]{{araya::service_id{"web", 1}}};
static const araya::plugin_descriptor g_descriptor{"web", g_deps, g_provs, &make_web};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::web
