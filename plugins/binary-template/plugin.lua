-- Binary Template plugin for REHex
-- Copyright (C) 2021-2022 Daniel Collins <solemnwarning@solemnwarning.net>
--
-- This program is free software; you can redistribute it and/or modify it
-- under the terms of the GNU General Public License version 2 as published by
-- the Free Software Foundation.
--
-- This program is distributed in the hope that it will be useful, but WITHOUT
-- ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
-- FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
-- more details.
--
-- You should have received a copy of the GNU General Public License along with
-- this program; if not, write to the Free Software Foundation, Inc., 51
-- Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

local preprocessor = require 'preprocessor';
local parser = require 'parser';
local executor = require 'executor';

local function _find_templates(path)
	local templates = {}
	
	local dir = wx.wxDir.new(path)
	if not dir:IsOpened()
	then
		print("Unable to open " .. path)
		return templates
	end
	
	local ok, name = dir:GetFirst("*.bt", wx.wxDIR_FILES)
	while ok
	do
		table.insert(templates, { name, path .. "/" .. name })
		ok, name = dir:GetNext()
	end
	
-- 	ok, name = dir:GetFirst("*", wx.wxDIR_DIRS)
-- 	
-- 	while ok
-- 	do
-- 		local t = _find_templates(path .. "/" .. name)
-- 		
-- 		for _, v in ipairs(t)
-- 		do
-- 			table.insert(templates, v)
-- 		end
-- 		
-- 		ok, name = dir:GetNext()
-- 	end
	
	-- Sort templates by name
	table.sort(templates, function(a, b) return a[1]:upper() < b[1]:upper() end)
	
	return templates
end

local ID_BROWSE = 1
local ID_RANGE_FILE = 2
local ID_RANGE_SEL = 3

rehex.AddToToolsMenu("Execute binary template / script...", function(window)
	local tab = window:active_tab()
	local doc = window:active_document()
	
	local templates = _find_templates(rehex.PLUGIN_DIR .. "/" .. "templates")
	
	local my_window = wx.wxDialog(window, wx.wxID_ANY, "Execute binary template")
	
	local template_sizer = wx.wxStaticBoxSizer(wx.wxHORIZONTAL, my_window, "Template")
	local template_box = template_sizer:GetStaticBox()
	
	local template_choice = wx.wxChoice(template_box, wx.wxID_ANY)
	template_sizer:Add(template_choice, 1, wx.wxALL, 5)
	
	for _, v in ipairs(templates)
	do
		template_choice:Append(v[1])
	end
	
	template_choice:SetSelection(0)
	
	local browse_btn = wx.wxButton(template_box, ID_BROWSE, "Browse...")
	template_sizer:Add(browse_btn, 0, wx.wxLEFT | wx.wxRIGHT | wx.wxTOP, 5)
	
	my_window:Connect(ID_BROWSE, wx.wxEVT_BUTTON, function(event)
		local browse_dialog = wx.wxFileDialog(my_window, "Select template file", "", "", "Binary Template files (*.bt)|*.bt", wx.wxFD_OPEN | wx.wxFD_FILE_MUST_EXIST)
		local result = browse_dialog:ShowModal()
		
		if result == wx.wxID_OK
		then
			local name = browse_dialog:GetFilename()
			local path = browse_dialog:GetPath()
			
			template_choice:Append(name)
			template_choice:SetSelection(#templates)
			
			table.insert(templates, { name, path })
		end
	end)
	
	local range_sizer = wx.wxStaticBoxSizer(wx.wxVERTICAL, my_window, "Range")
	local range_box = range_sizer:GetStaticBox()
	
	local range_file = wx.wxRadioButton(range_box, ID_RANGE_FILE, "Apply template to whole file")
	range_sizer:Add(range_file)
	
	local range_sel  = wx.wxRadioButton(range_box, ID_RANGE_SEL,  "Apply template to selection only")
	range_sizer:Add(range_sel)
	
	local selection_off, selection_length = tab:get_selection_linear()
	if selection_off ~= nil
	then
		range_sel:SetValue(true)
	else
		range_sel:Disable()
		range_file:SetValue(true)
	end
	
	local ok_btn = wx.wxButton(my_window, wx.wxID_OK, "OK")
	local cancel_btn = wx.wxButton(my_window, wx.wxID_CANCEL, "Cancel")
	
	local btn_sizer = wx.wxBoxSizer(wx.wxHORIZONTAL)
	btn_sizer:Add(ok_btn)
	btn_sizer:Add(cancel_btn, 0, wx.wxLEFT, 5)
	
	local main_sizer = wx.wxBoxSizer(wx.wxVERTICAL)
	main_sizer:Add(template_sizer, 0, wx.wxEXPAND | wx.wxTOP | wx.wxLEFT | wx.wxRIGHT, 5)
	main_sizer:Add(range_sizer, 0, wx.wxEXPAND | wx.wxTOP | wx.wxLEFT | wx.wxRIGHT, 5)
	main_sizer:Add(btn_sizer, 0, wx.wxALIGN_RIGHT | wx.wxALL, 5)
	
	my_window:SetSizerAndFit(main_sizer)
	
	local btn_id = my_window:ShowModal()
	
	if btn_id == wx.wxID_OK
	then
		local template_idx = template_choice:GetSelection() + 1
		local template_path = templates[template_idx][2]
		
		local progress_dialog = wx.wxProgressDialog("Processing template", "Processing template...", 100, window, wx.wxPD_CAN_ABORT | wx.wxPD_ELAPSED_TIME)
		progress_dialog:Show()
		
		if range_file:GetValue()
		then
			selection_off = 0
			selection_length = doc:buffer_length()
		end
		
		local yield_counter = 0
		
		local interface = {
			set_data_type = function(offset, length, data_type)
				doc:set_data_type(selection_off + offset, length, data_type)
			end,
			
			set_comment = function(offset, length, text)
				doc:set_comment(selection_off + offset, length, rehex.Comment.new(text))
			end,
			
			read_data = function(offset, length)
				return doc:read_data(selection_off + offset, length)
			end,
			
			file_length = function()
				return selection_length
			end,
			
			print = function(s) rehex.print_info(s) end,
			
			yield = function()
				-- The yield method gets called at least once for every statement
				-- as it gets executed, don't pump the event loop every time or we
				-- wind up spending all our time doing that.
				--
				-- There isn't any (portable) time counter I can check in Lua, so
				-- the interval is an arbitrarily chosen number that seems to give
				-- (more than) good responsiveness on my PC and speeds up execution
				-- of an idle loop by ~10x ish.
				
				if yield_counter < 8000
				then
					yield_counter = yield_counter + 1
					return
				end
				
				yield_counter = 0
				
				progress_dialog:Pulse()
				wx.wxGetApp():ProcessPendingEvents()
				
				if progress_dialog:WasCancelled()
				then
					error("Template execution aborted", 0)
				end
			end,
		}
		
		doc:transact_begin("Binary template")
		
		local ok, err = pcall(function()
			executor.execute(interface, parser.parse_text(preprocessor.preprocess_file(template_path)))
		end)
		
		progress_dialog:Destroy()
		
		if ok
		then
			doc:transact_commit()
		else
			doc:transact_rollback()
			wx.wxMessageBox(err, "Error")
		end
	end
end);
