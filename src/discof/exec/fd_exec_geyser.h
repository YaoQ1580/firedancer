#ifndef HEADER_fd_src_discof_exec_fd_exec_geyser_h
#define HEADER_fd_src_discof_exec_fd_exec_geyser_h

#include "../../disco/pack/fd_microblock.h" /* for fd_txn_p_t */
#include "../../ballet/txn/fd_txn.h"        /* for fd_acct_addr_t, FD_TXN_ACCT_ADDR_MAX */

/* fd_exec_geyser_msg_t is sent from exec tiles to the geyser service
   when a transaction is executed and committed.  This allows real-time
   streaming of transaction data to gRPC clients. */

struct fd_exec_geyser_msg {
  ulong      slot;                /* Slot number of the transaction. */
  ulong      txn_idx;             /* Transaction index in the scheduler pool. */
  ulong      bank_idx;            /* Bank index for looking up the funk_txn. */
  int        is_success;          /* 1 if transaction succeeded (is_committable), 0 otherwise. */
  ulong      fee;                 /* Total fee (execution_fee + priority_fee) in lamports. */

  /* Error information (only valid when is_success == 0). */
  int        txn_err;             /* FD_RUNTIME_TXN_ERR_* error code. */
  int        instr_err;           /* FD_EXECUTOR_INSTR_ERR_* error code (for InstructionError). */
  int        instr_err_idx;       /* Index of the failed instruction. */
  uint       custom_err;          /* Custom program error code (for Custom error). */

  /* Entry boundary information. */
  ulong      entry_idx;           /* Index of the entry (microblock) within the slot. */
  uint       is_last_txn_in_entry;/* 1 if this is the last transaction in the entry (by parse order). */
  uint       is_last_entry_in_slot;/* 1 if this entry is the last in the slot. */

  fd_txn_p_t txn;                 /* Transaction payload. The fd-geyser process can parse this
                                     to get the account list and query funk for account data. */

  /* ALT resolved accounts: For V0 transactions with Address Lookup Tables,
     the static accounts in txn.payload only contain acct_addr_cnt accounts.
     The additional ALT-resolved accounts are stored here. */
  uchar          alt_acct_cnt;                        /* Number of ALT-resolved accounts (0 for legacy txns) */
  fd_acct_addr_t alt_accts[FD_TXN_ACCT_ADDR_MAX];    /* Resolved ALT account addresses */
};
typedef struct fd_exec_geyser_msg fd_exec_geyser_msg_t;

#endif /* HEADER_fd_src_discof_exec_fd_exec_geyser_h */
