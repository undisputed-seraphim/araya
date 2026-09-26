#include "araya/user-questions/user_questions.hpp"

#include "araya/plugin_context.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

namespace araya::user_questions {
namespace {

class user_questions_impl : public user_questions_service, public std::enable_shared_from_this<user_questions_impl> {
public:
	araya::registration register_answerer(araya::plugin_context& caller, answerer_fn answerer) override {
		auto const token = next_++;
		answerers_.emplace(token, std::move(answerer));
		std::weak_ptr<user_questions_impl> weak = weak_from_this();
		return caller.effect([weak, token]() -> araya::cleanup_action {
			return [weak, token] {
				if (auto impl = weak.lock())
					impl->answerers_.erase(token);
			};
		});
	}

	araya::task<answer> ask(ask_request request) override {
		if (request.questions.empty())
			throw std::runtime_error("ask_user_question requires at least one question");
		if (request.stop.stop_requested())
			throw std::runtime_error("ask_user_question was aborted before the user answered");
		if (answerers_.empty())
			throw std::runtime_error("no user-questions answerer accepted the request");
		auto const& answerer = answerers_.begin()->second;
		co_return answerer(request);
	}

private:
	std::map<std::uint64_t, answerer_fn> answerers_;
	std::uint64_t next_ = 1;
};

std::unique_ptr<araya::plugin> make_user_questions(araya::plugin_config const& config) {
	(void)config;
	struct user_questions_plugin : araya::plugin {
		araya::task<void> apply(araya::plugin_context& ctx) override {
			std::shared_ptr<user_questions_service> impl = std::make_shared<user_questions_impl>();
			ctx.provide(user_questions_key, std::move(impl));
			co_return;
		}
	};
	return std::make_unique<user_questions_plugin>();
}

static constexpr std::span<araya::dependency_spec const> g_deps{};
static const araya::provision_spec g_provs[]{{araya::service_id{"user-questions", 1}}};
static const araya::plugin_descriptor g_descriptor{"user-questions", g_deps, g_provs, &make_user_questions};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::user_questions
