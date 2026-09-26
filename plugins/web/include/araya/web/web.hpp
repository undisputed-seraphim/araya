#pragma once

#include "araya/plugin.hpp"
#include "araya/service.hpp"
#include "araya/task.hpp"

#include <cstddef>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

// The web access service: fetch one URL, or search the web through a
// configured provider. A feature-replication of the deepseek-harness
// `@deepseek-ai/dsh-web` + `@deepseek-ai/dsh-web-fetch-http`, trimmed to a
// single built-in HTTP fetch client (the shared `araya::llm::http` Beast client)
// and one configurable HTTP-JSON search provider.
//
// Divergences (recorded): no provider registry or provider pinning (one
// config-driven search provider); no redirect following, robots handling, or
// HTML-to-markdown conversion (tags are stripped to plain text); search speaks a
// simple documented JSON contract.
namespace araya::web {

struct fetch_request {
	std::string url;
	std::size_t max_bytes = 200'000;
};

struct fetch_result {
	std::string url;
	unsigned status = 0;
	std::string content_type;
	// Decoded body text (HTML tags stripped when the response is HTML).
	std::string text;
	bool truncated = false;
};

struct search_source {
	std::string title;
	std::string url;
	std::string snippet;
};

struct search_request {
	std::string query;
	std::size_t max_results = 8;
};

struct search_result {
	std::vector<search_source> sources;
	bool truncated = false;
};

class web_service {
public:
	virtual ~web_service() = default;

	// Whether a usable search provider is configured.
	virtual bool has_search() const = 0;

	// Fetch one URL. A non-2xx response is a result, not an error; transport
	// failures throw.
	virtual araya::task<fetch_result> fetch(fetch_request request, std::stop_token stop) = 0;

	// Run one search query. Throws when no provider is configured or the
	// provider fails.
	virtual araya::task<search_result> search(search_request request, std::stop_token stop) = 0;
};

inline constexpr araya::service_key<web_service> web_key{"web", 1};

// The plugin descriptor: provides `web`; no service dependencies.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::web
