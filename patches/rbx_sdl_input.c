/* SDL3 Xbox pad adapter from the NHLHitzPro recompilation project. */
#include "xinput_xbox.h"
#include "keyboard.h"
#include "mkb_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <SDL3/SDL.h>

static volatile LONG g_controller_connected[XBOX_MAX_CONTROLLERS];

/* Set by src/rbx_settings.c: the keyboard through the player's key map
 * (Controls tab), and which face buttons to swap on controllers (bit 0 A/B,
 * bit 1 X/Y). Unset, the toolkit's keyboard map is used and nothing swaps. */
int (*rbx_keyboard_poll_hook)(XBOX_INPUT_STATE *out);
int (*rbx_pad_swaps_hook)(void);

static int rbx_keyboard_poll(XBOX_INPUT_STATE *out)
{
    return rbx_keyboard_poll_hook ? rbx_keyboard_poll_hook(out) : xbox_InputKeyboardPoll(out);
}

static void swap_face_buttons(XBOX_INPUT_STATE *state)
{
    int swaps = rbx_pad_swaps_hook ? rbx_pad_swaps_hook() : 0;
    BYTE *b = state->Gamepad.bAnalogButtons, t;
    if (swaps & 1) { t = b[XBOX_BUTTON_A]; b[XBOX_BUTTON_A] = b[XBOX_BUTTON_B]; b[XBOX_BUTTON_B] = t; }
    if (swaps & 2) { t = b[XBOX_BUTTON_X]; b[XBOX_BUTTON_X] = b[XBOX_BUTTON_Y]; b[XBOX_BUTTON_Y] = t; }
}
int (*rbx_input_block_hook)(void);
volatile LONG rbx_input_guest_frame;

/* Diagnostic input is sampled at the guest's render flip number. */
static struct FramePress {
    unsigned long frame, length;
    unsigned button;
    int reported;
} frame_presses[512];
static unsigned frame_press_count;
static const char *const frame_buttons[] = {
    "up", "down", "left", "right", "start", "back", "ls", "rs",
    "a", "b", "x", "y", "black", "white", "lt", "rt"
};

static void load_frame_presses(void)
{
    const char *spec = getenv("RECOMP_RBX_FRAME_PAD");
    if (!spec || !*spec) return;
    while (*spec) {
        char *end;
        unsigned long frame, length = 8;
        unsigned index;
        if (*spec++ != 'f' || *spec < '0' || *spec > '9') break;
        frame = strtoul(spec, &end, 10);
        if (*end != ':') break;
        spec = end + 1;
        size_t button_length = strcspn(spec, ":,");
        for (index = 0; index < sizeof frame_buttons / sizeof frame_buttons[0]; index++)
            if (strlen(frame_buttons[index]) == button_length &&
                !strncmp(spec, frame_buttons[index], button_length)) break;
        spec += button_length;
        if (*spec == ':') {
            if (*++spec < '0' || *spec > '9') break;
            length = strtoul(spec, &end, 10);
            spec = end;
        }
        char separator = *spec;
        if ((separator && separator != ',') || !length || frame > LONG_MAX ||
            length > LONG_MAX - frame || index == sizeof frame_buttons / sizeof frame_buttons[0] ||
            frame_press_count == sizeof frame_presses / sizeof frame_presses[0])
            break;
        frame_presses[frame_press_count++] = (struct FramePress){frame, length, index, 0};
        if (!separator) {
            fprintf(stderr, "[RBX PAD] loaded %u frame-numbered presses\n", frame_press_count);
            return;
        }
        spec++;
    }
    frame_press_count = 0;
    fprintf(stderr, "[RBX PAD] invalid RECOMP_RBX_FRAME_PAD script\n");
}

static void read_frame_pad(XBOX_INPUT_STATE *state)
{
    unsigned long frame = (unsigned long)rbx_input_guest_frame;
    memset(state, 0, sizeof *state);
    state->dwPacketNumber = frame;
    for (unsigned i = 0; i < frame_press_count; i++) {
        struct FramePress *press = &frame_presses[i];
        if (frame < press->frame || frame - press->frame >= press->length) continue;
        if (press->button < 8) state->Gamepad.wButtons |= (WORD)(1u << press->button);
        else state->Gamepad.bAnalogButtons[press->button - 8] = 255;
        if (!press->reported) {
            press->reported = 1;
            fprintf(stderr, "[RBX PAD] %s observed at f%lu (scheduled f%lu)\n",
                    frame_buttons[press->button], frame, press->frame);
        }
    }
}

#define SDLPAD_WANTED(pad)     1          /* every gamepad SDL knows */
#define SDLPAD_SLOT_FREE(slot) 1
#include "sdl3_pad.inc"

/* Merge the keyboard on top of a pad on port 0 rather than only standing in
 * for a missing one (see the toolkit's xinput_device.c for the history). */
static void keyboard_merge(XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb;
    int i;

    if (!rbx_keyboard_poll(&kb))
        return;
    pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
    for (i = 0; i < 8; i++)
        if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
            pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
    if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
    if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
    if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
    if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
    /* The input layer records edges, so an unchanged packet number is
     * read as the same state and the press never happens. */
    pState->dwPacketNumber = kb.dwPacketNumber;
}

