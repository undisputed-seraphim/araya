#include "araya/session-query-sqlite/sqlite.hpp"

#include "araya/config.hpp"
#include "araya/persistence/persistence.hpp"
#include "araya/session-query/query.hpp"
#include "araya/session/store.hpp"
#include "araya/util/string.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace araya::session_query_sqlite {
namespace {

using araya::persistence::persistence_key;
using araya::persistence::session_persistence;
using araya::session::session_header;
using araya::session::session_id;
using araya::session::session_store;
using araya::session::sessions_key;
using araya::session_query::event_filter;
using araya::session_query::event_search_hit;
using araya::session_query::event_search_page;
using araya::session_query::event_search_request;
using araya::session_query::query_error;
using araya::session_query::query_error_code;
using araya::session_query::session_filter;
using araya::session_query::session_query_key;
using araya::session_query::session_query_service;
using araya::session_query::session_record;
using araya::session_query::session_search_hit;
using araya::session_query::session_search_page;
using araya::session_query::session_search_request;
using araya::session_query::session_source;

constexpr araya::config_key<std::string> path_key{"path"};
constexpr araya::config_key<std::string> open_at_key{"openAt"};
constexpr araya::config_key<std::string> journal_mode_key{"journalMode"};
constexpr araya::config_key<std::int64_t> limit_key{"limit"};
constexpr araya::config_key<std::int64_t> max_limit_key{"maxLimit"};
constexpr araya::config_key<std::int64_t> snippet_chars_key{"snippetChars"};

constexpr araya::config_field g_config[] = {
	field(path_key, "SQLite index path.", "araya-session-query.sqlite3"),
	field(open_at_key, "When to open the index: startup | first-search | never.", "first-search"),
	field(journal_mode_key, "SQLite journal mode: wal | delete | truncate | persist.", "wal"),
	field(limit_key, "Default page size.", "20"),
	field(max_limit_key, "Maximum page size.", "100"),
	field(snippet_chars_key, "Maximum snippet characters.", "240"),
};

constexpr std::int32_t k_application_id = 0x41524151; // "ARAQ"
constexpr int k_schema_version = 1;

std::string normalize_query(std::string_view text) {
	std::string out;
	bool pending_space = false;
	for (char const c : text) {
		if (c == '\0')
			throw query_error(query_error_code::invalid_query, "query must not contain NUL");
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			pending_space = !out.empty();
			continue;
		}
		if (pending_space) {
			out += ' ';
			pending_space = false;
		}
		out += c;
	}
	if (out.empty())
		throw query_error(query_error_code::invalid_query, "query must not be empty");
	return out;
}

std::string quote_fts(std::string const& text) {
	std::string out;
	out.reserve(text.size() + 2);
	out += '"';
	for (char const c : text) {
		if (c == '"')
			out += '"';
		out += c;
	}
	out += '"';
	return out;
}

std::string trim_snippet(std::string text, std::size_t max_chars) {
	if (text.size() <= max_chars)
		return text;
	text.resize(max_chars);
	text += "...";
	return text;
}

constexpr char const* surface_name(araya::session_query::event_surface surface) {
	using araya::session_query::event_surface;
	switch (surface) {
	case event_surface::current:
		return "current";
	case event_surface::shadowed:
		return "shadowed";
	case event_surface::log_only:
		return "log-only";
	}
	return "log-only";
}

[[noreturn]] void fail(std::string const& what) { throw query_error(query_error_code::index_failed, what); }

void check_rc(sqlite3* db, int rc, std::string const& what) {
	if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
		fail(what + ": " + (db ? sqlite3_errmsg(db) : "out of memory"));
}

void exec_sql(sqlite3* db, std::string const& sql) {
	char* message = nullptr;
	int const rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message);
	if (rc != SQLITE_OK) {
		std::string detail = message ? message : "unknown error";
		sqlite3_free(message);
		fail("sqlite: " + detail);
	}
}

using sql_value = std::variant<std::nullptr_t, std::int64_t, std::string>;

