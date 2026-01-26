#include "local-pragmas.h"
#include "fd_geyser_service.hxx"

extern "C" {
#include "../../ballet/base58/fd_base58.h"
#include "../../ballet/txn/fd_txn.h"
#include "../../util/log/fd_log.h"
}

#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>

/* Map FD_EXECUTOR_INSTR_ERR_* to Solana InstructionError variant index.
   Solana InstructionError enum ordering (from sdk/program/src/instruction.rs):
   0: GenericError, 1: InvalidArgument, 2: InvalidInstructionData, 3: InvalidAccountData,
   4: AccountDataTooSmall, 5: InsufficientFunds, 6: IncorrectProgramId,
   7: MissingRequiredSignature, 8: AccountAlreadyInitialized, 9: UninitializedAccount,
   10: UnbalancedInstruction, 11: ModifiedProgramId, 12: ExternalAccountLamportSpend,
   13: ExternalAccountDataModified, 14: ReadonlyLamportChange, 15: ReadonlyDataModified,
   16: DuplicateAccountIndex (deprecated), 17: ExecutableModified, 18: RentEpochModified,
   19: NotEnoughAccountKeys, 20: AccountDataSizeChanged, 21: AccountNotExecutable,
   22: AccountBorrowFailed, 23: AccountBorrowOutstanding, 24: DuplicateAccountOutOfSync,
   25: Custom(u32), 26: InvalidError, 27: ExecutableDataModified,
   28: ExecutableLamportChange, 29: ExecutableAccountNotRentExempt, 30: UnsupportedProgramId,
   31: CallDepth, 32: MissingAccount, 33: ReentrancyNotAllowed, 34: MaxSeedLengthExceeded,
   35: InvalidSeeds, 36: InvalidRealloc, 37: ComputationalBudgetExceeded,
   38: PrivilegeEscalation, 39: ProgramEnvironmentSetupFailure, 40: ProgramFailedToComplete,
   41: ProgramFailedToCompile, 42: Immutable, 43: IncorrectAuthority, 44: BorshIoError,
   45: AccountNotRentExempt, 46: InvalidAccountOwner, 47: ArithmeticOverflow,
   48: UnsupportedSysvar, 49: IllegalOwner, 50: MaxAccountsDataAllocsExceeded,
   51: MaxAccountsExceeded, 52: MaxInstructionTraceLengthExceeded,
   53: BuiltinProgramsMustConsumeComputeUnits */
