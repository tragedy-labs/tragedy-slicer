// tragedy-slicer-core — see Engine.hpp.
//
// The engine is driven the way its own CLI drives it: a CLIRuntime (workbench,
// project interactor, preset bundle loaded the standard way), a project per
// task, the preset interactor's views for listing, the slicing interactor for the
// slice and the status cache for how far it is. Where the CLI has a function for
// a step (centering on the bed), that function is called, not copied.
#include "tragedy/slicer/Engine.hpp"

#include "Slic3r/App/CLI/CLIRuntime.hpp"
#include "Slic3r/App/Init.hpp"
#include "Slic3r/Biz/Algorithms/BoundingBox.hpp"
#include "Slic3r/Biz/Algorithms/ModelObject.hpp"
#include "Slic3r/Biz/Algorithms/Point.hpp"
#include "Slic3r/Biz/Config/ConfigLoad.hpp"
#include "Slic3r/Biz/Config/ConfigSerialize.hpp"
#include "Slic3r/Biz/FDMResultCache.hpp"
#include "Slic3r/Biz/FileLoadingLogic.hpp"
#include "Slic3r/Biz/Platform/PlatformServices.hpp"
#include "Slic3r/Biz/Preset/PresetInteractor.hpp"
#include "Slic3r/Biz/ProjectInteractor.hpp"
#include "Slic3r/Biz/Scene/SceneInteractor.hpp"
#include "Slic3r/Biz/Slicing/SlicingInteractor.hpp"
#include "Slic3r/Biz/StatusCache.hpp"
#include "Slic3r/Directories.hpp"
#include "Slic3r/Domain/Bed.hpp"
#include "Slic3r/Domain/BedInstance.hpp"
#include "Slic3r/Domain/ConfigContainer.hpp"
#include "Slic3r/Domain/ConfigPack.hpp"
#include "Slic3r/Domain/Model.hpp"
#include "Slic3r/Domain/ModelInstance.hpp"
#include "Slic3r/Domain/ModelObject.hpp"
#include "Slic3r/Domain/ModelVolume.hpp"
#include "Slic3r/Domain/Project.hpp"
#include "Slic3r/Version.hpp"

#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <set>
#include <sstream>
#include <thread>

using namespace Slic3r;
using Slic3r::App::CLI::CLIRuntime;
using Slic3r::Biz::ProjectInteractor;
using Slic3r::Biz::Preset::PresetInteractor;
using Slic3r::Domain::ConfigContainer;
using Slic3r::Domain::ConfigPack;
using Slic3r::Domain::ConfigPackFDM;
using Slic3r::Domain::PrinterTechnology;
using Slic3r::Domain::Preset::HwPrinterConfig;
using Slic3r::Domain::Preset::PresetOrigin;

