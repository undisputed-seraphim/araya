#include "araya/llm/http.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace beast = boost::beast;
namespace net = boost::asio;

using tcp = net::ip::tcp;

namespace araya::llm::http {
namespace {

constexpr std::size_t k_max_error_body = 1024 * 1024;

std::string host_header(endpoint const& ep) {
	auto const default_port = ep.scheme == "https" ? std::string_view("443") : std::string_view("80");
	if (ep.port.empty() || ep.port == default_port)
		return ep.host;
	return ep.host + ":" + ep.port;
}

// The layer carrying beast's timeout machinery: the tcp_stream itself
// for a plain connection, its next_layer() through the ssl_stream.
beast::tcp_stream& timed_layer(beast::tcp_stream& stream) { return stream; }

beast::tcp_stream& timed_layer(beast::ssl_stream<beast::tcp_stream>& stream) { return stream.next_layer(); }

[[noreturn]] void throw_for(boost::system::error_code ec, std::stop_token const& stop, std::string_view what) {
	if (ec == beast::error::timeout) {
		throw llm_error(llm_failure{llm_error_code::timeout, std::string(what) + ": idle timeout"});
	}
	if (ec == net::error::operation_aborted || ec == net::ssl::error::stream_truncated) {
		if (stop.stop_requested())
			throw llm_error(llm_failure{llm_error_code::aborted, std::string(what) + ": cancelled"});
		throw llm_error(llm_failure{llm_error_code::transport, std::string(what) + ": connection closed"});
	}
	throw llm_error(llm_failure{llm_error_code::transport, std::string(what) + ": " + ec.message()});
}

template <class Stream>
araya::task<response> execute(
	Stream& stream,
	request const& req,
	body_callback const& on_body,
	request_options const& options,
	std::stop_token stop) {
	std::optional<std::stop_callback<std::function<void()>>> guard;
	if (stop.stop_possible()) {
		guard.emplace(stop, [&stream]() {
			// asio guarantees close() is thread-safe and aborts every
			// in-flight operation on the socket with operation_aborted.
			boost::system::error_code ignored;
			beast::get_lowest_layer(stream).socket().close(ignored);
		});
	}

	beast::http::request<beast::http::string_body> out;
	out.method(req.method);
	out.version(11);
	out.target(req.target.empty() ? "/" : req.target);
	out.set(beast::http::field::host, host_header(req.server));
	out.set(beast::http::field::user_agent, "araya-llm/0.1");
	out.set(beast::http::field::accept, "application/json");
	out.set(beast::http::field::content_type, "application/json");
	for (auto const& [name, value] : req.headers)
		out.set(name, value);
	out.body() = req.body;
	out.prepare_payload();

	beast::flat_buffer buffer;
	beast::http::response_parser<beast::http::buffer_body> parser;
	parser.body_limit(std::numeric_limits<std::uint64_t>::max());

	response result;
	try {
		timed_layer(stream).expires_after(options.idle_timeout);
		co_await beast::http::async_write(stream, out, net::use_awaitable);

		timed_layer(stream).expires_after(options.idle_timeout);
		co_await beast::http::async_read_header(stream, buffer, parser, net::use_awaitable);
		result.status = static_cast<unsigned>(parser.get().result_int());
		result.reason = std::string(parser.get().reason());
	} catch (llm_error const&) {
		throw;
	} catch (boost::system::system_error const& e) {
		throw_for(e.code(), stop, "request");
	}

	auto& body = parser.get().body();
	auto const is_error = result.status < 200 || result.status >= 300;
	std::string chunk_buffer;
	chunk_buffer.resize(8192);
	while (!parser.is_done()) {
		// buffer_body is caller-buffered: hand the parser a fresh region
		// per read_some; it returns the bytes stored there.
		body.data = chunk_buffer.data();
		body.size = chunk_buffer.size();
		std::size_t bytes = 0;
		try {
			timed_layer(stream).expires_after(options.idle_timeout);
			bytes = co_await beast::http::async_read_some(stream, buffer, parser, net::use_awaitable);
		} catch (llm_error const&) {
			throw;
		} catch (boost::system::system_error const& e) {
			throw_for(e.code(), stop, "response body");
		}
		if (bytes == 0)
			continue;
		if (is_error) {
			if (result.body.size() < k_max_error_body)
				result.body.append(chunk_buffer.data(), std::min(bytes, k_max_error_body - result.body.size()));
		} else {
			co_await on_body(std::string_view(chunk_buffer.data(), bytes));
		}
	}
	co_return result;
}

} // namespace

endpoint parse_url(std::string_view url) {
	endpoint ep;
	auto const scheme_end = url.find("://");
	if (scheme_end == std::string_view::npos)
		throw std::invalid_argument("url: missing scheme");
	ep.scheme = std::string(url.substr(0, scheme_end));
	if (ep.scheme != "http" && ep.scheme != "https")
		throw std::invalid_argument("url: scheme must be http or https");
	auto const rest = url.substr(scheme_end + 3);
	auto const slash = rest.find('/');
	auto const authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);
	if (slash != std::string_view::npos)
		ep.path = std::string(rest.substr(slash));
	while (ep.path.size() > 1 && ep.path.back() == '/')
		ep.path.pop_back();
	auto const colon = authority.rfind(':');
	if (colon != std::string_view::npos && authority.find(':') == colon) {
		ep.host = std::string(authority.substr(0, colon));
		ep.port = std::string(authority.substr(colon + 1));
	} else {
		ep.host = std::string(authority);
	}
	if (ep.host.empty())
		throw std::invalid_argument("url: empty host");
	return ep;
}

araya::task<response> stream_request(
	boost::asio::any_io_executor executor,
	request const& req,
	body_callback const& on_body,
	request_options const& options,
	std::stop_token stop) {
	endpoint ep = req.server;
	auto const port = ep.port.empty() ? (ep.scheme == "https" ? std::string("443") : std::string("80")) : ep.port;

	tcp::resolver resolver(executor);
	beast::tcp_stream tcp(executor);
	tcp.expires_after(options.connect_timeout);
	try {
		auto const results = co_await resolver.async_resolve(ep.host, port, net::use_awaitable);
		co_await tcp.async_connect(results, net::use_awaitable);
	} catch (llm_error const&) {
		throw;
	} catch (boost::system::system_error const& e) {
		throw_for(e.code(), stop, "connect");
	}

	response result;
	if (ep.scheme == "https") {
		net::ssl::context ssl_ctx(net::ssl::context::tls_client);
		ssl_ctx.set_verify_mode(options.verify_peer ? net::ssl::verify_peer : net::ssl::verify_none);
		if (options.verify_peer)
			ssl_ctx.set_default_verify_paths();
		beast::ssl_stream<beast::tcp_stream> stream(std::move(tcp), ssl_ctx);
		if (!SSL_set_tlsext_host_name(stream.native_handle(), ep.host.c_str())) {
			boost::system::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
			throw_for(ec, stop, "handshake");
		}
		timed_layer(stream).expires_after(options.connect_timeout);
		try {
			co_await stream.async_handshake(net::ssl::stream_base::client, net::use_awaitable);
		} catch (llm_error const&) {
			throw;
		} catch (boost::system::system_error const& e) {
			throw_for(e.code(), stop, "handshake");
		}
		result = co_await execute(stream, req, on_body, options, stop);
	} else {
		result = co_await execute(tcp, req, on_body, options, stop);
	}
	co_return result;
}

} // namespace araya::llm::http
