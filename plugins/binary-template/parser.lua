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

--local lpeg = require 'lpeg'
local lpeg = require 'lulpeg.lulpeg'
setmetatable(_ENV, { __index=lpeg })

local RESERVED_WORDS = {
	["if"]        = true,
	["else"]      = true,
	["struct"]    = true,
	["typedef"]   = true,
	["while"]     = true,
	["for"]       = true,
	["break"]     = true,
	["continue"]  = true,
	["return"]    = true,
	["unsigned"]  = true,
}

local function comment(openp,endp)
    openp = P(openp)
    endp = P(endp)
    local upto_endp = (1 - endp) ^ 1
    return openp * upto_endp * endp
end

-- Global table of line start/end positions in the input stream
-- Generated by build_lines_table() and searched by input_pos_to_file_and_line_num()
local input_lines

local function build_lines_table(text)
	input_lines = {}
	
	local filename = "UNKNOWN FILE"
	local line_num = 1
	
	local line_start = 1
	
	local i = 1
	while i <= text:len()
	do
		local m_filename, m_line_num = text:match("^#file%s+([^\n]+)%s+(%d+)\n", i)
		
		if m_filename ~= nil
		then
			filename = m_filename
			line_num = math.floor(m_line_num)
			
			i = text:find("\n", i)
			line_start = i + 1
		elseif text:sub(i, i) == "\n" or i == text:len()
		then
			table.insert(input_lines, {
				["filename"] = filename,
				["line_num"] = line_num,
				
				["line_start"] = line_start,
				["line_end"]   = i,
			})
			
			line_num = line_num + 1
			line_start = i + 1
		end
		
		i = i + 1
	end
end

local function input_pos_to_file_and_line_num(pos)
	local lower = 1
	local upper = #input_lines
	
	while lower <= upper
	do
		local i = lower + math.floor((upper - lower) / 2)
		
		if input_lines[i].line_end < pos
		then
			lower = i + 1
		elseif input_lines[i].line_start > pos
		then
			upper = i - 1
		else
			return input_lines[i].filename, input_lines[i].line_num
		end
	end
	
	return "UNKNOWN FILE", 0
end

local function _parser_fallback(text, pos)
	-- Getting here means we're trying to parse something and none of the real captures have
	-- matched, so any actual text is a parse error.
	
	pos = pos - 1
	
	if pos < text:len()
	then
		local pos_filename, pos_line_num = input_pos_to_file_and_line_num(pos)
		error("Parse error at " .. pos_filename .. ":" .. pos_line_num .. " (at '" .. text:sub(pos, pos + 10) .. "')")
	end
	
	return nil
end

local function _consume_directive(text, pos)
	-- Directives from the preprocessor begin at column zero, anything else is from the
	-- template source.
	
	if (pos == 2 or text:sub(pos - 2, pos - 2) == "\n") and text:sub(pos - 1, pos - 1) == "#"
	then
		local directive_end = text:find("\n", pos);
		return directive_end + 1;
	end
	
	return nil
end

local function _capture_position(text, pos)
	return pos, "_input_stream_pos", pos
end

local function _capture_string(text, pos)
	local s = ""
	
	for i = pos, text:len()
	do
		local c = text:sub(i, i)
		
		if c == "\\"
		then
			-- TODO
		elseif c == '"'
		then
			return i + 1, s
		else
			s = s .. c
		end
	end
	
	local filename, line_num = input_pos_to_file_and_line_num(pos - 1)
	error("Unmatched \" at " .. filename .. ":" .. line_num)
end

