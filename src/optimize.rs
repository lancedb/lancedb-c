// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Configurable table optimization
//!
//! `Table::optimize` runs exactly one [`OptimizeAction`]. The legacy
//! `lancedb_table_optimize` binds all four actions with the SDK defaults of each
//! action and discards the statistics. This module binds the same SDK operation
//! with the per-action options and the statistics the SDK reports, so a caller can
//! choose the options of the action it runs and read what the SDK did.
//!
//! Every option the SDK treats as optional keeps an explicit presence here: an
//! omitted option starts from the SDK default instead of a value that happens to
//! equal that default. The SDK alone decides which options a backend supports and
//! what it reports; this layer only converts the C representation.

use std::os::raw::{c_char, c_int};

use lancedb::table::{CompactionOptions, Duration, OptimizeAction, OptimizeOptions};

use crate::connection::{get_runtime, LanceDBTable};
use crate::error::{handle_error, set_custom_error_message, LanceDBError};

/// Which [`LanceDBCompactionOptions`] fields carry an override
///
/// These values mirror the `LanceDBCompactionField` enumerators of the public
/// header and tell an omitted option apart from a value equal to the SDK default.
const COMPACTION_TARGET_ROWS_PER_FRAGMENT: u32 = 1 << 0;
const COMPACTION_MAX_ROWS_PER_GROUP: u32 = 1 << 1;
const COMPACTION_MAX_BYTES_PER_FILE: u32 = 1 << 2;
const COMPACTION_MATERIALIZE_DELETIONS: u32 = 1 << 3;
const COMPACTION_MATERIALIZE_DELETIONS_THRESHOLD: u32 = 1 << 4;
const COMPACTION_NUM_THREADS: u32 = 1 << 5;
const COMPACTION_BATCH_SIZE: u32 = 1 << 6;
const COMPACTION_ALL_FIELDS: u32 = COMPACTION_TARGET_ROWS_PER_FRAGMENT
    | COMPACTION_MAX_ROWS_PER_GROUP
    | COMPACTION_MAX_BYTES_PER_FILE
    | COMPACTION_MATERIALIZE_DELETIONS
    | COMPACTION_MATERIALIZE_DELETIONS_THRESHOLD
    | COMPACTION_NUM_THREADS
    | COMPACTION_BATCH_SIZE;

/// Which [`LanceDBPruneOptions`] fields carry an override
///
/// These values mirror the `LanceDBPruneField` enumerators of the public header.
const PRUNE_OLDER_THAN: u32 = 1 << 0;
const PRUNE_DELETE_UNVERIFIED: u32 = 1 << 1;
const PRUNE_ERROR_IF_TAGGED_OLD_VERSIONS: u32 = 1 << 2;
const PRUNE_ALL_FIELDS: u32 =
    PRUNE_OLDER_THAN | PRUNE_DELETE_UNVERIFIED | PRUNE_ERROR_IF_TAGGED_OLD_VERSIONS;

/// Compaction overrides for `LANCEDB_OPTIMIZE_COMPACT`
///
/// `set_fields` selects the overrides; a field whose bit is not set keeps the
/// LanceDB default. The defaults are the ones of the SDK's `CompactionOptions`,
/// so this struct only reaches the SDK for the fields a caller set.
#[repr(C)]
#[derive(Debug, Copy, Clone, Default)]
pub struct LanceDBCompactionOptions {
    /// Bitmask of `LanceDBCompactionField` values; 0 uses every default
    pub set_fields: u32,
    /// Target number of rows per fragment
    pub target_rows_per_fragment: u64,
    /// Maximum number of rows per group in a written fragment
    pub max_rows_per_group: u64,
    /// Maximum number of bytes per output data file
    pub max_bytes_per_file: u64,
    /// Whether to rewrite fragments with deletions (0 = false, 1 = true)
    pub materialize_deletions: c_int,
    /// Fraction of deleted rows that triggers materializing deletions
    pub materialize_deletions_threshold: f32,
    /// Number of compaction tasks to run in parallel
    pub num_threads: u64,
    /// Batch size used when scanning the input fragments
    pub batch_size: u64,
}

