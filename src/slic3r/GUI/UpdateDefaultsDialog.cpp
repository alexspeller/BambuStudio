#include "UpdateDefaultsDialog.hpp"

#include <algorithm>
#include <set>

#include <wx/checkbox.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/scrolwin.h>
#include <wx/button.h>
#include <wx/msgdlg.h>

#include "GUI_App.hpp"
#include "OptionsGroup.hpp"
#include "I18N.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PresetBundle.hpp"

namespace Slic3r { namespace GUI {

// ---- value serialization ---------------------------------------------------

// Converts the current value of opt_key in config into a nlohmann::json value
// matching the shape that BambuStudio writes to its preset JSON files:
//   - scalars are stored as JSON strings
//   - vectors are stored as JSON arrays of strings
// This mirrors ConfigBase::save_to_json (libslic3r/Config.cpp).
static nlohmann::json serialize_option_for_preset(const DynamicPrintConfig& config,
                                                  const std::string& opt_key)
{
    const ConfigOption* opt = config.option(opt_key);
    if (!opt)
        return nullptr;
    if (opt->is_scalar()) {
        if (opt->type() == coString)
            return static_cast<const ConfigOptionString*>(opt)->value;
        return opt->serialize();
    } else {
        const ConfigOptionVectorBase* vec = static_cast<const ConfigOptionVectorBase*>(opt);
        std::vector<std::string> string_values = vec->vserialize();
        return nlohmann::json(string_values);
    }
}

static std::string json_value_as_display_string(const nlohmann::json& v)
{
    if (v.is_string()) return v.get<std::string>();
    if (v.is_array()) {
        std::string s = "[";
        bool first = true;
        for (const auto& e : v) {
            if (!first) s += ", ";
            first = false;
            if (e.is_string())  s += e.get<std::string>();
            else                s += e.dump();
        }
        s += "]";
        return s;
    }
    return v.dump();
}

// ---- static entry points --------------------------------------------------

void UpdateDefaultsDialog::show_for(wxWindow* parent,
                                    const std::string& opt_key,
                                    const wxString& opt_label,
                                    Preset::Type preset_type,
                                    ConfigOptionsGroup* src_group)
{
    if (!src_group || !src_group->get_config()) {
        wxMessageBox(_L("No config available for this setting."), _L("Update Defaults"),
                     wxICON_ERROR | wxOK, parent);
        return;
    }

    const DynamicPrintConfig* config = src_group->get_config();
    nlohmann::json new_value = serialize_option_for_preset(*config, opt_key);
    if (new_value.is_null()) {
        wxMessageBox(_L("Could not read current value for this setting."),
                     _L("Update Defaults"), wxICON_ERROR | wxOK, parent);
        return;
    }
    std::string current_value_str = json_value_as_display_string(new_value);

    // The preset the user was editing when they invoked this is the natural
    // "starting point" for the override - pre-select it regardless of whether
    // its current on-disk value already matches (it may already match due to
    // a prior override or a BBL default, but the user clearly intends it).
    std::string starting_preset;
    PresetBundle* pb = wxGetApp().preset_bundle;
    if (pb) {
        const PresetCollection* coll = nullptr;
        switch (preset_type) {
        case Preset::TYPE_PRINT:    coll = &pb->prints;    break;
        case Preset::TYPE_FILAMENT: coll = &pb->filaments; break;
        case Preset::TYPE_PRINTER:  coll = &pb->printers;  break;
        default: break;
        }
        if (coll)
            starting_preset = coll->get_edited_preset().name;
    }

    UpdateDefaultsDialog dlg(parent, opt_key, opt_label, preset_type,
                             new_value, current_value_str,
                             /*initial_targets*/ {}, /*edit_override_id*/ "",
                             starting_preset);
    dlg.ShowModal();

    // Defer the reload so the dialog can finish destroying before
    // PresetBundle is torn down and rebuilt - otherwise we'd be tearing
    // down tabs/params panels while the event that opened the dialog is
    // still unwinding above us.
    if (dlg.was_applied())
        wxGetApp().CallAfter([] { wxGetApp().reload_presets_after_override(); });
}

void UpdateDefaultsDialog::show_for_edit(wxWindow* parent, const std::string& override_id)
{
    auto mgr = wxGetApp().default_override_manager;
    if (!mgr) return;
    auto maybe = mgr->find(override_id);
    if (!maybe) return;
    const DefaultOverride& ov = *maybe;

    UpdateDefaultsDialog dlg(parent, ov.setting_key,
                             wxString::FromUTF8(ov.setting_key.c_str()),
                             ov.preset_type, ov.value,
                             json_value_as_display_string(ov.value),
                             ov.targets, ov.id, /*starting_preset*/ "");
    dlg.ShowModal();

    if (dlg.was_applied())
        wxGetApp().CallAfter([] { wxGetApp().reload_presets_after_override(); });
}

// ---- construction ---------------------------------------------------------

UpdateDefaultsDialog::UpdateDefaultsDialog(wxWindow* parent,
                                           const std::string& opt_key,
                                           const wxString& opt_label,
                                           Preset::Type preset_type,
                                           const nlohmann::json& new_value,
                                           const std::string& current_value_str,
                                           const std::vector<std::string>& initial_targets,
                                           const std::string& edit_override_id,
                                           const std::string& starting_preset)
    : DPIDialog(parent, wxID_ANY, _L("Update Defaults"),
                wxDefaultPosition, wxSize(720, 640),
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_opt_key(opt_key)
    , m_opt_label(opt_label)
    , m_preset_type(preset_type)
    , m_new_value(new_value)
    , m_current_value_str(current_value_str)
    , m_edit_override_id(edit_override_id)
    , m_starting_preset(starting_preset)
    , m_initial_targets(initial_targets)
{
    build_ui();
    populate_filters();
    populate_preset_list();
    update_count_label();
    CenterOnParent();
}

// ---- UI construction ------------------------------------------------------

void UpdateDefaultsDialog::build_ui()
{
    auto* root = new wxBoxSizer(wxVERTICAL);

    // Header
    auto* header_text = new wxStaticText(this, wxID_ANY, wxString::Format(
        _L("Setting: %s  (%s)"), m_opt_label,
        wxString::FromUTF8(m_opt_key.c_str())));
    wxFont bold = header_text->GetFont();
    bold.SetWeight(wxFONTWEIGHT_BOLD);
    header_text->SetFont(bold);
    root->Add(header_text, 0, wxALL, 12);

    auto* value_text = new wxStaticText(this, wxID_ANY, wxString::Format(
        _L("New value: %s"), wxString::FromUTF8(m_current_value_str.c_str())));
    root->Add(value_text, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    // Filter area - populated later in populate_filters().
    auto* filter_label = new wxStaticText(this, wxID_ANY, _L("Scope"));
    filter_label->SetFont(bold);
    root->Add(filter_label, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    m_filter_panel = new wxPanel(this);
    m_filter_panel->SetSizer(new wxBoxSizer(wxVERTICAL));
    root->Add(m_filter_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    // Preset list header
    m_count_label = new wxStaticText(this, wxID_ANY, "");
    root->Add(m_count_label, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    // Scrolled preset checklist
    m_preset_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition,
                                           wxDefaultSize,
                                           wxVSCROLL | wxBORDER_SIMPLE);
    m_preset_scroll->SetScrollRate(0, 16);
    auto* list_sizer = new wxBoxSizer(wxVERTICAL);
    m_preset_scroll->SetSizer(list_sizer);
    root->Add(m_preset_scroll, 1, wxEXPAND | wxALL, 12);

    // Selection helpers
    auto* sel_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* btn_all  = new wxButton(this, wxID_ANY, _L("Select All"));
    auto* btn_none = new wxButton(this, wxID_ANY, _L("Deselect All"));
    btn_all->Bind(wxEVT_BUTTON, &UpdateDefaultsDialog::on_select_all, this);
    btn_none->Bind(wxEVT_BUTTON, &UpdateDefaultsDialog::on_select_none, this);
    sel_sizer->Add(btn_all, 0, wxRIGHT, 8);
    sel_sizer->Add(btn_none, 0);
    root->Add(sel_sizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    // Action buttons
    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer(1);
    auto* btn_apply  = new wxButton(this, wxID_OK,     _L("Apply"));
    auto* btn_cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    btn_apply->Bind(wxEVT_BUTTON, &UpdateDefaultsDialog::on_apply, this);
    btn_cancel->Bind(wxEVT_BUTTON, &UpdateDefaultsDialog::on_cancel, this);
    btn_sizer->Add(btn_apply, 0, wxRIGHT, 8);
    btn_sizer->Add(btn_cancel, 0);
    root->Add(btn_sizer, 0, wxEXPAND | wxALL, 12);

    SetSizer(root);
    Layout();
}

// ---- filter population ----------------------------------------------------

void UpdateDefaultsDialog::populate_filters()
{
    auto* mgr = wxGetApp().default_override_manager;
    if (!mgr) return;

    auto all_presets = mgr->list_concrete_presets(m_preset_type);

    // Gather unique values per dimension. We only populate the dimensions that
    // apply to this preset type - for process we have all four, for filament/
    // machine we'd want different dimensions (left for v2).
    std::set<std::string> printers, nozzles, heights, tiers;
    for (const auto& p : all_presets) {
        if (!p.printer_model.empty()) printers.insert(p.printer_model);
        if (!p.nozzle.empty())        nozzles.insert(p.nozzle);
        if (!p.layer_height.empty())  heights.insert(p.layer_height);
        if (!p.quality_tier.empty())  tiers.insert(p.quality_tier);
    }

    if (!m_filter_panel) return;
    auto* filter_sizer = m_filter_panel->GetSizer();

    auto add_dimension_row = [&](const std::string& dim_label,
                                 const std::string& dim_key,
                                 const std::set<std::string>& values) {
        if (values.empty())
            return;
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* label = new wxStaticText(m_filter_panel, wxID_ANY,
            wxString::FromUTF8(dim_label.c_str()) + ":",
            wxDefaultPosition, wxSize(100, -1));
        row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

        for (const auto& v : values) {
            auto* cb = new wxCheckBox(m_filter_panel, wxID_ANY,
                                      wxString::FromUTF8(v.c_str()));
            cb->SetValue(true);
            cb->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
                refresh_preset_visibility();
                update_count_label();
            });
            m_filter_checks[dim_key][v] = cb;
            row->Add(cb, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        }
        filter_sizer->Add(row, 0, wxEXPAND | wxBOTTOM, 4);
    };

    if (m_preset_type == Preset::TYPE_PRINT) {
        add_dimension_row("Printer",      "printer", printers);
        add_dimension_row("Nozzle",       "nozzle",  nozzles);
        add_dimension_row("Layer Height", "height",  heights);
        add_dimension_row("Quality",      "quality", tiers);
    } else {
        // For filament/machine we just show printer filters for now.
        add_dimension_row("Printer", "printer", printers);
    }

    m_filter_panel->Layout();
}

// ---- preset list population -----------------------------------------------

void UpdateDefaultsDialog::populate_preset_list()
{
    auto* mgr = wxGetApp().default_override_manager;
    if (!mgr) return;

    auto all_presets = mgr->list_concrete_presets(m_preset_type);

    auto* list_sizer = m_preset_scroll->GetSizer();

    std::set<std::string> initial_set(m_initial_targets.begin(), m_initial_targets.end());

    for (const auto& dims : all_presets) {
        PresetRow row;
        row.dims = dims;

        // Effective current value, for display.
        auto effective = mgr->effective_value(dims.preset_name, m_opt_key);
        row.current_effective_value = effective.value_or("");

        auto* row_panel = new wxPanel(m_preset_scroll);
        auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);
        row_panel->SetSizer(row_sizer);

        row.checkbox = new wxCheckBox(row_panel, wxID_ANY,
                                       wxString::FromUTF8(dims.preset_name.c_str()));

        // Edit flow: check exactly the presets that were targeted by the
        //            existing override.
        // New flow:  always check the preset the user was editing when they
        //            invoked this (m_starting_preset), plus any other preset
        //            whose current effective value differs from the new one.
        bool should_check;
        if (!m_edit_override_id.empty()) {
            should_check = initial_set.count(dims.preset_name) > 0;
        } else {
            std::string new_as_str;
            if (m_new_value.is_string())
                new_as_str = m_new_value.get<std::string>();
            else
                new_as_str = m_new_value.dump();
            should_check = (dims.preset_name == m_starting_preset)
                        || (row.current_effective_value != new_as_str);
        }
        row.checkbox->SetValue(should_check);

        row_sizer->Add(row.checkbox, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

        wxString value_line;
        if (row.current_effective_value.empty())
            value_line = wxString::Format(_L("-> %s"),
                wxString::FromUTF8(m_current_value_str.c_str()));
        else
            value_line = wxString::Format("%s -> %s",
                wxString::FromUTF8(row.current_effective_value.c_str()),
                wxString::FromUTF8(m_current_value_str.c_str()));
        row.value_label = new wxStaticText(row_panel, wxID_ANY, value_line);
        row_sizer->Add(row.value_label, 0, wxALIGN_CENTER_VERTICAL);

        list_sizer->Add(row_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

        row.checkbox->Bind(wxEVT_CHECKBOX,
                           [this](wxCommandEvent&) { update_count_label(); });

        m_rows.push_back(row);
    }

    m_preset_scroll->FitInside();
}

void UpdateDefaultsDialog::refresh_preset_visibility()
{
    auto is_checked = [this](const std::string& dim_key, const std::string& value) {
        auto it = m_filter_checks.find(dim_key);
        if (it == m_filter_checks.end()) return true;
        auto vit = it->second.find(value);
        if (vit == it->second.end())     return true;
        return vit->second->IsChecked();
    };

    for (auto& row : m_rows) {
        bool visible = true;
        if (m_preset_type == Preset::TYPE_PRINT) {
            visible = visible && is_checked("printer", row.dims.printer_model);
            visible = visible && is_checked("nozzle",  row.dims.nozzle);
            visible = visible && is_checked("height",  row.dims.layer_height);
            visible = visible && is_checked("quality", row.dims.quality_tier);
        }
        wxWindow* parent = row.checkbox->GetParent();
        parent->Show(visible);
    }
    m_preset_scroll->Layout();
    m_preset_scroll->FitInside();
}

void UpdateDefaultsDialog::update_count_label()
{
    int total = 0, selected = 0;
    for (const auto& row : m_rows) {
        wxWindow* parent = row.checkbox->GetParent();
        if (!parent->IsShown()) continue;
        total++;
        if (row.checkbox->IsChecked())
            selected++;
    }
    m_count_label->SetLabel(wxString::Format(
        _L("%d of %d presets selected"), selected, total));
}

// ---- actions --------------------------------------------------------------

void UpdateDefaultsDialog::on_select_all(wxCommandEvent&)
{
    for (auto& row : m_rows) {
        wxWindow* parent = row.checkbox->GetParent();
        if (!parent->IsShown()) continue;
        row.checkbox->SetValue(true);
    }
    update_count_label();
}

void UpdateDefaultsDialog::on_select_none(wxCommandEvent&)
{
    for (auto& row : m_rows) {
        wxWindow* parent = row.checkbox->GetParent();
        if (!parent->IsShown()) continue;
        row.checkbox->SetValue(false);
    }
    update_count_label();
}

void UpdateDefaultsDialog::on_apply(wxCommandEvent&)
{
    std::vector<std::string> targets;
    for (const auto& row : m_rows) {
        if (row.checkbox->IsChecked())
            targets.push_back(row.dims.preset_name);
    }

    if (targets.empty()) {
        wxMessageBox(_L("No presets selected."), _L("Update Defaults"),
                     wxICON_INFORMATION | wxOK, this);
        return;
    }

    auto* mgr = wxGetApp().default_override_manager;
    if (!mgr) {
        EndModal(wxID_CANCEL);
        return;
    }

    if (m_edit_override_id.empty()) {
        mgr->add_override(m_opt_key, m_new_value, m_preset_type, targets,
                          /*description*/ std::string());
    } else {
        mgr->update_override(m_edit_override_id, m_new_value, targets);
    }

    m_applied = true;
    EndModal(wxID_OK);
}

void UpdateDefaultsDialog::on_cancel(wxCommandEvent&)
{
    EndModal(wxID_CANCEL);
}

}} // namespace Slic3r::GUI
