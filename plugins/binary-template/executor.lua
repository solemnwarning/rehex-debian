-- Binary Template plugin for REHex
-- Copyright (C) 2021 Daniel Collins <solemnwarning@solemnwarning.net>
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

local M = {}

local FRAME_TYPE_BASE     = "base"
local FRAME_TYPE_STRUCT   = "struct"
local FRAME_TYPE_FUNCTION = "function"
local FRAME_TYPE_SCOPE    = "scope"

local FLOWCTRL_TYPE_RETURN   = 1
local FLOWCTRL_TYPE_BREAK    = 2
local FLOWCTRL_TYPE_CONTINUE = 4

local _find_type;

local _eval_number;
local _eval_string;
local _eval_ref;
local _eval_add;
local _numeric_op_func;
local _eval_logical_and;
local _eval_logical_or;
local _eval_variable;
local _eval_call;
local _eval_return
local _eval_func_defn;
local _eval_struct_defn
local _eval_if
local _eval_statement;
local _exec_statements;

local _ops

local function _topmost_frame_of_type(context, type)
	for i = #context.stack, 1, -1
	do
		if context.stack[i].frame_type == type
		then
			return context.stack[i]
		end
	end
	
	return nil
end

local function _can_do_flowctrl_here(context, flowctrl_type)
	for i = #context.stack, 1, -1
	do
		local frame = context.stack[i]
		
		if (frame.handles_flowctrl_types & flowctrl_type) == flowctrl_type
		then
			return true
		end
		
		if (frame.blocks_flowctrl_types & flowctrl_type) == flowctrl_type
		then
			return false
		end
	end
	
	return false
end

--
-- Type system
--

local function _make_named_type(name, type)
	local new_type = {};
	
	for k,v in pairs(type)
	do
		new_type[k] = v
	end
	
	new_type.name = name
	
	return new_type
end

local function _make_aray_type(type)
	local new_type = {};
	
	for k,v in pairs(type)
	do
		new_type[k] = v
	end
	
	new_type.is_array = true
	
	return new_type
end

local function _get_type_name(type)
	if type and type.is_array
	then
		return type.name .. "[]"
	elseif type
	then
		return type.name
	else
		return "void"
	end
end

local function _get_value_size(type, value)
	local x = function(v)
		if type.base == "struct"
		then
			local size = 0
			
			for k,v in pairs(v)
			do
				local member_type = v[1]
				local member_val  = v[2]
				
				size = size + _get_value_size(member_type, member_val)
			end
			
			return size
		else
			return type.length
		end
	end
	
	if type.is_array
	then
		local array_size = 0
		
		for _,v in ipairs(value)
		do
			array_size = array_size + x(v)
		end
		
		return array_size
	else
		return x(value)
	end
end

local function _type_assignable(dst_t, src_t)
	
	return (dst_t == nil and src_t == nil) or (
		
		dst_t ~= nil and src_t ~= nil and
		dst_t.base ~= "struct" and
		dst_t.base == src_t.base and
		dst_t.is_array == src_t.is_array)
end

local function _make_plain_value(value)
	return {
		value = value,
		
		get = function(self)
			return self.value
		end,
		
		set = function(self, value)
			self.value = value
		end,
	}
end

local function _make_const_plain_value(value)
	return {
		value = value,
		
		get = function(self)
			return self.value
		end,
		
		set = function(self, value)
			error("Attempt to set constant") -- TODO: Include template file/line
		end,
	}
end

local function _make_file_value(context, offset, length, fmt)
	return {
		get = function(self)
			local data = context.interface.read_data(offset, length)
			if data:len() < length
			then
				return nil
			end
			
			return string.unpack(fmt, data)
		end,
		
		set = function(self, value)
			error("Attempt to write to file variable") -- TODO: Include template file/line
		end,
	}
end

