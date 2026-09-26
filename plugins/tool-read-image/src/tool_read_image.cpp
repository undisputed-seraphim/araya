#include "araya/tool-read-image/tool_read_image.hpp"

#include "araya/attachment/attachment.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin_context.hpp"
#include "araya/session/store.hpp"
#include "araya/tools/tools.hpp"
#include "araya/util/json.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace araya::tool_read_image {
namespace {

namespace fs = std::filesystem;

using araya::attachment::attachment_service;
using araya::attachment::attachments_key;
using araya::session::session_id;
using araya::session::session_store;
using araya::session::sessions_key;
using araya::tools::tool_context;
using araya::tools::tool_definition;
using araya::tools::tool_result;
using araya::tools::tools_key;
using araya::tools::tools_service;

constexpr std::size_t max_image_bytes = 10 * 1024 * 1024;

tool_result text_result(std::string text, bool is_error = false) {
	return tool_result{boost::json::array{{{"type", "text"}, {"text", std::move(text)}}}, is_error};
}

tool_result error_result(std::string text) { return text_result(std::move(text), true); }

std::string base64_encode(std::string const& data) {
	static char const* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve(((data.size() + 2) / 3) * 4);
	std::size_t index = 0;
	while (index + 2 < data.size()) {
		auto const chunk = (static_cast<std::uint32_t>(static_cast<unsigned char>(data[index])) << 16) |
						   (static_cast<std::uint32_t>(static_cast<unsigned char>(data[index + 1])) << 8) |
						   static_cast<unsigned char>(data[index + 2]);
		out.push_back(table[(chunk >> 18) & 0x3F]);
		out.push_back(table[(chunk >> 12) & 0x3F]);
		out.push_back(table[(chunk >> 6) & 0x3F]);
		out.push_back(table[chunk & 0x3F]);
		index += 3;
	}
	if (index < data.size()) {
		std::uint32_t chunk = static_cast<std::uint32_t>(static_cast<unsigned char>(data[index])) << 16;
		bool const two = index + 1 < data.size();
		if (two)
			chunk |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[index + 1])) << 8;
		out.push_back(table[(chunk >> 18) & 0x3F]);
		out.push_back(table[(chunk >> 12) & 0x3F]);
		out.push_back(two ? table[(chunk >> 6) & 0x3F] : '=');
		out.push_back('=');
	}
	return out;
}

// The provider/model of the session's most recent request header, if any.
std::optional<std::pair<std::string, std::string>> latest_route(araya::session::session const& session) {
	for (auto it = session.log().rbegin(); it != session.log().rend(); ++it) {
		if (it->type != "request/header")
			continue;
		auto const* object = it->data.if_object();
		auto const* header = object ? object->if_contains("header") : nullptr;
		auto const* header_object = header ? header->if_object() : nullptr;
		if (!header_object)
			continue;
		auto const provider = araya::util::json::get_string(*header_object, "provider");
		auto const model = araya::util::json::get_string(*header_object, "model");
		if (!provider.empty() && !model.empty())
			return std::make_pair(provider, model);
	}
	return std::nullopt;
}

std::string render_envelope(std::string const& path, araya::attachment::image_ref const& image) {
	return "<path>" + path + "</path>\n<type>image</type>\n<content>\n" + image.media_type + " image, " +
		   std::to_string(image.width) + "x" + std::to_string(image.height) + " px, " + std::to_string(image.bytes) +
		   " bytes\n</content>";
}

