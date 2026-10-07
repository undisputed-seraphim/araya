#include "araya/lsp/lsp.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::lsp {
namespace {

std::string lower(std::string_view text) {
	std::string out;
	out.reserve(text.size());
	for (char const c : text)
		out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	return out;
}

// A well-formed normalized extension: a dot followed by one or more non-dot,
// non-separator chars.
bool valid_extension(std::string_view ext) {
	if (ext.size() < 2 || ext.front() != '.')
		return false;
	for (char const c : ext.substr(1)) {
		if (c == '.' || c == '/' || c == '\\')
			return false;
	}
	return true;
}

// Lowercase an extension and ensure it carries a leading dot.
std::string normalize_extension(std::string_view ext) {
	std::string lowered = lower(ext);
	return lowered.empty() || lowered.front() == '.' ? lowered : "." + lowered;
}

std::string trim(std::string_view text) {
	auto const first = text.find_first_not_of(" \t\r\n");
	if (first == std::string_view::npos)
		return {};
	auto const last = text.find_last_not_of(" \t\r\n");
	return std::string(text.substr(first, last - first + 1));
}

struct route {
	std::shared_ptr<lsp_provider> provider;
	std::string language_id;
};

class lsp_registry : public lsp_service {
public:
	araya::registration
	register_provider(araya::plugin_context& caller, std::shared_ptr<lsp_provider> provider) override {
		std::string const id = std::string(provider->id());
		if (trim(id).empty())
			throw lsp_error("an LSP provider id must be a non-empty string", std::string(error_code::invalid_provider));
		if (provider_ids_.contains(id))
			throw lsp_error(
				"an LSP provider with id \"" + id + "\" is already registered", std::string(error_code::conflict));

		auto const& mapping = provider->extension_to_language();
		if (mapping.empty())
			throw lsp_error(
				"LSP provider \"" + id + "\" registers no file extensions", std::string(error_code::invalid_provider));

		std::map<std::string, route> pending;
		for (auto const& [raw_ext, language_id] : mapping) {
			std::string const ext = normalize_extension(raw_ext);
			if (!valid_extension(ext))
				throw lsp_error(
					"LSP provider \"" + id + "\" maps an invalid extension \"" + raw_ext + "\"",
					std::string(error_code::invalid_provider));
			if (trim(language_id).empty())
				throw lsp_error(
					"LSP provider \"" + id + "\" maps extension \"" + ext + "\" to an empty language id",
					std::string(error_code::invalid_provider));
			if (pending.contains(ext))
				throw lsp_error(
					"LSP provider \"" + id + "\" maps extension \"" + ext + "\" more than once",
					std::string(error_code::invalid_provider));
			pending.emplace(ext, route{provider, language_id});
		}
		for (auto const& [ext, value] : pending) {
			if (routes_.contains(ext))
				throw lsp_error(
					"extension \"" + ext + "\" is already handled by another LSP provider",
					std::string(error_code::conflict));
		}

		return caller.effect([this, id, pending = std::move(pending)]() -> araya::cleanup_action {
			provider_ids_.insert(id);
			std::vector<std::string> extensions;
			extensions.reserve(pending.size());
			for (auto const& [ext, value] : pending) {
				routes_.emplace(ext, value);
				extensions.push_back(ext);
			}
			return [this, id, extensions = std::move(extensions)] {
				provider_ids_.erase(id);
				for (auto const& ext : extensions)
					routes_.erase(ext);
			};
		});
	}

	araya::task<lsp_query_result> query(lsp_query_request request, std::stop_token stop) override {
		auto const it = routes_.find(final_extension(request.file_path));
		if (it == routes_.end())
			throw lsp_error(
				"no LSP provider handles \"" + request.file_path + "\"", std::string(error_code::unavailable));
		lsp_provider_query provider_query{std::move(request), it->second.language_id};
		co_return co_await it->second.provider->query(provider_query, stop);
	}

private:
	std::set<std::string, std::less<>> provider_ids_;
	std::map<std::string, route, std::less<>> routes_;
};

std::unique_ptr<araya::plugin> make_lsp(araya::plugin_config const&) {
	struct lsp_plugin : araya::plugin {
		araya::task<void> apply(araya::plugin_context& ctx) override {
			std::shared_ptr<lsp_service> service = std::make_shared<lsp_registry>();
			ctx.provide(lsp_key, std::move(service));
			co_return;
		}
	};
	return std::make_unique<lsp_plugin>();
}

static constexpr std::span<araya::dependency_spec const> g_deps{};
static const araya::provision_spec g_provs[]{{araya::service_id{"lsp", 1}}};
static const araya::plugin_descriptor g_descriptor{"lsp", g_deps, g_provs, &make_lsp, {}};

} // namespace

std::string_view operation_name(lsp_operation operation) {
	switch (operation) {
	case lsp_operation::go_to_definition:
		return "textDocument/definition";
	case lsp_operation::find_references:
		return "textDocument/references";
	case lsp_operation::go_to_implementation:
		return "textDocument/implementation";
	case lsp_operation::hover:
		return "textDocument/hover";
	}
	return {};
}

std::string final_extension(std::string_view file_path) {
	auto const slash = file_path.find_last_of("/\\");
	std::string_view const base = slash == std::string_view::npos ? file_path : file_path.substr(slash + 1);
	auto const dot = base.find_last_of('.');
	// dot <= 0 covers both "no dot" and a leading-dot dotfile: neither has an extension.
	if (dot == std::string_view::npos || dot == 0)
		return {};
	return lower(base.substr(dot));
}

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::lsp
