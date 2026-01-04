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
    fd_ed25519_sig_t const * sigs ) {
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
  FD_LOG_DEBUG(( "[GRPC_SEND] txn slot=%lu sig=%s is_vote=%d", msg->slot, sig_b58, is_vote ));

  geyser::SubscribeUpdate update;
  auto* txn_update = update.mutable_transaction();
  auto* txn_info = txn_update->mutable_transaction();

  txn_update->set_slot( msg->slot );
  txn_info->set_signature( sigs, 64 );
  txn_info->set_is_vote( is_vote );

  /* Note: full transaction and meta would require more context */

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
     For now, we leave it empty or use parent_block_id if available. */
  /* block_meta->set_parent_blockhash( "" ); */

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
