#ifndef HEADER_fd_src_app_fd_geyser_service_hxx
#define HEADER_fd_src_app_fd_geyser_service_hxx

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include "geyser.grpc.pb.h"

#include <map>

extern "C" {
/* Use minimal C++ safe headers instead of full firedancer headers */
#include "fd_geyser_cxx_api.h"
#include "fd_geyser_filter_cxx.h"
}

struct fd_hash_cmp {
  bool operator() ( const fd_hash_t& a, const fd_hash_t& b ) const {
    for( uint i = 0; i < 4; ++i )
      if( a.ul[i] != b.ul[i] )
        return ( a.ul[i] < b.ul[i] );
    return false;
  }
};

/* Forward declarations */
class GeyserSubscribeReactor;

class GeyserServiceImpl final : public geyser::Geyser::CallbackService {
  fd_geyser_filter_t * filt_;
  fd_funk_t * funk_;
  fd_replay_slot_completed_t lastinfo_;
  std::map<fd_hash_t,ulong,fd_hash_cmp> validhashes_;

public:
  GeyserServiceImpl( fd_geyser_ctx_t * ctx );
  virtual ~GeyserServiceImpl() override;

  virtual ::grpc::ServerBidiReactor< ::geyser::SubscribeRequest, ::geyser::SubscribeUpdate>* Subscribe(
    ::grpc::CallbackServerContext* context ) override;

  virtual ::grpc::ServerUnaryReactor* SubscribeReplayInfo(
    ::grpc::CallbackServerContext* context,
    const ::geyser::SubscribeReplayInfoRequest* request,
    ::geyser::SubscribeReplayInfoResponse* response ) override;

  virtual ::grpc::ServerUnaryReactor* Ping(
    ::grpc::CallbackServerContext* context,
    const ::geyser::PingRequest* request,
    ::geyser::PongResponse* response ) override;

  virtual ::grpc::ServerUnaryReactor* GetLatestBlockhash(
    ::grpc::CallbackServerContext* context,
    const ::geyser::GetLatestBlockhashRequest* request,
    ::geyser::GetLatestBlockhashResponse* response ) override;

  virtual ::grpc::ServerUnaryReactor* GetBlockHeight(
    ::grpc::CallbackServerContext* context,
    const ::geyser::GetBlockHeightRequest* request,
    ::geyser::GetBlockHeightResponse* response ) override;

  virtual ::grpc::ServerUnaryReactor* GetSlot(
    ::grpc::CallbackServerContext* context,
    const ::geyser::GetSlotRequest* request,
    ::geyser::GetSlotResponse* response ) override;

  virtual ::grpc::ServerUnaryReactor* IsBlockhashValid(
    ::grpc::CallbackServerContext* context,
    const ::geyser::IsBlockhashValidRequest* request,
    ::geyser::IsBlockhashValidResponse* response ) override;

  virtual ::grpc::ServerUnaryReactor* GetVersion(
    ::grpc::CallbackServerContext* context,
    const ::geyser::GetVersionRequest* request,
    ::geyser::GetVersionResponse* response ) override;

  void notify( fd_replay_slot_completed_t * msg );

  /* Static methods for sending updates to reactors */
  static void updateAcct(
    GeyserSubscribeReactor * reactor,
    ulong slot,
    fd_pubkey_t * key,
    fd_ed25519_sig_t const * sig,
    fd_account_meta_t * meta,
    const uchar * data,
    ulong data_sz );

  /* Extended version with entry boundary markers */
  static void updateAcctWithMarkers(
    GeyserSubscribeReactor * reactor,
    ulong slot,
    fd_pubkey_t * key,
    fd_ed25519_sig_t const * sig,
    fd_account_meta_t * meta,
    const uchar * data,
    ulong data_sz,
    fd_ed25519_sig_t const * end_txn_sig,
    fd_exec_geyser_msg_t const * geyser_msg );

  static void updateSlot( GeyserSubscribeReactor * reactor, fd_replay_slot_completed_t * msg, geyser::SlotStatus status );
  static void updateEntry( GeyserSubscribeReactor * reactor, ulong slot, ulong entry_idx );
  static void updateTxn(
    GeyserSubscribeReactor * reactor,
    fd_replay_slot_completed_t * msg,
    fd_txn_t * txn,
    fd_pubkey_t * accts,
    fd_ed25519_sig_t const * sigs,
    uchar const * payload,        /* Raw transaction payload for ALT data */
    bool is_success,
    ulong fee,
    int txn_err,
    int instr_err,
    int instr_err_idx,
    uint custom_err );
  static void updateBlockMeta( GeyserSubscribeReactor * reactor, fd_replay_slot_completed_t * msg );
};

/* Type alias for C code */
typedef GeyserSubscribeReactor GeyserSubscribeReactor_t;

#endif /* HEADER_fd_src_app_fd_geyser_service_hxx */
