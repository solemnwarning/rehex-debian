/* Reverse Engineer's Hex Editor
 * Copyright (C) 2017-2021 Daniel Collins <solemnwarning@solemnwarning.net>
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
#include <algorithm>
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <iterator>
#include <jansson.h>
#include <limits>
#include <map>
#include <stack>
#include <string>
#include <wx/clipbrd.h>
#include <wx/dcbuffer.h>

#include "App.hpp"
#include "document.hpp"
#include "DocumentCtrl.hpp"
#include "Events.hpp"
#include "Palette.hpp"
#include "textentrydialog.hpp"
#include "util.hpp"

static_assert(std::numeric_limits<json_int_t>::max() >= std::numeric_limits<off_t>::max(),
	"json_int_t must be large enough to store any offset in an off_t");

/* Is the given byte a printable 7-bit ASCII character? */
static bool isasciiprint(int c)
{
	return (c >= ' ' && c <= '~');
}

enum {
	ID_REDRAW_CURSOR = 1,
	ID_SELECT_TIMER,
};

BEGIN_EVENT_TABLE(REHex::DocumentCtrl, wxControl)
	EVT_PAINT(REHex::DocumentCtrl::OnPaint)
	EVT_ERASE_BACKGROUND(REHex::DocumentCtrl::OnErase)
	EVT_SIZE(REHex::DocumentCtrl::OnSize)
	EVT_SCROLLWIN(REHex::DocumentCtrl::OnScroll)
	EVT_MOUSEWHEEL(REHex::DocumentCtrl::OnWheel)
	EVT_CHAR(REHex::DocumentCtrl::OnChar)
	EVT_LEFT_DOWN(REHex::DocumentCtrl::OnLeftDown)
	EVT_LEFT_UP(REHex::DocumentCtrl::OnLeftUp)
	EVT_RIGHT_DOWN(REHex::DocumentCtrl::OnRightDown)
	EVT_MOTION(REHex::DocumentCtrl::OnMotion)
	EVT_TIMER(ID_SELECT_TIMER, REHex::DocumentCtrl::OnSelectTick)
	EVT_TIMER(ID_REDRAW_CURSOR, REHex::DocumentCtrl::OnRedrawCursor)
	EVT_IDLE(REHex::DocumentCtrl::OnIdle)
END_EVENT_TABLE()

REHex::DocumentCtrl::DocumentCtrl(wxWindow *parent, SharedDocumentPointer &doc):
	wxControl(),
	doc(doc),
	hex_font(wxFontInfo().Family(wxFONTFAMILY_MODERN)),
	linked_scroll_prev(NULL),
	linked_scroll_next(NULL),
	selection_begin(-1),
	selection_end(-1),
	redraw_cursor_timer(this, ID_REDRAW_CURSOR),
	mouse_select_timer(this, ID_SELECT_TIMER)
{
	App &app = wxGetApp();
	
	app.Bind(FONT_SIZE_ADJUSTMENT_CHANGED, &REHex::DocumentCtrl::OnFontSizeAdjustmentChanged, this);
	
	int font_size_adjustment = app.get_font_size_adjustment();
	
	while(font_size_adjustment > 0) { hex_font.MakeLarger(); --font_size_adjustment; }
	while(font_size_adjustment < 0) { hex_font.MakeSmaller(); ++font_size_adjustment; }
	
	/* The background style MUST be set before the control is created. */
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
		(wxVSCROLL | wxHSCROLL | wxWANTS_CHARS));
	
	client_width      = 0;
	client_height     = 0;
	visible_lines     = 1;
	bytes_per_line    = BYTES_PER_LINE_FIT_BYTES;
	bytes_per_group   = 4;
	offset_display_base = OFFSET_BASE_HEX;
	show_ascii        = true;
	highlight_selection_match = false;
	scroll_xoff       = 0;
	scroll_yoff       = 0;
	scroll_yoff_max   = 0;
	scroll_ydiv       = 1;
	wheel_vert_accum  = 0;
	wheel_horiz_accum = 0;
	selection_off     = 0;
	selection_length  = 0;
	cursor_visible    = true;
	mouse_down_area   = GenericDataRegion::SA_NONE;
	mouse_shift_initial = -1;
	cursor_state      = Document::CSTATE_HEX;
	
	assert(hex_font.IsFixedWidth());
	
	{
		wxClientDC dc(this);
		dc.SetFont(hex_font);
		
		wxSize hf_char_size = dc.GetTextExtent("X");
		hf_height           = hf_char_size.GetHeight();
		
		/* Precompute widths for hf_string_width() */
		
		for(unsigned int i = 0; i < PRECOMP_HF_STRING_WIDTH_TO; ++i)
		{
			hf_string_width_precomp[i]
				= dc.GetTextExtent(std::string((i + 1), 'X')).GetWidth();
		}
	}
	
	redraw_cursor_timer.Start(750, wxTIMER_CONTINUOUS);
	
	/* SetDoubleBuffered() isn't implemented on all platforms. */
	#if defined(__WXMSW__) || defined(__WXGTK__)
	SetDoubleBuffered(true);
	#endif
	
	SetMinClientSize(wxSize(hf_string_width(10), (hf_height * 20)));
}

REHex::DocumentCtrl::~DocumentCtrl()
{
	if(linked_scroll_prev != NULL || linked_scroll_next != NULL)
	{
		linked_scroll_remove_self();
	}
	
	for(auto region = regions.begin(); region != regions.end(); ++region)
	{
		delete *region;
	}
	
	wxGetApp().Unbind(FONT_SIZE_ADJUSTMENT_CHANGED, &REHex::DocumentCtrl::OnFontSizeAdjustmentChanged, this);
}

void REHex::DocumentCtrl::OnFontSizeAdjustmentChanged(FontSizeAdjustmentEvent &event)
{
	hex_font = wxFont(wxFontInfo().Family(wxFONTFAMILY_MODERN));
	
	for(int i = 0; i < event.font_size_adjustment; ++i) { hex_font.MakeLarger(); }
	for(int i = 0; i > event.font_size_adjustment; --i) { hex_font.MakeSmaller(); }
	
	assert(hex_font.IsFixedWidth());
	
	{
		wxClientDC dc(this);
		dc.SetFont(hex_font);
		
		hf_height = dc.GetTextExtent("X").GetHeight();
		
		/* Precompute widths for hf_string_width() */
		
		for(unsigned int i = 0; i < PRECOMP_HF_STRING_WIDTH_TO; ++i)
		{
			hf_string_width_precomp[i]
				= dc.GetTextExtent(std::string((i + 1), 'X')).GetWidth();
		}
	}
	
	_handle_width_change();
	
	event.Skip();
}

int REHex::DocumentCtrl::get_bytes_per_line()
{
	return bytes_per_line;
}

void REHex::DocumentCtrl::set_bytes_per_line(int bytes_per_line)
{
	this->bytes_per_line = bytes_per_line;
	_handle_width_change();
}

unsigned int REHex::DocumentCtrl::get_bytes_per_group()
{
	return bytes_per_group;
}

void REHex::DocumentCtrl::set_bytes_per_group(unsigned int bytes_per_group)
{
	this->bytes_per_group = bytes_per_group;
	_handle_width_change();

	wxCommandEvent event(REHex::EV_DISP_SETTING_CHANGED);
	event.SetEventObject(this);

	wxPostEvent(this, event);
}

bool REHex::DocumentCtrl::get_show_offsets()
{
	return offset_column;
}

void REHex::DocumentCtrl::set_show_offsets(bool show_offsets)
{
	offset_column = show_offsets;
	_handle_width_change();
}

REHex::OffsetBase REHex::DocumentCtrl::get_offset_display_base() const
{
	return offset_display_base;
}

void REHex::DocumentCtrl::set_offset_display_base(REHex::OffsetBase offset_display_base)
{
	this->offset_display_base = offset_display_base;
	_handle_width_change();
	
	wxCommandEvent event(REHex::EV_DISP_SETTING_CHANGED);
	event.SetEventObject(this);
	
	wxPostEvent(this, event);
}

bool REHex::DocumentCtrl::get_show_ascii()
{
	return show_ascii;
}

void REHex::DocumentCtrl::set_show_ascii(bool show_ascii)
{
	this->show_ascii = show_ascii;
	_handle_width_change();
}

bool REHex::DocumentCtrl::get_highlight_selection_match()
{
	return highlight_selection_match;
}

void REHex::DocumentCtrl::set_highlight_selection_match(bool highlight_selection_match)
{
	this->highlight_selection_match = highlight_selection_match;
	Refresh();
}

off_t REHex::DocumentCtrl::get_cursor_position() const
{
	return this->cpos_off;
}

REHex::Document::CursorState REHex::DocumentCtrl::get_cursor_state() const
{
	return cursor_state;
}

bool REHex::DocumentCtrl::hex_view_active() const
{
	return cursor_state == Document::CSTATE_HEX || cursor_state == Document::CSTATE_HEX_MID;
}

bool REHex::DocumentCtrl::ascii_view_active() const
{
	return cursor_state == Document::CSTATE_ASCII;
}

bool REHex::DocumentCtrl::special_view_active() const
{
	return cursor_state == Document::CSTATE_SPECIAL;
}

void REHex::DocumentCtrl::set_cursor_position(off_t position, Document::CursorState cursor_state)
{
	/* Clamp the cursor position to the valid ranges defined by the data regions. */
	
	GenericDataRegion *first_dr = data_regions.front();
	GenericDataRegion *last_dr = data_regions.back();
	
	if(_data_region_by_offset(position) == data_regions.end())
	{
		position = first_dr->d_offset;
	}
	
	if(!insert_mode && position > last_dr->d_offset && position == (last_dr->d_offset + last_dr->d_length))
	{
		--position;
	}
	
	if(cursor_state == Document::CSTATE_GOTO)
	{
		if(this->cursor_state == Document::CSTATE_HEX_MID)
		{
			cursor_state = Document::CSTATE_HEX;
		}
		else{
			cursor_state = this->cursor_state;
		}
	}
	
	/* Clamp cursor state to states valid at the new position. */
	
	GenericDataRegion *region = data_region_by_offset(position);
	assert(region != NULL);
	
	GenericDataRegion::ScreenArea valid_areas = region->screen_areas_at_offset(position, this);
	assert((valid_areas & (GenericDataRegion::SA_HEX | GenericDataRegion::SA_ASCII | GenericDataRegion::SA_SPECIAL)) != 0);
	
	if(((cursor_state == Document::CSTATE_HEX || cursor_state == Document::CSTATE_HEX_MID) && (valid_areas & GenericDataRegion::SA_HEX) == 0)
		|| (cursor_state == Document::CSTATE_ASCII && (valid_areas & GenericDataRegion::SA_ASCII) == 0)
		|| (cursor_state == Document::CSTATE_SPECIAL && (valid_areas & GenericDataRegion::SA_SPECIAL) == 0))
	{
		/* Requested cursor state is not valid. Pick something that is. */
		
		if((valid_areas & GenericDataRegion::SA_HEX) != 0)
		{
			cursor_state = Document::CSTATE_HEX;
		}
		else if((valid_areas & GenericDataRegion::SA_ASCII) != 0)
		{
			cursor_state = Document::CSTATE_ASCII;
		}
		else if((valid_areas & GenericDataRegion::SA_SPECIAL) != 0)
		{
			cursor_state = Document::CSTATE_SPECIAL;
		}
	}
	
	/* Blink cursor to visibility and reset timer */
	cursor_visible = true;
	redraw_cursor_timer.Start();
	
	cpos_off = position;
	this->cursor_state = cursor_state;
	
	_make_byte_visible(cpos_off);
	save_scroll_position();
	
	/* TODO: Limit paint to affected area */
	Refresh();
}

void REHex::DocumentCtrl::_set_cursor_position(off_t position, REHex::Document::CursorState cursor_state)
{
	off_t old_cursor_pos                   = get_cursor_position();
	Document::CursorState old_cursor_state = get_cursor_state();
	
	set_cursor_position(position, cursor_state);
	
	off_t new_cursor_pos                   = get_cursor_position();
	Document::CursorState new_cursor_state = get_cursor_state();
	
	if(old_cursor_pos != new_cursor_pos || old_cursor_state != new_cursor_state)
	{
		CursorUpdateEvent cursor_update_event(this, new_cursor_pos, new_cursor_state);
		ProcessWindowEvent(cursor_update_event);
	}
}

bool REHex::DocumentCtrl::get_insert_mode()
{
	return this->insert_mode;
}

void REHex::DocumentCtrl::set_insert_mode(bool enabled)
{
	if(insert_mode == enabled)
	{
		return;
	}
	
	insert_mode = enabled;
	
	off_t cursor_pos = get_cursor_position();
	if(!insert_mode && cursor_pos > 0 && cursor_pos == doc->buffer_length())
	{
		/* Move cursor back if going from insert to overwrite mode and it
		 * was at the end of the file.
		*/
		_set_cursor_position((cursor_pos - 1), Document::CSTATE_GOTO);
	}
	
	wxCommandEvent event(REHex::EV_INSERT_TOGGLED);
	event.SetEventObject(this);
	wxPostEvent(this, event);
	
	/* TODO: Limit paint to affected area */
	this->Refresh();
}

void REHex::DocumentCtrl::linked_scroll_insert_self_after(DocumentCtrl *p)
{
	assert(linked_scroll_prev == NULL);
	assert(linked_scroll_next == NULL);
	
	/* Insert ourself into the linked scroll list after p. */
	
	linked_scroll_prev = p;
	
	if(p->linked_scroll_next != NULL)
	{
		p->linked_scroll_next->linked_scroll_prev = this;
		linked_scroll_next = p->linked_scroll_next;
	}
	
	p->linked_scroll_next = this;
}

void REHex::DocumentCtrl::linked_scroll_remove_self()
{
	assert(linked_scroll_prev != NULL || linked_scroll_next != NULL);
	
	if(linked_scroll_prev != NULL)
	{
		linked_scroll_prev->linked_scroll_next = linked_scroll_next;
	}
	
	if(linked_scroll_next != NULL)
	{
		linked_scroll_next->linked_scroll_prev = linked_scroll_prev;
	}
	
	linked_scroll_prev = NULL;
	linked_scroll_next = NULL;
}

void REHex::DocumentCtrl::linked_scroll_visit_others(const std::function<void(DocumentCtrl*)> &func)
{
	for(DocumentCtrl *p = linked_scroll_prev; p != NULL; p = p->linked_scroll_prev)
	{
		func(p);
	}
	
	for(DocumentCtrl *p = linked_scroll_next; p != NULL; p = p->linked_scroll_next)
	{
		func(p);
	}
}

void REHex::DocumentCtrl::set_selection(off_t off, off_t length)
{
	selection_off    = off;
	selection_length = length;
	
	if(length > 0)
	{
		selection_begin = off;
		selection_end   = off + length - 1;
	}
	else{
		selection_begin = -1;
		selection_end   = -1;
	}
	
	if(length <= 0 || mouse_shift_initial < off || mouse_shift_initial > (off + length))
	{
		mouse_shift_initial = -1;
	}
	
	{
		wxCommandEvent event(REHex::EV_SELECTION_CHANGED);
		event.SetEventObject(this);
		
		wxPostEvent(this, event);
	}
	
	/* TODO: Limit paint to affected area */
	Refresh();
}

void REHex::DocumentCtrl::clear_selection()
{
	set_selection(0, 0);
}

std::pair<off_t, off_t> REHex::DocumentCtrl::get_selection()
{
	return std::make_pair(selection_off, selection_length);
}

std::pair<off_t, off_t> REHex::DocumentCtrl::get_selection_raw()
{
	if(selection_begin < 0)
	{
		/* No selection. */
		return std::make_pair(-1, -1);
	}
	else{
		return std::make_pair(selection_begin, selection_end);
	}
}

REHex::ByteRangeSet REHex::DocumentCtrl::get_selection_ranges()
{
	ByteRangeSet selected_ranges;
	
	auto region = _data_region_by_offset(selection_begin);
	off_t region_select_begin = selection_begin;
	
	while(region != data_regions.end())
	{
		assert(region_select_begin >= (*region)->d_offset);
		assert(region_select_begin <= ((*region)->d_offset + (*region)->d_length));
		
		if((*region)->d_offset <= selection_end && ((*region)->d_length + (*region)->d_offset) >= selection_end)
		{
			if(selection_end > region_select_begin)
			{
				selected_ranges.set_range(region_select_begin, selection_end - region_select_begin);
			}
			
			break;
		}
		else{
			selected_ranges.set_range(region_select_begin, ((*region)->d_offset + (*region)->d_length) - region_select_begin);
		}
		
		++region;
		
		if(region != data_regions.end())
		{
			region_select_begin = (*region)->d_offset;
		}
	}
	
	return selected_ranges;
}