class statement {
public:
	statement(sqlite3* db, std::string const& sql) {
		int const rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr);
		check_rc(db, rc, "sqlite: prepare");
	}
	~statement() {
		if (stmt_)
			sqlite3_finalize(stmt_);
	}
	statement(statement const&) = delete;
	statement& operator=(statement const&) = delete;

	void bind(std::vector<sql_value> const& values) {
		for (std::size_t i = 0; i < values.size(); ++i) {
			int const index = static_cast<int>(i) + 1;
			if (std::holds_alternative<std::nullptr_t>(values[i])) {
				sqlite3_bind_null(stmt_, index);
			} else if (auto const* number = std::get_if<std::int64_t>(&values[i])) {
				sqlite3_bind_int64(stmt_, index, *number);
			} else {
				auto const& text = std::get<std::string>(values[i]);
				sqlite3_bind_text(stmt_, index, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
			}
		}
	}

	bool step() {
		int const rc = sqlite3_step(stmt_);
		if (rc == SQLITE_ROW)
			return true;
		if (rc == SQLITE_DONE)
			return false;
		fail(std::string("sqlite: step: ") + sqlite3_errmsg(sqlite3_db_handle(stmt_)));
	}

	void reset() {
		sqlite3_reset(stmt_);
		sqlite3_clear_bindings(stmt_);
	}

	std::int64_t column_int(int index) const { return sqlite3_column_int64(stmt_, index); }

	std::string column_text(int index) const {
		auto const* text = sqlite3_column_text(stmt_, index);
		if (!text)
			return {};
		return std::string(
			reinterpret_cast<char const*>(text), static_cast<std::size_t>(sqlite3_column_bytes(stmt_, index)));
	}

	bool column_is_null(int index) const { return sqlite3_column_type(stmt_, index) == SQLITE_NULL; }

private:
	sqlite3_stmt* stmt_ = nullptr;
};

std::string placeholders(std::size_t count) {
	std::string out;
	for (std::size_t i = 0; i < count; ++i) {
		if (i)
			out += ',';
		out += '?';
	}
	return out;
}

void append_session_filters(
	std::string& where,
	std::vector<sql_value>& binds,
	std::vector<session_filter> const& filters) {
	for (auto const& filter : filters) {
		switch (filter.clause) {
		case session_filter::kind::id:
			if (filter.values.empty()) {
				where += " AND 0";
				break;
			}
			where += " AND documents.session_id IN (" + placeholders(filter.values.size()) + ")";
			for (auto const& value : filter.values)
				binds.emplace_back(value);
			break;
		case session_filter::kind::cwd:
			if (filter.values.empty()) {
				where += " AND 0";
				break;
			}
			where += " AND s.cwd IN (" + placeholders(filter.values.size()) + ")";
			for (auto const& value : filter.values)
				binds.emplace_back(value);
			break;
		case session_filter::kind::created_at:
			if (filter.from) {
				where += " AND s.created_at >= ?";
				binds.emplace_back(*filter.from);
			}
			if (filter.to) {
				where += " AND s.created_at <= ?";
				binds.emplace_back(*filter.to);
			}
			break;
		case session_filter::kind::parent:
			if (filter.values.empty()) {
				if (filter.allow_root)
					where += " AND s.parent_session IS NULL";
				else
					where += " AND 0";
				break;
			}
			where += " AND (s.parent_session IN (" + placeholders(filter.values.size()) + ")";
			for (auto const& value : filter.values)
				binds.emplace_back(value);
			if (filter.allow_root)
				where += " OR s.parent_session IS NULL";
			where += ")";
			break;
		case session_filter::kind::availability: {
			std::string clause;
			for (auto const& value : filter.values) {
				std::string term;
				if (value == "live")
					term = "s.live = 1";
				else if (value == "persisted")
					term = "s.persisted = 1";
				if (term.empty())
					continue;
				if (!clause.empty())
					clause += " OR ";
				clause += term;
			}
			where += clause.empty() ? " AND 0" : " AND (" + clause + ")";
			break;
		}
		}
	}
}

