/* Minimal execution contexts for save states (native/runtime/savestate.c, native/sdk/os.c).
 *
 *   int  albw_ctx_save(AlbwCtx *c)                 returns 0; later returns again with the value
 *                                                  passed to albw_ctx_jump (like setjmp)
 *   void albw_ctx_jump(const AlbwCtx *c, int v)    resume at that albw_ctx_save (like longjmp)
 *   void albw_ctx_run_on(void *top, fn, arg)       call fn(arg) on another stack (fn must not return)
 *
 * Why not setjmp/longjmp: a save state jumps into a context whose stack bytes were just copied back
 * (the stack it was saved on has been used since), from a different stack; glibc's longjmp checks
 * (_FORTIFY_SOURCE __longjmp_chk) and pointer mangling are not meant for that. These save only the
 * callee-saved registers, the stack pointer and the return address; the caller of albw_ctx_save is
 * marked returns_twice (runtime.h) so the compiler keeps nothing else live in registers across it. */
#include "runtime.h"

#if defined(__i386__)
__asm__(
    ".text\n"
    ".globl albw_ctx_save\n.type albw_ctx_save,@function\nalbw_ctx_save:\n"
    "  movl 4(%esp), %eax\n"
    "  movl %ebx, 0(%eax)\n  movl %esi, 4(%eax)\n  movl %edi, 8(%eax)\n  movl %ebp, 12(%eax)\n"
    "  leal 4(%esp), %ecx\n  movl %ecx, 16(%eax)\n"      /* sp after the return */
    "  movl (%esp), %ecx\n  movl %ecx, 20(%eax)\n"       /* return address */
    "  xorl %eax, %eax\n  ret\n"
    ".globl albw_ctx_jump\n.type albw_ctx_jump,@function\nalbw_ctx_jump:\n"
    "  movl 4(%esp), %edx\n  movl 8(%esp), %eax\n"
    "  movl 0(%edx), %ebx\n  movl 4(%edx), %esi\n  movl 8(%edx), %edi\n  movl 12(%edx), %ebp\n"
    "  movl 16(%edx), %esp\n  jmp *20(%edx)\n"
    ".globl albw_ctx_run_on\n.type albw_ctx_run_on,@function\nalbw_ctx_run_on:\n"
    "  movl 4(%esp), %eax\n  movl 8(%esp), %ecx\n  movl 12(%esp), %edx\n"
    "  movl %eax, %esp\n  andl $-16, %esp\n  subl $12, %esp\n  pushl %edx\n  call *%ecx\n  ud2\n");
#elif defined(__x86_64__)
__asm__(
    ".text\n"
    ".globl albw_ctx_save\n.type albw_ctx_save,@function\nalbw_ctx_save:\n"
    "  movq %rbx, 0(%rdi)\n  movq %rbp, 8(%rdi)\n  movq %r12, 16(%rdi)\n  movq %r13, 24(%rdi)\n"
    "  movq %r14, 32(%rdi)\n  movq %r15, 40(%rdi)\n"
    "  leaq 8(%rsp), %rax\n  movq %rax, 48(%rdi)\n"
    "  movq (%rsp), %rax\n  movq %rax, 56(%rdi)\n"
    "  xorl %eax, %eax\n  ret\n"
    ".globl albw_ctx_jump\n.type albw_ctx_jump,@function\nalbw_ctx_jump:\n"
    "  movl %esi, %eax\n"
    "  movq 0(%rdi), %rbx\n  movq 8(%rdi), %rbp\n  movq 16(%rdi), %r12\n  movq 24(%rdi), %r13\n"
    "  movq 32(%rdi), %r14\n  movq 40(%rdi), %r15\n"
    "  movq 48(%rdi), %rsp\n  jmp *56(%rdi)\n"
    ".globl albw_ctx_run_on\n.type albw_ctx_run_on,@function\nalbw_ctx_run_on:\n"
    "  movq %rdi, %rsp\n  andq $-16, %rsp\n  movq %rdx, %rdi\n  call *%rsi\n  ud2\n");
#elif defined(__arm__)
/* AAPCS: callee-saved r4-r11, sp, lr (return address) and the VFP registers d8-d15 */
__asm__(
    ".text\n.arm\n.p2align 2\n"
    ".globl albw_ctx_save\n.type albw_ctx_save,%function\nalbw_ctx_save:\n"
    "  stmia r0, {r4-r11}\n  str sp, [r0, #32]\n  str lr, [r0, #36]\n"
    "  add r1, r0, #40\n  vstmia r1, {d8-d15}\n"
    "  mov r0, #0\n  bx lr\n"
    ".globl albw_ctx_jump\n.type albw_ctx_jump,%function\nalbw_ctx_jump:\n"
    "  add r2, r0, #40\n  vldmia r2, {d8-d15}\n"
    "  ldmia r0, {r4-r11}\n  ldr sp, [r0, #32]\n  ldr lr, [r0, #36]\n"
    "  mov r0, r1\n  bx lr\n"
    ".globl albw_ctx_run_on\n.type albw_ctx_run_on,%function\nalbw_ctx_run_on:\n"
    "  bic r0, r0, #7\n  mov sp, r0\n  mov r0, r2\n  blx r1\n  udf #0\n");
#else
#error "save-state contexts: i386 / x86-64 / arm only"
#endif
#if defined(__arm__)
__asm__(".section .note.GNU-stack,\"\",%progbits\n.text\n");
#else
__asm__(".section .note.GNU-stack,\"\",@progbits\n.text\n");
#endif

uintptr_t albw_ctx_sp(const AlbwCtx *c) {
#if defined(__i386__)
    return c->r[4];
#elif defined(__arm__)
    return c->r[8];
#else
    return c->r[6];
#endif
}

/* the stack-protector canary of the calling thread (TCB: i386 %gs:0x14, x86-64 %fs:0x28). Every run
 * picks a random one; frames restored from a state carry the saving run's, so a loading thread takes
 * the saved value (threads created later copy it from their creator). */
uintptr_t albw_canary_get(void) {
    uintptr_t v = 0;
#if defined(__ANDROID__)
    /* 32-bit ARM Android: one process-wide __stack_chk_guard; game and port are built with
     * -fno-stack-protector there (tools/build_decomp.py, tools/link_android.sh): nothing to carry */
#elif defined(__i386__)
    __asm__ volatile("movl %%gs:0x14, %0" : "=r"(v));
#else
    __asm__ volatile("movq %%fs:0x28, %0" : "=r"(v));
#endif
    return v;
}
void albw_canary_set(uintptr_t v) {
#if defined(__ANDROID__)
    (void)v;
#elif defined(__i386__)
    __asm__ volatile("movl %0, %%gs:0x14" : : "r"(v) : "memory");
#else
    __asm__ volatile("movq %0, %%fs:0x28" : : "r"(v) : "memory");
#endif
}