namespace tragedy::slicer {

namespace {

std::string lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string origin_name(PresetOrigin origin)
{
    return lower(std::string(magic_enum::enum_name(origin)));
}

template <typename View>
std::vector<messages::Preset> presets_of(View&& view)
{
    std::vector<messages::Preset> out;
    for (const auto& [ref, is_runtime] : view) {
        const auto& p = ref.get();
        out.push_back({p.id, p.name, origin_name(p.origin)});
    }
    return out;
}

messages::Printer printer_of(const HwPrinterConfig& hw)
{
    messages::Printer out;
    out.id         = hw.id;
    out.name       = hw.name;
    out.vendor     = hw.vendor_id;
    out.technology = hw.technology == PrinterTechnology::FFF ? "fff" : "sla";
    out.model      = hw.model.model;
    for (const auto& tool : hw.tools) {
        messages::PrinterTool t;
        t.id   = tool.id;
        t.name = tool.name;
        if (auto it = tool.features.find("nozzle_diameter"); it != tool.features.end()) {
            if (const double* d = std::get_if<double>(&it->second)) t.nozzleDiameter = *d;
        }
        out.tools.push_back(std::move(t));
    }
    return out;
}

nlohmann::json plain_json(const nlohmann::ordered_json& ordered)
{
    // The engine serializes into ordered_json; the messages carry nlohmann::json. A
    // round trip through text is the one conversion that keeps every value exact.
    return nlohmann::json::parse(ordered.dump());
}

messages::SliceNotice notice_of(const Biz::Slicing::Error& error)
{
    std::ostringstream text;
    text << error;
    return {std::string(magic_enum::enum_name(error.code)), text.str(), error.item_keys, std::nullopt};
}

messages::SliceNotice notice_of(const Biz::Slicing::Warning& warning)
{
    messages::SliceNotice n;
    n.code     = std::string(magic_enum::enum_name(warning.code));
    n.message  = n.code; // the engine prints warnings only through its GUI strings
    n.settings = warning.item_keys;
    return n;
}

// Where the selected bed's centre is, as the CLI computes it
// (LoadPrintData.cpp, center_projects_on_selected_bed).
std::optional<Domain::Vec2d> selected_bed_center(ProjectInteractor& pi)
{
    const Domain::BedRef bed_ref = pi.scene_interactor().bed_selection().last_selected_bed();
    const ConfigContainer* cc = pi.selected_project().find_config_container(bed_ref.config_container_id);
    if (cc == nullptr) return std::nullopt;
    const Domain::BedInstance& instance = cc->find_bed_instance(bed_ref.instance_id);
    return cc->bed().center() + Biz::Algorithms::Point::to_2d(instance.transformation.get_offset());
}

// Centre a model's instances on a point and stand them on the bed, on the model
// itself before the scene takes it. The CLI moves the objects through the scene's
// selection (transform_selection) instead; done synchronously before a slice that
// path left the slicer reading freed mesh memory (a crash in slice_mesh), so the
// shift is applied where it is only arithmetic: the instances' offsets.
void center_on(Domain::Model& model, const Domain::Vec2d& center)
{
    for (Domain::ModelObject* object : model.objects)
        if (object->instances.empty()) object->add_instance();
    Domain::BoundingBox3d box;
    for (const Domain::ModelObject* object : model.objects)
        for (size_t i = 0; i < object->instances.size(); ++i)
            box = Biz::Algorithms::BoundingBox::merge(box, Biz::Algorithms::ModelObject::instance_bounding_box(*object, i, false));
    if (!box.defined) return;
    const Domain::Vec3d mid = Biz::Algorithms::BoundingBox::center(box);
    const Domain::Vec3d shift(center.x() - mid.x(), center.y() - mid.y(), -box.min.z());
    for (Domain::ModelObject* object : model.objects)
        for (Domain::ModelInstance* instance : object->instances)
            instance->set_offset(instance->get_offset() + shift);
}

} // namespace

struct Engine::Impl
{
    EngineOptions options;
    App::InitParams init_params;
    std::unique_ptr<CLIRuntime> runtime;
    Domain::SelectionId scratch_project{};
    // One project slices, for the service's whole run: its objects are replaced
    // before each slice. A project per slice, removed after, was tried first; a
    // project's id is its position in the workbench, so the next project took
    // the removed one's and the scene's callbacks for the new slice found the
    // old project's state (a crash in on_extruder_candidates_changed).
    Domain::SelectionId slicing_project{};

    PresetInteractor& presets() { return runtime->project_interactor().preset_interactor(); }

