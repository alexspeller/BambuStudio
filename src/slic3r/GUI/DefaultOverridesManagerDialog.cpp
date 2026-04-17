#include "DefaultOverridesManagerDialog.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/msgdlg.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "DefaultOverrideManager.hpp"
#include "UpdateDefaultsDialog.hpp"

namespace Slic3r { namespace GUI {

void DefaultOverridesManagerDialog::show(wxWindow* parent)
{
    DefaultOverridesManagerDialog dlg(parent);
    dlg.ShowModal();
}

DefaultOverridesManagerDialog::DefaultOverridesManagerDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Default Overrides"),
                wxDefaultPosition, wxSize(720, 420),
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    build_ui();
    refresh_list();
    CenterOnParent();
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
            if (e.is_string()) s += e.get<std::string>();
            else               s += e.dump();
        }
        s += "]";
        return s;
    }
    return v.dump();
}

static std::string section_name_for(Preset::Type t)
{
    switch (t) {
    case Preset::TYPE_PRINT:    return "process";
    case Preset::TYPE_FILAMENT: return "filament";
    case Preset::TYPE_PRINTER:  return "machine";
    default:                    return "?";
    }
}

void DefaultOverridesManagerDialog::build_ui()
{
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* intro = new wxStaticText(this, wxID_ANY,
        _L("Default overrides push a single setting value into the read-only "
           "system presets. They persist across BambuStudio restarts, but may "
           "need re-applying after BambuStudio updates."));
    intro->Wrap(680);
    root->Add(intro, 0, wxALL, 12);

    m_list = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition,
                                    wxDefaultSize, wxDV_ROW_LINES);
    m_list->AppendTextColumn(_L("Setting"),   wxDATAVIEW_CELL_INERT, 200);
    m_list->AppendTextColumn(_L("Value"),     wxDATAVIEW_CELL_INERT, 160);
    m_list->AppendTextColumn(_L("Type"),      wxDATAVIEW_CELL_INERT, 80);
    m_list->AppendTextColumn(_L("Presets"),   wxDATAVIEW_CELL_INERT, 80);
    // Hidden id column for lookup - wx has no true hidden columns on all
    // platforms so we use a narrow column; it's a minor cosmetic issue that
    // can be polished later.
    m_list->AppendTextColumn("id", wxDATAVIEW_CELL_INERT, 0);
    root->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* btn_edit = new wxButton(this, wxID_ANY, _L("Edit..."));
    auto* btn_undo = new wxButton(this, wxID_ANY, _L("Undo"));
    auto* btn_all  = new wxButton(this, wxID_ANY, _L("Undo All"));
    btn_edit->Bind(wxEVT_BUTTON, &DefaultOverridesManagerDialog::on_edit_selected, this);
    btn_undo->Bind(wxEVT_BUTTON, &DefaultOverridesManagerDialog::on_undo_selected, this);
    btn_all->Bind (wxEVT_BUTTON, &DefaultOverridesManagerDialog::on_undo_all, this);
    btn_sizer->Add(btn_edit, 0, wxRIGHT, 8);
    btn_sizer->Add(btn_undo, 0, wxRIGHT, 8);
    btn_sizer->Add(btn_all,  0);
    btn_sizer->AddStretchSpacer(1);
    auto* btn_close = new wxButton(this, wxID_CLOSE, _L("Close"));
    btn_close->Bind(wxEVT_BUTTON, &DefaultOverridesManagerDialog::on_close, this);
    btn_sizer->Add(btn_close, 0);
    root->Add(btn_sizer, 0, wxEXPAND | wxALL, 12);

    SetSizer(root);
    Layout();
}

void DefaultOverridesManagerDialog::refresh_list()
{
    m_list->DeleteAllItems();
    auto* mgr = wxGetApp().default_override_manager;
    if (!mgr) return;

    for (const auto& ov : mgr->overrides()) {
        wxVector<wxVariant> row;
        row.push_back(wxVariant(wxString::FromUTF8(ov.setting_key.c_str())));
        row.push_back(wxVariant(wxString::FromUTF8(
            json_value_as_display_string(ov.value).c_str())));
        row.push_back(wxVariant(wxString::FromUTF8(
            section_name_for(ov.preset_type).c_str())));
        row.push_back(wxVariant(wxString::Format("%d", (int)ov.targets.size())));
        row.push_back(wxVariant(wxString::FromUTF8(ov.id.c_str())));
        m_list->AppendItem(row);
    }
}

void DefaultOverridesManagerDialog::on_undo_selected(wxCommandEvent&)
{
    int sel = m_list->GetSelectedRow();
    if (sel == wxNOT_FOUND)
        return;
    wxVariant v;
    m_list->GetValue(v, sel, 4);
    std::string id = v.GetString().ToUTF8().data();
    if (id.empty()) return;

    if (wxMessageBox(_L("Undo this override? The affected system presets will "
                        "be restored to their original values."),
                     _L("Default Overrides"),
                     wxICON_QUESTION | wxYES_NO, this) != wxYES)
        return;

    wxGetApp().default_override_manager->remove_override(id);
    refresh_list();
    wxGetApp().CallAfter([] { wxGetApp().reload_presets_after_override(); });
}

void DefaultOverridesManagerDialog::on_edit_selected(wxCommandEvent&)
{
    int sel = m_list->GetSelectedRow();
    if (sel == wxNOT_FOUND)
        return;
    wxVariant v;
    m_list->GetValue(v, sel, 4);
    std::string id = v.GetString().ToUTF8().data();
    if (id.empty()) return;

    UpdateDefaultsDialog::show_for_edit(this, id);
    refresh_list();
}

void DefaultOverridesManagerDialog::on_undo_all(wxCommandEvent&)
{
    if (wxMessageBox(_L("Remove ALL default overrides? All affected system "
                        "presets will be restored to their original values."),
                     _L("Default Overrides"),
                     wxICON_EXCLAMATION | wxYES_NO, this) != wxYES)
        return;

    wxGetApp().default_override_manager->remove_all();
    refresh_list();
    wxGetApp().CallAfter([] { wxGetApp().reload_presets_after_override(); });
}

void DefaultOverridesManagerDialog::on_close(wxCommandEvent&)
{
    EndModal(wxID_CLOSE);
}

}} // namespace Slic3r::GUI
