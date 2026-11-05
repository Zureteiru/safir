#ifndef SAFIR_GAMES_INI_H
#define SAFIR_GAMES_INI_H

#include <stdint.h>
#include <stdbool.h>
#include "safir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SAFIR_GAMES_INI_DEFAULT_PATH "sdmc:/config/safir/games.ini"

/**
 * Loads the saved dialogue box rectangle for a given Switch TitleID.
 * Returns true if a valid section and coordinates were found, false otherwise.
 * If false, caller falls back to safir_default_dialogue_rect().
 */
bool safir_games_ini_load(const char *ini_path, uint64_t title_id, SafirRect *out_rect);

/**
 * Saves or updates the dialogue box rectangle for a given Switch TitleID in the INI file.
 * Creates parent directory if possible and updates the section in-place or appends it.
 * Zero dynamic heap allocations.
 */
bool safir_games_ini_save(const char *ini_path, uint64_t title_id, SafirRect rect);

#ifdef __cplusplus
}
#endif

#endif /* SAFIR_GAMES_INI_H */
