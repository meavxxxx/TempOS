#include <stddef.h>
#include <stdint.h>

#include <gnuos/fb.h>
#include <gnuos/mm.h>
#include <gnuos/multiboot2.h>
#include <gnuos/printk.h>
#include <gnuos/vmm.h>

#define FB_MMIO_BASE 0x0000000060000000ULL
#define FB_MAX_MAPPED_BYTES (32ULL * 1024ULL * 1024ULL)

typedef struct
{
    uint8_t *base;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t red_shift;
    uint8_t red_bits;
    uint8_t green_shift;
    uint8_t green_bits;
    uint8_t blue_shift;
    uint8_t blue_bits;
    uint8_t ready;
} fb_state_t;

static fb_state_t g_fb;

static uint64_t
fb_align_down (uint64_t value, uint64_t alignment)
{
    return value & ~(alignment - 1U);
}

static uint64_t
fb_align_up (uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static uint64_t
fb_clamp_u64 (uint64_t value, uint64_t max)
{
    return value > max ? max : value;
}

static uint32_t
fb_scale_channel (uint8_t value, uint8_t bits)
{
    if (bits == 0U)
        {
            return 0U;
        }
    if (bits >= 8U)
        {
            return value;
        }

    return (uint32_t)value >> (8U - bits);
}

static uint32_t
fb_pack_rgb (uint8_t red, uint8_t green, uint8_t blue)
{
    uint32_t pixel = 0U;

    pixel |= fb_scale_channel (red, g_fb.red_bits) << g_fb.red_shift;
    pixel |= fb_scale_channel (green, g_fb.green_bits) << g_fb.green_shift;
    pixel |= fb_scale_channel (blue, g_fb.blue_bits) << g_fb.blue_shift;
    return pixel;
}

static void
fb_put_pixel (uint32_t x, uint32_t y, uint32_t color)
{
    if (!g_fb.ready || x >= g_fb.width || y >= g_fb.height)
        {
            return;
        }

    uint8_t *pixel = g_fb.base + ((uint64_t)y * g_fb.pitch) + ((uint64_t)x * (g_fb.bpp / 8U));
    if (g_fb.bpp == 32U)
        {
            *(uint32_t *)(void *)pixel = color;
        }
    else if (g_fb.bpp == 24U)
        {
            pixel[0] = (uint8_t)(color & 0xFFU);
            pixel[1] = (uint8_t)((color >> 8U) & 0xFFU);
            pixel[2] = (uint8_t)((color >> 16U) & 0xFFU);
        }
    else if (g_fb.bpp == 16U)
        {
            *(uint16_t *)(void *)pixel = (uint16_t)color;
        }
}

static void
fb_fill_rect (uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color)
{
    if (!g_fb.ready || width == 0U || height == 0U)
        {
            return;
        }

    uint32_t max_x = (uint32_t)fb_clamp_u64 ((uint64_t)x + width, g_fb.width);
    uint32_t max_y = (uint32_t)fb_clamp_u64 ((uint64_t)y + height, g_fb.height);
    for (uint32_t row = y; row < max_y; row++)
        {
            for (uint32_t col = x; col < max_x; col++)
                {
                    fb_put_pixel (col, row, color);
                }
        }
}

static uint8_t
fb_font_row (char ch, uint8_t row)
{
    static const uint8_t space[7] = { 0, 0, 0, 0, 0, 0, 0 };
    static const uint8_t colon[7] = { 0, 4, 4, 0, 4, 4, 0 };
    static const uint8_t dot[7] = { 0, 0, 0, 0, 0, 12, 12 };
    static const uint8_t dash[7] = { 0, 0, 0, 31, 0, 0, 0 };
    static const uint8_t zero[7] = { 14, 17, 19, 21, 25, 17, 14 };
    static const uint8_t one[7] = { 4, 12, 4, 4, 4, 4, 14 };
    static const uint8_t two[7] = { 14, 17, 1, 2, 4, 8, 31 };
    static const uint8_t three[7] = { 30, 1, 1, 14, 1, 1, 30 };
    static const uint8_t four[7] = { 2, 6, 10, 18, 31, 2, 2 };
    static const uint8_t five[7] = { 31, 16, 30, 1, 1, 17, 14 };
    static const uint8_t six[7] = { 6, 8, 16, 30, 17, 17, 14 };
    static const uint8_t seven[7] = { 31, 1, 2, 4, 8, 8, 8 };
    static const uint8_t eight[7] = { 14, 17, 17, 14, 17, 17, 14 };
    static const uint8_t nine[7] = { 14, 17, 17, 15, 1, 2, 12 };
    static const uint8_t letters[26][7] = {
        { 14, 17, 17, 31, 17, 17, 17 }, { 30, 17, 17, 30, 17, 17, 30 },
        { 14, 17, 16, 16, 16, 17, 14 }, { 30, 17, 17, 17, 17, 17, 30 },
        { 31, 16, 16, 30, 16, 16, 31 }, { 31, 16, 16, 30, 16, 16, 16 },
        { 14, 17, 16, 23, 17, 17, 15 }, { 17, 17, 17, 31, 17, 17, 17 },
        { 14, 4, 4, 4, 4, 4, 14 },      { 7, 2, 2, 2, 18, 18, 12 },
        { 17, 18, 20, 24, 20, 18, 17 }, { 16, 16, 16, 16, 16, 16, 31 },
        { 17, 27, 21, 21, 17, 17, 17 }, { 17, 25, 21, 19, 17, 17, 17 },
        { 14, 17, 17, 17, 17, 17, 14 }, { 30, 17, 17, 30, 16, 16, 16 },
        { 14, 17, 17, 17, 21, 18, 13 }, { 30, 17, 17, 30, 20, 18, 17 },
        { 15, 16, 16, 14, 1, 1, 30 },   { 31, 4, 4, 4, 4, 4, 4 },
        { 17, 17, 17, 17, 17, 17, 14 }, { 17, 17, 17, 17, 17, 10, 4 },
        { 17, 17, 17, 21, 21, 21, 10 }, { 17, 17, 10, 4, 10, 17, 17 },
        { 17, 17, 10, 4, 4, 4, 4 },     { 31, 1, 2, 4, 8, 16, 31 },
    };
    const uint8_t *glyph = space;

    if (row >= 7U)
        {
            return 0U;
        }
    if (ch >= 'a' && ch <= 'z')
        {
            ch = (char)(ch - 'a' + 'A');
        }
    if (ch >= 'A' && ch <= 'Z')
        {
            glyph = letters[(uint8_t)(ch - 'A')];
        }
    else if (ch == '0')
        {
            glyph = zero;
        }
    else if (ch == '1')
        {
            glyph = one;
        }
    else if (ch == '2')
        {
            glyph = two;
        }
    else if (ch == '3')
        {
            glyph = three;
        }
    else if (ch == '4')
        {
            glyph = four;
        }
    else if (ch == '5')
        {
            glyph = five;
        }
    else if (ch == '6')
        {
            glyph = six;
        }
    else if (ch == '7')
        {
            glyph = seven;
        }
    else if (ch == '8')
        {
            glyph = eight;
        }
    else if (ch == '9')
        {
            glyph = nine;
        }
    else if (ch == ':')
        {
            glyph = colon;
        }
    else if (ch == '.')
        {
            glyph = dot;
        }
    else if (ch == '-')
        {
            glyph = dash;
        }

    return glyph[row];
}

static void
fb_draw_char (uint32_t x, uint32_t y, char ch, uint32_t color, uint32_t scale)
{
    if (scale == 0U)
        {
            scale = 1U;
        }

    for (uint8_t row = 0; row < 7U; row++)
        {
            uint8_t bits = fb_font_row (ch, row);
            for (uint8_t col = 0; col < 5U; col++)
                {
                    if ((bits & (uint8_t)(1U << (4U - col))) != 0U)
                        {
                            fb_fill_rect (x + ((uint32_t)col * scale), y + ((uint32_t)row * scale),
                                          scale, scale, color);
                        }
                }
        }
}

static void
fb_draw_text (uint32_t x, uint32_t y, const char *text, uint32_t color, uint32_t scale)
{
    uint32_t cursor = x;

    if (!text)
        {
            return;
        }

    while (*text != '\0')
        {
            fb_draw_char (cursor, y, *text, color, scale);
            cursor += 6U * scale;
            text++;
        }
}

int
fb_init (const multiboot2_framebuffer_info_t *info)
{
    uint64_t fb_size = 0U;
    uint64_t phys_base = 0U;
    uint64_t phys_end = 0U;
    uint64_t map_base = 0U;
    uint64_t map_end = 0U;
    uint64_t map_size = 0U;
    uint64_t offset = 0U;

    g_fb.ready = 0U;
    if (!info || info->type != MULTIBOOT2_FRAMEBUFFER_TYPE_RGB
        || (info->bpp != 16U && info->bpp != 24U && info->bpp != 32U) || info->pitch == 0U
        || info->width == 0U || info->height == 0U)
        {
            return 0;
        }

    fb_size = (uint64_t)info->pitch * info->height;
    if (fb_size == 0U || fb_size > FB_MAX_MAPPED_BYTES)
        {
            return 0;
        }

    phys_base = info->address;
    phys_end = phys_base + fb_size;
    if (phys_end < phys_base)
        {
            return 0;
        }

    map_base = fb_align_down (phys_base, MM_PAGE_SIZE);
    map_end = fb_align_up (phys_end, MM_PAGE_SIZE);
    map_size = map_end - map_base;
    offset = phys_base - map_base;

    for (uint64_t page = 0U; page < map_size; page += MM_PAGE_SIZE)
        {
            uint64_t virt = FB_MMIO_BASE + page;
            uint64_t phys = map_base + page;
            uint64_t existing_phys = 0U;
            uint64_t existing_flags = 0U;

            if (vmm_query_mapping (virt, &existing_phys, &existing_flags))
                {
                    if ((existing_phys & ~(MM_PAGE_SIZE - 1ULL)) == phys)
                        {
                            continue;
                        }
                    return 0;
                }
            if (!vmm_map_page (virt, phys, VMM_MAP_WRITABLE | VMM_MAP_NX))
                {
                    return 0;
                }
        }

    g_fb.base = (uint8_t *)(uintptr_t)(FB_MMIO_BASE + offset);
    g_fb.pitch = info->pitch;
    g_fb.width = info->width;
    g_fb.height = info->height;
    g_fb.bpp = info->bpp;
    g_fb.red_shift = info->red_field_position;
    g_fb.red_bits = info->red_mask_size;
    g_fb.green_shift = info->green_field_position;
    g_fb.green_bits = info->green_mask_size;
    g_fb.blue_shift = info->blue_field_position;
    g_fb.blue_bits = info->blue_mask_size;
    g_fb.ready = 1U;

    kprintf ("GNU OS: framebuffer GUI mapped virt=0x%X phys=0x%X size=0x%X %ux%u@%u\n",
             FB_MMIO_BASE, map_base, map_size, (uint64_t)g_fb.width, (uint64_t)g_fb.height,
             (uint64_t)g_fb.bpp);
    return 1;
}

int
fb_is_ready (void)
{
    return g_fb.ready != 0U;
}

void
fb_draw_boot_screen (void)
{
    if (!g_fb.ready)
        {
            return;
        }

    uint32_t bg = fb_pack_rgb (18, 26, 38);
    uint32_t panel = fb_pack_rgb (33, 45, 62);
    uint32_t panel_light = fb_pack_rgb (57, 77, 101);
    uint32_t green = fb_pack_rgb (66, 220, 148);
    uint32_t yellow = fb_pack_rgb (242, 190, 72);
    uint32_t text = fb_pack_rgb (236, 242, 248);
    uint32_t muted = fb_pack_rgb (151, 166, 184);
    uint32_t accent = fb_pack_rgb (92, 173, 255);
    uint32_t w = g_fb.width;
    uint32_t h = g_fb.height;
    uint32_t margin = w / 16U;
    uint32_t top = h / 8U;
    uint32_t card_w = w - (margin * 2U);
    uint32_t card_h = h / 2U;
    uint32_t scale_title = w >= 900U ? 5U : 3U;
    uint32_t scale_body = w >= 900U ? 3U : 2U;

    fb_fill_rect (0, 0, w, h, bg);
    fb_fill_rect (0, 0, w, h / 18U, fb_pack_rgb (12, 17, 25));
    fb_fill_rect (margin, top, card_w, card_h, panel);
    fb_fill_rect (margin, top, card_w, 6U, accent);
    fb_fill_rect (margin + 16U, top + 18U, 18U, 18U, green);
    fb_fill_rect (margin + 44U, top + 18U, 18U, 18U, yellow);
    fb_fill_rect (margin + 72U, top + 18U, 18U, 18U, fb_pack_rgb (255, 99, 99));
    fb_fill_rect (margin + card_w - 170U, top + 24U, 120U, 4U, panel_light);
    fb_fill_rect (margin + card_w - 170U, top + 40U, 82U, 4U, panel_light);

    fb_draw_text (margin + 32U, top + 72U, "GNU OS", text, scale_title);
    fb_draw_text (margin + 34U, top + 126U, "KERNEL GUI BOOT SCREEN", green, scale_body);
    fb_draw_text (margin + 34U, top + 168U, "FRAMEBUFFER: ONLINE", text, scale_body);
    fb_draw_text (margin + 34U, top + 204U, "INPUT: KEYBOARD READY", muted, scale_body);
    fb_draw_text (margin + 34U, top + 240U, "NEXT: MOUSE EVENTS AND USERSPACE SERVER", muted,
                  scale_body);

    uint32_t dock_y = h - (h / 7U);
    uint32_t dock_x = margin;
    uint32_t tile = h / 14U;
    if (tile < 36U)
        {
            tile = 36U;
        }
    fb_fill_rect (dock_x, dock_y, card_w, tile + 24U, fb_pack_rgb (24, 33, 46));
    fb_fill_rect (dock_x + 24U, dock_y + 12U, tile, tile, accent);
    fb_fill_rect (dock_x + 44U + tile, dock_y + 12U, tile, tile, green);
    fb_fill_rect (dock_x + 64U + (tile * 2U), dock_y + 12U, tile, tile, yellow);
    fb_draw_text (dock_x + 96U + (tile * 3U), dock_y + 22U, "STAGE 1 GUI ONLINE", text, scale_body);
}
