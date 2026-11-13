#pragma once
#include <stddef.h>
enum {
    NATIVE_LANG_EN,
    NATIVE_LANG_ZH_TW,
    NATIVE_LANG_COUNT
};
// The catalog: English text (the key) and its Traditional Chinese translation.
typedef struct {
    const char *en, *zh;
} NativeTranslation;
extern const NativeTranslation native_translations[];
extern const unsigned native_translation_count;
const char *native_tr(unsigned language, const char *english);
void native_text_split(const char *value, char *first, size_t first_size, char *second, size_t second_size);
