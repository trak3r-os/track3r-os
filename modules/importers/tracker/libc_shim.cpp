// SPDX-License-Identifier: MIT
//
// The module libc shim (OS-FLASHDIET I0): the packer refuses an undefined symbol and -ffreestanding still lets GCC emit
// calls to these, so the importer module carries them itself. The implementations are the portable functions of libc_shim.h; this file
// gives them the extern "C" / __aeabi names the compiler emits calls to, and carries the __aeabi_uldivmod trampoline below (target
// only). The division helper follows the RTABI register convention (quotient r0:r1, remainder r2:r3) through that trampoline.
// tests/module_shim_test.cpp covers libc_shim.h on the host against native 64-bit arithmetic.
#include "libc_shim.h"

extern "C" {

void* memcpy(void* dst, const void* src, std::size_t n) { return tracker_shim::memcpy(dst, src, n); }

void* memmove(void* dst, const void* src, std::size_t n) { return tracker_shim::memmove(dst, src, n); }

void* memset(void* dst, int c, std::size_t n) { return tracker_shim::memset(dst, c, n); }

int memcmp(const void* a, const void* b, std::size_t n) { return tracker_shim::memcmp(a, b, n); }

std::size_t strlen(const char* s) { return tracker_shim::strlen(s); }

int strcmp(const char* a, const char* b) { return tracker_shim::strcmp(a, b); }

char* strncpy(char* dst, const char* src, std::size_t n) { return tracker_shim::strncpy(dst, src, n); }

// The EABI block-operation names.
void __aeabi_memcpy(void* dst, const void* src, std::size_t n) { (void)memcpy(dst, src, n); }
void __aeabi_memcpy4(void* dst, const void* src, std::size_t n) { (void)memcpy(dst, src, n); }
void __aeabi_memcpy8(void* dst, const void* src, std::size_t n) { (void)memcpy(dst, src, n); }
void __aeabi_memmove(void* dst, const void* src, std::size_t n) { (void)memmove(dst, src, n); }
void __aeabi_memmove4(void* dst, const void* src, std::size_t n) { (void)memmove(dst, src, n); }
void __aeabi_memmove8(void* dst, const void* src, std::size_t n) { (void)memmove(dst, src, n); }
void __aeabi_memset(void* dst, std::size_t n, int c) { (void)memset(dst, c, n); }
void __aeabi_memset4(void* dst, std::size_t n, int c) { (void)memset(dst, c, n); }
void __aeabi_memset8(void* dst, std::size_t n, int c) { (void)memset(dst, c, n); }
void __aeabi_memclr(void* dst, std::size_t n) { (void)memset(dst, 0, n); }
void __aeabi_memclr4(void* dst, std::size_t n) { (void)memset(dst, 0, n); }
void __aeabi_memclr8(void* dst, std::size_t n) { (void)memset(dst, 0, n); }
int __aeabi_memcmp(const void* a, const void* b, std::size_t n) { return memcmp(a, b, n); }

// 64-bit helpers.
std::uint64_t __aeabi_llsl(std::uint64_t v, int s) { return tracker_shim::llsl(v, s); }

std::uint64_t __aeabi_llsr(std::uint64_t v, int s) { return tracker_shim::llsr(v, s); }

std::uint64_t __aeabi_lasr(std::uint64_t v, int s) { return tracker_shim::lasr(v, s); }

std::uint64_t __aeabi_lmul(std::uint64_t a, std::uint64_t b) { return tracker_shim::lmul(a, b); }

void udiv64_impl(std::uint64_t n, std::uint64_t d, std::uint64_t* out) { tracker_shim::udiv64_impl(n, d, out); }

}  // extern "C"

#if defined(__arm__)
// __aeabi_uldivmod, in the RTABI register convention the compiler's calls expect: numerator r0:r1, denominator r2:r3,
// quotient out r0:r1, remainder out r2:r3. A plain C function cannot return two 64-bit values in four registers, so
// this trampoline calls udiv64_impl and moves the results. Host builds do not need it (64-bit division is native there).
asm(
    ".syntax unified\n"
    ".thumb\n"
    ".thumb_func\n"
    ".global __aeabi_uldivmod\n"
    ".type __aeabi_uldivmod, %function\n"
    "__aeabi_uldivmod:\n"
    "push {r4, r5, lr}\n"       // 12 bytes; +28 below keeps sp 8-aligned at the call
    "sub sp, #28\n"
    "mov r4, r2\n"              // save d
    "mov r5, r3\n"
    "add r2, sp, #8\n"          // out buffer at sp+8 (16 bytes); [sp] holds the pointer argument
    "str r2, [sp]\n"
    "mov r2, r4\n"              // d back into r2:r3; r0:r1 still hold n
    "mov r3, r5\n"
    "bl udiv64_impl\n"          // udiv64_impl(n, d, out): r0:r1, r2:r3, [sp]
    "ldr r0, [sp, #8]\n"        // quotient
    "ldr r1, [sp, #12]\n"
    "ldr r2, [sp, #16]\n"       // remainder
    "ldr r3, [sp, #20]\n"
    "add sp, #28\n"
    "pop {r4, r5, pc}\n");
#endif
