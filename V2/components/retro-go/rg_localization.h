#pragma once

#include <string.h>
#include <stdbool.h>

#define _(String) rg_gettext(String)

// Adding a language: add it here, add its name to language_names in
// translations.h, and fill in the matching [RG_LANG_xx] line for every entry in
// the translations table. Anything left out falls back to English.
// French was removed before the 1.1 release: only 183 of 288 strings had a
// translation, which left the menus half English and looked worse than not
// offering the language at all.
typedef enum
{
    RG_LANG_EN = 0,
  //RG_LANG_FR,
  //RG_LANG_ES,

    RG_LANG_MAX
} rg_language_t;

// Lookup function
const char *rg_gettext(const char *msg);
int rg_localization_get_language_id(void);
bool rg_localization_set_language_id(int language_id);
const char *rg_localization_get_language_name(int language_id);