static uint32_t
map_instr_error( int err ) {
  switch( err ) {
    case   0: return 40; /* SUCCESS -> shouldn't happen, use ProgramFailedToComplete */
    case  -1: return  0; /* GENERIC_ERR -> GenericError (or Custom if custom_err != 0) */
    case  -2: return  1; /* INVALID_ARG -> InvalidArgument */
    case  -3: return  2; /* INVALID_INSTR_DATA -> InvalidInstructionData */
    case  -4: return  3; /* INVALID_ACC_DATA -> InvalidAccountData */
    case  -5: return  4; /* ACC_DATA_TOO_SMALL -> AccountDataTooSmall */
    case  -6: return  5; /* INSUFFICIENT_FUNDS -> InsufficientFunds */
    case  -7: return  6; /* INCORRECT_PROGRAM_ID -> IncorrectProgramId */
    case  -8: return  7; /* MISSING_REQUIRED_SIGNATURE -> MissingRequiredSignature */
    case  -9: return  8; /* ACC_ALREADY_INITIALIZED -> AccountAlreadyInitialized */
    case -10: return  9; /* UNINITIALIZED_ACCOUNT -> UninitializedAccount */
    case -11: return 10; /* UNBALANCED_INSTR -> UnbalancedInstruction */
    case -12: return 11; /* MODIFIED_PROGRAM_ID -> ModifiedProgramId */
    case -13: return 12; /* EXTERNAL_ACCOUNT_LAMPORT_SPEND -> ExternalAccountLamportSpend */
    case -14: return 13; /* EXTERNAL_DATA_MODIFIED -> ExternalAccountDataModified */
    case -15: return 14; /* READONLY_LAMPORT_CHANGE -> ReadonlyLamportChange */
    case -16: return 15; /* READONLY_DATA_MODIFIED -> ReadonlyDataModified */
    case -17: return 16; /* DUPLICATE_ACCOUNT_IDX -> DuplicateAccountIndex */
    case -18: return 17; /* EXECUTABLE_MODIFIED -> ExecutableModified */
    case -19: return 18; /* RENT_EPOCH_MODIFIED -> RentEpochModified */
    case -20: return 19; /* NOT_ENOUGH_ACC_KEYS -> NotEnoughAccountKeys */
    case -21: return 20; /* ACC_DATA_SIZE_CHANGED -> AccountDataSizeChanged */
    case -22: return 21; /* ACC_NOT_EXECUTABLE -> AccountNotExecutable */
    case -23: return 22; /* ACC_BORROW_FAILED -> AccountBorrowFailed */
    case -24: return 23; /* ACC_BORROW_OUTSTANDING -> AccountBorrowOutstanding */
    case -25: return 24; /* DUPLICATE_ACCOUNT_OUT_OF_SYNC -> DuplicateAccountOutOfSync */
    case -26: return 25; /* CUSTOM_ERR -> Custom (special handling needed) */
    case -27: return 26; /* INVALID_ERR -> InvalidError */
    case -28: return 27; /* EXECUTABLE_DATA_MODIFIED -> ExecutableDataModified */
    case -29: return 28; /* EXECUTABLE_LAMPORT_CHANGE -> ExecutableLamportChange */
    case -30: return 29; /* EXECUTABLE_ACCOUNT_NOT_RENT_EXEMPT -> ExecutableAccountNotRentExempt */
    case -31: return 30; /* UNSUPPORTED_PROGRAM_ID -> UnsupportedProgramId */
    case -32: return 31; /* CALL_DEPTH -> CallDepth */
    case -33: return 32; /* MISSING_ACC -> MissingAccount */
    case -34: return 33; /* REENTRANCY_NOT_ALLOWED -> ReentrancyNotAllowed */
    case -35: return 34; /* MAX_SEED_LENGTH_EXCEEDED -> MaxSeedLengthExceeded */
    case -36: return 35; /* INVALID_SEEDS -> InvalidSeeds */
    case -37: return 36; /* INVALID_REALLOC -> InvalidRealloc */
    case -38: return 37; /* COMPUTE_BUDGET_EXCEEDED -> ComputationalBudgetExceeded */
    case -39: return 38; /* PRIVILEGE_ESCALATION -> PrivilegeEscalation */
    case -40: return 39; /* PROGRAM_ENVIRONMENT_SETUP_FAILURE -> ProgramEnvironmentSetupFailure */
    case -41: return 40; /* PROGRAM_FAILED_TO_COMPLETE -> ProgramFailedToComplete */
    case -42: return 41; /* PROGRAM_FAILED_TO_COMPILE -> ProgramFailedToCompile */
    case -43: return 42; /* ACC_IMMUTABLE -> Immutable */
    case -44: return 43; /* INCORRECT_AUTHORITY -> IncorrectAuthority */
    case -45: return 44; /* BORSH_IO_ERROR -> BorshIoError */
    case -46: return 45; /* ACC_NOT_RENT_EXEMPT -> AccountNotRentExempt */
    case -47: return 46; /* INVALID_ACC_OWNER -> InvalidAccountOwner */
    case -48: return 47; /* ARITHMETIC_OVERFLOW -> ArithmeticOverflow */
    case -49: return 48; /* UNSUPPORTED_SYSVAR -> UnsupportedSysvar */
    case -50: return 49; /* ILLEGAL_OWNER -> IllegalOwner */
    case -51: return 50; /* MAX_ACCS_DATA_ALLOCS_EXCEEDED -> MaxAccountsDataAllocsExceeded */
    case -52: return 51; /* MAX_ACCS_EXCEEDED -> MaxAccountsExceeded */
    case -53: return 52; /* MAX_INSN_TRACE_LENS_EXCEEDED -> MaxInstructionTraceLengthExceeded */
    case -54: return 53; /* BUILTINS_MUST_CONSUME_CUS -> BuiltinProgramsMustConsumeComputeUnits */
    default:  return 40; /* Unknown -> ProgramFailedToComplete */
  }
}

