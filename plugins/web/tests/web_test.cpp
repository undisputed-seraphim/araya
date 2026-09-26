#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/web/web.hpp"

#include "support/plugin_harness.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/string_body.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

struct test_server {
	explicit test_server(boost::asio::any_io_executor executor)
		: acceptor(executor, tcp::endpoint{net::ip::make_address("127.0.0.1"), 0}) {}

	std::uint16_t port() const { return acceptor.local_endpoint().port(); }

	std::function<http::response<http::string_body>(http::request<http::string_body> const&)> on_request;

	araya::task<void> serve() {
		auto socket = co_await acceptor.async_accept(net::use_awaitable);
		beast::flat_buffer buffer;
		http::request<http::string_body> request;
		co_await http::async_read(socket, buffer, request, net::use_awaitable);
		auto response = on_request(request);
		co_await http::async_write(socket, response, net::use_awaitable);
	}

	tcp::acceptor acceptor;
};

struct harness : araya_test::plugin_harness {
	araya::component_spec web_spec(araya::plugin_config cfg = {}) {
		return spec(&araya::web::plugin_descriptor(), std::move(cfg));
	}
};

} // namespace

TEST_CASE("web fetch retrieves a URL and strips HTML") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		test_server server(rt.root_context().executor());
		server.on_request = [](http::request<http::string_body> const&) {
			http::response<http::string_body> response{http::status::ok, 11};
			response.set(http::field::content_type, "text/html; charset=utf-8");
			response.body() = "<html><head><style>x{}</style></head><body><h1>Hello &amp; "
							  "welcome</h1><script>bad()</script><p>Body text.</p></body></html>";
			return response;
		};
		boost::asio::co_spawn(h.io.get_executor(), server.serve(), boost::asio::detached);

		co_await rt.mount(h.web_spec());
		co_await rt.wait_idle();
		auto web = rt.root_context().require<araya::web::web_service>(araya::web::web_key).shared();

		auto result = co_await web->fetch(
			araya::web::fetch_request{"http://127.0.0.1:" + std::to_string(server.port()) + "/page"}, {});
		CHECK(result.status == 200);
		CHECK(result.text.find("Hello & welcome") != std::string::npos);
		CHECK(result.text.find("Body text.") != std::string::npos);
		CHECK(result.text.find("bad()") == std::string::npos);
		CHECK(result.text.find("<h1>") == std::string::npos);
	});
}

TEST_CASE("web search returns provider sources and reports absence") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		test_server server(rt.root_context().executor());
		server.on_request = [](http::request<http::string_body> const& request) {
			CHECK(request.target().starts_with("/search?q="));
			http::response<http::string_body> response{http::status::ok, 11};
			response.set(http::field::content_type, "application/json");
			response.body() = R"({"results":[{"title":"First","url":"https://example.com/a","snippet":"alpha"},)"
							  R"({"title":"Second","url":"https://example.com/b","snippet":"beta"}]})";
			return response;
		};
		boost::asio::co_spawn(h.io.get_executor(), server.serve(), boost::asio::detached);

		co_await rt.mount(h.web_spec({
			{"search_endpoint", "http://127.0.0.1:" + std::to_string(server.port()) + "/search"},
		}));
		co_await rt.wait_idle();
		auto web = rt.root_context().require<araya::web::web_service>(araya::web::web_key).shared();
		REQUIRE(web->has_search());

		auto result = co_await web->search(araya::web::search_request{"araya", 8}, {});
		REQUIRE(result.sources.size() == 2);
		CHECK(result.sources[0].title == "First");
		CHECK(result.sources[0].url == "https://example.com/a");
		CHECK(result.sources[0].snippet == "alpha");
	});
}

TEST_CASE("web search without a provider is unavailable") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.web_spec());
		co_await rt.wait_idle();
		auto web = rt.root_context().require<araya::web::web_service>(araya::web::web_key).shared();
		CHECK_FALSE(web->has_search());
		CHECK_THROWS_AS(co_await web->search(araya::web::search_request{"x", 8}, {}), std::runtime_error);
	});
}
