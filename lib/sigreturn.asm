BITS 64

global mich_sigreturn

; The return trampoline the sigaction wrapper stacks for every caught
; handler. The kernel finds the saved frame under the stack pointer and
; resumes it, so the call never comes back: an unreachable trap guards the
; fall through. Number 241 is POSIX_SYSCALL_SIGRETURN in posix_abi.h.
; This lives outside syscall.asm because the capsule build scans that
; object's siblings for a sole ud2, and a second one would break the
; recovery rip generator.

section .text
mich_sigreturn:
    mov eax, 241
    syscall
    ud2

section .note.GNU-stack noalloc noexec nowrite progbits
