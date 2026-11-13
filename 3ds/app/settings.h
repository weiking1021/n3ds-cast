#pragma once
#include "config.h"
#define NATIVE_CONFIG_PATH "sdmc:/3ds/n3ds-cast/native.ini"
void native_load_settings(CastConfig *cfg);

unsigned native_load_language(void);
bool native_load_show_timings(void);
unsigned native_load_cpu_core(void);       // 0=auto 1=core1 2=core2
unsigned native_load_capture_source(void); // 0=game 1=screen
bool native_save_settings(const CastConfig *cfg, unsigned language, bool show_timings, unsigned cpu_core,
                          unsigned capture_source);
