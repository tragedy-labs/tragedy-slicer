// The service's edge. Rules, each for a reason:
//
// - 127.0.0.1 only, and a request that carries an `Origin` must come from a page
//   served by this machine (`http(s)://localhost` or `127.0.0.1`, any port) — the
//   workspace service's rule. A page from another site cannot slice with this
//   machine's engine or read its files. No `Origin` is not a browser page (curl, an
//   agent) and is let through.
// - Nothing that needs the engine runs on the I/O thread. A handler posts its work
//   to the engine thread and answers from there, so a slice in progress never
//   blocks the WebSocket that reports it.
// - Every answer is a message from contracts/schema/v1/slicer, or `{error}` with the
//   status, so the shell has one shape to read per route.
//
// Routes:
//   GET    /health                    engine.json
//   GET    /presets?<selection>       preset-choices.json (selection as query keys;
//                                     tools and materials comma-separated)
//   GET    /config?<selection>        config.json
//   POST   /models?name=<file>        body = the file's bytes → model.json (201)
//   GET    /models/<id>               model.json
//   POST   /slices                    slice-request.json → slice.json (202)
//   GET    /slices                    [slice.json]
//   GET    /slices/<id>               slice.json
//   GET    /slices/<id>/gcode         text/plain, the G-code
//   DELETE /slices/<id>               cancel while running; forget otherwise (204)
//   GET    /events                    WebSocket; each frame {"type":"slice","slice":slice.json}
#include "Server.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;
using json          = nlohmann::json;

