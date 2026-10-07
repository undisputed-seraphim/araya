#pragma once

#include "araya/lsp/lsp.hpp"
#include "araya/task.hpp"
#include "config.hpp"
#include "instance.hpp"

#include <boost/asio/any_io_executor.hpp>

#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace araya::lsp_stdio {

// One configured language server: pools one process per canonical workspace
// and runs transient-open navigation queries through it.
class stdio_lsp_provider : public araya::lsp::lsp_provider, public std::enable_shared_from_this<stdio_lsp_provider> {
public:
	stdio_lsp_provider(boost::asio::any_io_executor executor, server_config config);

	std::string_view id() const override { return config_.id; }
	std::map<std::string, std::string> const& extension_to_language() const override { return mapping_; }

	araya::task<araya::lsp::lsp_query_result>
	query(araya::lsp::lsp_provider_query const& request, std::stop_token stop) override;

	// Closes every pooled instance.
	void dispose();

private:
	std::shared_ptr<lsp_instance> instance_for(std::string const& key, host_workspace const& workspace);

	boost::asio::any_io_executor executor_;
	server_config config_;
	std::map<std::string, std::string> mapping_;
	std::map<std::string, std::shared_ptr<lsp_instance>> instances_;
	bool disposed_ = false;
};

} // namespace araya::lsp_stdio
