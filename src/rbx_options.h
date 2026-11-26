/* The video settings, shared by the game's F1 menu (rbx_settings.c) and
 * Config.exe (src/config/rbx_config.c). Both read and write the [video]
 * section of settings.ini beside the executable, by the keys below. The row
 * order is the menu order; the values stored are indexes into values[]. */
#ifndef RBX_OPTIONS_H
#define RBX_OPTIONS_H

enum { RBX_OPT_SCALE, RBX_OPT_FULLSCREEN, RBX_OPT_STRETCH, RBX_OPT_WIDE, RBX_OPT_VSYNC,
       RBX_OPT_RATE, RBX_OPT_AA, RBX_OPT_ANISO, RBX_OPT_HIRES, RBX_OPT_INTERP, RBX_OPT_COUNT };

/* The first RBX_OPT_DISPLAY rows are the "Display" section, the rest "Graphics". */
#define RBX_OPT_DISPLAY 4

typedef struct {
    const char *label, *key;
    unsigned count, initial;
    const char *values[7];
    const char *note;               /* one line shown while the row is selected */
} RbxOption;

static const RbxOption rbx_options[RBX_OPT_COUNT] = {
    {"Render scale", "scale", 5, 0,
     {"1x  /  640 x 480", "2x  /  1280 x 960", "3x  /  1920 x 1440", "4x  /  2560 x 1920", "5x  /  3200 x 2400"},
     "Internal resolution. Higher is sharper and costs more GPU time."},
    {"Fullscreen", "fullscreen", 2, 0, {"Off", "On"},
     "Borderless fullscreen on the current monitor. F11 or Alt+Enter also switches."},
    {"Stretch image", "stretch", 2, 0, {"Off  /  Keep shape", "On  /  Fill window"},
     "Off keeps the picture's shape with bars; On fills the whole window."},
    /* Widescreen renders more of the scene left and right (Hor+) and keeps the HUD and menus at 4:3 in the middle. */
    {"Widescreen", "widescreen", 5, 1, {"Off  /  4:3", "Auto  /  Match window", "16:9", "16:10", "21:9"},
     "Shows more of the battlefield left and right. The HUD and menus stay 4:3."},
    {"VSync", "vsync", 2, 0, {"Off", "On"},
     "Waits for the monitor's refresh: no tearing, slightly more input delay."},
    {"FPS limit", "rate", 7, 6, {"30 FPS", "60 FPS", "120 FPS", "144 FPS", "240 FPS", "360 FPS", "Uncapped"},
     "Caps how often frames are shown. The game's own clock is unchanged."},
    {"Anti-aliasing", "msaa", 4, 0, {"Off", "2x MSAA", "4x MSAA", "8x MSAA"},
     "Smooths jagged edges. Levels your GPU cannot do are skipped."},
    {"Anisotropic filtering", "anisotropy", 5, 0, {"Off", "2x", "4x", "8x", "16x"},
     "Keeps ground and walls sharp at steep angles. Cheap on modern GPUs."},
    {"Hi-res textures", "hires", 2, 1, {"Off", "On"},
     "Uses replacement textures from the texture_pack folder when present."},
    {"Frame interpolation", "interp", 2, 1, {"Off", "On"},
     "Smooths 3D motion between the game's 30 Hz steps. Turn it off if anything shakes or slides."},
};

#endif