/* Serialize Firedancer error codes to Solana bincode TransactionError format.
   Solana TransactionError enum (from sdk/src/transaction/error.rs):
   0: AccountInUse, 1: AccountLoadedTwice, 2: AccountNotFound, 3: ProgramAccountNotFound,
   4: InsufficientFundsForFee, 5: InvalidAccountForFee, 6: AlreadyProcessed,
   7: BlockhashNotFound, 8: InstructionError(u8, InstructionError), 9: CallChainTooDeep,
   10: MissingSignatureForFee, 11: InvalidAccountIndex, 12: SignatureFailure,
   13: InvalidProgramForExecution, 14: SanitizeFailure, 15: ClusterMaintenance,
   16: AccountBorrowOutstanding, 17: WouldExceedMaxBlockCostLimit, 18: UnsupportedVersion,
   19: InvalidWritableAccount, 20: WouldExceedMaxAccountCostLimit,
   21: WouldExceedAccountDataBlockLimit, 22: TooManyAccountLocks,
   23: AddressLookupTableNotFound, 24: InvalidAddressLookupTableOwner,
   25: InvalidAddressLookupTableData, 26: InvalidAddressLookupTableIndex,
   27: InvalidRentPayingAccount, 28: WouldExceedMaxVoteCostLimit,
   29: WouldExceedAccountDataTotalLimit, 30: DuplicateInstruction(u8),
   31: InsufficientFundsForRent, 32: MaxLoadedAccountsDataSizeExceeded,
   33: InvalidLoadedAccountsDataSizeLimit, 34: ResanitizationNeeded,
   35: ProgramExecutionTemporarilyRestricted, 36: UnbalancedTransaction,
   37: ProgramCacheHitMaxLimit */
