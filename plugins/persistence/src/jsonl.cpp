#include "araya/persistence/persistence.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The JSONL backend: root / hex(id) / session.v<N>.jsonl, where line 1
// is a "session" header record and every event is one compact record.
// Records are single lines because boost::json escapes newlines, so a
// torn write is exactly "the file does not end in a newline": attach()
// truncates back to the last complete line.
//
// Durability: appends buffer in memory; flush() drains the buffer with
// write() and fsync()s - the session/flush barrier's contract. No
// directory fsync and no temp-file rename: this is an append-only log,
// so a power cut between creation and the first flush can only lose a
// not-yet-flushed session file, never corrupt a committed one.

namespace araya::persistence {
namespace {

using session::session_event;
using session::SESSION_FORMAT_VERSION;
using session::session_header;
using session::session_id;

constexpr std::size_t k_flush_threshold = 64 * 1024;

[[noreturn]] void fail(std::string const& what) { throw std::runtime_error(what); }

std::string hex_encode(std::string_view text) {
	static constexpr char digits[] = "0123456789abcdef";
	std::string out;
	out.reserve(text.size() * 2);
	for (unsigned char c : text) {
		out.push_back(digits[c >> 4]);
		out.push_back(digits[c & 0xF]);
	}
	return out;
}

std::optional<std::string> hex_decode(std::string_view text) {
	if (text.size() % 2 != 0)
		return std::nullopt;
	std::string out;
	out.reserve(text.size() / 2);
	for (std::size_t i = 0; i < text.size(); i += 2) {
		auto nibble = [](char c) -> int {
			if (c >= '0' && c <= '9')
				return c - '0';
			if (c >= 'a' && c <= 'f')
				return c - 'a' + 10;
			return -1;
		};
		int hi = nibble(text[i]);
		int lo = nibble(text[i + 1]);
		if (hi < 0 || lo < 0)
			return std::nullopt;
		out.push_back(static_cast<char>((hi << 4) | lo));
	}
	return out;
}

void write_all(int fd, std::string_view data) {
	while (!data.empty()) {
		ssize_t n = ::write(fd, data.data(), data.size());
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fail("write failed: " + std::string(std::strerror(errno)));
		}
		data.remove_prefix(static_cast<std::size_t>(n));
	}
}

std::string read_all(std::filesystem::path const& path) {
	int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ENOENT)
			return {};
		fail("cannot open '" + path.string() + "': " + std::strerror(errno));
	}
	std::string out;
	char chunk[16 * 1024];
	for (;;) {
		ssize_t n = ::read(fd, chunk, sizeof chunk);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			::close(fd);
			fail("read failed on '" + path.string() + "': " + std::strerror(errno));
		}
		if (n == 0)
			break;
		out.append(chunk, static_cast<std::size_t>(n));
	}
	::close(fd);
	return out;
}

// Truncates an incomplete trailing record: when the file does not end in
// '\n', drop everything after the last newline.
void truncate_torn_tail(std::filesystem::path const& path) {
	auto contents = read_all(path);
	if (contents.empty() || contents.back() == '\n')
		return;
	auto last = contents.rfind('\n');
	contents.resize(last == std::string::npos ? 0 : last + 1);
	int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
	if (fd < 0)
		fail("cannot reopen '" + path.string() + "': " + std::strerror(errno));
	write_all(fd, contents);
	::close(fd);
}

boost::json::object header_record(session_header const& h) {
	boost::json::object out{
		{"type", "session"},
		{"version", SESSION_FORMAT_VERSION},
		{"id", h.id.value},
		{"created_at", h.created_at},
		{"is_seeded", h.is_seeded},
		{"delegation_depth", h.delegation_depth},
		{"origin", h.origin == session::session_origin::subagent ? "subagent" : "none"},
	};
	if (h.cwd)
		out["cwd"] = *h.cwd;
	if (h.parent_session)
		out["parent_session"] = h.parent_session->value;
	if (h.agent_preset)
		out["agent_preset"] = *h.agent_preset;
	return out;
}