std::pair<off_t, off_t> REHex::DocumentCtrl::get_selection_in_region(GenericDataRegion *region)
{
	if(selection_begin < 0)
	{
		/* No selection. */
		return std::make_pair<off_t, off_t>(-1, -1);
	}
	
	auto region_iter = _data_region_by_offset(region->d_offset);
	assert(region_iter != data_regions.end());
	
	auto sel_begin_iter = _data_region_by_offset(selection_begin);
	assert(sel_begin_iter != data_regions.end());
	
	auto sel_end_iter = _data_region_by_offset(selection_end);
	assert(sel_end_iter != data_regions.end());
	
	if(sel_begin_iter > region_iter || sel_end_iter < region_iter)
	{
		/* Selection doesn't overlap region. */
		return std::make_pair<off_t, off_t>(-1, -1);
	}
	
	off_t region_selection_offset = (sel_begin_iter < region_iter)
		? region->d_offset
		: selection_begin;
	
	off_t region_selection_length = (sel_end_iter > region_iter)
		? (region->d_length - (region_selection_offset - region->d_offset))
		: ((selection_end - region_selection_offset) + 1);
	
	return std::make_pair(region_selection_offset, region_selection_length);
}

void REHex::DocumentCtrl::OnPaint(wxPaintEvent &event)
{
	wxBufferedPaintDC dc(this);
	
	dc.SetFont(hex_font);
	
	dc.SetBackground(wxBrush((*active_palette)[Palette::PAL_NORMAL_TEXT_BG]));
	dc.Clear();
	
	/* Find the region containing the first visible line. */
	auto base_region = region_by_y_offset(scroll_yoff);
	int64_t yo_end = scroll_yoff + visible_lines + 1;
	
	/* Iterate over the visible regions and draw them. */
	for(auto region = base_region; region != regions.end() && (*region)->y_offset < yo_end; ++region)
	{
		int x_px = 0 - scroll_xoff;
		
		int64_t y_px = (*region)->y_offset;
		assert(y_px >= 0);
		
		y_px -= scroll_yoff;
		y_px *= hf_height;
		
		(*region)->draw(*this, dc, x_px, y_px);
	}
	
	/* Iterate over the visible regions again and give them a chance to do any processing. */
	
	bool width_changed = false;
	bool height_changed = false;
	bool redraw = false;
	
	for(auto region = base_region; region != regions.end() && (*region)->y_offset < yo_end; ++region)
	{
		if(std::find_if(processing_regions.begin(), processing_regions.end(),
			[&](const Region *r) { return r == *region; }) != processing_regions.end())
		{
			/* This region is already in processing_regions - will be checked on next idle. */
			continue;
		}
		
		unsigned int state = (*region)->check();
		
		if(state & Region::PROCESSING)
		{
			processing_regions.push_back(*region);
		}
		
		if(state & Region::WIDTH_CHANGE)  { width_changed = true; }
		if(state & Region::HEIGHT_CHANGE) { height_changed = true; }
		if(state & Region::REDRAW)        { redraw = true; }
	}
	
	if(width_changed || height_changed)
	{
		_handle_width_change();
	}
	else if(redraw)
	{
		Refresh();
	}
}

void REHex::DocumentCtrl::OnErase(wxEraseEvent &event)
{
	// Left blank to disable erase
}

void REHex::DocumentCtrl::OnSize(wxSizeEvent &event)
{
	if(regions.empty())
	{
		/* Great big dirty hack: If regions is empty, we're being invoked within the
		 * Create() method call and we aren't set up properly yet, do nothing.
		*/
		return;
	}
	
	/* Get the size of the area we can draw into */
	
	wxSize client_size    = GetClientSize();
	int new_client_width  = client_size.GetWidth();
	int new_client_height = client_size.GetHeight();
	
	bool width_changed  = (new_client_width  != client_width);
	bool height_changed = (new_client_height != client_height);
	
	client_width  = new_client_width;
	client_height = new_client_height;
	
	/* Clamp to 1 if window is too small to display a single whole line, to avoid edge casey
	 * crashing in the scrolling code.
	*/
	visible_lines = std::max((client_height / hf_height), 1);
	
	if(width_changed)
	{
		_handle_width_change();
	}
	else if(height_changed)
	{
		/* _handle_height_change() is a subset of _handle_width_change() */
		_handle_height_change();
	}
}

void REHex::DocumentCtrl::_handle_width_change()
{
	/* Calculate how much space (if any) to reserve for the offsets to the left. */
	
	if(offset_column)
	{
		/* Offset column width includes the vertical line between it and the hex area, so
		 * size is calculated for n+1 characters.
		*/
		
		if(doc->buffer_length() > 0xFFFFFFFF)
		{
			if(offset_display_base == OFFSET_BASE_HEX)
			{
				offset_column_width = hf_string_width(18);
			}
			else{
				offset_column_width = hf_string_width(20);
			}
		}
		else{
			if(offset_display_base == OFFSET_BASE_HEX)
			{
				offset_column_width = hf_string_width(10);
			}
			else{
				offset_column_width = hf_string_width(11);
			}
		}
	}
	else{
		offset_column_width = 0;
	}
	
	virtual_width = 0;
	
	for(auto r = regions.begin(); r != regions.end(); ++r)
	{
		int r_min_width = (*r)->calc_width(*this);
		if(r_min_width > virtual_width)
		{
			virtual_width = r_min_width;
		}
	}
	
	if(virtual_width < client_width)
	{
		/* Raise virtual_width to client_width, so that things drawn relative to the right
		 * edge of the virtual client area don't end up in the middle.
		*/
		virtual_width = client_width;
	}
	
	/* Recalculate the height and y offset of each region. */
	
	{
		wxClientDC dc(this);
		
		int64_t next_yo = 0;
		
		for(auto i = regions.begin(); i != regions.end(); ++i)
		{
			(*i)->y_offset = next_yo;
			(*i)->calc_height(*this, dc);
			
			next_yo += (*i)->y_lines;
		}
	}

	/* TODO: Preserve/scale the position as the window size changes. */
	SetScrollbar(wxHORIZONTAL, 0, client_width, virtual_width);
	
	/* Update vertical scrollbar, since we just recalculated the height of the document. */
	_update_vscroll();
	
	/* Force a redraw of the whole control since resizing can change pretty much the entire
	 * thing depending on rendering settings.
	*/
	Refresh();
}

void REHex::DocumentCtrl::_handle_height_change()
{
	/* Update vertical scrollbar, since the client area height has changed. */
	_update_vscroll();
	
	/* Force a redraw of the whole control since resizing can change pretty much the entire
	 * thing depending on rendering settings.
	*/
	Refresh();
}

void REHex::DocumentCtrl::_update_vscroll()
{
	static const int MAX_STEPS = 10000;
	
	uint64_t total_lines = regions.back()->y_offset + regions.back()->y_lines;
	
	if(total_lines > visible_lines)
	{
		int64_t new_scroll_yoff_max = total_lines - visible_lines;
		
		restore_scroll_position();
		
		int range, thumb, position;
		
		if(total_lines <= (uint64_t)(MAX_STEPS))
		{
			scroll_ydiv = 1;
			
			range    = total_lines;
			thumb    = visible_lines;
			position = scroll_yoff;
		}
		else{
			scroll_ydiv = total_lines / MAX_STEPS;
			
			range    = MAX_STEPS;
			thumb    = 1;
			position = std::min((int)(scroll_yoff / scroll_ydiv), (range - thumb));
			
			if(position == 0 && scroll_yoff > 0)
			{
				/* Past the first line, but not the first scrollbar division.
				 * Skip to the next so the scrollbar doesn't appear fully scrolled
				 * up when there's a bit to go.
				*/
				position = 1;
			}
			else if(position == (range - thumb) && scroll_yoff < scroll_yoff_max)
			{
				/* Ditto, but for the bottom of the document. */
				--position;
			}
		}
		
		assert(range > 0);
		assert(range <= MAX_STEPS);
		assert(thumb > 0);
		assert(thumb <= range);
		assert(position >= 0);
		assert(position <= (range - thumb));
		
		SetScrollbar(wxVERTICAL, position, thumb, range);
		scroll_yoff_max = new_scroll_yoff_max;
	}
	else{
		/* We don't need a vertical scroll bar, but force one to appear anyway so
		 * the bytes per line can't change within OnSize and get us stuck in a loop.
		*/
		#ifdef _WIN32
		SetScrollbar(wxVERTICAL, 0, 0, -1);
		#else
		/* TODO: Do this in a non-crappy way on non-win32 */
		SetScrollbar(wxVERTICAL, 0, 1, 2);
		#endif
		
		scroll_yoff_max = 0;
	}
}

void REHex::DocumentCtrl::_update_vscroll_pos(bool update_linked_scroll_others)
{
	int range = GetScrollRange(wxVERTICAL);
	int thumb = GetScrollThumb(wxVERTICAL);
	
	if(scroll_yoff == scroll_yoff_max)
	{
		/* Last line, overcome any rounding and set scroll bar to max. */
		SetScrollPos(wxVERTICAL, (range - thumb));
	}
	else{
		int position = std::min((int)(scroll_yoff / scroll_ydiv), (range - thumb));
		if(position == 0 && scroll_yoff > 0)
		{
			/* Past the first line, but not the first scrollbar division.
			 * Skip to the next so the scrollbar doesn't appear fully scrolled
			 * up when there's a bit to go.
			*/
			position = 1;
		}
		else if(position == (range - thumb) && scroll_yoff < scroll_yoff_max)
		{
			/* Ditto, but for the bottom of the document. */
			--position;
		}
		
		assert(position >= 0);
		assert(position <= (range - thumb));
		
		SetScrollPos(wxVERTICAL, position);
	}
	
	if(update_linked_scroll_others)
	{
		linked_scroll_visit_others([this](DocumentCtrl *other)
		{
			other->scroll_yoff = scroll_yoff;
			if(other->scroll_yoff > other->scroll_yoff_max)
			{
				other->scroll_yoff = other->scroll_yoff_max;
			}
			
			other->_update_vscroll_pos(false);
			other->Refresh();
		});
	}
}

REHex::DocumentCtrl::FuzzyScrollPosition REHex::DocumentCtrl::get_scroll_position_fuzzy()
{
	FuzzyScrollPosition fsp;
	
	if(scroll_yoff >= (regions.back()->y_offset + regions.back()->y_lines))
	{
		/* This can happen in obscure cases where the DocumentCtrl is "empty", e.g. the
		 * data backing a DiffWindow range is erased. Avoid an assertion failure within
		 * the region_by_y_offset() call.
		*/
		
		return fsp;
	}
	
	auto base_region = region_by_y_offset(scroll_yoff);
	
	fsp.region_idx       = base_region - regions.begin();
	fsp.region_idx_line  = (*base_region)->y_offset - scroll_yoff;
	fsp.region_idx_valid = true;
	
	/* Figure out where the cursor is in screen space. */
	
	off_t cursor_pos = get_cursor_position();
	
	GenericDataRegion *cursor_dr = data_region_by_offset(cursor_pos);
	assert(cursor_dr != NULL);
	
	Rect cursor_rect = cursor_dr->calc_offset_bounds(cursor_pos, this);
	
	if(cursor_rect.y >= scroll_yoff && cursor_rect.y < (scroll_yoff + visible_lines))
	{
		/* Cursor is on-screen, use it as the scroll position anchor. */
		
		fsp.data_offset       = cursor_pos;
		fsp.data_offset_line  = cursor_rect.y - scroll_yoff;
		fsp.data_offset_valid = true;
	}
	else{
		/* Cursor isn't on-screen, use first visible line of data (if any) as the scroll
		 * position anchor.
		*/
		
		for(auto r = base_region; r != regions.end() && (*r)->y_offset < (scroll_yoff + visible_lines); ++r)
		{
			GenericDataRegion *dr = dynamic_cast<GenericDataRegion*>(*r);
			if(dr == NULL)
			{
				continue;
			}
			
			if(dr->y_offset >= scroll_yoff)
			{
				fsp.data_offset       = dr->nth_row_nearest_column(0, 0);
				fsp.data_offset_line  = dr->y_offset - scroll_yoff;
				fsp.data_offset_valid = true;
			}
			else{
				fsp.data_offset       = dr->nth_row_nearest_column((scroll_yoff - dr->y_offset), 0);
				fsp.data_offset_line  = 0;
				fsp.data_offset_valid = true;
			}
			
			break;
		}
	}
	
	return fsp;
}

void REHex::DocumentCtrl::set_scroll_position_fuzzy(const FuzzyScrollPosition &fsp)
{
	if(fsp.data_offset_valid)
	{
		auto dr = _data_region_by_offset(fsp.data_offset);
		if(dr != data_regions.end())
		{
			Rect byte_rect = (*dr)->calc_offset_bounds(fsp.data_offset, this);
			set_scroll_yoff_clamped(byte_rect.y - fsp.data_offset_line);
			
			return;
		}
	}
	
	if(fsp.region_idx_valid)
	{
		if(regions.size() > fsp.region_idx)
		{
			Region *r = regions[fsp.region_idx];
			set_scroll_yoff_clamped(r->y_offset - fsp.region_idx_line);
			
			return;
		}
	}
}

void REHex::DocumentCtrl::save_scroll_position()
{
	saved_scroll_position = get_scroll_position_fuzzy();
}

void REHex::DocumentCtrl::restore_scroll_position()
{
	set_scroll_position_fuzzy(saved_scroll_position);
}

void REHex::DocumentCtrl::OnScroll(wxScrollWinEvent &event)
{
	wxEventType type = event.GetEventType();
	int orientation  = event.GetOrientation();
	
	if(orientation == wxVERTICAL)
	{
		if(type == wxEVT_SCROLLWIN_THUMBTRACK || type == wxEVT_SCROLLWIN_THUMBRELEASE)
		{
			int position = event.GetPosition();
			int range = GetScrollRange(wxVERTICAL);
			int thumb = GetScrollThumb(wxVERTICAL);
			
			if(position == (range - thumb))
			{
				/* Dragged to the end of the scroll bar, jump to last line. */
				scroll_yoff = scroll_yoff_max;
			}
			else{
				scroll_yoff = position * scroll_ydiv;
			}
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_TOP)
		{
			scroll_yoff = 0;
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_BOTTOM)
		{
			scroll_yoff = scroll_yoff_max;
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_LINEUP)
		{
			--scroll_yoff;
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_LINEDOWN)
		{
			++scroll_yoff;
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_PAGEUP)
		{
			scroll_yoff -= visible_lines;
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_PAGEDOWN)
		{
			scroll_yoff += visible_lines;
		}
		
		if(scroll_yoff < 0)
		{
			scroll_yoff = 0;
		}
		else if(scroll_yoff > scroll_yoff_max)
		{
			scroll_yoff = scroll_yoff_max;
		}
		
		_update_vscroll_pos();
		Refresh();
		
		save_scroll_position();
	}
	else if(orientation == wxHORIZONTAL)
	{
		if(type == wxEVT_SCROLLWIN_THUMBTRACK || type == wxEVT_SCROLLWIN_THUMBRELEASE)
		{
			scroll_xoff = event.GetPosition();
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_TOP)
		{
			scroll_xoff = 0;
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_BOTTOM)
		{
			scroll_xoff = virtual_width - client_width;
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_LINEUP)
		{
			scroll_xoff -= hf_char_width();
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_LINEDOWN)
		{
			scroll_xoff += hf_char_width();
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_PAGEUP)   {}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_PAGEDOWN) {}
		
		if(scroll_xoff < 0)
		{
			scroll_xoff = 0;
		}
		else if(scroll_xoff > (virtual_width - client_width))
		{
			scroll_xoff = virtual_width - client_width;
		}
		
		SetScrollPos(wxHORIZONTAL, scroll_xoff);
		Refresh();
	}
}