local _builtin_type_int8    = { rehex_type_le = "s8",    rehex_type_be = "s8",    length = 1, base = "number", string_fmt = "i1" }
local _builtin_type_uint8   = { rehex_type_le = "u8",    rehex_type_be = "u8",    length = 1, base = "number", string_fmt = "I1" }
local _builtin_type_int16   = { rehex_type_le = "s16le", rehex_type_be = "s16be", length = 2, base = "number", string_fmt = "i2" }
local _builtin_type_uint16  = { rehex_type_le = "u16le", rehex_type_be = "u16be", length = 2, base = "number", string_fmt = "I2" }
local _builtin_type_int32   = { rehex_type_le = "s32le", rehex_type_be = "s32be", length = 4, base = "number", string_fmt = "i4" }
local _builtin_type_uint32  = { rehex_type_le = "u32le", rehex_type_be = "u32be", length = 4, base = "number", string_fmt = "I4" }
local _builtin_type_int64   = { rehex_type_le = "s64le", rehex_type_be = "s64be", length = 8, base = "number", string_fmt = "i8" }
local _builtin_type_uint64  = { rehex_type_le = "u64le", rehex_type_be = "u64be", length = 8, base = "number", string_fmt = "I8" }
local _builtin_type_float32 = { rehex_type_le = "f32le", rehex_type_be = "f32be", length = 4, base = "number", string_fmt = "f" }
local _builtin_type_float64 = { rehex_type_le = "f64le", rehex_type_be = "f64be", length = 8, base = "number", string_fmt = "d" }

-- Placeholder for ... in builtin function parameters. Not a valid type in most contexts but the
-- _eval_call() function handles this specific object specially.
local _variadic_placeholder = {}

local _builtin_types = {
	char = _make_named_type("char", _builtin_type_int8),
	byte = _make_named_type("byte", _builtin_type_int8),
	CHAR = _make_named_type("CHAR", _builtin_type_int8),
	BYTE = _make_named_type("BYTE", _builtin_type_int8),
	
	uchar = _make_named_type("uchar", _builtin_type_uint8),
	ubyte = _make_named_type("ubyte", _builtin_type_uint8),
	UCHAR = _make_named_type("UCHAR", _builtin_type_uint8),
	UBYTE = _make_named_type("UBYTE", _builtin_type_uint8),
	
	short = _make_named_type("short", _builtin_type_int16),
	int16 = _make_named_type("int16", _builtin_type_int16),
	SHORT = _make_named_type("SHORT", _builtin_type_int16),
	INT16 = _make_named_type("INT16", _builtin_type_int16),
	
	ushort = _make_named_type("ushort", _builtin_type_uint16),
	uint16 = _make_named_type("uint16", _builtin_type_uint16),
	USHORT = _make_named_type("USHORT", _builtin_type_uint16),
	UINT16 = _make_named_type("UINT16", _builtin_type_uint16),
	WORD   = _make_named_type("WORD",   _builtin_type_uint16),
	
	int   = _make_named_type("int",   _builtin_type_int32),
	int32 = _make_named_type("int32", _builtin_type_int32),
	long  = _make_named_type("long",  _builtin_type_int32),
	INT   = _make_named_type("INT",   _builtin_type_int32),
	INT32 = _make_named_type("INT32", _builtin_type_int32),
	LONG  = _make_named_type("LONG",  _builtin_type_int32),
	
	uint   = _make_named_type("uint",   _builtin_type_uint32),
	uint32 = _make_named_type("uint32", _builtin_type_uint32),
	ulong  = _make_named_type("ulong",  _builtin_type_uint32),
	UINT   = _make_named_type("UINT",   _builtin_type_uint32),
	UINT32 = _make_named_type("UINT32", _builtin_type_uint32),
	ULONG  = _make_named_type("ULONG",  _builtin_type_uint32),
	DWORD  = _make_named_type("DWORD",  _builtin_type_uint32),
	
	int64   = _make_named_type("int64",   _builtin_type_int64),
	quad    = _make_named_type("quad",    _builtin_type_int64),
	QUAD    = _make_named_type("QUAD",    _builtin_type_int64),
	INT64   = _make_named_type("INT64",   _builtin_type_int64),
	__int64 = _make_named_type("__int64", _builtin_type_int64),
	
	uint64   = _make_named_type("uint64",   _builtin_type_uint64),
	uquad    = _make_named_type("uquad",    _builtin_type_uint64),
	UQUAD    = _make_named_type("UQUAD",    _builtin_type_uint64),
	UINT64   = _make_named_type("UINT64",   _builtin_type_uint64),
	QWORD    = _make_named_type("QWORD",    _builtin_type_uint64),
	__uint64 = _make_named_type("__uint64", _builtin_type_uint64),
	
	float = _make_named_type("float", _builtin_type_float32),
	FLOAT = _make_named_type("FLOAT", _builtin_type_float32),
	
	double = _make_named_type("double", _builtin_type_float64),
	DOUBLE = _make_named_type("DOUBLE", _builtin_type_float64),
	
	string = { name = "string", base = "string" },
}

