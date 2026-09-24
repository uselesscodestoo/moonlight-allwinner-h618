#if !defined(__linux__) || !defined(__aarch64__) || !defined(__LP64__) || defined(__ILP32__)
#error "cedar54 ioctl entry requires Linux AArch64 LP64"
#endif

/* ioctl has optional arguments: forward the untouched raw x2 register without
 * reading a nonexistent C variadic argument in two-argument calls. */
    .text
    .p2align 2
    .global ioctl
    .type ioctl, %function
    .hidden k2b_cedar54_ioctl_dispatch
ioctl:
    .cfi_startproc
    hint #34 /* BTI c on supporting CPUs; a harmless hint otherwise. */
    b k2b_cedar54_ioctl_dispatch
    .cfi_endproc
    .size ioctl, .-ioctl

    .section .note.GNU-stack,"",%progbits
