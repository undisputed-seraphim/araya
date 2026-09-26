#include <catch2/catch_test_macros.hpp>

#include "araya/attachment/attachment.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"

#include "support/plugin_harness.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;

// A minimal PNG signature + IHDR header carrying 3x2 dimensions (the store
// sniffs the signature and reads the IHDR fields; the pixel data is ignored).
std::string png_3x2() {
	std::string data("\x89PNG\r\n\x1a\n", 8);
	data += "\x00\x00\x00\x0dIHDR";
	data += '\x00';
	data += '\x00';
	data += '\x00';
	data += '\x03'; // width = 3
	data += '\x00';
	data += '\x00';
	data += '\x00';
	data += '\x02'; // height = 2
	data += "padding";
	return data;
}

struct harness : araya_test::plugin_harness {
	araya::component_spec attachments_spec(std::string const& root) {
		return spec(&araya::attachment::plugin_descriptor(), {{"root", root}});
	}
};

struct workspace {
	fs::path root;

	workspace() {
		std::error_code ec;
		auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		root = fs::temp_directory_path(ec) / ("araya-attach-" + std::to_string(stamp));
		fs::remove_all(root, ec);
	}

	~workspace() {
		std::error_code ec;
		fs::remove_all(root, ec);
	}
};

} // namespace

TEST_CASE("the attachment store validates, content-addresses, and loads images") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		workspace ws;
		co_await rt.mount(h.attachments_spec(ws.root.string()));
		co_await rt.wait_idle();
		auto attachments = rt.root_context()
							   .require<araya::attachment::attachment_service>(araya::attachment::attachments_key)
							   .shared();

		auto const png = png_3x2();
		auto ref = attachments->save_image(png, "image/png", "tiny.png");
		CHECK(ref.media_type == "image/png");
		CHECK(ref.width == 3);
		CHECK(ref.height == 2);
		CHECK(ref.bytes == png.size());
		CHECK(ref.name == "tiny.png");
		CHECK(ref.attachment_id.size() == 64);

		auto loaded = attachments->load(ref.attachment_id);
		REQUIRE(loaded.has_value());
		CHECK(*loaded == png);

		// Saving the same bytes again reuses the same content address.
		auto again = attachments->save_image(png, "", "copy.png");
		CHECK(again.attachment_id == ref.attachment_id);
		CHECK(again.media_type == "image/png");

		CHECK_THROWS_AS(attachments->save_image(png, "image/jpeg", "x.jpg"), std::runtime_error);
		CHECK_THROWS_AS(attachments->save_image("not an image", "", "x"), std::runtime_error);
		CHECK_FALSE(attachments->load("nonexistent").has_value());
	});
}
