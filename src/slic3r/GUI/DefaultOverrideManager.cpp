#include "DefaultOverrideManager.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <regex>
#include <set>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/Utils.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "GUI_App.hpp"

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

static constexpr const char* OVERRIDES_JSON_NAME = "overrides.json";
static constexpr int         OVERRIDES_SCHEMA_VERSION = 1;

// ---- path helpers ---------------------------------------------------------

fs::path DefaultOverrideManager::root_dir()
{
    return fs::path(data_dir()) / "default_overrides";
}

fs::path DefaultOverrideManager::overrides_json_path()
{
    return root_dir() / OVERRIDES_JSON_NAME;
}

fs::path DefaultOverrideManager::snapshots_dir()
{
    return root_dir() / "snapshots";
}

std::string DefaultOverrideManager::section_for_type(Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINT:    return PRESET_PRINT_NAME;    // "process"
    case Preset::TYPE_FILAMENT: return PRESET_FILAMENT_NAME; // "filament"
    case Preset::TYPE_PRINTER:  return PRESET_PRINTER_NAME;  // "machine"
    default:                    return {};
    }
}

Preset::Type DefaultOverrideManager::type_from_section(const std::string& section)
{
    if (section == PRESET_PRINT_NAME)    return Preset::TYPE_PRINT;
    if (section == PRESET_FILAMENT_NAME) return Preset::TYPE_FILAMENT;
    if (section == PRESET_PRINTER_NAME)  return Preset::TYPE_PRINTER;
    return Preset::TYPE_INVALID;
}

fs::path DefaultOverrideManager::snapshot_path(Preset::Type type, const std::string& preset_name)
{
    return snapshots_dir() / section_for_type(type) / (preset_name + ".json");
}

fs::path DefaultOverrideManager::system_preset_path(Preset::Type type, const std::string& preset_name)
{
    // System presets currently all live under BBL (the only bundled vendor). If/when
    // the user installs third-party vendor profiles we'd need to resolve the vendor
    // from the Preset - but for v1 (process presets only, BBL) this is sufficient.
    return fs::path(data_dir()) / PRESET_SYSTEM_DIR / "BBL" / section_for_type(type)
           / (preset_name + ".json");
}

void DefaultOverrideManager::ensure_directories()
{
    fs::create_directories(root_dir());
    fs::create_directories(snapshots_dir() / PRESET_PRINT_NAME);
    fs::create_directories(snapshots_dir() / PRESET_FILAMENT_NAME);
    fs::create_directories(snapshots_dir() / PRESET_PRINTER_NAME);
}

// ---- construction / load / save -------------------------------------------

DefaultOverrideManager::DefaultOverrideManager() = default;

void DefaultOverrideManager::load()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_overrides.clear();
    m_next_id_counter = 1;
    m_loaded = true;

    const fs::path path = overrides_json_path();
    if (!fs::exists(path))
        return;

    try {
        boost::nowide::ifstream ifs(path.string());
        if (!ifs)
            return;
        nlohmann::json root;
        ifs >> root;

        if (!root.contains("overrides") || !root["overrides"].is_array())
            return;

        int max_id = 0;
        for (const auto& entry : root["overrides"]) {
            DefaultOverride ov;
            ov.id           = entry.value("id", "");
            ov.setting_key  = entry.value("setting_key", "");
            ov.value        = entry.value("value", nlohmann::json());
            ov.preset_type  = type_from_section(entry.value("preset_type", ""));
            ov.description  = entry.value("description", "");
            ov.created_ts   = entry.value("created_ts", (int64_t)0);
            if (entry.contains("targets") && entry["targets"].is_array()) {
                for (const auto& t : entry["targets"])
                    if (t.is_string())
                        ov.targets.push_back(t.get<std::string>());
            }

            if (ov.setting_key.empty() || ov.preset_type == Preset::TYPE_INVALID || ov.id.empty())
                continue;

            try { max_id = std::max(max_id, std::stoi(ov.id)); } catch (...) {}
            m_overrides.push_back(std::move(ov));
        }
        m_next_id_counter = max_id + 1;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[DefaultOverrideManager] failed to load overrides.json: " << e.what();
    }
}