void append_event_filters(std::string& where, std::vector<sql_value>& binds, std::vector<event_filter> const& filters) {
	for (auto const& filter : filters) {
		switch (filter.clause) {
		case event_filter::kind::seq:
			if (filter.from) {
				where += " AND documents.seq >= ?";
				binds.emplace_back(*filter.from);
			}
			if (filter.to) {
				where += " AND documents.seq <= ?";
				binds.emplace_back(*filter.to);
			}
			break;
		case event_filter::kind::time:
			if (filter.from) {
				where += " AND documents.time >= ?";
				binds.emplace_back(*filter.from);
			}
			if (filter.to) {
				where += " AND documents.time <= ?";
				binds.emplace_back(*filter.to);
			}
			break;
		case event_filter::kind::type:
			if (filter.values.empty()) {
				where += " AND 0";
				break;
			}
			where += " AND documents.type IN (" + placeholders(filter.values.size()) + ")";
			for (auto const& value : filter.values)
				binds.emplace_back(value);
			break;
		case event_filter::kind::surface:
			if (filter.values.empty()) {
				where += " AND 0";
				break;
			}
			where += " AND documents.surface IN (" + placeholders(filter.values.size()) + ")";
			for (auto const& value : filter.values)
				binds.emplace_back(value);
			break;
		case event_filter::kind::text:
			where += " AND instr(lower(documents.text), lower(?)) > 0";
			binds.emplace_back(filter.text);
			break;
		}
	}
}

struct engine_config {
	std::string path = "araya-session-query.sqlite3";
	std::string open_at = "first-search";
	std::string journal_mode = "wal";
	int limit = 20;
	int max_limit = 100;
	int snippet_chars = 240;
};

class sqlite_engine : public session_query_service {
public:
	sqlite_engine(
		std::shared_ptr<session_store> store,
		std::shared_ptr<session_persistence> persistence,
		engine_config config,
		std::shared_ptr<session_query_service> reads)
		: reads_(std::move(reads))
		, store_(std::move(store))
		, persistence_(std::move(persistence))
		, config_(std::move(config)) {}

	~sqlite_engine() override {
		if (db_)
			sqlite3_close(db_);
	}

	void warm() const { ensure_open(); }

	// Reads are backend-independent; the index only accelerates ranked search.
	std::vector<session_record> list_sessions() const override { return reads_->list_sessions(); }
	std::optional<session_source> read_session(session_id const& id) const override { return reads_->read_session(id); }
	std::vector<session_record> filter_sessions(std::vector<session_filter> const& filters) const override {
		return reads_->filter_sessions(filters);
	}
	std::vector<event_search_hit>
	filter_events(session_id const& session, std::vector<event_filter> const& filters) const override {
		return reads_->filter_events(session, filters);
	}
	std::optional<araya::session_query::event_read>
	read_event(session_id const& session, araya::session::session_seq seq, unsigned before, unsigned after)
		const override {
		return reads_->read_event(session, seq, before, after);
	}
	std::optional<std::string> read_title(session_id const& id) const override { return reads_->read_title(id); }
	std::optional<araya::session_query::session_trace> trace_session(session_id const& id) const override {
		return reads_->trace_session(id);
	}
	std::optional<araya::session_query::event_trace>
	trace_event(session_id const& session, araya::session::session_seq seq) const override {
		return reads_->trace_event(session, seq);
	}