local function _builtin_function_BigEndian(context, argv)
	context.big_endian = true
end

local function _builtin_function_LittleEndian(context, argv)
	context.big_endian = false
end

local function _builtin_function_Printf(context, argv)
	-- Copy format unchanged
	local print_args = { argv[1][2]:get() }
	
	-- Pass value part of other arguments - should all be lua numbers or strings
	for i = 2, #argv
	do
		print_args[i] = argv[i][2]:get()
	end
	
	context.interface.print(string.format(table.unpack(print_args)))
end

-- Table of builtin functions - gets copied into new interpreter contexts
--
-- Each key is a function name, the value is a table with the following values:
--
-- Table of argument types (arguments)
-- Function implementation (impl)

local _builtin_functions = {
	BigEndian    = { arguments = {}, impl = _builtin_function_BigEndian },
	LittleEndian = { arguments = {}, impl = _builtin_function_LittleEndian },
	
	Printf = { arguments = { _builtin_types.string, _variadic_placeholder }, impl = _builtin_function_Printf },
}

_find_type = function(context, type_name)
	for i = #context.stack, 1, -1
	do
		for k, v in pairs(context.stack[i].var_types)
		do
			if k == type_name
			then
				return v
			end
		end
	end
end

--
-- The _eval_XXX functions are what actually take the individual statements
-- from the AST and execute them.
--
-- All _eval_XXX functions take the context and a single statement and return
-- the type and value of their result (or nothing if void).
--
-- Most _eval_XXX functions are called indirectly via the _eval_statement()
-- function and the _ops table.
---

_eval_number = function(context, statement)
	return _builtin_types.int, _make_const_plain_value(statement[4])
end

_eval_string = function(context, statement)
	return _builtin_types.string, _make_const_plain_value(statement[4])
end

