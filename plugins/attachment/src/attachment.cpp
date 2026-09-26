#include "araya/attachment/attachment.hpp"

#include "araya/config.hpp"
#include "araya/plugin_context.hpp"

#include <openssl/sha.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace araya::attachment {
namespace {

namespace fs = std::filesystem;

constexpr araya::config_key<std::string> root_key{"root"};

struct image_info {
	std::string media_type;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
};

std::uint32_t be32(std::string const& data, std::size_t offset) {
	auto const byte = [&](std::size_t index) {
		return static_cast<std::uint32_t>(static_cast<unsigned char>(data[index]));
	};
	return (byte(offset) << 24) | (byte(offset + 1) << 16) | (byte(offset + 2) << 8) | byte(offset + 3);
}

std::uint32_t be16(std::string const& data, std::size_t offset) {
	auto const byte = [&](std::size_t index) {
		return static_cast<std::uint32_t>(static_cast<unsigned char>(data[index]));
	};
	return (byte(offset) << 8) | byte(offset + 1);
}

std::uint32_t le16(std::string const& data, std::size_t offset) {
	auto const byte = [&](std::size_t index) {
		return static_cast<std::uint32_t>(static_cast<unsigned char>(data[index]));
	};
	return byte(offset) | (byte(offset + 1) << 8);
}

// Identify PNG/GIF/JPEG from the file signature and read the intrinsic
// dimensions. WebP and other formats are not accepted in v1.
std::optional<image_info> sniff(std::string const& data) {
	if (data.size() >= 24 && static_cast<unsigned char>(data[0]) == 0x89 &&
		data.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0)
		return image_info{"image/png", be32(data, 16), be32(data, 20)};
	if (data.size() >= 10 && (data.compare(0, 6, "GIF87a") == 0 || data.compare(0, 6, "GIF89a") == 0))
		return image_info{"image/gif", le16(data, 6), le16(data, 8)};
	if (data.size() >= 4 && static_cast<unsigned char>(data[0]) == 0xFF &&
		static_cast<unsigned char>(data[1]) == 0xD8 && static_cast<unsigned char>(data[2]) == 0xFF) {
		std::size_t cursor = 2;
		while (cursor + 9 < data.size()) {
			if (static_cast<unsigned char>(data[cursor]) != 0xFF) {
				++cursor;
				continue;
			}
			auto const marker = static_cast<unsigned char>(data[cursor + 1]);
			if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
				cursor += 2;
				continue;
			}
			if (marker == 0xD9 || marker == 0xDA)
				break;
			if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
				std::uint32_t const height = be16(data, cursor + 5);
				std::uint32_t const width = be16(data, cursor + 7);
				return image_info{"image/jpeg", width, height};
			}
			std::size_t const length =
				(static_cast<unsigned char>(data[cursor + 2]) << 8) | static_cast<unsigned char>(data[cursor + 3]);
			cursor += 2 + length;
		}
	}
	return std::nullopt;
}

std::string sha256_hex(std::string const& data) {
	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<unsigned char const*>(data.data()), data.size(), digest);
	static char const* hex = "0123456789abcdef";
	std::string out;
	out.reserve(SHA256_DIGEST_LENGTH * 2);
	for (unsigned char const byte : digest) {
		out.push_back(hex[byte >> 4]);
		out.push_back(hex[byte & 0x0F]);
	}
	return out;
}

class local_attachments : public attachment_service {
public:
	explicit local_attachments(fs::path root)
		: root_(std::move(root)) {}

	image_ref save_image(std::string const& data, std::string const& media_type, std::string const& name) override {
		auto info = sniff(data);
		if (!info)
			throw std::runtime_error("unsupported image format (PNG/JPEG/GIF only)");
		if (!media_type.empty() && media_type != info->media_type)
			throw std::runtime_error(
				"declared media type " + media_type + " does not match the file signature " + info->media_type);
		std::error_code ec;
		fs::create_directories(root_, ec);
		if (ec)
			throw std::runtime_error("cannot create attachment root: " + ec.message());
		auto const id = sha256_hex(data);
		auto const path = root_ / id;
		if (!fs::exists(path, ec)) {
			std::ofstream stream(path, std::ios::binary);
			if (!stream)
				throw std::runtime_error("cannot write attachment " + id);
			stream.write(data.data(), static_cast<std::streamsize>(data.size()));
		}
		return image_ref{id, info->media_type, data.size(), info->width, info->height, name};
	}

	std::optional<std::string> load(std::string const& attachment_id) const override {
		std::ifstream stream(root_ / attachment_id, std::ios::binary);
		if (!stream)
			return std::nullopt;
		std::string data;
		stream.seekg(0, std::ios::end);
		data.resize(static_cast<std::size_t>(stream.tellg()));
		stream.seekg(0, std::ios::beg);
		stream.read(data.data(), static_cast<std::streamsize>(data.size()));
		return data;
	}

private:
	fs::path root_;
};

std::unique_ptr<araya::plugin> make_attachment(araya::plugin_config const& config) {
	struct attachment_plugin : araya::plugin {
		explicit attachment_plugin(araya::plugin_config const& cfg) {
			araya::plugin_config_view const view(cfg);
			if (auto value = view.try_get(root_key))
				root = *value;
		}

		araya::task<void> apply(araya::plugin_context& ctx) override {
			std::shared_ptr<attachment_service> impl = std::make_shared<local_attachments>(std::filesystem::path(root));
			ctx.provide(attachments_key, std::move(impl));
			co_return;
		}

		std::string root = "araya-attachments";
	};
	return std::make_unique<attachment_plugin>(config);
}

static constexpr std::span<araya::dependency_spec const> g_deps{};
static const araya::provision_spec g_provs[]{{araya::service_id{"attachments", 1}}};
static const araya::plugin_descriptor g_descriptor{"attachment", g_deps, g_provs, &make_attachment};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::attachment
