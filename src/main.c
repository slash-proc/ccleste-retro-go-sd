/*
 * Celeste Classic — GWHB homebrew for Retro-Go SD.
 *
 * Memory layout (new firmware pools: less AHB, more DTCM, ITCM for code):
 *   ITCM   — hot .text (celeste + audio + blit helpers), self-copied at boot
 *   DTCM   — framebuffer + mix scratch (dtc_*)
 *   RAM_EMU — image, .rodata assets (gfx/font/tilemap/sfx), cold code, BSS
 *   AHB    — unused here (tight ~56 KiB heap; keep free for firmware)
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

#include "common.h"
#include "gw_lcd.h"
#include "gw_audio.h"
#include "rom_manager.h"
#include "odroid_system.h"
#include "appid.h"
#include "gw_malloc.h"

/* common.h already defines WIDTH/HEIGHT as the LCD size. */

#ifndef HOST_BUILD
#include "gw_core_bridge.h"
#include "stm32h7xx.h"
#else
#include "host_compat.h"
#endif

#include "celeste.h"
#include "tilemap.h"
#include "celeste_audio.h"
#include "celeste_data.h"

#define WIDTH_P8  128
#define PITCH_P8  148
#define HEIGHT_P8 128
#define CELESTE_FPS 30
#define CELESTE_AUDIO_SAMPLE_RATE 22050
#define CELESTE_AUDIO_BUFFER_LENGTH (CELESTE_AUDIO_SAMPLE_RATE / CELESTE_FPS)

#ifndef HOST_BUILD
#define ITCM_TEXT __attribute__((section(".itcm_text"), noinline))
#else
#define ITCM_TEXT
#endif

/* Extra rows: sprites may draw past the bottom; cheaper than per-pixel clips. */
#define FB_HEIGHT (HEIGHT_P8 + 20)
#define FB_BYTES  (PITCH_P8 * FB_HEIGHT)

static uint8_t *fb_celeste;
static int16_t *audioBuffer;
static bool enable_screenshake = true;

typedef struct {
    int16_t x, y;
    uint16_t w, h;
} SDL_Rect;

typedef struct SDL_Surface {
    int w, h;
    uint16_t pitch;
    void *pixels;
    SDL_Rect clip_rect;
} SDL_Surface;

static SDL_Surface screen_local;
static SDL_Surface *screen = &screen_local;
static SDL_Surface gfx_local;
static SDL_Surface *gfx = &gfx_local;
static SDL_Surface font_local;
static SDL_Surface *font = &font_local;

static void blit(void);

static uint16_t buttons_state = 0;

struct track_info {
    int8_t index;
    uint8_t fade;
    uint8_t mask;
};

struct track_info current_track = {-1, 0, 0};

#ifndef HOST_BUILD
/* Linker symbols from celeste.ld — ITCM image packed after .data in payload. */
extern uint8_t __itcm_lma_start__;
extern uint8_t __itcm_lma_end__;
extern uint8_t __itcm_vma_start__;
extern uint8_t __itcm_vma_end__;
extern uint8_t __ITCM_CORE_START__;

static void celeste_load_itcm(void)
{
    size_t n = (size_t)(&__itcm_lma_end__ - &__itcm_lma_start__);
    if (n == 0) {
        return;
    }

    itc_init();
    memcpy(&__ITCM_CORE_START__, &__itcm_lma_start__, n);

    /* Reserve the bump so later itc_* never overwrite hot code. */
    void *reserved = itc_malloc(n);
    if ((uintptr_t)reserved == 0xffffffffu || reserved != (void *)&__ITCM_CORE_START__) {
        printf("Celeste: ITCM reserve failed (%p, n=%u)\n", reserved, (unsigned)n);
    }

    SCB_CleanDCache_by_Addr((uint32_t *)&__itcm_lma_start__, (int32_t)n);
    SCB_InvalidateICache();
}
#else
static void celeste_load_itcm(void) {}
#endif

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

