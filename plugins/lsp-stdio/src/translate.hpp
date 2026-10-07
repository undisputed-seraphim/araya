#pragma once

#include "araya/lsp/lsp.hpp"

#include <boost/json/value.hpp>

#include <optional>
#include <string>
#include <vector>

// Pure protocol translation: server capability checks and normalization of
// Location/LocationLink/Hover payloads into the seam's closed results.
namespace araya::lsp_stdio {

// Whether the server advertises the operation (a present boolean or options
// object; absent/false is unsupported).
bool supports_operation(boost::json::value const& capabilities, araya::lsp::lsp_operation operation);

// Whether the server's textDocumentSync permits the transient didOpen/didClose
// this host relies on.
bool supports_transient_open(boost::json::value const& capabilities);

// Normalizes a navigation result. Throws lsp_error (LSP_MALFORMED_RESPONSE).
std::vector<araya::lsp::lsp_location> normalize_locations(boost::json::value const& payload);

// Normalizes a hover result (nullopt when absent). Throws lsp_error.
std::optional<araya::lsp::lsp_hover> normalize_hover(boost::json::value const& payload);

} // namespace araya::lsp_stdio
