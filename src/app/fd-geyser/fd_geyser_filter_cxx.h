/* fd_geyser_filter_cxx.h - C++ safe version of fd_geyser_filter.h
 *
 * This header provides the filter API declarations without pulling in
 * the full firedancer header stack.
 *
 * IMPORTANT: This header must be included INSIDE extern "C" { }
 */
#ifndef FD_GEYSER_FILTER_CXX_H
#define FD_GEYSER_FILTER_CXX_H

#include "fd_geyser_cxx_api.h"

/* Forward declarations */
typedef struct fd_geyser_filter fd_geyser_filter_t;
typedef struct GeyserSubscribeReactor GeyserSubscribeReactor_t;
/* fd_spad_t is already defined in fd_spad.h (via fd_util.h chain) */

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

#endif /* FD_GEYSER_FILTER_CXX_H */