    // Apply a (partial) selection to the selected project. What is not named keeps
    // its value; a changed printer takes the engine's defaults for the rest, as the
    // desktop app does when a printer is picked.
    std::optional<Failure> apply(const messages::PresetSelection& wanted)
    {
        PresetInteractor& pi = presets();
        const Domain::Preset::SelectedPreset& current = pi.selected_printer_preset();

        std::string hw_id = wanted.printer.value_or(current.hw_config.id);
        const HwPrinterConfig* hw = nullptr;
        for (const auto& [ref, is_runtime] : pi.get_printer_configs())
            if (ref.get().id == hw_id) hw = &ref.get();
        if (hw == nullptr) return Failure{400, "unknown printer " + hw_id};

        const bool printer_changed = hw_id != current.hw_config.id;
        std::string printer_preset = wanted.printerPreset.value_or(printer_changed ? "" : current.printer.id);
        if (printer_preset.empty() || printer_changed || wanted.printerPreset) {
            bool found = false;
            for (const auto& [ref, is_runtime] : pi.get_printer_presets(hw_id)) {
                if (printer_preset.empty()) printer_preset = ref.get().id;
                if (ref.get().id == printer_preset) found = true;
            }
            if (!found) return Failure{400, "unknown printer preset " + printer_preset + " for printer " + hw_id};
            pi.select_printer_preset(hw_id, printer_preset);
        }

        if (wanted.print) {
            bool found = false;
            for (const auto& [ref, is_runtime] : pi.get_print_presets(hw_id, printer_preset))
                if (ref.get().id == *wanted.print) found = true;
            if (!found) return Failure{400, "unknown print preset " + *wanted.print + " for this printer"};
            pi.select_print_preset(*wanted.print);
        }
        const std::string print_id = pi.selected_printer_preset().print.id;

        if (wanted.tools) {
            for (size_t i = 0; i < wanted.tools->size() && i < hw->tool_count; ++i) {
                bool found = false;
                for (const auto& [ref, is_runtime] : pi.get_tool_print_presets(hw_id, printer_preset, print_id, i))
                    if (ref.get().id == (*wanted.tools)[i]) found = true;
                if (!found) return Failure{400, "unknown tool print preset " + (*wanted.tools)[i] + " for tool " + std::to_string(i)};
                pi.select_tool_print_preset(i, (*wanted.tools)[i]);
            }
        }
        if (wanted.materials) {
            for (size_t i = 0; i < wanted.materials->size() && i < hw->material_slot_count(); ++i) {
                bool found = false;
                for (const auto& [ref, is_runtime] : pi.get_material_presets(hw_id, printer_preset, print_id, i))
                    if (ref.get().id == (*wanted.materials)[i]) found = true;
                if (!found) return Failure{400, "unknown material preset " + (*wanted.materials)[i] + " for slot " + std::to_string(i)};
                pi.select_material_preset(i, (*wanted.materials)[i]);
            }
        }
        return std::nullopt;
    }

