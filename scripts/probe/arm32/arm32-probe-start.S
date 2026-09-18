.syntax unified
.text
.global _start
.type _start, %function
_start:
    mov r6, sp
    sub sp, sp, #12
    mov r4, #0
    str r4, [sp]
    str r4, [sp, #4]
    str r6, [sp, #8]
    ldr r0, =main
    ldr r1, [r6]
    add r2, r6, #4
    mov r3, #0
    bl __libc_start_main
    mov r7, #1
    svc #0
.size _start, .-_start