/// Version pruning options for `LANCEDB_OPTIMIZE_PRUNE`
///
/// `set_fields` selects the overrides; a field whose bit is not set keeps the
/// SDK default. The SDK keeps an older version when its age is below
/// `older_than_nanos`, and keeps versions referenced by a tag unless
/// `error_if_tagged_old_versions` asks for an error instead.
#[repr(C)]
#[derive(Debug, Copy, Clone, Default)]
pub struct LanceDBPruneOptions {
    /// Bitmask of `LanceDBPruneField` values; 0 uses every default
    pub set_fields: u32,
    /// Minimum age of a prunable version in nanoseconds; not negative
    pub older_than_nanos: i64,
    /// Whether to delete files newer than the SDK's safety margin (0 = false, 1 = true)
    pub delete_unverified: c_int,
    /// Whether a tagged old version makes the prune fail (0 = false, 1 = true)
    pub error_if_tagged_old_versions: c_int,
}

/// One configurable optimize action
///
/// `action` selects the optimization and the matching option struct carries its
/// overrides; the other option struct is ignored, like the SDK ignores options of
/// an action it does not run. `action` is the raw C value of a
/// `LanceDBOptimizeType` enumerator so an out-of-range value is rejected instead
/// of becoming an invalid Rust enum.
#[repr(C)]
#[derive(Debug, Copy, Clone, Default)]
pub struct LanceDBOptimizeConfig {
    /// One of `LANCEDB_OPTIMIZE_ALL`, `LANCEDB_OPTIMIZE_COMPACT`,
    /// `LANCEDB_OPTIMIZE_PRUNE` or `LANCEDB_OPTIMIZE_INDEX`
    pub action: c_int,
    /// Overrides used by `LANCEDB_OPTIMIZE_COMPACT`
    pub compaction: LanceDBCompactionOptions,
    /// Overrides used by `LANCEDB_OPTIMIZE_PRUNE`
    pub prune: LanceDBPruneOptions,
}

/// Fragments and files that compaction rewrote
#[repr(C)]
#[derive(Debug, Copy, Clone, Default)]
pub struct LanceDBCompactionMetrics {
    /// Fragments that compaction removed
    pub fragments_removed: usize,
    /// Fragments that compaction added
    pub fragments_added: usize,
    /// Files that compaction removed, including deletion files
    pub files_removed: usize,
    /// Files that compaction added, including deletion files
    pub files_added: usize,
}

/// Files and versions that pruning removed
#[repr(C)]
#[derive(Debug, Copy, Clone, Default)]
pub struct LanceDBRemovalStats {
    /// Bytes of the removed files
    pub bytes_removed: u64,
    /// Old dataset versions that were removed
    pub old_versions: u64,
}

/// Statistics of one optimize action
///
/// The SDK reports compaction and pruning statistics only for the action that ran
/// them, so each group carries its own presence flag. The values of a group whose
/// flag is 0 are zero.
#[repr(C)]
#[derive(Debug, Copy, Clone, Default)]
pub struct LanceDBOptimizeStats {
    /// 1 when the action reported compaction metrics
    pub has_compaction: c_int,
    /// Compaction metrics; valid only when `has_compaction` is 1
    pub compaction: LanceDBCompactionMetrics,
    /// 1 when the action reported pruning metrics
    pub has_prune: c_int,
    /// Pruning metrics; valid only when `has_prune` is 1
    pub prune: LanceDBRemovalStats,
}

/// Build the SDK action from the configured action and its overrides
///
/// The action values are the `LanceDBOptimizeType` enumerators of the public
/// header. Matching the raw C value validates it before any Rust enum exists, and
/// it leaves the legacy `lancedb_table_optimize` dispatch unchanged.
fn build_optimize_action(config: &LanceDBOptimizeConfig) -> Result<OptimizeAction, String> {
    match config.action {
        // LANCEDB_OPTIMIZE_ALL
        0 => Ok(OptimizeAction::All),
        // LANCEDB_OPTIMIZE_COMPACT
        1 => Ok(OptimizeAction::Compact {
            options: build_compaction_options(&config.compaction)?,
            // The C API only exposes the SDK's default index remapping.
            remap_options: None,
        }),
        // LANCEDB_OPTIMIZE_PRUNE
        2 => {
            let prune = build_prune_options(&config.prune)?;
            Ok(OptimizeAction::Prune {
                older_than: prune.older_than,
                delete_unverified: prune.delete_unverified,
                error_if_tagged_old_versions: prune.error_if_tagged_old_versions,
            })
        }
        // LANCEDB_OPTIMIZE_INDEX; index rebuild options are not part of this call.
        3 => Ok(OptimizeAction::Index(OptimizeOptions::default())),
        other => Err(format!(
            "'action' value {} is not a LanceDBOptimizeType enumerator",
            other
        )),
    }
}

