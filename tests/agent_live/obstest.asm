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
;   '4'  Phase 4 (mem.watch): a single, known store —
;        `mov word ptr [watch_target], WATCH_VALUE` — at landmark_store. The
;        field starts at WATCH_INITIAL, so the write transitions
;        WATCH_INITIAL -> WATCH_VALUE in one instruction. The driver arms
;        mem.watch on the field (with when new_eq=WATCH_VALUE) before tapping
;        '4', then asserts the mem.write event names the storing instruction's
;        CS:IP (== landmark_store) and reports old==WATCH_INITIAL,
;        new==WATCH_VALUE. This is the proposal-4.9.4 "name the stamp site"
;        test made concrete. Run once (the field stays WATCH_VALUE after).
;
;   '5'  Phase 5 (mem.watch on VGA framebuffer): switch to planar mode 12h,
;        store a single known byte into the A000 framebuffer at VGA_OFFSET via
;        `mov es:[di], al` at landmark_vga_store, then restore text mode. The
;        driver arms mem.watch on A000:VGA_OFFSET (size 1, when new_eq=VGA_VALUE
;        so the BIOS mode-set screen clear is filtered out) and asserts the
;        mem.write event names this instruction with new==VGA_VALUE and
;        old==null (a VGA read loads the plane latches, so old is not sampled).
;        This is the proposal-4.9.4 "VGA blind spot" closure made concrete.
;
;   '6'  Phase 6 (multi-sentinel cpu.watch_target, 4.9.5): two NEAR calls to
;        distinct targets landmark_msa / landmark_msb. The driver arms
;        cpu.watch_target with both as a set and asserts the two cpu.transfer
;        events carry which==0 then which==1, with one hit per sentinel in
;        debug.status. Proves the proposal-4.9.5 multi-sentinel `which` field.
;
;   '7'  Phase 7 (cpu.step / cpu.step_over, 4.9.7): a single known NEAR call.
;        The driver parks a BP at landmark_call, taps '7', and then: cpu.step
;        must land at landmark_sub (single-step descends into the call) while
;        cpu.step_over must land at landmark_call_ret (the call is treated as
;        one unit). Single-step register granularity is proven separately on
;        the phase-3 trace chain. Proves the proposal-4.9.7 structured step.
;
;   'q'  Exit cleanly via INT 21h AH=4Ch.
;
; All phases return to main_loop so the driver can run them in any order and
; repeat them.

LOOP_ITERS      equ     300
WATCH_INITIAL   equ     0761h           ; watch_target value at load
WATCH_VALUE     equ     0853h           ; value phase 4 stamps into watch_target
VGA_OFFSET      equ     0064h           ; phase 5 framebuffer write offset (A000:0064)
VGA_VALUE       equ     005Ah           ; byte phase 5 stores to the framebuffer

code    segment
        assume  cs:code, ds:code, es:code, ss:code
        org     100h

start:  jmp     near ptr setup          ; 3 bytes -> table at 103h

; ---- Landmarks table -- fixed offset 103h --------------------------------
; The driver reads (14) 16-bit offsets here with one mem.read at CS:0103.
landmarks_table:
        dw      landmark_loopbody       ; +0  probe / BP / hit-counter target
        dw      landmark_disasm         ; +2  cpu.disasm target (never executed)
        dw      landmark_trace_a        ; +4  trace chain start
        dw      landmark_trace_end      ; +6  trace chain end (BP target)
        dw      LOOP_ITERS              ; +8  iteration count the driver checks
        dw      landmark_disasm_end     ; +10 one-past the disasm bytes
        dw      watch_target            ; +12 mem.watch write-target field
        dw      landmark_store          ; +14 the storing instruction (phase 4)
        dw      landmark_vga_store      ; +16 the VGA storing instruction (phase 5)
        dw      landmark_msa            ; +18 multi-sentinel target A (phase 6)
        dw      landmark_msb            ; +20 multi-sentinel target B (phase 6)
        dw      landmark_call           ; +22 the NEAR call instruction (phase 7)
        dw      landmark_call_ret       ; +24 instruction after the call (step_over)
        dw      landmark_sub            ; +26 subroutine entry (step lands here)