-- Resolves a variable reference to an actual value.
_eval_ref = function(context, statement)
	local filename = statement[1]
	local line_num = statement[2]
	
	local path = statement[4]
	
	-- This function walks along from the second element of path, resolving any array and/or
	-- struct dereferences before returning the final type+value pair.
	
	local _walk_path = function(rvalue)
		local rv_type = rvalue[1]
		local rv_val  = rvalue[2]
		
		for i = 2, #path
		do
			if type(path[i]) == "table"
			then
				-- This is a statement to be evalulated and used as an array index.
				
				local array_idx_t, array_idx_v = _eval_statement(context, path[i])
				
				if array_idx_t == nil or array_idx_t.base ~= "number"
				then
					error("Invalid '" .. _get_type_name(array_idx_t) .. "' operand to '[]' operator - expected a number at " .. filename .. ":" .. line_num)
				end
				
				if rv_type.is_array
				then
					local array_idx = array_idx_v:get();
					
					if array_idx < 0 or array_idx >= #rv_val
					then
						error("Attempt to access out-of-range array index " .. array_idx .. " at " .. filename .. ":" .. line_num)
					else
						rv_val = rv_val[array_idx_v:get() + 1]
					end
				else
					error("Attempt to access non-array variable as array at " .. filename .. ":" .. line_num)
				end
			else
				-- This is a string to be used as a struct member
				
				local member = path[i]
				
				if rv_type.base ~= "struct"
				then
					error("Attempt to access '" .. _get_type_name(rv_type) .. "' as a struct at " .. filename .. ":" .. line_num)
				end
				
				if rv_val[member] == nil
				then
					error("Attempt to access undefined struct member '" .. member .. "' at " .. filename .. ":" .. line_num)
				end
				
				rv_type = rv_val[member][1]
				rv_val  = rv_val[member][2]
			end
		end
		
		return rv_type, rv_val
	end
	
	-- Walk through symbol tables looking for the first element in path
	
	for frame_idx = #context.stack, 1, -1
	do
		local frame = context.stack[frame_idx]
		
		if frame.vars[ path[1] ] ~= nil
		then
			return _walk_path(frame.vars[ path[1] ])
		end
		
		if frame.frame_type == FRAME_TYPE_FUNCTION
		then
			-- Don't look for variables in parent functions
			break
		end
	end
	
	if context.global_vars[ path[1] ] ~= nil
	then
		return _walk_path(context.global_vars[ path[1] ])
	end
	
	error("Internal error: undefined variable '" .. path[1] .. "' at " .. filename .. ":" .. line_num)
end

_eval_add = function(context, statement)
	local filename = statement[1]
	local line_num = statement[2]
	
	local v1_t, v1_v = _eval_statement(context, statement[4])
	local v2_t, v2_v = _eval_statement(context, statement[5])
	
	if v1_t == nil or v2_t == nil or v1_t.base ~= v2_t.base
	then
		local v1_type = v1 and v1_t.name or "void"
		local v2_type = v2 and v2_t.name or "void"
		
		error("Invalid operands to '+' operator - '" .. v1_type .. "' and '" .. v2_type .. "' at " .. filename .. ":" .. line_num)
	end
	
	if v1_t.base == "number"
	then
		return v1_t, _make_const_plain_value(v1_v:get() + v2_v:get())
	elseif v1[1].base == "string"
	then
		return v1_t, _make_const_plain_value(v1_v:get() .. v2_v:get())
	else
		error("Internal error: unknown base type '" .. v1_t.base "' at " .. filename .. ":" .. line_num)
	end
end

_numeric_op_func = function(func, sym)
	return function(context, statement)
		local filename = statement[1]
		local line_num = statement[2]
		
		local v1_t, v1_v = _eval_statement(context, statement[4])
		local v2_t, v2_v = _eval_statement(context, statement[5])
		
		if (v1_t and v1_t.base) ~= "number" or (v2_t and v2_t.base) ~= "number"
		then
			error("Invalid operands to '" .. sym .. "' operator - '" .. _get_type_name(v1_t) .. "' and '" .. _get_type_name(v2_t) .. "' at " .. filename .. ":" .. line_num)
		end
		
		return v1_t, _make_const_plain_value(func(v1_v:get(), v2_v:get()))
	end
end

_eval_logical_and = function(context, statement)
	local filename = statement[1]
	local line_num = statement[2]
	
	local v1_t, v1_v = _eval_statement(context, statement[4])
	
	if v1_t == nil or v1_t.base ~= "number"
	then
		error("Invalid left operand to '&&' operator - expected numeric, got '" .. _get_type_name(v1_t) .. "' at " .. filename .. ":" .. line_num)
	end
	
	if v1_v:get() == 0
	then
		return _builtin_types.int, _make_const_plain_value(0)
	end
	
	local v2_t, v2_v = _eval_statement(context, statement[5])
	
	if v2_t == nil or v2_t.base ~= "number"
	then
		error("Invalid right operand to '&&' operator - expected numeric, got '" .. _get_type_name(v2_t) .. "' at " .. filename .. ":" .. line_num)
	end
	
	if v2_v:get() == 0
	then
		return _builtin_types.int, _make_const_plain_value(0)
	end
	
	return _builtin_types.int, _make_const_plain_value(1)
