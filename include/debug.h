/*
 *  Copyright (C) 2002-2021  The DOSBox Team
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

void DEBUG_SetupConsole(void);
void DEBUG_DrawScreen(void);
bool DEBUG_Breakpoint(void);
bool DEBUG_IntBreakpoint(uint8_t intNum);
void DEBUG_Enable(bool pressed);
void DEBUG_CheckExecuteBreakpoint(uint16_t seg, uint32_t off);
bool DEBUG_ExitLoop(void);
void DEBUG_RefreshPage(char scroll);
Bitu DEBUG_EnableDebugger(void);

extern Bitu cycle_count;
extern Bitu debugCallback;

uint16_t DEBUG_GetPrevCS(void);
uint16_t DEBUG_GetPrevIP(void);

/* ---- Agent observability bridge -----------------------------------------
 * Thin C-style entry points the agent subsystem uses to read debugger
 * internals (the CBreakpoint list is file-local to debug.cpp) without
 * coupling the agent translation units to the debugger's STL containers.
 * All are no-ops / empty when there is nothing to report and are only
 * meaningful in a C_DEBUG build. */

/* debug.h has no overall include guard (it is intentionally re-includable —
 * it is only function prototypes and externs, which are idempotent). The
 * agent-observability additions below are the first *type* definitions in
 * this header, so they need their own guard against double inclusion. */
#ifndef DOSBOX_DEBUG_AGENT_TYPES
#define DOSBOX_DEBUG_AGENT_TYPES

/* Kind codes reported in AgentBreakpointInfo so the agent does not have to
 * know the internal EBreakpoint enum values. */
enum AgentBpKind {
    AGENT_BPKIND_EXEC   = 0,   /* BKPNT_PHYSICAL                         */
    AGENT_BPKIND_INT    = 1,   /* BKPNT_INTERRUPT                        */
    AGENT_BPKIND_MEM    = 2,   /* BKPNT_MEMORY*                          */
    AGENT_BPKIND_OTHER  = 3
};

struct AgentBreakpointInfo {
    uint32_t  id;       /* stable handle (bp.add/list/del) — survives reorder */
    int       index;    /* BPoints iteration order — matches bp.hit bp_index */
    int       kind;     /* one of AgentBpKind                                */
    uint16_t  seg;      /* segment for EXEC/MEM kinds (0 for INT)            */
    uint32_t  off;      /* offset  for EXEC/MEM kinds (0 for INT)            */
    uint32_t  linear;   /* GetAddress(seg,off) — for the bytes_now read      */
    uint8_t   intnr;    /* interrupt number for INT kind                     */
    bool      enabled;  /* IsActive()                                        */
    uint64_t  hits;     /* monotonic hit counter                            */
};

#endif /* DOSBOX_DEBUG_AGENT_TYPES */

/* Invoke `cb` once per breakpoint, in BPoints iteration order. */
void DEBUG_AgentForEachBreakpoint(
    void (*cb)(void *ctx, const AgentBreakpointInfo *info), void *ctx);

/* Typed breakpoint add/delete with stable handles (bp.add / bp.del). Each
 * CBreakpoint carries a monotonic id assigned at construction, so a handle
 * stays valid as other breakpoints come and go (unlike the BPoints iteration
 * index that bp.hit's bp_index and BPDEL use). The exec/int adders return the
 * new breakpoint's id (>0), 0 on failure. For an interrupt breakpoint pass
 * ah/al < 0 to match any AH/AL (the BPINT "all" wildcard). DeleteById removes
 * one breakpoint by handle (false if no such id); DeleteAll removes every
 * breakpoint and returns how many were removed. */
uint32_t DEBUG_AgentAddExecBreakpoint(uint16_t seg, uint32_t off);
uint32_t DEBUG_AgentAddIntBreakpoint(uint8_t intnr, int ah, int al);
bool     DEBUG_AgentDeleteBreakpointById(uint32_t id);
size_t   DEBUG_AgentDeleteAllBreakpoints(void);

/* Disassemble one instruction at guest seg:off into `text` (NUL-terminated,
 * truncated to textsz). Returns the instruction length in bytes (0 if
 * textsz==0). Honours the current code-segment operand size. */
int DEBUG_AgentDisasmOne(uint16_t seg, uint32_t off, char *text, size_t textsz);

/* Single-step the guest from the agent dispatch. `over`
 * false = trace into (one instruction); true = step over CALL/INT/LOOP/REP.
 * Returns 0 if the CPU is not paused, 1 if it stepped one instruction and is
 * still paused (read regs now), 2 if it launched an asynchronous step-over of
 * a CALL/INT/LOOP/REP (the pause arrives later, via AGENT_OnDebuggerPaused). */
int DEBUG_AgentStep(bool over);

/* True when the guest CPU is paused in the debugger (the agent dispatch is then
 * running at a safe point between instructions). state.save / state.restore
 * gate on this: replacing the whole machine state mid-
 * instruction would corrupt the emulator, and a well-defined snapshot needs
 * settled registers. Reports the same `debugging` flag DEBUG_AgentStep checks. */
bool DEBUG_AgentIsPaused(void);

#ifdef C_HEAVY_DEBUG
bool DEBUG_HeavyIsBreakpoint(void);
void DEBUG_HeavyWriteLogInstruction(void);
#endif
