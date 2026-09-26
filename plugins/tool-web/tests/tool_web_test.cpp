#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tool-web/tool_web.hpp"
#include "araya/tools/tools.hpp"
#include "araya/web/web.hpp"

#include "support/plugin_harness.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using namespace araya::tools;

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
	araya::component_spec prompt_spec() {
		return spec(&araya::system_prompt::plugin_descriptor(), {{"include_harness_identity", "false"}});
	}
	araya::component_spec tools_spec() { return spec(&araya::tools::plugin_descriptor()); }
	araya::component_spec web_spec(araya::plugin_config cfg = {}) {
		return spec(&araya::web::plugin_descriptor(), std::move(cfg));
	}
	araya::component_spec tool_web_spec() { return spec(&araya::tool_web::plugin_descriptor()); }
};

struct rig {
	std::shared_ptr<tools_service> tools;

	araya::task<void> mount(araya::runtime& rt, harness& h, araya::plugin_config web_config = {}) {
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.web_spec(std::move(web_config)));
		co_await rt.mount(h.tool_web_spec());
		co_await rt.wait_idle();
		tools = rt.root_context().require<tools_service>(tools_key).shared();
	}

	araya::task<std::optional<tool_result>> call(std::string name, boost::json::object args = {}) {
		std::string const tool = name;
		co_return co_await tools->invoke(
			tool, tool_context{.call_id = "c", .name = tool, .arguments = std::move(args)});
	}
};

std::string text_of(tool_result const& result) {
	auto const* arr = result.content.if_array();
	if (!arr || arr->empty())
		return {};
	auto const* object = arr->front().if_object();
	if (!object)
		return {};
	auto it = object->find("text");
	return it != object->end() && it->value().is_string() ? std::string(it->value().as_string()) : std::string{};
}

} // namespace

TEST_CASE("web_fetch retrieves and renders page content") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		test_server server(rt.root_context().executor());
		server.on_request = [](http::request<http::string_body> const&) {
			http::response<http::string_body> response{http::status::ok, 11};
			response.set(http::field::content_type, "text/html");
			response.body() = "<html><body><p>Hello web.</p></body></html>";
			return response;
		};
		boost::asio::co_spawn(h.io.get_executor(), server.serve(), boost::asio::detached);

		rig r;
		co_await r.mount(rt, h);
		auto out =
			co_await r.call("web_fetch", {{"url", "http://127.0.0.1:" + std::to_string(server.port()) + "/page"}});
		REQUIRE(out.has_value());
		CHECK_FALSE(out->is_error);
		CHECK(text_of(*out).find("(HTTP 200)") != std::string::npos);
		CHECK(text_of(*out).find("Hello web.") != std::string::npos);
		CHECK(text_of(*out).find("untrusted") != std::string::npos);
	});
}

TEST_CASE("web_search merges provider sources and recommends citing") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		test_server server(rt.root_context().executor());
		server.on_request = [](http::request<http::string_body> const&) {
			http::response<http::string_body> response{http::status::ok, 11};
			response.set(http::field::content_type, "application/json");
			response.body() = R"({"results":[{"title":"A","url":"https://e.com/a","snippet":"alpha"}]})";
			return response;
		};
		boost::asio::co_spawn(h.io.get_executor(), server.serve(), boost::asio::detached);

		rig r;
		co_await r.mount(rt, h, {{"search_endpoint", "http://127.0.0.1:" + std::to_string(server.port()) + "/search"}});
		auto out = co_await r.call("web_search", {{"queries", boost::json::array{"araya"}}});
		REQUIRE(out.has_value());
		CHECK_FALSE(out->is_error);
		CHECK(text_of(*out).find("[A](https://e.com/a)") != std::string::npos);
		CHECK(text_of(*out).find("alpha") != std::string::npos);
		CHECK(text_of(*out).find("Cite the relevant URLs") != std::string::npos);
	});
}

TEST_CASE("web_search is not registered without a provider") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		CHECK(r.tools->find("web_fetch", std::nullopt).has_value());
		CHECK_FALSE(r.tools->find("web_search", std::nullopt).has_value());
	});
}
