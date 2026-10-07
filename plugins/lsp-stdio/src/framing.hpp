#pragma once

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// LSP base-protocol framing: `Content-Length`-delimited JSON-RPC over a byte
// stream. The encoder produces one framed message; the decoder buffers
// incoming bytes and yields complete message bodies, bounding the header and
// total message size.
namespace araya::lsp_stdio {

inline constexpr std::size_t max_header_bytes = 1 << 16;

inline std::string encode_message(boost::json::value const& message) {
	std::string const body = boost::json::serialize(message);
	return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

class message_decoder {
public:
	explicit message_decoder(std::size_t max_message_bytes)
		: max_message_bytes_(max_message_bytes) {}

	std::vector<boost::json::value> push(std::string_view chunk) {
		buffer_.append(chunk.data(), chunk.size());
		std::vector<boost::json::value> messages;
		for (;;) {
			auto step = next();
			if (!step.has_value())
				break;
			messages.push_back(std::move(*step));
		}
		return messages;
	}

private:
	std::optional<boost::json::value> next() {
		auto const separator = buffer_.find("\r\n\r\n");
		if (separator == std::string::npos) {
			if (buffer_.size() > max_header_bytes)
				throw std::runtime_error("LSP header exceeded the size limit without a terminator");
			return std::nullopt;
		}
		if (separator > max_header_bytes)
			throw std::runtime_error("LSP header exceeded the size limit");
		std::string const header = buffer_.substr(0, separator);
		std::size_t const length = parse_content_length(header);
		if (length > max_message_bytes_)
			throw std::runtime_error("LSP message exceeds the configured maxMessageBytes");
		std::size_t const body_start = separator + 4;
		if (buffer_.size() < body_start + length)
			return std::nullopt;
		std::string const body = buffer_.substr(body_start, length);
		buffer_.erase(0, body_start + length);
		try {
			return boost::json::parse(body);
		} catch (std::exception const&) {
			throw std::runtime_error("LSP message body was not valid JSON");
		}
	}

	static std::size_t parse_content_length(std::string_view header) {
		std::size_t start = 0;
		while (start <= header.size()) {
			auto const end = header.find("\r\n", start);
			std::string_view const line =
				header.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
			auto const colon = line.find(':');
			if (colon != std::string_view::npos) {
				std::string_view name = line.substr(0, colon);
				while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
					name.remove_suffix(1);
				std::string lowered;
				for (char const c : name)
					lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
				if (lowered == "content-length") {
					std::string_view value = line.substr(colon + 1);
					while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
						value.remove_prefix(1);
					std::size_t parsed = 0;
					for (char const c : value) {
						if (c < '0' || c > '9')
							throw std::runtime_error("invalid Content-Length header");
						parsed = parsed * 10 + static_cast<std::size_t>(c - '0');
					}
					return parsed;
				}
			}
			if (end == std::string_view::npos)
				break;
			start = end + 2;
		}
		throw std::runtime_error("LSP header block missing Content-Length");
	}

	std::size_t max_message_bytes_;
	std::string buffer_;
};

} // namespace araya::lsp_stdio
