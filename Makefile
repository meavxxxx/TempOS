ARCH ?= x86_64

-include config.mk

ifeq ($(ARCH),x86_64)
TARGET ?= x86_64-elf
QEMU_SYSTEM ?= qemu-system-x86_64
KERNEL_LINKER_SCRIPT := kernel/arch/x86_64/linker.ld
ARCH_C_SOURCES := kernel/arch/x86_64/serial.c
ARCH_C_SOURCES += kernel/arch/x86_64/interrupts/pic.c
ARCH_C_SOURCES += kernel/arch/x86_64/interrupts/apic.c
ARCH_C_SOURCES += kernel/arch/x86_64/acpi/acpi.c
ARCH_C_SOURCES += kernel/arch/x86_64/timer/pit.c
ARCH_C_SOURCES += kernel/arch/x86_64/mm/hardening.c
ARCH_C_SOURCES += kernel/arch/x86_64/syscall/fastpath.c
ARCH_ASM_SOURCES := kernel/arch/x86_64/boot/multiboot2_header.S \
	kernel/arch/x86_64/boot/entry.S \
	kernel/arch/x86_64/syscall/entry.S \
	kernel/arch/x86_64/sched/context_switch.S
else
$(error Unsupported ARCH "$(ARCH)". Supported now: x86_64)
endif

CC ?= $(TARGET)-gcc
OBJCOPY ?= $(TARGET)-objcopy

CSTD ?= gnu17
BUILD_DIR ?= build/$(ARCH)
ISO_DIR := $(BUILD_DIR)/iso
KERNEL_ELF := $(BUILD_DIR)/kernel.elf
ISO_IMAGE := $(BUILD_DIR)/gnuos-$(ARCH).iso
GNUOS_TARGET ?= x86_64-gnuos
USER_STARTFILES_DIR := userspace/libc/startfiles/$(ARCH)
USER_STARTFILES_SOURCES := \
	$(USER_STARTFILES_DIR)/crt0.S \
	$(USER_STARTFILES_DIR)/crti.S \
	$(USER_STARTFILES_DIR)/crtn.S
USER_STARTFILES_OBJECTS := \
	$(patsubst $(USER_STARTFILES_DIR)/%.S,$(BUILD_DIR)/$(USER_STARTFILES_DIR)/%.o,$(USER_STARTFILES_SOURCES))
USER_LDSO_DIR := userspace/libc/ldso/$(ARCH)
USER_LDSO_ASM_SOURCE := $(USER_LDSO_DIR)/ldso_start.S
USER_LDSO_ASM_OBJECT := $(BUILD_DIR)/$(USER_LDSO_ASM_SOURCE:.S=.o)
USER_LDSO_C_SOURCES := \
	userspace/libc/ldso/ldso_dlfcn.c \
	userspace/libc/ldso/ldso_elf.c \
	$(USER_LDSO_DIR)/ldso_bootstrap.c
USER_LDSO_C_OBJECTS := \
	$(patsubst userspace/libc/ldso/%.c,$(BUILD_DIR)/userspace/libc/ldso/%.o,$(USER_LDSO_C_SOURCES))