void REHex::DocumentCtrl::OnWheel(wxMouseEvent &event)
{
	wxMouseWheelAxis axis = event.GetWheelAxis();
	int delta             = event.GetWheelDelta();
	int ticks_per_delta   = event.GetLinesPerAction();
	
	if(axis == wxMOUSE_WHEEL_VERTICAL)
	{
		wheel_vert_accum += event.GetWheelRotation();
		
		scroll_yoff -= (wheel_vert_accum / delta) * ticks_per_delta;
		
		wheel_vert_accum = (wheel_vert_accum % delta);
		
		if(scroll_yoff < 0)
		{
			scroll_yoff = 0;
		}
		else if(scroll_yoff > scroll_yoff_max)
		{
			scroll_yoff = scroll_yoff_max;
		}
		
		_update_vscroll_pos();
		Refresh();
		
		save_scroll_position();
	}
	else if(axis == wxMOUSE_WHEEL_HORIZONTAL)
	{
		ticks_per_delta *= hf_char_width();
		
		wheel_horiz_accum += event.GetWheelRotation();
		
		scroll_xoff += (wheel_horiz_accum / delta) * ticks_per_delta;
		
		wheel_horiz_accum = (wheel_horiz_accum % delta);
		
		if(scroll_xoff < 0)
		{
			scroll_xoff = 0;
		}
		else if(scroll_xoff > (virtual_width - client_width))
		{
			scroll_xoff = virtual_width - client_width;
		}
		
		SetScrollPos(wxHORIZONTAL, scroll_xoff);
		Refresh();
	}
}

void REHex::DocumentCtrl::OnChar(wxKeyEvent &event)
{
	int key       = event.GetKeyCode();
	int modifiers = event.GetModifiers();
	
	off_t cursor_pos = get_cursor_position();
	
	if(region_OnChar(event))
	{
		/* Key press handled by cursor region. */
		return;
	}
	
	if(key == WXK_TAB && modifiers == wxMOD_NONE)
	{
		GenericDataRegion *cur_region = data_region_by_offset(cursor_pos);
		assert(cur_region != NULL);
		
		GenericDataRegion::ScreenArea valid_areas = cur_region->screen_areas_at_offset(cursor_pos, this);
		assert((valid_areas & (GenericDataRegion::SA_HEX | GenericDataRegion::SA_ASCII | GenericDataRegion::SA_SPECIAL)) != 0);
		
		switch(cursor_state)
		{
			case Document::CSTATE_HEX:
			case Document::CSTATE_HEX_MID:
				if((valid_areas & GenericDataRegion::SA_SPECIAL) != 0)
				{
					/* Focus "special" view. */
					_set_cursor_position(cursor_pos, Document::CSTATE_SPECIAL);
					break;
				}
				
			case Document::CSTATE_SPECIAL:
				if((valid_areas & GenericDataRegion::SA_ASCII) != 0)
				{
					/* Focus ASCII view. */
					_set_cursor_position(cursor_pos, Document::CSTATE_ASCII);
					break;
				}
				
			default:
				/* Let wxWidgets handle the event and focus the next control. */
				HandleAsNavigationKey(event);
		}
		
		return;
	}
	else if(key == WXK_TAB && modifiers == wxMOD_SHIFT)
	{
		GenericDataRegion *cur_region = data_region_by_offset(cursor_pos);
		assert(cur_region != NULL);
		
		GenericDataRegion::ScreenArea valid_areas = cur_region->screen_areas_at_offset(cursor_pos, this);
		assert((valid_areas & (GenericDataRegion::SA_HEX | GenericDataRegion::SA_ASCII | GenericDataRegion::SA_SPECIAL)) != 0);
		
		switch(cursor_state)
		{
			case Document::CSTATE_ASCII:
				if((valid_areas & GenericDataRegion::SA_SPECIAL) != 0)
				{
					/* Focus "special" view. */
					_set_cursor_position(cursor_pos, Document::CSTATE_SPECIAL);
					break;
				}
				
			case Document::CSTATE_SPECIAL:
				if((valid_areas & GenericDataRegion::SA_HEX) != 0)
				{
					/* Focus hex view. */
					_set_cursor_position(cursor_pos, Document::CSTATE_HEX);
					break;
				}
				
			default:
				/* Let wxWidgets handle the event and focus the previous control. */
				HandleAsNavigationKey(event);
		}
		
		return;
	}
	else if((modifiers == wxMOD_NONE || modifiers == wxMOD_SHIFT || ((modifiers & ~wxMOD_SHIFT) == wxMOD_CONTROL && (key == WXK_HOME || key == WXK_END)))
		&& (key == WXK_LEFT || key == WXK_RIGHT || key == WXK_UP || key == WXK_DOWN || key == WXK_HOME || key == WXK_END || key == WXK_PAGEUP || key == WXK_PAGEDOWN))
	{
		off_t new_cursor_pos = cursor_pos;
		
		bool update_scrollpos = false;
		int64_t new_scroll_yoff;
		
		auto cur_region = _data_region_by_offset(cursor_pos);
		assert(cur_region != data_regions.end());
		
		if(key == WXK_LEFT)
		{
			new_cursor_pos = (*cur_region)->cursor_left_from(new_cursor_pos);
			
			if(new_cursor_pos == GenericDataRegion::CURSOR_PREV_REGION)
			{
				/* Cursor is at the start of this region. Move to the last byte in
				 * the previous region.
				*/
				
				if(cur_region != data_regions.begin())
				{
					auto prev_region = std::prev(cur_region);
					
					new_cursor_pos = (*prev_region)->last_row_nearest_column(INT_MAX);
					assert(new_cursor_pos >= 0);
				}
				else{
					/* No previous region. Nowhere to go. */
					new_cursor_pos = cursor_pos;
				}
			}
			else{
				assert(new_cursor_pos >= 0);
			}
		}
		else if(key == WXK_RIGHT)
		{
			new_cursor_pos = (*cur_region)->cursor_right_from(new_cursor_pos);
			
			if(new_cursor_pos == GenericDataRegion::CURSOR_NEXT_REGION)
			{
				auto next_region = std::next(cur_region);
				if(next_region != data_regions.end())
				{
					new_cursor_pos = (*next_region)->first_row_nearest_column(0);
					assert(new_cursor_pos >= 0);
				}
				else if(get_insert_mode())
				{
					/* Special case: Can move one past the end of the final
					 * data region in insert mode.
					*/
					
					new_cursor_pos = (*cur_region)->d_offset + (*cur_region)->d_length;
				}
				else{
					/* No further region. Nowhere to go. */
					new_cursor_pos = cursor_pos;
				}
			}
			else{
				assert(new_cursor_pos >= 0);
			}
		}
		else if(key == WXK_UP)
		{
			new_cursor_pos = (*cur_region)->cursor_up_from(new_cursor_pos);
			
			if(new_cursor_pos == GenericDataRegion::CURSOR_PREV_REGION)
			{
				int cur_column = (*cur_region)->cursor_column(cursor_pos);
				
				if(cur_region != data_regions.begin())
				{
					auto prev_region = std::prev(cur_region);
					
					new_cursor_pos = (*prev_region)->last_row_nearest_column(cur_column);
					
					assert(new_cursor_pos >= (*prev_region)->d_offset);
					assert(new_cursor_pos <= (*prev_region)->d_offset + (*prev_region)->d_length);
				}
				else{
					/* No previous region. Nowhere to go. */
					new_cursor_pos = cursor_pos;
				}
			}
			else{
				assert(new_cursor_pos >= 0);
			}
		}
		else if(key == WXK_DOWN)
		{
			new_cursor_pos = (*cur_region)->cursor_down_from(new_cursor_pos);
			
			if(new_cursor_pos == GenericDataRegion::CURSOR_NEXT_REGION)
			{
				int cur_column = (*cur_region)->cursor_column(cursor_pos);
				
				auto next_region = std::next(cur_region);
				if(next_region != data_regions.end())
				{
					new_cursor_pos = (*next_region)->first_row_nearest_column(cur_column);
					assert(new_cursor_pos >= 0);
				}
				else if(get_insert_mode())
				{
					/* Special case: Can move one past the end of the final
					 * data region in insert mode.
					*/
					
					new_cursor_pos = (*cur_region)->d_offset + (*cur_region)->d_length;
				}
				else{
					/* No further region. Nowhere to go. */
					new_cursor_pos = cursor_pos;
				}
			}
			else{
				assert(new_cursor_pos >= 0);
			}
		}
		else if(key == WXK_HOME && (modifiers & wxMOD_CONTROL))
		{
			/* Move cursor to first position in first region. */
			
			assert(!data_regions.empty());
			GenericDataRegion *first_dr = data_regions.front();
			
			new_cursor_pos = first_dr->first_row_nearest_column(0);
		}
		else if(key == WXK_HOME)
		{
			/* Move cursor to start of line. */
			new_cursor_pos = (*cur_region)->cursor_home_from(new_cursor_pos);
		}
		else if(key == WXK_END && (modifiers & wxMOD_CONTROL))
		{
			/* Move cursor to last position in last region, or one past the end if we
			 * are in insert mode.
			*/
			
			assert(!data_regions.empty());
			GenericDataRegion *last_dr = data_regions.back();
			
			if(get_insert_mode())
			{
				new_cursor_pos = last_dr->d_offset + last_dr->d_length;
			}
			else{
				new_cursor_pos = last_dr->last_row_nearest_column(INT_MAX);
			}
		}
		else if(key == WXK_END)
		{
			/* Move cursor to end of line. */
			new_cursor_pos = (*cur_region)->cursor_end_from(new_cursor_pos);
			
			/* Special case: If "end" is pressed on the last line of the final data
			 * region when in insert mode, jump past it.
			*/
			if(get_insert_mode() && (*cur_region)->last_row_nearest_column(INT_MAX) == new_cursor_pos)
			{
				auto next_region = std::next(cur_region);
				if(next_region == data_regions.end())
				{
					new_cursor_pos = (*cur_region)->d_offset + (*cur_region)->d_length;
				}
			}
		}
		else if (key == WXK_PAGEUP)
		{
			/* Scroll the screen up one full times its height and reposition the cursor
			 * to the first visible data region line on screen (if there are any).
			*/
			
			new_scroll_yoff = std::max<int64_t>((scroll_yoff - (int64_t)(visible_lines)), 0);
			int cur_column = (*cur_region)->cursor_column(cursor_pos);
			
			auto region = region_by_y_offset(new_scroll_yoff);
			
			while(region != regions.end() && (*region)->y_offset < (new_scroll_yoff + (int64_t)(visible_lines)))
			{
				GenericDataRegion *dr = dynamic_cast<GenericDataRegion*>(*region);
				if(dr != NULL)
				{
					int64_t cursor_to_line_rel = std::max<int64_t>((new_scroll_yoff - dr->y_offset), 0);
					new_cursor_pos = dr->nth_row_nearest_column(cursor_to_line_rel, cur_column);
					
					break;
				}
				
				++region;
			}
			
			update_scrollpos = true;
		}
		else if (key == WXK_PAGEDOWN)
		{
			/* Scroll the screen down one full times its height and reposition the
			 * cursor to the last data region line visible on screen (if any).
			*/
			
			new_scroll_yoff = std::min((scroll_yoff + (int64_t)(visible_lines)), scroll_yoff_max);
			int cur_column = (*cur_region)->cursor_column(cursor_pos);
			
			auto region = region_by_y_offset(new_scroll_yoff);
			
			while(region != regions.end() && (*region)->y_offset < (new_scroll_yoff + (int64_t)(visible_lines)))
			{
				GenericDataRegion *dr = dynamic_cast<GenericDataRegion*>(*region);
				if(dr != NULL)
				{
					int64_t cursor_to_line_abs = std::min(
						(dr->y_offset + dr->y_lines - 1),
						(new_scroll_yoff + (int64_t)(visible_lines) - 1));
					
					new_cursor_pos = dr->nth_row_nearest_column((cursor_to_line_abs - dr->y_offset), cur_column);
				}
				
				++region;
			}
			
			update_scrollpos = true;
		}
		
		_set_cursor_position(new_cursor_pos, Document::CSTATE_GOTO);
		
		if (update_scrollpos)
		{
			scroll_yoff = new_scroll_yoff;
			_update_vscroll_pos();
			save_scroll_position();
			Refresh();
		}

		if(modifiers & wxMOD_SHIFT)
		{
			off_t selection_end = selection_off + selection_length;
			
			if(new_cursor_pos < cursor_pos)
			{
				if(selection_length > 0)
				{
					if(selection_off >= cursor_pos)
					{
						assert(selection_end >= new_cursor_pos);
						set_selection(new_cursor_pos, (selection_end - new_cursor_pos));
					}
					else{
						if(new_cursor_pos < selection_off)
						{
							set_selection(new_cursor_pos, (selection_off - new_cursor_pos));
						}
						else{
							set_selection(selection_off, (new_cursor_pos - selection_off));
						}
					}
				}
				else{
					set_selection(new_cursor_pos, (cursor_pos - new_cursor_pos));
				}
			}
			else if(new_cursor_pos > cursor_pos)
			{
				if(selection_length > 0)
				{
					if(selection_off >= cursor_pos)
					{
						if(new_cursor_pos >= selection_end)
						{
							set_selection(selection_end, (new_cursor_pos - selection_end));
						}
						else{
							set_selection(new_cursor_pos, (selection_end - new_cursor_pos));
						}
					}
					else{
						set_selection(selection_off, (new_cursor_pos - selection_off));
					}
				}
				else{
					set_selection(cursor_pos, (new_cursor_pos - cursor_pos));
				}
			}
		}
		else{
			clear_selection();
		}
		
		return;
	}
	
	/* Unhandled key press - propagate to parent. */
	event.Skip();
}

void REHex::DocumentCtrl::OnLeftDown(wxMouseEvent &event)
{
	wxClientDC dc(this);
	
	int mouse_x = event.GetX();
	int rel_x   = mouse_x + this->scroll_xoff;
	int mouse_y = event.GetY();
	
	/* Find the region containing the first visible line. */
	auto region = region_by_y_offset(scroll_yoff);
	
	/* If we are scrolled past the start of the regiomn, will need to skip some of the first one. */
	int64_t skip_lines_in_region = (this->scroll_yoff - (*region)->y_offset);
	
	int64_t line_off = (mouse_y / hf_height) + skip_lines_in_region;
	
	while(region != regions.end() && line_off >= (*region)->y_lines)
	{
		line_off -= (*region)->y_lines;
		++region;
	}
	
	if(region != regions.end())
	{
		GenericDataRegion *dr = dynamic_cast<GenericDataRegion*>(*region);
		CommentRegion     *cr = dynamic_cast<CommentRegion*>    (*region);
		
		if(dr != NULL)
		{
			off_t clicked_offset;
			GenericDataRegion::ScreenArea clicked_area;
			
			std::tie(clicked_offset, clicked_area) = dr->offset_near_xy(*this, rel_x, line_off, GenericDataRegion::SA_NONE);
			
			if(clicked_offset >= 0)
			{
				assert(clicked_area != GenericDataRegion::SA_NONE);
				
				off_t old_position = (mouse_shift_initial >= 0 ? mouse_shift_initial : get_cursor_position());
				
				switch(clicked_area)
				{
					case GenericDataRegion::SA_HEX:
						_set_cursor_position(clicked_offset, Document::CSTATE_HEX);
						break;
						
					case GenericDataRegion::SA_ASCII:
						_set_cursor_position(clicked_offset, Document::CSTATE_ASCII);
						break;
						
					case GenericDataRegion::SA_SPECIAL:
						_set_cursor_position(clicked_offset, Document::CSTATE_SPECIAL);
						break;
						
					default:
						_set_cursor_position(clicked_offset, Document::CSTATE_GOTO);
						break;
				}
				
				if(event.ShiftDown())
				{
					if(clicked_offset > old_position)
					{
						set_selection(old_position, (clicked_offset - old_position));
					}
					else{
						set_selection(clicked_offset, (old_position - clicked_offset));
					}
					
					mouse_shift_initial  = old_position;
					mouse_down_at_offset = old_position;
					mouse_down_at_x      = rel_x;
					mouse_down_area      = clicked_area;
				}
				else{
					clear_selection();
					
					mouse_down_at_offset = clicked_offset;
					mouse_down_at_x      = rel_x;
					mouse_down_area      = clicked_area;
				}
				
				CaptureMouse();
				mouse_select_timer.Start(MOUSE_SELECT_INTERVAL, wxTIMER_CONTINUOUS);
				
				/* TODO: Limit paint to affected area */
				Refresh();
			}
		}
		else if(cr != NULL)
		{
			/* Mouse was clicked within a Comment region, ensure we are within the border drawn around the
			 * comment text.
			*/
			
			int hf_width = hf_char_width();
			int indent_width = this->indent_width(cr->indent_depth);
			
			if(
				(line_off > 0 || (mouse_y % hf_height) >= (hf_height / 4)) /* Not above top edge. */
				&& (line_off < (cr->y_lines - 1) || (mouse_y % hf_height) <= ((hf_height / 4) * 3)) /* Not below bottom edge. */
				&& rel_x >= (indent_width + (hf_width / 4)) /* Not left of left edge. */
				&& rel_x < ((virtual_width - (hf_width / 4)) - indent_width)) /* Not right of right edge. */
			{
				OffsetLengthEvent event(this, COMMENT_LEFT_CLICK, cr->c_offset, cr->c_length);
				ProcessWindowEvent(event);
			}
		}
	}
	
	/* Document takes focus when clicked. */
	SetFocus();
}

