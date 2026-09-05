#ifndef TRAINING_CONFIG_H
#define TRAINING_CONFIG_H

#include "types.h"

#include <stdbool.h>

/// Load training settings from disk into Training[0] and Training[2].
/// If file is missing or corrupt, does nothing (caller should init defaults first).
/// Returns true if settings were loaded successfully.
bool TrainingConfig_Load(void);

/// Save current training settings (Training[2]) to disk, along with the live
/// Cursor_X/Cursor_Y/Super_Arts/My_char.
///
/// DEBUG builds refuse the write for a test session that owns the selection
/// -- an input script, or any `--test-enable` run other than Quick Training.
/// Those sessions choose their own characters/arts/stage, and every one of
/// them that reaches menu.c -> Setup_NTr_Data() would otherwise persist that
/// choice over the maintainer's real file; measured, and it happened. Quick
/// Training is exempt because its contract is that the shipped save leaves
/// the file byte-identical, which a suppressed write would satisfy vacuously.
/// See test_session_owns_training_config() in training_config.c.
void TrainingConfig_Save(void);

/// Restore saved character select cursor positions and SA selections.
/// Call before character select starts in training mode.
void TrainingConfig_RestoreCharSelect(void);

#if defined(DEBUG)
/// Harness seam: read the clamped on-disk contents block WITHOUT installing it
/// into Training[]. False when there is no readable config.
///
/// Exists because the two ways Quick Training could get the training settings
/// wrong are BOTH invisible to a liveness check: never loading them (the match
/// runs on zeros) and, worse, then flushing those zeros back over the user's
/// file, since menu.c -> Setup_NTr_Data() opens with TrainingConfig_Save().
///
/// A caller must SNAPSHOT before the run and compare after. Comparing the live
/// Training[] against a fresh read at the end is a trap and was measured as
/// one: a run that never loaded the config saves zeros over the file, so the
/// zeroed file and the zeroed globals agree and the check passes on exactly
/// the failure it exists to catch.
bool TrainingConfig_ReadDiskContents(s8 out[2][2][7]);
#endif

/// Read the last-used characters and super arts off the on-disk training
/// config (the same file TrainingConfig_RestoreCharSelect reads; written on
/// every training soft reset / pause save / character change). Each output
/// slot is written only when the stored value is in range (characters
/// 0..NUM_CHARS-1, arts 0..2), so callers pre-fill defaults. Returns false
/// without touching the outputs when the file is missing or unreadable.
/// Consumer: the Quick Training jump (src/quick_training.c).
bool TrainingConfig_GetLastUsed(s8 chars_out[2], s8 arts_out[2]);

#endif
