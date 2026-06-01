; obstest.asm  --  DOSBox-X agent observability live-fire test (proposal 4.9).
;
; Companion to bptest.asm. Where bptest exercises the breakpoint / watch /
; farcall machinery, this program exercises the *observability & trust*
; commands added in proposal 4.9:
;
;   debug.status   (4.9.1)  hit counters on BPs and watches; bytes_now.
;   cpu.probe      (4.9.2)  non-halting execution counters.
;   cpu.trace_ring (4.9.3)  + cpu.traceback — "how did we get here?".
;   cpu.disasm     (4.9.6)  structured disassembly of known bytes.
;
; Assembled with MASM 6.11 (ML.EXE /c /AT) + Borland TLINK 2.0 (/t) into a
; tiny-model .COM, driven from tests/agent_live/test_observability.py.
; Build it with tests/agent_live/build_masm.py.
;
; The program publishes a signature + its CS at the IBM intra-application
; communication area (physical 0x4F0) exactly like bptest.asm, and a
; landmark table at a FIXED offset (0103h) so the Python driver can locate
; the volatile label offsets with a single mem.read and never needs retuning
; when the source layout shifts.
;
; Keystroke dispatch (driver sends one ASCII byte via keyboard.tap):
;
;   '1'  Phase 1 (probe / hit counters): run a known, bounded NOP loop body
;        at landmark_loopbody a fixed number of times (LOOP_ITERS). The
;        driver arms cpu.probe on landmark_loopbody before, taps '1', then
;        reads debug.status and asserts probe hits == LOOP_ITERS. A BP at the
;        same address (separate sub-phase) must report the same hit count.
;
;   '2'  Phase 2 (disasm): does nothing but spin back to the loop. The bytes
;        at landmark_disasm are a hand-chosen, known instruction sequence the
;        driver disassembles with cpu.disasm and checks against expected
;        mnemonics — it never executes them (they sit after a JMP), so they
;        are pure data-as-code for the decoder.
;
;   '3'  Phase 3 (trace ring): execute a short, distinctive straight-line
;        chain of cheap instructions ending at landmark_trace_end. The driver
;        arms cpu.trace_ring (CS-filtered) before, taps '3', pauses at a BP on
;        landmark_trace_end, and asserts cpu.traceback shows the chain in
;        order, most-recent-last.
;
;   'q'  Exit cleanly via INT 21h AH=4Ch.
;
; All phases return to main_loop so the driver can run them in any order and
; repeat them.

LOOP_ITERS      equ     300

code    segment
        assume  cs:code, ds:code, es:code, ss:code
        org     100h

start:  jmp     near ptr setup          ; 3 bytes -> table at 103h

; ---- Landmarks table -- fixed offset 103h --------------------------------
; The driver reads (8) 16-bit offsets here with one mem.read at CS:0103.
landmarks_table:
        dw      landmark_loopbody       ; +0  probe / BP / hit-counter target
        dw      landmark_disasm         ; +2  cpu.disasm target (never executed)
        dw      landmark_trace_a        ; +4  trace chain start
        dw      landmark_trace_end      ; +6  trace chain end (BP target)
        dw      LOOP_ITERS              ; +8  iteration count the driver checks
        dw      landmark_disasm_end     ; +10 one-past the disasm bytes
        dw      0                       ; +12 reserved
        dw      0                       ; +14 reserved

; ---- Variables -----------------------------------------------------------
loopcount       dw      0

; ---- Setup ---------------------------------------------------------------
setup:
        push    cs
        pop     ds
        push    cs
        pop     es

        ; Publish signature 0BEEFh + CS at physical 0x4F0 (driver polls it).
        xor     ax, ax
        mov     es, ax
        mov     word ptr es:[04F0h], 0BEEFh
        mov     ax, cs
        mov     word ptr es:[04F2h], ax

        push    cs
        pop     es

; ---- Main dispatch loop --------------------------------------------------
main_loop:
        mov     ah, 0
        int     16h                     ; AL = ASCII
        cmp     al, '1'
        je      phase1
        cmp     al, '2'
        je      phase2
        cmp     al, '3'
        je      phase3
        cmp     al, 'q'
        je      do_exit
        jmp     main_loop

; ---- Phase 1: bounded NOP loop (probe + hit counters) --------------------
; Exactly LOOP_ITERS executions of landmark_loopbody. A NOP is one byte, so a
; probe / BP at landmark_loopbody must register exactly LOOP_ITERS hits.
phase1:
        mov     cx, LOOP_ITERS
phase1_loop:
landmark_loopbody:
        nop                             ; <- probe / BP target (1 byte)
        loop    phase1_loop
        jmp     main_loop

; ---- Phase 2: disasm fodder (never executed) -----------------------------
; Jump straight over a block of known instruction bytes. The driver decodes
; them with cpu.disasm and matches mnemonics; the CPU never runs them.
phase2:
        jmp     short phase2_done
landmark_disasm:
        mov     ax, 1234h               ; B8 34 12
        add     bx, ax                  ; 03 D8
        push    bx                      ; 53
        pop     cx                      ; 59
        nop                             ; 90
        retn                            ; C3
landmark_disasm_end:
phase2_done:
        jmp     main_loop

; ---- Phase 3: distinctive straight-line chain (trace ring) ---------------
; A short run of distinct, side-effect-free instructions the driver can
; recognise in a traceback. Ends at landmark_trace_end where the driver
; parks a BP.
phase3:
landmark_trace_a:
        mov     ax, 0AAAAh
        mov     bx, 0BBBBh
        mov     dx, 0DDDDh
        xchg    ax, bx
        inc     ax
        dec     bx
landmark_trace_end:
        nop                             ; <- BP target; trace stops here
        jmp     main_loop

; ---- Clean exit ----------------------------------------------------------
do_exit:
        mov     ax, 4C00h
        int     21h

code    ends
        end     start
