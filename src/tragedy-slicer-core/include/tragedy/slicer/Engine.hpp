// tragedy-slicer-core — the engine as plain functions over PrusaSlicer's own code.
//
// Everything a host (the native service, later the WASM worker) asks of the engine
// goes through this one class, on one thread: the engine's interactors are not
// thread-safe and its main-thread dispatcher has to be pumped by whoever owns it.
// The host owns that thread and calls these methods from it; a long call (a slice)
// pumps the dispatcher itself while it waits, and polls `cancel` so the host can
// stop it from any thread.
#pragma once

#include "tragedy/slicer/Messages.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace Slic3r::App::CLI {
class CLIRuntime;
}

namespace tragedy::slicer {

struct EngineOptions
{
    /// The engine's `resources/` (presets, shapes, localization). Defaults to the
    /// fork's own tree, compiled in.
    std::string resources_dir;
    /// The engine's data directory: user presets, the local copy of the vendor
    /// bundles. Defaults to the engine's own default, the one its desktop app of the
    /// same build uses, so presets saved in either are seen by both (ADR 0032:
    /// presets stay the engine's files).
    std::string data_dir;
};

/// A request that could not be met: an HTTP status to answer with, and a sentence a
/// person may read (nothing here is a secret; the service runs for its own user).
struct Failure
{
    int status;
    std::string message;
};

template <typename T>
struct Outcome
{
    std::optional<T> value;
    std::optional<Failure> failure;
    static Outcome ok(T v) { return {std::move(v), std::nullopt}; }
    static Outcome fail(int status, std::string message) { return {std::nullopt, Failure{status, std::move(message)}}; }
    explicit operator bool() const { return value.has_value(); }
};

struct SliceOutcome
{
    /// The G-code as text. Binary G-code (`.bgcode`) is not produced here: the
    /// printer preset's `binary_gcode` is reported and the export step that would
    /// convert is left to a later step (modules/slicer/TODO.md).
    std::string gcode;
    bool binary_requested{false};
    messages::SliceResultStatistics statistics;
    std::vector<messages::SliceNotice> warnings;
};

class Engine
{
public:
    explicit Engine(const EngineOptions& options);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    messages::Engine describe() const;

    /// Complete the selection (what is not named keeps its current value, or the
    /// engine's default for a changed printer), apply it, and list what can be
    /// chosen for it.
    Outcome<messages::PresetChoices> choices(const messages::PresetSelection& selection);

    /// The values every setting resolves to under the selection, box by box.
    Outcome<messages::Config> config(const messages::PresetSelection& selection);

    /// Read a model file with the engine's own loader and describe what it holds.
    Outcome<messages::Model> inspect(const std::string& path, const std::string& id, const std::string& name);

    using Progress = std::function<void(double percent, const std::string& stage)>;

    /// Slice a model file under the selection and overrides. Blocks, pumping the
    /// engine; reports progress; stops at the next poll once `cancel` is set, in
    /// which case the outcome's failure has status 499.
    Outcome<SliceOutcome> slice(
        const std::string& model_path,
        const messages::SliceRequest& request,
        const Progress& progress,
        const std::atomic<bool>& cancel
    );

    /// The selection the engine currently holds, completed.
    messages::PresetSelection current_selection() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace tragedy::slicer
