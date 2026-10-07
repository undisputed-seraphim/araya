#pragma once

#include "araya/effects.hpp"
#include "araya/plugin.hpp"
#include "araya/plugin_context.hpp"
#include "araya/service.hpp"
#include "araya/task.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The LSP capability seam: a provider registry keyed by file extension and four
// normalized read-only navigation operations. A feature-replication of the
// deepseek-harness `@deepseek-ai/dsh-lsp`. Providers register capabilities (not
// tools); the model-facing surface is `tool-lsp` (M16).
//
// The seam exposes exactly goToDefinition/findReferences/goToImplementation/
// hover and no JSON-RPC escape hatch. Positions and ranges are zero-based
// UTF-16, matching the protocol; the model-facing tool owns the one-based
// cursor convention.
namespace araya::lsp {

// The four semantic queries. A closed set: adding one is a source-wide change.
enum class lsp_operation {
	go_to_definition,
	find_references,
	go_to_implementation,
	hover,
};

// The wire method name for one operation.
std::string_view operation_name(lsp_operation operation);

// A zero-based UTF-16 cursor coordinate.
struct lsp_position {
	std::uint32_t line = 0;
	std::uint32_t character = 0;
};

// A zero-based UTF-16 half-open range [start, end).
struct lsp_range {
	lsp_position start;
	lsp_position end;
};

// One resolved location: a document URI and a range within it.
struct lsp_location {
	std::string uri;
	lsp_range range;
};

// Normalized hover content, or none.
struct lsp_hover {
	std::string contents;
	std::optional<lsp_range> range;
};

// A caller's normalized query. Every field is required.
struct lsp_query_request {
	lsp_operation operation = lsp_operation::go_to_definition;
	std::string file_path;
	lsp_position position;
	std::string workspace_root;
};

// A query as a provider receives it: the caller request plus the language id
// the seam derived from the provider's extension mapping.
struct lsp_provider_query {
	lsp_query_request request;
	std::string language_id;
};

// The closed result union: navigation normalizes to `locations`, hover to a
// value or none. `resolved_workspace_uri` is the provider's canonical `file:`
// URI for the workspace root (used to relativize location URIs).
struct lsp_query_result {
	enum class kind { locations, hover };
	kind type = kind::locations;
	std::vector<lsp_location> locations;
	std::string resolved_workspace_uri;
	std::optional<lsp_hover> hover;
};

// Stable failure codes callers route on instead of parsing the message.
namespace error_code {
inline constexpr std::string_view invalid_provider = "LSP_INVALID_PROVIDER";
inline constexpr std::string_view conflict = "LSP_CONFLICT";
inline constexpr std::string_view unavailable = "LSP_UNAVAILABLE";
inline constexpr std::string_view disposed = "LSP_DISPOSED";
inline constexpr std::string_view unsupported_operation = "LSP_UNSUPPORTED_OPERATION";
inline constexpr std::string_view malformed_response = "LSP_MALFORMED_RESPONSE";
inline constexpr std::string_view workspace_required = "LSP_WORKSPACE_REQUIRED";
} // namespace error_code

// A structured LSP failure carrying a stable code.
class lsp_error : public std::runtime_error {
public:
	lsp_error(std::string message, std::string code)
		: std::runtime_error(std::move(message))
		, code_(std::move(code)) {}

	std::string const& code() const noexcept { return code_; }

private:
	std::string code_;
};

// A language-server backend registered on the `lsp` service.
class lsp_provider {
public:
	virtual ~lsp_provider() = default;

	// A stable, non-empty provider id.
	virtual std::string_view id() const = 0;

	// Lowercase leading-dot extension -> LSP language id.
	virtual std::map<std::string, std::string> const& extension_to_language() const = 0;

	// Run one selected query. The seam has already chosen this provider and
	// derived the language id. `findReferences` must include declarations.
	virtual araya::task<lsp_query_result> query(lsp_provider_query const& request, std::stop_token stop) = 0;
};

// The `lsp` service: provider registration/selection and normalized queries.
class lsp_service {
public:
	virtual ~lsp_service() = default;

	// Registers a provider, atomically reserving its id and every extension.
	// Any invalid input or conflict publishes nothing and throws `lsp_error`;
	// the registration is owned by `caller` (disposal releases everything).
	virtual araya::registration
	register_provider(araya::plugin_context& caller, std::shared_ptr<lsp_provider> provider) = 0;

	// Selects a provider by the file's extension and runs one query. No match
	// throws `lsp_error` with `LSP_UNAVAILABLE`.
	virtual araya::task<lsp_query_result> query(lsp_query_request request, std::stop_token stop) = 0;
};

inline constexpr araya::service_key<lsp_service> lsp_key{"lsp", 1};

// The file's final extension as a normalized, lowercase, leading-dot key
// (e.g. `Foo.TS` -> `.ts`, `foo.d.ts` -> `.ts`). Returns "" for no extension
// or a leading-dot dotfile, which no route matches.
std::string final_extension(std::string_view file_path);

// The plugin descriptor: no dependencies, provides `lsp`.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::lsp
