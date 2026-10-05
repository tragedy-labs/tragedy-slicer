# tragedy-slicer-dump: the settings vocabulary as JSON (tragedy-labs ADR 0032).
#
# Walks the engine's own configuration definitions (slic3r-domain's
# ConfigDefinitions, FDM and SLA) and writes every item with all of its metadata:
# label, tooltip, units, category and option group, bounds, choices, GUI type,
# default. The contracts repository turns this into JSON Schema and uischema,
# so nothing about a setting is ever retyped by hand.
#
# Prusa's own `--config-model` dump (slic3r-shared, ConfigModelDump.cpp) emits
# name, type, bounds, enum values, location and default — enough to validate
# YAML, not enough to draw a form. This target emits the rest, and links only
# slic3r-domain so it builds without the GUI layers.
project(tragedy-slicer-dump)

find_package(nlohmann_json 3.11 REQUIRED)
find_package(magic_enum REQUIRED)

add_executable(tragedy-slicer-dump main.cpp)
target_link_libraries(tragedy-slicer-dump PRIVATE
    slic3r-domain
    nlohmann_json::nlohmann_json
    magic_enum::magic_enum
)
