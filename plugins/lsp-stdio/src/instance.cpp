#include "instance.hpp"

#include "translate.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace araya::lsp_stdio {
namespace {

boost::json::object client_capabilities() {
	boost::json::object general;
	general["positionEncodings"] = boost::json::array{"utf-16"};
	boost::json::object workspace;
	workspace["workspaceFolders"] = true;
	workspace["configuration"] = true;
	boost::json::object synchronization;
	synchronization["dynamicRegistration"] = false;
	boost::json::object hover;
	hover["contentFormat"] = boost::json::array{"markdown", "plaintext"};
	boost::json::object definition;
	definition["linkSupport"] = true;
	boost::json::object implementation;
	implementation["linkSupport"] = true;
	boost::json::object text_document;
	text_document["synchronization"] = std::move(synchronization);
	text_document["hover"] = std::move(hover);
	text_document["definition"] = std::move(definition);
	text_document["implementation"] = std::move(implementation);
	text_document["references"] = boost::json::object{};
	boost::json::object capabilities;
	capabilities["general"] = std::move(general);
	capabilities["workspace"] = std::move(workspace);
	capabilities["textDocument"] = std::move(text_document);
	return capabilities;
}

} // namespace

lsp_instance::lsp_instance(boost::asio::any_io_executor executor, server_config config, host_workspace workspace)
	: executor_(std::move(executor))
	, config_(std::move(config))
	, workspace_(std::move(workspace))
	, ready_(executor_, 1) {
	config_.cwd = workspace_.canonical_path;
	connection_ = std::make_shared<lsp_connection>(executor_, config_);
}

void lsp_instance::start() {
	boost::asio::co_spawn(executor_, [self = shared_from_this()] { return self->initialize(); }, boost::asio::detached);
}

araya::task<void> lsp_instance::initialize() {
	std::exception_ptr error;
	try {
		connection_->start();
		boost::json::object folder;
		folder["uri"] = workspace_.file_url;
		folder["name"] = "workspace";
		boost::json::object params;
		params["processId"] = nullptr;
		params["rootUri"] = workspace_.file_url;
		params["workspaceFolders"] = boost::json::array{std::move(folder)};
		params["capabilities"] = client_capabilities();
		params["initializationOptions"] = config_.initialization_options;
		boost::json::value const result = co_await connection_->request("initialize", std::move(params), {});
		if (auto const* object = result.if_object()) {
			if (auto const* capabilities = object->if_contains("capabilities"))
				capabilities_ = *capabilities;
		}
		if (auto const* object = capabilities_.if_object()) {
			if (auto const* encoding = object->if_contains("positionEncoding");
				encoding && encoding->is_string() && encoding->as_string() != "utf-16")
				throw std::runtime_error("server negotiated unsupported position encoding");
		}
		co_await connection_->notify("initialized", boost::json::object{});
	} catch (...) {
		error = std::current_exception();
	}
	ready_done_ = true;
	ready_error_ = error;
	ready_.try_send(boost::system::error_code{}, error);
}

araya::task<void> lsp_instance::ready(std::stop_token stop) {
	if (disposed_)
		throw araya::lsp::lsp_error("LSP instance was disposed", std::string(araya::lsp::error_code::disposed));
	if (stop.stop_requested())
		throw araya::lsp::lsp_error("LSP query was cancelled", std::string(araya::lsp::error_code::disposed));
	if (!ready_done_) {
		try {
			(void)co_await ready_.async_receive(boost::asio::use_awaitable);
		} catch (std::exception const&) {
			throw araya::lsp::lsp_error(
				"LSP instance closed before initialization", std::string(araya::lsp::error_code::disposed));
		}
	}
	if (ready_error_)
		std::rethrow_exception(ready_error_);
	if (disposed_)
		throw araya::lsp::lsp_error("LSP instance was disposed", std::string(araya::lsp::error_code::disposed));
}