static void
serialize_transaction_error(
    int txn_err,
    int instr_err,
    int instr_err_idx,
    uint custom_err,
    std::vector<uchar>& out ) {
  out.clear();

  if( txn_err == 0 ) return;  /* No error */

  /* Map FD_RUNTIME_TXN_ERR_* to Solana TransactionError variant index */
  uint32_t variant_idx;
  switch( txn_err ) {
    case  -1: variant_idx =  0; break; /* ACCOUNT_IN_USE */
    case  -2: variant_idx =  1; break; /* ACCOUNT_LOADED_TWICE */
    case  -3: variant_idx =  2; break; /* ACCOUNT_NOT_FOUND */
    case  -4: variant_idx =  3; break; /* PROGRAM_ACCOUNT_NOT_FOUND */
    case  -5: variant_idx =  4; break; /* INSUFFICIENT_FUNDS_FOR_FEE */
    case  -6: variant_idx =  5; break; /* INVALID_ACCOUNT_FOR_FEE */
    case  -7: variant_idx =  6; break; /* ALREADY_PROCESSED */
    case  -8: variant_idx =  7; break; /* BLOCKHASH_NOT_FOUND */
    case  -9: variant_idx =  8; break; /* INSTRUCTION_ERROR */
    case -10: variant_idx =  9; break; /* CALL_CHAIN_TOO_DEEP */
    case -11: variant_idx = 10; break; /* MISSING_SIGNATURE_FOR_FEE */
    case -12: variant_idx = 11; break; /* INVALID_ACCOUNT_INDEX */
    case -13: variant_idx = 12; break; /* SIGNATURE_FAILURE */
    case -14: variant_idx = 13; break; /* INVALID_PROGRAM_FOR_EXECUTION */
    case -15: variant_idx = 14; break; /* SANITIZE_FAILURE */
    case -16: variant_idx = 15; break; /* CLUSTER_MAINTENANCE */
    case -17: variant_idx = 16; break; /* ACCOUNT_BORROW_OUTSTANDING */
    case -18: variant_idx = 17; break; /* WOULD_EXCEED_MAX_BLOCK_COST_LIMIT */
    case -19: variant_idx = 18; break; /* UNSUPPORTED_VERSION */
    case -20: variant_idx = 19; break; /* INVALID_WRITABLE_ACCOUNT */
    case -21: variant_idx = 20; break; /* WOULD_EXCEED_MAX_ACCOUNT_COST_LIMIT */
    case -22: variant_idx = 21; break; /* WOULD_EXCEED_ACCOUNT_DATA_BLOCK_LIMIT */
    case -23: variant_idx = 22; break; /* TOO_MANY_ACCOUNT_LOCKS */
    case -24: variant_idx = 23; break; /* ADDRESS_LOOKUP_TABLE_NOT_FOUND */
    case -25: variant_idx = 24; break; /* INVALID_ADDRESS_LOOKUP_TABLE_OWNER */
    case -26: variant_idx = 25; break; /* INVALID_ADDRESS_LOOKUP_TABLE_DATA */
    case -27: variant_idx = 26; break; /* INVALID_ADDRESS_LOOKUP_TABLE_INDEX */
    case -28: variant_idx = 27; break; /* INVALID_RENT_PAYING_ACCOUNT */
    case -29: variant_idx = 28; break; /* WOULD_EXCEED_MAX_VOTE_COST_LIMIT */
    case -30: variant_idx = 29; break; /* WOULD_EXCEED_ACCOUNT_DATA_TOTAL_LIMIT */
    case -31: variant_idx = 30; break; /* DUPLICATE_INSTRUCTION */
    case -32: variant_idx = 31; break; /* INSUFFICIENT_FUNDS_FOR_RENT */
    case -33: variant_idx = 32; break; /* MAX_LOADED_ACCOUNTS_DATA_SIZE_EXCEEDED */
    case -34: variant_idx = 33; break; /* INVALID_LOADED_ACCOUNTS_DATA_SIZE_LIMIT */
    case -35: variant_idx = 34; break; /* RESANITIZATION_NEEDED */
    case -36: variant_idx = 35; break; /* PROGRAM_EXECUTION_TEMPORARILY_RESTRICTED */
    case -37: variant_idx = 36; break; /* UNBALANCED_TRANSACTION */
    case -38: variant_idx = 37; break; /* PROGRAM_CACHE_HIT_MAX_LIMIT */
    /* Nonce-related errors map to BlockhashNotFound */
    case -50: variant_idx =  7; break; /* BLOCKHASH_NONCE_ALREADY_ADVANCED */
    case -51: variant_idx =  7; break; /* BLOCKHASH_FAIL_ADVANCE_NONCE_INSTR */
    case -52: variant_idx =  7; break; /* BLOCKHASH_FAIL_WRONG_NONCE */
    default:  variant_idx =  8; break; /* Unknown -> InstructionError */
  }

  /* Write variant index (4 bytes, little endian) */
  out.push_back( (variant_idx >>  0) & 0xFF );
  out.push_back( (variant_idx >>  8) & 0xFF );
  out.push_back( (variant_idx >> 16) & 0xFF );
  out.push_back( (variant_idx >> 24) & 0xFF );

  /* For InstructionError (variant 8), write instruction index and error type */
  if( variant_idx == 8 ) {
    /* Instruction index (1 byte) */
    out.push_back( (uchar)( instr_err_idx < 0 ? 0 : instr_err_idx ) );

    /* Map FD_EXECUTOR_INSTR_ERR_* to Solana InstructionError variant */
    uint32_t instr_variant = map_instr_error( instr_err );

    /* Check if this is a Custom error (-26 or -1 with custom_err != 0) */
    bool is_custom = ( instr_err == -26 ) || ( instr_err == -1 && custom_err != 0 );
    if( is_custom ) {
      instr_variant = 25; /* Custom variant */
    }

    /* Write InstructionError variant index (4 bytes, little endian) */
    out.push_back( (instr_variant >>  0) & 0xFF );
    out.push_back( (instr_variant >>  8) & 0xFF );
    out.push_back( (instr_variant >> 16) & 0xFF );
    out.push_back( (instr_variant >> 24) & 0xFF );

    /* For Custom error (variant 25), write the custom error code */
    if( instr_variant == 25 ) {
      out.push_back( (custom_err >>  0) & 0xFF );
      out.push_back( (custom_err >>  8) & 0xFF );
      out.push_back( (custom_err >> 16) & 0xFF );
      out.push_back( (custom_err >> 24) & 0xFF );
    }
  }
  /* For DuplicateInstruction (variant 30), write instruction index */
  else if( variant_idx == 30 ) {
    out.push_back( (uchar)( instr_err_idx < 0 ? 0 : instr_err_idx ) );
  }
  /* For InsufficientFundsForRent (variant 31), write account index */
  else if( variant_idx == 31 ) {
    out.push_back( (uchar)( instr_err_idx < 0 ? 0 : instr_err_idx ) );
  }
}

/* Bidirectional streaming reactor for Subscribe */
class GeyserSubscribeReactor : public grpc::ServerBidiReactor<geyser::SubscribeRequest, geyser::SubscribeUpdate> {
  GeyserServiceImpl * service_;
  fd_geyser_filter_t * filter_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<geyser::SubscribeUpdate> pending_;
  bool writing_ = false;
  bool finished_ = false;
  bool subscribed_ = false;
  bool started_read_ = false;

public:
  GeyserSubscribeReactor( GeyserServiceImpl * service, fd_geyser_filter_t * filter )
    : service_(service), filter_(filter) {
    FD_LOG_NOTICE(( "[GRPC] New client connected, sending initial ping..." ));

    /* Send an initial ping to trigger tonic client's internal machinery.
       This is a workaround for tonic clients that use subscribe() + send()
       pattern instead of subscribe_with_request(). */
    geyser::SubscribeUpdate ping_update;
    ping_update.mutable_ping();
    writing_ = true;
    StartWrite( &ping_update );

    /* Note: StartRead will be called after ping is sent (in OnWriteDone) */
  }