void DefaultOverrideManager::save() const
{
    try {
        ensure_directories();
        nlohmann::json root;
        root["version"]   = OVERRIDES_SCHEMA_VERSION;
        root["overrides"] = nlohmann::json::array();
        for (const auto& ov : m_overrides) {
            nlohmann::json entry;
            entry["id"]           = ov.id;
            entry["setting_key"]  = ov.setting_key;
            entry["value"]        = ov.value;
            entry["preset_type"]  = section_for_type(ov.preset_type);
            entry["targets"]      = ov.targets;
            entry["description"]  = ov.description;
            entry["created_ts"]   = ov.created_ts;
            root["overrides"].push_back(entry);
        }

        const fs::path path = overrides_json_path();
        const fs::path tmp  = path.string() + ".tmp";
        {
            boost::nowide::ofstream ofs(tmp.string());
            ofs << root.dump(2);
        }
        fs::rename(tmp, path);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[DefaultOverrideManager] failed to save overrides.json: " << e.what();
    }
}

std::string DefaultOverrideManager::next_id()
{
    return std::to_string(m_next_id_counter++);
}

// ---- CRUD -----------------------------------------------------------------

std::string DefaultOverrideManager::add_override(const std::string& setting_key,
                                                 const nlohmann::json& value,
                                                 Preset::Type preset_type,
                                                 const std::vector<std::string>& targets,
                                                 const std::string& description)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    DefaultOverride ov;
    ov.id          = next_id();
    ov.setting_key = setting_key;
    ov.value       = value;
    ov.preset_type = preset_type;
    ov.targets     = targets;
    ov.description = description;
    ov.created_ts  = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
    std::string id = ov.id;
    m_overrides.push_back(std::move(ov));
    lock.unlock();

    rebuild_all_affected_files();
    save();
    return id;
}

void DefaultOverrideManager::update_override(const std::string& id,
                                             const nlohmann::json& value,
                                             const std::vector<std::string>& targets)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = std::find_if(m_overrides.begin(), m_overrides.end(),
                               [&](const DefaultOverride& o) { return o.id == id; });
        if (it == m_overrides.end())
            return;
        it->value   = value;
        it->targets = targets;
    }
    rebuild_all_affected_files();
    save();
}

void DefaultOverrideManager::remove_override(const std::string& id)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = std::find_if(m_overrides.begin(), m_overrides.end(),
                               [&](const DefaultOverride& o) { return o.id == id; });
        if (it == m_overrides.end())
            return;
        m_overrides.erase(it);
    }
    rebuild_all_affected_files();
    save();
}

void DefaultOverrideManager::remove_all()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_overrides.clear();
    }
    rebuild_all_affected_files();
    save();
}

std::optional<DefaultOverride> DefaultOverrideManager::find(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = std::find_if(m_overrides.begin(), m_overrides.end(),
                           [&](const DefaultOverride& o) { return o.id == id; });
    if (it == m_overrides.end())
        return std::nullopt;
    return *it;
}

// ---- Rebuild logic --------------------------------------------------------

std::map<Preset::Type, std::vector<std::string>>
DefaultOverrideManager::collect_touched_presets() const
{
    std::map<Preset::Type, std::set<std::string>> sets;

    // Presets referenced by current overrides
    for (const auto& ov : m_overrides)
        for (const auto& target : ov.targets)
            sets[ov.preset_type].insert(target);

    // Presets that still have a snapshot on disk (may have been orphaned by
    // a just-removed override) - those need to be restored.
    for (Preset::Type t : {Preset::TYPE_PRINT, Preset::TYPE_FILAMENT, Preset::TYPE_PRINTER}) {
        fs::path dir = snapshots_dir() / section_for_type(t);
        if (!fs::is_directory(dir))
            continue;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!fs::is_regular_file(entry.path()))
                continue;
            if (entry.path().extension() != ".json")
                continue;
            sets[t].insert(entry.path().stem().string());
        }
    }

    std::map<Preset::Type, std::vector<std::string>> out;
    for (auto& [t, s] : sets)
        out[t] = std::vector<std::string>(s.begin(), s.end());
    return out;
}

void DefaultOverrideManager::rebuild_all_affected_files()
{
    ensure_directories();
    auto touched = collect_touched_presets();
    for (const auto& [type, names] : touched)
        for (const auto& name : names)
            rebuild_preset_file(type, name);
}

