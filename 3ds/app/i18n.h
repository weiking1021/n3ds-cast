#pragma once
#include <stddef.h>
// English is the default; the setting is saved with the other settings.
enum {
    NATIVE_LANG_EN,
    NATIVE_LANG_ZH_TW,
    NATIVE_LANG_JA,
    NATIVE_LANG_COUNT
};
// The catalog: English text (the key) and its Traditional Chinese and Japanese translations.
typedef struct {
    const char *en, *zh, *ja;
} NativeTranslation;
extern const NativeTranslation native_translations[];
extern const unsigned native_translation_count;
const char *native_tr(unsigned language, const char *english);
// A language's own name for the settings screen ("English", "繁體中文", "日本語") and its language tag as the receiver
// page understands it ("en", "zh-TW", "ja").
const char *native_language_name(unsigned language);
const char *native_language_tag(unsigned language);
void native_text_split(const char *value, char *first, size_t first_size, char *second, size_t second_size);
