#include "translate.hpp"

#include "araya/lsp/lsp.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace araya::lsp_stdio {
namespace {

araya::lsp::lsp_error malformed(std::string message) {
	return araya::lsp::lsp_error(std::move(message), std::string(araya::lsp::error_code::malformed_response));
}

bool nonneg_int(boost::json::value const& value, std::uint32_t& out) {
	if (value.is_int64() && value.as_int64() >= 0) {
		out = static_cast<std::uint32_t>(value.as_int64());
		return true;
	}
	if (value.is_uint64()) {
		out = static_cast<std::uint32_t>(value.as_uint64());
		return true;
	}
	return false;
}

bool parse_position(boost::json::value const& value, araya::lsp::lsp_position& out) {
	auto const* object = value.if_object();
	if (!object)
		return false;
	auto const* line = object->if_contains("line");
	auto const* character = object->if_contains("character");
	return line && character && nonneg_int(*line, out.line) && nonneg_int(*character, out.character);
}

bool parse_range(boost::json::value const& value, araya::lsp::lsp_range& out) {
	auto const* object = value.if_object();
	if (!object)
		return false;
	auto const* start = object->if_contains("start");
	auto const* end = object->if_contains("end");
	return start && end && parse_position(*start, out.start) && parse_position(*end, out.end);
}

bool supports_capability(boost::json::value const& value) {
	if (value.is_bool())
		return value.as_bool();
	if (value.is_object())
		return true;
	return false;
}

std::string render_marked_string(boost::json::value const& value) {
	if (value.is_string())
		return std::string(value.as_string());
	if (auto const* object = value.if_object()) {
		auto const* language = object->if_contains("language");
		auto const* text = object->if_contains("value");
		if (language && language->is_string() && text && text->is_string())
			return "```" + std::string(language->as_string()) + "\n" + std::string(text->as_string()) + "\n```";
	}
	throw malformed("LSP hover contents contained a malformed MarkedString");
}

std::string render_hover_contents(boost::json::value const& contents) {
	if (contents.is_string())
		return std::string(contents.as_string());
	if (auto const* array = contents.if_array()) {
		std::string joined;
		for (std::size_t i = 0; i < array->size(); ++i) {
			if (i)
				joined += "\n\n";
			joined += render_marked_string((*array)[i]);
		}
		return joined;
	}
	if (auto const* object = contents.if_object()) {
		if (auto const* kind = object->if_contains("kind");
			kind && kind->is_string() && (kind->as_string() == "markdown" || kind->as_string() == "plaintext")) {
			auto const* value = object->if_contains("value");
			if (!value || !value->is_string())
				throw malformed("LSP hover MarkupContent value was not a string");
			return std::string(value->as_string());
		}
		return render_marked_string(contents);
	}
	throw malformed("LSP hover contents were not MarkupContent, MarkedString, or an array");
}

} // namespace

bool supports_operation(boost::json::value const& capabilities, araya::lsp::lsp_operation operation) {
	auto const* object = capabilities.if_object();
	if (!object)
		return false;
	std::string_view field;
	switch (operation) {
	case araya::lsp::lsp_operation::go_to_definition:
		field = "definitionProvider";
		break;
	case araya::lsp::lsp_operation::find_references:
		field = "referencesProvider";
		break;
	case araya::lsp::lsp_operation::go_to_implementation:
		field = "implementationProvider";
		break;
	case araya::lsp::lsp_operation::hover:
		field = "hoverProvider";
		break;
	}
	auto const* value = object->if_contains(field);
	return value && supports_capability(*value);
}

bool supports_transient_open(boost::json::value const& capabilities) {
	auto const* object = capabilities.if_object();
	if (!object)
		return false;
	auto const* sync = object->if_contains("textDocumentSync");
	if (!sync)
		return false;
	if (sync->is_int64()) {
		auto const kind = sync->as_int64();
		return kind == 1 || kind == 2;
	}
	if (auto const* options = sync->if_object()) {
		auto const* open_close = options->if_contains("openClose");
		return open_close && open_close->is_bool() && open_close->as_bool();
	}
	return false;
}

std::vector<araya::lsp::lsp_location> normalize_locations(boost::json::value const& payload) {
	std::vector<araya::lsp::lsp_location> locations;
	if (payload.is_null())
		return locations;
	std::vector<boost::json::value const*> elements;
	if (auto const* array = payload.if_array()) {
		for (auto const& entry : *array)
			elements.push_back(&entry);
	} else {
		elements.push_back(&payload);
	}
	for (auto const* element : elements) {
		auto const* object = element->if_object();
		if (!object)
			throw malformed("LSP navigation result contained a non-object entry");
		auto const* target_uri = object->if_contains("targetUri");
		auto const* selection = object->if_contains("targetSelectionRange");
		if (target_uri && target_uri->is_string() && selection) {
			araya::lsp::lsp_range range;
			if (!parse_range(*selection, range))
				throw malformed("LSP LocationLink had a malformed selection range");
			locations.push_back(araya::lsp::lsp_location{std::string(target_uri->as_string()), range});
			continue;
		}
		auto const* uri = object->if_contains("uri");
		auto const* range_value = object->if_contains("range");
		if (uri && uri->is_string() && range_value) {
			araya::lsp::lsp_range range;
			if (!parse_range(*range_value, range))
				throw malformed("LSP Location had a malformed range");
			locations.push_back(araya::lsp::lsp_location{std::string(uri->as_string()), range});
			continue;
		}
		throw malformed("LSP navigation result contained neither a Location nor a LocationLink");
	}
	return locations;
}

std::optional<araya::lsp::lsp_hover> normalize_hover(boost::json::value const& payload) {
	if (payload.is_null())
		return std::nullopt;
	auto const* object = payload.if_object();
	if (!object)
		throw malformed("LSP hover result was not an object");
	auto const* contents = object->if_contains("contents");
	if (!contents)
		throw malformed("LSP hover result had no contents");
	std::string const rendered = render_hover_contents(*contents);
	if (rendered.empty())
		return std::nullopt;
	araya::lsp::lsp_hover hover;
	hover.contents = rendered;
	if (auto const* range_value = object->if_contains("range")) {
		araya::lsp::lsp_range range;
		if (!parse_range(*range_value, range))
			throw malformed("LSP hover result contained a malformed range");
		hover.range = range;
	}
	return hover;
}

} // namespace araya::lsp_stdio
