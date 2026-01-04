/* fd_geyser_cxx_api.h - Minimal C++ safe API for fd-geyser
 *
 * This header provides access to types and function declarations
 * needed by C++ code, by including only C++ compatible headers
 * and manually defining types from incompatible headers.
 *
 * IMPORTANT: This header must be included INSIDE extern "C" { }
 */
#ifndef FD_GEYSER_CXX_API_H
#define FD_GEYSER_CXX_API_H

/* These headers ARE C++ compatible (with -Wno-error for minor issues) */
#include "../../funk/fd_funk.h"
#include "../../ballet/txn/fd_txn.h"
#include "../../flamenco/types/fd_types_custom.h"  /* For fd_pubkey_t, fd_hash_t */
#include "../../disco/fd_txn_p.h"

/* Helper function for funk account lookups (from fd_acc_mgr.h) */
static inline fd_funk_rec_key_t
fd_funk_acc_key( fd_pubkey_t const * pubkey ) {
  fd_funk_rec_key_t key = {0};
  memcpy( key.uc, pubkey, sizeof(fd_pubkey_t) );
  return key;
}

/* Types from headers that are NOT C++ compatible */

/* From discof/replay/fd_replay_tile.h */
#define REPLAY_SIG_SLOT_COMPLETED 0

struct fd_replay_slot_completed {
  ulong slot;
  ulong root_slot;
  ulong storage_slot;
  ulong epoch;
  ulong slot_in_epoch;
  ulong block_height;
  ulong parent_slot;

  fd_hash_t block_id;
  fd_hash_t parent_block_id;
  fd_hash_t bank_hash;
  fd_hash_t block_hash;

  ulong transaction_count;

  struct {
    double initial;
    double terminal;
    double taper;
    double foundation;
    double foundation_term;
  } inflation;

  struct {
    ulong lamports_per_uint8_year;
    double exemption_threshold;
    uchar burn_percent;
  } rent;

  ulong bank_idx;
  ulong parent_bank_idx;

  long first_fec_set_received_nanos;
  long preparation_begin_nanos;
  long first_transaction_scheduled_nanos;
  long last_transaction_finished_nanos;
  long completion_time_nanos;

  int is_leader;
  ulong identity_balance;

  struct {
    ulong block_cost;
    ulong vote_cost;
    ulong allocated_accounts_data_size;
    ulong block_cost_limit;
    ulong vote_cost_limit;
    ulong account_cost_limit;
  } cost_tracker;
};
typedef struct fd_replay_slot_completed fd_replay_slot_completed_t;

/* From discof/exec/fd_exec_geyser.h - uses fd_txn_p_t */
struct fd_exec_geyser_msg {
  ulong      slot;
  ulong      txn_idx;
  ulong      bank_idx;
  int        is_success;
  ulong      entry_idx;
  uint       is_last_txn_in_entry;
  uint       is_last_entry_in_slot;
  fd_txn_p_t txn;
};
typedef struct fd_exec_geyser_msg fd_exec_geyser_msg_t;

/* From discof/tower/fd_tower_tile.h */
#define FD_TOWER_SIG_SLOT_CONFIRMED 0
#define FD_TOWER_SLOT_CONFIRMED_OPTIMISTIC 1

struct fd_tower_slot_confirmed {
  ulong slot;
  int kind;
};
typedef struct fd_tower_slot_confirmed fd_tower_slot_confirmed_t;

/* fd_account_meta_t is defined in fd_flamenco_base.h (via fd_types_custom.h) */

/* Vote program ID - base58 decode of Vote111111111111111111111111111111111111111 */
static const uchar fd_geyser_vote_program_id[32] = {
    0x07U,0x61U,0x48U,0x1dU,0x35U,0x74U,0x74U,0xbbU,0x7cU,0x4dU,0x76U,0x24U,0xebU,0xd3U,0xbdU,0xb3U,
    0xd8U,0x35U,0x5eU,0x73U,0xd1U,0x10U,0x43U,0xfcU,0x0dU,0xa3U,0x53U,0x80U,0x00U,0x00U,0x00U,0x00U };

/* Geyser context and filter - opaque types */
struct fd_geyser_ctx;
typedef struct fd_geyser_ctx fd_geyser_ctx_t;

struct fd_geyser_filter;
typedef struct fd_geyser_filter fd_geyser_filter_t;

/* Geyser arguments */
#define FD_GEYSER_MAX_EXEC_TILES 64

struct fd_geyser_args {
  char funk_wksp[ 32 ];
  char replay_out_wksp[ 32 ];
  char tower_out_wksp[ 32 ];
  ulong exec_tile_cnt;
  char exec_geyser_wksp[ FD_GEYSER_MAX_EXEC_TILES ][ 32 ];
};
typedef struct fd_geyser_args fd_geyser_args_t;

/* Geyser context API */
fd_geyser_ctx_t * fd_geyser_init( fd_geyser_args_t * args );
void fd_geyser_loop( fd_geyser_ctx_t * ctx );
fd_geyser_filter_t * fd_geyser_get_filter( fd_geyser_ctx_t * ctx );
fd_funk_t * fd_geyser_get_funk( fd_geyser_ctx_t * ctx );

#endif /* FD_GEYSER_CXX_API_H */
