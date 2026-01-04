#define _DEFAULT_SOURCE

#include "fd_geyser_poller.h"
#include "fd_geyser_filter.h"
#include "../../tango/fd_tango_base.h"
#include "../../util/wksp/fd_wksp_private.h"
#include "../../disco/topo/fd_topo.h"
#include "../../flamenco/runtime/fd_acc_mgr.h"
#include "../../ballet/txn/fd_txn.h"
#include "../../ballet/base58/fd_base58.h"
#include <unistd.h>

/* Instantiate sham_link for replay notifications (slot completion = Processed) */
#define SHAM_LINK_CONTEXT fd_geyser_ctx_t
#define SHAM_LINK_NAME    replay_sham_link
#include "sham_link.h"

/* Instantiate sham_link for tower notifications (slot confirmed = Confirmed) */
#define SHAM_LINK_CONTEXT fd_geyser_ctx_t
#define SHAM_LINK_NAME    tower_sham_link
#include "sham_link.h"

/* Instantiate sham_link for exec_geyser messages */
#define SHAM_LINK_CONTEXT fd_geyser_ctx_t
#define SHAM_LINK_NAME    exec_sham_link
#include "sham_link.h"

struct fd_geyser_ctx {
  fd_spad_t * spad;
  fd_funk_t   funk_ljoin[1];
  fd_funk_t * funk;

  /* Replay link for slot completion signals (Processed commitment) */
  replay_sham_link_t * replay_notify;

  /* Tower link for slot confirmed signals (Confirmed commitment) */
  tower_sham_link_t * tower_notify;

  /* Multiple exec_geyser links (one per exec tile) */
  ulong              exec_link_cnt;
  exec_sham_link_t * exec_links[ FD_GEYSER_MAX_EXEC_TILES ];

  /* Filter/subscription management */
  fd_geyser_filter_t * filter;

  /* Last slot completion info */
  fd_replay_slot_completed_t last_slot_completed;

  /* Current exec link being polled (for context in callbacks) */
  ulong current_exec_link_idx;
};

fd_geyser_ctx_t *
fd_geyser_init( fd_geyser_args_t * args ) {
  fd_geyser_ctx_t * ctx = (fd_geyser_ctx_t *)malloc(sizeof(fd_geyser_ctx_t));
  if( FD_UNLIKELY( !ctx ) ) FD_LOG_ERR(( "failed to allocate geyser context" ));
  memset( ctx, 0, sizeof(fd_geyser_ctx_t) );

  /* Attach to funk workspace with retry (Firedancer may still be starting) */
  fd_wksp_t * funk_wksp = NULL;
  FD_LOG_NOTICE(( "waiting for funk workspace \"%s\"...", args->funk_wksp ));
  for( int t = 0; t < 120; t++ ) {  /* Wait up to 2 minutes */
    funk_wksp = fd_wksp_attach( args->funk_wksp );
    if( funk_wksp ) break;
    sleep( 1 );
  }
  if( FD_UNLIKELY( !funk_wksp ) )
    FD_LOG_ERR(( "unable to attach to funk workspace \"%s\"", args->funk_wksp ));

  fd_wksp_tag_query_info_t info;
  ulong tag = 1;
  if( fd_wksp_tag_query( funk_wksp, &tag, 1, &info, 1 ) <= 0 ) {
    FD_LOG_ERR(( "workspace does not contain a funk" ));
  }
  void * funk_shmem = fd_wksp_laddr_fast( funk_wksp, info.gaddr_lo );

  /* Retry funk join (it may not be ready immediately) */
  for( int t = 0; t < 10; t++ ) {
    ctx->funk = fd_funk_join( ctx->funk_ljoin, funk_shmem );
    if( ctx->funk ) break;
    FD_LOG_WARNING(( "failed to join funk, retrying..." ));
    sleep( 1 );
  }
  if( FD_UNLIKELY( !ctx->funk ) )
    FD_LOG_ERR(( "failed to join funk after retries" ));

  /* Allocate scratch pad for temporary allocations */
#define SMAX (1LU<<30)
  uchar * smem = aligned_alloc( FD_SPAD_ALIGN, SMAX );
  ctx->spad = fd_spad_join( fd_spad_new( smem, SMAX ) );
  fd_spad_push( ctx->spad );

  /* Create filter */
  ctx->filter = fd_geyser_filter_create( ctx->spad, ctx->funk );

  /* Attach to replay_out workspace (slot completion = Processed) */
  ctx->replay_notify = replay_sham_link_new(
    aligned_alloc( replay_sham_link_align(), replay_sham_link_footprint() ),
    args->replay_out_wksp );

  /* Attach to tower_out workspace (slot confirmed = Confirmed) */
  ctx->tower_notify = tower_sham_link_new(
    aligned_alloc( tower_sham_link_align(), tower_sham_link_footprint() ),
    args->tower_out_wksp );

  /* Attach to all exec_geyser workspaces */
  ctx->exec_link_cnt = args->exec_tile_cnt;
  if( ctx->exec_link_cnt > FD_GEYSER_MAX_EXEC_TILES ) {
    FD_LOG_ERR(( "too many exec tiles: %lu > %d", ctx->exec_link_cnt, FD_GEYSER_MAX_EXEC_TILES ));
  }

  for( ulong i = 0; i < ctx->exec_link_cnt; i++ ) {
    FD_LOG_NOTICE(( "attaching to exec_geyser workspace %lu: %s", i, args->exec_geyser_wksp[i] ));
    ctx->exec_links[i] = exec_sham_link_new(
      aligned_alloc( exec_sham_link_align(), exec_sham_link_footprint() ),
      args->exec_geyser_wksp[i] );
  }

  FD_LOG_NOTICE(( "fd-geyser initialized with %lu exec links", ctx->exec_link_cnt ));
  return ctx;
}