araya::task<tool_result> handle_read_image(
	std::shared_ptr<attachment_service> attachments,
	std::shared_ptr<araya::llm::llm_service> llm,
	std::shared_ptr<session_store> store,
	tool_context const& ctx) {
	auto const* args = ctx.arguments.if_object();
	auto file_path = args ? araya::util::json::get_string(*args, "file_path") : std::string{};
	if (file_path.empty())
		co_return error_result("Error: read_image requires a non-empty 'file_path'");

	std::string cwd = fs::current_path().string();
	auto session = ctx.session.empty() ? nullptr : store->get(session_id{ctx.session});
	if (session && session->header().cwd)
		cwd = *session->header().cwd;
	fs::path const target = fs::path(file_path).is_absolute() ? fs::path(file_path) : fs::path(cwd) / file_path;

	// Route gate: only an image-capable model can use the result.
	if (!session)
		co_return error_result("Error: read_image requires an entered session");
	auto route = latest_route(*session);
	if (!route)
		co_return error_result("Error: cannot read \"" + file_path + "\" as an image: the model route is unresolved");
	auto model = llm->resolve_model(route->first, route->second);
	if (!model || !model->supports_image)
		co_return error_result(
			"Error: cannot read \"" + file_path + "\" as an image: model \"" + route->second +
			"\" does not declare image input; switch to an image-capable model to read images");

	std::error_code ec;
	if (!fs::is_regular_file(target, ec))
		co_return error_result("Error: cannot read \"" + file_path + "\": not a regular file");
	std::ifstream stream(target, std::ios::binary);
	if (!stream)
		co_return error_result("Error: cannot read \"" + file_path + "\"");
	std::string data;
	stream.seekg(0, std::ios::end);
	auto const size = static_cast<std::size_t>(stream.tellg());
	if (size > max_image_bytes)
		co_return error_result(
			"Error: cannot read \"" + file_path + "\": file exceeds the " + std::to_string(max_image_bytes) +
			"-byte limit");
	data.resize(size);
	stream.seekg(0, std::ios::beg);
	stream.read(data.data(), static_cast<std::streamsize>(data.size()));

	araya::attachment::image_ref image;
	try {
		image = attachments->save_image(data, "", target.filename().string());
	} catch (std::exception const& e) {
		co_return error_result("Error: cannot read \"" + file_path + "\": " + e.what());
	}

	std::string const encoded = base64_encode(data);
	boost::json::array content;
	content.emplace_back(boost::json::object{{"type", "text"}, {"text", render_envelope(file_path, image)}});
	content.emplace_back(boost::json::object{
		{"type", "image"},
		{"attachment_id", image.attachment_id},
		{"media_type", image.media_type},
		{"bytes", image.bytes},
		{"width", image.width},
		{"height", image.height},
		{"data", encoded}});
	co_return tool_result{std::move(content), false};
}

boost::json::value read_image_schema() {
	boost::json::object file_path;
	file_path["type"] = "string";
	file_path["description"] = "Path to the image file, resolved against the session workspace.";
	boost::json::object properties;
	properties["file_path"] = std::move(file_path);
	boost::json::object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = boost::json::array{"file_path"};
	return schema;
}

std::unique_ptr<araya::plugin> make_tool_read_image(araya::plugin_config const& config) {
	(void)config;
	struct tool_read_image_plugin : araya::plugin {
		araya::task<void> apply(araya::plugin_context& ctx) override {
			auto attachments = ctx.require<attachment_service>(attachments_key).shared();
			auto llm = ctx.require<araya::llm::llm_service>(araya::llm::llm_key).shared();
			auto store = ctx.require<session_store>(sessions_key).shared();
			auto tools = ctx.require<tools_service>(tools_key).shared();
			tools->register_tool(
				ctx,
				tool_definition{
					"read_image",
					"Read a PNG/JPEG/GIF file and return the image itself. The format is detected from the file "
					"content, so an extension-less path is accepted. Requires the current model to accept image "
					"input.",
					read_image_schema()},
				[attachments, llm, store](tool_context const& call) {
					return handle_read_image(attachments, llm, store, call);
				});
			co_return;
		}
	};
	return std::make_unique<tool_read_image_plugin>();
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"attachments", 1}, true, {}},
	{araya::service_id{"llm", 1}, true, {}},
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"tools", 1}, true, {}},
};
static constexpr std::span<araya::provision_spec const> g_provs{};
static const araya::plugin_descriptor g_descriptor{"tool-read-image", g_deps, g_provs, &make_tool_read_image};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::tool_read_image
