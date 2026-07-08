#include <stddef.h>
#include <stdint.h>

#include <gnuos/capability.h>
#include <gnuos/apic.h>
#include <gnuos/acpi.h>
#include <gnuos/fb.h>
#include <gnuos/interrupts.h>
#include <gnuos/keyboard.h>
#include <gnuos/dma.h>
#include <gnuos/meminfo.h>
#include <gnuos/mm.h>
#include <gnuos/multiboot2.h>
#include <gnuos/numa.h>
#include <gnuos/ipc.h>
#include <gnuos/panic.h>
#include <gnuos/pci.h>
#include <gnuos/pic.h>
#include <gnuos/pit.h>
#include <gnuos/printk.h>
#include <gnuos/rcu.h>
#include <gnuos/sched.h>
#include <gnuos/seccomp.h>
#include <gnuos/serial.h>
#include <gnuos/shm.h>
#include <gnuos/spinlock.h>
#include <gnuos/syscall.h>
#include <gnuos/uaccess.h>
#include <gnuos/vmm.h>
#include <gnuos/x86_64_hardening.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY ((volatile uint16_t *)0xB8000)
#define VGA_LOG_COLOR 0x07
#define DMA_LEGACY_WINDOW_END 0x01000000ULL

typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} __attribute__((packed)) multiboot2_info_header_t;

static uint8_t g_vga_row;
static uint8_t g_vga_col;

extern uint8_t __kernel_start;
extern uint8_t __kernel_end;

typedef struct {
    const char *name;
    uint32_t period_ticks;
    uint64_t reads;
} demo_rcu_reader_arg_t;

typedef struct {
    const char *name;
    uint32_t period_ticks;
    uint64_t published;
    uint64_t dropped;
} demo_rcu_updater_arg_t;

typedef struct {
    rcu_head_t rcu_head;
    uint64_t generation;
    uint8_t in_use;
} demo_rcu_node_t;

typedef struct {
    const char *name;
    int channel_id;
    uint32_t period_ticks;
    uint64_t sent;
    uint64_t failed;
} demo_ipc_rendezvous_sender_arg_t;

typedef struct {
    const char *name;
    int channel_id;
    uint64_t received;
} demo_ipc_rendezvous_receiver_arg_t;

#define DEMO_RCU_POOL_SIZE 32U

static spinlock_t g_demo_rcu_pool_lock;
static demo_rcu_node_t g_demo_rcu_pool[DEMO_RCU_POOL_SIZE];
static demo_rcu_node_t *g_demo_rcu_current;

static demo_rcu_reader_arg_t g_demo_rcu_reader = {
    .name = "rcu-reader",
    .period_ticks = 20U,
    .reads = 0U,
};

static demo_rcu_updater_arg_t g_demo_rcu_updater = {
    .name = "rcu-updater",
    .period_ticks = 60U,
    .published = 0U,
    .dropped = 0U,
};

static demo_ipc_rendezvous_sender_arg_t g_demo_ipc_sender = {
    .name = "ipc-rv-sender",
    .channel_id = -1,
    .period_ticks = 50U,
    .sent = 0U,
    .failed = 0U,
};

static demo_ipc_rendezvous_receiver_arg_t g_demo_ipc_receiver = {
    .name = "ipc-rv-receiver",
    .channel_id = -1,
    .received = 0U,
};

static void demo_rcu_pool_init(void)
{
    spinlock_init(&g_demo_rcu_pool_lock);
    for (uint16_t i = 0; i < DEMO_RCU_POOL_SIZE; i++) {
        g_demo_rcu_pool[i].rcu_head.next = NULL;
        g_demo_rcu_pool[i].rcu_head.func = NULL;
        g_demo_rcu_pool[i].generation = 0;
        g_demo_rcu_pool[i].in_use = 0;
    }
    g_demo_rcu_current = NULL;
}

static demo_rcu_node_t *demo_rcu_alloc_node(void)
{
    demo_rcu_node_t *node = NULL;

    spinlock_lock(&g_demo_rcu_pool_lock);
    for (uint16_t i = 0; i < DEMO_RCU_POOL_SIZE; i++) {
        if (g_demo_rcu_pool[i].in_use == 0U) {
            g_demo_rcu_pool[i].in_use = 1U;
            node = &g_demo_rcu_pool[i];
            break;
        }
    }
    spinlock_unlock(&g_demo_rcu_pool_lock);

    return node;
}