end

_eval_logical_or = function(context, statement)
	local filename = statement[1]
	local line_num = statement[2]
	
	local v1_t, v1_v = _eval_statement(context, statement[4])
	
	if v1_t == nil or v1_t.base ~= "number"
	then
		error("Invalid left operand to '||' operator - expected numeric, got '" .. _get_type_name(v1_t) .. "' at " .. filename .. ":" .. line_num)
	end
	
	if v1_v:get() ~= 0
	then
		return _builtin_types.int, _make_const_plain_value(1)
	end
	
	local v2_t, v2_v = _eval_statement(context, statement[5])
	
	if v2_t == nil or v2_t.base ~= "number"
	then
		error("Invalid right operand to '||' operator - expected numeric, got '" .. _get_type_name(v2_t) .. "' at " .. filename .. ":" .. line_num)
	end
	
	if v2_v:get() ~= 0
	then
		return _builtin_types.int, _make_const_plain_value(1)
	end
	
	return _builtin_types.int, _make_const_plain_value(0)
end

_eval_variable = function(context, statement)
	local filename = statement[1]
	local line_num = statement[2]
	
	local v_type = statement[4]
	local v_name = statement[5]
	local v_length = statement[6][1]
	
	local type_info = _find_type(context, v_type)
	if type_info == nil
	then
		error("Unknown variable type '" .. v_type .. "' at " .. filename .. ":" .. line_num)
	end
	
	-- TODO: Make sure we're not in a function... or handle it if we are
	
	local data_type_key = context.big_endian and type_info.rehex_type_be or type_info.rehex_type_le
	
	local func_frame = _topmost_frame_of_type(context, FRAME_TYPE_FUNCTION)
	if func_frame ~= nil
	then
		error("Attempt to define global variable '" .. v_name .. "' inside function at " .. filename .. ":" .. line_num)
	end
	
	local dest_tables
	
	local struct_frame = _topmost_frame_of_type(context, FRAME_TYPE_STRUCT)
	if struct_frame ~= nil
	then
		dest_tables = { struct_frame.vars, struct_frame.struct_members }
		
		if struct_frame.vars[v_name] ~= nil
		then
			error("Attempt to redefine struct member '" .. v_name .. "' at " .. filename .. ":" .. line_num)
		end
	else
		dest_tables = { context.global_vars }
		
		if context.global_vars[v_name] ~= nil
		then
			error("Attempt to redefine global variable '" .. v_name .. "' at " .. filename .. ":" .. line_num)
		end
	end
	
	local expand_value = function()
		if type_info.base == "struct"
		then
			-- TODO: Support struct arguments
			
			local members = {}
			
			local frame = {
				frame_type = FRAME_TYPE_STRUCT,
				var_types = {},
				vars = {},
				struct_members = members,
				
				blocks_flowctrl_types = (FLOWCTRL_TYPE_RETURN | FLOWCTRL_TYPE_BREAK | FLOWCTRL_TYPE_CONTINUE),
			}
			
			table.insert(context.stack, frame)
			_exec_statements(context, type_info.code)
			table.remove(context.stack)
			
			return members
		else
			local data_type_fmt = (context.big_endian and ">" or "<") .. type_info.string_fmt
			return _make_file_value(context, context.next_variable, type_info.length, data_type_fmt)
		end
	end
	
	if v_length == nil
	then
		local base_off = context.next_variable
		
		local value = expand_value()
		local length = _get_value_size(type_info, value)
		
		for _,t in ipairs(dest_tables)
		do
			t[v_name] = { type_info, value }
		end
		
		if data_type_key ~= nil
		then
			context.interface.set_data_type(base_off, length, data_type_key)
		end
		
		context.interface.set_comment(base_off, length, v_name)
		
		context.next_variable = base_off + length
	else
		local array_length_type, array_length_val = _eval_statement(context, v_length)
		if array_length_type == nil or array_length_type.base ~= "number"
		then
			error("Expected numeric type for array size, got '" .. _get_type_name(array_length_type) .. "' at " .. filename .. ":" .. line_num)
		end
		
		local g_values = {}
		
		for _,t in ipairs(dest_tables)
		do
			t[v_name] = {
				_make_aray_type(type_info),
				g_values
			}
		end
		
		for i = 0, array_length_val:get() - 1
		do
			local base_off = context.next_variable
			
			local value = expand_value()
			local length = _get_value_size(type_info, value)
			
			table.insert(g_values, value)
			
			if data_type_key ~= nil
			then
				context.interface.set_data_type(base_off, length, data_type_key)
			end
			
			context.interface.set_comment(base_off, length, v_name .. "[" .. i .. "]")
			
			context.next_variable = base_off + length
		end
	end
