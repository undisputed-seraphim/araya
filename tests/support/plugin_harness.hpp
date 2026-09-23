#pragma once

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/task.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <memory>
#include <utility>

// The shared runtime test harness for plugin tests: an io_context plus
// a runtime on its executor, a run() that drives one coroutine to
// completion, and a component_spec factory for the runtime's mount().
namespace araya_test {

struct plugin_harness {
	boost::asio::io_context io;
	std::shared_ptr<araya::runtime> rt = std::make_shared<araya::runtime>(io.get_executor());

	template <typename Fn>
	void run(Fn&& fn) {
		struct driver {
			std::decay_t<Fn> fn;
			plugin_harness* self;
			araya::task<void> operator()() { co_await fn(*self->rt); }
		};
		boost::asio::co_spawn(io.get_executor(), driver{std::forward<Fn>(fn), this}, boost::asio::detached);
		io.run();
		io.restart();
	}

	araya::component_spec spec(araya::plugin_descriptor const* d, araya::plugin_config cfg = {}) {
		return araya::component_spec{
			std::shared_ptr<araya::plugin_descriptor>(const_cast<araya::plugin_descriptor*>(d), [](auto*) {}),
			std::move(cfg),
			nullptr,
			""};
	}
};

} // namespace araya_test