; ---- Variables -----------------------------------------------------------
loopcount       dw      0
watch_target    dw      WATCH_INITIAL   ; phase 4 stamps WATCH_VALUE here

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
        cmp     al, '4'
        jne     chk_5
        jmp     phase4                  ; near jmp: phase4 is out of je's rel8 range
chk_5:
        cmp     al, '5'
        jne     chk_6
        jmp     phase5                  ; near jmp: phase5 is out of je's rel8 range
chk_6:
        cmp     al, '6'
        jne     chk_7
        jmp     phase6                  ; near jmp: phase6 is out of je's rel8 range
chk_7:
        cmp     al, '7'
        jne     chk_q
        jmp     phase7                  ; near jmp: phase7 is out of je's rel8 range
chk_q:
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

; ---- Phase 4: single known store (mem.watch write-intercept) -------------
; One word write transitioning watch_target from WATCH_INITIAL to WATCH_VALUE.
; landmark_store is the address of the storing instruction itself, which the
; driver compares against the mem.write event's from_cs:from_ip.
phase4:
landmark_store:
        mov     word ptr [watch_target], WATCH_VALUE
        jmp     main_loop

; ---- Phase 5: VGA framebuffer write (mem.watch VGA blind-spot, 4.9.4) -----
; Switch to planar mode 12h, store one known byte into the A000 framebuffer at
; VGA_OFFSET, then restore text mode. landmark_vga_store is the storing
; instruction itself, which the driver compares against the mem.write event's
; from_cs:from_ip. The value predicate (new_eq=VGA_VALUE) filters out the BIOS
; mode-set screen clear (which writes 0), so the one matching event is this
; store. The VGA write handler does not expose a side-effect-free read-back, so
; the event reports old==null.
phase5:
        mov     ax, 0012h               ; 640x480x16 planar (ES window at A000)
        int     10h
        mov     ax, 0A000h
        mov     es, ax
        mov     di, VGA_OFFSET
        mov     al, VGA_VALUE
landmark_vga_store:
        mov     es:[di], al             ; <- the watched framebuffer store
        mov     ax, 0003h               ; restore 80x25 colour text
        int     10h
        push    cs
        pop     es                      ; restore ES for the rest of the program
        jmp     main_loop

; ---- Phase 6: multi-sentinel cpu.watch_target (4.9.5) --------------------
; Two NEAR calls to distinct targets. The driver arms cpu.watch_target with
; BOTH entry points as a 2-sentinel set (msa first, msb second) and asserts
; the two cpu.transfer events carry which==0 (msa) then which==1 (msb), and
; that debug.status shows one hit per sentinel. The RETN landings fall on the
; instruction after each call (not watched), so only the two calls fire.
phase6:
        call    near ptr landmark_msa
        call    near ptr landmark_msb
        jmp     main_loop
landmark_msa:
        nop                             ; <- watch_target sentinel 0 (which==0)
        retn
landmark_msb:
        nop                             ; <- watch_target sentinel 1 (which==1)
        retn

; ---- Phase 7: known NEAR call (cpu.step / cpu.step_over, 4.9.7) -----------
; A single NEAR call whose instruction address (landmark_call), return address
; (landmark_call_ret) and subroutine entry (landmark_sub) are all in the
; landmark table. With a BP parked at landmark_call: cpu.step lands at
; landmark_sub (step descends into the call); cpu.step_over lands at
; landmark_call_ret (the whole call is one unit). The subroutine is a couple of
; NOPs + RETN so step-over has a real body to run through before the temp BP
; at the return address fires.
phase7:
landmark_call:
        call    near ptr landmark_sub
landmark_call_ret:
        mov     ax, 7777h
        jmp     main_loop
landmark_sub:
        nop
        nop
        retn

; ---- Clean exit ----------------------------------------------------------
do_exit:
        mov     ax, 4C00h
        int     21h

code    ends
        end     start
