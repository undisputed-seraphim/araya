#include "provider.hpp"

#include "araya/lsp/lsp.hpp"
#include "host.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace araya::lsp_stdio {

stdio_lsp_provider::stdio_lsp_provider(boost::asio::any_io_executor executor, server_config config)
	: executor_(std::move(executor))
	, config_(std::move(config)) {
	for (auto const& [ext, language] : config_.extension_to_language)
		mapping_.emplace(ext, language);
}

std::shared_ptr<lsp_instance>
stdio_lsp_provider::instance_for(std::string const& key, host_workspace const& workspace) {
	auto const it = instances_.find(key);
	if (it != instances_.end())
		return it->second;
	auto instance = std::make_shared<lsp_instance>(executor_, config_, workspace);
	instance->start();
	instances_.emplace(key, instance);
	return instance;
}

araya::task<araya::lsp::lsp_query_result>
stdio_lsp_provider::query(araya::lsp::lsp_provider_query const& request, std::stop_token stop) {
	if (disposed_)
		throw araya::lsp::lsp_error("lsp-stdio provider is disposed", std::string(araya::lsp::error_code::disposed));

	host_workspace workspace;
	std::string error;
	if (!canonicalize_workspace(request.request.workspace_root, workspace, error))
		throw araya::lsp::lsp_error(std::move(error), std::string(araya::lsp::error_code::unavailable));
	host_source source;
	if (!read_host_source(request.request.file_path, workspace, config_.max_document_bytes, source, error))
		throw araya::lsp::lsp_error(std::move(error), std::string(araya::lsp::error_code::unavailable));

	std::string const key = workspace.canonical_path;
	auto instance = instance_for(key, workspace);
	araya::lsp::lsp_query_result result;
	bool retry = false;
	try {
		result = co_await instance->query(request, source, stop);
	} catch (...) {
		// A read-only query on a dead transport is retried once on a fresh process.
		if (!instance->dead())
			throw;
		retry = true;
	}
	if (retry) {
		instance->close();
		instances_.erase(key);
		auto fresh = instance_for(key, workspace);
		result = co_await fresh->query(request, source, stop);
	}
	co_return result;
}

void stdio_lsp_provider::dispose() {
	disposed_ = true;
	for (auto& [key, instance] : instances_)
		instance->close();
	instances_.clear();
}

} // namespace araya::lsp_stdio