USER_LDSO_OBJECTS := $(USER_LDSO_ASM_OBJECT) $(USER_LDSO_C_OBJECTS)
USER_LDSO_ELF := $(BUILD_DIR)/userspace/libc/ldso/ld-gnuos.so.1
USER_LIBC_STUB_SOURCE := $(USER_LDSO_DIR)/libc_stub.c
USER_LIBC_STUB_OBJECT := $(BUILD_DIR)/$(USER_LDSO_DIR)/libc_stub.pic.o
USER_LIBC_SO := $(BUILD_DIR)/userspace/libc/libc.so.6
USER_SMOKE_SOURCE := userspace/init/init_minimal.c
USER_SMOKE_OBJECT := $(BUILD_DIR)/$(USER_SMOKE_SOURCE:.c=.o)
USER_SMOKE_DYNAMIC_OBJECT := $(BUILD_DIR)/userspace/init/init_minimal.dynamic.o
USER_SMOKE_ELF := $(BUILD_DIR)/userspace/init/init_minimal.elf
USER_SMOKE_DYNAMIC_ELF := $(BUILD_DIR)/userspace/init/init_minimal.dynamic.elf
USER_HEADERS_DIR := userspace/libc/include
USER_SYSROOT_DIR := $(BUILD_DIR)/sysroot/$(GNUOS_TARGET)
USER_CFLAGS := -std=$(CSTD) -O2 -g -ffreestanding -fno-stack-protector -fno-pie \
	-Wall -Wextra -Wpedantic --sysroot=$(USER_SYSROOT_DIR) -isystem $(USER_SYSROOT_DIR)/usr/include \
	-I$(USER_HEADERS_DIR)
USER_LDSO_CFLAGS := $(USER_CFLAGS) -fPIC -Iuserspace/libc/ldso
UEFI_DIR := boot/efi/$(ARCH)
UEFI_HEADERS_DIR := boot/efi/include
UEFI_STUB_SOURCE := $(UEFI_DIR)/uefi_stub.c
UEFI_STUB_OBJECT := $(BUILD_DIR)/$(UEFI_DIR)/uefi_stub.o
UEFI_KERNEL_BLOB_OBJECT := $(BUILD_DIR)/$(UEFI_DIR)/kernel_blob.o
UEFI_STUB_SO := $(BUILD_DIR)/$(UEFI_DIR)/BOOTX64.so
UEFI_STUB_EFI := $(BUILD_DIR)/$(UEFI_DIR)/BOOTX64.EFI
UEFI_ESP_DIR := $(BUILD_DIR)/efi/EFI/BOOT
UEFI_CC ?= x86_64-linux-gnu-gcc
UEFI_LD ?= x86_64-linux-gnu-ld
UEFI_OBJCOPY ?= x86_64-linux-gnu-objcopy
UEFI_CFLAGS := -std=$(CSTD) -O2 -g -ffreestanding -fno-stack-protector -fPIC -fshort-wchar \
	-mno-red-zone -maccumulate-outgoing-args -Wall -Wextra -Wpedantic -I$(UEFI_HEADERS_DIR)

COMMON_CFLAGS := -std=$(CSTD) -O2 -g -ffreestanding -fno-stack-protector -fno-pie \
	-mno-red-zone -mgeneral-regs-only -Wall -Wextra -Wpedantic -Iinclude -Ikernel/include
COMMON_LDFLAGS := -nostdlib -no-pie -Wl,--build-id=none,-n,-T,$(KERNEL_LINKER_SCRIPT)

KERNEL_C_SOURCES := \
	kernel/init/kmain.c \
	kernel/init/panic.c \
	kernel/arch/x86_64/interrupts/idt.c \
	kernel/drivers/fb.c \
	kernel/drivers/char/ps2_keyboard.c \
	kernel/drivers/pci.c \
	kernel/drivers/dma.c \
	kernel/ipc/ipc.c \
	kernel/ipc/shm.c \
	kernel/mm/pmm.c \
	kernel/mm/meminfo.c \
	kernel/mm/numa.c \
	kernel/mm/uaccess.c \
	kernel/mm/vmm.c \
	kernel/sched/sched.c \
	kernel/sched/workqueue.c \
	kernel/lib/spinlock.c \
	kernel/lib/mutex.c \
	kernel/lib/rwlock.c \
	kernel/lib/rcu.c \
	kernel/lib/printk.c \
	kernel/security/capability.c \
	kernel/security/seccomp.c \
	kernel/syscall/syscall.c \
	kernel/arch/x86_64/boot/multiboot2.c

KERNEL_OBJECTS := \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(KERNEL_C_SOURCES) $(ARCH_C_SOURCES)) \
	$(patsubst %.S,$(BUILD_DIR)/%.o,$(ARCH_ASM_SOURCES))