fd_geyser_filter_t *
fd_geyser_get_filter( fd_geyser_ctx_t * ctx ) {
  return ctx->filter;
}

fd_funk_t *
fd_geyser_get_funk( fd_geyser_ctx_t * ctx ) {
  return ctx->funk;
}

void
fd_geyser_loop( fd_geyser_ctx_t * ctx ) {
  /* Start all links */
  replay_sham_link_start( ctx->replay_notify );
  tower_sham_link_start( ctx->tower_notify );
  for( ulong i = 0; i < ctx->exec_link_cnt; i++ ) {
    exec_sham_link_start( ctx->exec_links[i] );
  }

  FD_LOG_NOTICE(( "fd-geyser polling loop started" ));

  while( 1 ) {
    /* Poll replay_out for slot completion signals (Processed commitment) */
    replay_sham_link_poll( ctx->replay_notify, ctx );

    /* Poll tower_out for slot confirmed signals (Confirmed commitment) */
    tower_sham_link_poll( ctx->tower_notify, ctx );

    /* Poll all exec_geyser links for real-time transaction data */
    for( ulong i = 0; i < ctx->exec_link_cnt; i++ ) {
      ctx->current_exec_link_idx = i;
      exec_sham_link_poll( ctx->exec_links[i], ctx );
    }
  }
}

/* Callback for replay link messages */
void
replay_sham_link_during_frag( fd_geyser_ctx_t * ctx, ulong sig, ulong ctl, void const * msg, ulong sz ) {
  (void)ctl;

  /* Only handle slot completion signals */
  if( FD_UNLIKELY( sig != REPLAY_SIG_SLOT_COMPLETED ) ) return;

  FD_TEST( sz == sizeof(fd_replay_slot_completed_t) );
  memcpy( &ctx->last_slot_completed, msg, sz );
}

void
replay_sham_link_after_frag( fd_geyser_ctx_t * ctx, ulong sig ) {
  if( FD_UNLIKELY( sig != REPLAY_SIG_SLOT_COMPLETED ) ) return;

  fd_replay_slot_completed_t * slot_msg = &ctx->last_slot_completed;
  FD_LOG_INFO(( "slot %lu completed (Processed)", slot_msg->slot ));

  FD_LOG_DEBUG(( "[SHM_RECV] slot_completed slot=%lu bank_idx=%lu block_height=%lu txn_cnt=%lu",
                       slot_msg->slot,
                       slot_msg->bank_idx,
                       slot_msg->block_height,
                       slot_msg->transaction_count ));

  /* Notify filter about slot completion (Processed commitment) */
  fd_geyser_filter_notify_slot_completed( ctx->filter, slot_msg );
}