/// Build the SDK compaction options from the C overrides
fn build_compaction_options(
    options: &LanceDBCompactionOptions,
) -> Result<CompactionOptions, String> {
    check_set_fields("compaction", options.set_fields, COMPACTION_ALL_FIELDS)?;

    let mut compaction = CompactionOptions::default();
    if options.set_fields & COMPACTION_TARGET_ROWS_PER_FRAGMENT != 0 {
        compaction.target_rows_per_fragment = size_from_u64(
            "compaction.target_rows_per_fragment",
            options.target_rows_per_fragment,
        )?;
    }
    if options.set_fields & COMPACTION_MAX_ROWS_PER_GROUP != 0 {
        compaction.max_rows_per_group =
            size_from_u64("compaction.max_rows_per_group", options.max_rows_per_group)?;
    }
    if options.set_fields & COMPACTION_MAX_BYTES_PER_FILE != 0 {
        compaction.max_bytes_per_file = Some(size_from_u64(
            "compaction.max_bytes_per_file",
            options.max_bytes_per_file,
        )?);
    }
    if options.set_fields & COMPACTION_MATERIALIZE_DELETIONS != 0 {
        compaction.materialize_deletions = bool_from_c_int(
            "compaction.materialize_deletions",
            options.materialize_deletions,
        )?;
    }
    if options.set_fields & COMPACTION_MATERIALIZE_DELETIONS_THRESHOLD != 0 {
        compaction.materialize_deletions_threshold = options.materialize_deletions_threshold;
    }
    if options.set_fields & COMPACTION_NUM_THREADS != 0 {
        compaction.num_threads = Some(size_from_u64(
            "compaction.num_threads",
            options.num_threads,
        )?);
    }
    if options.set_fields & COMPACTION_BATCH_SIZE != 0 {
        compaction.batch_size = Some(size_from_u64("compaction.batch_size", options.batch_size)?);
    }
    Ok(compaction)
}

/// The SDK prune options, each `None` when the caller left it unset
struct PruneOptions {
    older_than: Option<Duration>,
    delete_unverified: Option<bool>,
    error_if_tagged_old_versions: Option<bool>,
}

/// Build the SDK prune options from the C overrides
fn build_prune_options(options: &LanceDBPruneOptions) -> Result<PruneOptions, String> {
    check_set_fields("prune", options.set_fields, PRUNE_ALL_FIELDS)?;

    let older_than = if options.set_fields & PRUNE_OLDER_THAN != 0 {
        if options.older_than_nanos < 0 {
            return Err(format!(
                "'prune.older_than_nanos' must not be negative, got {}",
                options.older_than_nanos
            ));
        }
        // Any non-negative nanosecond count fits the SDK's chrono::Duration.
        Some(Duration::nanoseconds(options.older_than_nanos))
    } else {
        None
    };
    let delete_unverified = if options.set_fields & PRUNE_DELETE_UNVERIFIED != 0 {
        Some(bool_from_c_int(
            "prune.delete_unverified",
            options.delete_unverified,
        )?)
    } else {
        None
    };
    let error_if_tagged_old_versions =
        if options.set_fields & PRUNE_ERROR_IF_TAGGED_OLD_VERSIONS != 0 {
            Some(bool_from_c_int(
                "prune.error_if_tagged_old_versions",
                options.error_if_tagged_old_versions,
            )?)
        } else {
            None
        };
    Ok(PruneOptions {
        older_than,
        delete_unverified,
        error_if_tagged_old_versions,
    })
}

/// Reject set bits that no option of this group uses
fn check_set_fields(group: &str, set_fields: u32, known: u32) -> Result<(), String> {
    let unknown = set_fields & !known;
    if unknown != 0 {
        return Err(format!("unknown {} option bits: 0x{:x}", group, unknown));
    }
    Ok(())
}

/// Convert a row or byte count from C to the SDK's `usize`
fn size_from_u64(name: &str, value: u64) -> Result<usize, String> {
    usize::try_from(value).map_err(|_| format!("'{}' value {} does not fit in size_t", name, value))
}