end

_eval_call = function(context, statement)
	local filename = statement[1]
	local line_num = statement[2]
	
	local func_name = statement[4]
	local func_args = statement[5]
	
	local func_defn = context.functions[func_name]
	if func_defn == nil
	then
		error("Attempt to call undefined function '" .. func_name .. "' at " .. filename .. ":" .. line_num)
	end
	
	local func_arg_values = {}
	
	for i = 1, #func_args
	do
		func_arg_values[i] = { _eval_statement(context, func_args[i]) }
		-- TODO: Check for type compatibility
	end
	
	return func_defn.impl(context, func_arg_values)
end

_eval_return = function(context, statement)
	local filename = statement[1]
	local line_num = statement[2]
	
	local retval = statement[4]
	
	if not _can_do_flowctrl_here(context, FLOWCTRL_TYPE_RETURN)
	then
		error("'return' statement not allowed here at " .. filename .. ":" .. line_num)
	end
	
	local func_frame = _topmost_frame_of_type(context, FRAME_TYPE_FUNCTION)
	
	if retval
	then
		local retval_t, retval_v = _eval_statement(context, retval)
		
		if not _type_assignable(func_frame.return_type, retval_t)
		then
			error("return operand type '" .. _get_type_name(retval_t) .. "' not compatible with function return type '" .. _get_type_name(func_frame.return_type) .. "' at " .. filename .. ":" .. line_num)
		end
		
		if retval_t
		then
			retval = { retval_t, retval_v }
		else
			retval = nil
		end
	elseif func_frame.return_type ~= nil
	then
		error("return without an operand in function that returns type '" .. _get_type_name(func_frame.return_type) .. "' at " .. filename .. ":" .. line_num)
	end
	
	return { flowctrl = FLOWCTRL_TYPE_RETURN }, retval
end

