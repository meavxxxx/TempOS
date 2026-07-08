.section .multiboot2, "a"
.align 8

.set MB2_MAGIC, 0xE85250D6
.set MB2_ARCH, 0
.set MB2_HEADER_LEN, header_end - header_start
.set MB2_CHECKSUM, -(MB2_MAGIC + MB2_ARCH + MB2_HEADER_LEN)

header_start:
    .long MB2_MAGIC
    .long MB2_ARCH
    .long MB2_HEADER_LEN
    .long MB2_CHECKSUM

    /* Request a linear RGB framebuffer from Multiboot2 loaders. */
    .align 8
    .short 5
    .short 0
    .long 20
    .long 1024
    .long 768
    .long 32

    /* End tag */
    .align 8
    .short 0
    .short 0
    .long 8
header_end:

.section .note.GNU-stack, "", @progbits
