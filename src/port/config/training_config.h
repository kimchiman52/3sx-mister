#ifndef TRAINING_CONFIG_H
#define TRAINING_CONFIG_H

#include "types.h"

#include <stdbool.h>

/// Load training settings from disk into Training[0] and Training[2].
/// If file is missing or corrupt, does nothing (caller should init defaults first).
/// Returns true if settings were loaded successfully.
bool TrainingConfig_Load(void);

/// Save current training settings (Training[2]) to disk.
void TrainingConfig_Save(void);

/// Restore saved character select cursor positions and SA selections.
/// Call before character select starts in training mode.
void TrainingConfig_RestoreCharSelect(void);

/// Read the last-used characters and super arts off the on-disk training
/// config (the same file TrainingConfig_RestoreCharSelect reads; written on
/// every training soft reset / pause save / character change). Each output
/// slot is written only when the stored value is in range (characters
/// 0..NUM_CHARS-1, arts 0..2), so callers pre-fill defaults. Returns false
/// without touching the outputs when the file is missing or unreadable.
/// Consumer: the Quick Training jump (src/quick_training.c).
bool TrainingConfig_GetLastUsed(s8 chars_out[2], s8 arts_out[2]);

#endif
