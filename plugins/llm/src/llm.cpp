#include "araya/llm/llm.hpp"

#include <array>
#include <stdexcept>
#include <utility>

namespace araya::llm {
namespace {

// Indexed by llm_error_code; keep in enum order.
inline constexpr std::array<char const*, 14> k_error_code_names{
	"no_adapter",
	"duplicate_adapter",
	"auth",
	"rate_limit",
	"context_window_exceeded",
	"empty_response",
	"invalid_request",
	"quota",
	"server",
	"timeout",
	"transport",
	"aborted",
	"stream_closed",
	"malformed_response",
};

} // namespace

llm_error::llm_error(llm_failure failure)
	: std::runtime_error(std::move(failure.message))
	, failure_(std::move(failure)) {}

char const* llm_error::code_name(llm_error_code code) noexcept {
	auto const index = std::to_underlying(code);
	return index < k_error_code_names.size() ? k_error_code_names[index] : "?";
}

char const* llm_failure::code_string() const noexcept {
	return provider_code.empty() ? llm_error::code_name(code) : provider_code.c_str();
}

araya::registration llm_service::register_adapter(
	std::vector<std::string> providers,
	std::shared_ptr<llm_adapter> adapter,
	araya::plugin_context& caller) {
	for (auto const& provider : providers) {
		if (routes_.contains(provider)) {
			throw llm_error(llm_failure{
				llm_error_code::duplicate_adapter, "an adapter for provider '" + provider + "' is already registered"});
		}
	}
	return caller.effect([this, providers = std::move(providers), adapter = std::move(adapter)]() {
		for (auto const& provider : providers)
			routes_.insert_or_assign(provider, route{adapter});
		return [this, providers]() {
			for (auto const& provider : providers)
				routes_.erase(provider);
		};
	});
}

araya::task<void> llm_service::stream(generate_options const& options, chunk_sink const& sink) {
	if (options.provider.empty())
		throw std::invalid_argument("llm stream: empty provider");
	if (options.model.empty())
		throw std::invalid_argument("llm stream: empty model");
	auto const found = routes_.find(options.provider);
	if (found == routes_.end()) {
		throw llm_error(
			llm_failure{llm_error_code::no_adapter, "no adapter registered for provider '" + options.provider + "'"});
	}
	co_await found->second.adapter->stream(options, sink);
}

std::vector<std::string> llm_service::providers() const {
	std::vector<std::string> result;
	result.reserve(routes_.size());
	for (auto const& entry : routes_)
		result.push_back(entry.first);
	return result;
}

std::optional<std::string_view> llm_service::first_provider() const {
	if (routes_.empty())
		return std::nullopt;
	return std::string_view(routes_.begin()->first);
}

std::optional<model_info> llm_service::resolve_model(std::string_view provider, std::string_view model) const {
	auto const found = routes_.find(provider);
	if (found == routes_.end())
		return std::nullopt;
	return found->second.adapter->resolve_model(provider, model);
}

std::vector<model_info> llm_adapter::list_models(std::string_view) { return {}; }

model_info llm_adapter::resolve_model(std::string_view provider, std::string_view model) {
	model_info info;
	info.provider = std::string(provider);
	info.model = std::string(model);
	info.name = std::string(model);
	return info;
}

provider_info llm_adapter::describe_provider(std::string_view provider) {
	return provider_info{std::string(provider), std::string(provider)};
}

std::optional<retry_policy> llm_adapter::provider_retry_policy(std::string_view) { return std::nullopt; }

} // namespace araya::llm
