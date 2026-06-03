; bptest.asm  --  DOSBox-X agent live-fire test (Plan § 5.2).
;
; Exercises the CPU-core code paths touched by the agent fixes shipped in
; commits 2ac9b58 (BP activation on Add), c50d109 (Trap_Run skips DBINT_STEP
; when the inner core surrenders to the debugger) and d929a00 (farcall.watch
; / farcall.transfer).  None of those paths are reached by the GTest suite,
; which bypasses the CPU core entirely.
;
; The program waits for an ASCII keystroke and dispatches a scenario per byte:
;
;   '1'  -- Scenario 1: tight LOOP of NOPs at landmark2.  Driver sets BP at
;          landmark2 mid-run; before commit 2ac9b58 the BP stayed inactive
;          and never fired.
;   '2'  -- Scenario 2: illegal opcode "FFh FCh" -> CPU_Exception(6) ->
;          dispatches INT 6.  Driver sets BPINT 06 mid-run.
;   '3'  -- Scenario 3: MOV byte ptr ds:[0500h], 0AAh.  Driver sets a BPM
;          watch at DS:0500h mid-run.
;   '4'  -- Scenario 4: PUSHF / set TF / POPF, then NOP at landmark_tf_next.
;          With TF set, a successful BP at the NOP would (without fix
;          c50d109) be clobbered by Trap_Run firing DBINT_STEP -> INT 1.
;          With the fix, regs.get returns CS:IP at the NOP, not at the
;          INT 1 vector.
;   '5'  -- Scenario 5: CALL FAR DWORD PTR ds:[farptr].  Target is 9000:0010,
;          where we plant a single RETF byte at setup.  Driver runs
;          farcall.watch target_seg=9000h and expects a farcall.transfer
;          event with kind="call_far_indirect", target_off=0010h.
;   'q'  -- Restore INT 6 vector and exit cleanly via INT 21 AH=4Ch.
;
; A landmarks table at fixed offset 103h publishes the volatile offsets so
; the Python driver does not have to be re-tuned every time the source
; layout shifts.  The driver reads the table via mem.read once at startup.
;
; Assemble (TASM 1.0 / TLINK 2.0):
;   tasm bptest.asm
;   tlink /t bptest.obj , bptest.com
;
; Tested with the TASM 1.0 build pinned under %TASM% (1988-vintage Borland
; toolchain).

code    segment
        assume  cs:code, ds:code, es:code, ss:code

        org     100h

start:  jmp     near ptr setup          ; 3 bytes -> table at 103h

; ---- Landmarks table -- offset 103h ---------------------------------------
; Each entry is a 16-bit offset within this segment.  The driver reads six
; bytes here and uses them to set BPs / verify farcall.transfer fields.

landmarks_table:
        dw      landmark2               ; +0: BP target for scenario 1
        dw      landmark_tf_next        ; +2: BP target for scenario 4
        dw      landmark_callfar        ; +4: from_ip expected in scenario 5

; ---- Variables -----------------------------------------------------------

oldint6_off     dw      0
oldint6_seg     dw      0
int6_count      dw      0

; Far pointer for scenario 5 -- CALL FAR DWORD PTR ds:[farptr].  We plant a
; RETF at 9000:0010 in setup so the call returns cleanly.

farptr          dw      0010h           ; offset
                dw      9000h           ; segment

; ---- Setup ---------------------------------------------------------------

setup:
        push    cs
        pop     ds

        ; Publish a signature at 0000:04F0 (the IBM-documented 16-byte
        ; "intra-application communication area" at linear 4F0h..4FFh).
        ; The Python driver polls this address for the magic word and uses
        ; the CS that follows to compute breakpoint addresses.
        xor     ax, ax
        mov     es, ax
        mov     word ptr es:[04F0h], 0BEEFh
        mov     ax, cs
        mov     word ptr es:[04F2h], ax

        push    cs
        pop     es

        mov     ax, 3506h               ; AH=35h Get INT vector
        int     21h                     ; -> ES:BX = current INT 6 vector
        mov     word ptr [oldint6_off], bx
        mov     ax, es
        mov     word ptr [oldint6_seg], ax

        push    cs                      ; restore DS = CS for AH=25h
        pop     ds
        mov     dx, offset my_int6
        mov     ax, 2506h               ; AH=25h Set INT vector
        int     21h

        mov     ax, 9000h               ; plant RETF at 9000:0010
        mov     es, ax
        mov     byte ptr es:[0010h], 0CBh

        push    cs                      ; restore segs
        pop     ds
        push    cs
        pop     es

; ---- Main dispatch loop --------------------------------------------------

main_loop:
        mov     ah, 0
        int     16h                     ; AL = ASCII, AH = scancode
        cmp     al, '1'
        je      scenario1
        cmp     al, '2'
        je      scenario2
        cmp     al, '3'
        je      scenario3
        cmp     al, '4'
        je      scenario4
        cmp     al, '5'
        je      scenario5
        cmp     al, 'q'
        je      do_exit
        jmp     main_loop

; ---- Scenario 1: tight NOP loop --------------------------------------------

scenario1:
        mov     cx, 100
scenario1_loop:
landmark2:
        nop                             ; <- BP target
        loop    scenario1_loop
        jmp     main_loop

; ---- Scenario 2: illegal opcode FFh,FCh -> INT 6 ----------------------------

scenario2:
landmark4:
        db      0FFh, 0FCh              ; GRP5 /7 with reg operand -> #UD
        jmp     main_loop

; ---- Scenario 3: BPM target write ------------------------------------------

scenario3:
        mov     byte ptr ds:[0500h], 0AAh
        jmp     main_loop

; ---- Scenario 4: TF set + NOP ----------------------------------------------

scenario4:
        pushf
        pop     ax
        or      ax, 0100h               ; set TF
        push    ax
        popf                            ; TF=1 takes effect for next insn

landmark_tf_next:
        nop                             ; <- BP target

        pushf                           ; clear TF (multiple traps will
        pop     ax                      ; fire here; each IRET is benign)
        and     ax, 0FEFFh
        push    ax
        popf
        jmp     main_loop

; ---- Scenario 5: CALL FAR DWORD PTR ----------------------------------------

scenario5:
landmark_callfar:
        call    dword ptr ds:[farptr]   ; -> 9000:0010 (RETF) -> here
        jmp     main_loop

; ---- Clean exit ----------------------------------------------------------

do_exit:
        push    cs
        pop     ds
        mov     dx, word ptr [oldint6_off]
        mov     ax, word ptr [oldint6_seg]
        mov     ds, ax                  ; DS:DX = original INT 6 vector
        mov     ax, 2506h
        int     21h

        mov     ax, 4C00h
        int     21h

; ---- INT 6 handler -------------------------------------------------------
; Stack layout on entry (16-bit real mode):
;   [SP+0]=IP, [SP+2]=CS, [SP+4]=FLAGS.
; The IP saved on the stack still points at the offending FFh,FCh sequence
; (the CPU did not advance it).  We skip 2 bytes so re-execution proceeds
; past the trap byte pair.

my_int6:
        push    bp
        mov     bp, sp                  ; [BP+0]=oldbp, [BP+2]=IP, ...
        add     word ptr [bp+2], 2
        inc     word ptr cs:[int6_count]
        pop     bp
        iret

code    ends
        end     start