  void OnWriteDone( bool ok ) override {
    (void)ok;
    std::lock_guard<std::mutex> lock( mutex_ );
    writing_ = false;

    /* If this is the first write (initial ping), start reading */
    if( !started_read_ ) {
      started_read_ = true;
      FD_LOG_NOTICE(( "[GRPC] Initial ping sent, now waiting for subscription request..." ));
      StartRead( &request_ );
    } else if( !pending_.empty() && !finished_ ) {
      auto update = std::move( pending_.front() );
      pending_.pop();
      writing_ = true;
      StartWrite( &update );
    }
  }

  void OnReadDone( bool ok ) override {
    FD_LOG_NOTICE(( "[GRPC] OnReadDone called, ok=%d subscribed=%d", ok, subscribed_ ));

    if( !ok ) {
      if( subscribed_ ) {
        /* Client closed its sending side after subscribing - this is normal.
           The connection stays open for receiving updates. */
        FD_LOG_NOTICE(( "[GRPC] Client closed sending side (subscribed, keeping connection open)" ));
        return;
      } else {
        /* Client closed before subscribing - end the connection */
        FD_LOG_WARNING(( "[GRPC] Client closed before subscribing, finishing connection" ));
        Finish( grpc::Status::OK );
        return;
      }
    }

    /* Process subscription request */
    if( !subscribed_ ) {
      FD_LOG_NOTICE(( "[GRPC] Processing subscription request..." ));
      fd_geyser_filter_add_sub( filter_, &request_, this );
      subscribed_ = true;
      FD_LOG_NOTICE(( "[GRPC] Subscription registered successfully" ));
    }

    /* Continue reading for more subscription updates (like ping requests) */
    StartRead( &request_ );
  }

  void OnDone() override {
    if( subscribed_ ) {
      fd_geyser_filter_un_sub( filter_, this );
    }
    delete this;
  }

  void OnCancel() override {
    std::lock_guard<std::mutex> lock( mutex_ );
    finished_ = true;
  }

  /* Queue an update for sending */
  void Send( const geyser::SubscribeUpdate& update ) {
    std::lock_guard<std::mutex> lock( mutex_ );
    if( finished_ ) return;

    if( writing_ ) {
      pending_.push( update );
    } else {
      writing_ = true;
      StartWrite( &update );
    }
  }

private:
  geyser::SubscribeRequest request_;
};

/* GeyserServiceImpl implementation */

GeyserServiceImpl::GeyserServiceImpl( fd_geyser_ctx_t * ctx ) {
  filt_ = fd_geyser_get_filter( ctx );
  funk_ = fd_geyser_get_funk( ctx );
  fd_geyser_filter_set_service( filt_, this );
  memset( &lastinfo_, 0, sizeof(lastinfo_) );
}

GeyserServiceImpl::~GeyserServiceImpl() {}

grpc::ServerBidiReactor<geyser::SubscribeRequest, geyser::SubscribeUpdate>*
GeyserServiceImpl::Subscribe( grpc::CallbackServerContext* context ) {
  (void)context;
  return new GeyserSubscribeReactor( this, filt_ );
}

grpc::ServerUnaryReactor*
GeyserServiceImpl::SubscribeReplayInfo(
    grpc::CallbackServerContext* context,
    const geyser::SubscribeReplayInfoRequest* request,
    geyser::SubscribeReplayInfoResponse* response ) {
  (void)request;
  auto* reactor = context->DefaultReactor();
  /* TODO: implement first_available slot tracking */
  reactor->Finish( grpc::Status::OK );
  return reactor;
}

grpc::ServerUnaryReactor*
GeyserServiceImpl::Ping(
    grpc::CallbackServerContext* context,
    const geyser::PingRequest* request,
    geyser::PongResponse* response ) {
  auto* reactor = context->DefaultReactor();
  response->set_count( request->count() );
  reactor->Finish( grpc::Status::OK );
  return reactor;
}

grpc::ServerUnaryReactor*
GeyserServiceImpl::GetLatestBlockhash(
    grpc::CallbackServerContext* context,
    const geyser::GetLatestBlockhashRequest* request,
    geyser::GetLatestBlockhashResponse* response ) {
  (void)request;
  auto* reactor = context->DefaultReactor();
  response->set_slot( lastinfo_.slot );

  char blockhash_str[FD_BASE58_ENCODED_32_SZ];
  fd_base58_encode_32( lastinfo_.block_hash.uc, NULL, blockhash_str );
  response->set_blockhash( blockhash_str );
  response->set_last_valid_block_height( lastinfo_.block_height + 150 );

  reactor->Finish( grpc::Status::OK );
  return reactor;
}

