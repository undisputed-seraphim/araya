#include <catch2/catch_test_macros.hpp>

#include "config.hpp"
#include "connection.hpp"
#include "jsonrpc.hpp"
#include "naming.hpp"

#include "araya/task.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace araya::mcp;

araya::plugin_config config_with_json(std::string json) {
	araya::plugin_config config;
	config["config"] = std::move(json);
	return config;
}

template <class Fn>
void run_coro(boost::asio::io_context& io, Fn fn) {
	std::exception_ptr error;
	boost::asio::co_spawn(
		io,
		[&]() -> araya::task<void> {
			try {
				co_await fn();
			} catch (...) {
				error = std::current_exception();
			}
		},
		boost::asio::detached);
	io.run();
	if (error)
		std::rethrow_exception(error);
}

// A minimal stdio MCP fixture: initialize (with tools capability and
// instructions), tools/list with one `echo` tool, and tools/call.
std::string write_fixture() {
	static constexpr char const* script = R"PY(
import sys, json
tools = [{
    "name": "echo", "description": "Echo text.",
    "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}},
                    "required": ["text"]}}]
def send(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()
for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    msg = json.loads(line)
    method = msg.get("method")
    mid = msg.get("id")
    if method == "initialize":
        send({"jsonrpc": "2.0", "id": mid, "result": {
            "protocolVersion": "2025-06-18",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "fixture", "version": "1"},
            "instructions": "FIXTURE INSTRUCTIONS"}})
    elif method == "notifications/initialized":
        pass
    elif method == "tools/list":
        send({"jsonrpc": "2.0", "id": mid, "result": {"tools": tools}})
    elif method == "tools/call":
        args = msg.get("params", {}).get("arguments", {})
        name = msg.get("params", {}).get("name", "")
        if name == "bump":
            tools.append({"name": "echo2", "description": "Second.",
                          "inputSchema": {"type": "object", "properties": {}}})
            send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
            send({"jsonrpc": "2.0", "id": mid, "result": {"content": [{"type": "text", "text": "bumped"}]}})
        else:
            send({"jsonrpc": "2.0", "id": mid, "result": {
                "content": [{"type": "text", "text": "echo: " + str(args.get("text", ""))}]}})
    elif mid is not None:
        send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found"}})
)PY";
	std::filesystem::path const path =
		std::filesystem::temp_directory_path() / ("araya-mcp-fixture-" + std::to_string(::getpid()) + ".py");
	std::ofstream file(path);
	file << script;
	file.close();
	return path.string();
}

} // namespace

TEST_CASE("mcp public tool names are stable and server-qualified") {
	REQUIRE(public_tool_name("github", "create_issue") == "mcp__github__create_issue");
	REQUIRE(public_tool_name("a-b_c", "x.y") != public_tool_name("a-b_c", "x-0"));
	// A clean name that fits is verbatim; a lossy one carries a hash suffix.
	std::string const clean = public_tool_name("s", "tool");
	REQUIRE(clean == "mcp__s__tool");
	std::string const long_name = public_tool_name("server", std::string(80, 'x'));
	REQUIRE(long_name.size() <= 64);
	REQUIRE(long_name != public_tool_name("server", std::string(79, 'x') + "y"));
}

TEST_CASE("mcp jsonrpc decode classifies responses and notifications") {
	auto request = jsonrpc::make_request(7, "tools/list", boost::json::object{});
	auto parsed = jsonrpc::decode(boost::json::serialize(request));
	REQUIRE(parsed.has_value());
	REQUIRE(parsed->has_id);
	REQUIRE(parsed->id == "7");
	REQUIRE(parsed->method == "tools/list");

	auto response = jsonrpc::decode(R"({"jsonrpc":"2.0","id":7,"result":{"tools":[]}})");
	REQUIRE(response.has_value());
	REQUIRE(response->has_id);
	REQUIRE_FALSE(response->has_method);
	REQUIRE_FALSE(response->is_error);

	auto notification = jsonrpc::decode(R"({"jsonrpc":"2.0","method":"notifications/tools/list_changed"})");
	REQUIRE(notification.has_value());
	REQUIRE_FALSE(notification->has_id);
	REQUIRE(notification->has_method);

	REQUIRE_FALSE(jsonrpc::decode("not json").has_value());
}

TEST_CASE("mcp config parses servers and expands variables") {
	::setenv("MCP_TEST_TOKEN", "s3cret", 1);
	auto config = config_with_json(R"({
		"requestTimeoutMs": 1234,
		"reconnect": { "maxAttempts": 3 },
		"servers": {
			"alpha": {
				"type": "local",
				"command": ["server", "--flag"],
				"environment": { "TOKEN": "${MCP_TEST_TOKEN}" }
			}
		}
	})");
	auto parsed = parse_config(config);
	REQUIRE(parsed.request_timeout_ms == 1234);
	REQUIRE(parsed.reconnect.max_attempts == 3);
	REQUIRE(parsed.servers.size() == 1);
	REQUIRE(parsed.servers[0].name == "alpha");
	REQUIRE(parsed.servers[0].command.size() == 2);
	REQUIRE(parsed.servers[0].command[1] == "--flag");
	REQUIRE(parsed.servers[0].environment.size() == 1);
	REQUIRE(parsed.servers[0].environment[0].second == "s3cret");
	::unsetenv("MCP_TEST_TOKEN");
}

