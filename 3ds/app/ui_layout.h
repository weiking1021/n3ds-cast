#pragma once
#define NATIVE_SETTINGS_COUNT 13u
#define NATIVE_SETTINGS_PER_PAGE 4u
#define NATIVE_TVS_PER_PAGE 2u
static inline int native_ui_tab_at(unsigned x, unsigned y) {
    return x < 320 && y < 43 ? (int)(x / 107 > 2 ? 2 : x / 107) : -1;
}
static inline int native_ui_page_at(unsigned x, unsigned y) {
    if (y < 210 || y >= 240) {
        return 0;
    }
    return x >= 12 && x < 78 ? -1 : x >= 242 && x < 308 ? 1 : 0;
}
static inline unsigned native_ui_setting_page(unsigned selected, int direction) {
    unsigned pages = (NATIVE_SETTINGS_COUNT + NATIVE_SETTINGS_PER_PAGE - 1) / NATIVE_SETTINGS_PER_PAGE;
    return ((selected / NATIVE_SETTINGS_PER_PAGE + pages + direction) % pages) * NATIVE_SETTINGS_PER_PAGE;
}
static inline int native_ui_setting_at(unsigned selected, unsigned x, unsigned y) {
    if (x < 12 || x >= 308 || y < 46 || y >= 202) {
        return -1;
    }
    unsigned row = (y - 46) / 40;
    if ((y - 46) % 40 >= 36) {
        return -1;
    }
    unsigned index = (selected / NATIVE_SETTINGS_PER_PAGE) * NATIVE_SETTINGS_PER_PAGE + row;
    return index < NATIVE_SETTINGS_COUNT ? (int)index : -1;
}
static inline unsigned native_ui_device_page(unsigned selected, unsigned count, int direction) {
    if (!count) {
        return 0;
    }
    unsigned pages = (count + NATIVE_TVS_PER_PAGE - 1) / NATIVE_TVS_PER_PAGE;
    return ((selected / NATIVE_TVS_PER_PAGE + pages + direction) % pages) * NATIVE_TVS_PER_PAGE;
}
static inline int native_ui_device_at(unsigned selected, unsigned count, unsigned x, unsigned y) {
    if (x < 12 || x >= 308 || y < 78 || y >= 202) {
        return -1;
    }
    unsigned row = (y - 78) / 66;
    if ((y - 78) % 66 >= 58) {
        return -1;
    }
    unsigned index = (selected / NATIVE_TVS_PER_PAGE) * NATIVE_TVS_PER_PAGE + row;
    return index < count ? (int)index : -1;
}