grpc::ServerUnaryReactor*
GeyserServiceImpl::GetBlockHeight(
    grpc::CallbackServerContext* context,
    const geyser::GetBlockHeightRequest* request,
    geyser::GetBlockHeightResponse* response ) {
  (void)request;
  auto* reactor = context->DefaultReactor();
  response->set_block_height( lastinfo_.block_height );
  reactor->Finish( grpc::Status::OK );
  return reactor;
}

grpc::ServerUnaryReactor*
GeyserServiceImpl::GetSlot(
    grpc::CallbackServerContext* context,
    const geyser::GetSlotRequest* request,
    geyser::GetSlotResponse* response ) {
  (void)request;
  auto* reactor = context->DefaultReactor();
  response->set_slot( lastinfo_.slot );
  reactor->Finish( grpc::Status::OK );
  return reactor;
}

grpc::ServerUnaryReactor*
GeyserServiceImpl::IsBlockhashValid(
    grpc::CallbackServerContext* context,
    const geyser::IsBlockhashValidRequest* request,
    geyser::IsBlockhashValidResponse* response ) {
  auto* reactor = context->DefaultReactor();

  fd_hash_t hash;
  if( !fd_base58_decode_32( request->blockhash().c_str(), hash.uc ) ) {
    reactor->Finish( grpc::Status( grpc::INVALID_ARGUMENT, "Invalid blockhash" ) );
    return reactor;
  }

  auto it = validhashes_.find( hash );
  if( it != validhashes_.end() ) {
    response->set_slot( it->second );
    response->set_valid( true );
  } else {
    response->set_slot( 0 );
    response->set_valid( false );
  }

  reactor->Finish( grpc::Status::OK );
  return reactor;
}

grpc::ServerUnaryReactor*
GeyserServiceImpl::GetVersion(
    grpc::CallbackServerContext* context,
    const geyser::GetVersionRequest* request,
    geyser::GetVersionResponse* response ) {
  (void)request;
  auto* reactor = context->DefaultReactor();
  response->set_version( "fd-geyser 1.0.0" );
  reactor->Finish( grpc::Status::OK );
  return reactor;
}

void
GeyserServiceImpl::notify( fd_replay_slot_completed_t * msg ) {
  lastinfo_ = *msg;

  /* Track valid blockhashes (last 150 slots) */
  validhashes_[msg->block_hash] = msg->slot;

  /* Clean old hashes */
  for( auto it = validhashes_.begin(); it != validhashes_.end(); ) {
    if( it->second + 150 < msg->slot ) {
      it = validhashes_.erase( it );
    } else {
      ++it;
    }
  }
}

/* Static helper to send account update */
void
GeyserServiceImpl::updateAcct(
    GeyserSubscribeReactor * reactor,
    ulong slot,
    fd_pubkey_t * key,
    fd_ed25519_sig_t const * sig,
    fd_account_meta_t * meta,
    const uchar * data,
    ulong data_sz ) {
  updateAcctWithMarkers( reactor, slot, key, sig, meta, data, data_sz, nullptr, nullptr );
}

