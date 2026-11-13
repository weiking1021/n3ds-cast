#pragma once
#include "protocol.h"
#include "discovery.h"
typedef struct {
    CastConfig cfg;
    NativeStatus status;
    NativeTV tvs[NATIVE_TV_LIMIT];
    unsigned tv_count, selected_tv, selected_setting, tab;
    bool scanning, connected, show_timings;
    unsigned cpu_core, capture_source;
    unsigned language;
    char selected_name[64], notice[160];
} NativeUI;
bool native_ui_init(void);
void native_ui_draw(const NativeUI *ui);
void native_ui_exit(void);