static void demo_rcu_reclaim_callback(rcu_head_t *head)
{
    demo_rcu_node_t *node = rcu_container_of(head, demo_rcu_node_t, rcu_head);

    spinlock_lock(&g_demo_rcu_pool_lock);
    node->in_use = 0U;
    spinlock_unlock(&g_demo_rcu_pool_lock);
}

static void demo_rcu_reader_entry(void *arg)
{
    demo_rcu_reader_arg_t *reader = (demo_rcu_reader_arg_t *)arg;
    uint64_t last_tick = pit_ticks();

    for (;;) {
        while ((pit_ticks() - last_tick) < reader->period_ticks) {
            __asm__ volatile("hlt");
        }

        last_tick = pit_ticks();
        rcu_read_lock();
        demo_rcu_node_t *current = rcu_dereference(g_demo_rcu_current);
        (void)current;
        rcu_read_unlock();

        reader->reads++;

        sched_yield();
    }
}

static void demo_rcu_updater_entry(void *arg)
{
    demo_rcu_updater_arg_t *updater = (demo_rcu_updater_arg_t *)arg;
    uint64_t last_tick = pit_ticks();

    for (;;) {
        while ((pit_ticks() - last_tick) < updater->period_ticks) {
            __asm__ volatile("hlt");
        }

        last_tick = pit_ticks();
        demo_rcu_node_t *next = demo_rcu_alloc_node();
        if (!next) {
            updater->dropped++;
            sched_yield();
            continue;
        }

        next->generation = updater->published + updater->dropped + 1U;
        demo_rcu_node_t *old = rcu_dereference(g_demo_rcu_current);
        rcu_assign_pointer(g_demo_rcu_current, next);
        updater->published++;

        if (old) {
            call_rcu(&old->rcu_head, demo_rcu_reclaim_callback);
        }
        sched_yield();
    }
}

static void demo_rcu_reclaimer_entry(void *arg)
{
    (void)arg;

    for (;;) {
        (void)rcu_process_callbacks(1);
        sched_yield();
    }
}

static void demo_ipc_rendezvous_sender_entry(void *arg)
{
    demo_ipc_rendezvous_sender_arg_t *sender = (demo_ipc_rendezvous_sender_arg_t *)arg;
    uint64_t last_tick = pit_ticks();
    static const char payload[] = "rv-sync";

    for (;;) {
        while ((pit_ticks() - last_tick) < sender->period_ticks) {
            __asm__ volatile("hlt");
        }
        last_tick = pit_ticks();

        if (ipc_channel_rendezvous_send(
                sender->channel_id,
                payload,
                (uint16_t)(sizeof(payload) - 1U),
                0U) == 0) {
            sender->sent++;
            if ((sender->sent % 8U) == 0U) {
                kprintf(
                    "GNU OS: %s sent=%u failed=%u\n",
                    sender->name,
                    sender->sent,
                    sender->failed);
            }
        } else {
            sender->failed++;
        }

        sched_yield();
    }
}

static void demo_ipc_rendezvous_receiver_entry(void *arg)
{
    demo_ipc_rendezvous_receiver_arg_t *receiver = (demo_ipc_rendezvous_receiver_arg_t *)arg;
    char payload[IPC_MESSAGE_DATA_MAX + 1U];
    uint16_t size = 0U;
    uint64_t sender_tid = 0U;

    for (;;) {
        if (ipc_channel_rendezvous_recv(
                receiver->channel_id,
                payload,
                IPC_MESSAGE_DATA_MAX,
                &size,
                &sender_tid) == 0) {
            receiver->received++;
            if ((receiver->received % 8U) == 0U) {
                payload[size] = '\0';
                kprintf(
                    "GNU OS: %s received=%u sender_tid=%u msg='%s'\n",
                    receiver->name,
                    receiver->received,
                    sender_tid,
                    payload);
            }
        } else {
            sched_yield();
        }
    }
}

static uint16_t vga_entry(char c, uint8_t color)
{
    return (uint16_t)c | ((uint16_t)color << 8U);
}

static void vga_scroll_one_line(uint8_t color)
{
    for (uint16_t y = 1U; y < VGA_HEIGHT; y++) {
        for (uint16_t x = 0U; x < VGA_WIDTH; x++) {
            VGA_MEMORY[(y - 1U) * VGA_WIDTH + x] = VGA_MEMORY[y * VGA_WIDTH + x];
        }
    }

    for (uint16_t x = 0U; x < VGA_WIDTH; x++) {
        VGA_MEMORY[(VGA_HEIGHT - 1U) * VGA_WIDTH + x] = vga_entry(' ', color);
    }
}

