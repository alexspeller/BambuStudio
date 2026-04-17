#ifndef slic3r_DefaultOverridesManagerDialog_hpp_
#define slic3r_DefaultOverridesManagerDialog_hpp_

#include <wx/dialog.h>
#include <wx/dataview.h>

#include "GUI_Utils.hpp"

namespace Slic3r { namespace GUI {

// Lists all currently-active default overrides and lets the user undo or edit
// them. Also detects overrides that have become stale after an external change
// (typically a BambuStudio update that refreshed system presets) and offers a
// re-apply action.
class DefaultOverridesManagerDialog : public DPIDialog
{
public:
    static void show(wxWindow* parent);

    DefaultOverridesManagerDialog(wxWindow* parent);

protected:
    void on_dpi_changed(const wxRect& /*suggested_rect*/) override {}

private:
    void build_ui();
    void refresh_list();
    void on_undo_selected(wxCommandEvent&);
    void on_edit_selected(wxCommandEvent&);
    void on_undo_all(wxCommandEvent&);
    void on_close(wxCommandEvent&);

    // Row data model: we use wxDataViewListCtrl and store the override id in
    // the hidden last column for lookup.
    wxDataViewListCtrl* m_list = nullptr;
};

}} // namespace Slic3r::GUI

#endif // slic3r_DefaultOverridesManagerDialog_hpp_
