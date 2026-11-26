/* SDL3 keyboard and mouse events from the existing framebuffer HWND. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>
#include "input/keyboard.h"

static SDL_Window *input_window;
static SDL_WindowID input_window_id;
static int attached;

static int to_vk(SDL_Keycode key)
{
    if (key >= SDLK_A && key <= SDLK_Z) return 'A' + (int)(key - SDLK_A);
    if (key >= SDLK_0 && key <= SDLK_9) return (int)key;
    switch (key) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return VK_RETURN;
    case SDLK_ESCAPE: return VK_ESCAPE;
    case SDLK_F1: return VK_F1;
    case SDLK_BACKSPACE: return VK_BACK;
    case SDLK_SPACE: return VK_SPACE;
    case SDLK_UP: return VK_UP;
    case SDLK_DOWN: return VK_DOWN;
    case SDLK_LEFT: return VK_LEFT;
    case SDLK_RIGHT: return VK_RIGHT;
    case SDLK_LSHIFT: return VK_LSHIFT;
    case SDLK_RSHIFT: return VK_RSHIFT;
    case SDLK_LCTRL: return VK_LCONTROL;
    case SDLK_RCTRL: return VK_RCONTROL;
    case SDLK_LALT: return VK_LMENU;
    case SDLK_RALT: return VK_RMENU;
    case SDLK_TAB: return VK_TAB;
    case SDLK_PAUSE: return VK_PAUSE;
    case SDLK_CAPSLOCK: return VK_CAPITAL;
    case SDLK_PAGEUP: return VK_PRIOR;
    case SDLK_PAGEDOWN: return VK_NEXT;
    case SDLK_HOME: return VK_HOME;
    case SDLK_END: return VK_END;
    case SDLK_INSERT: return VK_INSERT;
    case SDLK_KP_0: return VK_NUMPAD0;
    case SDLK_KP_1: return VK_NUMPAD1;
    case SDLK_KP_2: return VK_NUMPAD2;
    case SDLK_KP_3: return VK_NUMPAD3;
    case SDLK_KP_4: return VK_NUMPAD4;
    case SDLK_KP_5: return VK_NUMPAD5;
    case SDLK_KP_6: return VK_NUMPAD6;
    case SDLK_KP_7: return VK_NUMPAD7;
    case SDLK_KP_8: return VK_NUMPAD8;
    case SDLK_KP_9: return VK_NUMPAD9;
    case SDLK_KP_MULTIPLY: return VK_MULTIPLY;
    case SDLK_KP_PLUS: return VK_ADD;
    case SDLK_KP_MINUS: return VK_SUBTRACT;
    case SDLK_KP_PERIOD: return VK_DECIMAL;
    case SDLK_KP_DIVIDE: return VK_DIVIDE;
    case SDLK_SEMICOLON: return VK_OEM_1;
    case SDLK_EQUALS: return VK_OEM_PLUS;
    case SDLK_COMMA: return VK_OEM_COMMA;
    case SDLK_MINUS: return VK_OEM_MINUS;
    case SDLK_PERIOD: return VK_OEM_PERIOD;
    case SDLK_SLASH: return VK_OEM_2;
    case SDLK_GRAVE: return VK_OEM_3;
    case SDLK_LEFTBRACKET: return VK_OEM_4;
    case SDLK_BACKSLASH: return VK_OEM_5;
    case SDLK_RIGHTBRACKET: return VK_OEM_6;
    case SDLK_APOSTROPHE: return VK_OEM_7;
    default: return 0;
    }
}

int rbx_sdl_window_tick(HWND hwnd, int (*handle_key)(unsigned, long, int),
                        int (*input_blocked)(void))
{
    SDL_Event event;
    if (!attached) {
        SDL_PropertiesID props;
        attached = 1;
        SDL_SetHint(SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP, "0");
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            fprintf(stderr, "[SDL3] window input unavailable: %s\n", SDL_GetError());
            return 0;
        }
        props = SDL_CreateProperties();
        if (props) {
            SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, hwnd);
            SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);
            /* The window can be resized (it is WS_OVERLAPPEDWINDOW). Without this SDL treats it as fixed-size and,
             * on WM_WINDOWPOSCHANGING, holds it at the last size it saw: after fullscreen, the monitor's, so
             * leaving fullscreen gave a screen-sized window with its title bar off the top. */
            SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
            input_window = SDL_CreateWindowWithProperties(props);
            SDL_DestroyProperties(props);
        }
        if (!input_window) {
            fprintf(stderr, "[SDL3] window input unavailable: %s\n", SDL_GetError());
            return 0;
        }
        input_window_id = SDL_GetWindowID(input_window);
        fprintf(stderr, "[SDL3] keyboard and mouse attached to the framebuffer window\n");
    }
    if (!input_window) return 0;
    {
        /* Leaving fullscreen (F11, Alt+Enter, F1) restores the window's caption style, but SDL saw the
         * borderless fullscreen style and keeps treating the window as borderless: it answers WM_NCCALCSIZE
         * with no frame, so the window came back with no title bar. Tell it the window is bordered again. */
        extern int xbox_fb_is_fullscreen(void);
        if (!xbox_fb_is_fullscreen() && (SDL_GetWindowFlags(input_window) & SDL_WINDOW_BORDERLESS)) {
            SDL_SetWindowBordered(input_window, true);
            fprintf(stderr, "[SDL3] window bordered again after fullscreen\n");
        }
    }
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0) {
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST && event.window.windowID == input_window_id)
            xbox_InputKeysClear();
        if ((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
            event.key.windowID == input_window_id) {
            int vk = to_vk(event.key.key);
            if (!vk) continue;
            if (vk == VK_RETURN && (event.key.mod & SDL_KMOD_ALT)) continue;
            if (handle_key((unsigned)vk, event.key.repeat ? 1L << 30 : 0, event.key.down) ||
                input_blocked())
                continue;
            xbox_InputKeySet(vk, event.key.down);
            if (event.key.down && !event.key.repeat && getenv("RECOMP_KEY_TRACE")) {
                static unsigned logged;
                if (logged++ < 12) fprintf(stderr, "[SDL3] key vk=0x%02X\n", (unsigned)vk);
            }
            if (vk == VK_LSHIFT || vk == VK_RSHIFT)
                xbox_InputKeySet(VK_SHIFT, (event.key.mod & SDL_KMOD_SHIFT) != 0);
            if (vk == VK_LCONTROL || vk == VK_RCONTROL)
                xbox_InputKeySet(VK_CONTROL, (event.key.mod & SDL_KMOD_CTRL) != 0);
            if (vk == VK_LMENU || vk == VK_RMENU)
                xbox_InputKeySet(VK_MENU, (event.key.mod & SDL_KMOD_ALT) != 0);
        }
        if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
            event.button.windowID == input_window_id && !input_blocked()) {
            static const int buttons[] = {0, VK_LBUTTON, VK_MBUTTON, VK_RBUTTON, VK_XBUTTON1, VK_XBUTTON2};
            if (event.button.button < sizeof buttons / sizeof buttons[0])
                xbox_InputKeySet(buttons[event.button.button], event.button.down);
        }
    }
    return 1;
}
