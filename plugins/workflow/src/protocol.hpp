#pragma once

#include <boost/json/value.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace araya::workflow {

// One control frame: a 4-byte big-endian length prefix plus UTF-8 JSON.
std::string encode_frame(boost::json::value const& message);

// Incremental frame decoder. feed() consumes arbitrary byte chunks and
// returns every complete message. Throws std::runtime_error on a frame that
// exceeds the configured limit or carries malformed JSON.
class frame_decoder {
public:
	explicit frame_decoder(std::size_t max_bytes);

	std::vector<boost::json::value> feed(std::string_view bytes);

private:
	std::size_t max_bytes_;
	std::string buffer_;
};

} // namespace araya::workflow
