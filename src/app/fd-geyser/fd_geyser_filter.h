#ifndef HEADER_fd_src_app_fd_geyser_filter_h
#define HEADER_fd_src_app_fd_geyser_filter_h

#include "fd_geyser_poller.h"
#include "../../flamenco/types/fd_types.h"
#include "../../discof/exec/fd_exec_geyser.h"

/* Forward declarations */
typedef struct fd_geyser_filter fd_geyser_filter_t;
typedef struct GeyserSubscribeReactor GeyserSubscribeReactor_t;

/* Create a new filter */
fd_geyser_filter_t * fd_geyser_filter_create( fd_spad_t * spad, fd_funk_t * funk );

/* Set the gRPC service implementation */
void fd_geyser_filter_set_service( fd_geyser_filter_t * filter, void * serv );

/* Add a subscription */
void fd_geyser_filter_add_sub( fd_geyser_filter_t * filter, void * request, GeyserSubscribeReactor_t * reactor );

/* Remove a subscription */
void fd_geyser_filter_un_sub( fd_geyser_filter_t * filter, GeyserSubscribeReactor_t * reactor );

/* Notify filter about a real-time transaction (from exec_geyser) */
void fd_geyser_filter_notify_txn( fd_geyser_filter_t * filter, fd_exec_geyser_msg_t const * msg );

/* Notify filter about slot completion (Processed commitment) - called from replay_out */
void fd_geyser_filter_notify_slot_completed( fd_geyser_filter_t * filter, fd_replay_slot_completed_t * msg );

/* Notify filter about slot confirmed (Confirmed commitment) - called from tower_out */
void fd_geyser_filter_notify_slot_confirmed( fd_geyser_filter_t * filter, ulong slot );

#endif /* HEADER_fd_src_app_fd_geyser_filter_h */