/* The Windows player never calls xbox_InputInit, so the first use starts SDL. The pad state is read from the
 * game thread and, for the overlays, from the window thread: one lock covers SDL's side of it. */
static CRITICAL_SECTION g_lock;
static volatile LONG g_state;       /* 0 = not started, 1 = starting, 2 = ready */

static void ensure_init(void)
{
    if (g_state == 2)
        return;
    if (InterlockedCompareExchange(&g_state, 1, 0) == 0) {
        InitializeCriticalSection(&g_lock);
        load_frame_presses();
        sdlpad_init();
        if (sp_log_on() && !sdlpad_any_open())
            fprintf(stderr, "[PAD] SDL started; no gamepad found yet (one plugged in later is picked up)\n");
        InterlockedExchange(&g_state, 2);
        return;
    }
    while (g_state != 2)
        Sleep(1);
}

void xbox_InputInit(void)
{
    ensure_init();
}

/* Look for pads that came or went, now and then (once a second inside). */
static void pads_scan(int force)
{
    EnterCriticalSection(&g_lock);
    sdlpad_rescan(force);
    LeaveCriticalSection(&g_lock);
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    int ok;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    ensure_init();
    if (dwPort == 0 && frame_press_count) {
        EnterCriticalSection(&g_lock);
        read_frame_pad(pState);
        LeaveCriticalSection(&g_lock);
        if (rbx_input_block_hook && rbx_input_block_hook())
            memset(&pState->Gamepad, 0, sizeof pState->Gamepad);
        return ERROR_SUCCESS;
    }
    EnterCriticalSection(&g_lock);
    sdlpad_rescan(0);
    ok = sdlpad_get(dwPort, pState);
    LeaveCriticalSection(&g_lock);

    if (!ok) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0) {
            /* Keyboard and mouse stand in for an absent pad, so the game is
             * playable with no controller plugged in. */
            int kb;
            memset(pState, 0, sizeof *pState);
            kb = rbx_keyboard_poll(pState);
            if (mkb_merge(dwPort, pState))
                g_controller_connected[dwPort] = TRUE;
            if (kb || mkb_enabled()) {
                if (rbx_input_block_hook && rbx_input_block_hook())
                    memset(&pState->Gamepad, 0, sizeof pState->Gamepad);
                return ERROR_SUCCESS;
            }
        }
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    g_controller_connected[dwPort] = TRUE;
    swap_face_buttons(pState);

    if (dwPort == 0)
        keyboard_merge(pState);

    /* Mouse+keyboard merges on top of a real pad too: the mouse owns the
     * aim stick while it is moving, WASD owns movement while held. */
    if (dwPort == 0)
        mkb_merge(dwPort, pState);

    /* Back+Start belongs to the host overlay on controller 1. */
    if (dwPort == 0 && (pState->Gamepad.wButtons & (XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START)) ==
                       (XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START))
        pState->Gamepad.wButtons &= (WORD)~XBOX_GAMEPAD_START;
    if (rbx_input_block_hook && rbx_input_block_hook())
        memset(&pState->Gamepad, 0, sizeof pState->Gamepad);
    return ERROR_SUCCESS;
}

/* 1 when a pad is connected on `port`. The USB layer asks this every quarter of a second to decide which
 * players are plugged in. */
int rbx_pad_connected(DWORD port)
{
    int connected;
    if (port >= XBOX_MAX_CONTROLLERS) return 0;
    ensure_init();
    pads_scan(0);
    EnterCriticalSection(&g_lock);
    connected = sdlpad_slot_used((int)port) && SDL_GamepadConnected(sp_pads[port]);
    LeaveCriticalSection(&g_lock);
    return connected;
}

BOOL xbox_InputPadPresent(DWORD port)
{
    return rbx_pad_connected(port) != 0;
}

/* Physical controller state for the settings overlay, before guest input gating. */
int rbx_pad_peek(DWORD port, XBOX_INPUT_STATE *state)
{
    int ok;
    if (port >= XBOX_MAX_CONTROLLERS || !state) return 0;
    ensure_init();
    EnterCriticalSection(&g_lock);
    sdlpad_rescan(0);
    ok = sdlpad_get(port, state);
    LeaveCriticalSection(&g_lock);
    if (ok) swap_face_buttons(state);
    return ok;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    int rc = -1;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    ensure_init();
    EnterCriticalSection(&g_lock);
    if (sdlpad_slot_used((int)dwPort))
        rc = sdlpad_rumble(dwPort, pVibration);     /* includes the minimum pulse (RECOMP_RUMBLE_MIN_MS) */
    LeaveCriticalSection(&g_lock);
    return rc < 0 ? ERROR_DEVICE_NOT_CONNECTED : ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort] || rbx_pad_connected(dwPort);
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    ensure_init();
    if (!rbx_pad_connected(dwPort))
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = 1;       /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;    /* XINPUT_DEVSUBTYPE_GAMEPAD */
    return ERROR_SUCCESS;
}
