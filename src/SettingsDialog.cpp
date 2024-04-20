/* Reverse Engineer's Hex Editor
 * Copyright (C) 2024 Daniel Collins <solemnwarning@solemnwarning.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "platform.hpp"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/statline.h>

#include "SettingsDialog.hpp"

BEGIN_EVENT_TABLE(REHex::SettingsDialog, wxDialog)
	EVT_TREE_SEL_CHANGED(wxID_ANY, REHex::SettingsDialog::OnTreeSelect)
	EVT_BUTTON(wxID_OK, REHex::SettingsDialog::OnOK)
END_EVENT_TABLE()

REHex::SettingsDialog::SettingsDialog(wxWindow *parent, std::vector< std::unique_ptr<SettingsDialogPanel> > &&panels):
	wxDialog(parent, wxID_ANY, "test"),
	panels(std::move(panels))
{
	wxSizer *top_sizer = new wxBoxSizer(wxVERTICAL);
	
	wxSizer *tree_panel_sizer = new wxBoxSizer(wxHORIZONTAL);
	top_sizer->Add(tree_panel_sizer, 1);
	
	treectrl = new wxTreeCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, (wxTR_HAS_BUTTONS | wxTR_HIDE_ROOT));
	tree_panel_sizer->Add(treectrl, 0, wxEXPAND);
	
	wxTreeItemId tree_root = treectrl->AddRoot(wxEmptyString);
	
	for(auto p = this->panels.begin(); p != this->panels.end(); ++p)
	{
		(*p)->Create(this);
		tree_panel_sizer->Add(p->get(), 1, wxEXPAND);
		
		wxTreeItemId p_item = treectrl->AppendItem(tree_root, (*p)->label());
		
		assert(panel_tree_items.find(p_item) == panel_tree_items.end());
		panel_tree_items[p_item] = p->get();
		
		if(p == this->panels.begin())
		{
			treectrl->SelectItem(p_item);
		}
		else{
			(*p)->Hide();
		}
	}
	
	treectrl->SetMinSize(wxSize(200, 600));
	
	wxSizer *button_sizer = new wxBoxSizer(wxHORIZONTAL);
	top_sizer->Add(button_sizer);
	
	wxButton *ok_button = new wxButton(this, wxID_OK);
	button_sizer->Add(ok_button);
	
	wxButton *cancel_button = new wxButton(this, wxID_CANCEL);
	button_sizer->Add(cancel_button);
	
	SetSizerAndFit(top_sizer);
}

void REHex::SettingsDialog::OnTreeSelect(wxTreeEvent &event)
{
	auto old_item = panel_tree_items.find(event.GetOldItem());
	if(old_item != panel_tree_items.end())
	{
		old_item->second->Hide();
	}
	
	auto new_item = panel_tree_items.find(event.GetItem());
	if(new_item != panel_tree_items.end())
	{
		new_item->second->Show();
	}
	
	Layout();
}

void REHex::SettingsDialog::OnOK(wxCommandEvent &event)
{
	for(auto i = panels.begin(); i != panels.end(); ++i)
	{
		if(!(*i)->validate())
		{
			/* TODO */
			abort();
		}
	}
	
	for(auto i = panels.begin(); i != panels.end(); ++i)
	{
		(*i)->save();
	}
	
	EndModal(wxID_OK);
}