    messages::PresetSelection selection() const
    {
        const Domain::Preset::SelectedPreset& s =
            runtime->project_interactor().preset_interactor().selected_printer_preset();
        messages::PresetSelection out;
        out.printer       = s.hw_config.id;
        out.printerPreset = s.printer.id;
        out.print         = s.print.id;
        std::vector<std::string> tools, materials;
        for (const auto& t : s.tools) tools.push_back(t.id);
        for (const auto& m : s.materials) materials.push_back(m.id);
        out.tools     = tools;
        out.materials = materials;
        return out;
    }
};

Engine::Engine(const EngineOptions& options) : m_impl(std::make_unique<Impl>())
{
    m_impl->options = options;
    App::init_common();

    const std::string resources = options.resources_dir.empty() ? TRAGEDY_SLICER_RESOURCES_DIR : options.resources_dir;
    const std::filesystem::path res(resources);
    set_resources_dir(res.string());
    set_var_dir((res / "icons").string());
    set_local_dir((res / "localization").string());
    set_sys_shapes_dir((res / "shapes").string());
    set_custom_gcodes_dir((res / "custom_gcodes").string());

    if (!options.data_dir.empty()) m_impl->init_params.misc.datadir = options.data_dir;
    App::init_paths(m_impl->init_params);

    m_impl->runtime = std::make_unique<CLIRuntime>(m_impl->init_params);
    // One project holds the selection for listing and config; another slices.
    ProjectInteractor& pi    = m_impl->runtime->project_interactor();
    m_impl->scratch_project  = pi.new_project();
    m_impl->slicing_project  = pi.new_project();
    pi.select_project(m_impl->scratch_project);
}

Engine::~Engine() = default;

messages::Engine Engine::describe() const
{
    messages::Engine out;
    out.name    = "PrusaSlicer";
    out.version = SLIC3R_VERSION;
    out.fork    = "tragedy-slicer";
    out.commit  = TRAGEDY_SLICER_COMMIT;
    std::set<std::string> vendors;
    for (const auto& [ref, is_runtime] : m_impl->presets().get_printer_configs()) vendors.insert(ref.get().vendor_id);
    out.vendors = std::vector<std::string>(vendors.begin(), vendors.end());
    return out;
}

messages::PresetSelection Engine::current_selection() const
{
    return m_impl->selection();
}

Outcome<messages::PresetChoices> Engine::choices(const messages::PresetSelection& wanted)
{
    ProjectInteractor& project = m_impl->runtime->project_interactor();
    project.select_project(m_impl->scratch_project);
    if (auto failure = m_impl->apply(wanted)) return Outcome<messages::PresetChoices>::fail(failure->status, failure->message);

    PresetInteractor& pi = m_impl->presets();
    const Domain::Preset::SelectedPreset& s = pi.selected_printer_preset();
    messages::PresetChoices out;
    out.selection = m_impl->selection();
    for (const auto& [ref, is_runtime] : pi.get_printer_configs()) out.printers.push_back(printer_of(ref.get()));
    out.printerPresets = presets_of(pi.get_printer_presets(s.hw_config.id));
    out.prints         = presets_of(pi.get_print_presets(s.hw_config.id, s.printer.id));
    for (size_t i = 0; i < s.hw_config.tool_count; ++i)
        out.tools.push_back(presets_of(pi.get_tool_print_presets(s.hw_config.id, s.printer.id, s.print.id, i)));
    for (size_t i = 0; i < s.hw_config.material_slot_count(); ++i)
        out.materials.push_back(presets_of(pi.get_material_presets(s.hw_config.id, s.printer.id, s.print.id, i)));
    return Outcome<messages::PresetChoices>::ok(std::move(out));
}

Outcome<messages::Config> Engine::config(const messages::PresetSelection& wanted)
{
    ProjectInteractor& project = m_impl->runtime->project_interactor();
    project.select_project(m_impl->scratch_project);
    if (auto failure = m_impl->apply(wanted)) return Outcome<messages::Config>::fail(failure->status, failure->message);

    const ConfigPack pack = project.selected_config_container().build_print_config();
    if (!std::holds_alternative<ConfigPackFDM>(pack))
        return Outcome<messages::Config>::fail(501, "only FFF printers are served for now (modules/slicer/TODO.md)");
    const ConfigPackFDM& fdm = std::get<ConfigPackFDM>(pack);

    messages::Config out;
    out.technology = "fff";
    nlohmann::ordered_json j;
    Domain::to_json(j, fdm.print);
    out.print = plain_json(j);
    Domain::to_json(j, fdm.printer);
    out.printer = plain_json(j);
    for (const auto& filament : fdm.filament) {
        Domain::to_json(j, filament);
        out.filaments.push_back(plain_json(j));
    }
    return Outcome<messages::Config>::ok(std::move(out));
}

Outcome<messages::Model> Engine::inspect(const std::string& path, const std::string& id, const std::string& name)
{
    auto loaded = Biz::FileLoadingLogic::read_model_from_file(path, nullptr);
    if (!loaded) return Outcome<messages::Model>::fail(422, "the engine could not read " + name + ": " + loaded.error());

    messages::Model out;
    out.id    = id;
    out.name  = name;
    out.bytes = static_cast<std::int64_t>(std::filesystem::file_size(path));
    for (const Domain::ModelObject* object : loaded->objects) {
        messages::ModelObject o;
        // The loader names an STL's one object after the file on disk, which for
        // an upload is the stored copy's name; the file's own name is the one the
        // person knows.
        const std::string stored = std::filesystem::path(path).filename().string();
        o.name = (object->name.empty() || object->name == stored || object->name == std::filesystem::path(stored).stem().string()) ? name : object->name;
        for (const Domain::ModelVolume* volume : object->volumes)
            o.triangles += static_cast<std::int64_t>(volume->mesh().facets_count());
        const Domain::BoundingBox3d& box = Biz::Algorithms::ModelObject::bounding_box_exact(*object);
        const Domain::Vec3d size = box.max - box.min;
        o.size = {size.x(), size.y(), size.z()};
        out.objects.push_back(std::move(o));
    }
    return Outcome<messages::Model>::ok(std::move(out));
}

Outcome<SliceOutcome> Engine::slice(
    const std::string& model_path,
    const messages::SliceRequest& request,
    const Progress& progress,
    const std::atomic<bool>& cancel
)
{
    using Out = Outcome<SliceOutcome>;
    CLIRuntime& runtime   = *m_impl->runtime;
    ProjectInteractor& pi = runtime.project_interactor();

    auto loaded = Biz::FileLoadingLogic::read_model_from_file(model_path, nullptr);
    if (!loaded) return Out::fail(422, "the engine could not read the model: " + loaded.error());

    pi.select_project(m_impl->slicing_project);
    struct Reselect
    {
        ProjectInteractor& pi;
        Domain::SelectionId scratch;
        ~Reselect() { pi.select_project(scratch); }
    } reselect{pi, m_impl->scratch_project};
    // The last slice's objects go; the project stays.
    {
        const Domain::ModelObjectPtrs previous = pi.selected_project().model().objects;
        for (Domain::ModelObject* object : previous) pi.scene_interactor().delete_object(object);
    }

    // The selection first: the printer decides the bed the model is centred on.
    if (auto failure = m_impl->apply(request.selection.value_or(messages::PresetSelection{})))
        return Out::fail(failure->status, failure->message);

    if (request.arrange) {
        if (const auto center = selected_bed_center(pi)) center_on(*loaded, *center);
    }
    pi.scene_interactor().add_new_objects(loaded->objects);

    Domain::Project& project = pi.selected_project();
    const ConfigContainer& cc = pi.selected_config_container();
    ConfigPack pack = cc.build_print_config();
    if (!std::holds_alternative<ConfigPackFDM>(pack))
        return Out::fail(501, "only FFF printers are sliced for now (modules/slicer/TODO.md)");
    ConfigPackFDM& fdm = std::get<ConfigPackFDM>(pack);

    // Overrides: the engine's own loader onto the boxes the slice will use. An issue
    // on any key fails the whole request — a setting silently left as the preset had
    // it is a print with the wrong settings.
    if (request.overrides) {
        std::string issues;
        auto apply_box = [&](const std::optional<nlohmann::json>& values, Domain::ConfigBox& box, const std::string& where) {
            if (!values || !values->is_object()) return;
            const auto ordered = nlohmann::ordered_json::parse(values->dump());
            // load_box loads a whole box, so a key the overrides do not name is
            // reported as NotFound; here that is the preset's value kept, not an issue.
            for (const auto& [key, issue] : Biz::Config::load_box(ordered, box)) {
                if (issue.type == Biz::Config::ItemParsingIssueType::NotFound) continue;
                issues += where + "." + key + ": "
                    + (issue.type == Biz::Config::ItemParsingIssueType::ExtraKey ? "no such setting" : "invalid value")
                    + (issue.message.empty() ? "" : " (" + issue.message + ")") + "; ";
            }
        };
        apply_box(request.overrides->print, fdm.print, "print");
        apply_box(request.overrides->printer, fdm.printer, "printer");
        if (request.overrides->filaments) {
            for (size_t i = 0; i < request.overrides->filaments->size() && i < fdm.filament.size(); ++i)
                apply_box((*request.overrides->filaments)[i], fdm.filament[i], "filaments[" + std::to_string(i) + "]");
        }
        if (!issues.empty()) return Out::fail(400, "overrides not applied: " + issues);
    }

    const Domain::SlicingId slicing_id = pi.selected_bed_slicing_id();
    const Domain::BedInstance* bed = project.find_bed_instance_by_id(slicing_id.bed_instance_id);
    if (bed == nullptr) return Out::fail(500, "no bed is selected in the new project");

    Biz::Slicing::SlicingInteractor& slicing = pi.slicing_interactor();
    Biz::StatusCache& status_cache          = pi.status_cache();
    slicing.update_process(project.model(), project.metadata(), cc.selected_preset().metadata(), pack, *bed);
    slicing.slice_bed(slicing_id);

    bool cancelled = false;
    double last_percent = -1;
    std::string last_stage;
    while (true) {
        runtime.dispatcher().dispatch_enqueued();
        const std::optional<Biz::Slicing::Status> status = status_cache.get_status(slicing_id);
        if (status && status->progress) {
            const double percent = status->progress->progress.value;
            const std::string stage(magic_enum::enum_name(status->progress->progress_info));
            if (percent != last_percent || stage != last_stage) {
                last_percent = percent;
                last_stage   = stage;
                progress(percent, stage);
            }
        }
        if (status) {
            using Biz::Slicing::StatusCode;
            if (status->code == StatusCode::Empty || status->code == StatusCode::Removed
                || status->code == StatusCode::Finished || status->code == StatusCode::InvalidData)
                break;
            // A stopped process settles at Modified ("still needs the full slicing"),
            // never at a code above: once a stop was asked for, that is the end.
            if (cancelled && status->code != StatusCode::Running && status->code != StatusCode::Updating
                && status->code != StatusCode::Stopping)
                break;
        }
        if (cancel.load() && !cancelled) {
            cancelled = true;
            slicing.stop_slicing_bed(slicing_id);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Let the stop settle so the project can be removed cleanly.
    for (int i = 0; i < 20; ++i) runtime.dispatcher().dispatch_enqueued();

    if (cancelled) return Out::fail(499, "cancelled");
    const Biz::Slicing::Status status = status_cache.get_status(slicing_id).value();
    using Biz::Slicing::StatusCode;
    if (status.code == StatusCode::Empty) return Out::fail(422, "all objects are outside of the print volume");
    if (status.code == StatusCode::Removed) return Out::fail(500, "the slicing input was removed");
    if (status.code == StatusCode::InvalidData) {
        std::string text;
        for (const auto& error : status.errors) text += notice_of(error).message + " ";
        return Out::fail(422, text.empty() ? "the engine rejected the input" : text);
    }

    const auto result = pi.fdm_result_cache().get_result(slicing_id);
    if (!result) return Out::fail(500, "the slice finished but left no G-code");
    const Biz::libpgcode::ProcessorResult& r = result->get();

    SliceOutcome out;
    out.gcode = r.const_gcode()->str();
    out.binary_requested = fdm.printer.items.opt("binary_gcode").get<bool>();
    if (const auto* full = std::get_if<Domain::FullPrintStatistics>(&r.print_statistics)) {
        out.statistics.printTime      = full->normal_mode_time.time;
        out.statistics.filamentLength = full->total_used_filament_mm;
        out.statistics.filamentVolume = full->total_used_filament_cm3;
        out.statistics.filamentWeight = full->total_used_filament_g;
        out.statistics.filamentCost   = full->total_filament_cost;
    } else if (const auto* basic = std::get_if<Domain::BasicPrintStatistics>(&r.print_statistics)) {
        out.statistics.printTime = basic->normal_mode_time.time;
    }
    for (const auto& warning : status.warrnings) out.warnings.push_back(notice_of(warning));
    return Out::ok(std::move(out));
}

} // namespace tragedy::slicer