	session_search_page search_sessions(session_search_request const& request) const override {
		ensure_open();
		reconcile();
		std::size_t const offset = decode_cursor(request.cursor, generation_);
		std::size_t const limit = effective_limit(request.limit);

		std::string where;
		std::vector<sql_value> binds;
		binds.emplace_back(quote_fts(normalize_query(request.query)));
		append_session_filters(where, binds, request.session_filters);
		append_event_filters(where, binds, request.event_filters);

		// Group in C++ rather than with a window function: FTS5 auxiliary
		// functions (snippet/bm25) cannot be used in that context. Rows are
		// ranked globally, so the first row seen for a session is its best.
		std::string const sql =
			"SELECT documents.session_id, documents.seq, documents.type, documents.time, documents.surface,"
			"       snippet(documents, 0, '', '', '...', 16),"
			"       s.version, s.created_at, s.cwd, s.parent_session, s.delegation_depth, s.agent_preset,"
			"       s.live, s.persisted"
			" FROM documents JOIN indexed_sessions s ON s.id = documents.session_id"
			" WHERE documents MATCH ?" +
			where + " ORDER BY bm25(documents), documents.time DESC, documents.session_id ASC, documents.seq DESC";

		statement query(db_, sql);
		query.bind(binds);
		session_search_page page;
		std::vector<session_search_hit> grouped;
		std::map<std::string, bool> seen;
		while (query.step()) {
			std::string const id = query.column_text(0);
			if (seen.contains(id))
				continue;
			seen.emplace(id, true);

			session_header header;
			header.version = static_cast<std::uint32_t>(query.column_int(6));
			header.id = session_id{id};
			header.created_at = query.column_int(7);
			if (!query.column_is_null(8))
				header.cwd = query.column_text(8);
			if (!query.column_is_null(9))
				header.parent_session = session_id{query.column_text(9)};
			header.delegation_depth = static_cast<std::uint32_t>(query.column_int(10));
			if (!query.column_is_null(11))
				header.agent_preset = query.column_text(11);
			session_record record{std::move(header), query.column_int(12) != 0, query.column_int(13) != 0};

			event_search_hit hit;
			hit.session = record.header.id;
			hit.seq = static_cast<araya::session::session_seq>(query.column_int(1));
			hit.type = query.column_text(2);
			hit.time = query.column_int(3);
			hit.surface = parse_surface(query.column_text(4));
			hit.snippet = trim_snippet(query.column_text(5), static_cast<std::size_t>(config_.snippet_chars));
			grouped.push_back(session_search_hit{std::move(record), std::move(hit)});
			if (grouped.size() > offset + limit)
				break;
		}
		for (std::size_t i = offset; i < grouped.size() && page.items.size() < limit; ++i)
			page.items.push_back(std::move(grouped[i]));
		if (grouped.size() > offset + limit)
			page.next_cursor = encode_cursor(generation_, offset + limit);
		return page;
	}

	event_search_page search_events(event_search_request const& request) const override {
		ensure_open();
		reconcile();
		auto source = reads_->read_session(request.session);
		if (!source)
			throw query_error(query_error_code::session_not_found, "session not found: " + request.session.value);
		std::size_t const offset = decode_cursor(request.cursor, generation_);
		std::size_t const limit = effective_limit(request.limit);

		std::string where = " AND documents.session_id = ?";
		std::vector<sql_value> binds;
		binds.emplace_back(quote_fts(normalize_query(request.query)));
		binds.emplace_back(request.session.value);
		append_event_filters(where, binds, request.filters);
		binds.emplace_back(static_cast<std::int64_t>(limit + 1));
		binds.emplace_back(static_cast<std::int64_t>(offset));

		std::string const sql = "SELECT documents.seq, documents.type, documents.time, documents.surface,"
								"       snippet(documents, 0, '', '', '...', 16)"
								" FROM documents WHERE documents MATCH ?" +
								where +
								" ORDER BY bm25(documents), documents.time DESC, documents.seq DESC LIMIT ? OFFSET ?";

		statement query(db_, sql);
		query.bind(binds);
		event_search_page page;
		page.session = source->header;
		while (query.step()) {
			if (page.items.size() == limit) {
				page.next_cursor = encode_cursor(generation_, offset + limit);
				break;
			}
			event_search_hit hit;
			hit.session = request.session;
			hit.seq = static_cast<araya::session::session_seq>(query.column_int(0));
			hit.type = query.column_text(1);
			hit.time = query.column_int(2);
			hit.surface = parse_surface(query.column_text(3));
			hit.snippet = trim_snippet(query.column_text(4), static_cast<std::size_t>(config_.snippet_chars));
			page.items.push_back(std::move(hit));
		}
		return page;
	}

private:
	struct indexed_row {
		std::string revision;
		bool live = false;
		bool persisted = false;
	};

	struct wanted_row {
		std::string revision;
		bool live = false;
		bool persisted = false;
	};

	static araya::session_query::event_surface parse_surface(std::string const& name) {
		if (name == "current")
			return araya::session_query::event_surface::current;
		if (name == "shadowed")
			return araya::session_query::event_surface::shadowed;
		return araya::session_query::event_surface::log_only;
	}

