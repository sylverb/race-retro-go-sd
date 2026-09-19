/*
 * Neo Geo Pocket / Color (RACE) — standalone Retro-Go SD dynamic core.
 *
 * Entry (run_dynamic_core):
 *   void app_main(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
 *
 * Memory:
 *   ITCM  — hot .text (tlcs900h, graphics, cz80, race-memory) via race_core.ld
 *   DTCM  — framebuffer + colour LUT + DAC ring (dtc_malloc); no ITCM data.
 *           TLCS decode tables stay in .rodata (cached AXI) — DTCM copy hurt.
 *   RAM_EMU — mainram / cpurom / cold code / BSS
 *   AHB   — avoided for Blip (accurate audio off by default)
 */

#include <odroid_system.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gw_lcd.h"
#include "gw_buttons.h"
#include "rom_manager.h"
#include "common.h"
#include "gw_malloc.h"
#include "odroid_overlay.h"
#include "appid.h"
#include "bilinear.h"
#include "odroid_settings.h"

#ifndef HOST_BUILD
#include "gw_core_bridge.h"
#else
#include "host_compat.h"
#endif

#include "graphics.h"
#include "state.h"

void mainemuinit(void);
int handleInputFile(const char *romName, const unsigned char *romData, int romSize);
void tlcs_execute(int cycles, int skipRender);
void system_sound_chipreset(int sample_rate);
void sound_update(uint16_t *chip_buffer, int length_bytes);
void dac_update(uint16_t *dac_buffer, int length_bytes);

uint8_t ngpInputState = 0;
int tipo_consola = 0;
int gfx_hacks = 0;
int setting_ngp_language = 0;

#define WIDTH 320
#define NGP_WIDTH  (160)
#define NGP_HEIGHT (152)
#define NGP_FPS    (60)

#define NGP_CPU_FREQ      (6144000)
#define NGP_CYCLES_FRAME  (NGP_CPU_FREQ / NGP_FPS)

#define NGP_SAMPLE_RATE         (44100)
#define NGP_AUDIO_BUFFER_LENGTH (NGP_SAMPLE_RATE / NGP_FPS)

static int16_t audioBuffer_ngp[NGP_AUDIO_BUFFER_LENGTH];

/* RGB565 framebuffer — DTCM (hot scanline writes). */
static uint16_t *ngp_framebuffer = NULL;

static struct ngp_screen ngp_screen_obj;
struct ngp_screen *screen = &ngp_screen_obj;

static odroid_video_frame_t video_frame = {
    NGP_WIDTH, NGP_HEIGHT, NGP_WIDTH * 2, 2, 0xFF, -1, NULL, NULL, 0, {}
};

static void blit_emulator(void);

void graphics_paint(unsigned char render)
{
    (void)render;
}

static bool LoadState(const char *savePathName)
{
    audio_clear_buffers();

    unsigned char *data = (unsigned char *)lcd_get_active_buffer();
    int size = state_get_size();

    FILE *file = fopen(savePathName, "rb");
    if (file == NULL)
        return false;

    size_t read = fread(data, size, 1, file);
    fclose(file);
    if (!read)
        return false;

    state_restore_mem(data);
    lcd_clear_active_buffer();
    return true;
}

static bool SaveState(const char *savePathName)
{
    lcd_wait_for_vblank();
    unsigned char *data = (unsigned char *)lcd_get_active_buffer();
    int size = state_get_size();

    state_store_mem(data);

    FILE *file = fopen(savePathName, "wb");
    if (file == NULL)
        return false;

    size_t written = fwrite(data, size, 1, file);
    fclose(file);
    return written != 0;
}

static void *Screenshot(void)
{
    lcd_wait_for_vblank();
    lcd_clear_active_buffer();
    blit_emulator();
    return lcd_get_active_buffer();
}

void ngp_pcm_submit(void)
{
    int samples = NGP_AUDIO_BUFFER_LENGTH;
    sound_update((uint16_t *)audioBuffer_ngp, samples * sizeof(int16_t));
    dac_update((uint16_t *)audioBuffer_ngp, samples * sizeof(int16_t));

    if (common_emu_sound_loop_is_muted())
        return;

    int32_t factor = common_emu_sound_get_volume();
    int16_t *sound_buffer = audio_get_active_buffer();
    uint16_t sound_buffer_length = audio_get_buffer_length();

    for (int i = 0; i < sound_buffer_length; i++)
        sound_buffer[i] = (int16_t)((audioBuffer_ngp[i] * factor) >> 8);
}