_eval_func_defn = function(context, statement)
	local filename = statement[1]
	local line_num = statement[2]
	
	local func_ret_type   = statement[4]
	local func_name       = statement[5]
	local func_arg_types  = statement[6]
	local func_statements = statement[7]
	
	if context.functions[func_name] ~= nil
	then
		error("Attempt to redefine function '" .. func_name .. "' at " .. filename .. ":" .. line_num)
	end
	
	local ret_type
	if func_ret_type ~= "void"
	then
		ret_type = _find_type(context, func_ret_type)
		if ret_type == nil
		then
			error("Attempt to define function '" .. func_name .. "' with undefined return type '" .. func_ret_type .. "' at " .. filename .. ":" .. line_num)
		end
	end
	
	local arg_types = {}
	for i = 1, #func_arg_types
	do
		local type_info = _find_type(context, func_arg_types[i])
		if type_info == nil
		then
			error("Attempt to define function '" .. func_name .. "' with undefined argument type '" .. func_arg_values[i] .. "' at " .. filename .. ":" .. line_num)
		end
		
		table.insert(arg_types, type_info)
	end
	
	local impl_func = function(context, arguments)
		local frame = {
			frame_type = FRAME_TYPE_FUNCTION,
			var_types = {},
			vars = {},
			
			handles_flowctrl_types = FLOWCTRL_TYPE_RETURN,
			blocks_flowctrl_types  = (FLOWCTRL_TYPE_BREAK | FLOWCTRL_TYPE_CONTINUE),
			
			return_type = ret_type,
		}
		
		table.insert(context.stack, frame)
		
		local retval
		
		for _, statement in ipairs(func_statements)
		do
			local sr_t, sr_v = _eval_statement(context, statement)
			
			if sr_t and sr_t.flowctrl ~= nil
			then
				if sr_t.flowctrl == FLOWCTRL_TYPE_RETURN
				then
					retval = sr_v
					break
				else
					error("Internal error: unexpected flowctrl type '" .. sr_t.flowctrl .. "'")
				end
			end
		end
		
		table.remove(context.stack)
		
		if retval == nil and ret_type ~= nil
		then
			error("No return statement in function returning non-void at " .. filename .. ":" .. line_num)
		end
		
		if retval ~= nil
		then
			return table.unpack(retval)
		end
	end
	
	context.functions[func_name] = {
		arguments = arg_types,
		impl      = impl_func,
	}
end

_eval_struct_defn = function(context, statement)
	local filename = statement[1]
	local line_num = statement[2]
	
	local struct_name       = statement[4]
	local struct_arg_types  = statement[5]
	local struct_statements = statement[6]
	
	if _find_type(context, "struct " .. struct_name) ~= nil
	then
		error("Attempt to redefine type 'struct " .. struct_name .. "' at " .. filename .. ":" .. line_num)
	end
	
	local arg_types = {}
	for i = 1, #struct_arg_types
	do
		local type_info = _find_type(context, struct_arg_types[i])
		if type_info == nil
		then
			error("Attempt to define 'struct " .. struct_name .. "' with undefined argument type '" .. struct_arg_values[i] .. "' at " .. filename .. ":" .. line_num)
		end
		
		table.insert(arg_types, type_info)
	end
	
	context.stack[#context.stack].var_types["struct " .. struct_name] = {
		base      = "struct",
		name      = "struct " .. struct_name,
		arguments = arg_types,
		code      = struct_statements
		
		-- rehex_type_le = "s8",
		-- rehex_type_be = "s8",
		-- length = 1,
		-- string_fmt = "i1"
	}
end

_eval_if = function(context, statement)
	local filename = statement[1]
	local line_num = statement[2]
	
	for i = 4, #statement
	do
		local cond = statement[i][2] and statement[i][1] or { "BUILTIN", 0, "num", 1 }
		local code = statement[i][2] or statement[i][1]
		
		local cond_t, cond_v = _eval_statement(context, cond)
		
		if (cond_t and cond_t.base) ~= "number"
		then
			error("Expected numeric expression to if/else if at " .. cond[1] .. ":" .. cond[2])
		end
		
		if cond_v:get() ~= 0
		then
			_exec_statements(context, code)
			break
		end
	end
end

_eval_statement = function(context, statement)
	local filename = statement[1]
	local line_num = statement[2]
	
	-- TODO: Call yield() periodically
	
	local op = statement[3]
	
	if _ops[op] ~= nil
	then
		return _ops[op](context, statement)
	else
		error("Internal error: unexpected op '" .. op .. "' at " .. filename .. ":" .. line_num)
	end
	
end

_exec_statements = function(context, statements)
	for _, statement in ipairs(statements)
	do
		_eval_statement(context, statement)
	end
end