namespace tragedy::slicer {

namespace {

using Request  = http::request<http::string_body>;
using Response = http::response<http::string_body>;
using Reply    = std::function<void(http::message_generator&&)>;

std::string new_id()
{
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static const char* digits = "0123456789abcdef";
    std::string out(16, '0');
    std::uint64_t v = rng();
    for (char& c : out) {
        c = digits[v & 15];
        v >>= 4;
    }
    return out;
}

bool local_origin(std::string_view origin)
{
    std::string_view rest;
    if (origin.starts_with("http://")) rest = origin.substr(7);
    else if (origin.starts_with("https://")) rest = origin.substr(8);
    else return false;
    const std::string_view host = rest.substr(0, rest.find(':'));
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

std::string percent_decode(std::string_view in)
{
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') out += ' ';
        else if (in[i] == '%' && i + 2 < in.size() + 0 && i + 2 <= in.size() - 1 + 0) {
            out += static_cast<char>(std::stoi(std::string(in.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else out += in[i];
    }
    return out;
}

std::map<std::string, std::string> query_of(std::string_view target)
{
    std::map<std::string, std::string> out;
    const size_t q = target.find('?');
    if (q == std::string_view::npos) return out;
    std::string_view rest = target.substr(q + 1);
    while (!rest.empty()) {
        const size_t amp = rest.find('&');
        const std::string_view pair = rest.substr(0, amp);
        const size_t eq = pair.find('=');
        if (eq != std::string_view::npos) out[percent_decode(pair.substr(0, eq))] = percent_decode(pair.substr(eq + 1));
        if (amp == std::string_view::npos) break;
        rest = rest.substr(amp + 1);
    }
    return out;
}

std::vector<std::string> split_commas(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) out.push_back(item);
    return out;
}

messages::PresetSelection selection_of(const std::map<std::string, std::string>& query)
{
    messages::PresetSelection s;
    if (auto it = query.find("printer"); it != query.end()) s.printer = it->second;
    if (auto it = query.find("printerPreset"); it != query.end()) s.printerPreset = it->second;
    if (auto it = query.find("print"); it != query.end()) s.print = it->second;
    if (auto it = query.find("tools"); it != query.end()) s.tools = split_commas(it->second);
    if (auto it = query.find("materials"); it != query.end()) s.materials = split_commas(it->second);
    return s;
}

void cors(const Request& req, http::fields& fields)
{
    if (auto it = req.find(http::field::origin); it != req.end()) {
        fields.set(http::field::access_control_allow_origin, it->value());
        fields.set(http::field::access_control_allow_methods, "GET, POST, DELETE, OPTIONS");
        fields.set(http::field::access_control_allow_headers, "Content-Type");
        fields.set(http::field::vary, "Origin");
    }
}

Response text_response(const Request& req, http::status status, std::string body, std::string_view type)
{
    Response res{status, req.version()};
    res.set(http::field::server, "tragedy-slicer");
    res.set(http::field::content_type, type);
    res.keep_alive(req.keep_alive());
    cors(req, res);
    res.body() = std::move(body);
    res.prepare_payload();
    return res;
}

Response json_response(const Request& req, http::status status, const json& body)
{
    return text_response(req, status, body.dump(), "application/json");
}

Response error_response(const Request& req, int status, const std::string& message)
{
    return json_response(req, static_cast<http::status>(status), json{{"error", message}});
}

template <typename T>
Response outcome_response(const Request& req, const Outcome<T>& outcome, http::status ok = http::status::ok)
{
    if (!outcome) return error_response(req, outcome.failure->status, outcome.failure->message);
    return json_response(req, ok, json(*outcome.value));
}

std::string filename_only(std::string name)
{
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    return name.empty() ? "model" : name;
}

} // namespace

// --- WebSocket ------------------------------------------------------------------

// The open sockets, owned jointly by the server and every session: the
// io_context releases its pending operations — and the sessions they hold —
// after the server is gone, and a session's last act is to leave this set.
using WsRegistry = std::shared_ptr<std::set<WsSession*>>;

class WsSession : public std::enable_shared_from_this<WsSession>
{
public:
    WsSession(tcp::socket&& socket, WsRegistry registry) :
        m_ws(std::move(socket)), m_registry(std::move(registry))
    {}

    void run(Request&& req)
    {
        m_ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        m_ws.set_option(websocket::stream_base::decorator([](websocket::response_type& res) {
            res.set(http::field::server, "tragedy-slicer");
        }));
        m_ws.async_accept(req, [self = shared_from_this()](beast::error_code ec) {
            if (ec) return;
            self->m_registry->insert(self.get());
            self->read();
        });
    }

    void send(const std::string& text)
    {
        m_queue.push_back(text);
        if (m_queue.size() == 1) write();
    }

    ~WsSession() { m_registry->erase(this); }

private:
    void read()
    {
        m_ws.async_read(m_buffer, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) {
                self->m_registry->erase(self.get());
                return;
            }
            // Nothing is read from the client yet: the stroke protocol of step 4 is
            // what will arrive here. Frames are consumed so a close is noticed.
            self->m_buffer.consume(self->m_buffer.size());
            self->read();
        });
    }

    void write()
    {
        m_ws.text(true);
        m_ws.async_write(net::buffer(m_queue.front()), [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) {
                self->m_registry->erase(self.get());
                return;
            }
            self->m_queue.pop_front();
            if (!self->m_queue.empty()) self->write();
        });
    }

    websocket::stream<beast::tcp_stream> m_ws;
    beast::flat_buffer m_buffer;
    std::deque<std::string> m_queue;
    WsRegistry m_registry;
};

// --- HTTP -----------------------------------------------------------------------

struct Server::Impl
{
    net::io_context& ioc;
    EngineThread& engine;
    Jobs& jobs;
    ServerOptions options;
    tcp::acceptor acceptor;
    WsRegistry sockets{std::make_shared<std::set<WsSession*>>()};

    Impl(net::io_context& ioc, EngineThread& engine, Jobs& jobs, ServerOptions options) :
        ioc(ioc), engine(engine), jobs(jobs), options(std::move(options)), acceptor(ioc)
    {}

    void accept();
    void handle(Request&& req, const Reply& reply);

