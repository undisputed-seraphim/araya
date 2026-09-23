#pragma once

namespace araya::util {

// The classic std::visit overload set.
template <class... Ts>
struct overloaded : Ts... {
	using Ts::operator()...;
};

template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

} // namespace araya::util
