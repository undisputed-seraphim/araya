#include <catch2/catch_test_macros.hpp>

#include "araya/lsp-stdio/lsp_stdio.hpp"
#include "araya/lsp/lsp.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"

#include "support/plugin_harness.hpp"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

namespace {

using namespace araya::lsp;

std::string write_fixture(std::filesystem::path const& dir) {
	std::filesystem::path const path = dir / "fixture.py";
	std::ofstream file(path);
	file << R"PY(
import sys, json
def read_msg():
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        line = line.strip()
        if line == b'':
            break
        k, _, v = line.partition(b':')
        headers[k.strip().lower()] = v.strip()
    n = int(headers[b'content-length'])
    return json.loads(sys.stdin.buffer.read(n))
def send(obj):
    body = json.dumps(obj).encode('utf-8')
    sys.stdout.buffer.write(b'Content-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body)
    sys.stdout.buffer.flush()
while True:
    msg = read_msg()
    if msg is None:
        break
    method = msg.get('method'); mid = msg.get('id')
    if method == 'initialize':
        send({'jsonrpc':'2.0','id':mid,'result':{'capabilities':{
            'positionEncoding':'utf-16','textDocumentSync':1,
            'definitionProvider':True,'referencesProvider':True,
            'implementationProvider':True,'hoverProvider':True}}})
    elif method == 'initialized':
        pass
    elif method in ('textDocument/didOpen','textDocument/didClose'):
        pass
    elif method == 'textDocument/definition':
        send({'jsonrpc':'2.0','id':mid,'result':[
            {'uri':'file:///ws/a.ts','range':{'start':{'line':0,'character':0},'end':{'line':0,'character':1}}}]})
    elif method == 'textDocument/references':
        send({'jsonrpc':'2.0','id':mid,'result':[
            {'uri':'file:///ws/a.ts','range':{'start':{'line':1,'character':2},'end':{'line':1,'character':3}}}]})
    elif method == 'textDocument/implementation':
        send({'jsonrpc':'2.0','id':mid,'result':[
            {'targetUri':'file:///ws/impl.ts','targetSelectionRange':{
                'start':{'line':2,'character':0},'end':{'line':2,'character':1}}}]})
    elif method == 'textDocument/hover':
        send({'jsonrpc':'2.0','id':mid,'result':{
            'contents':{'kind':'markdown','value':'Hover docs'},
            'range':{'start':{'line':0,'character':0},'end':{'line':0,'character':1}}}})
    elif method == 'shutdown':
        send({'jsonrpc':'2.0','id':mid,'result':None})
    elif method == 'exit':
        break
    elif mid is not None:
        send({'jsonrpc':'2.0','id':mid,'error':{'code':-32601,'message':'not found'}})
)PY";
	file.close();
	return path.string();
}

struct harness : araya_test::plugin_harness {
	araya::component_spec lsp_spec() { return spec(&araya::lsp::plugin_descriptor()); }
	araya::component_spec stdio_spec(araya::plugin_config cfg) {
		return spec(&araya::lsp_stdio::plugin_descriptor(), std::move(cfg));
	}
};

} // namespace

TEST_CASE("lsp-stdio navigates through a fixture server") {
	std::filesystem::path const dir =
		std::filesystem::temp_directory_path() / ("araya-lsp-" + std::to_string(::getpid()));
	std::filesystem::create_directories(dir);
	{
		std::ofstream source(dir / "a.ts");
		source << "const x = 1;\n";
	}
	std::string const fixture = write_fixture(dir);

	boost::json::object server;
	server["command"] = "python3";
	server["args"] = boost::json::array{fixture};
	server["extensionToLanguage"] = boost::json::object{{".ts", "typescript"}};
	boost::json::object servers;
	servers["fixture"] = std::move(server);
	boost::json::object root;
	root["servers"] = std::move(servers);
	std::string const config = boost::json::serialize(root);

	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.lsp_spec());
		co_await rt.mount(h.stdio_spec({{"config", config}}));
		co_await rt.wait_idle();
		auto const lsp = rt.root_context().require<lsp_service>(lsp_key).shared();

		lsp_query_request definition;
		definition.operation = lsp_operation::go_to_definition;
		definition.file_path = "a.ts";
		definition.position = {0, 0};
		definition.workspace_root = dir.string();
		auto const located = co_await lsp->query(definition, {});
		REQUIRE(located.type == lsp_query_result::kind::locations);
		REQUIRE(located.locations.size() == 1);
		CHECK(located.locations[0].uri == "file:///ws/a.ts");

		lsp_query_request hover;
		hover.operation = lsp_operation::hover;
		hover.file_path = "a.ts";
		hover.position = {0, 0};
		hover.workspace_root = dir.string();
		auto const hovered = co_await lsp->query(hover, {});
		REQUIRE(hovered.type == lsp_query_result::kind::hover);
		REQUIRE(hovered.hover.has_value());
		CHECK(hovered.hover->contents == "Hover docs");

		// The pooled server keeps a detached reader coroutine alive; stop the
		// harness loop once the assertions are done.
		h.io.stop();
	});

	std::filesystem::remove_all(dir);
}