#ifndef HOST_BUILD
__attribute__((optimize("unroll-loops")))
#endif
static inline void screen_blit_nn(int32_t dest_width, int32_t dest_height)
{
    int w1 = video_frame.width;
    int h1 = video_frame.height;
    int w2 = dest_width;
    int h2 = dest_height;

    int x_ratio = (int)((w1 << 16) / w2) + 1;
    int y_ratio = (int)((h1 << 16) / h2) + 1;
    int hpad = (320 - dest_width) / 2;
    int wpad = (240 - dest_height) / 2;

    uint16_t *screen_buf = (uint16_t *)video_frame.buffer;
    uint16_t *dest = lcd_get_active_buffer();

    for (int i = 0; i < wpad; i++)
        memset(dest + i * WIDTH, 0, WIDTH * sizeof(uint16_t));
    for (int i = wpad + h2; i < 240; i++)
        memset(dest + i * WIDTH, 0, WIDTH * sizeof(uint16_t));

    for (int i = 0; i < h2; i++) {
        uint16_t *row = dest + (i + wpad) * WIDTH;
        int y2 = ((i * y_ratio) >> 16);
        const uint16_t *src_row = screen_buf + (y2 * w1);
        for (int j = 0; j < hpad; j++)
            row[j] = 0;
        for (int j = 0; j < w2; j++) {
            int x2 = ((j * x_ratio) >> 16);
            row[j + hpad] = src_row[x2];
        }
        for (int j = hpad + w2; j < WIDTH; j++)
            row[j] = 0;
    }
}

static void screen_blit_bilinear(int32_t dest_width)
{
    int w1 = video_frame.width;
    int h1 = video_frame.height;
    int w2 = dest_width;
    int h2 = 240;
    int stride = 320;
    int hpad = (320 - dest_width) / 2;

    uint16_t *dest = lcd_get_active_buffer();

    image_t dst_img;
    dst_img.w = dest_width;
    dst_img.h = 240;
    dst_img.bpp = 2;
    dst_img.pixels = ((uint8_t *)dest) + hpad * 2;

    if (hpad > 0)
        memset(dest, 0x00, hpad * 2);

    image_t src_img;
    src_img.w = video_frame.width;
    src_img.h = video_frame.height;
    src_img.bpp = 2;
    src_img.pixels = video_frame.buffer;

    float x_scale = ((float)w2) / ((float)w1);
    float y_scale = ((float)h2) / ((float)h1);

    imlib_draw_image(&dst_img, &src_img, 0, 0, stride, x_scale, y_scale, NULL, -1, 255, NULL,
                     NULL, IMAGE_HINT_BILINEAR, NULL, NULL);
}

static void blit_emulator(void)
{
    lcd_sleep_while_swap_pending();

    odroid_display_scaling_t scaling = odroid_display_get_scaling_mode();
    odroid_display_filter_t filtering = odroid_display_get_filter_mode();

    static odroid_display_scaling_t last_scaling = -1;
    if (scaling != last_scaling) {
        lcd_clear_buffers();
        last_scaling = scaling;
    }

    switch (scaling) {
    case ODROID_DISPLAY_SCALING_OFF:
        screen_blit_nn(NGP_WIDTH, NGP_HEIGHT);
        break;
    case ODROID_DISPLAY_SCALING_FIT:
        if (filtering == ODROID_DISPLAY_FILTER_SOFT) {
            lcd_clear_active_buffer();
            screen_blit_bilinear(252);
        } else {
            screen_blit_nn(252, 240);
        }
        break;
    case ODROID_DISPLAY_SCALING_FULL:
    case ODROID_DISPLAY_SCALING_CUSTOM:
        if (filtering == ODROID_DISPLAY_FILTER_SOFT)
            screen_blit_bilinear(320);
        else
            screen_blit_nn(320, 240);
        break;
    default:
        screen_blit_nn(252, 240);
        break;
    }
}