static void vga_clear(uint8_t color)
{
    for (uint16_t y = 0; y < VGA_HEIGHT; y++) {
        for (uint16_t x = 0; x < VGA_WIDTH; x++) {
            VGA_MEMORY[y * VGA_WIDTH + x] = vga_entry(' ', color);
        }
    }

    g_vga_row = 0;
    g_vga_col = 0;
}

static void vga_putc(char c, uint8_t color)
{
    if (c == '\r') {
        return;
    }

    if (c == '\n') {
        g_vga_col = 0;
        g_vga_row++;
        if (g_vga_row >= VGA_HEIGHT) {
            vga_scroll_one_line(color);
            g_vga_row = VGA_HEIGHT - 1U;
        }
        return;
    }

    if (g_vga_row >= VGA_HEIGHT) {
        vga_scroll_one_line(color);
        g_vga_row = VGA_HEIGHT - 1U;
    }

    VGA_MEMORY[g_vga_row * VGA_WIDTH + g_vga_col] = vga_entry(c, color);
    g_vga_col++;

    if (g_vga_col >= VGA_WIDTH) {
        g_vga_col = 0;
        g_vga_row++;
        if (g_vga_row >= VGA_HEIGHT) {
            vga_scroll_one_line(color);
            g_vga_row = VGA_HEIGHT - 1U;
        }
    }
}

static void vga_write(const char *message, uint8_t color)
{
    while (*message != '\0') {
        vga_putc(*message, color);
        message++;
    }
}

static void vga_log_mirror_char(char c)
{
    vga_putc(c, VGA_LOG_COLOR);
}

static uint64_t clamp_end(uint64_t base, uint64_t size)
{
    if (base > (UINT64_MAX - size)) {
        return UINT64_MAX;
    }

    return base + size;
}

static uint64_t framebuffer_size_bytes(const multiboot2_framebuffer_info_t *framebuffer)
{
    uint64_t pitch = 0U;
    uint64_t height = 0U;

    if (!framebuffer) {
        return 0U;
    }

    pitch = framebuffer->pitch;
    height = framebuffer->height;
    if (pitch == 0U || height == 0U) {
        return 0U;
    }
    if (pitch > (UINT64_MAX / height)) {
        return 0U;
    }

    return pitch * height;
}

static void reserve_dma_low_window(uint64_t pmm_base, uint64_t pmm_size)
{
    uint64_t pmm_end = 0U;
    uint64_t reserve_size = 0U;

    if (pmm_size == 0U || pmm_base >= DMA_LEGACY_WINDOW_END) {
        return;
    }

    pmm_end = clamp_end(pmm_base, pmm_size);
    if (pmm_end > DMA_LEGACY_WINDOW_END) {
        pmm_end = DMA_LEGACY_WINDOW_END;
    }
    if (pmm_end <= pmm_base) {
        return;
    }

    reserve_size = pmm_end - pmm_base;
    pmm_reserve_range(pmm_base, reserve_size);
    kprintf(
        "GNU OS: PMM reserved DMA low window 0x%X..0x%X\n",
        pmm_base,
        pmm_end);
}

static void reserve_multiboot_info(uint64_t boot_info_addr)
{
    const multiboot2_info_header_t *header =
        (const multiboot2_info_header_t *)(uintptr_t)boot_info_addr;
    uint64_t total_size = 0U;

    if (boot_info_addr == 0U) {
        return;
    }
    if (header->total_size < sizeof(multiboot2_info_header_t)) {
        return;
    }

    total_size = header->total_size;
    pmm_reserve_range(boot_info_addr, total_size);
    kprintf(
        "GNU OS: PMM reserved multiboot2 info 0x%X..0x%X\n",
        boot_info_addr,
        clamp_end(boot_info_addr, total_size));
}

static void reserve_framebuffer_mmio(
    int have_framebuffer,
    const multiboot2_framebuffer_info_t *framebuffer)
{
    uint64_t fb_size = 0U;

    if (!have_framebuffer || !framebuffer) {
        return;
    }

    fb_size = framebuffer_size_bytes(framebuffer);
    if (fb_size == 0U) {
        return;
    }

    pmm_reserve_range(framebuffer->address, fb_size);
    kprintf(
        "GNU OS: PMM reserved framebuffer MMIO 0x%X..0x%X\n",
        framebuffer->address,
        clamp_end(framebuffer->address, fb_size));
}