void REHex::DocumentCtrl::OnLeftUp(wxMouseEvent &event)
{
	if(mouse_down_area != GenericDataRegion::SA_NONE)
	{
		mouse_select_timer.Stop();
		ReleaseMouse();
	}
	
	mouse_down_area = GenericDataRegion::SA_NONE;
}

void REHex::DocumentCtrl::OnRightDown(wxMouseEvent &event)
{
	/* If the user right clicks while selecting, and then releases the left button over the
	 * menu, we never receive the EVT_LEFT_UP event. Release the mouse and cancel the selection
	 * now, else we wind up keeping the mouse grabbed and stop it interacting with any other
	 * windows...
	*/
	
	if(mouse_down_area != GenericDataRegion::SA_NONE)
	{
		mouse_select_timer.Stop();
		ReleaseMouse();
		
		mouse_down_area = GenericDataRegion::SA_NONE;
	}
	
	wxClientDC dc(this);
	
	int mouse_x = event.GetX();
	int rel_x   = mouse_x + this->scroll_xoff;
	int mouse_y = event.GetY();
	
	/* Find the region containing the first visible line. */
	auto region = region_by_y_offset(scroll_yoff);
	
	/* If we are scrolled past the start of the regiomn, will need to skip some of the first one. */
	int64_t skip_lines_in_region = (this->scroll_yoff - (*region)->y_offset);
	
	int64_t line_off = (mouse_y / hf_height) + skip_lines_in_region;
	
	while(region != regions.end() && line_off >= (*region)->y_lines)
	{
		line_off -= (*region)->y_lines;
		++region;
	}
	
	if(region != regions.end())
	{
		GenericDataRegion *dr = dynamic_cast<GenericDataRegion*>(*region);
		CommentRegion *cr = dynamic_cast<CommentRegion*>(*region);
		
		if(dr != NULL)
		{
			off_t clicked_offset;
			GenericDataRegion::ScreenArea clicked_area;
			
			std::tie(clicked_offset, clicked_area) = dr->offset_at_xy(*this, rel_x, line_off);
			
			if(clicked_offset >= 0)
			{
				if(clicked_area == GenericDataRegion::SA_HEX)
				{
					_set_cursor_position(clicked_offset, Document::CSTATE_HEX);
				}
				else if(clicked_area == GenericDataRegion::SA_ASCII)
				{
					_set_cursor_position(clicked_offset, Document::CSTATE_ASCII);
				}
				else if(clicked_area == GenericDataRegion::SA_SPECIAL)
				{
					_set_cursor_position(clicked_offset, Document::CSTATE_SPECIAL);
				}
				else{
					_set_cursor_position(clicked_offset, Document::CSTATE_GOTO);
				}
				
				if(clicked_offset < selection_off || clicked_offset >= selection_off + selection_length)
				{
					clear_selection();
				}
				
				/* TODO: Limit paint to affected area */
				Refresh();
			}
			
			wxCommandEvent event(DATA_RIGHT_CLICK, GetId());
			event.SetEventObject(this);
			
			ProcessWindowEvent(event);
		}
		else if(cr != NULL)
		{
			/* Mouse was clicked within a Comment region, ensure we are within the border drawn around the
			 * comment text.
			*/
			
			int hf_width = hf_char_width();
			int indent_width = this->indent_width(cr->indent_depth);
			
			if(
				(line_off > 0 || (mouse_y % hf_height) >= (hf_height / 4)) /* Not above top edge. */
				&& (line_off < (cr->y_lines - 1) || (mouse_y % hf_height) <= ((hf_height / 4) * 3)) /* Not below bottom edge. */
				&& rel_x >= (indent_width + (hf_width / 4)) /* Not left of left edge. */
				&& rel_x < ((virtual_width - (hf_width / 4)) - indent_width)) /* Not right of right edge. */
			{
				OffsetLengthEvent event(this, COMMENT_RIGHT_CLICK, cr->c_offset, cr->c_length);
				ProcessWindowEvent(event);
			}
		}
	}
	
	/* Document takes focus when clicked. */
	SetFocus();
}

void REHex::DocumentCtrl::OnMotion(wxMouseEvent &event)
{
	int mouse_x = event.GetX();
	int mouse_y = event.GetY();
	
	int rel_x = mouse_x + scroll_xoff;
	
	/* Find the region containing the first visible line. */
	auto region = region_by_y_offset(scroll_yoff);
	
	/* If we are scrolled past the start of the regiomn, will need to skip some of the first one. */
	int64_t skip_lines_in_region = (this->scroll_yoff - (*region)->y_offset);
	
	int64_t line_off = (mouse_y / hf_height) + skip_lines_in_region;
	
	while(region != regions.end() && line_off >= (*region)->y_lines)
	{
		line_off -= (*region)->y_lines;
		++region;
	}
	
	wxCursor cursor = wxNullCursor;
	
	if(region != regions.end())
	{
		cursor = (*region)->cursor_for_point(*this, rel_x, line_off, (mouse_y % hf_height));
	}
	
	SetCursor(cursor);
	
	OnMotionTick(event.GetX(), event.GetY());
}

void REHex::DocumentCtrl::OnSelectTick(wxTimerEvent &event)
{
	wxPoint window_pos = GetScreenPosition();
	wxPoint mouse_pos  = wxGetMousePosition();
	
	OnMotionTick((mouse_pos.x - window_pos.x), (mouse_pos.y - window_pos.y));
}

void REHex::DocumentCtrl::OnMotionTick(int mouse_x, int mouse_y)
{
	if(mouse_down_area == GenericDataRegion::SA_NONE)
	{
		return;
	}
	
	wxClientDC dc(this);
	
	int scroll_xoff_max = GetScrollRange(wxHORIZONTAL) - GetScrollThumb(wxHORIZONTAL);
	
	if(mouse_x < 0)
	{
		scroll_xoff -= std::min(abs(mouse_x), scroll_xoff);
		SetScrollPos(wxHORIZONTAL, scroll_xoff);
		
		mouse_x = 0;
	}
	else if(mouse_x >= client_width)
	{
		scroll_xoff += std::min((int)(mouse_x - client_width), (scroll_xoff_max - scroll_xoff));
		SetScrollPos(wxHORIZONTAL, scroll_xoff);
		
		mouse_x = client_width - 1;
	}
	
	if(mouse_y < 0)
	{
		scroll_yoff -= std::min((int64_t)(abs(mouse_y) / hf_height + 1), scroll_yoff);
		_update_vscroll_pos();
		
		mouse_y = 0;
	}
	else if(mouse_y >= client_height)
	{
		scroll_yoff += std::min((int64_t)((mouse_y - client_height) / hf_height + 1), (scroll_yoff_max - scroll_yoff));
		_update_vscroll_pos();
		
		mouse_y = client_height - 1;
	}
	
	save_scroll_position();
	
	int rel_x = mouse_x + scroll_xoff;
	
	/* Find the region containing the first visible line. */
	auto region = region_by_y_offset(scroll_yoff);
	
	/* If we are scrolled past the start of the regiomn, will need to skip some of the first one. */
	int64_t skip_lines_in_region = (this->scroll_yoff - (*region)->y_offset);
	
	int64_t line_off = (mouse_y / hf_height) + skip_lines_in_region;
	
	while(region != regions.end() && line_off >= (*region)->y_lines)
	{
		line_off -= (*region)->y_lines;
		++region;
	}
	
	if(region != regions.end())
	{
		GenericDataRegion *dr = dynamic_cast<GenericDataRegion*>(*region);
		CommentRegion *cr;
		if(dr != NULL)
		{
			off_t select_to_offset = dr->offset_near_xy(*this, rel_x, line_off, mouse_down_area).first;
			
			if(select_to_offset >= 0)
			{
				off_t new_sel_off, new_sel_len;
				
				if(select_to_offset >= mouse_down_at_offset)
				{
					new_sel_off = mouse_down_at_offset;
					new_sel_len = (select_to_offset - mouse_down_at_offset) + 1;
				}
				else{
					new_sel_off = select_to_offset;
					new_sel_len = (mouse_down_at_offset - select_to_offset) + 1;
				}
				
				if(new_sel_len == 1 && abs(rel_x - mouse_down_at_x) < (hf_char_width() / 2))
				{
					clear_selection();
				}
				else{
					set_selection(new_sel_off, new_sel_len);
				}
				
				/* TODO: Limit paint to affected area */
				Refresh();
			}
		}
		else if((cr = dynamic_cast<REHex::DocumentCtrl::CommentRegion*>(*region)) != NULL && cr->c_offset >= 0)
		{
			if(mouse_down_area != GenericDataRegion::SA_NONE)
			{
				off_t select_to_offset = cr->c_offset;
				off_t new_sel_off, new_sel_len;
				
				if(select_to_offset >= mouse_down_at_offset)
				{
					new_sel_off = mouse_down_at_offset;
					new_sel_len = select_to_offset - mouse_down_at_offset;
				}
				else{
					new_sel_off = select_to_offset;
					new_sel_len = (mouse_down_at_offset - select_to_offset) + 1;
				}
				
				if(new_sel_len == 1 && abs(rel_x - mouse_down_at_x) < (hf_char_width() / 2))
				{
					clear_selection();
				}
				else{
					set_selection(new_sel_off, new_sel_len);
				}
				
				/* TODO: Limit paint to affected area */
				Refresh();
			}
		}
	}
}

void REHex::DocumentCtrl::OnRedrawCursor(wxTimerEvent &event)
{
	cursor_visible = !cursor_visible;
	
	/* TODO: Limit paint to cursor area */
	Refresh();
}

void REHex::DocumentCtrl::OnIdle(wxIdleEvent &event)
{
	bool width_changed = false;
	bool height_changed = false;
	bool redraw = false;
	
	for(auto r = processing_regions.begin(); r != processing_regions.end();)
	{
		unsigned int status = (*r)->check();
		
		if(status & Region::WIDTH_CHANGE)
		{
			width_changed = true;
		}
		
		if(status & Region::HEIGHT_CHANGE)
		{
			height_changed = true;
		}
		
		if(status & Region::REDRAW)
		{
			redraw = true;
		}
		
		if(status & Region::PROCESSING)
		{
			++r;
		}
		else{
			r = processing_regions.erase(r);
		}
	}
	
	if(width_changed || height_changed)
	{
		_handle_width_change();
	}
	else if(redraw)
	{
		Refresh();
	}
	
	if(!processing_regions.empty())
	{
		event.RequestMore();
	}
}

std::vector<REHex::DocumentCtrl::GenericDataRegion*>::iterator REHex::DocumentCtrl::_data_region_by_offset(off_t offset)
{
	/* Find region that encompasses the given offset using binary search. */
	
	class StubRegion: public GenericDataRegion
	{
		public:
			StubRegion(off_t offset):
				GenericDataRegion(offset, 0, 0) {}
				
				virtual std::pair<off_t, ScreenArea> offset_at_xy(DocumentCtrl &doc, int mouse_x_px, int64_t mouse_y_lines) override { abort(); }
				virtual std::pair<off_t, ScreenArea> offset_near_xy(DocumentCtrl &doc, int mouse_x_px, int64_t mouse_y_lines, ScreenArea type_hint) override { abort(); }
				virtual off_t cursor_left_from(off_t pos) override { abort(); }
				virtual off_t cursor_right_from(off_t pos) override { abort(); }
				virtual off_t cursor_up_from(off_t pos) override { abort(); }
				virtual off_t cursor_down_from(off_t pos) override { abort(); }
				virtual off_t cursor_home_from(off_t pos) override { abort(); }
				virtual off_t cursor_end_from(off_t pos) override { abort(); }
				virtual int cursor_column(off_t pos) override { abort(); }
				virtual off_t first_row_nearest_column(int column) override { abort(); }
				virtual off_t last_row_nearest_column(int column) override { abort(); }
				virtual off_t nth_row_nearest_column(int64_t row, int column) override { abort(); }
				virtual Rect calc_offset_bounds(off_t offset, DocumentCtrl *doc_ctrl) override { abort(); }
				virtual ScreenArea screen_areas_at_offset(off_t offset, DocumentCtrl *doc_ctrl) override { abort(); }
				
				virtual void calc_height(REHex::DocumentCtrl &doc, wxDC &dc) override { abort(); }
				virtual void draw(REHex::DocumentCtrl &doc, wxDC &dc, int x, int64_t y) override { abort(); }
				virtual wxCursor cursor_for_point(REHex::DocumentCtrl &doc, int x, int64_t y_lines, int y_px) override { abort(); }
	};
	
	const StubRegion d_offset_to_find(offset);
	std::vector<GenericDataRegion*> d_offset_to_find_vec({ (GenericDataRegion*)(&d_offset_to_find) });
	
	auto cmp_by_d_offset = [](std::vector<GenericDataRegion*>::iterator lhs, std::vector<GenericDataRegion*>::iterator rhs)
	{
		return (*lhs)->d_offset < (*rhs)->d_offset;
	};
	
	/* std::upper_bound() will give us the first element whose d_offset is greater than the one
	 * we're looking for...
	*/
	auto region = std::upper_bound(data_regions_sorted.begin(), data_regions_sorted.end(), d_offset_to_find_vec.begin(), cmp_by_d_offset);
	
	if(region == data_regions_sorted.begin())
	{
		/* No region encompassing the requested offset. */
		return data_regions.end();
	}
	
	/* ...so step backwards to get to the correct element. */
	--region;
	
	if((**region)->d_offset <= offset
		/* Requested offset must be within region range to match, or one past the end if
		 * this is the last data region.
		*/
		&& ((**region)->d_offset + (**region)->d_length + (*region == std::prev(data_regions.end())) > offset))
	{
		return *region;
	}
	else{
		return data_regions.end();
	}
}

std::vector<REHex::DocumentCtrl::Region*>::iterator REHex::DocumentCtrl::region_by_y_offset(int64_t y_offset)
{
	/* Find region that encompasses the given line using binary search. */
	
	class StubRegion: public Region
	{
		public:
			StubRegion(int64_t y_offset):
				Region(0, 0)
			{
				this->y_offset = y_offset;
			}
			
			virtual void calc_height(REHex::DocumentCtrl &doc, wxDC &dc) override
			{
				abort();
			}
			
			virtual void draw(REHex::DocumentCtrl &doc, wxDC &dc, int x, int64_t y) override
			{
				abort();
			}
			
			virtual wxCursor cursor_for_point(REHex::DocumentCtrl &doc, int x, int64_t y_lines, int y_px) override
			{
				abort();
			}
	};
	
	const StubRegion y_offset_to_find(y_offset);
	
	auto cmp_by_y_offset = [](const Region *lhs, const Region *rhs)
	{
		return lhs->y_offset < rhs->y_offset;
	};
	
	/* std::upper_bound() will give us the first element whose y_offset is greater than the one
	 * we're looking for...
	*/
	auto region = std::upper_bound(regions.begin(), regions.end(), &y_offset_to_find, cmp_by_y_offset);
	
	/* ...by definition that can't be the first element... */
	assert(region != regions.begin());
	
	/* ...so step backwards to get to the correct element. */
	--region;
	
	assert((*region)->y_offset <= y_offset);
	assert(((*region)->y_offset + (*region)->y_lines) > y_offset);
	
	return region;
}

/* Scroll the Document vertically to make the given line visible.
 * Does nothing if the line is already on-screen.
*/
void REHex::DocumentCtrl::_make_line_visible(int64_t line)
{
	if(scroll_yoff > line)
	{
		/* Need to scroll up, line will be at the top. */
		scroll_yoff = line;
	}
	else if((scroll_yoff + visible_lines) <= line)
	{
		/* Need to scroll down, line will be the last fully-visible one. */
		scroll_yoff = (line - visible_lines) + !!visible_lines;
	}
	else{
		/* Don't need to scroll. */
		return;
	}
	
	assert(scroll_yoff <= line);
	assert((scroll_yoff + visible_lines + !visible_lines) > line);
	
	_update_vscroll_pos();
	Refresh();
}