boost::json::object event_record(session_event const& ev) {
	return {
		{"seq", ev.seq},
		{"time", ev.time},
		{"type", ev.type},
		{"data", ev.data},
		{"ignorable", ev.ignorable},
	};
}

session::session_origin parse_origin(std::string_view word) {
	if (word == "subagent")
		return session::session_origin::subagent;
	return session::session_origin::none;
}

struct writer_state {
	int fd = -1;
	std::string buffer;
};

class jsonl_backend : public session_persistence {
public:
	explicit jsonl_backend(std::filesystem::path root)
		: root_(std::move(root)) {}

	~jsonl_backend() override {
		// detach() erases from writers_, so collect the keys first.
		std::vector<session_id> ids;
		ids.reserve(writers_.size());
		for (auto const& [id, w] : writers_)
			ids.push_back(id);
		for (auto const& id : ids)
			detach(id);
	}

	void attach(session_header const& header) override {
		if (writers_.contains(header.id))
			fail("session '" + header.id.value + "' already attached");
		auto dir = session_dir(header.id);
		std::filesystem::create_directories(dir);
		auto path = session_file(header.id);
		if (std::filesystem::exists(path)) {
			truncate_torn_tail(path);
		} else {
			boost::json::value record = header_record(header);
			int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
			if (fd < 0)
				fail("cannot create '" + path.string() + "': " + std::strerror(errno));
			std::string line = boost::json::serialize(record) + "\n";
			try {
				write_all(fd, line);
			} catch (...) {
				::close(fd);
				throw;
			}
			::close(fd);
		}
		int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
		if (fd < 0)
			fail("cannot open '" + path.string() + "': " + std::strerror(errno));
		writers_.emplace(header.id, writer_state{fd, {}});
	}

	void append(session_id const& id, session_event const& ev) override {
		auto it = writers_.find(id);
		if (it == writers_.end())
			return;
		it->second.buffer += boost::json::serialize(event_record(ev)) + "\n";
		if (it->second.buffer.size() >= k_flush_threshold)
			drain(it->second);
	}

	void flush(session_id const& id) override {
		auto it = writers_.find(id);
		if (it == writers_.end())
			return;
		drain(it->second);
		if (::fsync(it->second.fd) != 0)
			fail("fsync failed: " + std::string(std::strerror(errno)));
	}

	void detach(session_id const& id) override {
		auto it = writers_.find(id);
		if (it == writers_.end())
			return;
		flush(id);
		::close(it->second.fd);
		writers_.erase(it);
	}

	std::optional<stored_session> read(session_id const& id) const override {
		auto path = session_file(id);
		if (!std::filesystem::exists(path))
			return std::nullopt;
		auto contents = read_all(path);
		stored_session stored;
		bool saw_header = false;
		std::size_t begin = 0;
		while (begin < contents.size()) {
			auto end = contents.find('\n', begin);
			if (end == std::string::npos)
				end = contents.size();
			if (end > begin) {
				auto line = std::string_view(contents).substr(begin, end - begin);
				boost::json::value record = boost::json::parse(line);
				if (!record.is_object())
					fail("malformed record in '" + path.string() + "'");
				auto const& obj = record.as_object();
				auto type_it = obj.find("type");
				if (type_it == obj.end() || !type_it->value().is_string())
					fail("record without a type in '" + path.string() + "'");
				if (type_it->value() == "session") {
					if (saw_header)
						fail("duplicate header record in '" + path.string() + "'");
					saw_header = true;
					stored.header = parse_header(obj, path);
				} else {
					if (!saw_header)
						fail("event before header record in '" + path.string() + "'");
					stored.events.push_back(parse_event(obj, path));
				}
			}
			begin = end + 1;
		}
		if (!saw_header)
			fail("missing header record in '" + path.string() + "'");
		return stored;
	}

