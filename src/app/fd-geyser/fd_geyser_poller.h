#ifndef HEADER_fd_src_app_fd_geyser_poller_h
#define HEADER_fd_src_app_fd_geyser_poller_h

#include "../../util/fd_util.h"
#include "../../discof/replay/fd_replay_tile.h"
#include "../../discof/tower/fd_tower_tile.h"
#include "../../discof/exec/fd_exec_geyser.h"
#include "../../funk/fd_funk.h"

#define FD_GEYSER_MAX_EXEC_TILES 64

struct fd_geyser_args {
  char funk_wksp[ 32 ];       /* Funk workspace name */
  char replay_out_wksp[ 32 ]; /* Replay output workspace (slot completion = Processed) */
  char tower_out_wksp[ 32 ];  /* Tower output workspace (slot confirmed = Confirmed) */
  ulong exec_tile_cnt;        /* Number of exec tiles */
  char exec_geyser_wksp[ FD_GEYSER_MAX_EXEC_TILES ][ 32 ]; /* exec_geyser workspace names */
};

typedef struct fd_geyser_args fd_geyser_args_t;
typedef struct fd_geyser_ctx fd_geyser_ctx_t;
typedef struct fd_geyser_filter fd_geyser_filter_t;

/* Initialize the geyser context */
fd_geyser_ctx_t * fd_geyser_init( fd_geyser_args_t * args );

/* Main polling loop */
void fd_geyser_loop( fd_geyser_ctx_t * ctx );

/* Get the filter for subscription management */
fd_geyser_filter_t * fd_geyser_get_filter( fd_geyser_ctx_t * ctx );

/* Get funk handle for account lookups */
fd_funk_t * fd_geyser_get_funk( fd_geyser_ctx_t * ctx );

#endif /* HEADER_fd_src_app_fd_geyser_poller_h */