_ops = {
	num = _eval_number,
	str = _eval_string,
	ref = _eval_ref,
	
	variable     = _eval_variable,
	call         = _eval_call,
	["return"]   = _eval_return,
	["function"] = _eval_func_defn,
	["struct"]   = _eval_struct_defn,
	["if"]       = _eval_if,
	
	add      = _eval_add,
	subtract = _numeric_op_func(function(v1, v2) return v1 - v2 end, "-"),
	multiply = _numeric_op_func(function(v1, v2) return v1 * v2 end, "*"),
	divide   = _numeric_op_func(function(v1, v2) return v1 / v2 end, "/"),
	mod      = _numeric_op_func(function(v1, v2) return v1 % v2 end, "%"),
	
	["left-shift"]  = _numeric_op_func(function(v1, v2) return v1 << v2 end, "<<"),
	["right-shift"] = _numeric_op_func(function(v1, v2) return v1 >> v2 end, ">>"),
	
	["bitwise-and"] = _numeric_op_func(function(v1, v2) return v1 & v2 end, "&"),
	["bitwise-xor"] = _numeric_op_func(function(v1, v2) return v1 ^ v2 end, "^"),
	["bitwise-or"]  = _numeric_op_func(function(v1, v2) return v1 | v2 end, "|"),
	
	["less-than"]             = _numeric_op_func(function(v1, v2) return v1 <  v2 and 1 or 0 end, "<"),
	["less-than-or-equal"]    = _numeric_op_func(function(v1, v2) return v1 <= v2 and 1 or 0 end, "<="),
	["greater-than"]          = _numeric_op_func(function(v1, v2) return v1 >  v2 and 1 or 0 end, ">"),
	["greater-than-or-equal"] = _numeric_op_func(function(v1, v2) return v1 >= v2 and 1 or 0 end, ">="),
	
	["equal"]     = _numeric_op_func(function(v1, v2) return v1 == v2 and 1 or 0 end, "=="),
	["not-equal"] = _numeric_op_func(function(v1, v2) return v1 ~= v2 and 1 or 0 end, "!="),
	
	["logical-and"] = _eval_logical_and,
	["logical-or"]  = _eval_logical_or,
}

--- External entry point into the interpreter
-- @function execute
--
-- @param interface Table of interface functions to be used by the interpreter.
-- @param statements AST table as returned by the parser.
--
-- The interface table must have the following functions:
--
-- interface.set_data_type(offset, length, data_type)
-- interface.set_comment(offset, length, comment_text)
--
-- Thin wrappers around the REHex APIs, to allow for testing.
--
-- interface.yield()
--
-- Periodically called by the executor to allow processing UI events.
-- An error() may be raised within to abort the interpreter.

local function execute(interface, statements)
	local context = {
		interface = interface,
		
		functions = {},
		stack = {},
		
		-- This table holds the top-level "global" (i.e. not "local") variables by name.
		-- Each element points to a tuple of { type, value } like other rvalues.
		global_vars = {},
		
		-- This table holds our current position in global_vars - under which new variables
		-- be added and relative to which any lookups should be performed.
		--
		-- Each key will either be a tuple of { key, table }, where the key is a string
		-- when we are descending into a struct member or a number where we are in an array
		-- and the table is a reference to the element in global_vars at this point.
		global_stack = {},
		
		next_variable = 0,
		
		-- Are we currently accessing variables from the file as big endian? Toggled by
		-- the built-in BigEndian() and LittleEndian() functions.
		big_endian = false,
	}
	
	for k, v in pairs(_builtin_functions)
	do
		context.functions[k] = v
	end
	
	local base_types = {};
	for k, v in pairs(_builtin_types)
	do
		base_types[k] = v
	end
	
	table.insert(context.stack, {
		frame_type = FRAME_TYPE_BASE,
		
		var_types = base_types,
		vars = {},
	})
	
	_exec_statements(context, statements)
end

M.execute = execute

return M