static void blit(void)
{
    blit_emulator();
    common_ingame_overlay();
}

static void ngp_input_read(odroid_gamepad_state_t *joystick)
{
    uint8_t state = 0x00;
    if (joystick->values[ODROID_INPUT_UP])     state |= 0x01;
    if (joystick->values[ODROID_INPUT_DOWN])   state |= 0x02;
    if (joystick->values[ODROID_INPUT_LEFT])   state |= 0x04;
    if (joystick->values[ODROID_INPUT_RIGHT])  state |= 0x08;
    if (joystick->values[ODROID_INPUT_A])      state |= 0x10;
    if (joystick->values[ODROID_INPUT_B])      state |= 0x20;
    if (joystick->values[ODROID_INPUT_START]  || joystick->values[ODROID_INPUT_SELECT] ||
        joystick->values[ODROID_INPUT_X]      || joystick->values[ODROID_INPUT_Y])
        state |= 0x40;
    ngpInputState = state;
}

/* ROMs up to 4 MiB stay XIP in flash (or host malloc cache). */
static size_t ngp_getromdata(unsigned char **data)
{
    uint32_t size = 0;
    const unsigned char *src = odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &size, false);
    if (src == NULL || size == 0) {
        *data = NULL;
        return 0;
    }
    *data = (unsigned char *)src;
    return size;
}

void app_main(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    uint32_t rom_length = 0;
    unsigned char *rom_ptr = NULL;
    odroid_gamepad_state_t joystick;
    odroid_dialog_choice_t options[] = {
        ODROID_DIALOG_CHOICE_LAST
    };

    /* DTCM: FB(~47K) + palette(16K) + DAC(~8K) ≈ 71 KiB of ~104 KiB. */
    dtc_init();
    ngp_framebuffer = (uint16_t *)dtc_malloc(NGP_WIDTH * NGP_HEIGHT * sizeof(uint16_t));
    if (ngp_framebuffer == NULL) {
        printf("ngp: DTCM framebuffer alloc failed\n");
        return;
    }

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }
    common_emu_state.frame_time_10us = (uint16_t)(100000 / NGP_FPS + 0.5f);
    lcd_set_refresh_rate(NGP_FPS);

    video_frame.buffer = ngp_framebuffer;
    screen->w = NGP_WIDTH;
    screen->h = NGP_HEIGHT;
    screen->pixels = ngp_framebuffer;

    /* Set max OC level */
    SystemClock_Config(3);

    odroid_system_init(APPID_CORE, NGP_SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, &Screenshot, NULL, NULL, NULL, NULL);

    if (odroid_display_get_scaling_mode() == ODROID_DISPLAY_SCALING_OFF)
        odroid_display_set_scaling_mode(ODROID_DISPLAY_SCALING_FIT);

    audio_start_playing(NGP_AUDIO_BUFFER_LENGTH);

    /* Sound chip first; mainemuinit() runs inside handleInputFile → initRom
     * after mainrom is set (cart header reads at 0x002000xx need a valid ROM). */
    system_sound_chipreset(NGP_SAMPLE_RATE);

    rom_length = ngp_getromdata(&rom_ptr);
    if (rom_ptr == NULL || rom_length == 0) {
        printf("ngp: failed to load ROM '%s'\n",
               ACTIVE_FILE && ACTIVE_FILE->path[0] ? ACTIVE_FILE->path : "(none)");
        return;
    }
    if (!handleInputFile(ACTIVE_FILE->path, rom_ptr, (int)rom_length)) {
        printf("ngp: unsupported or invalid ROM '%s'\n", ACTIVE_FILE->path);
        return;
    }

    if (load_state)
        odroid_system_emu_load_state(save_slot);
    else
        lcd_clear_buffers();

    while (1) {
        wdog_refresh();

        bool drawFrame = common_emu_frame_loop();

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, &blit);
        common_emu_input_loop_handle_turbo(&joystick);

        ngp_input_read(&joystick);

        tlcs_execute(NGP_CYCLES_FRAME, drawFrame ? 0 : 1);

        if (drawFrame) {
            blit();
            lcd_swap();
        }

        ngp_pcm_submit();
        common_emu_sound_sync(false);
    }
}