/* Scroll the Document horizontally to (try to) make the given range of X co-ordinates visible.
 * Does nothing if the range is fully visible.
*/
void REHex::DocumentCtrl::_make_x_visible(int x_px, int width_px)
{
	if(scroll_xoff > x_px)
	{
		/* Scroll to the left */
		scroll_xoff = x_px;
	}
	else if((scroll_xoff + client_width) < (x_px + width_px) && width_px <= client_width)
	{
		/* Scroll to the right. */
		scroll_xoff = x_px - (client_width - width_px);
	}
	else{
		/* Don't need to scroll. */
		return;
	}
	
	assert(scroll_xoff <= x_px);
	assert((scroll_xoff + client_width) >= (x_px + width_px) || width_px > client_width);
	
	SetScrollPos(wxHORIZONTAL, scroll_xoff);
	Refresh();
}

/* Scroll the Document to make the byte at the given offset visible.
 * Does nothing if the byte is already on-screen.
*/
void REHex::DocumentCtrl::_make_byte_visible(off_t offset)
{
	auto dr = _data_region_by_offset(offset);
	assert(dr != data_regions.end());
	
	Rect bounds = (*dr)->calc_offset_bounds(offset, this);
	assert(bounds.h == 1);
	
	_make_line_visible(bounds.y);
	_make_x_visible(bounds.x, bounds.w);
}

std::list<wxString> REHex::DocumentCtrl::format_text(const wxString &text, unsigned int cols, unsigned int from_line, unsigned int max_lines)
{
	assert(cols > 0);
	
	/* TODO: Throw myself into the abyss and support Unicode properly...
	 * (This function assumes one byte is one full-width character on the screen.
	*/
	
	std::list<wxString> lines;
	
	for(size_t at = 0; at < text.size();)
	{
		size_t newline_at = text.find_first_of('\n', at);
		
		if(newline_at != std::string::npos && newline_at <= (at + cols))
		{
			/* There is a newline within one row's worth of text of our current position.
			 * Add all the text up to it and continue from after it.
			*/
			lines.push_back(text.substr(at, newline_at - at));
			at = newline_at + 1;
		}
		else{
			/* The line is too long, just wrap it at whatever character is on the boundary.
			 *
			 * std::string::substr() will clamp the length if it goes beyond the end of
			 * the string.
			*/
			lines.push_back(text.substr(at, cols));
			at += cols;
		}
	}
	
	lines.erase(lines.begin(), std::next(lines.begin(), std::min((size_t)(from_line), lines.size())));
	lines.erase(std::next(lines.begin(), std::min((size_t)(max_lines), lines.size())), lines.end());
	
	return lines;
}

int REHex::DocumentCtrl::indent_width(int depth)
{
	return hf_char_width() * depth;
}

int REHex::DocumentCtrl::get_offset_column_width()
{
	return offset_column_width;
}

bool REHex::DocumentCtrl::get_cursor_visible()
{
	return cursor_visible;
}

/* Calculate the width of a character in hex_font. */
int REHex::DocumentCtrl::hf_char_width()
{
	return hf_string_width(1);
}

int REHex::DocumentCtrl::hf_char_height()
{
	return hf_height;
}

/* Calculate the bounding box for a string which is length characters long when
 * rendered using hex_font. The string should fit within the box.
 *
 * We can't just multiply the width of a single character because certain
 * platforms *cough* *OSX* use subpixel co-ordinates for character spacing.
*/
int REHex::DocumentCtrl::hf_string_width(int length)
{
	if(length == 0)
	{
		return 0;
	}
	
	if(length <= PRECOMP_HF_STRING_WIDTH_TO)
	{
		return hf_string_width_precomp[length - 1];
	}
	
	wxClientDC dc(this);
	dc.SetFont(hex_font);
	
	wxSize te = dc.GetTextExtent(std::string(length, 'X'));
	return te.GetWidth();
}

/* Calculate the character at the pixel offset relative to the start of the string. */
int REHex::DocumentCtrl::hf_char_at_x(int x_px)
{
	for(int i = 0;; ++i)
	{
		int w = hf_string_width(i + 1);
		if(w > x_px)
		{
			return i;
		}
	}
}

const std::vector<REHex::DocumentCtrl::Region*> &REHex::DocumentCtrl::get_regions() const
{
	return regions;
}

void REHex::DocumentCtrl::replace_all_regions(std::vector<Region*> &new_regions)
{
	assert(!new_regions.empty());
	
	/* Erase the old regions and swap the contents of the new list in. */
	
	for(auto r = regions.begin(); r != regions.end(); ++r)
	{
		delete *r;
	}
	
	regions.clear();
	
	regions.swap(new_regions);
	
	/* Initialise the indent_depth and indent_final counters. */
	
	std::list<off_t> indent_to;
	
	for(auto r = regions.begin(), p = r; r != regions.end(); ++r)
	{
		assert((*r)->indent_offset >= (*p)->indent_offset);
		
		while(!indent_to.empty() && indent_to.back() <= (*r)->indent_offset)
		{
			++((*p)->indent_final);
			indent_to.pop_back();
		}
		
		(*r)->indent_depth = indent_to.size();
		(*r)->indent_final = 0;
		
		if((*r)->indent_length > 0)
		{
			if(!indent_to.empty())
			{
				assert(((*r)->indent_offset + (*r)->indent_length) <= indent_to.back());
			}
			
			indent_to.push_back((*r)->indent_offset + (*r)->indent_length);
		}
		
		/* Advance p from second iteration. */
		if(p != r)
		{
			++p;
		}
	}
	
	regions.back()->indent_final = indent_to.size();
	
	/* Clear and repopulate data_regions with the GenericDataRegion regions. */
	
	data_regions.clear();
	
	for(auto r = regions.begin(); r != regions.end(); ++r)
	{
		GenericDataRegion *dr = dynamic_cast<GenericDataRegion*>(*r);
		if(dr != NULL)
		{
			data_regions.push_back(dr);
		}
	}
	
	/* Clear and repopulate data_regions_sorted with iterators to each element in data_regions
	 * sorted by d_offset.
	*/
	
	data_regions_sorted.clear();
	
	for(auto r = data_regions.begin(); r != data_regions.end(); ++r)
	{
		data_regions_sorted.push_back(r);
	}
	
	std::sort(data_regions_sorted.begin(), data_regions_sorted.end(),
		[](const std::vector<GenericDataRegion*>::iterator &lhs, const std::vector<GenericDataRegion*>::iterator &rhs)
		{
			return (*lhs)->d_offset < (*rhs)->d_offset;
		});
	
	/* Clear and repopulate processing_regions with the regions which have some background work to do. */
	
	processing_regions.clear();
	
	for(auto r = regions.begin(); r != regions.end(); ++r)
	{
		unsigned int status = (*r)->check();
		
		if(status & Region::PROCESSING)
		{
			processing_regions.push_back(*r);
		}
	}
	
	/* Recalculates region widths/heights and updates scroll bars */
	_handle_width_change();
	
	/* Update the cursor position/state if not valid within the new regions. */
	_set_cursor_position(get_cursor_position(), get_cursor_state());
}

bool REHex::DocumentCtrl::region_OnChar(wxKeyEvent &event)
{
	off_t cursor_pos = get_cursor_position();
	
	auto cur_region = _data_region_by_offset(cursor_pos);
	assert(cur_region != data_regions.end());
	
	return (*cur_region)->OnChar(this, event);
}

REHex::DocumentCtrl::GenericDataRegion *REHex::DocumentCtrl::data_region_by_offset(off_t offset)
{
	auto region = _data_region_by_offset(offset);
	return region != data_regions.end() ? *region : NULL;
}

wxFont &REHex::DocumentCtrl::get_font()
{
	return hex_font;
}

int64_t REHex::DocumentCtrl::get_scroll_yoff() const
{
	return scroll_yoff;
}

void REHex::DocumentCtrl::set_scroll_yoff(int64_t scroll_yoff)
{
	set_scroll_yoff_clamped(scroll_yoff);
	
	_update_vscroll_pos();
	save_scroll_position();
	Refresh();
}

void REHex::DocumentCtrl::set_scroll_yoff_clamped(int64_t scroll_yoff)
{
	if(scroll_yoff < 0)
	{
		scroll_yoff = 0;
	}
	else if(scroll_yoff > scroll_yoff_max)
	{
		scroll_yoff = scroll_yoff_max;
	}
	
	this->scroll_yoff = scroll_yoff;
}

REHex::DocumentCtrl::Region::Region(off_t indent_offset, off_t indent_length):
	indent_depth(0),
	indent_final(0),
	indent_offset(indent_offset),
	indent_length(indent_length)  {}

REHex::DocumentCtrl::Region::~Region() {}

unsigned int REHex::DocumentCtrl::Region::check()
{
	return StateFlag::IDLE;
}

int REHex::DocumentCtrl::Region::calc_width(REHex::DocumentCtrl &doc)
{
	return 0;
}

wxCursor REHex::DocumentCtrl::Region::cursor_for_point(REHex::DocumentCtrl &doc, int x, int64_t y_lines, int y_px)
{
	return wxNullCursor;
}

void REHex::DocumentCtrl::Region::draw_container(REHex::DocumentCtrl &doc, wxDC &dc, int x, int64_t y)
{
	if(indent_depth > 0)
	{
		int cw = doc.hf_char_width();
		int ch = doc.hf_height;
		
		int64_t skip_lines = (y < 0 ? (-y / ch) : 0);
		
		int     box_y  = y + (skip_lines * (int64_t)(ch));
		int64_t box_h  = (y_lines - skip_lines) * (int64_t)(ch);
		int     box_hc = std::min(box_h, (int64_t)(doc.client_height));
		
		int box_x = x + (cw / 4);
		int box_w = doc.virtual_width - (cw / 2);
		
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(wxBrush((*active_palette)[Palette::PAL_NORMAL_TEXT_BG]));
		
		dc.DrawRectangle(0, box_y, doc.client_width, box_hc);
		
		dc.SetPen(wxPen((*active_palette)[Palette::PAL_NORMAL_TEXT_FG]));
		
		for(int i = 0; i < indent_depth; ++i)
		{
			if(box_h <= (int64_t)(doc.client_height) && (i + indent_final) == indent_depth)
			{
				box_h  -= ch / 2;
				box_hc -= ch / 2;
			}
			
			dc.DrawLine(box_x, box_y, box_x, (box_y + box_hc));
			dc.DrawLine((box_x + box_w - 1), box_y, (box_x + box_w - 1), (box_y + box_hc));
			
			if(box_h <= (int64_t)(doc.client_height) && (i + indent_final) >= indent_depth)
			{
				dc.DrawLine(box_x, (box_y + box_h), (box_x + box_w - 1), (box_y + box_h));
				
				box_h  -= ch;
				box_hc -= ch;
			}
			
			box_x += cw;
			box_w -= cw * 2;
		}
	}
}

void REHex::DocumentCtrl::Region::draw_full_height_line(DocumentCtrl *doc_ctrl, wxDC &dc, int x, int64_t y)
{
	int cw = doc_ctrl->hf_char_width();
	int ch = doc_ctrl->hf_height;
	
	int64_t skip_lines = (y < 0 ? (-y / ch) : 0);
	
	int     box_y  = y + (skip_lines * (int64_t)(ch));
	int64_t box_h  = (y_lines - skip_lines) * (int64_t)(ch);
	int     box_hc = std::min(box_h, (int64_t)(doc_ctrl->client_height));
	
	dc.SetPen(wxPen((*active_palette)[Palette::PAL_NORMAL_TEXT_FG]));
	
	dc.DrawLine(x, box_y, x, (box_y + box_hc));
}

REHex::DocumentCtrl::GenericDataRegion::GenericDataRegion(off_t d_offset, off_t d_length, off_t indent_offset):
	Region(indent_offset, 0),
	d_offset(d_offset),
	d_length(d_length)
{
	assert(d_offset >= 0);
	assert(d_length >= 0);
}

bool REHex::DocumentCtrl::GenericDataRegion::OnChar(DocumentCtrl *doc_ctrl, wxKeyEvent &event)
{
	return false;
}

wxDataObject *REHex::DocumentCtrl::GenericDataRegion::OnCopy(DocumentCtrl &doc_ctrl)
{
	return NULL;
}

bool REHex::DocumentCtrl::GenericDataRegion::OnPaste(DocumentCtrl *doc_ctrl)
{
	return false;
}

REHex::DocumentCtrl::DataRegion::DataRegion(off_t d_offset, off_t d_length, off_t virt_offset):
	GenericDataRegion(d_offset, d_length, virt_offset),
	virt_offset(virt_offset),
	bytes_per_line_actual(1) {}

int REHex::DocumentCtrl::DataRegion::calc_width(REHex::DocumentCtrl &doc)
{
	/* Decide how many bytes to display per line */
	
	if(doc.bytes_per_line == BYTES_PER_LINE_FIT_BYTES)
	{
		/* TODO: Can I do this algorithmically? */
		
		bytes_per_line_actual = 1;
		
		while(calc_width_for_bytes(doc, bytes_per_line_actual + 1) <= doc.client_width)
		{
			++bytes_per_line_actual;
		}
		
		first_line_pad_bytes = 0;
	}
	else if(doc.bytes_per_line == BYTES_PER_LINE_FIT_GROUPS)
	{
		bytes_per_line_actual = doc.bytes_per_group;
		
		while(calc_width_for_bytes(doc, bytes_per_line_actual + doc.bytes_per_group) <= doc.client_width)
		{
			bytes_per_line_actual += doc.bytes_per_group;
		}
		
		first_line_pad_bytes = 0;
	}
	else{
		bytes_per_line_actual = doc.bytes_per_line;
		
		first_line_pad_bytes = d_offset % bytes_per_line_actual;
	}
	
	return calc_width_for_bytes(doc, bytes_per_line_actual);
}

int REHex::DocumentCtrl::DataRegion::calc_width_for_bytes(DocumentCtrl &doc_ctrl, unsigned int line_bytes) const
{
	return doc_ctrl.offset_column_width
		/* indentation */
		+ (doc_ctrl.indent_width(indent_depth) * 2)
		
		/* hex data */
		+ doc_ctrl.hf_string_width(line_bytes * 2)
		+ doc_ctrl.hf_string_width((line_bytes - 1) / doc_ctrl.bytes_per_group)
		
		/* ASCII data */
		+ (doc_ctrl.show_ascii * doc_ctrl.hf_char_width())
		+ (doc_ctrl.show_ascii * doc_ctrl.hf_string_width(line_bytes));
}

void REHex::DocumentCtrl::DataRegion::calc_height(REHex::DocumentCtrl &doc, wxDC &dc)
{
	int indent_width = doc.indent_width(indent_depth);
	
	offset_text_x = indent_width;
	hex_text_x    = indent_width + doc.offset_column_width;
	ascii_text_x  = (doc.virtual_width - indent_width) - doc.hf_string_width(bytes_per_line_actual);
	
	/* If we are rendering the first line of the region, then we offset it to (mostly)
	 * preserve column alignment between regions.
	*/
	
	off_t effective_length = d_length + first_line_pad_bytes;
	
	/* Height of the region is simply the number of complete lines of data plus an incomplete
	 * one if the data isn't a round number of lines.
	*/
	y_lines = (effective_length / bytes_per_line_actual) + !!(effective_length % bytes_per_line_actual) + indent_final;
	
	if((d_offset + d_length) == doc.doc->buffer_length() && (effective_length % bytes_per_line_actual) == 0)
	{
		/* This is the last data region in the document. Make it one row taller if the last
		 * row is full so there is always somewhere to draw the insert cursor.
		*/
		++y_lines;
	}
}

