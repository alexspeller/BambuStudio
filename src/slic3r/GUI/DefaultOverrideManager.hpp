#ifndef slic3r_DefaultOverrideManager_hpp_
#define slic3r_DefaultOverrideManager_hpp_

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <optional>

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/Preset.hpp"

namespace Slic3r { namespace GUI {

// An override applies a single {setting_key -> value} pair to a set of system presets.
// Multiple overrides can target the same preset as long as they target different keys.
struct DefaultOverride {
    std::string          id;            // sequential integer as string, stable across save/load
    std::string          setting_key;   // e.g. "wall_generator"
    nlohmann::json       value;         // serialized value in the same shape preset JSON uses (string for scalars, array of strings for vectors)
    Preset::Type         preset_type;   // TYPE_PRINT, TYPE_FILAMENT, TYPE_PRINTER
    std::vector<std::string> targets;   // preset names (matches on-disk filename minus .json)
    std::string          description;
    int64_t              created_ts = 0;
};

// One concrete system preset plus its parsed dimensions, used to populate the scope
// filter UI in the UpdateDefaultsDialog.
struct PresetDimensions {
    std::string preset_name;
    std::string printer_model;   // "A1", "X1C", "H2D", ...
    std::string nozzle;          // "0.2", "0.4" (absence of the nozzle suffix in the name means 0.4)
    std::string layer_height;    // process presets only - e.g. "0.20mm"
    std::string quality_tier;    // process presets only - e.g. "Standard", "Extra Fine"
};

struct StaleReport {
    // preset name -> reason string (for diagnostics)
    std::map<std::string, std::string> mismatched_presets;
    bool any() const { return !mismatched_presets.empty(); }
};

// Owns the collection of default overrides that get mutated into the BambuStudio
// system preset JSON files on disk. The manager:
//   - persists the override list to overrides.json in data_dir/default_overrides/
//   - takes snapshots of pristine system preset files before first mutation
//   - rebuilds the system files from the snapshots + current overrides whenever
//     anything changes (this keeps the apply logic simple and deterministic)
//   - detects when external changes (e.g. a BambuStudio update) have invalidated
//     previously-applied overrides
class DefaultOverrideManager
{
public:
    DefaultOverrideManager();

    // Storage --------------------------------------------------------------

    // Called once at startup after data_dir is known. Loads overrides.json if present.
    void load();

    // Serializes the current override list. Safe to call multiple times.
    void save() const;

    // Paths used by the manager. Relative to data_dir().
    static boost::filesystem::path root_dir();
    static boost::filesystem::path overrides_json_path();
    static boost::filesystem::path snapshots_dir();
    static boost::filesystem::path snapshot_path(Preset::Type type, const std::string& preset_name);
    static boost::filesystem::path system_preset_path(Preset::Type type, const std::string& preset_name);

    // Override CRUD --------------------------------------------------------

    // Adds a new override and rebuilds affected system files. Returns the new id.
    std::string add_override(const std::string& setting_key,
                             const nlohmann::json& value,
                             Preset::Type preset_type,
                             const std::vector<std::string>& targets,
                             const std::string& description = {});

    // Replaces an existing override's value/targets in place. Rebuilds affected files.
    void update_override(const std::string& id,
                         const nlohmann::json& value,
                         const std::vector<std::string>& targets);

    // Removes an override and rebuilds any system files it previously touched
    // (they may still be touched by other overrides, or may need to be restored).
    void remove_override(const std::string& id);

    // Wipe everything - restores snapshots for all touched presets, deletes overrides.
    void remove_all();

    // Querying -------------------------------------------------------------

    const std::vector<DefaultOverride>& overrides() const { return m_overrides; }
    std::optional<DefaultOverride>      find(const std::string& id) const;

    // Returns all concrete (instantiable) system presets of the given type, with
    // parsed dimensions. Queries the live PresetBundle.
    std::vector<PresetDimensions> list_concrete_presets(Preset::Type type) const;

    // Returns the value that would be in effect for a given setting on a given
    // preset, resolving through the inheritance chain. Empty optional if not set.
    std::optional<std::string> effective_value(const std::string& preset_name,
                                               const std::string& setting_key) const;

    // Compares every touched system preset file to what we expect after apply.
    // Populates the report for any files that have been externally modified since
    // the last apply - typically by a BambuStudio update.
    StaleReport check_stale() const;

    // Rebuild everything on disk from the current override state. Called internally
    // by the CRUD methods; exposed so callers can re-apply after a stale detection.
    void rebuild_all_affected_files();

private:
    // Parses "0.20mm Standard @BBL X1C 0.6 nozzle" into its four components.
    // Returns false if the name doesn't match the expected pattern.
    static bool parse_process_preset_name(const std::string& name, PresetDimensions& out);

    // Returns the list of preset names that appear in any override OR have a snapshot.
    // These are the files that need to be rebuilt when anything changes.
    std::map<Preset::Type, std::vector<std::string>> collect_touched_presets() const;

    // Rebuilds a single system preset file from its snapshot + applicable overrides.
    // Creates a snapshot on first touch if one doesn't already exist.
    void rebuild_preset_file(Preset::Type type, const std::string& preset_name);

    // Restores a preset file from its snapshot (if one exists).
    void restore_from_snapshot(Preset::Type type, const std::string& preset_name);

    // Returns the section name ("process"/"filament"/"machine") for a preset type.
    static std::string section_for_type(Preset::Type type);

    // Parses string -> Preset::Type. Returns TYPE_INVALID for unknown values.
    static Preset::Type type_from_section(const std::string& section);

    // Ensures snapshots/process etc. exist.
    static void ensure_directories();

    std::string next_id();

    mutable std::mutex       m_mutex;
    std::vector<DefaultOverride> m_overrides;
    int                      m_next_id_counter = 1;
    bool                     m_loaded = false;
};

}} // namespace Slic3r::GUI

#endif // slic3r_DefaultOverrideManager_hpp_
