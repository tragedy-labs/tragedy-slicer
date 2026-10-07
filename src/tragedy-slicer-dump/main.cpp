// tragedy-slicer-dump — the engine's settings vocabulary as JSON.
//
// Usage: tragedy-slicer-dump [out.json]      (stdout when no path is given)
//
// Every item of the FDM and SLA configuration definitions, with the metadata the
// engine holds about it. Values use the same JSON representation the engine's
// own ConfigJson uses (a percentage is {value, is_percent}, a point is [x, y]),
// so a default read from this file is a value the service can hand back.
//
// Enum identifiers (category, option group, GUI type, location) are emitted by
// name, so a consumer keys on `Print_Infill`, never on a code that a reordering
// of the enum would move.

#include "Slic3r/Domain/ConfigDef.hpp"
#include "Slic3r/Domain/ConfigDefsFDM.hpp"
#include "Slic3r/Domain/ConfigDefsSLA.hpp"
#include "Slic3r/Domain/ConfigValue.hpp"
#include "Slic3r/Domain/Percentage.hpp"
#include "Slic3r/Domain/PrinterTechnology.hpp"
#include "Slic3r/Version.hpp"

#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <variant>
#include <vector>

using json = nlohmann::ordered_json;
using namespace Slic3r::Domain;

// magic_enum names only the values it scans, -128..127 by default, and returns ""
// for the rest. The engine numbers categories in hundreds (Filament_* from 200,
// Printer_* from 300) and option groups in the thousands (Printer_* from 10000), so
// at the default range every filament and printer id came out empty and their boxes
// lost their pages. The ranges cover what the engine defines; enum_id() below
// refuses a value they miss, so a new band fails the dump instead of emptying it.
template <> struct magic_enum::customize::enum_range<ConfigItemDef::Category> {
    static constexpr int min = 0;
    static constexpr int max = 1023;
};
template <> struct magic_enum::customize::enum_range<ConfigItemDef::OptionGroup> {
    static constexpr int min = 0;
    static constexpr int max = 16383;
};