static void reserve_acpi_regions(void)
{
    acpi_info_t acpi_info;

    if (!acpi_get_info(&acpi_info)) {
        return;
    }

    pmm_reserve_range(acpi_info.rsdp_address, acpi_info.rsdp_length);
    pmm_reserve_range(acpi_info.root_sdt_address, acpi_info.root_sdt_length);
    pmm_reserve_range(acpi_info.madt_address, acpi_info.madt_length);
    pmm_reserve_range(acpi_info.fadt_address, acpi_info.fadt_length);
    pmm_reserve_range(acpi_info.srat_address, acpi_info.srat_length);
    pmm_reserve_range((uint64_t)acpi_info.local_apic_address, MM_PAGE_SIZE);

    kprintf(
        "GNU OS: PMM reserved ACPI/MMIO ranges rsdp=0x%X root=0x%X madt=0x%X fadt=0x%X srat=0x%X lapic=0x%X\n",
        acpi_info.rsdp_address,
        acpi_info.root_sdt_address,
        acpi_info.madt_address,
        acpi_info.fadt_address,
        acpi_info.srat_address,
        (uint64_t)acpi_info.local_apic_address);
}

void kmain(uint64_t boot_magic, uint64_t boot_info_addr)
{
    const uint8_t color = 0x0A;
    uint64_t pmm_base = 0;
    uint64_t pmm_size = 0;
    uint64_t kernel_start = 0;
    uint64_t kernel_end = 0;
    uint64_t kernel_size = 0;
    uint64_t translated = 0;
    uint64_t test_virt = 0;
    uint64_t split_test_virt = 0x0000000000200000ULL;
    uint64_t ticks_before = 0;
    int ipc_boot_channel = -1;
    int ipc_rendezvous_channel = -1;
    char ipc_recv_buffer[IPC_MESSAGE_DATA_MAX + 1U];
    uint16_t ipc_recv_size = 0U;
    uint64_t ipc_sender_tid = 0U;
    uint16_t ipc_source_capability = 0U;
    uint16_t ipc_received_capability = 0U;
    uint16_t ipc_received_capability_rights = 0U;
    capability_info_t ipc_received_capability_info;
    int shm_demo_segment = -1;
    void *shm_writer_address = NULL;
    void *shm_reader_address = NULL;
    uint64_t shm_writer_size = 0U;
    uint64_t shm_reader_size = 0U;
    int64_t syscall_gettid_result = 0LL;
    int64_t syscall_unknown_result = 0LL;
    int64_t syscall_yield_result = 0LL;
    int64_t syscall_userptr_bad_result = 0LL;
    int64_t syscall_userptr_good_result = -1LL;
    uint64_t syscall_user_tid_value = 0U;
    int64_t syscall_seccomp_log_result = 0LL;
    int64_t syscall_seccomp_deny_result = 0LL;
    uint64_t syscall_seccomp_audit_total = 0U;
    seccomp_audit_event_t syscall_seccomp_last_event;
    int syscall_seccomp_have_last = 0;
    uint64_t syscall_selftest_passed = 0U;
    uint64_t syscall_selftest_failed = 0U;
    uint8_t syscall_fastpath_ready = 0U;
    uint64_t syscall_user_page_virt = 0x0000000080000000ULL;
    vmm_kernel_layout_t kernel_vmm_layout = {0};
    void *syscall_user_page = NULL;
    int syscall_user_page_mapped = 0;
    task_t *current_task = NULL;
    task_t *rcu_reader_task = NULL;
    task_t *rcu_updater_task = NULL;
    task_t *rcu_reclaimer_task = NULL;
    task_t *ipc_rendezvous_sender_task = NULL;
    task_t *ipc_rendezvous_receiver_task = NULL;
    int have_mmap = 0;
    int have_framebuffer = 0;
    multiboot2_framebuffer_info_t framebuffer_info = {0};

    vga_clear(0x07);
    serial_set_mirror_callback(vga_log_mirror_char);
    serial_set_mirror_enabled(1U);
    serial_init();
    x86_64_idt_init();

    if ((uint32_t)boot_magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        kpanic("invalid multiboot2 magic");
    }

    have_mmap = multiboot2_find_largest_available_region(
        boot_info_addr,
        &pmm_base,
        &pmm_size);
    have_framebuffer = multiboot2_find_framebuffer(boot_info_addr, &framebuffer_info);
    if (!have_mmap) {
        serial_write("GNU OS: no valid multiboot memory map found, using fallback.\n");
        pmm_base = 0x100000ULL;
        pmm_size = 64ULL * 1024ULL * 1024ULL;
    }
    if (have_framebuffer) {
        kprintf(
            "GNU OS: framebuffer addr=0x%X pitch=%u size=%ux%u bpp=%u type=%u rgb=%u/%u %u/%u %u/%u\n",
            framebuffer_info.address,
            (uint64_t)framebuffer_info.pitch,
            (uint64_t)framebuffer_info.width,
            (uint64_t)framebuffer_info.height,
            (uint64_t)framebuffer_info.bpp,
            (uint64_t)framebuffer_info.type,
            (uint64_t)framebuffer_info.red_field_position,
            (uint64_t)framebuffer_info.red_mask_size,
            (uint64_t)framebuffer_info.green_field_position,
            (uint64_t)framebuffer_info.green_mask_size,
            (uint64_t)framebuffer_info.blue_field_position,
            (uint64_t)framebuffer_info.blue_mask_size);
    } else {
        serial_write("GNU OS: framebuffer tag not present.\n");
    }

    pmm_init(pmm_base, pmm_size);
    kernel_start = (uint64_t)(uintptr_t)&__kernel_start;
    kernel_end = (uint64_t)(uintptr_t)&__kernel_end;
    kernel_size = kernel_end - kernel_start;
    pmm_reserve_range(kernel_start, kernel_size);
    reserve_dma_low_window(pmm_base, pmm_size);
    reserve_multiboot_info(boot_info_addr);
    reserve_framebuffer_mmio(have_framebuffer, &framebuffer_info);

    if (!vmm_init()) {
        kpanic("failed to initialize vmm");
    }
    if (have_framebuffer && fb_init(&framebuffer_info)) {
        fb_draw_boot_screen();
    } else {
        serial_write("GNU OS: framebuffer GUI unavailable, continuing with text console.\n");
    }
    x86_64_hardening_init();
    vmm_get_kernel_layout(&kernel_vmm_layout);
    kprintf(
        "GNU OS: KASLR-ready layout window=0x%X+0x%X slide=0x%X heap=0x%X..0x%X\n",
        kernel_vmm_layout.kaslr_window_base,
        kernel_vmm_layout.kaslr_window_size,
        kernel_vmm_layout.kaslr_slide,
        kernel_vmm_layout.kernel_heap_base,
        kernel_vmm_layout.kernel_heap_base + kernel_vmm_layout.kernel_heap_size);
    (void)acpi_init(boot_info_addr);
    reserve_acpi_regions();
    numa_init(pmm_base, pmm_size);
    (void)apic_init();

    sched_init();
    rcu_init();
    demo_rcu_pool_init();
    demo_rcu_node_t *initial_node = demo_rcu_alloc_node();
    if (!initial_node) {
        kpanic("failed to allocate initial rcu node");
    }
    initial_node->generation = 1U;
    rcu_assign_pointer(g_demo_rcu_current, initial_node);
    g_demo_rcu_updater.published = 1U;
    if (!sched_create_idle_task()) {
        kpanic("failed to create idle task");
    }
    rcu_reader_task =
        sched_create_kernel_task("rcu-reader", demo_rcu_reader_entry, &g_demo_rcu_reader);
    rcu_updater_task =
        sched_create_kernel_task("rcu-updater", demo_rcu_updater_entry, &g_demo_rcu_updater);
    rcu_reclaimer_task = sched_create_kernel_task("rcu-reclaimer", demo_rcu_reclaimer_entry, NULL);
    if (!rcu_reader_task || !rcu_updater_task || !rcu_reclaimer_task) {
        kpanic("failed to create rcu demo tasks");
    }

    pic_init(PIC_IRQ_BASE, (uint8_t)(PIC_IRQ_BASE + 8U));
    for (uint8_t irq = 0; irq < 16U; irq++) {
        pic_set_mask(irq);
    }
    ps2_keyboard_init();
    pci_init();
    dma_init();
    capability_init();
    shm_init();
    ipc_init();
    syscall_init();
    x86_64_syscall_fastpath_init();
    pic_clear_mask(0U);
    pic_clear_mask(1U);
    pit_init(100U);
    x86_64_interrupts_enable();
    (void)apic_timer_init(APIC_TIMER_VECTOR, 100U);

    vga_write("GNU OS kernel bootstrap\n", color);
    vga_write("Phase 1.5 in progress: scheduler bootstrap online.\n", 0x0F);

    serial_write("GNU OS: serial console initialized.\n");
    serial_write("GNU OS: kernel bootstrap reached kmain().\n");
    kprintf(
        "GNU OS: multiboot info addr: 0x%X\n"
        "GNU OS: kernel image range: 0x%X..0x%X\n"
        "GNU OS: ready tasks: %u\n",
        boot_info_addr,
        kernel_start,
        kernel_end,
        sched_ready_count());
    current_task = sched_current_task();
    if (current_task) {
        kprintf("GNU OS: current task tid: %u\n", current_task->tid);
    } else {
        serial_write("GNU OS: current task tid: (null)\n");
    }
    serial_write("GNU OS: IRQ0 timer and IRQ1 keyboard unmasked; interrupts enabled.\n");
    syscall_gettid_result = syscall_dispatch(SYS_GETTID, 0U, 0U, 0U, 0U, 0U, 0U);
    syscall_yield_result = syscall_dispatch(SYS_SCHED_YIELD, 0U, 0U, 0U, 0U, 0U, 0U);
    syscall_unknown_result = syscall_dispatch(511U, 0U, 0U, 0U, 0U, 0U, 0U);
    syscall_fastpath_ready = x86_64_syscall_fastpath_ready();
    kprintf(
        "GNU OS: syscall demo gettid=%d yield=%d unknown=%d registered=%u fastpath=%u\n",
        syscall_gettid_result,
        syscall_yield_result,
        syscall_unknown_result,
        (uint64_t)syscall_registered_count(),
        (uint64_t)syscall_fastpath_ready);
    syscall_userptr_bad_result = syscall_dispatch(
        SYS_GETTID_USER,
        (uint64_t)(uintptr_t)&syscall_user_tid_value,
        0U,
        0U,
        0U,
        0U,
        0U);
    syscall_user_page = pmm_alloc_page();
    if (syscall_user_page) {
        if (vmm_map_page(
                syscall_user_page_virt,
                (uint64_t)(uintptr_t)syscall_user_page,
                VMM_MAP_USER | VMM_MAP_WRITABLE)) {
            uint64_t user_seed = UINT64_MAX;
            syscall_user_page_mapped = 1;
            if (uaccess_copy_to_user(syscall_user_page_virt, &user_seed, sizeof(user_seed)) == 0) {
                syscall_userptr_good_result = syscall_dispatch(
                    SYS_GETTID_USER,
                    syscall_user_page_virt,
                    0U,
                    0U,
                    0U,
                    0U,
                    0U);
                if (uaccess_copy_from_user(
                        &syscall_user_tid_value,
                        syscall_user_page_virt,
                        sizeof(syscall_user_tid_value)) != 0) {
                    syscall_user_tid_value = UINT64_MAX;
                }
            } else {
                syscall_userptr_good_result = -14LL;
                syscall_user_tid_value = UINT64_MAX;
            }
        } else {
            pmm_free_page(syscall_user_page);
            syscall_user_page = NULL;
        }
    }
    kprintf(
        "GNU OS: syscall uaccess demo bad=%d good=%d user_tid=%u\n",
        syscall_userptr_bad_result,
        syscall_userptr_good_result,
        syscall_user_tid_value);
    if (syscall_user_page_mapped) {
        (void)vmm_unmap_page(syscall_user_page_virt);
        pmm_free_page(syscall_user_page);
    }
    seccomp_set_default_action(SECCOMP_ACTION_ALLOW);
    (void)seccomp_set_syscall_action(SYS_GETTID, SECCOMP_ACTION_LOG);
    (void)seccomp_set_syscall_action(SYS_SCHED_YIELD, SECCOMP_ACTION_ERRNO);
    syscall_seccomp_log_result = syscall_dispatch(SYS_GETTID, 0U, 0U, 0U, 0U, 0U, 0U);
    syscall_seccomp_deny_result = syscall_dispatch(SYS_SCHED_YIELD, 0U, 0U, 0U, 0U, 0U, 0U);
    syscall_seccomp_audit_total = seccomp_audit_count();
    syscall_seccomp_have_last = (seccomp_audit_latest(&syscall_seccomp_last_event) == 0);
    kprintf(
        "GNU OS: seccomp demo log_result=%d deny_result=%d audit_total=%u\n",
        syscall_seccomp_log_result,
        syscall_seccomp_deny_result,
        syscall_seccomp_audit_total);
    if (syscall_seccomp_have_last) {
        kprintf(
            "GNU OS: seccomp latest seq=%u tid=%u nr=%u action=%u result=%d\n",
            syscall_seccomp_last_event.sequence,
            syscall_seccomp_last_event.tid,
            syscall_seccomp_last_event.syscall_number,
            (uint64_t)syscall_seccomp_last_event.action,
            syscall_seccomp_last_event.result);
    }
    (void)seccomp_set_syscall_action(SYS_GETTID, SECCOMP_ACTION_ALLOW);
    (void)seccomp_set_syscall_action(SYS_SCHED_YIELD, SECCOMP_ACTION_ALLOW);

    if (syscall_gettid_result == 0LL) {
        syscall_selftest_passed++;
    } else {
        syscall_selftest_failed++;
    }
    if (syscall_yield_result == 0LL) {
        syscall_selftest_passed++;
    } else {
        syscall_selftest_failed++;
    }
    if (syscall_userptr_bad_result == -14LL) {
        syscall_selftest_passed++;
    } else {
        syscall_selftest_failed++;
    }
    if (syscall_userptr_good_result == 0LL && syscall_user_tid_value == 0U) {
        syscall_selftest_passed++;
    } else {
        syscall_selftest_failed++;
    }
    if (syscall_seccomp_log_result == 0LL &&
        syscall_seccomp_deny_result == -1LL &&
        syscall_seccomp_audit_total >= 2U) {
        syscall_selftest_passed++;
    } else {
        syscall_selftest_failed++;
    }
    kprintf(
        "GNU OS: syscall selftests passed=%u failed=%u\n",
        syscall_selftest_passed,
        syscall_selftest_failed);
    kprintf("GNU OS: /proc/meminfo snapshot follows:\n");
    meminfo_log_snapshot();

    ipc_boot_channel = ipc_channel_create("boot-log");
    if (ipc_boot_channel >= 0) {
        static const char ipc_boot_message[] = "ipc online";
        static const char ipc_capability_message[] = "capability online";
        static const char shm_demo_message[] = "shared memory online";

        if (ipc_channel_send(
                ipc_boot_channel,
                ipc_boot_message,
                (uint16_t)(sizeof(ipc_boot_message) - 1U),
                0U) == 0) {
            if (ipc_channel_recv(
                    ipc_boot_channel,
                    ipc_recv_buffer,
                    IPC_MESSAGE_DATA_MAX,
                    &ipc_recv_size,
                    &ipc_sender_tid) == 0) {
                ipc_recv_buffer[ipc_recv_size] = '\0';
                kprintf(
                    "GNU OS: IPC channel=boot-log message='%s' sender_tid=%u channels=%u\n",
                    ipc_recv_buffer,
                    ipc_sender_tid,
                    ipc_channel_count());
            }
        }

        if (capability_create(
                0U,
                0x43415044454D4F31ULL,
                (uint16_t)(CAP_RIGHT_READ | CAP_RIGHT_WRITE | CAP_RIGHT_TRANSFER),
                &ipc_source_capability) == 0) {
            if (ipc_channel_send_capability(
                    ipc_boot_channel,
                    ipc_capability_message,
                    (uint16_t)(sizeof(ipc_capability_message) - 1U),
                    0U,
                    ipc_source_capability,
                    CAP_RIGHT_READ) == 0) {
                if (ipc_channel_recv_capability(
                        ipc_boot_channel,
                        ipc_recv_buffer,
                        IPC_MESSAGE_DATA_MAX,
                        &ipc_recv_size,
                        &ipc_sender_tid,
                        &ipc_received_capability,
                        &ipc_received_capability_rights) == 0) {
                    ipc_recv_buffer[ipc_recv_size] = '\0';
                    if (capability_describe(
                            ipc_received_capability,
                            &ipc_received_capability_info) == 0) {
                        kprintf(
                            "GNU OS: IPC capability message='%s' src=%u dst=%u rights=0x%X object=0x%X caps=%u\n",
                            ipc_recv_buffer,
                            (uint64_t)ipc_source_capability,
                            (uint64_t)ipc_received_capability,
                            (uint64_t)ipc_received_capability_rights,
                            ipc_received_capability_info.object_id,
                            (uint64_t)capability_count());
                    }
                }
            }
        }

        shm_demo_segment = shm_create("boot-shm", MM_PAGE_SIZE, 0U);
        if (shm_demo_segment >= 0) {
            if (shm_attach(shm_demo_segment, 11U, &shm_writer_address, &shm_writer_size) == 0 &&
                shm_attach(shm_demo_segment, 22U, &shm_reader_address, &shm_reader_size) == 0 &&
                shm_writer_address &&
                shm_reader_address &&
                shm_writer_size > 1U) {
                uint64_t copy_len = (uint64_t)(sizeof(shm_demo_message) - 1U);
                if (copy_len >= shm_writer_size) {
                    copy_len = shm_writer_size - 1U;
                }

                for (uint64_t index = 0U; index < copy_len; index++) {
                    ((char *)shm_writer_address)[index] = shm_demo_message[index];
                }
                ((char *)shm_writer_address)[copy_len] = '\0';

                kprintf(
                    "GNU OS: SHM demo segment=%u writer=11 reader=22 size=0x%X data='%s' segments=%u\n",
                    (uint64_t)(uint16_t)shm_demo_segment,
                    shm_reader_size,
                    (const char *)shm_reader_address,
                    (uint64_t)shm_segment_count());

                (void)shm_detach(shm_demo_segment, 11U);
                (void)shm_detach(shm_demo_segment, 22U);
            }
        }
    }

    ipc_rendezvous_channel = ipc_channel_create("sync-rv");
    if (ipc_rendezvous_channel >= 0) {
        g_demo_ipc_sender.channel_id = ipc_rendezvous_channel;
        g_demo_ipc_receiver.channel_id = ipc_rendezvous_channel;
        ipc_rendezvous_sender_task = sched_create_kernel_task(
            "ipc-rv-sender",
            demo_ipc_rendezvous_sender_entry,
            &g_demo_ipc_sender);
        ipc_rendezvous_receiver_task = sched_create_kernel_task(
            "ipc-rv-receiver",
            demo_ipc_rendezvous_receiver_entry,
            &g_demo_ipc_receiver);

        if (ipc_rendezvous_sender_task && ipc_rendezvous_receiver_task) {
            kprintf(
                "GNU OS: IPC rendezvous demo online channel=%u\n",
                (uint64_t)(uint16_t)ipc_rendezvous_channel);
        }
    }

    void *page = pmm_alloc_page();
    if (page) {
        kprintf("GNU OS: PMM first allocated page: %p\n", page);
    } else {
        serial_write("GNU OS: PMM first allocated page: (null)\n");
    }

    void *heap_page = vmm_alloc_kernel_pages(1, VMM_MAP_WRITABLE);
    test_virt = (uint64_t)(uintptr_t)heap_page;

    if (heap_page) {
        volatile uint64_t *probe = (volatile uint64_t *)(uintptr_t)test_virt;
        *probe = 0x474E554F53564D4DULL;

        if (vmm_translate(test_virt, &translated)) {
            kprintf(
                "GNU OS: VMM allocated/mapped 0x%X -> 0x%X\n",
                test_virt,
                translated);
        } else {
            serial_write("GNU OS: VMM translate failed for test mapping.\n");
        }
    } else {
        serial_write("GNU OS: VMM allocation test failed.\n");
    }

    void *split_page = pmm_alloc_page();
    if (split_page) {
        uint64_t split_phys = (uint64_t)(uintptr_t)split_page;
        int split_ok = vmm_unmap_page(split_test_virt) &&
            vmm_map_page(split_test_virt, split_phys, VMM_MAP_WRITABLE);

        if (split_ok) {
            volatile uint64_t *probe = (volatile uint64_t *)(uintptr_t)split_test_virt;
            *probe = 0x53504C4954564D4DULL;

            if (vmm_translate(split_test_virt, &translated)) {
                kprintf(
                    "GNU OS: VMM split/remap 0x%X -> 0x%X\n",
                    split_test_virt,
                    translated);
            }
        } else {
            serial_write("GNU OS: VMM split/remap test failed.\n");
        }

        (void)vmm_unmap_page(split_test_virt);
        (void)vmm_map_page(split_test_virt, split_test_virt, VMM_MAP_WRITABLE);
        pmm_free_page(split_page);
    } else {
        serial_write("GNU OS: no free page for VMM split test.\n");
    }

    ticks_before = pit_ticks();
    while (pit_ticks() == ticks_before) {
        __asm__ volatile("hlt");
    }

    kprintf("GNU OS: timer interrupt path active, ticks=%u\n", pit_ticks());
    kprintf(
        "GNU OS: APIC timer ticks=%u current_count=0x%X\n",
        apic_timer_ticks(),
        (uint64_t)apic_timer_current_count());
    current_task = sched_current_task();
    if (current_task) {
        kprintf(
            "GNU OS: sched ticks=%u, preemptions=%u, current runtime=%u, vruntime=%u, switches=%u\n",
            sched_total_ticks(),
            sched_preempt_count(),
            current_task->runtime_ticks,
            current_task->vruntime,
            current_task->context_switches);
    }

    vga_write("Boot/runtime logs mirrored to VGA + serial.\n", 0x0A);
    serial_write("GNU OS: VGA mirror kept active for runtime logs.\n");

#if 0
    /* Optional bring-up test: should trigger #DE and halt in kpanic. */
    __asm__ volatile("xor %rdx, %rdx; div %rdx");
#endif

    for (;;) {
        x86_64_interrupts_enable();
        if (!sched_run()) {
            __asm__ volatile("hlt");
        }
    }
}