    // Routes that need the engine answer from its thread; the reply is posted back
    // to the I/O thread, which is the only one that touches a socket.
    template <typename Work>
    void on_engine(Request req, const Reply& reply, Work work)
    {
        engine.post([this, req = std::move(req), reply, work = std::move(work)](Engine& e) mutable {
            Response res = work(e, req);
            net::post(ioc, [reply, res = std::move(res)]() mutable { reply(std::move(res)); });
        });
    }

    void start_slice(const std::shared_ptr<SliceJob>& job);
};

class HttpSession : public std::enable_shared_from_this<HttpSession>
{
public:
    HttpSession(tcp::socket&& socket, Server::Impl& server) : m_stream(std::move(socket)), m_server(server) {}

    void run() { read(); }

private:
    void read()
    {
        m_parser.emplace();
        // A model upload is the body: a large STL is a few hundred megabytes.
        m_parser->body_limit(1ull << 30);
        m_stream.expires_after(std::chrono::seconds(300));
        http::async_read(m_stream, m_buffer, *m_parser, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec == http::error::end_of_stream) return self->close();
            if (ec) return;
            Request req = self->m_parser->release();
            if (websocket::is_upgrade(req)) {
                if (auto it = req.find(http::field::origin); it != req.end() && !local_origin(it->value())) return self->close();
                std::make_shared<WsSession>(self->m_stream.release_socket(), self->m_server.sockets)->run(std::move(req));
                return;
            }
            self->m_server.handle(std::move(req), [self](http::message_generator&& msg) { self->write(std::move(msg)); });
        });
    }

    void write(http::message_generator&& msg)
    {
        const bool keep_alive = msg.keep_alive();
        beast::async_write(m_stream, std::move(msg), [self = shared_from_this(), keep_alive](beast::error_code ec, std::size_t) {
            if (ec) return;
            if (!keep_alive) return self->close();
            self->read();
        });
    }

    void close()
    {
        beast::error_code ec;
        m_stream.socket().shutdown(tcp::socket::shutdown_send, ec);
    }

    beast::tcp_stream m_stream;
    beast::flat_buffer m_buffer;
    std::optional<http::request_parser<http::string_body>> m_parser;
    Server::Impl& m_server;
};

void Server::Impl::accept()
{
    acceptor.async_accept(ioc, [this](beast::error_code ec, tcp::socket socket) {
        if (!ec) std::make_shared<HttpSession>(std::move(socket), *this)->run();
        accept();
    });
}

void Server::Impl::start_slice(const std::shared_ptr<SliceJob>& job)
{
    engine.post([this, job](Engine& e) {
        if (job->cancel.load()) {
            jobs.update(job, [](messages::Slice& s) { s.status = "cancelled"; });
            return;
        }
        jobs.update(job, [](messages::Slice& s) {
            s.status   = "running";
            s.progress = messages::SliceProgress{0, "Initializing"};
        });
        const auto progress = [this, job](double percent, const std::string& stage) {
            jobs.update(job, [&](messages::Slice& s) { s.progress = messages::SliceProgress{percent, stage}; });
        };
        Outcome<SliceOutcome> outcome = e.slice(job->model_path, job->request, progress, job->cancel);
        if (!outcome) {
            jobs.update(job, [&](messages::Slice& s) {
                s.progress.reset();
                s.status = outcome.failure->status == 499 ? "cancelled" : "failed";
                if (s.status == "failed") s.errors.push_back({"FatalError", outcome.failure->message, {}, std::nullopt});
            });
            return;
        }
        const std::string path = job->request.output.value_or(options.jobs_dir + "/slices/" + job->state.id + ".gcode");
        std::error_code fs_ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), fs_ec);
        {
            std::ofstream file(path, std::ios::binary);
            file << outcome.value->gcode;
            if (!file) {
                jobs.update(job, [&](messages::Slice& s) {
                    s.progress.reset();
                    s.status = "failed";
                    s.errors.push_back({"FatalError", "could not write " + path, {}, std::nullopt});
                });
                return;
            }
        }
        job->gcode_path = path;
        jobs.update(job, [&](messages::Slice& s) {
            s.progress.reset();
            s.status = "finished";
            messages::SliceResult result;
            result.gcode.url    = "/slices/" + s.id + "/gcode";
            result.gcode.path   = path;
            result.gcode.bytes  = static_cast<std::int64_t>(outcome.value->gcode.size());
            result.gcode.binary = false; // text always; the printer's wish is noted below
            result.statistics   = outcome.value->statistics;
            s.result            = std::move(result);
            s.warnings          = outcome.value->warnings;
            if (outcome.value->binary_requested)
                s.warnings.push_back({"BinaryGCodeNotWritten", "the printer preset asks for binary G-code; text was written (modules/slicer/TODO.md)", {"binary_gcode"}, std::nullopt});
        });
    });
}

