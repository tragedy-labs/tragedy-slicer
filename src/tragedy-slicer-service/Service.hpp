// The service's own state: the engine on its thread, and the handles it hands out.
//
// The engine runs on one thread of its own (Engine.hpp says why) and takes work
// as closures, one at a time; a slice holds that thread until it is done, so
// anything else asked of the engine waits behind it (listed in
// modules/slicer/TODO.md). The handles — models uploaded, slices requested — are
// a registry under a mutex that any thread may read, so a status query never
// waits for the engine.
#pragma once

#include "tragedy/slicer/Engine.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace tragedy::slicer {

class EngineThread
{
public:
    using Command = std::function<void(Engine&)>;

    explicit EngineThread(EngineOptions options)
    {
        std::promise<void> ready;
        std::future<void> started = ready.get_future();
        m_thread = std::thread([this, options = std::move(options), &ready]() mutable {
            try {
                Engine engine(options);
                ready.set_value();
                loop(engine);
            } catch (...) {
                ready.set_exception(std::current_exception());
            }
        });
        started.get();
    }

    ~EngineThread()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    void post(Command command)
    {
        {
            std::lock_guard lock(m_mutex);
            m_queue.push_back(std::move(command));
        }
        m_wake.notify_one();
    }

private:
    void loop(Engine& engine)
    {
        while (true) {
            Command command;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
                if (m_stop) return;
                command = std::move(m_queue.front());
                m_queue.pop_front();
            }
            command(engine);
        }
    }

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Command> m_queue;
    bool m_stop{false};
};

struct SliceJob
{
    messages::Slice state;
    std::string model_path;
    messages::SliceRequest request;
    std::string gcode_path;
    std::atomic<bool> cancel{false};
};

class Jobs
{
public:
    /// Called, outside the lock, with every slice that changed.
    std::function<void(const messages::Slice&)> on_slice_changed{[](const messages::Slice&) {}};

    void add_model(const messages::Model& model, const std::string& path)
    {
        std::lock_guard lock(m_mutex);
        m_models[model.id] = {model, path};
    }

    std::optional<std::pair<messages::Model, std::string>> model(const std::string& id) const
    {
        std::lock_guard lock(m_mutex);
        if (auto it = m_models.find(id); it != m_models.end()) return it->second;
        return std::nullopt;
    }

    std::shared_ptr<SliceJob> add_slice(std::shared_ptr<SliceJob> job)
    {
        {
            std::lock_guard lock(m_mutex);
            m_slices[job->state.id] = job;
        }
        on_slice_changed(job->state);
        return job;
    }

    std::shared_ptr<SliceJob> slice(const std::string& id) const
    {
        std::lock_guard lock(m_mutex);
        if (auto it = m_slices.find(id); it != m_slices.end()) return it->second;
        return nullptr;
    }

    std::vector<messages::Slice> slices() const
    {
        std::lock_guard lock(m_mutex);
        std::vector<messages::Slice> out;
        for (const auto& [id, job] : m_slices) out.push_back(job->state);
        return out;
    }

    /// Change a slice's state under the lock, then tell the listeners.
    void update(const std::shared_ptr<SliceJob>& job, const std::function<void(messages::Slice&)>& change)
    {
        messages::Slice copy;
        {
            std::lock_guard lock(m_mutex);
            change(job->state);
            copy = job->state;
        }
        on_slice_changed(copy);
    }

    messages::Slice snapshot(const std::shared_ptr<SliceJob>& job) const
    {
        std::lock_guard lock(m_mutex);
        return job->state;
    }

    void forget_slice(const std::string& id)
    {
        std::lock_guard lock(m_mutex);
        m_slices.erase(id);
    }

private:
    mutable std::mutex m_mutex;
    std::map<std::string, std::pair<messages::Model, std::string>> m_models;
    std::map<std::string, std::shared_ptr<SliceJob>> m_slices;
};

} // namespace tragedy::slicer