void DefaultOverrideManager::rebuild_preset_file(Preset::Type type, const std::string& preset_name)
{
    const fs::path sys_path  = system_preset_path(type, preset_name);
    const fs::path snap_path = snapshot_path(type, preset_name);

    if (!fs::exists(sys_path)) {
        BOOST_LOG_TRIVIAL(warning) << "[DefaultOverrideManager] system preset missing: " << sys_path.string();
        return;
    }

    auto read_json = [](const fs::path& p, nlohmann::json& out) -> bool {
        try {
            boost::nowide::ifstream ifs(p.string());
            if (!ifs) return false;
            ifs >> out;
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };

    // Collect overrides applicable to this preset.
    std::vector<const DefaultOverride*> applicable;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& ov : m_overrides) {
            if (ov.preset_type != type)
                continue;
            if (std::find(ov.targets.begin(), ov.targets.end(), preset_name) == ov.targets.end())
                continue;
            applicable.push_back(&ov);
        }
    }

    // Take a snapshot the first time we touch this file. Do this BEFORE
    // anything else mutates the on-disk file.
    if (!fs::exists(snap_path)) {
        fs::create_directories(snap_path.parent_path());
        fs::copy_file(sys_path, snap_path, fs::copy_option::overwrite_if_exists);
    } else if (!applicable.empty()) {
        // A snapshot already exists. Detect whether the live system file is
        // still consistent with (snapshot + applicable overrides). If it
        // isn't, something external has changed it - most likely a
        // BambuStudio update that replaced vendor profiles. In that case
        // re-seat the snapshot so that our overrides sit on top of the new
        // vendor defaults rather than the stale ones. We strip any key that
        // currently happens to equal one of our override values so that a
        // later unapply truly reverts to the non-overridden state.
        nlohmann::json live, snap;
        if (read_json(sys_path, live) && read_json(snap_path, snap)) {
            nlohmann::json expected = snap;
            for (const DefaultOverride* ov : applicable)
                expected[ov->setting_key] = ov->value;
            if (live != expected) {
                BOOST_LOG_TRIVIAL(info) << "[DefaultOverrideManager] refreshing "
                    "snapshot for externally-modified preset " << preset_name;
                nlohmann::json new_snap = live;
                for (const DefaultOverride* ov : applicable) {
                    auto it = new_snap.find(ov->setting_key);
                    if (it != new_snap.end() && *it == ov->value)
                        new_snap.erase(it);
                }
                try {
                    const fs::path tmp = snap_path.string() + ".tmp";
                    { boost::nowide::ofstream ofs(tmp.string()); ofs << new_snap.dump(4); }
                    fs::rename(tmp, snap_path);
                } catch (const std::exception& e) {
                    BOOST_LOG_TRIVIAL(error) << "[DefaultOverrideManager] failed to "
                        "refresh snapshot " << snap_path.string() << ": " << e.what();
                    return;
                }
            }
        }
    }

    // If nothing applies, restore the snapshot and delete it (no longer touched).
    if (applicable.empty()) {
        restore_from_snapshot(type, preset_name);
        boost::system::error_code ec;
        fs::remove(snap_path, ec);
        return;
    }

    // Start from the (possibly just-refreshed) snapshot and apply override values on top.
    nlohmann::json preset;
    if (!read_json(snap_path, preset)) {
        BOOST_LOG_TRIVIAL(error) << "[DefaultOverrideManager] cannot read snapshot "
                                 << snap_path.string();
        return;
    }

    for (const DefaultOverride* ov : applicable)
        preset[ov->setting_key] = ov->value;

    try {
        const fs::path tmp = sys_path.string() + ".tmp";
        {
            boost::nowide::ofstream ofs(tmp.string());
            ofs << preset.dump(4);
        }
        fs::rename(tmp, sys_path);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[DefaultOverrideManager] failed to write "
                                 << sys_path.string() << ": " << e.what();
    }
}

void DefaultOverrideManager::restore_from_snapshot(Preset::Type type, const std::string& preset_name)
{
    const fs::path sys_path  = system_preset_path(type, preset_name);
    const fs::path snap_path = snapshot_path(type, preset_name);
    if (!fs::exists(snap_path))
        return;
    try {
        fs::copy_file(snap_path, sys_path, fs::copy_option::overwrite_if_exists);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[DefaultOverrideManager] failed to restore "
                                 << sys_path.string() << ": " << e.what();
    }
}

// ---- Stale detection ------------------------------------------------------

