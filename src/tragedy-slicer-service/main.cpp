// tragedy-slicer-service — PrusaSlicer's engine as a local service for the
// tragedy-labs shell (ADR 0032). HTTP and a WebSocket on 127.0.0.1:7340.
//
//   tragedy-slicer-service [--port 7340] [--datadir <dir>] [--resources <dir>] [--jobs-dir <dir>]
//
// --datadir   the engine's data directory (user presets); the engine's own default
//             when omitted, which is the one its desktop app of this build uses.
// --resources the engine's resources/ (vendor presets, shapes); the fork's own
//             tree when omitted.
// --jobs-dir  where uploads and G-code of this run are kept; a directory under the
//             system's temporary directory when omitted.
#include "Server.hpp"
#include "Service.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

using namespace tragedy::slicer;

int main(int argc, char** argv)
{
    ServerOptions server_options;
    EngineOptions engine_options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << arg << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--port") server_options.port = static_cast<unsigned short>(std::stoi(value()));
        else if (arg == "--datadir") engine_options.data_dir = value();
        else if (arg == "--resources") engine_options.resources_dir = value();
        else if (arg == "--jobs-dir") server_options.jobs_dir = value();
        else {
            std::cerr << "usage: tragedy-slicer-service [--port N] [--datadir DIR] [--resources DIR] [--jobs-dir DIR]\n";
            return 2;
        }
    }
    if (server_options.jobs_dir.empty())
        server_options.jobs_dir = (std::filesystem::temp_directory_path() / "tragedy-slicer").string();
    std::filesystem::create_directories(server_options.jobs_dir);

    std::clog << "tragedy-slicer-service: loading the engine and its presets…" << std::endl;
    EngineThread engine(engine_options);

    boost::asio::io_context ioc{1};
    Jobs jobs;
    Server server(ioc, engine, jobs, server_options);
    jobs.on_slice_changed = [&server](const messages::Slice& slice) {
        server.broadcast(nlohmann::json{{"type", "slice"}, {"slice", slice}}.dump());
    };

    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int) {
        std::clog << "tragedy-slicer-service: stopping" << std::endl;
        ioc.stop();
    });

    server.run();
    std::clog << "tragedy-slicer-service: listening on http://127.0.0.1:" << server_options.port
              << " (jobs in " << server_options.jobs_dir << ")" << std::endl;
    ioc.run();
    return 0;
}
