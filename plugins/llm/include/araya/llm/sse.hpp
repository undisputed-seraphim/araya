#pragma once

#include <functional>
#include <string>
#include <string_view>

// SSE framing, the streaming half of the SSE spec: line-based, CRLF
// tolerant, leading BOM skipped, comments ignored but reported as
// activity (the idle-watchdog pulse), multiple data: lines joined with
// \n, events dispatched at the blank-line boundary. [DONE] and friends
// surface as plain data; the wire layer interprets them.
namespace araya::llm {

class sse_parser {
public:
	// Called for every complete event: the event: field (empty when
	// absent) and the joined data payload.
	std::function<void(std::string_view event, std::string_view data)> on_event;
	// Called for every non-empty line (data and heartbeat comments
	// alike) so transports can reset an idle watchdog.
	std::function<void()> on_activity;

	void feed(std::string_view bytes);

	void reset() noexcept;

private:
	void dispatch();

	std::string buffer_;
	std::string event_;
	std::string data_;
	bool has_data_ = false;
	bool first_ = true; // BOM skip
};

inline void sse_parser::feed(std::string_view bytes) {
	buffer_.append(bytes.data(), bytes.size());
	if (first_ && buffer_.size() >= 3 && buffer_.compare(0, 3, "\xEF\xBB\xBF") == 0)
		buffer_.erase(0, 3);
	first_ = false;
	for (;;) {
		auto pos = buffer_.find('\n');
		if (pos == std::string::npos)
			break;
		std::string line = buffer_.substr(0, pos);
		buffer_.erase(0, pos + 1);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty()) {
			dispatch();
			continue;
		}
		if (on_activity)
			on_activity();
		if (line[0] == ':')
			continue;
		auto colon = line.find(':');
		auto field = colon == std::string::npos ? line : line.substr(0, colon);
		auto value = colon == std::string::npos ? std::string{} : line.substr(colon + 1);
		if (!value.empty() && value.front() == ' ')
			value.erase(0, 1);
		if (field == "data") {
			if (has_data_)
				data_ += '\n';
			data_ += value;
			has_data_ = true;
		} else if (field == "event") {
			event_ = value;
		}
		// Other fields (id, retry) are ignored.
	}
}

inline void sse_parser::dispatch() {
	// The spec resets both buffers even when no data accumulated: the
	// event type applies to the next event only.
	if (has_data_ && on_event)
		on_event(event_, data_);
	event_.clear();
	data_.clear();
	has_data_ = false;
}

inline void sse_parser::reset() noexcept {
	buffer_.clear();
	event_.clear();
	data_.clear();
	has_data_ = false;
	first_ = true;
}

} // namespace araya::llm