	std::size_t effective_limit(std::size_t requested) const {
		int const limit = requested == 0 ? config_.limit : static_cast<int>(requested);
		return static_cast<std::size_t>(std::clamp(limit, 1, config_.max_limit));
	}

	static std::string encode_cursor(std::uint64_t generation, std::size_t offset) {
		return "v1:" + std::to_string(generation) + ":" + std::to_string(offset);
	}

	static std::size_t decode_cursor(std::string const& cursor, std::uint64_t generation) {
		if (cursor.empty())
			return 0;
		constexpr std::string_view prefix = "v1:";
		if (cursor.compare(0, prefix.size(), prefix) != 0)
			throw query_error(query_error_code::invalid_cursor, "invalid cursor");
		auto const body = cursor.substr(prefix.size());
		auto const colon = body.find(':');
		if (colon == std::string::npos)
			throw query_error(query_error_code::invalid_cursor, "invalid cursor");
		auto const number = [](std::string_view text) {
			if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
				return static_cast<std::uint64_t>(0);
			return static_cast<std::uint64_t>(std::stoull(std::string(text)));
		};
		auto const cursor_generation = number(body.substr(0, colon));
		if (cursor_generation != generation)
			throw query_error(query_error_code::stale_cursor, "session history changed while paging");
		return static_cast<std::size_t>(number(body.substr(colon + 1)));
	}

	void ensure_open() const {
		if (opened_)
			return;
		if (config_.open_at == "never")
			throw query_error(query_error_code::search_disabled, "session search is disabled");
		opened_ = true;

		auto const path = std::filesystem::path(config_.path);
		bool created_parent = false;
		if (path.has_parent_path()) {
			std::error_code ec;
			if (!std::filesystem::exists(path.parent_path(), ec)) {
				std::filesystem::create_directories(path.parent_path(), ec);
				created_parent = !ec;
			}
#ifndef _WIN32
			if (created_parent)
				::chmod(path.parent_path().c_str(), 0700);
#endif
		}
		bool const existed = [&] {
			std::error_code ec;
			return std::filesystem::exists(path, ec);
		}();

		sqlite3* db = nullptr;
		int const rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
		if (rc != SQLITE_OK) {
			std::string const message = db ? sqlite3_errmsg(db) : "out of memory";
			if (db)
				sqlite3_close(db);
			opened_ = false;
			fail("cannot open session index '" + config_.path + "': " + message);
		}
		db_ = db;
#ifndef _WIN32
		if (!existed)
			::chmod(path.c_str(), 0600);
#endif
		sqlite3_busy_timeout(db_, 5000);

		std::string const mode = config_.journal_mode;
		if (mode != "wal" && mode != "delete" && mode != "truncate" && mode != "persist")
			fail("invalid journalMode '" + mode + "'");
		exec_sql(db_, "PRAGMA journal_mode=" + mode);

		std::int64_t application_id = 0;
		{
			statement query(db_, "PRAGMA application_id");
			if (query.step())
				application_id = query.column_int(0);
		}
		if (application_id != 0 && application_id != k_application_id)
			fail("session index '" + config_.path + "' belongs to another application");
		if (application_id == 0) {
			statement query(
				db_, "SELECT count(*) FROM sqlite_master WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%'");
			if (query.step() && query.column_int(0) != 0)
				fail("session index '" + config_.path + "' is a non-empty unknown database");
		}

		exec_sql(
			db_,
			"PRAGMA application_id = " + std::to_string(k_application_id) +
				";"
				"CREATE TABLE IF NOT EXISTS search_state ("
				" singleton INTEGER PRIMARY KEY CHECK (singleton = 1),"
				" generation INTEGER NOT NULL"
				") STRICT;"
				"INSERT OR IGNORE INTO search_state(singleton, generation) VALUES (1, 0);"
				"CREATE TABLE IF NOT EXISTS indexed_sessions ("
				" id TEXT PRIMARY KEY,"
				" live INTEGER NOT NULL,"
				" persisted INTEGER NOT NULL,"
				" revision TEXT NOT NULL,"
				" version INTEGER NOT NULL,"
				" created_at INTEGER NOT NULL,"
				" cwd TEXT,"
				" parent_session TEXT,"
				" delegation_depth INTEGER NOT NULL,"
				" agent_preset TEXT"
				") STRICT;"
				"CREATE VIRTUAL TABLE IF NOT EXISTS documents USING fts5("
				" text,"
				" session_id UNINDEXED,"
				" seq UNINDEXED,"
				" type UNINDEXED,"
				" time UNINDEXED,"
				" surface UNINDEXED,"
				" tokenize = 'unicode61'"
				");"
				"PRAGMA user_version = " +
				std::to_string(k_schema_version));

		statement query(db_, "SELECT generation FROM search_state WHERE singleton = 1");
		if (query.step())
			generation_ = static_cast<std::uint64_t>(query.column_int(0));
	}