/* Extended version with entry boundary markers */
void
GeyserServiceImpl::updateAcctWithMarkers(
    GeyserSubscribeReactor * reactor,
    ulong slot,
    fd_pubkey_t * key,
    fd_ed25519_sig_t const * sig,
    fd_account_meta_t * meta,
    const uchar * data,
    ulong data_sz,
    fd_ed25519_sig_t const * end_txn_sig,
    fd_exec_geyser_msg_t const * geyser_msg ) {
  char key_b58[FD_BASE58_ENCODED_32_SZ];
  fd_base58_encode_32( key->uc, NULL, key_b58 );
  char sig_b58[FD_BASE58_ENCODED_64_SZ] = "(none)";
  if( sig ) fd_base58_encode_64( (uchar const *)sig, NULL, sig_b58 );
  FD_LOG_DEBUG(( "[GRPC_SEND] account slot=%lu pubkey=%s lamports=%lu data_len=%lu sig=%s",
                       slot, key_b58, meta->lamports, data_sz, sig_b58 ));
  geyser::SubscribeUpdate update;
  auto* acct_update = update.mutable_account();
  auto* acct_info = acct_update->mutable_account();

  acct_update->set_slot( slot );
  acct_update->set_is_startup( false );

  acct_info->set_pubkey( key->uc, 32 );
  acct_info->set_lamports( meta->lamports );
  acct_info->set_owner( meta->owner, 32 );
  acct_info->set_executable( meta->executable );
  acct_info->set_rent_epoch( 0 );  /* rent_epoch no longer stored in fd_account_meta */
  acct_info->set_data( data, data_sz );
  acct_info->set_write_version( 0 ); /* TODO: track write version */

  /* Set transaction signature for all accounts in the transaction.
   * Additionally, set end_of_txn_signature only on the last account. */
  if( sig ) {
    acct_info->set_txn_signature( sig, 64 );
  }
  if( end_txn_sig ) {
    acct_info->set_end_of_txn_signature( end_txn_sig, 64 );
  }

  /* NOTE: Entry boundary markers (end_of_entry, end_of_slot) require
   * regenerating the protobuf with the extended geyser.proto.
   * These fields are defined in geyser.proto but need protoc to be run.
   * For now, the boundary information is tracked in geyser_msg but not
   * sent over gRPC until protobuf is regenerated.
   *
   * When protobuf is regenerated with the extended fields:
   * if( geyser_msg ) {
   *   if( geyser_msg->is_last_txn_in_entry ) {
   *     auto* entry_marker = acct_info->mutable_end_of_entry();
   *     entry_marker->set_slot( geyser_msg->slot );
   *     entry_marker->set_entry_idx( geyser_msg->entry_idx );
   *   }
   *   if( geyser_msg->is_last_entry_in_slot ) {
   *     acct_info->set_end_of_slot( true );
   *   }
   * }
   */
  (void)geyser_msg;

  reactor->Send( update );
}

void
GeyserServiceImpl::updateSlot( GeyserSubscribeReactor * reactor, fd_replay_slot_completed_t * msg, geyser::SlotStatus status ) {
  const char * status_str = (status == geyser::SLOT_PROCESSED) ? "PROCESSED" :
                            (status == geyser::SLOT_CONFIRMED) ? "CONFIRMED" : "OTHER";
  FD_LOG_DEBUG(( "[GRPC_SEND] slot slot=%lu parent=%lu status=%s",
                       msg->slot, msg->parent_slot, status_str ));

  geyser::SubscribeUpdate update;
  auto* slot_update = update.mutable_slot();

  slot_update->set_slot( msg->slot );
  slot_update->set_parent( msg->parent_slot );
  slot_update->set_status( status );

  reactor->Send( update );
}

void
GeyserServiceImpl::updateEntry( GeyserSubscribeReactor * reactor, ulong slot, ulong entry_idx ) {
  FD_LOG_DEBUG(( "[GRPC_SEND] entry slot=%lu entry_idx=%lu", slot, entry_idx ));

  geyser::SubscribeUpdate update;
  auto* entry_update = update.mutable_entry();

  entry_update->set_slot( slot );
  entry_update->set_index( entry_idx );
  /* Note: num_hashes, hash, executed_transaction_count are not available
     in the real-time stream - they would require parsing the entry header */
  entry_update->set_num_hashes( 0 );
  entry_update->set_executed_transaction_count( 0 );

  reactor->Send( update );
}

