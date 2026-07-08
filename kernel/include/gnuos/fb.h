#ifndef GNUOS_FB_H
#define GNUOS_FB_H

#include <stdint.h>

#include <gnuos/multiboot2.h>

int fb_init (const multiboot2_framebuffer_info_t *info);
int fb_is_ready (void);
void fb_draw_boot_screen (void);

#endif