static bool SaveState(const char *savePathName)
{
    uint8_t *data = lcd_get_active_buffer();
    size_t size;

    Celeste_P8_save_state(data);
    size = Celeste_P8_get_state_size();

    FILE *file = fopen(savePathName, "wb");
    if (file == NULL) {
        return false;
    }

    size_t written = fwrite(data, 1, size, file);
    if (written != size) {
        fclose(file);
        return false;
    }

    written = fwrite((unsigned char *)&current_track, 1, sizeof(current_track), file);
    fclose(file);
    return written == sizeof(current_track);
}

static bool LoadState(const char *savePathName)
{
    unsigned char *data = (unsigned char *)lcd_get_active_buffer();
    size_t size = Celeste_P8_get_state_size();

    FILE *file = fopen(savePathName, "rb");
    if (file == NULL) {
        return false;
    }

    size_t nread = fread(data, 1, size, file);
    if (nread != size) {
        fclose(file);
        return false;
    }

    nread = fread((unsigned char *)&current_track, 1, sizeof(current_track), file);
    fclose(file);
    if (nread != sizeof(current_track)) {
        return false;
    }

    Celeste_P8_load_state(data);
    celeste_api_music(current_track.index, current_track.fade, current_track.mask);
    lcd_clear_active_buffer();
    return true;
}

#define RGB565(red, green, blue) \
    (((blue >> 3) & 0x1f) | (((green >> 2) & 0x3f) << 5) | (((red >> 3) & 0x1f) << 11))

static const uint16_t base_palette[16] = {
    RGB565(0x00, 0x00, 0x00),
    RGB565(0x1d, 0x2b, 0x53),
    RGB565(0x7e, 0x25, 0x53),
    RGB565(0x00, 0x87, 0x51),
    RGB565(0xab, 0x52, 0x36),
    RGB565(0x5f, 0x57, 0x4f),
    RGB565(0xc2, 0xc3, 0xc7),
    RGB565(0xff, 0xf1, 0xe8),
    RGB565(0xff, 0x00, 0x4d),
    RGB565(0xff, 0xa3, 0x00),
    RGB565(0xff, 0xec, 0x27),
    RGB565(0x00, 0xe4, 0x36),
    RGB565(0x29, 0xad, 0xff),
    RGB565(0x83, 0x76, 0x9c),
    RGB565(0xff, 0x77, 0xa8),
    RGB565(0xff, 0xcc, 0xaa)
};
static uint8_t base_color[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static uint8_t color[16];

static inline uint16_t getcolorid(char idx)
{
    return color[idx % 16];
}

static void *Screenshot(void)
{
    lcd_wait_for_vblank();
    lcd_clear_active_buffer();
    blit();
    return lcd_get_active_buffer();
}

static void ResetPalette(void)
{
    memcpy(color, base_color, sizeof color);
}

static int gettileflag(int tile, int flag)
{
    return tile < (int)(sizeof(tile_flags) / sizeof(*tile_flags))
        && (tile_flags[tile] & (1 << flag)) != 0;
}

ITCM_TEXT
static void p8_rectfill(int x0, int y0, int x1, int y1, int col)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }

    if (x1 < 0 || y1 < 0 || x0 >= WIDTH_P8 || y0 >= HEIGHT_P8) {
        return;
    }
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 >= WIDTH_P8 ? WIDTH_P8 - 1 : x1;
    y1 = y1 >= HEIGHT_P8 ? HEIGHT_P8 - 1 : y1;

    int colorid = getcolorid(col);
    for (int i = y0; i <= y1; i++) {
        for (int j = x0; j <= x1; j++) {
            fb_celeste[i * PITCH_P8 + j] = (uint8_t)colorid;
        }
    }
}

#define CLAMP(v, min, max) v = v < min ? min : v >= max ? max - 1 : v;

