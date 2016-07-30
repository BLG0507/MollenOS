/* The Macia Language (MACIA)
*
* Copyright 2016, Philip Meulengracht
*
* This program is free software : you can redistribute it and / or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation ? , either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.If not, see <http://www.gnu.org/licenses/>.
*
*
* Macia - IL Opcodes
*/
#pragma once

typedef enum {

	/* Unknown 
	 * Opcode 0 is noop */
	OpNone			= 0x00,

	/* Special Functions */
	OpLabel,					//(5) label #id
	OpReturn,					//(1) return

	/* Store Opcodes */
	OpStore,					//store #id, [val]
	OpStoreI,					//(9) storei #id, [val]
	OpStoreRI,					//(5) storeir $, [val]

	/* Load Opcodes */
	OpLoadA,					//(9) load #target_id, #source_id
	OpLoadRA,					//(5) load $, #source_id


	/* Arithmetics */
	OpAddRA,					//(6) addra #id, $
	OpDivRA,					//(6) divra #id, $
	OpSubRA,					//(6) subra #id, $
	OpRemRA,					//(6) remra $
	OpMulRA						//(6) mulra #id, $

} Opcode_t;