	std::vector<session_id> list() const override {
		std::vector<session_id> out;
		std::error_code ec;
		if (!std::filesystem::exists(root_, ec))
			return out;
		for (auto const& entry : std::filesystem::directory_iterator(root_, ec)) {
			if (!entry.is_directory(ec))
				continue;
			auto decoded = hex_decode(entry.path().filename().string());
			if (decoded)
				out.push_back(session_id{std::move(*decoded)});
		}
		return out;
	}

private:
	static session_header parse_header(boost::json::object const& obj, std::filesystem::path const& path) {
		auto version = obj.find("version");
		if (version == obj.end() || !version->value().is_int64())
			fail("header without a version in '" + path.string() + "'");
		if (version->value().as_int64() != SESSION_FORMAT_VERSION)
			fail(
				"session format version " + std::to_string(version->value().as_int64()) +
				" is not supported (expected " + std::to_string(SESSION_FORMAT_VERSION) + ")");
		session_header h;
		h.version = SESSION_FORMAT_VERSION;
		h.id = session_id{require_string(obj, "id", path)};
		h.created_at = require_int(obj, "created_at", path);
		if (auto it = obj.find("cwd"); it != obj.end() && it->value().is_string())
			h.cwd = std::string(it->value().as_string());
		if (auto it = obj.find("parent_session"); it != obj.end() && it->value().is_string())
			h.parent_session = session_id{std::string(it->value().as_string())};
		if (auto it = obj.find("is_seeded"); it != obj.end() && it->value().is_bool())
			h.is_seeded = it->value().as_bool();
		if (auto it = obj.find("delegation_depth"); it != obj.end() && it->value().is_int64())
			h.delegation_depth = static_cast<std::uint32_t>(it->value().as_int64());
		if (auto it = obj.find("origin"); it != obj.end() && it->value().is_string())
			h.origin = parse_origin(it->value().as_string());
		if (auto it = obj.find("agent_preset"); it != obj.end() && it->value().is_string())
			h.agent_preset = std::string(it->value().as_string());
		return h;
	}

	static session_event parse_event(boost::json::object const& obj, std::filesystem::path const& path) {
		session_event ev;
		ev.seq = require_int(obj, "seq", path);
		ev.time = require_int(obj, "time", path);
		ev.type = require_string(obj, "type", path);
		auto data = obj.find("data");
		if (data == obj.end())
			fail("event without data in '" + path.string() + "'");
		ev.data = data->value();
		if (auto it = obj.find("ignorable"); it != obj.end() && it->value().is_bool())
			ev.ignorable = it->value().as_bool();
		return ev;
	}

	static std::int64_t
	require_int(boost::json::object const& obj, std::string_view key, std::filesystem::path const& path) {
		auto it = obj.find(key);
		if (it == obj.end() || !it->value().is_int64())
			fail("record missing integer '" + std::string(key) + "' in '" + path.string() + "'");
		return it->value().as_int64();
	}

	static std::string
	require_string(boost::json::object const& obj, std::string_view key, std::filesystem::path const& path) {
		auto it = obj.find(key);
		if (it == obj.end() || !it->value().is_string())
			fail("record missing string '" + std::string(key) + "' in '" + path.string() + "'");
		return std::string(it->value().as_string());
	}

	static void drain(writer_state& w) {
		if (w.buffer.empty())
			return;
		write_all(w.fd, w.buffer);
		w.buffer.clear();
	}

	std::filesystem::path session_dir(session_id const& id) const { return root_ / hex_encode(id.value); }

	std::filesystem::path session_file(session_id const& id) const {
		return session_dir(id) / ("session.v" + std::to_string(SESSION_FORMAT_VERSION) + ".jsonl");
	}

	std::filesystem::path root_;
	std::map<session_id, writer_state> writers_;
};

} // namespace
} // namespace araya::persistence

namespace araya::persistence {

std::shared_ptr<session_persistence> make_jsonl_backend(std::filesystem::path root) {
	return std::make_shared<jsonl_backend>(std::move(root));
}

} // namespace araya::persistence
