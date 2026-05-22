/*
 *  Copyright (C) 2002-2025  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/* Agent control channel — event emitters and format helpers.
 * Iteration 1: skeleton only. AGENT_EmitBpHit / AGENT_EmitLog land here. */

#include "config.h"

#if C_DEBUG

#include "agent.h"

void AGENT_EmitBpHit(uint16_t /*seg*/, uint32_t /*off*/, int /*bp_index*/)
{
}

void AGENT_EmitLog(const char * /*line*/)
{
}

#endif /* C_DEBUG */
