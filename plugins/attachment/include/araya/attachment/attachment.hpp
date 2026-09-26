#pragma once

#include "araya/plugin.hpp"
#include "araya/service.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// The image-attachment subsystem: validate, content-address, and store image
// bytes on disk so a settled `read_image` result has a durable reference. A
// feature-replication of the deepseek-harness `@deepseek-ai/dsh-attachment` +
// `-local`, trimmed to the image path (no normalization/downscaling, no
// request-image encoding, no file attachments).
namespace araya::attachment {

// A durable reference to one stored image.
struct image_ref {
	std::string attachment_id; // sha256 hex of the stored bytes
	std::string media_type;
	std::size_t bytes = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::string name;
};

class attachment_service {
public:
	virtual ~attachment_service() = default;

	// Validate (content sniff + dimensions) and store one image. `media_type`
	// empty means "detect from the bytes". Throws std::runtime_error for
	// unsupported bytes or a declared/actual mismatch.
	virtual image_ref save_image(std::string const& data, std::string const& media_type, std::string const& name) = 0;

	// The stored bytes for an id, or nullopt when absent.
	virtual std::optional<std::string> load(std::string const& attachment_id) const = 0;
};

inline constexpr araya::service_key<attachment_service> attachments_key{"attachments", 1};

// The plugin descriptor: provides `attachments`; no dependencies.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::attachment
