#ifndef HEADER_fd_src_discof_exec_fd_exec_geyser_h
#define HEADER_fd_src_discof_exec_fd_exec_geyser_h

#include "../../disco/pack/fd_microblock.h" /* for fd_txn_p_t */

/* fd_exec_geyser_msg_t is sent from exec tiles to the geyser service
   when a transaction is executed and committed.  This allows real-time
   streaming of transaction data to gRPC clients. */

struct fd_exec_geyser_msg {
  ulong      slot;                /* Slot number of the transaction. */
  ulong      txn_idx;             /* Transaction index in the scheduler pool. */
  ulong      bank_idx;            /* Bank index for looking up the funk_txn. */
  int        is_success;          /* 1 if transaction succeeded (is_committable), 0 otherwise. */

  /* Entry boundary information. */
  ulong      entry_idx;           /* Index of the entry (microblock) within the slot. */
  uint       is_last_txn_in_entry;/* 1 if this is the last transaction in the entry (by parse order). */
  uint       is_last_entry_in_slot;/* 1 if this entry is the last in the slot. */

  fd_txn_p_t txn;                 /* Transaction payload. The fd-geyser process can parse this
                                     to get the account list and query funk for account data. */
};
typedef struct fd_exec_geyser_msg fd_exec_geyser_msg_t;

#endif /* HEADER_fd_src_discof_exec_fd_exec_geyser_h */