void
GeyserServiceImpl::updateTxn(
    GeyserSubscribeReactor * reactor,
    fd_replay_slot_completed_t * msg,
    fd_txn_t * txn,
    fd_pubkey_t * accts,
    fd_ed25519_sig_t const * sigs,
    uchar const * payload,
    bool is_success,
    ulong fee,
    int txn_err,
    int instr_err,
    int instr_err_idx,
    uint custom_err ) {
  /* Detect if any instruction invokes the vote program */
  bool is_vote = false;
  for( ushort i = 0; i < txn->instr_cnt; i++ ) {
    uchar prog_id_idx = txn->instr[i].program_id;
    if( !memcmp( accts[prog_id_idx].uc, fd_geyser_vote_program_id, 32 ) ) {
      is_vote = true;
      break;
    }
  }

  char sig_b58[FD_BASE58_ENCODED_64_SZ];
  fd_base58_encode_64( (uchar const *)sigs, NULL, sig_b58 );

  /* Debug log for failed transactions */
  if( !is_success ) {
    FD_LOG_DEBUG(( "[GRPC_SEND] txn_failed slot=%lu sig=%s txn_err=%d instr_err=%d instr_idx=%d custom_err=%u",
                   msg->slot, sig_b58, txn_err, instr_err, instr_err_idx, custom_err ));
  }
  FD_LOG_DEBUG(( "[GRPC_SEND] txn slot=%lu sig=%s is_vote=%d is_success=%d fee=%lu alt_cnt=%u",
                 msg->slot, sig_b58, is_vote, is_success, fee, txn->addr_table_lookup_cnt ));

  geyser::SubscribeUpdate update;
  auto* txn_update = update.mutable_transaction();
  auto* txn_info = txn_update->mutable_transaction();

  txn_update->set_slot( msg->slot );
  txn_info->set_signature( sigs, 64 );
  txn_info->set_is_vote( is_vote );

  /* Create Transaction.Message structure with versioned flag and ALT data */
  auto* transaction = txn_info->mutable_transaction();
  auto* message = transaction->mutable_message();

  /* Check if this is a v0 versioned transaction */
  if( txn->transaction_version == FD_TXN_V0 ) {
    message->set_versioned( true );

    /* Fill address_table_lookups for v0 transactions */
    if( payload != NULL && txn->addr_table_lookup_cnt > 0 ) {
      fd_txn_acct_addr_lut_t const * tables = fd_txn_get_address_tables_const( txn );
      for( uchar i = 0; i < txn->addr_table_lookup_cnt; i++ ) {
        auto* alt = message->add_address_table_lookups();

        /* account_key: 32 bytes ALT address from payload */
        alt->set_account_key( payload + tables[i].addr_off, 32 );

        /* writable_indexes: array of writable account indices */
        alt->set_writable_indexes(
          payload + tables[i].writable_off,
          tables[i].writable_cnt
        );

        /* readonly_indexes: array of readonly account indices */
        alt->set_readonly_indexes(
          payload + tables[i].readonly_off,
          tables[i].readonly_cnt
        );
      }
    }
  } else {
    message->set_versioned( false );
  }

  /* Fill meta field with fee and error status */
  auto* meta = txn_info->mutable_meta();

  /* Set fee */
  meta->set_fee( fee );

  /* Set err if transaction failed */
  if( !is_success ) {
    auto* err = meta->mutable_err();
    std::vector<uchar> err_data;
    serialize_transaction_error( txn_err, instr_err, instr_err_idx, custom_err, err_data );

    /* Debug log serialized error bytes */
    FD_LOG_DEBUG(( "[GRPC_SEND] txn_err_serialized sig=%s bytes_len=%lu first_4_bytes=[%02x %02x %02x %02x]",
                   sig_b58, (ulong)err_data.size(),
                   err_data.size() > 0 ? err_data[0] : 0,
                   err_data.size() > 1 ? err_data[1] : 0,
                   err_data.size() > 2 ? err_data[2] : 0,
                   err_data.size() > 3 ? err_data[3] : 0 ));

    err->set_err( err_data.data(), err_data.size() );
  }

  reactor->Send( update );
}

void
GeyserServiceImpl::updateBlockMeta(
    GeyserSubscribeReactor * reactor,
    fd_replay_slot_completed_t * msg ) {
  /* Encode blockhash as base58 (needed for both debug logging and gRPC) */
  char blockhash_str[FD_BASE58_ENCODED_32_SZ];
  fd_base58_encode_32( msg->block_hash.uc, NULL, blockhash_str );

  FD_LOG_DEBUG(( "[GRPC_SEND] block_meta slot=%lu block_height=%lu blockhash=%s txn_cnt=%lu",
                       msg->slot, msg->block_height, blockhash_str, msg->transaction_count ));

  geyser::SubscribeUpdate update;
  auto* block_meta = update.mutable_block_meta();

  block_meta->set_slot( msg->slot );
  block_meta->set_parent_slot( msg->parent_slot );
  block_meta->set_blockhash( blockhash_str );

  /* Note: parent_blockhash would need to be cached from previous slot.
     For now, we set it to default hash (all zeros in base58).
     This field is not used by dexclient, just needs a valid hash format to avoid parse error. */
  block_meta->set_parent_blockhash( "11111111111111111111111111111111" );

  /* Set block height */
  auto* block_height = block_meta->mutable_block_height();
  block_height->set_block_height( msg->block_height );

  /* Set block time - using current time as approximation.
     Note: Real block time comes from PoH clock which we don't have here. */
  auto* block_time = block_meta->mutable_block_time();
  block_time->set_timestamp( (long)fd_log_wallclock() / 1000000000L );

  /* Set transaction count */
  block_meta->set_executed_transaction_count( msg->transaction_count );

  reactor->Send( update );
}