ITCM_TEXT
static void p8_line(int x0, int y0, int x1, int y1, unsigned char col)
{
    CLAMP(x0, 0, WIDTH_P8);
    CLAMP(y0, 0, HEIGHT_P8);
    CLAMP(x1, 0, WIDTH_P8);
    CLAMP(y1, 0, HEIGHT_P8);

    int sx, sy, dx, dy, err, e2;
    int colorid = getcolorid(col);

    dx = iabs(x1 - x0);
    dy = iabs(y1 - y0);

    if (!dx && !dy) {
        return;
    }

    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx - dy;

    if (!dy && !dx) {
        fb_celeste[y0 * PITCH_P8 + x0] = (uint8_t)colorid;
        return;
    } else if (!dx) {
        for (int y = y0; y != y1; y += sy) {
            fb_celeste[y * PITCH_P8 + x0] = (uint8_t)colorid;
        }
    } else if (!dy) {
        for (int x = x0; x != x1; x += sx) {
            fb_celeste[y0 * PITCH_P8 + x] = (uint8_t)colorid;
        }
    }

    while (x0 != x1 || y0 != y1) {
        fb_celeste[y0 * PITCH_P8 + x0] = (uint8_t)colorid;
        e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}
#undef CLAMP

ITCM_TEXT
static void Xblit(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst,
                         SDL_Rect *dstrect, int col, int flipx, int flipy)
{
    (void)flipy;
    SDL_Rect fulldst;
    if (!dstrect) {
        dstrect = (fulldst = (SDL_Rect){0, 0, (uint16_t)dst->w, (uint16_t)dst->h}, &fulldst);
    }

    int srcx, srcy, w, h;

    if (srcrect) {
        int maxw, maxh;
        srcx = srcrect->x;
        w = srcrect->w;
        if (srcx < 0) {
            w += srcx;
            dstrect->x -= srcx;
            srcx = 0;
        }
        maxw = src->w - srcx;
        if (maxw < w) {
            w = maxw;
        }

        srcy = srcrect->y;
        h = srcrect->h;
        if (srcy < 0) {
            h += srcy;
            dstrect->y -= srcy;
            srcy = 0;
        }
        maxh = src->h - srcy;
        if (maxh < h) {
            h = maxh;
        }
    } else {
        srcx = srcy = 0;
        w = src->w;
        h = src->h;
    }

    {
        SDL_Rect *clip = &dst->clip_rect;
        int dx, dy;

        dx = clip->x - dstrect->x;
        if (dx > 0) {
            w -= dx;
            dstrect->x += dx;
            srcx += dx;
        }
        dx = dstrect->x + w - clip->x - clip->w;
        if (dx > 0) {
            w -= dx;
        }

        dy = clip->y - dstrect->y;
        if (dy > 0) {
            h -= dy;
            dstrect->y += dy;
            srcy += dy;
        }
        dy = dstrect->y + h - clip->y - clip->h;
        if (dy > 0) {
            h -= dy;
        }
    }

    if (w && h) {
        unsigned char *srcpix = src->pixels;
        int srcpitch = src->pitch;
        uint8_t *dstpix = dst->pixels;
#define _blitter(dp, xflip)                                                        \
        do {                                                                       \
            for (int y = 0; y < h; y++)                                            \
                for (int x = 0; x < w; x++) {                                      \
                    unsigned char p =                                              \
                        srcpix[!xflip ? srcx + x + (srcy + y) * srcpitch           \
                                      : srcx + (w - x - 1) + (srcy + y) * srcpitch]; \
                    if (p)                                                         \
                        dstpix[dstrect->x + x + (dstrect->y + y) * dst->w] =       \
                            (uint8_t)getcolorid(dp);                               \
                }                                                                  \
        } while (0)
        if (col && flipx) {
            _blitter(col, 1);
        } else if (!col && flipx) {
            _blitter(p, 1);
        } else if (col && !flipx) {
            _blitter(col, 0);
        } else {
            _blitter(p, 0);
        }
#undef _blitter
    }
}

ITCM_TEXT
static void p8_print(const char *str, int x, int y, int col)
{
    for (char c = *str; c; c = *(++str)) {
        c &= 0x7F;
        SDL_Rect srcrc = {8 * (c % 16), 8 * (c / 16), 8, 8};
        SDL_Rect dstrc = {x, y, 1, 1};
        Xblit(font, &srcrc, screen, &dstrc, col, 0, 0);
        x += 4;
    }
}

ITCM_TEXT
int pico8emu(CELESTE_P8_CALLBACK_TYPE call, ...)
{
    static int camera_x = 0, camera_y = 0;
    if (!enable_screenshake) {
        camera_x = camera_y = 0;
    }

    va_list args;
    int ret = 0;
    va_start(args, call);

#define INT_ARG() va_arg(args, int)
#define BOOL_ARG() (Celeste_P8_bool_t)va_arg(args, int)
#define RET_INT(_i)   do { ret = (_i); goto end; } while (0)
#define RET_BOOL(_b) RET_INT(!!(_b))

    switch (call) {
    case CELESTE_P8_MUSIC: {
        int index = INT_ARG();
        int fade = INT_ARG();
        int mask = INT_ARG();
        current_track.index = (int8_t)index;
        current_track.fade = (uint8_t)fade;
        current_track.mask = (uint8_t)mask;
        celeste_api_music(index, fade, mask);
    } break;
    case CELESTE_P8_SPR: {
        int sprite = INT_ARG();
        int x = INT_ARG();
        int y = INT_ARG();
        int cols = INT_ARG();
        int rows = INT_ARG();
        int flipx = BOOL_ARG();
        int flipy = BOOL_ARG();
        (void)cols;
        (void)rows;
        assert(rows == 1 && cols == 1);
        if (sprite >= 0) {
            SDL_Rect srcrc = {8 * (sprite % 16), 8 * (sprite / 16), 8, 8};
            SDL_Rect dstrc = {(int16_t)(x - camera_x), (int16_t)(y - camera_y), 1, 1};
            Xblit(gfx, &srcrc, screen, &dstrc, 0, flipx, flipy);
        }
    } break;
    case CELESTE_P8_BTN: {
        int b = INT_ARG();
        assert(b >= 0 && b <= 5);
        RET_BOOL(buttons_state & (1 << b));
    } break;
    case CELESTE_P8_SFX: {
        int id = INT_ARG();
        celeste_api_sfx(id, -1, 0);
    } break;
    case CELESTE_P8_PAL: {
        int a = INT_ARG();
        int b = INT_ARG();
        if (a >= 0 && a < 16 && b >= 0 && b < 16) {
            color[a] = (uint8_t)b;
        }
    } break;
    case CELESTE_P8_PAL_RESET:
        ResetPalette();
        break;
    case CELESTE_P8_CIRCFILL: {
        int cx = INT_ARG() - camera_x;
        int cy = INT_ARG() - camera_y;
        int r = INT_ARG();
        int col = INT_ARG();
        if (r <= 1) {
            p8_rectfill(cx - 1, cy, cx - 1 + 3, cy + 1, col);
            p8_rectfill(cx, cy - 1, cx + 1, cy + 2, col);
        } else if (r <= 2) {
            p8_rectfill(cx - 2, cy - 1, cx + 3, cy + 2, col);
            p8_rectfill(cx - 1, cy - 2, cx + 2, cy + 3, col);
        } else if (r <= 3) {
            p8_rectfill(cx - 3, cy - 1, cx + 4, cy + 2, col);
            p8_rectfill(cx - 1, cy - 3, cx + 2, cy + 4, col);
            p8_rectfill(cx - 2, cy - 2, cx + 3, cy + 3, col);
        }
    } break;
    case CELESTE_P8_PRINT: {
        const char *str = va_arg(args, const char *);
        int x = INT_ARG() - camera_x;
        int y = INT_ARG() - camera_y;
        int col = INT_ARG() % 16;
        if (!strcmp(str, "x+c")) {
            str = "a+b";
        }
        p8_print(str, x, y, col);
    } break;
    case CELESTE_P8_RECTFILL: {
        int x0 = INT_ARG() - camera_x;
        int y0 = INT_ARG() - camera_y;
        int x1 = INT_ARG() - camera_x;
        int y1 = INT_ARG() - camera_y;
        int col = INT_ARG();
        p8_rectfill(x0, y0, x1, y1, col);
    } break;
    case CELESTE_P8_LINE: {
        int x0 = INT_ARG() - camera_x;
        int y0 = INT_ARG() - camera_y;
        int x1 = INT_ARG() - camera_x;
        int y1 = INT_ARG() - camera_y;
        int col = INT_ARG();
        p8_line(x0, y0, x1, y1, (unsigned char)col);
    } break;
    case CELESTE_P8_MGET: {
        int tx = INT_ARG();
        int ty = INT_ARG();
        RET_INT(tilemap_data[tx + ty * 128]);
    } break;
    case CELESTE_P8_CAMERA:
        if (enable_screenshake) {
            camera_x = INT_ARG();
            camera_y = INT_ARG();
        }
        break;
    case CELESTE_P8_FGET: {
        int tile = INT_ARG();
        int flag = INT_ARG();
        RET_INT(gettileflag(tile, flag));
    } break;
    case CELESTE_P8_MAP: {
        int mx = INT_ARG(), my = INT_ARG();
        int tx = INT_ARG(), ty = INT_ARG();
        int mw = INT_ARG(), mh = INT_ARG();
        int mask = INT_ARG();

        for (int x = 0; x < mw; x++) {
            for (int y = 0; y < mh; y++) {
                int tile = tilemap_data[x + mx + (y + my) * 128];
                if (mask == 0 || (mask == 4 && tile_flags[tile] == 4)
                    || gettileflag(tile, mask != 4 ? mask - 1 : mask)) {
                    SDL_Rect srcrc = {8 * (tile % 16), 8 * (tile / 16), 8, 8};
                    SDL_Rect dstrc = {
                        (int16_t)(tx + x * 8 - camera_x),
                        (int16_t)(ty + y * 8 - camera_y),
                        8, 8
                    };
                    Xblit(gfx, &srcrc, screen, &dstrc, 0, 0, 0);
                }
            }
        }
    } break;
    }

end:
    va_end(args);
    return ret;
}

ITCM_TEXT
__attribute__((optimize("unroll-loops")))
static void blit_normal(uint8_t *src, uint16_t *framebuffer)
{
    int offsetx = WIDTH / 2 - WIDTH_P8;
    int offsety = 4;
    for (int y = 0; y < HEIGHT / 2; y++) {
        for (int x = 0; x < WIDTH_P8 - 1; x++) {
            uint16_t c = base_palette[src[(y + offsety) * PITCH_P8 + x]];
            framebuffer[offsetx + 2 * y * WIDTH + 2 * x] = c;
            framebuffer[offsetx + 2 * y * WIDTH + 2 * x + 1] = c;
            framebuffer[offsetx + 2 * y * WIDTH + 2 * x + WIDTH] = c;
            framebuffer[offsetx + 2 * y * WIDTH + 2 * x + WIDTH + 1] = c;
        }
    }
}

ITCM_TEXT
__attribute__((optimize("unroll-loops")))
static void screen_blit_nn(uint8_t *src, uint16_t *framebuffer, uint16_t width)
{
    uint16_t w1 = WIDTH_P8 - 1;
    uint16_t h1 = HEIGHT_P8;
    uint16_t w2 = width;
    uint16_t h2 = HEIGHT;
    uint8_t x_offset = (WIDTH - width) / 2;
    int x_ratio = (int)((w1 << 16) / w2) + 1;
    int y_ratio = (int)((h1 << 16) / h2) + 1;

    for (int i = 0; i < h2; i++) {
        for (int j = 0; j < w2; j++) {
            int x2 = (j * x_ratio) >> 16;
            int y2 = (i * y_ratio) >> 16;
            framebuffer[(i * WIDTH) + j + x_offset] = base_palette[src[y2 * PITCH_P8 + x2]];
        }
    }
}

ITCM_TEXT
static void blit(void)
{
    odroid_display_scaling_t scaling = odroid_display_get_scaling_mode();
    uint8_t *src = fb_celeste;
    uint16_t *framebuffer = lcd_get_active_buffer();

    switch (scaling) {
    case ODROID_DISPLAY_SCALING_OFF:
        blit_normal(src, framebuffer);
        break;
    case ODROID_DISPLAY_SCALING_FIT:
        screen_blit_nn(src, framebuffer, WIDTH_P8 * 2);
        break;
    case ODROID_DISPLAY_SCALING_FULL:
    case ODROID_DISPLAY_SCALING_CUSTOM:
        screen_blit_nn(src, framebuffer, WIDTH);
        break;
    default:
        printf("Unknown scaling mode %d\n", scaling);
        assert(!"Unknown scaling mode");
        break;
    }
    common_ingame_overlay();
}

static void update_sound_celeste(void)
{
    celeste_fill_audio_buffer(audioBuffer, 0, CELESTE_AUDIO_BUFFER_LENGTH);

    if (common_emu_sound_loop_is_muted()) {
        return;
    }

    int32_t factor = common_emu_sound_get_volume();
    int16_t *sound_buffer = audio_get_active_buffer();
    uint16_t sound_buffer_length = audio_get_buffer_length();

    for (int i = 0; i < sound_buffer_length; i++) {
        int32_t sample = audioBuffer[i];
        sound_buffer[i] = (int16_t)((sample * factor) >> 8);
    }
}

void app_main(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    odroid_dialog_choice_t options[] = {
        ODROID_DIALOG_CHOICE_LAST
    };

    /* Hot code → ITCM before any celeste/audio/blit call. */
    celeste_load_itcm();

    dtc_init();
    fb_celeste = dtc_calloc(1, FB_BYTES);
    audioBuffer = dtc_malloc(CELESTE_AUDIO_BUFFER_LENGTH * sizeof(int16_t));
    if (!fb_celeste || !audioBuffer) {
        printf("Celeste: DTCM alloc failed (fb=%p audio=%p free=%u)\n",
               (void *)fb_celeste, (void *)audioBuffer, (unsigned)dtc_get_free_size());
        return;
    }

    gfx->w = 128;
    gfx->pitch = 128;
    gfx->h = 64;
    gfx->pixels = (void *)gfx_data;

    font->w = 128;
    font->pitch = 128;
    font->h = 85;
    font->pixels = (void *)font_data;

    screen->w = PITCH_P8;
    screen->pitch = PITCH_P8;
    screen->h = HEIGHT_P8;
    screen->clip_rect.x = 0;
    screen->clip_rect.y = 0;
    screen->clip_rect.w = WIDTH;
    screen->clip_rect.h = HEIGHT;
    screen->pixels = fb_celeste;

    odroid_gamepad_state_t joystick;

    common_emu_state.pause_after_frames = start_paused ? 2 : 0;
    common_emu_state.frame_time_10us = (uint16_t)(100000 / CELESTE_FPS + 0.5f);

    odroid_system_init(APPID_HOMEBREW, CELESTE_AUDIO_SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, &Screenshot, NULL, NULL, NULL, NULL);

    audio_start_playing(CELESTE_AUDIO_BUFFER_LENGTH);

    celeste_init_audio();
    Celeste_P8_set_call_func(pico8emu);
    Celeste_P8_set_rndseed(4);
    Celeste_P8_init();

    if (load_state) {
        odroid_system_emu_load_state(save_slot);
    } else {
        lcd_clear_buffers();
    }

    while (true) {
        buttons_state = 0;
        screen->pixels = fb_celeste;

        wdog_refresh();

        bool drawFrame = common_emu_frame_loop();

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, &blit);
        common_emu_input_loop_handle_turbo(&joystick);

        if (joystick.values[ODROID_INPUT_LEFT])  buttons_state |= (1 << 0);
        if (joystick.values[ODROID_INPUT_RIGHT]) buttons_state |= (1 << 1);
        if (joystick.values[ODROID_INPUT_UP])    buttons_state |= (1 << 2);
        if (joystick.values[ODROID_INPUT_DOWN])  buttons_state |= (1 << 3);
        if (joystick.values[ODROID_INPUT_A])     buttons_state |= (1 << 4);
        if (joystick.values[ODROID_INPUT_B])     buttons_state |= (1 << 5);

        Celeste_P8_update();
        Celeste_P8_draw();
        update_sound_celeste();

        if (drawFrame) {
            blit();
        }
        lcd_swap();

        common_emu_sound_sync(false);
#ifdef HOST_BUILD
        if (!host_poll_events()) {
            break;
        }
#endif
    }
}
