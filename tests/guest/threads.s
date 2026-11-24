@ SPDX-License-Identifier: GPL-2.0-or-later
@ Synthetic ARM32 guest programs for tests/thread_tests.cpp. Regenerate thread_guest.inc with
@ tests/guest/generate.sh after editing. Imports are called through fixed trap addresses.
.syntax unified
.arm
.text
.equ T_CREATE, 0xf0000000
.equ T_JOIN, 0xf0000010
.equ T_EXIT, 0xf0000020
.equ T_SEM_INIT, 0xf0000030
.equ T_SEM_WAIT, 0xf0000040
.equ T_SEM_POST, 0xf0000050
.equ T_MUTEX_INIT, 0xf0000060
.equ T_MUTEX_LOCK, 0xf0000070
.equ T_MUTEX_UNLOCK, 0xf0000080
.equ T_COND_INIT, 0xf0000090
.equ T_COND_WAIT, 0xf00000a0
.equ T_COND_SIGNAL, 0xf00000b0
.equ T_COND_TIMEDWAIT, 0xf00000c0
.equ T_KEY_CREATE, 0xf00000d0
.equ T_SETSPECIFIC, 0xf00000e0
.equ T_GETSPECIFIC, 0xf00000f0
.equ T_USLEEP, 0xf0000100
.equ T_YIELD, 0xf0000110
.equ T_SELF, 0xf0000120
.equ T_SEM_TIMEDWAIT, 0xf0000130
.equ T_COND_BROADCAST, 0xf0000140
.equ DATA, 0x4000
.equ D_COUNTER, 0
.equ D_FLAG, 4
.equ D_HANDLE, 8
.equ D_RET, 12
.equ D_SEM, 16
.equ D_MUTEX, 32
.equ D_COND, 36
.equ D_KEY, 40
.equ D_OUT0, 48
.equ D_OUT1, 52
.equ D_OUT2, 56
.equ D_HANDLE2, 60
.equ D_TS, 64

@ r0 = data. The worker posts a semaphore five times; main waits five times, then joins.
.globl t_sem_workers
t_sem_workers:
    push {r4-r8, lr}
    mov r4, r0
    add r0, r4, #D_SEM
    mov r1, #0
    mov r2, #0
    ldr r6, =T_SEM_INIT
    blx r6
    add r0, r4, #D_HANDLE
    mov r1, #0
    adr r2, w_sem
    mov r3, r4
    ldr r6, =T_CREATE
    blx r6
    mov r5, #0
