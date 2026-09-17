#include "araya/activation.hpp"

#include "araya/detail/assert.hpp"
#include "araya/detail/fiber.hpp"

namespace araya {

activation::activation(std::shared_ptr<context> scope)
    : id(detail::next_fiber_id()),
      scope(std::move(scope)),
      effects(std::make_shared<effect_stack>()),
      stop_source(std::make_shared<std::stop_source>()) {}

activation::~activation() = default;

std::stop_token activation::stop_token() const noexcept {
    return stop_source ? stop_source->get_token()
                       : std::stop_token{};
}

void activation::teardown() noexcept {
    ARAYA_ASSERT_NOTHROW(state != fiber_state::inactive);
    if (effects) {
        effects->run_all();
        if (!error && effects->error)
            error = effects->error;
    }
    ARAYA_ASSERT_NOTHROW(!effects || effects->size() == 0);
}

}  // namespace araya
