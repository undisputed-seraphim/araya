#include "protocol.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::workflow {

std::string encode_frame(boost::json::value const& message) {
	auto const json = boost::json::serialize(message);
	if (json.size() > 0xffffffffull)
		throw std::runtime_error("workflow: control frame exceeds the 32-bit length prefix");
	std::string frame(4, '\0');
	auto const size = static_cast<std::uint32_t>(json.size());
	frame[0] = static_cast<char>((size >> 24) & 0xff);
	frame[1] = static_cast<char>((size >> 16) & 0xff);
	frame[2] = static_cast<char>((size >> 8) & 0xff);
	frame[3] = static_cast<char>(size & 0xff);
	frame += json;
	return frame;
}

frame_decoder::frame_decoder(std::size_t max_bytes)
	: max_bytes_(max_bytes) {}

std::vector<boost::json::value> frame_decoder::feed(std::string_view bytes) {
	buffer_.append(bytes);
	std::vector<boost::json::value> messages;
	for (;;) {
		if (buffer_.size() < 4)
			break;
		auto const* data = reinterpret_cast<unsigned char const*>(buffer_.data());
		auto const length = (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) |
							(static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
		if (length > max_bytes_)
			throw std::runtime_error("workflow: control frame exceeds the configured limit");
		if (buffer_.size() < 4 + static_cast<std::size_t>(length))
			break;
		auto const payload = buffer_.substr(4, length);
		buffer_.erase(0, 4 + length);
		boost::system::error_code ec;
		auto parsed = boost::json::parse(payload, ec);
		if (ec)
			throw std::runtime_error("workflow: malformed control frame: " + ec.message());
		messages.push_back(std::move(parsed));
	}
	return messages;
}

} // namespace araya::workflow
