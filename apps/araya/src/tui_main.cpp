#include "tui_state.hpp"
#include "tui_view.hpp"

#include "demo_plugins.hpp"

#include <ftxui/component/screen_interactive.hpp>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/RotatingFileSink.h>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

// The TUI surface: a thin driver over the shared app layer and the
// split presentation/state modules. Everything engine-side lives in
// app_core and tui_state; every renderer lives in tui_view.

int tui_main() {
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		std::cerr << "araya tui needs a terminal (interactive only)\n";
		return 2;
	}

	// The logger service adopts pre-created quill loggers by name, so
	// pre-create the known ones with a file sink - nothing writes over
	// the FTXUI canvas.
	quill::Backend::start();
	for (auto const* name : {"araya", "console", "watcher", "main", "timer"}) {
		quill::RotatingFileSinkConfig sink_config;
		(void)quill::Frontend::create_or_get_logger(
			name, std::make_shared<quill::RotatingFileSink>(std::filesystem::path{"araya-tui.log"}, sink_config));
	}

	araya::console_demo::init_demo_state(std::make_shared<araya::console_demo::demo_state>());

	araya::tui::shared_state sh;
	auto screen = ftxui::ScreenInteractive::Fullscreen();
	sh.wake = [&screen] { screen.PostEvent(ftxui::Event::Custom); };

	std::thread engine_thread([&sh] { araya::tui::run_engine(sh); });

	araya::tui::tui_theme theme{std::getenv("ARAYA_TUI_ASCII") != nullptr};
	std::string input_buffer;
	auto root = araya::tui::build_ui(
		sh,
		theme,
		input_buffer,
		screen,
		[&sh, &screen](std::string cmd) {
			{
				std::lock_guard lock(sh.command_mutex);
				sh.commands.push_back(std::move(cmd));
			}
			screen.PostEvent(ftxui::Event::Custom);
		},
		[&screen] { screen.Exit(); });

	// Exit is Ctrl+D (or the quit command); keep FTXUI's default Ctrl+C
	// handler off - the input swallows Ctrl+C instead.
	screen.ForceHandleCtrlC(false);
	screen.Loop(root);

	// Clean teardown: stop the engine, let it retire the tree on the
	// strand, and join before the process exits.
	sh.quit.store(true, std::memory_order_release);
	engine_thread.join();
	quill::Backend::stop();
	return 0;
}
