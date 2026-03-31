#ifndef PC_KEYBINDINGS_H
#define PC_KEYBINDINGS_H

#include <SDL2/SDL_scancode.h>
#include <SDL2/SDL_mouse.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PCInputCode: values 0..SDL_NUM_SCANCODES-1 are keyboard scancodes.
   Values with PC_INPUT_MOUSE_BIT set are mouse buttons (low bits = SDL button index). */
typedef int PCInputCode;

#define PC_INPUT_MOUSE_BIT   0x10000
#define PC_INPUT_MOUSE1      (PC_INPUT_MOUSE_BIT | SDL_BUTTON_LEFT)    /* left click */
#define PC_INPUT_MOUSE2      (PC_INPUT_MOUSE_BIT | SDL_BUTTON_RIGHT)   /* right click */
#define PC_INPUT_MOUSE3      (PC_INPUT_MOUSE_BIT | SDL_BUTTON_MIDDLE)  /* middle click */

/* Gamepad button codes: PC_INPUT_GAMEPAD_BIT | SDL_GameControllerButton value */
#define PC_INPUT_GAMEPAD_BIT 0x20000
#define PC_INPUT_GAMEPAD_BTN(b) (PC_INPUT_GAMEPAD_BIT | (int)(b))

typedef struct {
    /* buttons */
    PCInputCode a;
    PCInputCode b;
    PCInputCode x;
    PCInputCode y;
    PCInputCode start;
    PCInputCode z;
    PCInputCode l;
    PCInputCode r;

    /* main stick */
    PCInputCode stick_up;
    PCInputCode stick_down;
    PCInputCode stick_left;
    PCInputCode stick_right;

    /* C-stick */
    PCInputCode cstick_up;
    PCInputCode cstick_down;
    PCInputCode cstick_left;
    PCInputCode cstick_right;

    /* D-pad */
    PCInputCode dpad_up;
    PCInputCode dpad_down;
    PCInputCode dpad_left;
    PCInputCode dpad_right;
} PCKeybindings;

extern PCKeybindings g_pc_keybindings;

#define KB_COUNT 20  /* number of bindable actions, matches s_entries[] in pc_keybindings.c */

void pc_keybindings_load(void);
void pc_keybindings_save(void);        /* write current bindings to keybindings.ini */
void pc_keybindings_reset(void);       /* restore hard-coded defaults and save */
int  pc_keybindings_uses_gamepad(void); /* 1 if any binding is a gamepad button */

/* For overlay: idx 0..KB_COUNT-1 matches s_entries[] order (A, B, X, Y, Start, Z, L, R, Stick*, CStick*, DPad*) */
const char*  pc_keybinding_label(int idx);  /* human-readable name, e.g. "Stick Up" */
PCInputCode* pc_keybinding_ptr(int idx);    /* pointer into g_pc_keybindings for get/set */

#ifdef __cplusplus
}
#endif

#endif /* PC_KEYBINDINGS_H */
