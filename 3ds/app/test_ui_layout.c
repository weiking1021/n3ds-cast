#include "ui_layout.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    unsigned reached = 0;
    for (unsigned page = 0; page < 4; page++) {
        for (unsigned row = 0; row < 4; row++) {
            int index = native_ui_setting_at(page * 4, 150, 51 + row * 40);
            if (page * 4 + row < 12) {
                assert(index == (int)(page * 4 + row));
                reached |= 1u << index;
            } else {
                assert(index == -1);
            }
        }
    }
    assert(reached == 0xFFF);
    assert(native_ui_setting_page(0, -1) == 8 && native_ui_setting_page(11, 1) == 0);
    assert(native_ui_setting_at(0, 150, 83) == -1 && native_ui_setting_at(0, 150, 212) == -1);
    for (unsigned i = 0; i < 8; i++) {
        assert(native_ui_device_at(i, 8, 150, 82 + (i % 2) * 66) == (int)i);
    }
    assert(native_ui_device_at(4, 5, 150, 150) == -1);
    assert(native_ui_device_page(0, 8, -1) == 6 && native_ui_device_page(7, 8, 1) == 0);
    assert(native_ui_page_at(20, 220) == -1 && native_ui_page_at(290, 220) == 1);
    assert(!native_ui_page_at(170, 220) && !native_ui_page_at(20, 200));
    assert(native_ui_tab_at(20, 30) == 0 && native_ui_tab_at(150, 30) == 1 && native_ui_tab_at(300, 30) == 2);
    assert(native_ui_tab_at(320, 30) == -1 && native_ui_tab_at(20, 43) == -1);
    puts("PASS: all 12 enlarged settings, three pages, two-TV pages, wraparound and touch gaps/footer/tab boundaries");
}