void REHex::DocumentCtrl::DataRegion::draw(REHex::DocumentCtrl &doc, wxDC &dc, int x, int64_t y)
{
	draw_container(doc, dc, x, y);
	
	/* If we are scrolled part-way into a data region, don't render data above the client area
	 * as it would get expensive very quickly with large files.
	*/
	int64_t skip_lines = (y < 0 ? (-y / doc.hf_height) : 0);
	off_t skip_bytes  = skip_lines * bytes_per_line_actual;
	
	wxPen norm_fg_1px((*active_palette)[Palette::PAL_NORMAL_TEXT_FG], 1);
	
	bool alternate_row = ((y_offset + skip_lines) % 2) != 0;
	
	auto normal_text_colour = [&dc,&alternate_row]()
	{
		dc.SetTextForeground((*active_palette)[alternate_row ? Palette::PAL_ALTERNATE_TEXT_FG : Palette::PAL_NORMAL_TEXT_FG ]);
		dc.SetBackgroundMode(wxTRANSPARENT);
	};
	
	if(skip_bytes > 0)
	{
		assert(skip_bytes > first_line_pad_bytes);
		skip_bytes -= first_line_pad_bytes;
	}
	
	if(skip_lines >= (y_lines - indent_final))
	{
		/* All of our data is past the top of the client area, all that needed to be
		 * rendered is the bottom of the container around it.
		*/
		return;
	}
	
	/* Increment y up to our real drawing start point. We can now trust it to be within a
	 * hf_height of zero, not the stratospheric integer-overflow-causing values it could
	 * previously have on huge files.
	*/
	y += skip_lines * doc.hf_height;
	
	/* The maximum amount of data that can be drawn on the screen before we're past the bottom
	 * of the client area. Drawing more than this would be pointless and very expensive in the
	 * case of large files.
	*/
	int max_lines = ((doc.client_height - y) / doc.hf_height) + 1;
	off_t max_bytes = (off_t)(max_lines) * (off_t)(bytes_per_line_actual);
	
	if((int64_t)(max_lines) > (y_lines - indent_final - skip_lines))
	{
		max_lines = (y_lines - indent_final - skip_lines);
	}
	
	if(doc.offset_column)
	{
		int offset_vl_x = (x + offset_text_x + doc.offset_column_width) - (doc.hf_char_width() / 2);
		
		dc.SetPen(norm_fg_1px);
		dc.DrawLine(offset_vl_x, y, offset_vl_x, y + (max_lines * doc.hf_height));
	}
	
	if(doc.show_ascii)
	{
		int ascii_vl_x = (x + ascii_text_x) - (doc.hf_char_width() / 2);
		
		dc.SetPen(norm_fg_1px);
		dc.DrawLine(ascii_vl_x, y, ascii_vl_x, y + (max_lines * doc.hf_height));
	}
	
	static const int SECONDARY_SELECTION_MAX = 4096;
	
	std::vector<unsigned char> selection_data;
	if(doc.get_highlight_selection_match() && doc.selection_length > 0 && doc.selection_length <= SECONDARY_SELECTION_MAX)
	{
		try {
			selection_data = doc.doc->read_data(doc.selection_off, doc.selection_length);
		}
		catch(const std::exception &e)
		{
			fprintf(stderr, "Exception in REHex::Document::Region::Data::draw: %s\n", e.what());
		}
	}
	
	/* Fetch the data to be drawn. */
	std::vector<unsigned char> data;
	bool data_err = false;
	
	ByteRangeSet ranges_matching_selection;
	
	const unsigned char *data_p = NULL;
	size_t data_remain;
	
	try {
		off_t hsm_pre  = std::min<off_t>(d_offset + skip_bytes, selection_data.size());
		off_t hsm_post = selection_data.size();
		
		off_t data_base = d_offset + skip_bytes - hsm_pre;
		
		data = doc.doc->read_data(data_base, std::min(max_bytes, (d_length - std::min(skip_bytes, d_length))) + hsm_pre + hsm_post);
		
		data_p = data.data() + hsm_pre;
		data_remain = (data.size() - hsm_pre) - hsm_post;
		
		if(!selection_data.empty())
		{
			for(size_t i = 0; (i + selection_data.size()) <= data.size();)
			{
				if(memcmp((data.data() + i), selection_data.data(), selection_data.size()) == 0)
				{
					ranges_matching_selection.set_range(data_base + i, selection_data.size());
					i += selection_data.size();
				}
				else{
					++i;
				}
			}
		}
	}
	catch(const std::exception &e)
	{
		fprintf(stderr, "Exception in REHex::DocumentCtrl::DataRegion::draw: %s\n", e.what());
		
		data.insert(data.end(), std::min(max_bytes, (d_length - std::min(skip_bytes, d_length))), '?');
		data_err = true;
		data_p = NULL;
	}
	
	/* The offset of the character in the Buffer currently being drawn. */
	off_t cur_off = d_offset + skip_bytes;
	
	wxSize client_size = doc.GetClientSize();
	
	auto highlight_func = [&](off_t offset)
	{
		if(ranges_matching_selection.isset(offset))
		{
			return Highlight(Palette::PAL_SECONDARY_SELECTED_TEXT_FG, Palette::PAL_SECONDARY_SELECTED_TEXT_BG, true);
		}
		else{
			return highlight_at_off(offset);
		}
	};
	
	auto hex_highlight_func = [&](off_t offset)
	{
		if(doc.selection_length > 0 && offset >= doc.selection_off && offset < (doc.selection_off + doc.selection_length))
		{
			bool hex_active = doc.hex_view_active();
			return Highlight(Palette::PAL_SELECTED_TEXT_FG, Palette::PAL_SELECTED_TEXT_BG, hex_active);
		}
		else{
			return highlight_func(offset);
		}
	};
	
	auto ascii_highlight_func = [&](off_t offset)
	{
		if(doc.selection_length > 0 && offset >= doc.selection_off && offset < (doc.selection_off + doc.selection_length))
		{
			bool ascii_active = doc.ascii_view_active();
			return Highlight(Palette::PAL_SELECTED_TEXT_FG, Palette::PAL_SELECTED_TEXT_BG, ascii_active);
		}
		else{
			return highlight_func(offset);
		}
	};
	
	int64_t cur_line = y_offset + skip_lines;
	
	while(y < client_size.GetHeight() && cur_line < (y_offset + y_lines - indent_final))
	{
		if(doc.offset_column)
		{
			/* Draw the offsets to the left */
			
			off_t offset_within_region = cur_off - d_offset;
			off_t display_offset = virt_offset + offset_within_region;
			
			std::string offset_str = format_offset(display_offset, doc.offset_display_base, doc.doc->buffer_length());
			
			normal_text_colour();
			dc.DrawText(offset_str.c_str(), (x + offset_text_x), y);
		}
		
		/* If we are rendering the first line of the region, then we offset it to (mostly)
		 * preserve column alignment between regions.
		*/
		
		unsigned int line_pad_bytes = (cur_off == d_offset)
			? first_line_pad_bytes
			: 0;
		
		const unsigned char *line_data = data_err ? NULL : data_p;
		size_t line_data_len = std::min<size_t>(data_remain, (bytes_per_line_actual - line_pad_bytes));
		
		draw_hex_line(&doc, dc, x + hex_text_x, y, line_data, line_data_len, line_pad_bytes, cur_off, alternate_row, hex_highlight_func);
		
		if(doc.show_ascii)
		{
			draw_ascii_line(&doc, dc, x + ascii_text_x, y, line_data, line_data_len, line_pad_bytes, cur_off, alternate_row, ascii_highlight_func);
		}
		
		cur_off += line_data_len;
		
		data_p += line_data_len;
		data_remain -= line_data_len;
		
		y += doc.hf_height;
		++cur_line;
		
		alternate_row = !alternate_row;
	}
}

void REHex::DocumentCtrl::Region::draw_hex_line(DocumentCtrl *doc_ctrl, wxDC &dc, int x, int y, const unsigned char *data, size_t data_len, unsigned int pad_bytes, off_t base_off, bool alternate_row, const std::function<Highlight(off_t)> &highlight_at_off)
{
	int hex_base_x = x;                                                          /* Base X co-ordinate to draw hex characters from */
	int hex_x_char = (pad_bytes * 2) + (pad_bytes / doc_ctrl->bytes_per_group);  /* Column of current hex character */
	int hex_x      = hex_base_x + doc_ctrl->hf_string_width(hex_x_char);         /* X co-ordinate of current hex character */
	
	off_t cur_off = base_off;
	
	dc.SetFont(doc_ctrl->hex_font);
	
	wxPen norm_fg_1px((*active_palette)[Palette::PAL_NORMAL_TEXT_FG], 1);
	wxPen selected_bg_1px((*active_palette)[Palette::PAL_SELECTED_TEXT_BG], 1);
	dc.SetBrush(*wxTRANSPARENT_BRUSH);
	
	bool hex_active = doc_ctrl->HasFocus() && doc_ctrl->hex_view_active();
	
	off_t cursor_pos = doc_ctrl->get_cursor_position();
	
	auto normal_text_colour = [&dc,&alternate_row]()
	{
		dc.SetTextForeground((*active_palette)[alternate_row ? Palette::PAL_ALTERNATE_TEXT_FG : Palette::PAL_NORMAL_TEXT_FG ]);
		dc.SetBackgroundMode(wxTRANSPARENT);
	};
	
	auto draw_end_cursor = [&]()
	{
		if((doc_ctrl->cursor_visible && doc_ctrl->hex_view_active()) || !hex_active)
		{
			if(doc_ctrl->insert_mode || !hex_active)
			{
				dc.SetPen(norm_fg_1px);
				dc.DrawLine(hex_x, y, hex_x, y + doc_ctrl->hf_height);
			}
			else{
				/* Draw the cursor in red if trying to overwrite at an invalid
					* position. Should only happen in empty files.
				*/
				dc.SetPen(*wxRED_PEN);
				dc.DrawLine(hex_x, y, hex_x, y + doc_ctrl->hf_height);
			}
		}
	};
	
	if(data_len == 0)
	{
		if(cur_off == cursor_pos)
		{
			draw_end_cursor();
		}
		
		return;
	}
	
	/* Calling wxDC::DrawText() for each individual character on the screen is
	 * painfully slow, so we batch up the wxDC::DrawText() calls for each colour and
	 * area on a per-line basis.
	 *
	 * The key of the deferred_drawtext map is the X co-ordinate to render the string
	 * at (hex_base_x or ascii_base_x) and the foreground colour to use.
	 *
	 * The draw_char_deferred() function adds a character to be drawn to the map, while
	 * prefixing it with any spaces necessary to pad it to the correct column from the
	 * base X co-ordinate.
	*/
	
	std::map<std::pair<int, Palette::ColourIndex>, std::string> deferred_drawtext;
	
	auto draw_char_deferred = [&](int base_x, Palette::ColourIndex colour_idx, int col, char ch)
	{
		std::pair<int, Palette::ColourIndex> k(base_x, colour_idx);
		std::string &str = deferred_drawtext[k];
		
		assert(str.length() <= (size_t)(col));
		
		str.append((col - str.length()), ' ');
		str.append(1, ch);
	};
	
	/* Because we need to minimise wxDC::DrawText() calls (see above), we draw any
	 * background colours ourselves and set the background mode to transparent when
	 * drawing text, which enables us to skip over characters that shouldn't be
	 * touched by that particular wxDC::DrawText() call by inserting spaces.
	*/
	
	auto fill_char_bg = [&](int char_x, Palette::ColourIndex colour_idx, bool strong)
	{
		/* Abandoned dithering experiment. */
		#if 0
		wxBitmap bitmap(2, 2);
		
		{
			wxMemoryDC imagememDC;
			imagememDC.SelectObject(bitmap);
			
			wxBrush bg_brush((*active_palette)[Palette::PAL_NORMAL_TEXT_BG]);
			wxBrush fg_brush((*active_palette)[colour_idx]);
			
			imagememDC.SetBackground(bg_brush);
			imagememDC.Clear();
			
			imagememDC.SetBrush(fg_brush);
			imagememDC.SetPen(*wxTRANSPARENT_PEN);
			
			if(strong)
			{
				imagememDC.DrawRectangle(wxRect(0, 0, 2, 2));
			}
			else{
				imagememDC.DrawRectangle(wxRect(0, 0, 1, 1));
				imagememDC.DrawRectangle(wxRect(1, 1, 1, 1));
			}
		}
		
		wxBrush bg_brush(bitmap);
		#endif
		
		wxColour bg_colour = strong
			? (*active_palette)[colour_idx]
			: active_palette->get_average_colour(colour_idx, Palette::PAL_NORMAL_TEXT_BG);
		
		wxBrush bg_brush(bg_colour);
		
		dc.SetBrush(bg_brush);
		dc.SetPen(*wxTRANSPARENT_PEN);
		
		dc.DrawRectangle(char_x, y, doc_ctrl->hf_char_width(), doc_ctrl->hf_height);
	};
	
	for(size_t c = pad_bytes, i = 0; i < data_len; ++c, ++i)
	{
		if(c > pad_bytes && (c % doc_ctrl->bytes_per_group) == 0)
		{
			hex_x = hex_base_x + doc_ctrl->hf_string_width(++hex_x_char);
		}
		
		unsigned char byte        = (data != NULL) ? data[i] : '?';
		unsigned char high_nibble = (byte & 0xF0) >> 4;
		unsigned char low_nibble  = (byte & 0x0F);
		
		auto highlight = highlight_at_off(cur_off);
		
		auto draw_nibble = [&](unsigned char nibble, bool invert)
		{
			const char *nibble_to_hex = (data != NULL)
				? "0123456789ABCDEF"
				: "????????????????";
			
			if(invert && doc_ctrl->cursor_visible)
			{
				fill_char_bg(hex_x, Palette::PAL_INVERT_TEXT_BG, true);
				draw_char_deferred(hex_base_x, Palette::PAL_INVERT_TEXT_FG, hex_x_char, nibble_to_hex[nibble]);
			}
			else if(highlight.enable)
			{
				fill_char_bg(hex_x, highlight.bg_colour_idx, highlight.strong);
				draw_char_deferred(hex_base_x, highlight.fg_colour_idx, hex_x_char, nibble_to_hex[nibble]);
			}
			else{
				draw_char_deferred(hex_base_x, alternate_row ? Palette::PAL_ALTERNATE_TEXT_FG : Palette::PAL_NORMAL_TEXT_FG, hex_x_char, nibble_to_hex[nibble]);
			}
			
			hex_x = hex_base_x + doc_ctrl->hf_string_width(++hex_x_char);
		};
		
		bool inv_high, inv_low;
		if(cur_off == cursor_pos && hex_active)
		{
			if(doc_ctrl->cursor_state == Document::CSTATE_HEX)
			{
				inv_high = !doc_ctrl->insert_mode;
				inv_low  = !doc_ctrl->insert_mode;
			}
			else /* if(doc_ctrl->cursor_state == Document::CSTATE_HEX_MID) */
			{
				inv_high = false;
				inv_low  = true;
			}
		}
		else{
			inv_high = false;
			inv_low  = false;
		}
		
		/* Need the current hex_x value for drawing any boxes or insert cursors
		 * below, before it gets updated by draw_nibble().
		*/
		const int pd_hx = hex_x;
		
		draw_nibble(high_nibble, inv_high);
		draw_nibble(low_nibble,  inv_low);
		
		if(cur_off >= doc_ctrl->selection_off && cur_off < (doc_ctrl->selection_off + doc_ctrl->selection_length) && !hex_active)
		{
			#if 0
			dc.SetPen(selected_bg_1px);
			
			if(cur_off == doc_ctrl->selection_off || c == 0)
			{
				/* Draw vertical line left of selection. */
				dc.DrawLine(pd_hx, y, pd_hx, (y + doc_ctrl->hf_height));
			}
			
			if(cur_off == (doc_ctrl->selection_off + doc_ctrl->selection_length - 1) || i == (data_len - 1))
			{
				/* Draw vertical line right of selection. */
				dc.DrawLine((pd_hx + doc_ctrl->hf_string_width(2) - 1), y, (pd_hx + doc_ctrl->hf_string_width(2) - 1), (y + doc_ctrl->hf_height));
			}
			
			if(cur_off < (doc_ctrl->selection_off + bytes_per_line_actual))
			{
				/* Draw horizontal line above selection. */
				dc.DrawLine(pd_hx, y, (pd_hx + doc_ctrl->hf_string_width(2)), y);
			}
			
			if(cur_off > doc_ctrl->selection_off && cur_off <= (doc_ctrl->selection_off + bytes_per_line_actual) && c > 0 && (c % doc_ctrl->bytes_per_group) == 0)
			{
				/* Draw horizontal line above gap along top of selection. */
				dc.DrawLine((pd_hx - doc_ctrl->hf_char_width()), y, pd_hx, y);
			}
			
			if(cur_off >= (doc_ctrl->selection_off + doc_ctrl->selection_length - bytes_per_line_actual))
			{
				/* Draw horizontal line below selection. */
				dc.DrawLine(pd_hx, (y + doc_ctrl->hf_height - 1), (pd_hx + doc_ctrl->hf_string_width(2)), (y + doc_ctrl->hf_height - 1));
				
				if(c > 0 && (c % doc_ctrl->bytes_per_group) == 0 && cur_off > doc_ctrl->selection_off)
				{
					/* Draw horizontal line below gap along bottom of selection. */
					dc.DrawLine((pd_hx - doc_ctrl->hf_char_width()), (y + doc_ctrl->hf_height - 1), pd_hx, (y + doc_ctrl->hf_height - 1));
				}
			}
			#endif
		}
		
		if(cur_off == cursor_pos && doc_ctrl->insert_mode && ((doc_ctrl->cursor_visible && doc_ctrl->cursor_state == Document::CSTATE_HEX) || !hex_active))
		{
			/* Draw insert cursor. */
			dc.SetPen(norm_fg_1px);
			dc.DrawLine(pd_hx, y, pd_hx, y + doc_ctrl->hf_height);
		}
		
		if(cur_off == cursor_pos && !doc_ctrl->insert_mode && !hex_active)
		{
			/* Draw inactive overwrite cursor. */
			dc.SetBrush(*wxTRANSPARENT_BRUSH);
			dc.SetPen(norm_fg_1px);
			
			if(doc_ctrl->cursor_state == Document::CSTATE_HEX_MID)
			{
				dc.DrawRectangle(pd_hx + doc_ctrl->hf_char_width(), y, doc_ctrl->hf_char_width(), doc_ctrl->hf_height);
			}
			else{
				dc.DrawRectangle(pd_hx, y, doc_ctrl->hf_string_width(2), doc_ctrl->hf_height);
			}
		}
		
		++cur_off;
	}
	
	normal_text_colour();
	
	for(auto dd = deferred_drawtext.begin(); dd != deferred_drawtext.end(); ++dd)
	{
		dc.SetTextForeground((*active_palette)[dd->first.second]);
		dc.SetBackgroundMode(wxTRANSPARENT);
		
		dc.DrawText(dd->second, dd->first.first, y);
	}
}

