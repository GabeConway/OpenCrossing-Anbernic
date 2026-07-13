/* pc_overlay.h - FPS counter + in-game settings menu */
#ifndef PC_OVERLAY_H
#define PC_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

void pc_overlay_init(void);
void pc_overlay_shutdown(void);
void pc_overlay_update(double fps, double speed);
void pc_overlay_draw(void);
void pc_overlay_boot_splash(const char* msg);

/* Settings menu (Select / Tab) */
void pc_overlay_menu_toggle(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_OVERLAY_H */
