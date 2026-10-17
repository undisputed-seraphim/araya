#pragma once

#include "araya/llm-compat/compat.hpp"
#include "araya/logger/log.hpp"

#include <string_view>

// Shared request logging for the chat-completions and Messages paths: one
// debug line per request and one warn/info line per terminal failure. Bodies,
// headers, and keys are never logged; the endpoint goes through safe_url.
namespace araya::llm_compat {

inline void log_request_start(
	araya::logger::log_handle const& log,
	std::string_view provider,
	std::string_view model,
	std::string_view protocol,
	std::string_view base_url) {
	log.debug(
		"request provider={} model={} protocol={} endpoint={}",
		provider,
		model,
		protocol,
		araya::logger::safe_url(base_url));
}

inline void log_request_failure(
	araya::logger::log_handle const& log,
	std::string_view provider,
	std::string_view model,
	araya::llm::llm_failure const& failure) {
	if (failure.code == araya::llm::llm_error_code::aborted) {
		log.info("request provider={} model={} aborted", provider, model);
		return;
	}
	log.warn(
		"request provider={} model={} failed: {} status={} request_id='{}' retry_after={}ms",
		provider,
		model,
		failure.code_string(),
		failure.status,
		failure.request_id,
		failure.provider_retry_after ? failure.provider_retry_after->count() : 0);
}

} // namespace araya::llm_compat