void REHex::DocumentCtrl::Region::draw_ascii_line(DocumentCtrl *doc_ctrl, wxDC &dc, int x, int y, const unsigned char *data, size_t data_len, unsigned int pad_bytes, off_t base_off, bool alternate_row, const std::function<Highlight(off_t)> &highlight_at_off)
{
	int ascii_base_x = x;                                                       /* Base X co-ordinate to draw ASCII characters from */
	int ascii_x_char = pad_bytes;                                               /* Column of current ASCII character */
	int ascii_x      = ascii_base_x + doc_ctrl->hf_string_width(ascii_x_char);  /* X co-ordinate of current ASCII character */
	
	dc.SetFont(doc_ctrl->hex_font);
	
	wxPen norm_fg_1px((*active_palette)[Palette::PAL_NORMAL_TEXT_FG], 1);
	wxPen selected_bg_1px((*active_palette)[Palette::PAL_SELECTED_TEXT_BG], 1);
	dc.SetBrush(*wxTRANSPARENT_BRUSH);
	
	off_t cur_off = base_off;
	
	bool ascii_active = doc_ctrl->HasFocus() && doc_ctrl->ascii_view_active();
	
	off_t cursor_pos = doc_ctrl->get_cursor_position();
	
	auto normal_text_colour = [&dc,&alternate_row]()
	{
		dc.SetTextForeground((*active_palette)[alternate_row ? Palette::PAL_ALTERNATE_TEXT_FG : Palette::PAL_NORMAL_TEXT_FG ]);
		dc.SetBackgroundMode(wxTRANSPARENT);
	};
	
	auto draw_end_cursor = [&]()
	{
		if((doc_ctrl->cursor_visible && doc_ctrl->ascii_view_active()) || !ascii_active)
		{
			if(doc_ctrl->insert_mode || !ascii_active)
			{
				dc.SetPen(norm_fg_1px);
				dc.DrawLine(ascii_x, y, ascii_x, y + doc_ctrl->hf_height);
			}
			else{
				/* Draw the cursor in red if trying to overwrite at an invalid
				 * position. Should only happen in empty files.
				*/
				dc.SetPen(*wxRED_PEN);
				dc.DrawLine(ascii_x, y, ascii_x, y + doc_ctrl->hf_height);
			}
		}
	};
	
	if(data_len == 0)
	{
		if(cur_off == cursor_pos)
		{
			draw_end_cursor();
		}
		
		return;
	}
	
	/* Calling wxDC::DrawText() for each individual character on the screen is
	 * painfully slow, so we batch up the wxDC::DrawText() calls for each colour and
	 * area on a per-line basis.
	 *
	 * The key of the deferred_drawtext map is the X co-ordinate to render the string
	 * at (hex_base_x or ascii_base_x) and the foreground colour to use.
	 *
	 * The draw_char_deferred() function adds a character to be drawn to the map, while
	 * prefixing it with any spaces necessary to pad it to the correct column from the
	 * base X co-ordinate.
	*/
	
	std::map<std::pair<int, Palette::ColourIndex>, std::string> deferred_drawtext;
	
	auto draw_char_deferred = [&](int base_x, Palette::ColourIndex colour_idx, int col, char ch)
	{
		std::pair<int, Palette::ColourIndex> k(base_x, colour_idx);
		std::string &str = deferred_drawtext[k];
		
		assert(str.length() <= (size_t)(col));
		
		str.append((col - str.length()), ' ');
		str.append(1, ch);
	};
	
	/* Because we need to minimise wxDC::DrawText() calls (see above), we draw any
	 * background colours ourselves and set the background mode to transparent when
	 * drawing text, which enables us to skip over characters that shouldn't be
	 * touched by that particular wxDC::DrawText() call by inserting spaces.
	*/
	
	auto fill_char_bg = [&](int char_x, Palette::ColourIndex colour_idx, bool strong)
	{
		wxColour bg_colour = strong
			? (*active_palette)[colour_idx]
			: active_palette->get_average_colour(colour_idx, Palette::PAL_NORMAL_TEXT_BG);
		
		wxBrush bg_brush(bg_colour);
		
		dc.SetBrush(bg_brush);
		dc.SetPen(*wxTRANSPARENT_PEN);
		
		dc.DrawRectangle(char_x, y, doc_ctrl->hf_char_width(), doc_ctrl->hf_height);
	};
	
	for(size_t c = pad_bytes, i = 0; i < data_len; ++c, ++i)
	{
		unsigned char byte = (data != NULL) ? data[i] : '?';
		
		auto highlight = highlight_at_off(cur_off);
		
		char ascii_byte = isasciiprint(byte)
			? byte
			: '.';
		
		if(ascii_active)
		{
			if(cur_off == cursor_pos && !doc_ctrl->insert_mode && doc_ctrl->cursor_visible)
			{
				fill_char_bg(ascii_x, Palette::PAL_INVERT_TEXT_BG, true);
				draw_char_deferred(ascii_base_x, Palette::PAL_INVERT_TEXT_FG, ascii_x_char, ascii_byte);
			}
			else if(highlight.enable)
			{
				fill_char_bg(ascii_x, highlight.bg_colour_idx, highlight.strong);
				draw_char_deferred(ascii_base_x, highlight.fg_colour_idx, ascii_x_char, ascii_byte);
			}
			else{
				draw_char_deferred(ascii_base_x, alternate_row ? Palette::PAL_ALTERNATE_TEXT_FG : Palette::PAL_NORMAL_TEXT_FG, ascii_x_char, ascii_byte);
			}
		}
		else{
			if(highlight.enable)
			{
				fill_char_bg(ascii_x, highlight.bg_colour_idx, highlight.strong);
				draw_char_deferred(ascii_base_x, highlight.fg_colour_idx, ascii_x_char, ascii_byte);
			}
			else{
				draw_char_deferred(ascii_base_x, alternate_row ? Palette::PAL_ALTERNATE_TEXT_FG : Palette::PAL_NORMAL_TEXT_FG, ascii_x_char, ascii_byte);
			}
			
			if(cur_off == cursor_pos && !doc_ctrl->insert_mode)
			{
				dc.SetBrush(*wxTRANSPARENT_BRUSH);
				dc.SetPen(norm_fg_1px);
				
				dc.DrawRectangle(ascii_x, y, doc_ctrl->hf_char_width(), doc_ctrl->hf_height);
			}
			else if(cur_off >= doc_ctrl->selection_off && cur_off < (doc_ctrl->selection_off + doc_ctrl->selection_length))
			{
				#if 0
				dc.SetPen(selected_bg_1px);
				
				if(cur_off == doc_ctrl->selection_off || c == 0)
				{
					/* Draw vertical line left of selection. */
					dc.DrawLine(ascii_x, y, ascii_x, (y + doc_ctrl->hf_height));
				}
				
				if(cur_off == (doc_ctrl->selection_off + doc_ctrl->selection_length - 1) || c == (bytes_per_line_actual - 1))
				{
					/* Draw vertical line right of selection. */
					dc.DrawLine((ascii_x + doc_ctrl->hf_char_width() - 1), y, (ascii_x + doc_ctrl->hf_char_width() - 1), (y + doc_ctrl->hf_height));
				}
				
				if(cur_off < (doc_ctrl->selection_off + bytes_per_line_actual))
				{
					/* Draw horizontal line above selection. */
					dc.DrawLine(ascii_x, y, (ascii_x + doc_ctrl->hf_char_width()), y);
				}
				
				if(cur_off >= (doc_ctrl->selection_off + doc_ctrl->selection_length - bytes_per_line_actual))
				{
					/* Draw horizontal line below selection. */
					dc.DrawLine(ascii_x, (y + doc_ctrl->hf_height - 1), (ascii_x + doc_ctrl->hf_char_width()), (y + doc_ctrl->hf_height - 1));
				}
				#endif
			}
		}
		
		if(cur_off == cursor_pos && doc_ctrl->insert_mode && (doc_ctrl->cursor_visible || !ascii_active))
		{
			dc.SetPen(norm_fg_1px);
			dc.DrawLine(ascii_x, y, ascii_x, y + doc_ctrl->hf_height);
		}
		
		ascii_x = ascii_base_x + doc_ctrl->hf_string_width(++ascii_x_char);
		
		++cur_off;
	}
	
	normal_text_colour();
	
	for(auto dd = deferred_drawtext.begin(); dd != deferred_drawtext.end(); ++dd)
	{
		dc.SetTextForeground((*active_palette)[dd->first.second]);
		dc.SetBackgroundMode(wxTRANSPARENT);
		
		dc.DrawText(dd->second, dd->first.first, y);
	}
}

wxCursor REHex::DocumentCtrl::DataRegion::cursor_for_point(REHex::DocumentCtrl &doc, int x, int64_t y_lines, int y_px)
{
	if(x >= hex_text_x)
	{
		return wxCursor(wxCURSOR_IBEAM);
	}
	else{
		return wxNullCursor;
	}
}

off_t REHex::DocumentCtrl::DataRegion::offset_at_xy_hex(REHex::DocumentCtrl &doc, int mouse_x_px, uint64_t mouse_y_lines)
{
	if(mouse_x_px < hex_text_x)
	{
		return -1;
	}
	
	mouse_x_px -= hex_text_x;
	
	/* Calculate the offset within the Buffer of the first byte on this line
	 * and the offset (plus one) of the last byte on this line.
	*/
	off_t line_data_begin = (d_offset - first_line_pad_bytes) + ((off_t)(bytes_per_line_actual) * mouse_y_lines);
	off_t line_data_end   = std::min((line_data_begin + bytes_per_line_actual), (d_offset + d_length));
	
	unsigned int char_offset = doc.hf_char_at_x(mouse_x_px);
	if(((char_offset + 1) % ((doc.bytes_per_group * 2) + 1)) == 0)
	{
		/* Click was over a space between byte groups. */
		return -1;
	}
	else{
		unsigned int char_offset_sub_spaces = char_offset - (char_offset / ((doc.bytes_per_group * 2) + 1));
		unsigned int line_offset_bytes      = char_offset_sub_spaces / 2;
		off_t clicked_offset                = line_data_begin + line_offset_bytes;
		
		if(clicked_offset < d_offset)
		{
			/* Clicked in padding on first line. */
			return -1;
		}
		else if(clicked_offset < line_data_end)
		{
			/* Clicked on a byte */
			return clicked_offset;
		}
		else{
			/* Clicked past the end of the line */
			return -1;
		}
	}
}

int REHex::DocumentCtrl::Region::offset_at_x_hex(DocumentCtrl *doc_ctrl, int rel_x)
{
	if(rel_x < 0)
	{
		return -1;
	}
	
	unsigned int bytes_per_group = doc_ctrl->get_bytes_per_group();
	
	unsigned int char_offset = doc_ctrl->hf_char_at_x(rel_x);
	if(((char_offset + 1) % ((bytes_per_group * 2) + 1)) == 0)
	{
		/* Click was over a space between byte groups. */
		return -1;
	}
	else{
		unsigned int char_offset_sub_spaces = char_offset - (char_offset / ((bytes_per_group * 2) + 1));
		int line_offset_bytes = char_offset_sub_spaces / 2;
		
		return line_offset_bytes;
	}
}

off_t REHex::DocumentCtrl::DataRegion::offset_at_xy_ascii(REHex::DocumentCtrl &doc, int mouse_x_px, uint64_t mouse_y_lines)
{
	if(!doc.show_ascii || mouse_x_px < ascii_text_x)
	{
		return -1;
	}
	
	mouse_x_px -= ascii_text_x;
	
	/* Calculate the offset within the Buffer of the first byte on this line
	 * and the offset (plus one) of the last byte on this line.
	*/
	off_t line_data_begin = (d_offset - first_line_pad_bytes) + ((off_t)(bytes_per_line_actual) * mouse_y_lines);
	off_t line_data_end   = std::min((line_data_begin + bytes_per_line_actual), (d_offset + d_length));
	
	unsigned int char_offset = doc.hf_char_at_x(mouse_x_px);
	off_t clicked_offset     = line_data_begin + char_offset;
	
	if(clicked_offset < d_offset)
	{
		/* Clicked in padding on first line. */
		return -1;
	}
	else if(clicked_offset < line_data_end)
	{
		/* Clicked on a character */
		return clicked_offset;
	}
	else{
		/* Clicked past the end of the line */
		return -1;
	}
}

off_t REHex::DocumentCtrl::DataRegion::offset_near_xy_hex(REHex::DocumentCtrl &doc, int mouse_x_px, uint64_t mouse_y_lines)
{
	/* Calculate the offset within the Buffer of the first byte on this line
	 * and the offset (plus one) of the last byte on this line.
	*/
	off_t line_data_begin = (d_offset - first_line_pad_bytes) + ((off_t)(bytes_per_line_actual) * mouse_y_lines);
	off_t line_data_end   = std::min((line_data_begin + bytes_per_line_actual), (d_offset + d_length));
	
	if(mouse_x_px < hex_text_x)
	{
		/* Mouse is in offset area, return offset of last byte of previous line. */
		return line_data_begin - 1;
	}
	
	mouse_x_px -= hex_text_x;
	
	unsigned int char_offset = doc.hf_char_at_x(mouse_x_px);
	
	unsigned int char_offset_sub_spaces = char_offset - (char_offset / ((doc.bytes_per_group * 2) + 1));
	unsigned int line_offset_bytes      = char_offset_sub_spaces / 2;
	off_t clicked_offset                = line_data_begin + line_offset_bytes;
	
	if(clicked_offset < d_offset)
	{
		/* Mouse is in padding area on first line, return offset of last byte of previous line. */
		return d_offset - 1;
	}
	else if(clicked_offset < line_data_end)
	{
		/* Mouse is on a byte. */
		return clicked_offset;
	}
	else{
		/* Mouse is past end of line, return last byte of this line. */
		return line_data_end - 1;
	}
}

int REHex::DocumentCtrl::Region::offset_near_x_hex(DocumentCtrl *doc_ctrl, int rel_x)
{
	if(rel_x < 0)
	{
		return -1;
	}
	
	unsigned int bytes_per_group = doc_ctrl->get_bytes_per_group();
	
	unsigned int char_offset = doc_ctrl->hf_char_at_x(rel_x);
	
	unsigned int char_offset_sub_spaces = char_offset - (char_offset / ((bytes_per_group * 2) + 1));
	int line_offset_bytes = char_offset_sub_spaces / 2;
	
	return line_offset_bytes;
}