	static std::string live_revision(araya::session::session const& session) {
		return "live:" + std::to_string(session.log().size()) + ":" + std::to_string(session.surface().generation());
	}

	std::map<std::string, wanted_row> desired_sessions() const {
		std::map<std::string, wanted_row> desired;
		if (store_) {
			for (auto const& id : store_->list()) {
				auto session = store_->get(id);
				if (!session)
					continue;
				bool persisted = false;
				if (persistence_) {
					try {
						persisted = persistence_->revision(id).has_value();
					} catch (...) {
					}
				}
				desired[id.value] = wanted_row{live_revision(*session), true, persisted};
			}
		}
		if (persistence_) {
			for (auto const& id : persistence_->list()) {
				if (desired.contains(id.value))
					continue;
				auto revision = persistence_->revision(id);
				if (!revision)
					continue;
				desired[id.value] = wanted_row{std::move(*revision), false, true};
			}
		}
		return desired;
	}

	void reconcile() const {
		auto const desired = desired_sessions();
		std::map<std::string, indexed_row> indexed;
		{
			statement query(db_, "SELECT id, revision, live, persisted FROM indexed_sessions");
			while (query.step())
				indexed[query.column_text(0)] =
					indexed_row{query.column_text(1), query.column_int(2) != 0, query.column_int(3) != 0};
		}

		bool changed = false;
		exec_sql(db_, "BEGIN IMMEDIATE");
		try {
			for (auto const& [id, row] : indexed) {
				if (desired.contains(id))
					continue;
				delete_session(id);
				changed = true;
			}
			for (auto const& [id, want] : desired) {
				auto const found = indexed.find(id);
				if (found != indexed.end() && found->second.revision == want.revision &&
					found->second.live == want.live)
					continue;
				auto source = source_of(id, want.live);
				if (!source) {
					if (found != indexed.end()) {
						delete_session(id);
						changed = true;
					}
					continue;
				}
				delete_session(id);
				index_session(*source, want);
				changed = true;
			}
			if (changed) {
				exec_sql(db_, "UPDATE search_state SET generation = generation + 1 WHERE singleton = 1");
				++generation_;
			}
			exec_sql(db_, "COMMIT");
		} catch (...) {
			exec_sql(db_, "ROLLBACK");
			throw;
		}
	}

	std::optional<session_source> source_of(std::string const& id, bool live) const {
		if (live && store_) {
			if (auto session = store_->get(session_id{id}))
				return session_source{session->header(), session->log(), true, false};
		}
		if (persistence_) {
			try {
				if (auto stored = persistence_->read(session_id{id}))
					return session_source{stored->header, stored->events, false, true};
			} catch (...) {
			}
		}
		return std::nullopt;
	}

	void delete_session(std::string const& id) const {
		{
			statement query(db_, "DELETE FROM documents WHERE session_id = ?");
			query.bind({id});
			query.step();
		}
		{
			statement query(db_, "DELETE FROM indexed_sessions WHERE id = ?");
			query.bind({id});
			query.step();
		}
	}