/* Callback for tower link messages (Confirmed commitment) */
void
tower_sham_link_during_frag( fd_geyser_ctx_t * ctx, ulong sig, ulong ctl, void const * msg, ulong sz ) {
  (void)ctl;

  /* Only handle slot confirmed signals */
  if( FD_UNLIKELY( sig != FD_TOWER_SIG_SLOT_CONFIRMED ) ) return;

  FD_TEST( sz == sizeof(fd_tower_slot_confirmed_t) );
  fd_tower_slot_confirmed_t const * confirmed = (fd_tower_slot_confirmed_t const *)msg;

  FD_LOG_DEBUG(( "[SHM_RECV] slot_confirmed slot=%lu kind=%d",
                       confirmed->slot, confirmed->kind ));

  /* Only process CLUSTER confirmations (2/3+ stake voted, may arrive before replay) */
  if( confirmed->kind == FD_TOWER_SLOT_CONFIRMED_CLUSTER ) {
    FD_LOG_INFO(( "slot %lu cluster confirmed (Confirmed)", confirmed->slot ));

    /* Notify filter about slot confirmation (Confirmed commitment) */
    fd_geyser_filter_notify_slot_confirmed( ctx->filter, confirmed->slot );
  }
}

void
tower_sham_link_after_frag( fd_geyser_ctx_t * ctx, ulong sig ) {
  (void)ctx;
  (void)sig;
  /* Nothing to do after fragment processing */
}

/* Callback for exec_geyser link messages (real-time transaction data) */
void
exec_sham_link_during_frag( fd_geyser_ctx_t * ctx, ulong sig, ulong ctl, void const * msg, ulong sz ) {
  (void)sig;
  (void)ctl;

  if( sz != sizeof(fd_exec_geyser_msg_t) ) {
    FD_LOG_WARNING(( "unexpected exec_geyser message size: %lu (expected %lu)",
                     sz, sizeof(fd_exec_geyser_msg_t) ));
    return;
  }

  fd_exec_geyser_msg_t const * geyser_msg = (fd_exec_geyser_msg_t const *)msg;

  FD_LOG_DEBUG(( "exec_geyser[%lu]: slot=%lu txn_idx=%lu entry_idx=%lu last_in_entry=%u last_entry=%u",
                 ctx->current_exec_link_idx,
                 geyser_msg->slot,
                 geyser_msg->txn_idx,
                 geyser_msg->entry_idx,
                 geyser_msg->is_last_txn_in_entry,
                 geyser_msg->is_last_entry_in_slot ));

  /* Extract transaction signature for debug logging */
  fd_txn_t const * txn = TXN( &geyser_msg->txn );
  uchar const * sig_bytes = geyser_msg->txn.payload + txn->signature_off;
  char sig_b58[FD_BASE58_ENCODED_64_SZ];
  fd_base58_encode_64( sig_bytes, NULL, sig_b58 );

  FD_LOG_DEBUG(( "[SHM_RECV] txn_received slot=%lu txn_idx=%lu entry_idx=%lu "
                       "is_last_txn_in_entry=%u is_last_entry_in_slot=%u is_success=%d sig=%s",
                       geyser_msg->slot,
                       geyser_msg->txn_idx,
                       geyser_msg->entry_idx,
                       geyser_msg->is_last_txn_in_entry,
                       geyser_msg->is_last_entry_in_slot,
                       geyser_msg->is_success,
                       sig_b58 ));

  /* Process the transaction and notify accounts */
  fd_geyser_filter_notify_txn( ctx->filter, geyser_msg );
}

void
exec_sham_link_after_frag( fd_geyser_ctx_t * ctx, ulong sig ) {
  (void)ctx;
  (void)sig;
  /* Nothing to do after fragment processing */
}
