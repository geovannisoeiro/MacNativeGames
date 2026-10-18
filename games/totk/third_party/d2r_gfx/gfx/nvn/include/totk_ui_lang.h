/* Language of the host's own text on screen (boot screen, warm-up progress, overlays, keyboard prompt): Chinese when the
   Mac's language is Chinese, Portuguese when it is Portuguese, English otherwise. TOTK_UI_LANG=zh, en or pt overrides it
   (read on every call, so tests can switch). The game's own language is chosen separately (TOTK_LANG, services.c
   desired_language).

   TOTK_UI(zh, en) is the original two-arm form; TOTK_UI(zh, en, pt) adds the Portuguese arm. A call site can be upgraded
   one at a time, and every site that has no Portuguese string yet falls back to English under TOTK_UI_LANG=pt. */
#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
#include <string.h>

enum { TOTK_UI_ZH, TOTK_UI_EN, TOTK_UI_PT };
static inline int totk_ui_of(const char *tag) {
    return !strncmp(tag, "zh", 2) ? TOTK_UI_ZH
         : !strncmp(tag, "pt", 2) ? TOTK_UI_PT
         : !strncmp(tag, "en", 2) ? TOTK_UI_EN : -1;
}
static inline int totk_ui_lang(void) {
    const char *e = getenv("TOTK_UI_LANG");
    if (e && *e) { int v = totk_ui_of(e); if (v >= 0) return v; }
    static int system_lang = -1;
    if (system_lang < 0) {
        /* The Mac's *ordered* language preferences, not just the locale's language code: a Mac set to
           "en_BR" with Portuguese second reports languageCode "en", so looking only at that would never
           reach Portuguese on a machine that lists it. First preference we have a language for wins. */
        int pick = -1;
        CFArrayRef pref = (CFArrayRef)CFPreferencesCopyAppValue(CFSTR("AppleLanguages"), kCFPreferencesAnyApplication);
        if (pref && CFGetTypeID(pref) == CFArrayGetTypeID()) {
            for (CFIndex i = 0; i < CFArrayGetCount(pref) && pick < 0; i++) {
                CFStringRef s = (CFStringRef)CFArrayGetValueAtIndex(pref, i);
                char tag[16] = {0};
                if (s && CFGetTypeID(s) == CFStringGetTypeID() && CFStringGetCString(s, tag, sizeof tag, kCFStringEncodingUTF8))
                    pick = totk_ui_of(tag);
            }
        }
        if (pref) CFRelease(pref);
        if (pick < 0) {
            char language[16] = {0};
            CFLocaleRef locale = CFLocaleCopyCurrent();
            CFStringRef value = CFLocaleGetValue(locale, kCFLocaleLanguageCode);
            if (value) CFStringGetCString(value, language, sizeof language, kCFStringEncodingUTF8);
            CFRelease(locale);
            pick = totk_ui_of(language);
        }
        system_lang = pick >= 0 ? pick : TOTK_UI_EN;
    }
    return system_lang;
}
#define TOTK_UI_PICK(_1, _2, _3, NAME, ...) NAME
#define TOTK_UI_2(zh, en) (totk_ui_lang() == TOTK_UI_ZH ? (zh) : (en))
#define TOTK_UI_3(zh, en, pt) (totk_ui_lang() == TOTK_UI_ZH ? (zh) \
                             : totk_ui_lang() == TOTK_UI_PT ? (pt) : (en))
#define TOTK_UI(...) TOTK_UI_PICK(__VA_ARGS__, TOTK_UI_3, TOTK_UI_2)(__VA_ARGS__)
