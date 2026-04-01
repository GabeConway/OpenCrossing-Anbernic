/* pc_pad.c - GC controller input via SDL gamepad + keyboard */
#include "pc_platform.h"
#include "pc_settings.h"
#include "pc_typing.h"
#include "pc_keybindings.h"
#include <dolphin/pad.h>

/* analog stick constants */
#define STICK_MAGNITUDE     80
#define AXIS_DEADZONE       4000
#define TRIGGER_THRESHOLD   100
#define RUMBLE_DURATION_MS  200

static SDL_GameController* g_controller = NULL;

/* L1 double-tap detection for zoom reset */
#define L1_DOUBLETAP_MS 300
static Uint32 l1_last_release = 0;
static int    l1_was_held = 0;
static Uint32 kb_l_last_release = 0;
static int    kb_l_was_held = 0;

BOOL PADInit(void) {
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            g_controller = SDL_GameControllerOpen(i);
            if (g_controller) {
                break;
            }
        }
    }
    return TRUE;
}

u32 PADRead(PADStatus* status) {
    memset(status, 0, sizeof(PADStatus) * 4);

    /* Suppress all game input while settings menu is open */
    if (g_pc_menu_open) {
        status[0].err = 0;
        return PAD_CHAN0_BIT;
    }

    const u8* keys = SDL_GetKeyboardState(NULL);
    u32 mouse = SDL_GetMouseState(NULL, NULL);
    u16 buttons = 0;
    s8 stickX = 0, stickY = 0;
    s8 cstickX = 0, cstickY = 0;

    /* Suppress keyboard-to-button mapping when typing into the in-game text editor */
    if (!(g_pc_typing_mode && g_pc_editor_active)) {
        /* helper: check if a PCInputCode is currently pressed */
        #define INPUT_PRESSED(code) \
            (((code) & PC_INPUT_MOUSE_BIT) \
                ? (mouse & SDL_BUTTON((code) & 0xFF)) \
                : (((code) & PC_INPUT_GAMEPAD_BIT) \
                    ? (g_controller ? SDL_GameControllerGetButton(g_controller, (SDL_GameControllerButton)((code) & 0xFF)) : 0) \
                    : ((code) >= 0 ? keys[(SDL_Scancode)(code)] : 0)))

        /* buttons (from keybindings.ini) */
        PCKeybindings* kb = &g_pc_keybindings;
        if (INPUT_PRESSED(kb->a))     buttons |= PAD_BUTTON_A;
        if (INPUT_PRESSED(kb->b))     buttons |= PAD_BUTTON_B;
        if (INPUT_PRESSED(kb->x))     buttons |= PAD_BUTTON_X;
        if (INPUT_PRESSED(kb->y))     buttons |= PAD_BUTTON_Y;
        if (INPUT_PRESSED(kb->start)) buttons |= PAD_BUTTON_START;
        if (INPUT_PRESSED(kb->z))     buttons |= PAD_TRIGGER_Z;
        if (INPUT_PRESSED(kb->l))     buttons |= PAD_TRIGGER_L;
        if (INPUT_PRESSED(kb->r))     buttons |= PAD_TRIGGER_R;

        /* Keyboard L double-tap to reset zoom */
        {
            int l_now = INPUT_PRESSED(kb->l);
            if (l_now && !kb_l_was_held) {
                Uint32 now = SDL_GetTicks();
                if (now - kb_l_last_release < L1_DOUBLETAP_MS)
                    g_pc_zoom = 1.0f;
            }
            if (!l_now && kb_l_was_held)
                kb_l_last_release = SDL_GetTicks();
            kb_l_was_held = l_now;
        }

        /* main stick */
        if (INPUT_PRESSED(kb->stick_up))    stickY += STICK_MAGNITUDE;
        if (INPUT_PRESSED(kb->stick_down))  stickY -= STICK_MAGNITUDE;
        if (INPUT_PRESSED(kb->stick_left))  stickX -= STICK_MAGNITUDE;
        if (INPUT_PRESSED(kb->stick_right)) stickX += STICK_MAGNITUDE;

        /* C-stick */
        if (INPUT_PRESSED(kb->cstick_up))    cstickY += STICK_MAGNITUDE;
        if (INPUT_PRESSED(kb->cstick_down))  cstickY -= STICK_MAGNITUDE;
        if (INPUT_PRESSED(kb->cstick_left))  cstickX -= STICK_MAGNITUDE;
        if (INPUT_PRESSED(kb->cstick_right)) cstickX += STICK_MAGNITUDE;

        /* D-pad — L + D-pad Up/Down = zoom (when enabled) */
        if (g_pc_settings.zoom_enabled && INPUT_PRESSED(kb->l) && INPUT_PRESSED(kb->dpad_up)) {
            g_pc_zoom += PC_ZOOM_STEP;
            if (g_pc_zoom > PC_ZOOM_MAX) g_pc_zoom = PC_ZOOM_MAX;
        } else if (g_pc_settings.zoom_enabled && INPUT_PRESSED(kb->l) && INPUT_PRESSED(kb->dpad_down)) {
            g_pc_zoom -= PC_ZOOM_STEP;
            if (g_pc_zoom < PC_ZOOM_MIN) g_pc_zoom = PC_ZOOM_MIN;
        } else {
            if (INPUT_PRESSED(kb->dpad_up))    buttons |= PAD_BUTTON_UP;
            if (INPUT_PRESSED(kb->dpad_down))  buttons |= PAD_BUTTON_DOWN;
        }
        if (INPUT_PRESSED(kb->dpad_left))  buttons |= PAD_BUTTON_LEFT;
        if (INPUT_PRESSED(kb->dpad_right)) buttons |= PAD_BUTTON_RIGHT;

        #undef INPUT_PRESSED
    }

    /* hotplug */
    if (!g_controller) {
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_IsGameController(i)) {
                g_controller = SDL_GameControllerOpen(i);
                if (g_controller) break;
            }
        }
    }

    if (g_controller) {
        if (!SDL_GameControllerGetAttached(g_controller)) {
            SDL_GameControllerClose(g_controller);
            g_controller = NULL;
        }
    }
    /* Hardcoded gamepad path: used when no gamepad buttons are bound via PCInputCode.
     * When the user has rebound controls to gamepad buttons, the INPUT_PRESSED path
     * above handles them, and this block is skipped to avoid double-firing. */
    if (g_controller && !pc_keybindings_uses_gamepad()) {
        int swap = g_pc_settings.swap_ab_xy;
        if (swap) {
            if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_A)) buttons |= PAD_BUTTON_B;
            if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_B)) buttons |= PAD_BUTTON_A;
            if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_X)) buttons |= PAD_BUTTON_Y;
            if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_Y)) buttons |= PAD_BUTTON_X;
        } else {
            if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_A)) buttons |= PAD_BUTTON_A;
            if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_B)) buttons |= PAD_BUTTON_B;
            if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_X)) buttons |= PAD_BUTTON_X;
            if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_Y)) buttons |= PAD_BUTTON_Y;
        }
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_START)) buttons |= PAD_BUTTON_START;
        /* BACK/Select used for overlay toggle (handled in pc_main.c event loop) */
        int l1_held = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        int dpad_up = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_UP);
        int dpad_down = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN);

        /* L1 double-tap to reset zoom */
        if (l1_held && !l1_was_held) {
            /* L1 just pressed — check if it's a double-tap */
            Uint32 now = SDL_GetTicks();
            if (now - l1_last_release < L1_DOUBLETAP_MS) {
                g_pc_zoom = 1.0f;
            }
        }
        if (!l1_held && l1_was_held) {
            /* L1 just released — record time */
            l1_last_release = SDL_GetTicks();
        }
        l1_was_held = l1_held;

        if (l1_held) buttons |= PAD_TRIGGER_L;
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) buttons |= PAD_TRIGGER_Z;

        /* L1 + D-pad Up/Down = camera zoom; otherwise pass D-pad to game */
        if (g_pc_settings.zoom_enabled && l1_held && dpad_up) {
            g_pc_zoom += PC_ZOOM_STEP;
            if (g_pc_zoom > PC_ZOOM_MAX) g_pc_zoom = PC_ZOOM_MAX;
        } else if (g_pc_settings.zoom_enabled && l1_held && dpad_down) {
            g_pc_zoom -= PC_ZOOM_STEP;
            if (g_pc_zoom < PC_ZOOM_MIN) g_pc_zoom = PC_ZOOM_MIN;
        } else {
            if (dpad_up)   buttons |= PAD_BUTTON_UP;
            if (dpad_down) buttons |= PAD_BUTTON_DOWN;
        }
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  buttons |= PAD_BUTTON_LEFT;
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) buttons |= PAD_BUTTON_RIGHT;
    } else if (g_controller && !((unsigned)g_pc_keybindings.dpad_up & PC_INPUT_GAMEPAD_BIT)) {
        /* Mixed-binding mode: D-pad bindings are still keyboard scancodes that won't
         * fire on a controller-only device, so read the physical D-pad directly.
         * Zoom is already handled by the PCInputCode path above. */
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_UP))    buttons |= PAD_BUTTON_UP;
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  buttons |= PAD_BUTTON_DOWN;
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  buttons |= PAD_BUTTON_LEFT;
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) buttons |= PAD_BUTTON_RIGHT;
    }

    /* Analog axes always run regardless of binding mode — sticks and triggers are
     * not part of PCInputCode and have no remappable equivalent. */
    if (g_controller) {
        int ldz = (int)(g_pc_settings.left_deadzone  / 100.0f * 32767.0f);
        int rdz = (int)(g_pc_settings.right_deadzone / 100.0f * 32767.0f);

        s16 lx = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX);
        s16 ly = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTY);
        if (abs(lx) > ldz) {
            int sx = lx >> 8;
            if (sx > 127) sx = 127; else if (sx < -128) sx = -128;
            stickX = (s8)sx;
        }
        if (abs(ly) > ldz) {
            int sy = -(ly >> 8);
            if (sy > 127) sy = 127; else if (sy < -128) sy = -128;
            stickY = (s8)sy;
        }

        s16 rx = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTX);
        s16 ry = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTY);
        if (abs(rx) > rdz) {
            int srx = rx >> 8;
            if (srx > 127) srx = 127; else if (srx < -128) srx = -128;
            cstickX = (s8)srx;
        }
        if (abs(ry) > rdz) {
            int sry = -(ry >> 8);
            if (sry > 127) sry = 127; else if (sry < -128) sry = -128;
            cstickY = (s8)sry;
        }

        u8 lt = (u8)(SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
        u8 rt = (u8)(SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
        if (lt > TRIGGER_THRESHOLD) buttons |= PAD_TRIGGER_L;
        if (rt > TRIGGER_THRESHOLD) buttons |= PAD_TRIGGER_R;
        status[0].triggerLeft = lt;
        status[0].triggerRight = rt;
    }

    /* D-pad also drives main analog stick (overrides axis when pressed) */
    if (g_pc_settings.dpad_as_stick) {
        if (buttons & PAD_BUTTON_LEFT)  stickX = -STICK_MAGNITUDE;
        if (buttons & PAD_BUTTON_RIGHT) stickX =  STICK_MAGNITUDE;
        if (buttons & PAD_BUTTON_DOWN)  stickY = -STICK_MAGNITUDE;
        if (buttons & PAD_BUTTON_UP)    stickY =  STICK_MAGNITUDE;
        /* Consume dpad — don't also send PAD_BUTTON_* to game */
        buttons &= ~(PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT | PAD_BUTTON_UP | PAD_BUTTON_DOWN);
    }

    status[0].button = buttons;
    status[0].stickX = stickX;
    status[0].stickY = stickY;
    status[0].substickX = cstickX;
    status[0].substickY = cstickY;
    status[0].err = 0; /* PAD_ERR_NONE */

    return PAD_CHAN0_BIT; /* Controller 1 connected */
}

void PADControlMotor(s32 chan, u32 command) {
    if (g_controller && chan == 0) {
        u16 intensity = (command == 1) ? 0xFFFF : 0;
        SDL_GameControllerRumble(g_controller, intensity, intensity, RUMBLE_DURATION_MS);
    }
}

void PADControlAllMotors(const u32* commands) {
    PADControlMotor(0, commands[0]);
}

void PADCleanup(void) {
    if (g_controller) {
        SDL_GameControllerClose(g_controller);
        g_controller = NULL;
    }
}

BOOL PADReset(u32 mask) { (void)mask; return TRUE; }
BOOL PADRecalibrate(u32 mask) { (void)mask; return TRUE; }
BOOL PADSync(void) { return TRUE; }
void PADSetSpec(u32 spec) { (void)spec; }
void PADSetAnalogMode(u32 mode) { (void)mode; }
/* PADClamp compiled from decomp: src/static/dolphin/pad/Padclamp.c */
BOOL PADGetType(s32 chan, u32* type) { if (type) *type = 0x09000000; return TRUE; }

SDL_GameController* pc_pad_get_controller(void) { return g_controller; }
