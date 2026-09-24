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

namespace {

// The exit summary, printed once the UI has restored the terminal: the
// block wordmark plus the session title and the resume command. Every
// connection-free host print, so it goes to plain stdout.
void print_session_summary(std::string_view title, std::string_view id) {
	bool ascii = std::getenv("ARAYA_TUI_ASCII") != nullptr;
	std::cout << '\n';
	if (ascii) {
		std::cout << "   a r a y a\n";
	} else {
		std::cout << "   \u2584\u2580\u2580\u2584 \u2588\u2580\u2580\u2584 \u2584\u2580\u2580\u2584 \u2588  \u2588 "
					 "\u2584\u2580\u2580\u2584\n";
		std::cout << "   \u2588\u2584\u2584\u2588 \u2588  \u2588 \u2588\u2584\u2584\u2588 \u2588\u2584\u2584\u2588 "
					 "\u2588\u2584\u2584\u2588\n";
		std::cout << "   \u2580  \u2580 \u2580  \u2580 \u2580  \u2580  \u2580\u2580\u2580 \u2580  \u2580\n";
	}
	std::cout << '\n';
	std::cout << "   Session   " << (title.empty() ? id : title) << '\n';
	std::cout << "   Continue  araya tui -s " << id << '\n';
	std::cout << '\n' << std::flush;
}

} // namespace

int tui_main(std::string session) {
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

	std::thread engine_thread([&sh, &session] { araya::tui::run_engine(sh, session); });

	araya::tui::tui_theme theme{std::getenv("ARAYA_TUI_ASCII") != nullptr};
	araya::tui::ui_state ui;
	std::string input_buffer;
	auto root = araya::tui::build_ui(
		sh,
		theme,
		ui,
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
	// strand (which flushes the session), and join before printing the
	// summary, so the id and title are final.
	sh.quit.store(true, std::memory_order_release);
	engine_thread.join();

	auto snap = sh.snap.load(std::memory_order_acquire);
	if (snap && !snap->session.empty() && snap->session != "no session")
		print_session_summary(snap->title, snap->session);

	quill::Backend::stop();
	return 0;
}