off_t REHex::DocumentCtrl::DataRegion::offset_near_xy_ascii(REHex::DocumentCtrl &doc, int mouse_x_px, uint64_t mouse_y_lines)
{
	/* Calculate the offset within the Buffer of the first byte on this line
	 * and the offset (plus one) of the last byte on this line.
	*/
	off_t line_data_begin = (d_offset - first_line_pad_bytes) + ((off_t)(bytes_per_line_actual) * mouse_y_lines);
	off_t line_data_end   = std::min((line_data_begin + bytes_per_line_actual), (d_offset + d_length));
	
	if(!doc.show_ascii || mouse_x_px < ascii_text_x)
	{
		/* Mouse is left of ASCII area, return last byte of previous line. */
		return line_data_begin - 1;
	}
	
	mouse_x_px -= ascii_text_x;
	
	unsigned int char_offset = doc.hf_char_at_x(mouse_x_px);
	off_t clicked_offset     = line_data_begin + char_offset;
	
	if(clicked_offset < d_offset)
	{
		/* Mouse is in padding area on first line, return offset of last byte of previous line. */
		return d_offset - 1;
	}
	else if(clicked_offset < line_data_end)
	{
		/* Mouse is on a character. */
		return clicked_offset;
	}
	else{
		/* Mouse is beyond end of line, return last byte of this line. */
		return line_data_end - 1;
	}
}

std::pair<off_t, REHex::DocumentCtrl::GenericDataRegion::ScreenArea> REHex::DocumentCtrl::DataRegion::offset_at_xy(DocumentCtrl &doc, int mouse_x_px, int64_t mouse_y_lines)
{
	if(doc.show_ascii && mouse_x_px >= ascii_text_x)
	{
		off_t off = offset_at_xy_ascii(doc, mouse_x_px, mouse_y_lines);
		return std::make_pair(off, (off >= 0 ? SA_ASCII : SA_NONE));
	}
	else if(mouse_x_px >= hex_text_x)
	{
		off_t off = offset_at_xy_hex(doc, mouse_x_px, mouse_y_lines);
		return std::make_pair(off, (off >= 0 ? SA_HEX : SA_NONE));
	}
	else{
		return std::make_pair(-1, SA_NONE);
	}
}

std::pair<off_t, REHex::DocumentCtrl::GenericDataRegion::ScreenArea> REHex::DocumentCtrl::DataRegion::offset_near_xy(DocumentCtrl &doc, int mouse_x_px, int64_t mouse_y_lines, ScreenArea type_hint)
{
	if(type_hint == SA_ASCII)
	{
		if(doc.show_ascii)
		{
			off_t off = offset_near_xy_ascii(doc, mouse_x_px, mouse_y_lines);
			return std::make_pair(off, (off >= 0 ? SA_ASCII : SA_NONE));
		}
		else{
			return std::make_pair(-1, SA_NONE);
		}
	}
	else if(type_hint == SA_HEX)
	{
		off_t off = offset_near_xy_hex(doc, mouse_x_px, mouse_y_lines);
		return std::make_pair(off, (off >= 0 ? SA_HEX : SA_NONE));
	}
	
	if(doc.show_ascii && mouse_x_px >= ascii_text_x)
	{
		off_t off = offset_near_xy_ascii(doc, mouse_x_px, mouse_y_lines);
		return std::make_pair(off, (off >= 0 ? SA_ASCII : SA_NONE));
	}
	else if(mouse_x_px >= hex_text_x)
	{
		off_t off = offset_near_xy_hex(doc, mouse_x_px, mouse_y_lines);
		return std::make_pair(off, (off >= 0 ? SA_HEX : SA_NONE));
	}
	else{
		return std::make_pair(-1, SA_NONE);
	}
}

off_t REHex::DocumentCtrl::DataRegion::cursor_left_from(off_t pos)
{
	assert(pos >= d_offset);
	assert(pos <= (d_offset + d_length));
	
	off_t new_pos = pos - 1;
	
	if(new_pos >= d_offset && new_pos < (d_offset + d_length))
	{
		return new_pos;
	}
	else{
		return CURSOR_PREV_REGION;
	}
}

off_t REHex::DocumentCtrl::DataRegion::cursor_right_from(off_t pos)
{
	assert(pos >= d_offset);
	assert(pos <= (d_offset + d_length));
	
	off_t new_pos = pos + 1;
	
	if(new_pos >= d_offset && new_pos < (d_offset + d_length))
	{
		return new_pos;
	}
	else{
		return CURSOR_NEXT_REGION;
	}
}

off_t REHex::DocumentCtrl::DataRegion::cursor_up_from(off_t pos)
{
	assert(pos >= d_offset);
	assert(pos <= (d_offset + d_length));
	
	off_t new_pos = pos - bytes_per_line_actual;
	
	if(new_pos < d_offset && new_pos >= (d_offset - (off_t)(first_line_pad_bytes)))
	{
		/* Moving from second line to first line, but first line is padded past this column. */
		new_pos = d_offset;
	}
	
	if(new_pos >= d_offset && new_pos < (d_offset + d_length))
	{
		return new_pos;
	}
	else{
		return CURSOR_PREV_REGION;
	}
}

off_t REHex::DocumentCtrl::DataRegion::cursor_down_from(off_t pos)
{
	assert(pos >= d_offset);
	assert(pos <= (d_offset + d_length));
	
	off_t new_pos = pos + bytes_per_line_actual;
	
	off_t visual_offset = d_offset - (off_t)(first_line_pad_bytes);
	off_t visual_length = d_length + (off_t)(first_line_pad_bytes);
	
	off_t last_row_off = visual_offset + (((visual_length - 1) / bytes_per_line_actual) * bytes_per_line_actual);
	
	if(pos < last_row_off && new_pos >= (d_offset + d_length))
	{
		/* There is a line below the current line, but it isn't as long as this one, so
		 * jump to the end of it.
		*/
		return d_offset + d_length - 1;
	}
	
	if(new_pos >= d_offset && new_pos < (d_offset + d_length))
	{
		return new_pos;
	}
	else{
		return CURSOR_NEXT_REGION;
	}
}

off_t REHex::DocumentCtrl::DataRegion::cursor_home_from(off_t pos)
{
	assert(pos >= d_offset);
	assert(pos <= (d_offset + d_length));
	
	off_t visual_offset = d_offset - (off_t)(first_line_pad_bytes);
	off_t bytes_from_start_of_visual_line = (pos - visual_offset) % bytes_per_line_actual;
	
	off_t new_pos = std::max(
		(pos - bytes_from_start_of_visual_line),
		d_offset);
	
	return new_pos;
}

off_t REHex::DocumentCtrl::DataRegion::cursor_end_from(off_t pos)
{
	assert(pos >= d_offset);
	assert(pos <= (d_offset + d_length));
	
	off_t visual_offset = d_offset - (off_t)(first_line_pad_bytes);
	off_t bytes_from_start_of_visual_line = (pos - visual_offset) % bytes_per_line_actual;
	
	if(bytes_from_start_of_visual_line == (bytes_per_line_actual - 1))
	{
		/* Already at the end of the line. */
		return pos;
	}
	
	off_t new_pos = std::min(
		(pos + (bytes_per_line_actual - bytes_from_start_of_visual_line) - 1),
		(d_offset + d_length - 1));
	
	return new_pos;
}

int REHex::DocumentCtrl::DataRegion::cursor_column(off_t pos)
{
	assert(pos >= d_offset);
	assert(pos <= (d_offset + d_length));
	
	off_t visual_offset = d_offset - (off_t)(first_line_pad_bytes);
	off_t region_offset = pos - visual_offset;
	
	int column = region_offset % bytes_per_line_actual;
	
	return column;
}

off_t REHex::DocumentCtrl::DataRegion::first_row_nearest_column(int column)
{
	off_t visual_offset = d_offset - (off_t)(first_line_pad_bytes);
	
	off_t offset_at_col = visual_offset + column;
	
	offset_at_col = std::max(offset_at_col, d_offset);
	offset_at_col = std::min(offset_at_col, (visual_offset + (off_t)(bytes_per_line_actual) - 1));
	offset_at_col = std::min(offset_at_col, (d_offset + d_length - (d_length > 0)));
	
	assert(offset_at_col >= d_offset);
	assert(offset_at_col < (d_offset + d_length + (d_length == 0)));
	
	return offset_at_col;
}

off_t REHex::DocumentCtrl::DataRegion::last_row_nearest_column(int column)
{
	off_t visual_offset = d_offset - (off_t)(first_line_pad_bytes);
	off_t visual_length = d_length + (off_t)(first_line_pad_bytes);
	
	off_t last_row_off = visual_offset + (((visual_length - 1) / bytes_per_line_actual) * bytes_per_line_actual);
	
	off_t offset_at_col = last_row_off + column;
	
	offset_at_col = std::max(offset_at_col, d_offset);
	offset_at_col = std::max(offset_at_col, last_row_off);
	offset_at_col = std::min(offset_at_col, (d_offset + d_length - (d_length > 0)));
	
	assert(offset_at_col >= d_offset);
	assert(offset_at_col < (d_offset + d_length + (d_length == 0)));
	
	return offset_at_col;
}

off_t REHex::DocumentCtrl::DataRegion::nth_row_nearest_column(int64_t row, int column)
{
	assert(row >= 0);
	assert(row < y_lines);
	
	off_t visual_offset = d_offset - (off_t)(first_line_pad_bytes);
	
	off_t offset_at_col = (visual_offset + (off_t)(column) + ((off_t)(row) * (off_t)(bytes_per_line_actual)));
	
	/* Clamp to data range. */
	offset_at_col = std::max(offset_at_col, d_offset);
	offset_at_col = std::min(offset_at_col, (d_offset + d_length - (d_length > 0)));
	
	return offset_at_col;
}

REHex::DocumentCtrl::Rect REHex::DocumentCtrl::DataRegion::calc_offset_bounds(off_t offset, DocumentCtrl *doc_ctrl)
{
	assert(offset >= d_offset);
	assert(offset <= (d_offset + d_length));
	
	off_t region_offset = offset - (d_offset - first_line_pad_bytes);
	
	uint64_t region_line = y_offset + (region_offset / bytes_per_line_actual);
	
	off_t line_off = region_offset % bytes_per_line_actual;
	
	Document::CursorState cursor_state = doc_ctrl->get_cursor_state();
	
	if(cursor_state == Document::CSTATE_ASCII)
	{
		int byte_x = ascii_text_x + doc_ctrl->hf_string_width(line_off);
		
		return Rect(
			byte_x,                     /* x */
			region_line,                /* y */
			doc_ctrl->hf_char_width(),  /* w */
			1);                         /* h */
	}
	else{
		unsigned int bytes_per_group = doc_ctrl->get_bytes_per_group();
		int line_x = hex_text_x + doc_ctrl->hf_string_width((line_off * 2) + (line_off / bytes_per_group));
		
		return Rect(
			line_x,                        /* x */
			region_line,                   /* y */
			doc_ctrl->hf_string_width(2),  /* w */
			1);                            /* h */
	}
}

REHex::DocumentCtrl::GenericDataRegion::ScreenArea REHex::DocumentCtrl::DataRegion::screen_areas_at_offset(off_t offset, DocumentCtrl *doc_ctrl)
{
	assert(offset >= d_offset);
	assert(offset <= (d_offset + d_length));
	
	if(doc_ctrl->get_show_ascii())
	{
		return (ScreenArea)(SA_HEX | SA_ASCII);
	}
	else{
		return SA_HEX;
	}
}

REHex::DocumentCtrl::DataRegion::Highlight REHex::DocumentCtrl::DataRegion::highlight_at_off(off_t off) const
{
	return NoHighlight();
}

REHex::DocumentCtrl::DataRegionDocHighlight::DataRegionDocHighlight(off_t d_offset, off_t d_length, off_t virt_offset, Document &doc):
	DataRegion(d_offset, d_length, virt_offset), doc(doc) {}

REHex::DocumentCtrl::DataRegion::Highlight REHex::DocumentCtrl::DataRegionDocHighlight::highlight_at_off(off_t off) const
{
	const NestedOffsetLengthMap<int> &highlights = doc.get_highlights();
	
	auto highlight = NestedOffsetLengthMap_get(highlights, off);
	if(highlight != highlights.end())
	{
		return Highlight(
			active_palette->get_highlight_fg_idx(highlight->second),
			active_palette->get_highlight_bg_idx(highlight->second),
			true);
	}
	else if(doc.is_byte_dirty(off))
	{
		return Highlight(
			Palette::PAL_DIRTY_TEXT_FG,
			Palette::PAL_DIRTY_TEXT_BG,
			true);
	}
	else{
		return NoHighlight();
	}
}

REHex::DocumentCtrl::CommentRegion::CommentRegion(off_t c_offset, off_t c_length, const wxString &c_text, bool truncate, off_t indent_offset, off_t indent_length):
	Region(indent_offset, indent_length),
	c_offset(c_offset),
	c_length(c_length),
	c_text(c_text),
	truncate(truncate) {}

void REHex::DocumentCtrl::CommentRegion::calc_height(REHex::DocumentCtrl &doc, wxDC &dc)
{
	if(truncate)
	{
		y_lines = 2 + indent_final;
		return;
	}
	
	unsigned int row_chars = doc.hf_char_at_x(doc.virtual_width - (2 * doc.indent_width(indent_depth))) - 1;
	if(row_chars == 0)
	{
		/* Zero columns of width. Probably still initialising. */
		this->y_lines = 1 + indent_final;
	}
	else{
		auto comment_lines = format_text(c_text, row_chars);
		this->y_lines  = comment_lines.size() + 1 + indent_final;
	}
}

void REHex::DocumentCtrl::CommentRegion::draw(REHex::DocumentCtrl &doc, wxDC &dc, int x, int64_t y)
{
	draw_container(doc, dc, x, y);
	
	int indent_width = doc.indent_width(indent_depth);
	x += indent_width;
	
	dc.SetFont(doc.hex_font);
	
	unsigned int row_chars = doc.hf_char_at_x(doc.virtual_width - (2 * indent_width)) - 1;
	if(row_chars == 0)
	{
		/* Zero columns of width. Probably still initialising. */
		return;
	}
	
	auto lines = format_text(c_text, row_chars);
	
	if(truncate && lines.size() > 1)
	{
		wxString &first_line = lines.front();
		if(first_line.length() < row_chars)
		{
			first_line += L"\u2026";
		}
		else{
			first_line.Last() = L'\u2026';
		}
		
		lines.erase(std::next(lines.begin()), lines.end());
	}
	
	{
		int box_x = x + (doc.hf_char_width() / 4);
		int box_y = y + (doc.hf_height / 4);
		
		unsigned int box_w = (doc.virtual_width - (indent_depth * doc.hf_char_width() * 2)) - (doc.hf_char_width() / 2);
		unsigned int box_h = (lines.size() * doc.hf_height) + (doc.hf_height / 2);
		
		dc.SetPen(wxPen((*active_palette)[Palette::PAL_NORMAL_TEXT_FG], 1));
		dc.SetBrush(wxBrush((*active_palette)[Palette::PAL_COMMENT_BG]));
		
		dc.DrawRectangle(box_x, box_y, box_w, box_h);
		
		if(indent_length > 0)
		{
			dc.DrawLine(box_x, (box_y + box_h), box_x, (box_y + box_h + doc.hf_height));
			dc.DrawLine((box_x + box_w - 1), (box_y + box_h), (box_x + box_w - 1), (box_y + box_h + doc.hf_height));
		}
	}
	
	y += doc.hf_height / 2;
	
	dc.SetTextForeground((*active_palette)[Palette::PAL_COMMENT_FG]);
	dc.SetBackgroundMode(wxTRANSPARENT);
	
	for(auto li = lines.begin(); li != lines.end(); ++li)
	{
		dc.DrawText(*li, (x + (doc.hf_char_width() / 2)), y);
		y += doc.hf_height;
	}
}

wxCursor REHex::DocumentCtrl::CommentRegion::cursor_for_point(REHex::DocumentCtrl &doc, int x, int64_t y_lines, int y_px)
{
	int hf_width = doc.hf_char_width();
	int indent_width = doc.indent_width(indent_depth);
	
	if(
		(y_lines > 0 || y_px >= (doc.hf_height / 4)) /* Not above top edge. */
		&& (y_lines < (this->y_lines - 1) || y_px <= ((doc.hf_height / 4) * 3)) /* Not below bottom edge. */
		&& x >= (indent_width + (hf_width / 4)) /* Not left of left edge. */
		&& x < ((doc.virtual_width - (hf_width / 4)) - indent_width)) /* Not right of right edge. */
	{
		return wxCursor(wxCURSOR_HAND);
	}
	else{
		return wxNullCursor;
	}
}