void Server::Impl::handle(Request&& req, const Reply& reply)
{
    if (auto it = req.find(http::field::origin); it != req.end() && !local_origin(it->value()))
        return reply(error_response(req, 403, "pages from other sites may not use the slicer"));

    const std::string_view target = req.target();
    const std::string path(target.substr(0, target.find('?')));
    const auto query = query_of(target);
    const auto method = req.method();

    if (method == http::verb::options) {
        Response res{http::status::no_content, req.version()};
        cors(req, res);
        res.keep_alive(req.keep_alive());
        return reply(std::move(res));
    }

    if (method == http::verb::get && path == "/health") {
        return on_engine(std::move(req), reply, [](Engine& e, const Request& r) { return json_response(r, http::status::ok, json(e.describe())); });
    }
    if (method == http::verb::get && path == "/presets") {
        return on_engine(std::move(req), reply, [query](Engine& e, const Request& r) { return outcome_response(r, e.choices(selection_of(query))); });
    }
    if (method == http::verb::get && path == "/config") {
        return on_engine(std::move(req), reply, [query](Engine& e, const Request& r) { return outcome_response(r, e.config(selection_of(query))); });
    }

    if (method == http::verb::post && path == "/models") {
        const std::string name = filename_only(query.count("name") ? query.at("name") : "model.stl");
        if (req.body().empty()) return reply(error_response(req, 400, "the body is the model file's bytes; it is empty"));
        const std::string id = new_id();
        const std::string file = options.jobs_dir + "/models/" + id + "-" + name;
        std::error_code ec;
        std::filesystem::create_directories(options.jobs_dir + "/models", ec);
        {
            std::ofstream out(file, std::ios::binary);
            out << req.body();
            if (!out) return reply(error_response(req, 500, "could not store the upload at " + file));
        }
        return on_engine(std::move(req), reply, [this, file, id, name](Engine& e, const Request& r) {
            Outcome<messages::Model> model = e.inspect(file, id, name);
            if (model) jobs.add_model(*model.value, file);
            return outcome_response(r, model, http::status::created);
        });
    }
    if (method == http::verb::get && path.starts_with("/models/")) {
        const auto found = jobs.model(path.substr(8));
        if (!found) return reply(error_response(req, 404, "no such model"));
        return reply(json_response(req, http::status::ok, json(found->first)));
    }

    if (method == http::verb::post && path == "/slices") {
        messages::SliceRequest request;
        try {
            request = json::parse(req.body()).get<messages::SliceRequest>();
        } catch (const std::exception& e) {
            return reply(error_response(req, 400, std::string("not a slice request: ") + e.what()));
        }
        const bool by_id = request.model.id.has_value(), by_path = request.model.path.has_value();
        if (by_id == by_path) return reply(error_response(req, 400, "name the model by id or by path, one of the two"));

        auto job = std::make_shared<SliceJob>();
        job->state.id     = new_id();
        job->state.status = "queued";
        job->request      = request;

        if (by_id) {
            const auto found = jobs.model(*request.model.id);
            if (!found) return reply(error_response(req, 404, "no such model " + *request.model.id));
            job->state.model = found->first;
            job->model_path  = found->second;
            return on_engine(std::move(req), reply, [this, job](Engine& e, const Request& r) {
                job->state.selection = job->request.selection.value_or(e.current_selection());
                jobs.add_slice(job);
                start_slice(job);
                return json_response(r, http::status::accepted, json(jobs.snapshot(job)));
            });
        }
        // By path: an agent's or a script's file. Inspected first so the slice names
        // what it sliced, like an upload does.
        return on_engine(std::move(req), reply, [this, job](Engine& e, const Request& r) {
            const std::string file = *job->request.model.path;
            Outcome<messages::Model> model = e.inspect(file, new_id(), filename_only(file));
            if (!model) return outcome_response(r, model);
            jobs.add_model(*model.value, file);
            job->state.model     = *model.value;
            job->model_path      = file;
            job->state.selection = job->request.selection.value_or(e.current_selection());
            jobs.add_slice(job);
            start_slice(job);
            return json_response(r, http::status::accepted, json(jobs.snapshot(job)));
        });
    }
    if (method == http::verb::get && path == "/slices") {
        return reply(json_response(req, http::status::ok, json(jobs.slices())));
    }
    if (path.starts_with("/slices/")) {
        std::string rest = path.substr(8);
        const bool gcode = rest.ends_with("/gcode");
        if (gcode) rest = rest.substr(0, rest.size() - 6);
        const auto job = jobs.slice(rest);
        if (!job) return reply(error_response(req, 404, "no such slice"));
        if (method == http::verb::get && gcode) {
            const messages::Slice state = jobs.snapshot(job);
            if (state.status != "finished") return reply(error_response(req, 409, "the slice is " + state.status));
            http::file_body::value_type body;
            beast::error_code ec;
            body.open(job->gcode_path.c_str(), beast::file_mode::scan, ec);
            if (ec) return reply(error_response(req, 500, "the G-code is gone from " + job->gcode_path));
            http::response<http::file_body> res{http::status::ok, req.version()};
            res.set(http::field::server, "tragedy-slicer");
            res.set(http::field::content_type, "text/plain; charset=utf-8");
            res.set(http::field::content_disposition, "inline; filename=\"" + std::filesystem::path(state.model.name).stem().string() + ".gcode\"");
            cors(req, res);
            res.keep_alive(req.keep_alive());
            res.body() = std::move(body);
            res.prepare_payload();
            return reply(std::move(res));
        }
        if (method == http::verb::get) return reply(json_response(req, http::status::ok, json(jobs.snapshot(job))));
        if (method == http::verb::delete_) {
            const messages::Slice state = jobs.snapshot(job);
            if (state.status == "queued" || state.status == "running") job->cancel = true;
            else jobs.forget_slice(state.id);
            Response res{http::status::no_content, req.version()};
            cors(req, res);
            res.keep_alive(req.keep_alive());
            return reply(std::move(res));
        }
    }

    reply(error_response(req, 404, "no such route"));
}

Server::Server(net::io_context& ioc, EngineThread& engine, Jobs& jobs, ServerOptions options) :
    m_impl(std::make_unique<Impl>(ioc, engine, jobs, std::move(options)))
{
    const tcp::endpoint endpoint{net::ip::make_address("127.0.0.1"), m_impl->options.port};
    m_impl->acceptor.open(endpoint.protocol());
    m_impl->acceptor.set_option(net::socket_base::reuse_address(true));
    m_impl->acceptor.bind(endpoint);
    m_impl->acceptor.listen(net::socket_base::max_listen_connections);
}

Server::~Server() = default;

void Server::run()
{
    m_impl->accept();
}

void Server::broadcast(const std::string& text)
{
    net::post(m_impl->ioc, [this, text] {
        for (WsSession* session : std::set<WsSession*>(*m_impl->sockets)) session->send(text);
    });
}

} // namespace tragedy::slicer
