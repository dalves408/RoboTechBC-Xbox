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

/* Pad discovery runs on its own thread (RECOMP_PAD_SCAN_THREAD=0 puts it back
 * on whichever thread polls the pad). Opening a pad (HIDAPI, Bluetooth) can
 * take tens of milliseconds and SDL holds its joystick lock meanwhile, so the
 * game thread must not touch SDL during a scan: while g_scanning is set (only
 * changed under g_lock) the readers answer from the last state they read, and
 * a rumble request waits for the scan to end. All under g_lock. */
static int  g_scan_thread;
static LONG g_scanning;
static XBOX_INPUT_STATE g_last[XBOX_MAX_CONTROLLERS];
static int  g_last_ok[XBOX_MAX_CONTROLLERS], g_last_conn[XBOX_MAX_CONTROLLERS];
static int  g_rumble_pending[XBOX_MAX_CONTROLLERS];
static XBOX_VIBRATION g_rumble_val[XBOX_MAX_CONTROLLERS];

/* sdlpad_get with the scan guard; caller holds g_lock. */
static int pad_read(DWORD port, XBOX_INPUT_STATE *state)
{
    if (g_scanning) {
        if (g_last_ok[port]) *state = g_last[port];
        return g_last_ok[port];
    }
    g_last_ok[port] = sdlpad_get(port, state);
    if (g_last_ok[port]) g_last[port] = *state;
    return g_last_ok[port];
}

/* Rescan inline only when there is no scan thread; caller holds g_lock. */
static void pad_rescan_inline(int force)
{
    if (!g_scan_thread)
        sdlpad_rescan(force);
}

static DWORD WINAPI pad_scan_main(LPVOID arg)
{
    unsigned logged = 0;
    (void)arg;
    for (;;) {
        Uint64 t0, dt;
        int s;
        Sleep(1000);
        EnterCriticalSection(&g_lock);
        g_scanning = 1;
        LeaveCriticalSection(&g_lock);
        t0 = SDL_GetTicksNS();
        sdlpad_rescan_now();
        dt = (SDL_GetTicksNS() - t0) / 1000000;
        EnterCriticalSection(&g_lock);
        g_scanning = 0;
        for (s = 0; s < XBOX_MAX_CONTROLLERS; s++) {
            g_last_conn[s] = sdlpad_slot_used(s) && SDL_GamepadConnected(sp_pads[s]);
            if (!g_last_conn[s]) g_last_ok[s] = 0;
            if (g_rumble_pending[s]) {
                g_rumble_pending[s] = 0;
                if (sdlpad_slot_used(s)) sdlpad_rumble((DWORD)s, &g_rumble_val[s]);
            }
        }
        LeaveCriticalSection(&g_lock);
        if (dt >= 4 && logged++ < 20)
            fprintf(stderr, "[PAD] rescan took %u ms on the pad thread (the game kept the last pad state)\n", (unsigned)dt);
    }
    /* not reached: the loop never ends */
}

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
        {
            const char *e = getenv("RECOMP_PAD_SCAN_THREAD");
            int s;
            for (s = 0; s < XBOX_MAX_CONTROLLERS; s++)
                g_last_conn[s] = sdlpad_slot_used(s) && SDL_GamepadConnected(sp_pads[s]);
            if (!(e && e[0] == '0')) {
                HANDLE th = CreateThread(NULL, 0, pad_scan_main, NULL, 0, NULL);
                if (th) { g_scan_thread = 1; CloseHandle(th); }
            }
        }
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
    pad_rescan_inline(force);
    LeaveCriticalSection(&g_lock);
}

/* RECOMP_PAD_LOG=1: log each change of what port 0 reports, stage by stage (the pad itself, then with the keyboard
 * merged, then with the mouse), with a millisecond time, so a press the player did not make can be traced to its
 * source. Buttons as hex (digital) plus the analog face buttons/triggers over 32, sticks in 8 steps. */
static void pad_log_stage(DWORD port, const char *stage, const XBOX_INPUT_STATE *s)
{
    static int on = -1;
    static char last[3][96];
    static ULONGLONG t0;
    char line[96];
    int i, k;
    if (on < 0) { const char *e = getenv("RECOMP_PAD_LOG"); on = e && *e == '1'; t0 = GetTickCount64(); }
    if (!on || port != 0) return;
    k = stage[1] == 'k' ? 1 : stage[1] == 'm' ? 2 : 0;
    i = snprintf(line, sizeof line, "dig=%04X an=", (unsigned)s->Gamepad.wButtons);
    for (int b = 0; b < 8 && i < (int)sizeof line - 2; b++) line[i++] = s->Gamepad.bAnalogButtons[b] > 32 ? "ABXYKWLR"[b] : '.';
    snprintf(line + i, sizeof line - i, " L=%d,%d R=%d,%d", s->Gamepad.sThumbLX / 8192, s->Gamepad.sThumbLY / 8192,
             s->Gamepad.sThumbRX / 8192, s->Gamepad.sThumbRY / 8192);
    if (strcmp(line, last[k])) {
        strcpy(last[k], line);
        fprintf(stderr, "[PADLOG] %7llu ms %-6s %s\n", (unsigned long long)(GetTickCount64() - t0), stage, line);
    }
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
    pad_rescan_inline(0);
    ok = pad_read(dwPort, pState);
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
    pad_log_stage(dwPort, "pad", pState);

    if (dwPort == 0)
        keyboard_merge(pState);
    pad_log_stage(dwPort, "+kbd", pState);

    /* Mouse+keyboard merges on top of a real pad too: the mouse owns the
     * aim stick while it is moving, WASD owns movement while held. */
    if (dwPort == 0)
        mkb_merge(dwPort, pState);
    pad_log_stage(dwPort, "+mouse", pState);

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
    if (g_scanning)
        connected = g_last_conn[port];
    else
        connected = g_last_conn[port] = sdlpad_slot_used((int)port) && SDL_GamepadConnected(sp_pads[port]);
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
    pad_rescan_inline(0);
    ok = pad_read(port, state);
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
    if (g_scanning) {
        /* Applied when the scan ends (the motors keep the last value). */
        if (g_last_conn[dwPort]) {
            g_rumble_val[dwPort] = *pVibration;
            g_rumble_pending[dwPort] = 1;
            rc = 0;
        }
    } else if (sdlpad_slot_used((int)dwPort))
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