1:  add r0, r4, #D_SEM
    ldr r6, =T_SEM_WAIT
    blx r6
    add r5, r5, #1
    cmp r5, #5
    blt 1b
    ldr r0, [r4, #D_HANDLE]
    add r1, r4, #D_RET
    ldr r6, =T_JOIN
    blx r6
    ldr r0, [r4, #D_COUNTER]
    pop {r4-r8, pc}
    .ltorg
w_sem:
    push {r4-r7, lr}
    mov r4, r0
    mov r5, #0
1:  ldr r0, [r4, #D_COUNTER]
    add r0, r0, #1
    str r0, [r4, #D_COUNTER]
    add r0, r4, #D_SEM
    ldr r6, =T_SEM_POST
    blx r6
    add r5, r5, #1
    cmp r5, #5
    blt 1b
    mov r0, #42
    pop {r4-r7, pc}
    .ltorg

@ Main spins on a flag only a second thread can set: needs instruction-quantum preemption.
.globl t_spin
t_spin:
    push {r4, r6, lr}
    mov r4, r0
    add r0, r4, #D_HANDLE
    mov r1, #0
    adr r2, w_flag
    mov r3, r4
    ldr r6, =T_CREATE
    blx r6
1:  ldr r0, [r4, #D_FLAG]
    cmp r0, #0
    beq 1b
    ldr r0, [r4, #D_HANDLE]
    mov r1, #0
    ldr r6, =T_JOIN
    blx r6
    ldr r0, [r4, #D_FLAG]
    pop {r4, r6, pc}
    .ltorg
w_flag:
    mov r1, #1
    str r1, [r0, #D_FLAG]
    mov r0, #0
    bx lr

@ Condition variable handshake: main waits for a flag the worker sets under the mutex.
.globl t_cond
t_cond:
    push {r4-r6, lr}
    mov r4, r0
    add r0, r4, #D_MUTEX
    mov r1, #0
    ldr r6, =T_MUTEX_INIT
    blx r6
    add r0, r4, #D_COND
    mov r1, #0
    ldr r6, =T_COND_INIT
    blx r6
    add r0, r4, #D_MUTEX
    ldr r6, =T_MUTEX_LOCK
    blx r6
    add r0, r4, #D_HANDLE
    mov r1, #0
    adr r2, w_cond
    mov r3, r4
    ldr r6, =T_CREATE
    blx r6
1:  ldr r0, [r4, #D_FLAG]
    cmp r0, #0
    bne 2f
    add r0, r4, #D_COND
    add r1, r4, #D_MUTEX
    ldr r6, =T_COND_WAIT
    blx r6
    b 1b
2:  add r0, r4, #D_MUTEX
    ldr r6, =T_MUTEX_UNLOCK
    blx r6
    ldr r0, [r4, #D_HANDLE]
    add r1, r4, #D_RET
    ldr r6, =T_JOIN
    blx r6
    ldr r0, [r4, #D_FLAG]
    pop {r4-r6, pc}
    .ltorg
w_cond:
    push {r4, r6, lr}
    mov r4, r0
    add r0, r4, #D_MUTEX
    ldr r6, =T_MUTEX_LOCK
    blx r6
    mov r1, #1
    str r1, [r4, #D_FLAG]
    add r0, r4, #D_COND
    ldr r6, =T_COND_SIGNAL
    blx r6
    add r0, r4, #D_MUTEX
    ldr r6, =T_MUTEX_UNLOCK
    blx r6
    mov r0, #7
    pop {r4, r6, pc}
    .ltorg

@ Per-thread keys and exit destructors. Returns main's own value (99) after the worker ran.
.globl t_tls
t_tls:
    push {r4-r6, lr}
    mov r4, r0
    add r0, r4, #D_KEY
    adr r1, tls_dtor
    ldr r6, =T_KEY_CREATE
    blx r6
    ldr r0, [r4, #D_KEY]
    mov r1, #99
    ldr r6, =T_SETSPECIFIC
    blx r6
    add r0, r4, #D_HANDLE
    mov r1, #0
    adr r2, w_tls
    mov r3, r4
    ldr r6, =T_CREATE
    blx r6
    ldr r0, [r4, #D_HANDLE]
    add r1, r4, #D_RET
    ldr r6, =T_JOIN
    blx r6
    ldr r0, [r4, #D_KEY]
    ldr r6, =T_GETSPECIFIC
    blx r6
    pop {r4-r6, pc}
    .ltorg
w_tls:
    push {r4, r6, lr}
    mov r4, r0
    ldr r0, [r4, #D_KEY]
    ldr r6, =T_GETSPECIFIC
    blx r6
    str r0, [r4, #D_OUT0]
    ldr r0, [r4, #D_KEY]
    mov r1, #7
    ldr r6, =T_SETSPECIFIC
    blx r6
    mov r0, #3
    pop {r4, r6, pc}
    .ltorg
tls_dtor:
    ldr r1, =DATA + D_OUT1
    ldr r2, [r1]
    add r2, r2, r0
    str r2, [r1]
    bx lr
    .ltorg

@ Worker ends with pthread_exit(5); main returns the joined value.
.globl t_exit
t_exit:
    push {r4-r6, lr}
    mov r4, r0
    add r0, r4, #D_HANDLE
    mov r1, #0
    adr r2, w_exit
    mov r3, r4
    ldr r6, =T_CREATE
    blx r6
    ldr r0, [r4, #D_HANDLE]
    add r1, r4, #D_RET
    ldr r6, =T_JOIN
    blx r6
    ldr r0, [r4, #D_RET]
    pop {r4-r6, pc}
    .ltorg
w_exit:
    mov r0, #5
    ldr r6, =T_EXIT
    blx r6
    b .
    .ltorg

@ usleep(r1) then return.
.globl t_sleep
t_sleep:
    push {r6, lr}
    mov r0, r1
    ldr r6, =T_USLEEP
    blx r6
    mov r0, #1
    pop {r6, pc}
    .ltorg

@ Timed condition wait that nobody signals: returns the result (ETIMEDOUT) with the mutex held again.
.globl t_timedwait
t_timedwait:
    push {r4-r6, lr}
    mov r4, r0
    add r0, r4, #D_MUTEX
    mov r1, #0
    ldr r6, =T_MUTEX_INIT
    blx r6
    add r0, r4, #D_COND
    mov r1, #0
    ldr r6, =T_COND_INIT
    blx r6
    add r0, r4, #D_MUTEX
    ldr r6, =T_MUTEX_LOCK
    blx r6
    add r0, r4, #D_COND
    add r1, r4, #D_MUTEX
    add r2, r4, #D_TS
    ldr r6, =T_COND_TIMEDWAIT
    blx r6
    mov r5, r0
    add r0, r4, #D_MUTEX
    ldr r6, =T_MUTEX_UNLOCK
    blx r6
    mov r0, r5
    pop {r4-r6, pc}
    .ltorg

@ sem_timedwait on a zero semaphore with a deadline: returns the result (-1).
.globl t_semtimed
t_semtimed:
    push {r4-r6, lr}
    mov r4, r0
    add r0, r4, #D_SEM
    mov r1, #0
    mov r2, #0
    ldr r6, =T_SEM_INIT
    blx r6
    add r0, r4, #D_SEM
    add r1, r4, #D_TS
    ldr r6, =T_SEM_TIMEDWAIT
    blx r6
    pop {r4-r6, pc}
    .ltorg

@ Main waits on a semaphore nobody posts.
.globl t_deadlock
t_deadlock:
    push {r4, r6, lr}
    mov r4, r0
    add r0, r4, #D_SEM
    mov r1, #0
    mov r2, #0
    ldr r6, =T_SEM_INIT
    blx r6
    add r0, r4, #D_SEM
    ldr r6, =T_SEM_WAIT
    blx r6
    pop {r4, r6, pc}
    .ltorg

@ Main starts a counting worker and returns at once; the worker only runs when pumped.
.globl t_start_only
t_start_only:
    push {r4, r6, lr}
    mov r4, r0
    add r0, r4, #D_HANDLE
    mov r1, #0
    adr r2, w_count
    mov r3, r4
    ldr r6, =T_CREATE
    blx r6
    pop {r4, r6, pc}
    .ltorg
w_count:
    push {r4, lr}
    mov r4, r0
    mov r1, #10
1:  ldr r2, [r4, #D_COUNTER]
    add r2, r2, #1
    str r2, [r4, #D_COUNTER]
    subs r1, r1, #1
    bne 1b
    mov r0, #11
    pop {r4, pc}
.globl t_join_only
t_join_only:
    push {r4, r6, lr}
    mov r4, r0
    ldr r0, [r4, #D_HANDLE]
    add r1, r4, #D_RET
    ldr r6, =T_JOIN
    blx r6
    ldr r0, [r4, #D_RET]
    pop {r4, r6, pc}
    .ltorg

@ Two workers append their letter to a log and sched_yield between letters.
.globl t_log
t_log:
    push {r4-r6, lr}
    mov r4, r0
    add r0, r4, #D_HANDLE
    mov r1, #0
    adr r2, w_logA
    mov r3, r4
    ldr r6, =T_CREATE
    blx r6
    add r0, r4, #D_HANDLE2
    mov r1, #0
    adr r2, w_logB
    mov r3, r4
    ldr r6, =T_CREATE
    blx r6
    ldr r0, [r4, #D_HANDLE]
    mov r1, #0
    ldr r6, =T_JOIN
    blx r6
    ldr r0, [r4, #D_HANDLE2]
    mov r1, #0
    ldr r6, =T_JOIN
    blx r6
    ldr r0, [r4, #D_OUT0]
    pop {r4-r6, pc}
    .ltorg
w_logA:
    mov r1, #65
    b w_log
w_logB:
    mov r1, #66
w_log:
    push {r4-r7, lr}
    mov r4, r0
    mov r5, r1
    mov r7, #0
1:  ldr r2, [r4, #D_OUT0]
    add r3, r4, #0x100
    strb r5, [r3, r2]
    add r2, r2, #1
    str r2, [r4, #D_OUT0]
    ldr r6, =T_YIELD
    blx r6
    add r7, r7, #1
    cmp r7, #3
    blt 1b
    mov r0, #0
    pop {r4-r7, pc}
    .ltorg