	void index_session(session_source const& source, wanted_row const& want) const {
		auto const surfaces = araya::session_query::classify_surfaces(source.events);
		{
			statement insert(
				db_,
				"INSERT INTO documents(text, session_id, seq, type, time, surface)"
				" VALUES (?, ?, ?, ?, ?, ?)");
			for (std::size_t i = 0; i < source.events.size(); ++i) {
				auto text = araya::session_query::extract_event_text(source.events[i]);
				if (text.empty())
					continue;
				insert.bind({
					std::move(text),
					source.header.id.value,
					static_cast<std::int64_t>(source.events[i].seq),
					source.events[i].type,
					source.events[i].time,
					std::string(surface_name(surfaces[i])),
				});
				insert.step();
				insert.reset();
			}
		}
		{
			statement upsert(
				db_,
				"INSERT OR REPLACE INTO indexed_sessions("
				" id, live, persisted, revision, version, created_at, cwd, parent_session, delegation_depth,"
				" agent_preset) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
			std::vector<sql_value> values{
				source.header.id.value,
				static_cast<std::int64_t>(want.live ? 1 : 0),
				static_cast<std::int64_t>(want.persisted ? 1 : 0),
				want.revision,
				static_cast<std::int64_t>(source.header.version),
				source.header.created_at,
			};
			values.emplace_back(source.header.cwd ? sql_value{*source.header.cwd} : sql_value{nullptr});
			values.emplace_back(
				source.header.parent_session ? sql_value{source.header.parent_session->value} : sql_value{nullptr});
			values.emplace_back(static_cast<std::int64_t>(source.header.delegation_depth));
			values.emplace_back(
				source.header.agent_preset ? sql_value{*source.header.agent_preset} : sql_value{nullptr});
			upsert.bind(values);
			upsert.step();
		}
	}

	std::shared_ptr<session_query_service> reads_;
	std::shared_ptr<session_store> store_;
	std::shared_ptr<session_persistence> persistence_;
	engine_config config_;
	mutable sqlite3* db_ = nullptr;
	mutable bool opened_ = false;
	mutable std::uint64_t generation_ = 0;
};

struct session_query_sqlite_plugin : araya::plugin {
	explicit session_query_sqlite_plugin(araya::plugin_config const& config) {
		araya::plugin_config_view const view(config);
		if (auto value = view.try_get(path_key))
			config_.path = *value;
		if (auto value = view.try_get(open_at_key))
			config_.open_at = *value;
		if (auto value = view.try_get(journal_mode_key))
			config_.journal_mode = *value;
		if (auto value = view.try_get(limit_key))
			config_.limit = static_cast<int>(*value);
		if (auto value = view.try_get(max_limit_key))
			config_.max_limit = static_cast<int>(*value);
		if (auto value = view.try_get(snippet_chars_key))
			config_.snippet_chars = static_cast<int>(*value);
		if (config_.limit < 1 || config_.max_limit < config_.limit)
			throw std::invalid_argument("session-query-sqlite: invalid limit/maxLimit");
		if (config_.snippet_chars < 1)
			throw std::invalid_argument("session-query-sqlite: snippetChars must be positive");
		if (config_.open_at != "startup" && config_.open_at != "first-search" && config_.open_at != "never")
			throw std::invalid_argument("session-query-sqlite: openAt must be startup, first-search, or never");
	}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto store = ctx.require<session_store>(sessions_key).shared();
		std::shared_ptr<session_persistence> persistence;
		if (auto lease = ctx.find<session_persistence>(persistence_key))
			persistence = lease->shared();
		auto reads = araya::session_query::make_in_memory_engine(store, persistence);
		auto engine = std::make_shared<sqlite_engine>(store, persistence, config_, std::move(reads));
		if (config_.open_at == "startup")
			engine->warm();
		ctx.provide(session_query_key, std::shared_ptr<session_query_service>(std::move(engine)));
		co_return;
	}

private:
	engine_config config_;
};

std::unique_ptr<araya::plugin> make_session_query_sqlite(araya::plugin_config const& config) {
	return std::make_unique<session_query_sqlite_plugin>(config);
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"session.persistence", 1}, false, {}},
};
static const araya::provision_spec g_provs[]{{araya::service_id{"session-query", 1}}};
static const araya::plugin_descriptor
	g_descriptor{"session-query-sqlite", g_deps, g_provs, &make_session_query_sqlite, g_config};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::session_query_sqlite
