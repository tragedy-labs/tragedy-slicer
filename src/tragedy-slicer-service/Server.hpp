// HTTP and WebSocket on 127.0.0.1 — see Server.cpp for the routes and the rules.
#pragma once

#include "Service.hpp"

#include <boost/asio/io_context.hpp>

#include <set>
#include <string>

namespace tragedy::slicer {

struct ServerOptions
{
    unsigned short port{7340};
    /// Where uploads and G-code live for this run.
    std::string jobs_dir;
};

class WsSession;

class Server
{
public:
    Server(boost::asio::io_context& ioc, EngineThread& engine, Jobs& jobs, ServerOptions options);
    ~Server();

    void run();

    /// Send a text frame to every open WebSocket. Any thread.
    void broadcast(const std::string& text);

    /// The sessions reach the routing and the registry through this; public for
    /// them, opaque to everyone else.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace tragedy::slicer