TEST_CASE("mcp config rejects malformed servers") {
	REQUIRE_THROWS_AS(
		parse_config(config_with_json(R"({"servers":{"bad name":{"type":"local","command":["x"]}}})")),
		std::invalid_argument);
	REQUIRE_THROWS_AS(parse_config(config_with_json(R"({"servers":{"s":{"type":"local"}}})")), std::invalid_argument);
	REQUIRE_THROWS_AS(parse_config(config_with_json(R"({"servers":{"s":{"type":"remote"}}})")), std::invalid_argument);
	REQUIRE_THROWS_AS(parse_config(config_with_json(R"({"servers":{"s":{"type":"warp"}}})")), std::invalid_argument);
}

TEST_CASE("mcp env scrub drops credential-shaped and ARAYA_ names") {
	REQUIRE(sensitive_env_name("GITHUB_TOKEN"));
	REQUIRE(sensitive_env_name("MY_SECRET"));
	REQUIRE(sensitive_env_name("db_password"));
	REQUIRE(sensitive_env_name("API_KEY"));
	REQUIRE(sensitive_env_name("ARAYA_STATE_DIR"));
	REQUIRE_FALSE(sensitive_env_name("PATH"));
	REQUIRE_FALSE(sensitive_env_name("HOME"));
	REQUIRE_FALSE(sensitive_env_name("LANG"));
	// The scrub is a substring match, so "MONKEY" is (deliberately) scrubbed.
	REQUIRE(sensitive_env_name("MONKEY"));
}

TEST_CASE("mcp client connects, discovers, and calls a stdio server") {
	std::string const fixture = write_fixture();
	boost::asio::io_context io;

	server_config config;
	config.name = "fixture";
	config.command = {"python3", fixture};
	config.timeout_ms = 10'000;

	std::vector<mcp_tool> discovered;
	std::string instructions;
	connection_observer observer;
	observer.on_tools = [&](std::vector<mcp_tool> const& tools) { discovered = tools; };
	observer.on_instructions = [&](std::string const& text) { instructions = text; };

	auto connection = std::make_shared<server_connection>(io.get_executor(), config, observer);
	std::exception_ptr ready_error;
	boost::json::value call_result;

	run_coro(io, [&]() -> araya::task<void> {
		connection->start();
		ready_error = co_await connection->ready();
		if (!ready_error) {
			boost::json::object params;
			params["name"] = "echo";
			params["arguments"] = boost::json::object{{"text", "hello"}};
			call_result = co_await connection->request("tools/call", std::move(params), {});
		}
		connection->stop();
	});

	if (ready_error) {
		std::string message = "unknown";
		try {
			std::rethrow_exception(ready_error);
		} catch (std::exception const& e) {
			message = e.what();
		} catch (...) {
		}
		INFO("ready error: " + message);
	}
	REQUIRE_FALSE(ready_error);
	REQUIRE(discovered.size() == 1);
	REQUIRE(discovered[0].name == "echo");
	REQUIRE(instructions == "FIXTURE INSTRUCTIONS");
	REQUIRE(boost::json::serialize(call_result).find("echo: hello") != std::string::npos);
	std::filesystem::remove(fixture);
}

TEST_CASE("mcp client surfaces a server connection failure") {
	boost::asio::io_context io;
	server_config config;
	config.name = "missing";
	config.command = {"/nonexistent/mcp-server-binary"};
	config.reconnect.enabled = false;

	auto connection = std::make_shared<server_connection>(io.get_executor(), config, connection_observer{});
	std::exception_ptr ready_error;
	run_coro(io, [&]() -> araya::task<void> {
		connection->start();
		ready_error = co_await connection->ready();
		connection->stop();
	});
	REQUIRE(ready_error);
}

TEST_CASE("mcp client re-discovers when the server tool list changes") {
	std::string const fixture = write_fixture();
	boost::asio::io_context io;
	server_config config;
	config.name = "fixture";
	config.command = {"python3", fixture};
	config.timeout_ms = 10'000;

	std::vector<std::vector<mcp_tool>> generations;
	connection_observer observer;
	observer.on_tools = [&](std::vector<mcp_tool> const& tools) { generations.push_back(tools); };
	auto connection = std::make_shared<server_connection>(io.get_executor(), config, observer);

	std::exception_ptr ready_error;
	run_coro(io, [&]() -> araya::task<void> {
		connection->start();
		ready_error = co_await connection->ready();
		if (!ready_error) {
			boost::json::object params;
			params["name"] = "bump";
			params["arguments"] = boost::json::object{};
			(void)co_await connection->request("tools/call", std::move(params), {});
			boost::asio::steady_timer timer(io);
			timer.expires_after(std::chrono::milliseconds(500));
			co_await timer.async_wait(boost::asio::use_awaitable);
		}
		connection->stop();
	});

	REQUIRE_FALSE(ready_error);
	REQUIRE(generations.size() >= 2);
	REQUIRE(generations.back().size() == 2);
	std::filesystem::remove(fixture);
}