namespace {

template <typename E>
std::string enum_id(E value)
{
    const auto name = magic_enum::enum_name(value);
    if (name.empty())
        throw std::runtime_error("tragedy-slicer-dump: " + std::string(magic_enum::enum_type_name<E>()) +
                                 " value " + std::to_string(static_cast<long>(value)) +
                                 " is outside magic_enum's range; widen its enum_range");
    return std::string(name);
}

// --- values, as the engine's ConfigJson writes them ---------------------------

json value_json(const ConfigValue& value)
{
    json out;
    value.visit([&]<typename T0>(const T0& v) {
        using T = std::remove_cvref_t<T0>;
        if constexpr (std::is_same_v<T, EnumWrapper>) {
            out = std::string(v.get_string());
        } else if constexpr (std::is_same_v<T, EnumVectorWrapper>) {
            out = json::array();
            for (auto s : v.get_strings()) out.push_back(std::string(s));
        } else if constexpr (std::is_same_v<T, std::optional<int>>) {
            if (v) out = *v; else out = nullptr;
        } else if constexpr (std::is_same_v<T, std::vector<std::optional<int>>>) {
            out = json::array();
            for (const auto& o : v) { if (o) out.push_back(*o); else out.push_back(nullptr); }
        } else if constexpr (std::is_same_v<T, Vec2d>) {
            out = json::array({v.x(), v.y()});
        } else if constexpr (std::is_same_v<T, std::vector<Vec2d>>) {
            out = json::array();
            for (const auto& p : v) out.push_back(json::array({p.x(), p.y()}));
        } else if constexpr (std::is_same_v<T, Percentage>) {
            out = {{"value", v.value}, {"is_percent", true}};
        } else if constexpr (std::is_same_v<T, std::vector<Percentage>>) {
            out = json::array();
            for (const auto& p : v) out.push_back({{"value", p.value}, {"is_percent", true}});
        } else if constexpr (std::is_same_v<T, FloatOrPercentage>) {
            out = {{"value", v.is_percentage() ? v.percentage().value : v.float_value()},
                   {"is_percent", v.is_percentage()}};
        } else if constexpr (std::is_same_v<T, std::vector<FloatOrPercentage>>) {
            out = json::array();
            for (const auto& f : v)
                out.push_back({{"value", f.is_percentage() ? f.percentage().value : f.float_value()},
                               {"is_percent", f.is_percentage()}});
        } else {
            out = v; // bool, int, double, string and their vectors
        }
    });
    return out;
}

// --- types --------------------------------------------------------------------

struct TypeInfo { const char* kind; bool array; bool nullable; };

// The variant inside ConfigValue, by type_info. A definition names its type with
// `typeid(T)`, so a comparison is exact.
std::optional<TypeInfo> type_info_of(const std::type_info* t)
{
    if (!t) return std::nullopt;
    struct Entry { const std::type_info& type; TypeInfo info; };
    static const Entry table[] = {
        {typeid(EnumWrapper),                     {"enum", false, false}},
        {typeid(bool),                            {"bool", false, false}},
        {typeid(int),                             {"int", false, false}},
        {typeid(std::optional<int>),              {"int", false, true}},
        {typeid(double),                          {"float", false, false}},
        {typeid(std::string),                     {"string", false, false}},
        {typeid(Vec2d),                           {"point", false, false}},
        {typeid(FloatOrPercentage),               {"float_or_percent", false, false}},
        {typeid(Percentage),                      {"percent", false, false}},
        {typeid(EnumVectorWrapper),               {"enum", true, false}},
        {typeid(std::vector<bool>),               {"bool", true, false}},
        {typeid(std::vector<int>),                {"int", true, false}},
        {typeid(std::vector<std::optional<int>>), {"int", true, true}},
        {typeid(std::vector<double>),             {"float", true, false}},
        {typeid(std::vector<std::string>),        {"string", true, false}},
        {typeid(std::vector<Vec2d>),              {"point", true, false}},
        {typeid(std::vector<FloatOrPercentage>),  {"float_or_percent", true, false}},
        {typeid(std::vector<Percentage>),         {"percent", true, false}},
    };
    for (const auto& e : table)
        if (e.type == *t) return e.info;
    return std::nullopt;
}

// --- locations ----------------------------------------------------------------

json location_json(const ConfigLocation& loc)
{
    return std::visit(
        [](const auto& l) -> json {
            using T = std::remove_cvref_t<decltype(l)>;
            if constexpr (std::is_same_v<T, PhysicalPrinterLocation>) return "physical_printer";
            else if constexpr (std::is_same_v<T, AppConfigLocation>) return "app_config";
            else return std::string(magic_enum::enum_name(l));
        },
        loc);
}

// --- one item -----------------------------------------------------------------

json item_json(const ConfigItemDef& def, PrinterTechnology pt)
{
    json j;
    j["name"] = def.name;

    if (auto t = type_info_of(def.type)) {
        j["type"] = {{"kind", t->kind}, {"array", t->array}, {"nullable", t->nullable}};
    } else {
        j["type"] = {{"kind", "unknown"}, {"array", false}, {"nullable", false}};
    }

    j["location"] = location_json(def.location);
    j["overrides_in"] = json::array();
    for (const auto& loc : def.overrides_in) j["overrides_in"].push_back(location_json(loc));

    j["label"] = def.label;
    j["full_label"] = def.full_label;
    j["i18n_context"] = def.i18n_context;
    j["tooltip"] = def.tooltip;
    j["units"] = def.units;

    // Category and option group name the page and the group on it — what the
    // 2.x Tab.cpp used to hold, now data on the definition. `order` is the enum's
    // value: the engine numbers pages and groups in the order its tabs show them
    // (Print_LayersSurfaces 100 before Print_WallsPerimeters 101), so it is the
    // order and nothing else; the id stays the key.
    j["category"] = {
        {"id", enum_id(def.category)},
        {"order", static_cast<int>(def.category)},
        {"title", def.category == ConfigItemDef::Category::Unknown
                      ? json(nullptr)
                      : json(ConfigItemDef::translate_category(def.category, pt))}};
    j["option_group"] = {
        {"id", enum_id(def.option_group)},
        {"order", static_cast<int>(def.option_group)},
        {"title", def.option_group == ConfigItemDef::OptionGroup::Unknown
                      ? json(nullptr)
                      : json(ConfigItemDef::translate_option_group(def.option_group))}};
    j["order"] = def.order;
    j["row_group"] = def.row_group;
    j["ratio_over"] = def.ratio_over;

    j["cli"] = def.cli == ConfigItemDef::nocli ? json(nullptr) : json(def.cli);
    j["multiline"] = def.multiline;
    j["full_width"] = def.full_width;
    if (def.height >= 0) j["height"] = def.height;
    if (def.min) j["min"] = *def.min;
    if (def.max) j["max"] = *def.max;
    j["max_literal"] = def.max_literal;
    j["require_tool_parity"] = def.require_tool_parity;
    j["compatibility_rule"] = std::string(magic_enum::enum_name(def.compatibility_rule));
    j["gui_type"] = std::string(magic_enum::enum_name(def.gui_type));
    j["aliases"] = def.aliases;

    // Suggested values for an open field (a layer height list, say): value + label.
    j["choices"] = json::array();
    for (const auto& [value, label] : def.choices) {
        json v = std::visit([](const auto& x) -> json { return x; }, value);
        j["choices"].push_back({{"value", v}, {"label", label}});
    }

    // The default, and for an enum the closed set it is drawn from.
    std::optional<ConfigValue> value;
    if (def.init_fn) value = def.init_fn();
    else if (def.init_fn_ex) value = def.init_fn_ex(def.location);
    if (value) {
        j["default"] = value_json(*value);
        const EnumValueDefs* enum_defs = nullptr;
        if (value->holds_alternative<EnumWrapper>()) enum_defs = &value->get<EnumWrapper>().def();
        else if (value->holds_alternative<EnumVectorWrapper>()) enum_defs = &value->get<EnumVectorWrapper>().def();
        if (enum_defs) {
            j["enum"] = json::array();
            for (const auto& e : *enum_defs)
                j["enum"].push_back({{"value", e.enum_value}, {"serialized", e.str_serialized}, {"label", e.str_ui}});
        }
    } else {
        j["default"] = nullptr;
    }
    return j;
}

json technology_json(const ConfigDefinitions& defs, PrinterTechnology pt)
{
    json items = json::array();
    for (const auto& def : defs.defs()) items.push_back(item_json(def, pt));
    return {{"items", std::move(items)}};
}

} // namespace

int main(int argc, char** argv)
{
    json out;
    out["engine"] = {{"name", "PrusaSlicer"}, {"version", SLIC3R_VERSION}, {"fork", "tragedy-slicer"}};
    out["technologies"] = {
        {"fff", technology_json(get_defs_fdm(), PrinterTechnology::FFF)},
        {"sla", technology_json(get_defs_sla(), PrinterTechnology::SLA)},
    };

    if (argc > 1) {
        std::ofstream ofs(argv[1], std::ios::out | std::ios::binary);
        if (!ofs) { std::cerr << "cannot write " << argv[1] << "\n"; return 1; }
        ofs << out.dump(2) << "\n";
    } else {
        std::cout << out.dump(2) << "\n";
    }
    return 0;
}
