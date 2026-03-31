#include "pc_keybindings.h"
#include "pc_platform.h"

static const PCKeybindings s_defaults = {
    /* buttons */
    .a     = SDL_SCANCODE_SPACE,
    .b     = SDL_SCANCODE_LSHIFT,
    .x     = SDL_SCANCODE_X,
    .y     = SDL_SCANCODE_Y,
    .start = SDL_SCANCODE_RETURN,
    .z     = SDL_SCANCODE_Z,
    .l     = SDL_SCANCODE_Q,
    .r     = SDL_SCANCODE_E,

    /* main stick (WASD) */
    .stick_up    = SDL_SCANCODE_W,
    .stick_down  = SDL_SCANCODE_S,
    .stick_left  = SDL_SCANCODE_A,
    .stick_right = SDL_SCANCODE_D,

    /* C-stick (arrows) */
    .cstick_up    = SDL_SCANCODE_UP,
    .cstick_down  = SDL_SCANCODE_DOWN,
    .cstick_left  = SDL_SCANCODE_LEFT,
    .cstick_right = SDL_SCANCODE_RIGHT,

    /* D-pad (IJKL) */
    .dpad_up    = SDL_SCANCODE_I,
    .dpad_down  = SDL_SCANCODE_K,
    .dpad_left  = SDL_SCANCODE_J,
    .dpad_right = SDL_SCANCODE_L,
};

PCKeybindings g_pc_keybindings;

static const char* KEYBINDINGS_FILE = "keybindings.ini";

/* mapping table: ini key name -> offset into PCKeybindings */
typedef struct {
    const char* ini_key;
    int offset; /* byte offset into PCKeybindings */
} KeybindEntry;

#define KB_ENTRY(name, field) { name, offsetof(PCKeybindings, field) }

static const KeybindEntry s_entries[] = {
    KB_ENTRY("A",            a),
    KB_ENTRY("B",            b),
    KB_ENTRY("X",            x),
    KB_ENTRY("Y",            y),
    KB_ENTRY("Start",        start),
    KB_ENTRY("Z",            z),
    KB_ENTRY("L",            l),
    KB_ENTRY("R",            r),
    KB_ENTRY("Stick_Up",     stick_up),
    KB_ENTRY("Stick_Down",   stick_down),
    KB_ENTRY("Stick_Left",   stick_left),
    KB_ENTRY("Stick_Right",  stick_right),
    KB_ENTRY("CStick_Up",    cstick_up),
    KB_ENTRY("CStick_Down",  cstick_down),
    KB_ENTRY("CStick_Left",  cstick_left),
    KB_ENTRY("CStick_Right", cstick_right),
    KB_ENTRY("DPad_Up",      dpad_up),
    KB_ENTRY("DPad_Down",    dpad_down),
    KB_ENTRY("DPad_Left",    dpad_left),
    KB_ENTRY("DPad_Right",   dpad_right),
};

#define NUM_ENTRIES (sizeof(s_entries) / sizeof(s_entries[0]))

/* mouse button name table */
typedef struct {
    const char* name;
    PCInputCode code;
} MouseButtonEntry;

static const MouseButtonEntry s_mouse_buttons[] = {
    { "Mouse1", PC_INPUT_MOUSE1 },
    { "Mouse2", PC_INPUT_MOUSE2 },
    { "Mouse3", PC_INPUT_MOUSE3 },
};
#define NUM_MOUSE_BUTTONS (sizeof(s_mouse_buttons) / sizeof(s_mouse_buttons[0]))

static const char* skip_ws(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void trim_end(char* s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                       s[len-1] == '\r' || s[len-1] == '\n')) {
        s[--len] = '\0';
    }
}

/* get display name for an input code (-1 = unbound → "None") */
static const char* input_code_name(PCInputCode code) {
    static char gpbuf[24];
    if (code < 0) return "None";
    if ((unsigned)code & PC_INPUT_GAMEPAD_BIT) {
        const char* s = SDL_GameControllerGetStringForButton(
            (SDL_GameControllerButton)(code & 0xFF));
        snprintf(gpbuf, sizeof(gpbuf), "GP_%s", s ? s : "unknown");
        return gpbuf;
    }
    if ((unsigned)code & PC_INPUT_MOUSE_BIT) {
        for (int i = 0; i < (int)NUM_MOUSE_BUTTONS; i++) {
            if (s_mouse_buttons[i].code == code)
                return s_mouse_buttons[i].name;
        }
        return "Unknown";
    }
    return SDL_GetScancodeName((SDL_Scancode)code);
}