local function _capture_type(text, pos)
	local patterns = {
		"^(enum)%s+([%a_][%d%a_]*)%s*",
		"^(struct)%s+([%a_][%d%a_]*)%s*",
		"^(unsigned)%s+([%a_][%d%a_]*)%s*",
		"^([%a_][%d%a_]*)%s*",
	}
	
	for _, pattern in ipairs(patterns)
	do
		local captures = table.pack(text:find(pattern, pos))
		
		local match_begin = captures[1]
		table.remove(captures, 1)
		
		local match_end = captures[1]
		table.remove(captures, 1)
		
		if match_begin ~= nil and not RESERVED_WORDS[ captures[#captures] ]
		then
			return match_end + 1, table.concat(captures, " ")
		end
	end
end

local function _skip_type(text, pos)
	local ret = _capture_type(text, pos)
	return ret
end

local function _capture_name(text, pos)
	local match_begin, match_end = text:find("^[%a_][%w_]*", pos)
	if match_begin ~= nil
	then
		local word = text:sub(match_begin, match_end)
		
		if RESERVED_WORDS[word] ~= nil
		then
			-- This is a reserved word, don't match
			return
		end
		
		return match_end + 1, word
	end
end

local spc = S(" \t\r\n")^0
local spc_req = S(" \t\r\n")^1
local digit = R('09')
local number = C( P('-')^-1 * digit^1 * ( P('.') * digit^1 )^-1 ) / tonumber * spc
local name = P(_capture_name) * spc
local name_nospc = P(_capture_name)
local comma  = P(",") * spc

local _parser = spc * P{
	"TEMPLATE";
	TEMPLATE =
		Ct( (V("STMT") + P(1) * P(_parser_fallback)) ^ 0),
	
	VALUE_NUM = Cc("num") * number,
	VALUE_STR = Cc("str") * P('"') * P(_capture_string) * spc,
	
	VALUE_REF = Cc("ref") * Ct(
		name_nospc * (P("[") * V("EXPR") * P("]"))^-1 *
		(P(".") * name_nospc * (P("[") * V("EXPR") * P("]"))^-1)^0
		) * spc,
	
	VALUE = P(_capture_position) * (V("VALUE_NUM") + V("VALUE_STR") + V("VALUE_REF")),
	
	STMT =
		P(1) * P(_consume_directive) * spc +
		V("BLOCK") +
		V("COMMENT") +
		V("IF") +
		V("FOR") +
		V("WHILE") +
		V("SWITCH") +
		V("STRUCT_DEFN") +
		V("ENUM_DEFN") +
		V("TYPEDEF") +
		V("FUNC_DEFN") +
		V("LOCAL_VAR_DEFN") +
		V("VAR_DEFN") +
		V("RETURN") +
		V("BREAK") +
		V("CONTINUE") +
		V("EXPR") * P(";") * spc +
		P(";") * spc,
	
	BLOCK = P("{") * spc * ( V("STMT") ^ 0 ) * spc * P("}"),
	
	COMMENT = spc * comment("//", "\n") * spc
		+ spc * comment("/*", "*/") * spc,
	
	EXPR =
		Ct( P(_capture_position) * Cc("_expr") * Ct(
			(
				-- TODO: Capture casts rather than discarding them
				P("(") * spc * P(_skip_type) * P(")") * V("EXPR") +
				V("EXPR2")
			) ^ 1
		) ),
	
	EXPR2 =
		P("(") * spc * V("EXPR") * P(")") * spc +
		Ct( P(_capture_position) * Cc("call") * name * Ct( S("(") * spc * (V("EXPR") * (comma * V("EXPR")) ^ 0) ^ -1 * S(")") ) * spc ) +
		Ct( V("VALUE") ) +
		Ct( P(_capture_position) * Cc("_token") *
			C( P("<<") + P(">>") + P("<=") + P(">=") + P("==") + P("!=") + P("&&") + P("||") + S("!~*/%+-<>&^|=") ) * spc),
	
	EXPR_OR_NIL = V("EXPR") + Cc(nil) * spc,
	ZERO_OR_MORE_EXPRS = (V("EXPR") * (comma * V("EXPR")) ^ 0) ^ -1,
	
	--  {
	--      "file.bt", <line>,
	--      "variable",
	--      <variable type>,
	--      <variable name>,
	--      { <struct parameters> } OR nil,
	--      <array size expr> OR nil,
	--  }
	VAR_DEFN = Ct(
		P(_capture_position) * Cc("variable") *
		P(_capture_type) * name *
		(P("(") * spc * Ct( V("ZERO_OR_MORE_EXPRS") ) * P(")") * spc + Cc(nil)) *
		(P("[") * spc * V("EXPR") * P("]") * spc + Cc(nil)) * P(";") * spc ),
	
	--  {
	--      "file.bt", <line>,
	--      "local-variable",
	--      <variable type>,
	--      <variable name>,
	--      { <struct parameters> } OR nil,
	--      <array size expr> OR nil,
	--      <initial value expr> OR nil,
	--  }
	LOCAL_VAR_DEFN = Ct(
		P(_capture_position) * Cc("local-variable") *
		P("local") * spc * P(_capture_type) * name *
		(P("(") * spc * Ct( V("ZERO_OR_MORE_EXPRS") ) * P(")") * spc + Cc(nil)) *
		(P("[") * spc * V("EXPR") * P("]") * spc + Cc(nil)) *
		(P("=") * spc * V("EXPR") * spc + Cc(nil)) * P(";") * spc ),
	
	RETURN = Ct( P(_capture_position) * Cc("return") * P("return") * spc * V("EXPR") * P(";") * spc),
	
	ARG = Ct( P(_capture_type) * name ),
	
	--  {
	--      "struct",
	--      "name" or nil
	--      { <arguments> },
	--      { <statements> },
	--      "typedef name" or nil,
	--      { <variable name>, { <parameters> }, <array size expr> OR nil } OR nil,
	--  }
	STRUCT_ARG_LIST = Ct( (S("(") * spc * (V("ARG") * (comma * V("ARG")) ^ 0) ^ -1 * S(")")) ^ -1 ),
	STRUCT_VAR_DECL = Ct( name * Ct( (P("(") * spc * V("ZERO_OR_MORE_EXPRS") * P(")") * spc) ^ -1) * (P("[") * spc * V("EXPR") * P("]") * spc + Cc(nil)) ),
	STRUCT_DEFN =
		Ct( P(_capture_position) * Cc("struct") *                      P("struct") * spc * name    * V("STRUCT_ARG_LIST") * spc * P("{") * spc * Ct( V("STMT") ^ 0 ) * P("}") * spc * Cc(nil) * (V("STRUCT_VAR_DECL") + Cc(nil)) * P(";") * spc ) +
		Ct( P(_capture_position) * Cc("struct") *                      P("struct") * spc * Cc(nil) * V("STRUCT_ARG_LIST") * spc * P("{") * spc * Ct( V("STMT") ^ 0 ) * P("}") * spc * Cc(nil) * (V("STRUCT_VAR_DECL") + Cc(nil)) * P(";") * spc ) +
		Ct( P(_capture_position) * Cc("struct") * P("typedef") * spc * P("struct") * spc * name    * V("STRUCT_ARG_LIST") * spc * P("{") * spc * Ct( V("STMT") ^ 0 ) * P("}") * spc * name                                       * P(";") * spc ) +
		Ct( P(_capture_position) * Cc("struct") * P("typedef") * spc * P("struct") * spc * Cc(nil) * V("STRUCT_ARG_LIST") * spc * P("{") * spc * Ct( V("STMT") ^ 0 ) * P("}") * spc * name                                       * P(";") * spc ),
	
	--  {
	--      "file.bt", <line>,
	--      "typedef",
	--      "struct foobar",
	--      "foobar_t",
	--      <array size expr> OR nil,
	--  }
	TYPEDEF = Ct( P(_capture_position) * Cc("typedef") * P("typedef") * spc * P(_capture_type) * name * (P("[") * spc * V("EXPR") * P("]") * spc + Cc(nil)) * P(";") * spc ),
	
	--  {
	--      "file.bt", <line>,
	--      "enum",
	--      "value type - int/word/etc",,
	--      "enum name" or nil,
	--      {
	--          { "member name" } or { "member name", <value expr> },
	--      },
	--      "typedef name" or nil,
	--  }
	ENUM_TYPE        = (P("<") * P(_capture_type) * P(">") * spc) + Cc("int"),
	ENUM_MEMBER      = Ct( name * (P("=") * spc * V("EXPR")) ^ -1 ),
	ENUM_MEMBER_LIST = Ct( V("ENUM_MEMBER") * (comma * V("ENUM_MEMBER")) ^ 0 ),
	ENUM_DEFN =
		Ct( P(_capture_position) * Cc("enum") *                      P("enum") * spc * V("ENUM_TYPE") * name    * P("{") * spc * V("ENUM_MEMBER_LIST") * P("}") * spc * Cc(nil) * P(";") * spc ) +
		Ct( P(_capture_position) * Cc("enum") * P("typedef") * spc * P("enum") * spc * V("ENUM_TYPE") * name    * P("{") * spc * V("ENUM_MEMBER_LIST") * P("}") * spc * name    * P(";") * spc ) +
		Ct( P(_capture_position) * Cc("enum") * P("typedef") * spc * P("enum") * spc * V("ENUM_TYPE") * Cc(nil) * P("{") * spc * V("ENUM_MEMBER_LIST") * P("}") * spc * name    * P(";") * spc ),
	
	--  {
	--      "function",
	--      "return type",
	--      "name",
	--      { <arguments> },
	--      { <statements> },
	--  }
	FUNC_ARG_LIST = Ct( S("(") * spc * (V("ARG") * (comma * V("ARG")) ^ 0) ^ -1 * S(")") ) * spc,
	FUNC_DEFN = Ct( P(_capture_position) * Cc("function") * name * name * V("FUNC_ARG_LIST") * P("{") * spc * Ct( (V("STMT") * spc) ^ 0 ) * P("}") * spc ),
	
	--  {
	--      "if",
	--      { <condition>, { <statements> } },  <-- if
	--      { <condition>, { <statements> } },  <-- else if
	--      { <condition>, { <statements> } },  <-- else if
	--      {              { <statements> } },  <-- else
	--  }
	IF = Ct( P(_capture_position) * Cc("if") *
		Ct( P("if")      * spc * P("(") * spc * V("EXPR") * P(")") * spc * Ct( V("STMT") ) )      * spc *
		Ct( P("else if") * spc * P("(") * spc * V("EXPR") * P(")") * spc * Ct( V("STMT") ) ) ^ 0  * spc *
		Ct( P("else")                                        * spc * Ct( V("STMT") ) ) ^ -1 * spc
	),
	
	--  {
	--      "file.bt", <line>,
	--      "for",
	--      { <init expr> } OR nil,
	--      { <cond expr> } OR nil,
	--      { <iter expr> } OR nil,
	--      { <statements> },
	--  }
	FOR = Ct( P(_capture_position) * Cc("for") *
		P("for") * spc * P("(") * spc *
			(V("LOCAL_VAR_DEFN") + (V("EXPR_OR_NIL") * P(";") * spc)) *
			V("EXPR_OR_NIL") * P(";") * spc *
			V("EXPR_OR_NIL") * P(")") * spc *
			Ct( V("STMT") ) * spc
	),
	
	-- while gets compiled to be a for loop with just a condition (see above)
	WHILE = Ct( P(_capture_position) * Cc("for") *
		P("while") * spc * P("(") * spc * Cc(nil) * V("EXPR") * Cc(nil) * P(")") * spc * Ct( V("STMT") ) * spc
	),
	
	--  {
	--      "file.bt", <line>,
	--      "switch",
	--      <expr>,
	--      {
	--          { <case expr> (nil for "default"), { <case statements> } },
	--          ...
	--      }
	--  }
	SWITCH = Ct(P(_capture_position) * Cc("switch") *
		P("switch") * spc * P("(") * spc * V("EXPR") * P(")") * spc * Ct( P("{") * spc *
			(Ct( P("case")    * spc_req * V("EXPR") * P(":") * spc * Ct( V("STMT") ^ 0 ) ) * spc +
			 Ct( P("default") * spc     * Cc(nil)   * P(":") * spc * Ct( V("STMT") ^ 0 ) ) * spc) ^ 1 *
		P("}") ) * spc
	),
	
	--  {
	--      "file.bt", <line>,
	--      "break",
	--  }
	BREAK = Ct( P(_capture_position) * Cc("break") * P("break") * spc ),
	
	--  {
	--      "file.bt", <line>,
	--      "continue",
	--  }
	CONTINUE = Ct( P(_capture_position) * Cc("continue") * P("continue") * spc ),
}

local function _resolve_pos(s)
	if s[1] ~= "_input_stream_pos"
	then
		error("Internal error: _resolve_pos called with filename '" .. s[1] .. "'")
	end
	
	s[1], s[2] = input_pos_to_file_and_line_num(s[2])
end

local function _compile_expr(expr)
	_resolve_pos(expr)
	
	local expr_parts = expr[4]
	
	if expr[3] ~= "_expr"
	then
		error("Internal error - _compile_expr() called with an '" .. expr[3] .. "' node")
	end
	
	local left_to_right = { start = function() return 1 end, step = 1 }
	local right_to_left = { start = function(e) return #expr_parts - e end, step = -1 }
	
	local expand_binops = function(dir, ops)
		local idx = dir.start(2)
		
		while idx >= 1 and (idx + 2) <= #expr_parts
		do
			local matched = false
			
			for op, ast_op in pairs(ops)
			do
				if
					expr_parts[idx + 1][3] == "_token" and expr_parts[idx + 1][4] == op and
					expr_parts[idx][3]:sub(1, 1) ~= "_" and
					expr_parts[idx + 2][3]:sub(1, 1) ~= "_"
				then
					expr_parts[idx] = { expr_parts[idx + 1][1], expr_parts[idx + 1][2], ast_op, expr_parts[idx], expr_parts[idx + 2] }
					table.remove(expr_parts, idx + 1)
					table.remove(expr_parts, idx + 1)
					
					matched = true
					break
				end
			end
			
			if not matched
			then
				idx = idx + dir.step
			elseif idx == #expr_parts
			then
				idx = idx + 2 * dir.step
			end
		end
	end
	
	local expand_unary_ops = function(dir, ops)
		local idx = dir.start(1)
		
		while idx >= 1 and (idx + 1) <= #expr_parts
		do
			local matched = false
			
			for op, ast_op in pairs(ops)
			do
				if
					expr_parts[idx][3] == "_token" and expr_parts[idx][4] == op and
					expr_parts[idx + 1][3]:sub(1, 1) ~= "_"
				then
					expr_parts[idx] = { expr_parts[idx][1], expr_parts[idx][2], ast_op, expr_parts[idx + 1] }
					table.remove(expr_parts, idx + 1)
					
					matched = true
					break
				end
			end
			
			if not matched
			then
				idx = idx + dir.step
			elseif idx == #expr_parts
			then
				idx = idx + 1 * dir.step
			end
		end
	end
	
	for i = 1, #expr_parts
	do
		if expr_parts[i][3] == "_expr"
		then
			_compile_expr(expr_parts[i])
		else
			_resolve_pos(expr_parts[i])
		end
		
		if expr_parts[i][3] == "ref"
		then
			local path = expr_parts[i][4]
			
			for i = 1, #path
			do
				if type(path[i]) == "table"
				then
					_compile_expr(path[i])
				end
			end
		elseif expr_parts[i][3] == "call"
		then
			local args = expr_parts[i][5]
			
			for i = 1, #args
			do
				_compile_expr(args[i])
			end
		end
	end
	
	expand_unary_ops(right_to_left, {
		["!"] = "logical-not",
		["~"] = "bitwise-not",
	})
	
	expand_binops(left_to_right, {
		["*"] = "multiply",
		["/"] = "divide",
		["%"] = "mod",
	})
	
	expand_binops(left_to_right, {
		["+"] = "add",
		["-"] = "subtract",
	})
	
	expand_binops(left_to_right, {
		["<<"] = "left-shift",
		[">>"] = "right-shift",
	})
	
	expand_binops(left_to_right, {
		["<"]  = "less-than",
		["<="] = "less-than-or-equal",
		[">"]  = "greater-than",
		[">="] = "greater-than-or-equal",
	})
	
	expand_binops(left_to_right, {
		["=="] = "equal",
		["!="] = "not-equal",
	})
	
	expand_binops(left_to_right, { ["&"] = "bitwise-and" })
	expand_binops(left_to_right, { ["^"] = "bitwise-xor" })
	expand_binops(left_to_right, { ["|"] = "bitwise-or" })
	
	expand_binops(left_to_right, { ["&&"] = "logical-and" })
	expand_binops(left_to_right, { ["||"] = "logical-or" })
	
	expand_binops(right_to_left, { ["="] = "assign" })
	
	if #expr_parts ~= 1
	then
		error("Unable to compile expression starting at " .. expr[1] .. ":" .. expr[2])
	end
	
	-- Replace expr's content with the compiled expression in expr_parts[1]
	
	while #expr > 0
	do
		table.remove(expr, #expr)
	end
	
	for _,v in ipairs(expr_parts[1])
	do
		table.insert(expr, v)
	end
end

local function _compile_statement(s)
	local op = s[3]
	
	if op == "_expr"
	then
		_compile_expr(s)
		return
	end
	
	_resolve_pos(s)
	
	if op == "function"
	then
		local body = s[7]
		
		for i = 1, #body
		do
			_compile_statement(body[i])
		end
	elseif op == "struct"
	then
		local body = s[6]
		local var_decl = s[8]
		
		for i = 1, #body
		do
			_compile_statement(body[i])
		end
		
		if var_decl ~= nil
		then
			local var_params = var_decl[2]
			local var_array_size = var_decl[3]
			
			for _, vp in ipairs(var_params)
			do
				_compile_expr(vp)
			end
			
			if var_array_size ~= nil then _compile_expr(var_array_size) end
		end
	elseif op == "enum"
	then
		local members = s[6]
		
		for i = 1, #members
		do
			local value_expr = members[i][2]
			
			if value_expr ~= nil
			then
				_compile_expr(value_expr)
			end
		end
	elseif op == "local-variable"
	then
		local arguments = s[6]
		local array_size = s[7]
		local init_val = s[8]
		
		if arguments ~= nil
		then
			for i = 1, #arguments
			do
				_compile_expr(arguments[i])
			end
		end
		
		if array_size then _compile_expr(array_size) end
		if init_val   then _compile_expr(init_val)   end
	elseif op == "variable"
	then
		local arguments = s[6]
		local array_size = s[7]
		
		if arguments ~= nil
		then
			for i = 1, #arguments
			do
				_compile_expr(arguments[i])
			end
		end
		
		if array_size then _compile_expr(array_size) end
	elseif op == "typedef"
	then
		local array_size = s[6]
		if array_size then _compile_expr(array_size) end
	elseif op == "for"
	then
		local init_expr = s[4]
		local cond_expr = s[5]
		local iter_expr = s[6]
		local body      = s[7]
		
		if init_expr then _compile_statement(init_expr) end
		if cond_expr then _compile_expr(cond_expr) end
		if iter_expr then _compile_expr(iter_expr) end
		
		for i = 1, #body
		do
			_compile_statement(body[i])
		end
	elseif op == "switch"
	then
		local expr  = s[4]
		local cases = s[5]
		
		_compile_expr(expr)
		
		for _, case in ipairs(cases)
		do
			local case_expr = case[1]
			local case_body = case[2]
			
			if case_expr ~= nil then _compile_expr(case_expr) end
			
			for _, statement in ipairs(case_body)
			do
				_compile_statement(statement)
			end
		end
	elseif op == "if"
	then
		for i = 4, #s
		do
			local branch = s[i]
			
			local condition  = #branch > 1 and branch[1] or nil
			local statements = #branch > 1 and branch[2] or branch[1]
			
			if condition ~= nil
			then
				_compile_expr(condition)
			end
			
			for _, s in pairs(statements)
			do
				_compile_statement(s)
			end
		end
	end
end

local function parse_text(text)
	build_lines_table(text)
	
	local ast = _parser:match(text)
	
	for i,v in ipairs(ast)
	do
		_compile_statement(v)
	end
	
	return ast
end

M.parse_text = parse_text;

-- local inspect = require 'inspect'
-- print(inspect(M.parser:match(io.input():read("*all"))));

return M
