// Tests the translation catalog: unique keys, matching format arguments, glyph coverage of the embedded font, and
// the UTF-8 aware line splitting. Run from 3ds/app (it reads assets/).
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "i18n.h"

#define MAX_CODEPOINT 0x10000

// The conversions of a printf format in order, e.g. "%lu FPS / Q%d" -> "lu d".
static void conversions(const char *format, char *out, size_t size) {
    size_t at = 0;
    for (const char *p = format; *p; p++) {
        if (*p != '%') {
            continue;
        }
        p++;
        if (*p == '%') {
            continue;
        }
        while (*p && (*p == '0' || (*p >= '1' && *p <= '9') || *p == 'l')) {
            assert(at + 2 < size);
            out[at++] = *p++;
        }
        assert(*p && strchr("sudXx", *p) && at + 2 < size);
        out[at++] = *p;
        out[at++] = ' ';
    }
    out[at] = 0;
}

// Next code point of a UTF-8 string, advancing *p.
static unsigned next_codepoint(const unsigned char **p) {
    unsigned c = *(*p)++;
    if (c < 0x80) {
        return c;
    }
    unsigned extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
    c &= (1u << (6 - extra)) - 1;
    while (extra--) {
        assert((**p & 0xC0) == 0x80);
        c = (c << 6) | (*(*p)++ & 63);
    }
    return c;
}

static void test_catalog(void) {
    assert(native_translation_count > 70);
    for (unsigned i = 0; i < native_translation_count; i++) {
        for (unsigned j = i + 1; j < native_translation_count; j++) {
            assert(strcmp(native_translations[i].en, native_translations[j].en) != 0);
        }
        char english[64], chinese[64];
        conversions(native_translations[i].en, english, sizeof(english));
        conversions(native_translations[i].zh, chinese, sizeof(chinese));
        if (strcmp(english, chinese) != 0) {
            fprintf(stderr, "format arguments differ: %s\n", native_translations[i].en);
            abort();
        }
    }
}

// Every character of every translation has a glyph in the font that is embedded in the program.
static void test_glyph_coverage(void) {
    static unsigned char covered[MAX_CODEPOINT];
    FILE *list = fopen("assets/glyphs.txt", "r");
    assert(list);
    unsigned code;
    while (fscanf(list, "%x", &code) == 1) {
        assert(code < MAX_CODEPOINT);
        covered[code] = 1;
    }
    fclose(list);
    for (unsigned i = 0; i < native_translation_count; i++) {
        for (const unsigned char *p = (const unsigned char *)native_translations[i].zh; *p;) {
            unsigned c = next_codepoint(&p);
            if (c >= MAX_CODEPOINT || !covered[c]) {
                fprintf(stderr, "no glyph for U+%04X in: %s\n", c, native_translations[i].zh);
                abort();
            }
        }
    }
    FILE *font = fopen("assets/ui-font.bcfnt", "rb");
    assert(font);
    char magic[4];
    assert(fread(magic, 1, 4, font) == 4 && !memcmp(magic, "CFNT", 4));
    fclose(font);
}

static void test_lookup_and_split(void) {
    assert(!strcmp(native_tr(NATIVE_LANG_EN, "READY"), "READY"));
    assert(!strcmp(native_tr(NATIVE_LANG_ZH_TW, "READY"), "準備就緒"));
    assert(!strcmp(native_tr(2, "unknown"), "unknown")); // no such language: the English text
    assert(!strcmp(native_tr(NATIVE_LANG_ZH_TW, "not in the catalog"), "not in the catalog"));
    const char *value = "這是一段很長的繁體中文狀態訊息，用來檢查換行不會切斷中文字元，並且完整保留後續內容。";
    char first[128], second[160], joined[288];
    native_text_split(value, first, sizeof(first), second, sizeof(second));
    assert(first[0] && second[0] && ((unsigned char)second[0] & 0xC0) != 0x80); // never inside a character
    snprintf(joined, sizeof(joined), "%s%s", first, second);
    assert(!strcmp(joined, value));
}

int main(void) {
    test_catalog();
    test_glyph_coverage();
    test_lookup_and_split();
    puts("PASS: English/Traditional Chinese catalog, format argument parity, embedded glyph coverage and UTF-8 line "
         "boundaries");
}