/* parse an input value string to a PCInputCode */
static PCInputCode parse_input_code(const char* value) {
    /* "None" = explicitly unbound */
    if (SDL_strcasecmp(value, "None") == 0) return -1;

    /* check mouse button names first (case-insensitive) */
    for (int i = 0; i < (int)NUM_MOUSE_BUTTONS; i++) {
        if (SDL_strcasecmp(value, s_mouse_buttons[i].name) == 0) {
            return s_mouse_buttons[i].code;
        }
    }

    /* gamepad button: "GP_" prefix + SDL controller button name (e.g. GP_a, GP_leftshoulder) */
    if (SDL_strncasecmp(value, "GP_", 3) == 0) {
        SDL_GameControllerButton btn = SDL_GameControllerGetButtonFromString(value + 3);
        if (btn != SDL_CONTROLLER_BUTTON_INVALID)
            return PC_INPUT_GAMEPAD_BTN(btn);
        return -1;
    }

    /* fall back to SDL scancode */
    SDL_Scancode sc = SDL_GetScancodeFromName(value);
    if (sc != SDL_SCANCODE_UNKNOWN) {
        return (PCInputCode)sc;
    }

    return -1; /* invalid */
}

static void apply_keybind(const char* key, const char* value) {
    PCInputCode code = parse_input_code(value);
    if (code < 0 && SDL_strcasecmp(value, "None") != 0) {
        printf("[Keybindings] WARNING: unknown key name '%s' for '%s'\n", value, key);
        return;
    }

    for (int i = 0; i < (int)NUM_ENTRIES; i++) {
        if (strcmp(key, s_entries[i].ini_key) == 0) {
            *(PCInputCode*)((char*)&g_pc_keybindings + s_entries[i].offset) = code;
            return;
        }
    }
    printf("[Keybindings] WARNING: unknown binding '%s'\n", key);
}

static void write_defaults(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "[Keyboard]\n");
    fprintf(f, "# Key names use SDL2 scancode names.\n");
    fprintf(f, "# Common names: Space, Left Shift, Right Shift, Left Ctrl, Right Ctrl,\n");
    fprintf(f, "#   Left Alt, Right Alt, Return, Escape, Tab, Backspace, Delete,\n");
    fprintf(f, "#   A-Z, 0-9, F1-F12, Up, Down, Left, Right, etc.\n");
    fprintf(f, "# Mouse buttons: Mouse1 (left), Mouse2 (right), Mouse3 (middle)\n");
    fprintf(f, "# Full list: https://wiki.libsdl.org/SDL2/SDL_Scancode\n");
    fprintf(f, "\n");
    fprintf(f, "# Buttons\n");

    for (int i = 0; i < (int)NUM_ENTRIES; i++) {
        PCInputCode code = *(PCInputCode*)((char*)&g_pc_keybindings + s_entries[i].offset);
        const char* name = input_code_name(code);
        fprintf(f, "%s = %s\n", s_entries[i].ini_key, name);

        /* blank line separators between sections */
        if (i == 7)  fprintf(f, "\n# Main Stick\n");
        if (i == 11) fprintf(f, "\n# C-Stick (Camera)\n");
        if (i == 15) fprintf(f, "\n# D-Pad\n");
    }

    fclose(f);
}

void pc_keybindings_save(void) {
    write_defaults(KEYBINDINGS_FILE);
}

void pc_keybindings_reset(void) {
    g_pc_keybindings = s_defaults;
    pc_keybindings_save();
}

int pc_keybindings_uses_gamepad(void) {
    int i;
    for (i = 0; i < (int)NUM_ENTRIES; i++) {
        PCInputCode* p = pc_keybinding_ptr(i);
        if (p && ((unsigned)*p & PC_INPUT_GAMEPAD_BIT)) return 1;
    }
    return 0;
}

const char* pc_keybinding_label(int idx) {
    if (idx < 0 || idx >= (int)NUM_ENTRIES) return "";
    /* Replace underscores with spaces for display */
    static char buf[32];
    const char* src = s_entries[idx].ini_key;
    int i;
    for (i = 0; i < (int)(sizeof(buf) - 1) && src[i]; i++)
        buf[i] = (src[i] == '_') ? ' ' : src[i];
    buf[i] = '\0';
    return buf;
}

PCInputCode* pc_keybinding_ptr(int idx) {
    if (idx < 0 || idx >= (int)NUM_ENTRIES) return NULL;
    return (PCInputCode*)((char*)&g_pc_keybindings + s_entries[idx].offset);
}

void pc_keybindings_load(void) {
    g_pc_keybindings = s_defaults; /* start from defaults; file overrides only what it specifies */
    FILE* f = fopen(KEYBINDINGS_FILE, "r");
    if (!f) {
        write_defaults(KEYBINDINGS_FILE);
        printf("[Keybindings] Created default %s\n", KEYBINDINGS_FILE);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        const char* p = skip_ws(line);

        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\n' || *p == '[')
            continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = (char*)skip_ws(line);
        trim_end(key);
        char* value = (char*)skip_ws(eq + 1);
        trim_end(value);

        if (*key && *value) {
            apply_keybind(key, value);
        }
    }
    fclose(f);
    printf("[Keybindings] Loaded %s\n", KEYBINDINGS_FILE);
}