araya::task<void> lsp_instance::lock() {
	if (!locked_) {
		locked_ = true;
		co_return;
	}
	auto waiter = std::make_shared<done_channel>(executor_, 1);
	waiters_.push_back(waiter);
	boost::system::error_code ec;
	co_await waiter->async_receive(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
	if (disposed_)
		throw araya::lsp::lsp_error("LSP instance was disposed", std::string(araya::lsp::error_code::disposed));
}

void lsp_instance::unlock() {
	if (waiters_.empty()) {
		locked_ = false;
		return;
	}
	auto waiter = waiters_.front();
	waiters_.pop_front();
	waiter->try_send(boost::system::error_code{});
}

araya::task<araya::lsp::lsp_query_result>
lsp_instance::query(araya::lsp::lsp_provider_query const& request, host_source const& source, std::stop_token stop) {
	if (disposed_)
		throw araya::lsp::lsp_error("LSP instance was disposed", std::string(araya::lsp::error_code::disposed));
	co_await lock();
	struct unlock_guard {
		lsp_instance* self;
		~unlock_guard() { self->unlock(); }
	} guard{this};
	co_await ready(stop);
	if (!supports_operation(capabilities_, request.request.operation))
		throw araya::lsp::lsp_error(
			"server does not support " + std::string(araya::lsp::operation_name(request.request.operation)),
			std::string(araya::lsp::error_code::unsupported_operation));
	if (!supports_transient_open(capabilities_))
		throw araya::lsp::lsp_error(
			"server does not support the transient textDocument/didOpen this host requires",
			std::string(araya::lsp::error_code::unsupported_operation));

	boost::json::object open_document;
	open_document["uri"] = source.file_url;
	open_document["languageId"] = request.language_id;
	open_document["version"] = 1;
	open_document["text"] = source.text;
	boost::json::object open_params;
	open_params["textDocument"] = std::move(open_document);
	co_await connection_->notify("textDocument/didOpen", std::move(open_params));

	araya::lsp::lsp_query_result result;
	std::exception_ptr error;
	try {
		boost::json::object position;
		position["line"] = request.request.position.line;
		position["character"] = request.request.position.character;
		boost::json::object document;
		document["uri"] = source.file_url;
		boost::json::object params;
		params["textDocument"] = std::move(document);
		params["position"] = std::move(position);
		if (request.request.operation == araya::lsp::lsp_operation::find_references) {
			boost::json::object context;
			context["includeDeclaration"] = true;
			params["context"] = std::move(context);
		}
		boost::json::value const payload = co_await connection_->request(
			std::string(araya::lsp::operation_name(request.request.operation)), std::move(params), stop);
		if (request.request.operation == araya::lsp::lsp_operation::hover) {
			result.type = araya::lsp::lsp_query_result::kind::hover;
			result.hover = normalize_hover(payload);
		} else {
			result.type = araya::lsp::lsp_query_result::kind::locations;
			result.locations = normalize_locations(payload);
			result.resolved_workspace_uri = workspace_.file_url;
		}
	} catch (...) {
		error = std::current_exception();
	}

	if (!disposed_ && !connection_->failed()) {
		boost::json::object close_document;
		close_document["uri"] = source.file_url;
		boost::json::object close_params;
		close_params["textDocument"] = std::move(close_document);
		try {
			co_await connection_->notify("textDocument/didClose", std::move(close_params));
		} catch (...) {
			// The instance can no longer be trusted; the provider evicts it.
			close();
		}
	}
	if (error)
		std::rethrow_exception(error);
	co_return result;
}

bool lsp_instance::dead() const { return disposed_ || (connection_ && connection_->failed()); }

void lsp_instance::close() {
	if (disposed_)
		return;
	disposed_ = true;
	if (!ready_done_) {
		ready_done_ = true;
		ready_error_ = std::make_exception_ptr(
			araya::lsp::lsp_error("LSP instance was disposed", std::string(araya::lsp::error_code::disposed)));
		ready_.try_send(boost::system::error_code{}, ready_error_);
	}
	if (connection_)
		connection_->close();
	for (auto& waiter : waiters_)
		waiter->try_send(boost::system::error_code{});
	waiters_.clear();
}

} // namespace araya::lsp_stdio