/// Convert a C boolean, rejecting values other than 0 and 1
fn bool_from_c_int(name: &str, value: c_int) -> Result<bool, String> {
    match value {
        0 => Ok(false),
        1 => Ok(true),
        other => Err(format!("'{}' must be 0 or 1, got {}", name, other)),
    }
}

/// Optimize the table with a configurable action and report its statistics
///
/// # Safety
/// - `table` must be a valid pointer returned from `lancedb_connection_open_table`
/// - `config` must point to a valid `LanceDBOptimizeConfig`
/// - `stats_out` must be a valid pointer to receive the statistics
/// - `error_message` can be NULL to ignore detailed error messages
///
/// # Returns
/// - `LANCEDB_SUCCESS` when the action ran; the statistics are written to
///   `stats_out`
/// - `LANCEDB_INVALID_ARGUMENT` when an argument is NULL or an option is invalid;
///   this happens before any native action, and `stats_out` is left unchanged
/// - Other error codes on failure, as mapped from the SDK error
#[no_mangle]
pub unsafe extern "C" fn lancedb_table_optimize_with_action(
    table: *const LanceDBTable,
    config: *const LanceDBOptimizeConfig,
    stats_out: *mut LanceDBOptimizeStats,
    error_message: *mut *mut c_char,
) -> LanceDBError {
    if table.is_null() || config.is_null() || stats_out.is_null() {
        set_custom_error_message(
            error_message,
            "Invalid argument: table, config and stats_out must not be NULL",
        );
        return LanceDBError::InvalidArgument;
    }

    let action = match build_optimize_action(&*config) {
        Ok(action) => action,
        Err(message) => {
            set_custom_error_message(error_message, &message);
            return LanceDBError::InvalidArgument;
        }
    };

    let tbl = &(*table).inner;
    let runtime = get_runtime();

    match runtime.block_on(tbl.optimize(action)) {
        Ok(stats) => {
            *stats_out = LanceDBOptimizeStats::default();
            if let Some(compaction) = stats.compaction {
                (*stats_out).has_compaction = 1;
                (*stats_out).compaction = LanceDBCompactionMetrics {
                    fragments_removed: compaction.fragments_removed,
                    fragments_added: compaction.fragments_added,
                    files_removed: compaction.files_removed,
                    files_added: compaction.files_added,
                };
            }
            if let Some(prune) = stats.prune {
                (*stats_out).has_prune = 1;
                (*stats_out).prune = LanceDBRemovalStats {
                    bytes_removed: prune.bytes_removed,
                    old_versions: prune.old_versions,
                };
            }
            LanceDBError::Success
        }
        Err(e) => handle_error(&e, error_message),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The prune age crosses the C boundary as nanoseconds and reaches the SDK
    /// unchanged, with its presence bit deciding whether the SDK sees an age.
    #[test]
    fn prune_keeps_the_configured_nanosecond_age() {
        let config = LanceDBOptimizeConfig {
            action: 2, // LANCEDB_OPTIMIZE_PRUNE
            prune: LanceDBPruneOptions {
                set_fields: PRUNE_OLDER_THAN,
                older_than_nanos: 500_000_000,
                ..Default::default()
            },
            ..Default::default()
        };

        match build_optimize_action(&config) {
            Ok(OptimizeAction::Prune {
                older_than,
                delete_unverified,
                error_if_tagged_old_versions,
            }) => {
                assert_eq!(older_than, Some(Duration::nanoseconds(500_000_000)));
                assert_eq!(delete_unverified, None);
                assert_eq!(error_if_tagged_old_versions, None);
            }
            Ok(_) => panic!("expected a prune action"),
            Err(message) => panic!("expected a prune action, got error: {message}"),
        }

        // The value alone does not set the age; the presence bit does.
        let without_age = LanceDBOptimizeConfig {
            prune: LanceDBPruneOptions {
                set_fields: 0,
                ..config.prune
            },
            ..config
        };
        match build_optimize_action(&without_age) {
            Ok(OptimizeAction::Prune { older_than, .. }) => assert_eq!(older_than, None),
            Ok(_) => panic!("expected a prune action"),
            Err(message) => panic!("expected a prune action, got error: {message}"),
        }
    }
}