KERNEL_DEPS := $(KERNEL_OBJECTS:.o=.d)
USER_DEPS := $(USER_STARTFILES_OBJECTS:.o=.d) $(USER_SMOKE_OBJECT:.o=.d) \
	$(USER_SMOKE_DYNAMIC_OBJECT:.o=.d) $(USER_LDSO_ASM_OBJECT:.o=.d) \
	$(USER_LDSO_C_OBJECTS:.o=.d) $(USER_LIBC_STUB_OBJECT:.o=.d)

.PHONY: all full kernel userspace userspace-startfiles userspace-ldso userspace-libc-stub userspace-sysroot userspace-smoke userspace-smoke-static userspace-smoke-dynamic uefi-stub image iso run run-debug test check-posix docs clean

all: kernel

full: kernel userspace uefi-stub image

kernel: $(KERNEL_ELF)

$(KERNEL_ELF): $(KERNEL_OBJECTS) $(KERNEL_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(COMMON_LDFLAGS) -o $@ $(KERNEL_OBJECTS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) -MMD -MP -c $< -o $@

image iso: $(ISO_IMAGE)

$(ISO_IMAGE): $(KERNEL_ELF) boot/grub/grub.cfg
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_ELF) $(ISO_DIR)/boot/kernel.elf
	cp boot/grub/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(ISO_DIR)

run: $(ISO_IMAGE)
	$(QEMU_SYSTEM) -cdrom $(ISO_IMAGE) -serial stdio

run-debug: $(ISO_IMAGE)
	$(QEMU_SYSTEM) -cdrom $(ISO_IMAGE) -serial stdio -s -S

userspace: userspace-smoke

userspace-startfiles: $(USER_STARTFILES_OBJECTS)
	@echo "GNU OS userspace start files built:"
	@for obj in $(USER_STARTFILES_OBJECTS); do echo "  $$obj"; done

userspace-ldso: $(USER_LDSO_ELF)
	@echo "GNU OS userspace dynamic loader: $(USER_LDSO_ELF)"

userspace-libc-stub: $(USER_LIBC_SO)
	@echo "GNU OS userspace shared libc stub: $(USER_LIBC_SO)"

userspace-sysroot: userspace-startfiles userspace-ldso userspace-libc-stub
	ARCH=$(ARCH) TARGET=$(GNUOS_TARGET) BUILD_DIR=$(BUILD_DIR) SYSROOT=$(USER_SYSROOT_DIR) \
	HEADERS_DIR=$(USER_HEADERS_DIR) STARTFILES_DIR=$(BUILD_DIR)/$(USER_STARTFILES_DIR) \
	LDSO_FILE=$(USER_LDSO_ELF) LIBC_SO_FILE=$(USER_LIBC_SO) \
	bash scripts/toolchain/install-gnuos-sysroot.sh

userspace-smoke: userspace-smoke-static userspace-smoke-dynamic
	@echo "GNU OS userspace smoke ELF: $(USER_SMOKE_ELF)"
	@echo "GNU OS userspace smoke ELF (PT_INTERP): $(USER_SMOKE_DYNAMIC_ELF)"

userspace-smoke-static: userspace-sysroot $(USER_SMOKE_ELF)

userspace-smoke-dynamic: userspace-sysroot $(USER_SMOKE_DYNAMIC_ELF)

uefi-stub: $(UEFI_STUB_EFI)
	@echo "GNU OS UEFI stub: $(UEFI_STUB_EFI)"

$(UEFI_STUB_EFI): $(UEFI_STUB_SO)
	@mkdir -p $(UEFI_ESP_DIR)
	$(UEFI_OBJCOPY) --target=efi-app-x86_64 --subsystem=10 $< $@
	cp $@ $(UEFI_ESP_DIR)/BOOTX64.EFI

