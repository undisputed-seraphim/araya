#pragma once

#include "araya/llm/llm.hpp"
#include "araya/task.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/beast/http/verb.hpp>

#include <chrono>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A small Beast HTTP client for the LLM adapters: https via the system
// OpenSSL, plain http for local endpoints, streaming response bodies,
// per-operation idle timeouts, and stop_token cancellation.
namespace araya::llm::http {

struct endpoint {
	std::string scheme; // "http" or "https"
	std::string host;
	std::string port; // empty -> scheme default
	std::string path; // URL path prefix, without a trailing slash
};

// Parses "scheme://host[:port][/path...]". Throws std::invalid_argument
// on a missing scheme, an unsupported scheme, or an empty host. IPv6
// bracket authorities are not supported yet.
endpoint parse_url(std::string_view url);

struct request {
	endpoint server;
	std::string target; // path + query, e.g. "/v1/chat/completions"
	boost::beast::http::verb method = boost::beast::http::verb::post;
	std::string body;
	std::vector<std::pair<std::string, std::string>> headers;
};

// A non-2xx response: status/reason plus the (small, bounded) buffered
// body and the response headers (Retry-After, request ids, ...).
// Streamed 2xx bodies never appear here.
struct response {
	unsigned status = 0;
	std::string reason;
	std::string body;
	std::vector<std::pair<std::string, std::string>> headers;
};

using body_callback = std::function<araya::task<void>(std::string_view)>;

struct request_options {
	std::chrono::milliseconds connect_timeout{30000};
	std::chrono::milliseconds idle_timeout{60000};
	// Verify the TLS peer against the system CA store (ignored for http).
	bool verify_peer = true;
};

// Case-insensitive header lookup over a response's headers; empty when
// the header is absent.
std::string header_value(std::vector<std::pair<std::string, std::string>> const& headers, std::string_view name);

// Sends the request and streams a 2xx body to on_body in read-sized
// chunks; on_body may await (the sink push). A non-2xx response is
// buffered and returned without invoking on_body.
//
// Throws llm_error: stop-requested -> aborted, idle timeout -> timeout,
// connect/read/handshake failures -> transport.
araya::task<response> stream_request(
	boost::asio::any_io_executor executor,
	request const& req,
	body_callback const& on_body,
	request_options const& options = {},
	std::stop_token stop = {});

} // namespace araya::llm::http
