#include "araya/session-log-export/export.hpp"

#include "araya/attachment/attachment.hpp"
#include "araya/config.hpp"
#include "araya/logger/log.hpp"
#include "araya/persistence/persistence.hpp"
#include "araya/session-query/query.hpp"
#include "araya/session/store.hpp"
#include "araya/util/json.hpp"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <zlib.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::session_export {
namespace {

using araya::attachment::attachment_service;
using araya::attachment::attachments_key;
using araya::persistence::persistence_key;
using araya::persistence::session_persistence;
using araya::persistence::stored_session;
using araya::session::session_event;
using araya::session::SESSION_FORMAT_VERSION;
using araya::session::session_header;
using araya::session::session_id;
using araya::session::session_store;
using araya::session::sessions_key;
using araya::session_query::session_query_key;
using araya::session_query::session_query_service;

constexpr araya::config_key<int> compression_level_key{"compressionLevel"};

constexpr araya::config_field g_config[] = {
	field(compression_level_key, "ZIP deflate compression level (0-9).", "6"),
};

std::string sanitize_id(std::string_view id) {
	std::string out;
	out.reserve(id.size());
	for (char const c : id) {
		bool const keep =
			(c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		out.push_back(keep ? c : '_');
	}
	return out;
}

// -- canonical JSONL --------------------------------------------------------

boost::json::object header_record(session_header const& h) {
	boost::json::object out{
		{"type", "session"},
		{"version", SESSION_FORMAT_VERSION},
		{"id", h.id.value},
		{"created_at", h.created_at},
		{"is_seeded", h.is_seeded},
		{"delegation_depth", h.delegation_depth},
		{"origin", h.origin == araya::session::session_origin::subagent ? "subagent" : "none"},
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

std::string serialize_log(stored_session const& stored) {
	std::string out;
	out += boost::json::serialize(header_record(stored.header));
	out += '\n';
	for (auto const& event : stored.events) {
		out += boost::json::serialize(event_record(event));
		out += '\n';
	}
	return out;
}

// -- image attachment discovery ---------------------------------------------

void collect_images(boost::json::value const& value, std::set<std::pair<std::string, std::string>>& found) {
	if (auto const* array = value.if_array()) {
		for (auto const& entry : *array)
			collect_images(entry, found);
		return;
	}
	auto const* object = value.if_object();
	if (!object)
		return;
	if (araya::util::json::get_string(*object, "type") == "image") {
		auto const id = araya::util::json::get_string(*object, "attachment_id");
		if (!id.empty())
			found.emplace(id, araya::util::json::get_string(*object, "media_type"));
	}
	for (auto const& [key, entry] : *object) {
		(void)key;
		collect_images(entry, found);
	}
}

std::string extension_for(std::string_view media_type) {
	if (media_type == "image/png")
		return "png";
	if (media_type == "image/jpeg")
		return "jpg";
	if (media_type == "image/webp")
		return "webp";
	if (media_type == "image/gif")
		return "gif";
	return "bin";
}

// -- a minimal streaming-free ZIP writer (deflate method 8, store fallback) --

class zip_writer {
public:
	explicit zip_writer(int level)
		: level_(level) {}

	void add(std::string const& name, std::string const& data) {
		entry record;
		record.name = name;
		record.offset = static_cast<std::uint32_t>(out_.size());
		record.uncompressed = static_cast<std::uint32_t>(data.size());
		record.crc = crc32(
			0L, data.empty() ? Z_NULL : reinterpret_cast<Bytef const*>(data.data()), static_cast<uInt>(data.size()));
		auto deflated = deflate_raw(data);
		if (deflated.size() < data.size()) {
			record.method = 8;
			record.compressed = static_cast<std::uint32_t>(deflated.size());
			record.payload = std::move(deflated);
		} else {
			record.method = 0;
			record.compressed = record.uncompressed;
			record.payload = data;
		}
		write_local_header(record);
		out_ += record.payload;
		entries_.push_back(std::move(record));
	}

	std::string finish() {
		std::uint32_t const directory_offset = static_cast<std::uint32_t>(out_.size());
		for (auto const& record : entries_)
			write_central_header(record);
		std::uint32_t const directory_size = static_cast<std::uint32_t>(out_.size()) - directory_offset;
		put32(0x06054b50);
		put16(0);
		put16(0);
		put16(static_cast<std::uint16_t>(entries_.size()));
		put16(static_cast<std::uint16_t>(entries_.size()));
		put32(directory_size);
		put32(directory_offset);
		put16(0);
		return std::move(out_);
	}

private:
	struct entry {
		std::string name;
		std::string payload;
		std::uint32_t crc = 0;
		std::uint32_t compressed = 0;
		std::uint32_t uncompressed = 0;
		std::uint32_t offset = 0;
		std::uint16_t method = 0;
	};

	static std::uint16_t dos_date() { return 0x0021; } // 1980-01-01

	void put16(std::uint16_t value) {
		out_.push_back(static_cast<char>(value & 0xFF));
		out_.push_back(static_cast<char>((value >> 8) & 0xFF));
	}

	void put32(std::uint32_t value) {
		for (int i = 0; i < 4; ++i)
			out_.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
	}

	void write_local_header(entry const& record) {
		put32(0x04034b50);
		put16(20);
		put16(0);
		put16(record.method);
		put16(0);
		put16(dos_date());
		put32(record.crc);
		put32(record.compressed);
		put32(record.uncompressed);
		put16(static_cast<std::uint16_t>(record.name.size()));
		put16(0);
		out_ += record.name;
	}

	void write_central_header(entry const& record) {
		put32(0x02014b50);
		put16(20);
		put16(20);
		put16(0);
		put16(record.method);
		put16(0);
		put16(dos_date());
		put32(record.crc);
		put32(record.compressed);
		put32(record.uncompressed);
		put16(static_cast<std::uint16_t>(record.name.size()));
		put16(0);
		put16(0);
		put16(0);
		put16(0);
		put32(0);
		put32(record.offset);
		out_ += record.name;
	}

	std::string deflate_raw(std::string const& data) {
		z_stream stream{};
		if (deflateInit2(&stream, level_, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
			throw std::runtime_error("session export: deflate init failed");
		stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
		stream.avail_in = static_cast<uInt>(data.size());
		std::string out;
		out.resize(deflateBound(&stream, static_cast<uLong>(data.size())) + 16);
		stream.next_out = reinterpret_cast<Bytef*>(out.data());
		stream.avail_out = static_cast<uInt>(out.size());
		int const result = deflate(&stream, Z_FINISH);
		if (result != Z_STREAM_END) {
			deflateEnd(&stream);
			throw std::runtime_error("session export: deflate failed");
		}
		out.resize(out.size() - stream.avail_out);
		deflateEnd(&stream);
		return out;
	}

	int level_;
	std::string out_;
	std::vector<entry> entries_;
};

// -- the engine -------------------------------------------------------------

class export_engine : public session_export_service {
public:
	export_engine(
		std::shared_ptr<session_store> store,
		std::shared_ptr<session_persistence> persistence,
		std::shared_ptr<attachment_service> attachments,
		std::shared_ptr<session_query_service> query,
		int level,
		araya::logger::log_handle log = {})
		: store_(std::move(store))
		, persistence_(std::move(persistence))
		, attachments_(std::move(attachments))
		, query_(std::move(query))
		, level_(level)
		, log_(std::move(log)) {}

	std::string build_zip(session_id const& session, bool include_descendants) const override {
		auto root = load(session);
		if (!root)
			throw std::runtime_error("session not found: " + session.value);

		zip_writer zip(level_);
		std::set<std::pair<std::string, std::string>> images;
		zip.add("session.v1.jsonl", serialize_log(*root));
		collect_images_of(*root, images);

		if (include_descendants && query_) {
			if (auto trace = query_->trace_session(session)) {
				std::set<std::string> seen{session.value};
				for (auto const& node : trace->descendants) {
					if (!seen.insert(node.node.header.id.value).second)
						continue;
					auto stored = load(node.node.header.id);
					if (!stored) {
						log_.debug("skipping unreadable descendant '{}'", node.node.header.id.value);
						continue;
					}
					zip.add(
						"subagents/" + sanitize_id(node.node.header.id.value) + "/session.v1.jsonl",
						serialize_log(*stored));
					collect_images_of(*stored, images);
				}
			}
		}

		if (attachments_) {
			for (auto const& [id, media_type] : images) {
				if (auto data = attachments_->load(id))
					zip.add("media/" + sanitize_id(id) + "." + extension_for(media_type), *data);
			}
		}
		return zip.finish();
	}

private:
	std::optional<stored_session> load(session_id const& id) const {
		if (store_) {
			if (auto session = store_->get(id))
				return stored_session{session->header(), session->log()};
		}
		if (persistence_) {
			try {
				if (auto stored = persistence_->read(id))
					return stored;
			} catch (...) {
				log_.warn("cannot read session '{}' for export", id.value);
			}
		}
		return std::nullopt;
	}

	static void collect_images_of(stored_session const& stored, std::set<std::pair<std::string, std::string>>& images) {
		for (auto const& event : stored.events)
			collect_images(event.data, images);
	}

	std::shared_ptr<session_store> store_;
	std::shared_ptr<session_persistence> persistence_;
	std::shared_ptr<attachment_service> attachments_;
	std::shared_ptr<session_query_service> query_;
	int level_;
	araya::logger::log_handle log_;
};

struct session_log_export_plugin : araya::plugin {
	explicit session_log_export_plugin(araya::plugin_config const& config) {
		araya::plugin_config_view const view(config);
		if (auto value = view.try_get(compression_level_key))
			level_ = *value;
		if (level_ < 0 || level_ > 9)
			throw std::invalid_argument("session-log-export: compressionLevel must be between 0 and 9");
	}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto store = ctx.require<session_store>(sessions_key).shared();
		std::shared_ptr<session_persistence> persistence;
		if (auto lease = ctx.find<session_persistence>(persistence_key))
			persistence = lease->shared();
		std::shared_ptr<attachment_service> attachments;
		if (auto lease = ctx.find<attachment_service>(attachments_key))
			attachments = lease->shared();
		std::shared_ptr<session_query_service> query;
		if (auto lease = ctx.find<session_query_service>(session_query_key))
			query = lease->shared();

		auto engine = std::make_shared<export_engine>(
			std::move(store),
			std::move(persistence),
			std::move(attachments),
			std::move(query),
			level_,
			araya::logger::from_context(ctx, "session-log-export"));
		ctx.provide(session_export_key, std::shared_ptr<session_export_service>(std::move(engine)));
		co_return;
	}

private:
	int level_ = 6;
};

std::unique_ptr<araya::plugin> make_session_log_export(araya::plugin_config const& config) {
	return std::make_unique<session_log_export_plugin>(config);
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"session.persistence", 1}, false, {}},
	{araya::service_id{"attachments", 1}, false, {}},
	{araya::service_id{"session-query", 1}, false, {}},
	{araya::service_id{"logger", 1}, false, {}},
};
static const araya::provision_spec g_provs[]{{araya::service_id{"session-export", 1}}};
static const araya::plugin_descriptor
	g_descriptor{"session-log-export", g_deps, g_provs, &make_session_log_export, g_config};

} // namespace

std::string session_export_filename(session_id const& session) {
	return "araya-session-" + sanitize_id(session.value) + ".zip";
}

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::session_export
