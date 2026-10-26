#!/bin/bash
# gen_gl_thunks.sh <names.txt> <out.S>: one x86-64 thunk per GL function name. The native launcher UI (kit/*.cpp) calls ~60 gl* functions directly; with the GL SDK linked next to Zink those
# symbols would always be the SDK's. Each thunk resolves the function of the ACTIVE renderer on its first call (ps5_gl_resolve in ps5lwjgl.c: SDK or Zink through eglGetProcAddress),
# keeps the pointer and tail-jumps to it. All argument registers (integer and the 8 SSE ones) are preserved around the resolver call.
set -e
names=$1; out=$2
{
echo '    .text'
while read -r n; do
  [ -n "$n" ] || continue
  cat <<ASM
    .globl $n
    .type $n,@function
$n:
    movq .Lgp_$n(%rip), %r11
    testq %r11, %r11
    jz .Lres_$n
    jmp *%r11
.Lres_$n:
    pushq %rbp
    movq %rsp, %rbp
    subq \$0xd0, %rsp
    movq %rdi, 0(%rsp)
    movq %rsi, 8(%rsp)
    movq %rdx, 16(%rsp)
    movq %rcx, 24(%rsp)
    movq %r8, 32(%rsp)
    movq %r9, 40(%rsp)
    movq %rax, 48(%rsp)
    movaps %xmm0, 64(%rsp)
    movaps %xmm1, 80(%rsp)
    movaps %xmm2, 96(%rsp)
    movaps %xmm3, 112(%rsp)
    movaps %xmm4, 128(%rsp)
    movaps %xmm5, 144(%rsp)
    movaps %xmm6, 160(%rsp)
    movaps %xmm7, 176(%rsp)
    leaq .Lnm_$n(%rip), %rdi
    call ps5_gl_resolve
    movq %rax, .Lgp_$n(%rip)
    movq %rax, %r11
    movq 0(%rsp), %rdi
    movq 8(%rsp), %rsi
    movq 16(%rsp), %rdx
    movq 24(%rsp), %rcx
    movq 32(%rsp), %r8
    movq 40(%rsp), %r9
    movq 48(%rsp), %rax
    movaps 64(%rsp), %xmm0
    movaps 80(%rsp), %xmm1
    movaps 96(%rsp), %xmm2
    movaps 112(%rsp), %xmm3
    movaps 128(%rsp), %xmm4
    movaps 144(%rsp), %xmm5
    movaps 160(%rsp), %xmm6
    movaps 176(%rsp), %xmm7
    movq %rbp, %rsp
    popq %rbp
    jmp *%r11
    .size $n, .-$n
ASM
done < "$names"
echo '    .section .rodata'
while read -r n; do [ -n "$n" ] && printf '.Lnm_%s: .asciz "%s"\n' "$n" "$n"; done < "$names"
echo '    .bss'
echo '    .balign 8'
while read -r n; do [ -n "$n" ] && printf '.Lgp_%s: .zero 8\n' "$n"; done < "$names"
echo '    .section .note.GNU-stack,"",@progbits'
} > "$out"