StaleReport DefaultOverrideManager::check_stale() const
{
    StaleReport report;

    std::lock_guard<std::mutex> lock(m_mutex);

    // Group active overrides by (type, preset_name) for lookup.
    std::map<std::pair<Preset::Type, std::string>, std::vector<const DefaultOverride*>> by_preset;
    for (const auto& ov : m_overrides)
        for (const auto& target : ov.targets)
            by_preset[{ov.preset_type, target}].push_back(&ov);

    for (const auto& [key, ovs] : by_preset) {
        const auto& [type, preset_name] = key;
        const fs::path sys_path = system_preset_path(type, preset_name);
        if (!fs::exists(sys_path)) {
            report.mismatched_presets[preset_name] = "system preset file missing";
            continue;
        }

        nlohmann::json live;
        try {
            boost::nowide::ifstream ifs(sys_path.string());
            ifs >> live;
        } catch (const std::exception& e) {
            report.mismatched_presets[preset_name] = std::string("parse error: ") + e.what();
            continue;
        }

        // If any applicable override's value is not currently present in the file,
        // treat the preset as stale.
        for (const DefaultOverride* ov : ovs) {
            if (!live.contains(ov->setting_key) || live[ov->setting_key] != ov->value) {
                report.mismatched_presets[preset_name] = "override value not present (external change?)";
                break;
            }
        }
    }

    return report;
}

// ---- Preset enumeration + dimension parsing -------------------------------

bool DefaultOverrideManager::parse_process_preset_name(const std::string& name, PresetDimensions& out)
{
    // Examples:
    //   "0.20mm Standard @BBL X1C"                  -> 0.4 nozzle implicit
    //   "0.08mm Extra Fine @BBL A1M 0.2 nozzle"
    //   "0.16mm Balanced Strength @BBL H2D 0.6 nozzle"
    static const std::regex re(
        R"(^(\d+\.\d+mm)\s+(.+?)\s+@BBL\s+(\S+?)(?:\s+(\d+\.\d+)\s+nozzle)?\s*$)");
    std::smatch m;
    if (!std::regex_match(name, m, re))
        return false;
    out.preset_name   = name;
    out.layer_height  = m[1].str();
    out.quality_tier  = m[2].str();
    out.printer_model = m[3].str();
    out.nozzle        = m[4].matched ? m[4].str() : std::string("0.4");
    return true;
}

std::vector<PresetDimensions>
DefaultOverrideManager::list_concrete_presets(Preset::Type type) const
{
    // Walk the system preset directory directly rather than relying on
    // PresetBundle's in-memory view. The in-memory is_visible flag reflects
    // current printer compatibility filtering, which would hide presets for
    // printers the user hasn't enabled - we want to show all of them so that
    // overrides can be set proactively.
    std::vector<PresetDimensions> out;

    const fs::path dir = fs::path(data_dir()) / PRESET_SYSTEM_DIR / "BBL"
                         / section_for_type(type);
    if (!fs::is_directory(dir))
        return out;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!fs::is_regular_file(entry.path()))
            continue;
        if (entry.path().extension() != ".json")
            continue;

        const std::string name = entry.path().stem().string();

        // Skip base templates: their filenames start with "fdm_" (e.g.
        // fdm_process_single_0.20, fdm_filament_common, fdm_bbl_3dp_001_common).
        // Concrete presets contain "@BBL" in the name.
        if (name.rfind("fdm_", 0) == 0)
            continue;
        if (name.find("@") == std::string::npos)
            continue;

        PresetDimensions d;
        d.preset_name = name;
        if (type == Preset::TYPE_PRINT) {
            if (!parse_process_preset_name(name, d)) {
                // Unparseable - still include with sentinel dimension values so
                // the user can select it manually via the preset list.
                d.printer_model = "(unknown)";
                d.nozzle        = "(unknown)";
                d.layer_height  = "(unknown)";
                d.quality_tier  = "(unknown)";
            }
        }
        out.push_back(std::move(d));
    }

    std::sort(out.begin(), out.end(), [](const PresetDimensions& a, const PresetDimensions& b) {
        return a.preset_name < b.preset_name;
    });
    return out;
}

std::optional<std::string>
DefaultOverrideManager::effective_value(const std::string& preset_name,
                                        const std::string& setting_key) const
{
    PresetBundle* pb = wxGetApp().preset_bundle;
    if (!pb)
        return std::nullopt;

    // Look across all preset types until we find a match by name.
    const PresetCollection* colls[] = { &pb->prints, &pb->filaments, &pb->printers };
    for (const PresetCollection* coll : colls) {
        const Preset* preset = coll->find_preset(preset_name, false /*first_visible_if_not_found*/);
        if (!preset)
            continue;
        const ConfigOption* opt = preset->config.option(setting_key);
        if (!opt)
            return std::nullopt;
        return opt->serialize();
    }
    return std::nullopt;
}

}} // namespace Slic3r::GUI
