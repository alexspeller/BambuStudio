#ifndef slic3r_UpdateDefaultsDialog_hpp_
#define slic3r_UpdateDefaultsDialog_hpp_

#include <string>
#include <vector>
#include <map>

#include <wx/dialog.h>

#include "GUI_Utils.hpp"
#include "DefaultOverrideManager.hpp"
#include "libslic3r/Preset.hpp"

class wxCheckBox;
class wxScrolledWindow;
class wxStaticText;
class wxBoxSizer;

namespace Slic3r { namespace GUI {

class ConfigOptionsGroup;

// Dialog that lets the user push the current value of a setting into one or
// more system presets as a managed override. Invoked from the right-click /
// broadcast-icon affordances on individual settings in the Print/Filament/
// Printer tabs.
class UpdateDefaultsDialog : public DPIDialog
{
public:
    // Entry point used by OG_CustomCtrl. Constructs and modally shows the
    // dialog. Returns after the dialog is dismissed. If an override is applied,
    // triggers GUI_App::reload_presets_after_override() before returning.
    static void show_for(wxWindow* parent,
                         const std::string& opt_key,
                         const wxString& opt_label,
                         Preset::Type preset_type,
                         ConfigOptionsGroup* src_group);

    // Edit flow from the overrides manager. Pre-populates the dialog with an
    // existing override's value and target list.
    static void show_for_edit(wxWindow* parent, const std::string& override_id);

    UpdateDefaultsDialog(wxWindow* parent,
                         const std::string& opt_key,
                         const wxString& opt_label,
                         Preset::Type preset_type,
                         const nlohmann::json& new_value,
                         const std::string& current_value_str,
                         const std::vector<std::string>& initial_targets,
                         const std::string& edit_override_id, /* empty for new */
                         const std::string& starting_preset /* name of the preset the user was editing; always pre-selected */);

    bool was_applied() const { return m_applied; }

protected:
    void on_dpi_changed(const wxRect& /*suggested_rect*/) override {}

private:
    struct PresetRow {
        PresetDimensions dims;
        wxCheckBox*      checkbox = nullptr;
        wxStaticText*    value_label = nullptr;
        std::string      current_effective_value; // from PresetBundle, for display
    };

    void build_ui();
    void populate_filters();
    void populate_preset_list();
    void refresh_preset_visibility();
    void update_count_label();

    void on_apply(wxCommandEvent&);
    void on_cancel(wxCommandEvent&);
    void on_select_all(wxCommandEvent&);
    void on_select_none(wxCommandEvent&);

    // Identity
    std::string      m_opt_key;
    wxString         m_opt_label;
    Preset::Type     m_preset_type;
    nlohmann::json   m_new_value;
    std::string      m_current_value_str; // how the new value displays
    std::string      m_edit_override_id;  // empty = new override
    std::string      m_starting_preset;   // preset the user was editing; always pre-selected

    // Dimension filter state: dim name -> (value -> checkbox)
    std::map<std::string, std::map<std::string, wxCheckBox*>> m_filter_checks;
    // Initial target set - used to default-check presets on new-override flow.
    std::vector<std::string> m_initial_targets;

    // Controls
    wxPanel*          m_filter_panel  = nullptr;
    wxScrolledWindow* m_preset_scroll = nullptr;
    wxStaticText*     m_count_label   = nullptr;
    std::vector<PresetRow> m_rows;

    bool m_applied = false;
};

}} // namespace Slic3r::GUI

#endif // slic3r_UpdateDefaultsDialog_hpp_
