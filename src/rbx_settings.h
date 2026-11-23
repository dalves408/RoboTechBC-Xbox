#ifndef RBX_SETTINGS_H
#define RBX_SETTINGS_H
void rbx_settings_install(void);
int rbx_settings_key(unsigned key, long flags, int down);
void rbx_settings_tick(void);
int rbx_settings_input_blocked(void);
void rbx_settings_wait(void);
unsigned rbx_settings_video(void);
void rbx_settings_msaa_supported(unsigned mask);
#endif