$(UEFI_STUB_SO): $(UEFI_STUB_OBJECT) $(UEFI_KERNEL_BLOB_OBJECT)
	@mkdir -p $(dir $@)
	$(UEFI_LD) -nostdlib -znocombreloc -shared -Bsymbolic -z noexecstack -e efi_main -o $@ $^

$(UEFI_STUB_OBJECT): $(UEFI_STUB_SOURCE)
	@mkdir -p $(dir $@)
	$(UEFI_CC) $(UEFI_CFLAGS) -MMD -MP -c $< -o $@

$(UEFI_KERNEL_BLOB_OBJECT): $(KERNEL_ELF)
	@mkdir -p $(dir $@)
	$(UEFI_OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 $< $@
	$(UEFI_OBJCOPY) \
		--redefine-sym _binary_build_$(ARCH)_kernel_elf_start=gnuos_kernel_blob_start \
		--redefine-sym _binary_build_$(ARCH)_kernel_elf_end=gnuos_kernel_blob_end \
		--redefine-sym _binary_build_$(ARCH)_kernel_elf_size=gnuos_kernel_blob_size \
		$@

$(USER_LDSO_ELF): $(USER_LDSO_OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) -nostdlib -shared -Wl,--build-id=none,-soname,ld-gnuos.so.1,-e,_start -o $@ \
		$(USER_LDSO_OBJECTS)

$(USER_LIBC_SO): $(USER_LIBC_STUB_OBJECT)
	@mkdir -p $(dir $@)
	$(CC) -nostdlib -shared -Wl,--build-id=none,-soname,libc.so.6 -o $@ \
		$(USER_LIBC_STUB_OBJECT)

$(USER_SMOKE_ELF): $(USER_SMOKE_OBJECT) userspace-sysroot
	@mkdir -p $(dir $@)
	$(CC) -nostdlib -no-pie -Wl,--build-id=none -o $@ \
		$(USER_SYSROOT_DIR)/usr/lib/crt0.o \
		$(USER_SYSROOT_DIR)/usr/lib/crti.o \
		$(USER_SMOKE_OBJECT) \
		$(USER_SYSROOT_DIR)/usr/lib/crtn.o

$(USER_SMOKE_DYNAMIC_ELF): $(USER_SMOKE_DYNAMIC_OBJECT) userspace-sysroot
	@mkdir -p $(dir $@)
	$(CC) -nostdlib -no-pie -Wl,--build-id=none,--dynamic-linker=/lib/ld-gnuos.so.1 -o $@ \
		$(USER_SYSROOT_DIR)/usr/lib/crt0.o \
		$(USER_SYSROOT_DIR)/usr/lib/crti.o \
		$(USER_SMOKE_DYNAMIC_OBJECT) \
		-L$(USER_SYSROOT_DIR)/usr/lib -Wl,-rpath-link,$(USER_SYSROOT_DIR)/usr/lib -lc \
		$(USER_SYSROOT_DIR)/usr/lib/crtn.o

$(USER_SMOKE_OBJECT): $(USER_SMOKE_SOURCE) userspace-sysroot
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(USER_SMOKE_DYNAMIC_OBJECT): $(USER_SMOKE_SOURCE) userspace-sysroot
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -DGNUOS_DYNAMIC_SMOKE -MMD -MP -c $< -o $@

$(USER_LIBC_STUB_OBJECT): $(USER_LIBC_STUB_SOURCE)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -fPIC -MMD -MP -c $< -o $@

$(BUILD_DIR)/userspace/libc/ldso/%.o: userspace/libc/ldso/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_LDSO_CFLAGS) -MMD -MP -c $< -o $@

test:
	@echo "Tests are not wired yet. Add unit/integration targets in tests/."

check-posix:
	@echo "POSIX conformance tests are not wired yet."

docs:
	@echo "Documentation generation pipeline is not wired yet."

clean:
	$(RM) -r $(BUILD_DIR)

-include $(KERNEL_DEPS)
-include $(USER_DEPS)
-include $(UEFI_STUB_OBJECT:.o=.d)
