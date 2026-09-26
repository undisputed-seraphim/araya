#include <catch2/catch_test_macros.hpp>

#include "araya/attachment/attachment.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"
#include "araya/tool-read-image/tool_read_image.hpp"
#include "araya/tools/tools.hpp"

#include "support/plugin_harness.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace araya::tools;

std::string png_3x2() {
	std::string data("\x89PNG\r\n\x1a\n", 8);
	data += "\x00\x00\x00\x0dIHDR";
	data += '\x00';
	data += '\x00';
	data += '\x00';
	data += '\x03';
	data += '\x00';
	data += '\x00';
	data += '\x00';
	data += '\x02';
	data += "padding";
	return data;
}

struct fake_adapter : araya::llm::llm_adapter {
	araya::llm::model_info info;

	araya::task<void> stream(araya::llm::generate_options const&, araya::llm::chunk_sink const&) override { co_return; }
	std::vector<araya::llm::model_info> list_models(std::string_view) override { return {info}; }
	araya::llm::model_info resolve_model(std::string_view, std::string_view model) override {
		return model == info.model ? info : araya::llm::model_info{};
	}
};

struct harness : araya_test::plugin_harness {
	araya::component_spec llm_spec() { return spec(&araya::llm::plugin_descriptor()); }
	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }
	araya::component_spec tools_spec() { return spec(&araya::tools::plugin_descriptor()); }
	araya::component_spec attachments_spec(std::string const& root) {
		return spec(&araya::attachment::plugin_descriptor(), {{"root", root}});
	}
	araya::component_spec read_image_spec() { return spec(&araya::tool_read_image::plugin_descriptor()); }
};

struct workspace {
	fs::path root;

	workspace() {
		std::error_code ec;
		auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		root = fs::temp_directory_path(ec) / ("araya-read-image-" + std::to_string(stamp));
		fs::create_directories(root, ec);
	}

	~workspace() {
		std::error_code ec;
		fs::remove_all(root, ec);
	}
};

struct rig {
	std::shared_ptr<tools_service> tools;
	std::shared_ptr<araya::llm::llm_service> llm;
	std::shared_ptr<araya::session::session_store> store;
	std::string session = "s1";

	araya::task<void> mount(araya::runtime& rt, harness& h, workspace& ws, bool supports_image) {
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.attachments_spec((ws.root / "attachments").string()));
		co_await rt.mount(h.read_image_spec());
		co_await rt.wait_idle();
		auto root = rt.root_context();
		llm = root.require<araya::llm::llm_service>(araya::llm::llm_key).shared();
		auto adapter = std::make_shared<fake_adapter>();
		adapter->info.provider = "fake";
		adapter->info.model = supports_image ? "vision" : "text";
		adapter->info.supports_image = supports_image;
		llm->register_adapter({"fake"}, adapter, root);

		tools = root.require<tools_service>(tools_key).shared();
		store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		araya::session::create_session_options options;
		options.cwd = ws.root.string();
		auto session_ptr = store->create(root, araya::session::session_id{session}, options);
		session_ptr->append(
			"request/header",
			boost::json::object{{"header", boost::json::object{{"provider", "fake"}, {"model", adapter->info.model}}}});
	}

	araya::task<std::optional<tool_result>> call(std::string file_path) {
		std::string const tool = "read_image";
		co_return co_await tools->invoke(
			tool,
			tool_context{
				.call_id = "c",
				.name = tool,
				.session = session,
				.arguments = boost::json::object{{"file_path", std::move(file_path)}}});
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

TEST_CASE("read_image returns a text envelope and an image block") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		workspace ws;
		{
			std::ofstream stream(ws.root / "tiny.png", std::ios::binary);
			stream << png_3x2();
		}
		rig r;
		co_await r.mount(rt, h, ws, /*supports_image=*/true);

		auto out = co_await r.call("tiny.png");
		REQUIRE(out.has_value());
		CHECK_FALSE(out->is_error);
		auto const* blocks = out->content.if_array();
		REQUIRE(blocks != nullptr);
		REQUIRE(blocks->size() == 2);
		CHECK(text_of(*out).find("<type>image</type>") != std::string::npos);
		CHECK(text_of(*out).find("image/png image, 3x2 px") != std::string::npos);
		auto const* image = (*blocks)[1].if_object();
		REQUIRE(image != nullptr);
		CHECK(image->at("type").as_string() == "image");
		CHECK(image->at("media_type").as_string() == "image/png");
		CHECK_FALSE(image->at("data").as_string().empty());
	});
}

TEST_CASE("read_image refuses when the routed model is not image-capable") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		workspace ws;
		{
			std::ofstream stream(ws.root / "tiny.png", std::ios::binary);
			stream << png_3x2();
		}
		rig r;
		co_await r.mount(rt, h, ws, /*supports_image=*/false);

		auto out = co_await r.call("tiny.png");
		REQUIRE(out.has_value());
		CHECK(out->is_error);
		CHECK(text_of(*out).find("does not declare image input") != std::string::npos);
	});
}

TEST_CASE("read_image rejects non-image files") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		workspace ws;
		{
			std::ofstream stream(ws.root / "notes.txt", std::ios::binary);
			stream << "just text";
		}
		rig r;
		co_await r.mount(rt, h, ws, /*supports_image=*/true);
		auto out = co_await r.call("notes.txt");
		REQUIRE(out.has_value());
		CHECK(out->is_error);
	});
}
