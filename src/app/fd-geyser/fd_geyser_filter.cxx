#include "local-pragmas.h"

#include <map>
#include <mutex>
#include "unordered_dense.h"  /* Fast cache-friendly hash map/set */
#include <vector>
#include <memory>

#include "geyser.grpc.pb.h"
#include "fd_geyser_service.hxx"
#include "fd_compact_encoder.h"

extern "C" {
/* Use C++ safe headers - avoids fd_topo.h which uses 'new' keyword */
#include "fd_geyser_filter_cxx.h"
#include "../../util/log/fd_log.h"

/* Additional C headers needed for implementation */
#include "../../ballet/base58/fd_base58.h"
/* Note: fd_txn.h, fd_txn_p.h, funk headers are included via fd_geyser_cxx_api.h */
}

/* Hash and equality for fd_hash_t to use with unordered_dense::set */
struct FdHashHasher {
  using is_avalanching = void;  /* Indicate good hash distribution */
  auto operator()( fd_hash_t const & h ) const noexcept -> uint64_t {
    /* Use first 8 bytes as hash - pubkeys have good entropy */
    return h.ul[0];
  }
};

struct FdHashEqual {
  auto operator()( fd_hash_t const & a, fd_hash_t const & b ) const noexcept -> bool {
    return !memcmp( a.uc, b.uc, 32 );
  }
};

/* Type alias for fd_hash_t set with O(1) lookup */
using FdHashSet = ankerl::unordered_dense::set<fd_hash_t, FdHashHasher, FdHashEqual>;

/* Compiled Account Filter */
struct CompiledFilterAccount {
  std::string name_;
  FdHashSet keys_;    /* O(1) lookup for account keys */
  FdHashSet owners_;  /* O(1) lookup for owner keys */
  std::optional<uint64_t> datasize_;
  bool nonempty_txn_signature_ = false;

  /* Pre-computed compact encoding type (0 = no encoding, >0 = FdCompactMsgType) */
  unsigned char compact_type_ = FD_COMPACT_MSG_RAW;

  /* Memcmp filters */
  struct MemcmpFilter {
    uint64_t offset;
    std::vector<uint8_t> data;
  };
  std::vector<MemcmpFilter> memcmp_;

  /* Lamports filter */
  struct LamportsFilter {
    enum { EQ, NE, LT, GT } op;
    uint64_t value;
  };
  std::optional<LamportsFilter> lamports_;
};

struct CompiledFilterSlot {
  std::string name_;
  bool filter_by_commitment_ = false;
};

struct CompiledFilterTxn {
  std::string name_;
  FdHashSet acct_include_;   /* O(1) lookup for include filter */
  FdHashSet acct_exclude_;   /* O(1) lookup for exclude filter */
  FdHashSet acct_required_;  /* O(1) lookup for required filter */
  std::optional<bool> vote_;
  std::optional<bool> failed_;
};

struct CompiledFilterEntry {
  std::string name_;
};

struct CompiledFilterBlockMeta {
  std::string name_;
  /* BlockMeta filters usually have no additional conditions */
};

/* Pending update structures for Confirmed commitment buffering */
struct PendingAccountUpdate {
  fd_pubkey_t key;
  fd_account_meta_t meta;
  std::vector<uchar> data;
  fd_ed25519_sig_t sig;
  fd_ed25519_sig_t end_txn_sig;
  fd_exec_geyser_msg_t geyser_msg;
  bool has_sig;
  bool has_end_txn_sig;
  bool has_geyser_msg;
  std::vector<GeyserSubscribeReactor*> target_reactors;  /* Pre-filtered targets */
};

struct PendingTxnUpdate {
  ulong slot;
  uchar txn_parsed[FD_TXN_MAX_SZ];  /* Pre-parsed fd_txn_t structure */
  ulong txn_sz;
  std::vector<fd_pubkey_t> accts;
  fd_ed25519_sig_t sig;
  std::vector<uchar> payload;                             /* Raw transaction payload for ALT data */
  bool is_success;
  ulong fee;                                              /* Total fee in lamports */
  int txn_err;                                            /* FD_RUNTIME_TXN_ERR_* */
  int instr_err;                                          /* FD_EXECUTOR_INSTR_ERR_* */
  int instr_err_idx;                                      /* Failed instruction index */
  uint custom_err;                                        /* Custom program error code */
  std::vector<GeyserSubscribeReactor*> target_reactors;  /* Pre-filtered targets */
};

/* Slot state for commitment level tracking */
struct SlotState {
  bool confirmed = false;
  bool has_completed_msg = false;
  fd_replay_slot_completed_t completed_msg;  /* Cached slot completion info */
  std::vector<PendingAccountUpdate> pending_accounts;
  std::vector<PendingTxnUpdate> pending_txns;
};

class CompiledFilter {
public:
  static CompiledFilter * compile( ::geyser::SubscribeRequest * request ) {
    CompiledFilter * filt = new CompiledFilter();
    if( filt->compile_internal( request ) )
      return filt;
    delete filt;
    return NULL;
  }

  unsigned char filterAccount( fd_pubkey_t * key, fd_account_meta_t * meta, const uchar * data, ulong data_sz );
  bool filterSlot( fd_replay_slot_completed_t * msg );
  bool filterTxn( fd_exec_geyser_msg_t const * msg, fd_txn_t * txn, fd_pubkey_t * accts, uint total_acct_cnt );
  bool hasEntryFilter() const { return !entries_.empty(); }
  bool hasBlockMetaFilter() const { return !blocks_meta_.empty(); }
  bool hasTxnFilter() const { return !txns_.empty(); }

  bool compile_internal( ::geyser::SubscribeRequest * request );

  std::vector<std::unique_ptr<CompiledFilterAccount>> accts_;
  std::vector<std::unique_ptr<CompiledFilterSlot>> slots_;
  std::vector<std::unique_ptr<CompiledFilterTxn>> txns_;
  std::vector<std::unique_ptr<CompiledFilterEntry>> entries_;
  std::vector<std::unique_ptr<CompiledFilterBlockMeta>> blocks_meta_;
  ::geyser::CommitmentLevel commitment_ = ::geyser::PROCESSED;
};

bool
CompiledFilter::compile_internal( ::geyser::SubscribeRequest * request ) {
  bool hasfilter = false;

  /* Compile account filters */
  for( auto& i : request->accounts() ) {
    auto* a = new CompiledFilterAccount();
    a->name_ = i.first;
    auto& f = i.second;

    /* Parse account keys */
    for( int j = 0; j < f.account_size(); ++j ) {
      auto& s = f.account(j);
      fd_hash_t hash;
      if( !fd_base58_decode_32( s.c_str(), hash.uc ) ) return false;
      a->keys_.insert(hash);
    }

    /* Parse owner keys */
    for( int j = 0; j < f.owner_size(); ++j ) {
      auto& s = f.owner(j);
      fd_hash_t hash;
      if( !fd_base58_decode_32( s.c_str(), hash.uc ) ) return false;
      a->owners_.insert(hash);
    }

    /* Parse advanced filters */
    for( int j = 0; j < f.filters_size(); ++j ) {
      auto& flt = f.filters(j);
      if( flt.has_memcmp() ) {
        CompiledFilterAccount::MemcmpFilter m;
        m.offset = flt.memcmp().offset();
        if( flt.memcmp().has_bytes() ) {
          auto& b = flt.memcmp().bytes();
          m.data.assign( b.begin(), b.end() );
        } else if( flt.memcmp().has_base58() ) {
          uchar decoded[64];
          if( !fd_base58_decode_32( flt.memcmp().base58().c_str(), decoded ) ) return false;
          m.data.assign( decoded, decoded + 32 );
        }
        a->memcmp_.push_back( std::move(m) );
      } else if( flt.has_datasize() ) {
        a->datasize_ = flt.datasize();
      } else if( flt.has_lamports() ) {
        CompiledFilterAccount::LamportsFilter lf;
        if( flt.lamports().has_eq() ) {
          lf.op = CompiledFilterAccount::LamportsFilter::EQ;
          lf.value = flt.lamports().eq();
        } else if( flt.lamports().has_ne() ) {
          lf.op = CompiledFilterAccount::LamportsFilter::NE;
          lf.value = flt.lamports().ne();
        } else if( flt.lamports().has_lt() ) {
          lf.op = CompiledFilterAccount::LamportsFilter::LT;
          lf.value = flt.lamports().lt();
        } else if( flt.lamports().has_gt() ) {
          lf.op = CompiledFilterAccount::LamportsFilter::GT;
          lf.value = flt.lamports().gt();
        }
        a->lamports_ = lf;
      }
    }

    if( f.has_nonempty_txn_signature() ) {
      a->nonempty_txn_signature_ = f.nonempty_txn_signature();
    }

    /* Pre-compute compact encoding type based on filter name (called once at compile time) */
    a->compact_type_ = fd_compact_get_type_for_filter( a->name_.c_str() );
    if( a->compact_type_ != FD_COMPACT_MSG_RAW ) {
      FD_LOG_NOTICE(( "Filter '%s' will use compact encoding type %d",
                      a->name_.c_str(), (int)a->compact_type_ ));
    }

    accts_.emplace_back(a);
    hasfilter = true;
  }

  /* Compile slot filters */
  for( auto& i : request->slots() ) {
    auto* a = new CompiledFilterSlot();
    a->name_ = i.first;
    if( i.second.has_filter_by_commitment() ) {
      a->filter_by_commitment_ = i.second.filter_by_commitment();
    }
    slots_.emplace_back(a);
    hasfilter = true;
  }

  /* Compile transaction filters */
  for( auto& i : request->transactions() ) {
    auto* a = new CompiledFilterTxn();
    a->name_ = i.first;
    auto& f = i.second;

    for( int j = 0; j < f.account_include_size(); ++j ) {
      fd_hash_t hash;
      if( !fd_base58_decode_32( f.account_include(j).c_str(), hash.uc ) ) return false;
      a->acct_include_.insert(hash);
    }
    for( int j = 0; j < f.account_exclude_size(); ++j ) {
      fd_hash_t hash;
      if( !fd_base58_decode_32( f.account_exclude(j).c_str(), hash.uc ) ) return false;
      a->acct_exclude_.insert(hash);
    }
    for( int j = 0; j < f.account_required_size(); ++j ) {
      fd_hash_t hash;
      if( !fd_base58_decode_32( f.account_required(j).c_str(), hash.uc ) ) return false;
      a->acct_required_.insert(hash);
    }
    if( f.has_vote() ) a->vote_ = f.vote();
    if( f.has_failed() ) a->failed_ = f.failed();

    txns_.emplace_back(a);
    hasfilter = true;
  }

  /* Compile entry filters */
  for( auto& i : request->entry() ) {
    auto* a = new CompiledFilterEntry();
    a->name_ = i.first;
    entries_.emplace_back(a);
    hasfilter = true;
  }

  /* Compile blocks_meta filters */
  for( auto& i : request->blocks_meta() ) {
    auto* a = new CompiledFilterBlockMeta();
    a->name_ = i.first;
    blocks_meta_.emplace_back(a);
    hasfilter = true;
  }

  /* Store commitment level */
  if( request->has_commitment() ) {
    commitment_ = request->commitment();
  }

  return hasfilter;
}

unsigned char
CompiledFilter::filterAccount( fd_pubkey_t * key, fd_account_meta_t * meta,
                               const uchar * data, ulong data_sz ) {
  for( auto& f : accts_ ) {
    /* Check keys filter - O(1) hash lookup */
    if( !f->keys_.empty() ) {
      if( f->keys_.find( *key ) == f->keys_.end() ) continue;  /* Try next filter */
    }

    /* Check owners filter - O(1) hash lookup */
    if( !f->owners_.empty() ) {
      fd_hash_t owner_key;
      memcpy( owner_key.uc, meta->owner, 32 );
      if( f->owners_.find( owner_key ) == f->owners_.end() ) continue;
    }

    /* Check datasize filter */
    if( f->datasize_.has_value() && data_sz != f->datasize_.value() ) {
      continue;
    }

    /* Check memcmp filters (all must match) */
    bool memcmp_ok = true;
    for( auto& m : f->memcmp_ ) {
      if( m.offset + m.data.size() > data_sz ) {
        memcmp_ok = false;
        break;
      }
      if( memcmp( data + m.offset, m.data.data(), m.data.size() ) != 0 ) {
        memcmp_ok = false;
        break;
      }
    }
    if( !memcmp_ok ) continue;

    /* Check lamports filter */
    if( f->lamports_.has_value() ) {
      uint64_t bal = meta->lamports;
      auto& lf = f->lamports_.value();
      bool ok = false;
      switch( lf.op ) {
        case CompiledFilterAccount::LamportsFilter::EQ: ok = (bal == lf.value); break;
        case CompiledFilterAccount::LamportsFilter::NE: ok = (bal != lf.value); break;
        case CompiledFilterAccount::LamportsFilter::LT: ok = (bal < lf.value); break;
        case CompiledFilterAccount::LamportsFilter::GT: ok = (bal > lf.value); break;
      }
      if( !ok ) continue;
    }

    /* Matched: return pre-computed compact type (0 = raw, >0 = compact encoding) */
    return f->compact_type_;
  }
  return FD_COMPACT_MSG_NO_MATCH;  /* 0xFF = no match */
}

bool
CompiledFilter::filterSlot( fd_replay_slot_completed_t * msg ) {
  (void)msg;
  return !slots_.empty();
}

bool
CompiledFilter::filterTxn( fd_exec_geyser_msg_t const * msg, fd_txn_t * txn, fd_pubkey_t * accts, uint total_acct_cnt ) {
  (void)msg;
  for( auto& f : txns_ ) {
    /* Check account_include - O(n) where n = total_acct_cnt (static + ALT) */
    if( !f->acct_include_.empty() ) {
      bool found = false;
      for( uint j = 0; j < total_acct_cnt; ++j ) {
        if( f->acct_include_.find( accts[j] ) != f->acct_include_.end() ) {
          found = true;
          break;
        }
      }
      if( !found ) continue;
    }

    /* Check account_exclude - O(n) where n = total_acct_cnt (static + ALT) */
    if( !f->acct_exclude_.empty() ) {
      bool excluded = false;
      for( uint j = 0; j < total_acct_cnt; ++j ) {
        if( f->acct_exclude_.find( accts[j] ) != f->acct_exclude_.end() ) {
          excluded = true;
          break;
        }
      }
      if( excluded ) continue;
    }

    /* Check account_required - must check each required is in txn (static + ALT) */
    bool all_required = true;
    for( auto& h : f->acct_required_ ) {
      bool found = false;
      for( uint j = 0; j < total_acct_cnt; ++j ) {
        if( !memcmp( h.uc, accts[j].uc, 32 ) ) {
          found = true;
          break;
        }
      }
      if( !found ) {
        all_required = false;
        break;
      }
    }
    if( !all_required ) continue;

    /* Check vote filter (program_id is always in static accounts) */
    if( f->vote_.has_value() ) {
      /* Detect if any instruction invokes the vote program */
      bool is_vote = false;
      for( ushort i = 0; i < txn->instr_cnt; i++ ) {
        uchar prog_id_idx = txn->instr[i].program_id;
        if( !memcmp( accts[prog_id_idx].uc, fd_geyser_vote_program_id, 32 ) ) {
          is_vote = true;
          break;
        }
      }
      if( is_vote != f->vote_.value() ) continue;
    }

    /* Check failed filter */
    if( f->failed_.has_value() ) {
      bool is_failed = !msg->is_success;
      if( is_failed != f->failed_.value() ) continue;
    }

    return true;  /* Matched */
  }
  return false;
}

/* Filter state structure */
struct fd_geyser_filter {
  struct Subscription {
    CompiledFilter * filter_;
    GeyserSubscribeReactor * reactor_;
  };
  std::vector<Subscription> subs_;
  ankerl::unordered_dense::set<GeyserSubscribeReactor*> active_reactors_;  /* O(1) reactor validity check */
  std::mutex mutex_;

  fd_spad_t * spad_;
  fd_funk_t * funk_;
  GeyserServiceImpl * serv_;
  bool need_acct_lookup_ = false;

  /* Slot state tracking for commitment level handling */
  ankerl::unordered_dense::map<ulong, SlotState> slot_states_;

  /* O(1) lookup: reactor -> filter (for data slice support) */
  ankerl::unordered_dense::map<GeyserSubscribeReactor*, CompiledFilter*> reactor_to_filter_;

  fd_geyser_filter( fd_spad_t * spad, fd_funk_t * funk ) : spad_(spad), funk_(funk), serv_(nullptr) {}

  /* Safety check: warn if slot_states_ grows too large (indicates missed confirmed signals) */
  void check_slot_states_size() {
    const ulong MAX_EXPECTED_SLOTS = 5000;
    if( slot_states_.size() > MAX_EXPECTED_SLOTS ) {
      FD_LOG_WARNING(( "[GEYSER] slot_states_ size=%lu exceeds threshold, possible missed confirmed signals",
                       (ulong)slot_states_.size() ));
    }
  }

  void notify_account( ulong slot, fd_pubkey_t * key, fd_ed25519_sig_t const * sig,
                       fd_account_meta_t * meta, const uchar * data, ulong data_sz,
                       fd_ed25519_sig_t const * end_txn_sig,
                       fd_exec_geyser_msg_t const * geyser_msg );
  void notify_slot( fd_replay_slot_completed_t * msg, ::geyser::CommitmentLevel event_level );
  void notify_entry( ulong slot, ulong entry_idx );
  void notify_txn( ulong slot, fd_exec_geyser_msg_t const * msg,
                   fd_txn_t * txn, fd_pubkey_t * accts, uint total_acct_cnt,
                   fd_ed25519_sig_t const * sigs, uchar const * payload, ulong payload_sz );
  void notify_block_meta( fd_replay_slot_completed_t * msg, ::geyser::CommitmentLevel event_level );
  void flush_confirmed_updates( SlotState& state, fd_replay_slot_completed_t * msg );
};

fd_geyser_filter_t *
fd_geyser_filter_create( fd_spad_t * spad, fd_funk_t * funk ) {
  return new fd_geyser_filter( spad, funk );
}

void
fd_geyser_filter_set_service( fd_geyser_filter_t * filter, void * serv ) {
  filter->serv_ = (GeyserServiceImpl *)serv;
}

void
fd_geyser_filter_add_sub( fd_geyser_filter_t * filter, void * request_void, GeyserSubscribeReactor_t * reactor ) {
  auto* request = (::geyser::SubscribeRequest *)request_void;

  /* Log subscription details */
  const char * commitment_str = "UNKNOWN";
  if( request->has_commitment() ) {
    switch( request->commitment() ) {
      case ::geyser::PROCESSED: commitment_str = "PROCESSED"; break;
      case ::geyser::CONFIRMED: commitment_str = "CONFIRMED"; break;
      case ::geyser::FINALIZED: commitment_str = "FINALIZED"; break;
      default: commitment_str = "UNKNOWN"; break;
    }
  } else {
    commitment_str = "PROCESSED(default)";
  }

  FD_LOG_NOTICE(( "[SUBSCRIBE] New subscription: accounts=%lu slots=%lu txns=%lu entries=%lu blocks_meta=%lu commitment=%s",
                  (ulong)request->accounts().size(),
                  (ulong)request->slots().size(),
                  (ulong)request->transactions().size(),
                  (ulong)request->entry().size(),
                  (ulong)request->blocks_meta().size(),
                  commitment_str ));

  /* Log individual filter names */
  for( auto& i : request->accounts() ) {
    FD_LOG_NOTICE(( "[SUBSCRIBE]   account filter: name='%s' keys=%d owners=%d",
                    i.first.c_str(), i.second.account_size(), i.second.owner_size() ));
  }
  for( auto& i : request->slots() ) {
    FD_LOG_NOTICE(( "[SUBSCRIBE]   slot filter: name='%s'", i.first.c_str() ));
  }
  for( auto& i : request->blocks_meta() ) {
    FD_LOG_NOTICE(( "[SUBSCRIBE]   blocks_meta filter: name='%s'", i.first.c_str() ));
  }

  auto* f = CompiledFilter::compile( request );
  if( !f ) {
    FD_LOG_WARNING(( "[SUBSCRIBE] Failed to compile filter!" ));
    return;
  }

  FD_LOG_NOTICE(( "[SUBSCRIBE] Filter compiled: hasBlockMetaFilter=%d commitment=%d",
                  f->hasBlockMetaFilter(), (int)f->commitment_ ));

  std::lock_guard<std::mutex> lock( filter->mutex_ );
  filter->subs_.push_back( { f, reactor } );
  filter->active_reactors_.insert( reactor );  /* Track for O(1) validity check */
  filter->reactor_to_filter_[reactor] = f;     /* Track for O(1) filter lookup */
  FD_LOG_NOTICE(( "[SUBSCRIBE] Subscription added, total subs=%lu", (ulong)filter->subs_.size() ));

  /* Enable account lookups if needed */
  if( !request->accounts().empty() ) {
    filter->need_acct_lookup_ = true;
  }
}

void
fd_geyser_filter_un_sub( fd_geyser_filter_t * filter, GeyserSubscribeReactor_t * reactor ) {
  std::lock_guard<std::mutex> lock( filter->mutex_ );
  filter->active_reactors_.erase( reactor );    /* Remove from validity set */
  filter->reactor_to_filter_.erase( reactor );  /* Remove from filter lookup */
  for( auto i = filter->subs_.begin(); i != filter->subs_.end(); ) {
    if( i->reactor_ == reactor ) {
      delete i->filter_;
      i = filter->subs_.erase( i );
    } else {
      ++i;
    }
  }
}

void
fd_geyser_filter::notify_account( ulong slot, fd_pubkey_t * key, fd_ed25519_sig_t const * sig,
                                   fd_account_meta_t * meta, const uchar * data, ulong data_sz,
                                   fd_ed25519_sig_t const * end_txn_sig,
                                   fd_exec_geyser_msg_t const * geyser_msg ) {
  char key_b58[FD_BASE58_ENCODED_32_SZ];
  fd_base58_encode_32( key->uc, NULL, key_b58 );
  char owner_b58[FD_BASE58_ENCODED_32_SZ];
  fd_base58_encode_32( meta->owner, NULL, owner_b58 );
  FD_LOG_DEBUG(( "[NOTIFY] account_update slot=%lu pubkey=%s lamports=%lu owner=%s data_len=%lu",
                       slot, key_b58, meta->lamports, owner_b58, data_sz ));

  std::lock_guard<std::mutex> lock( mutex_ );

  /* Thread-local buffer for compact encoding (avoid allocation per call) */
  thread_local std::vector<unsigned char> compact_buf(32768);  /* 32KB, sufficient for max compact output */

  /* Collect Confirmed targets that need buffering (slot not yet confirmed) */
  std::vector<GeyserSubscribeReactor*> confirmed_targets;
  auto& state = slot_states_[slot];
  bool slot_confirmed = state.confirmed;

  for( auto& sub : subs_ ) {
    unsigned char compact_type = sub.filter_->filterAccount( key, meta, data, data_sz );
    if( compact_type == FD_COMPACT_MSG_NO_MATCH ) continue;  /* No filter matched */

    /* Prepare data to send: apply compact encoding if configured */
    const uchar * final_data = data;
    ulong final_sz = data_sz;

    if( compact_type != FD_COMPACT_MSG_RAW ) {
      /* Try compact encoding (using pre-computed type, no strcmp at runtime) */
      ulong encoded_sz = fd_compact_encode(
        compact_type, data, data_sz, compact_buf.data(), compact_buf.size() );
      if( encoded_sz > 0 ) {
        final_data = compact_buf.data();
        final_sz = encoded_sz;
        FD_LOG_DEBUG(( "[GRPC_SEND] compact encoded: type=%d original=%lu encoded=%lu",
                       (int)compact_type, data_sz, encoded_sz ));
      }
    }

    /* Check subscriber's commitment level */
    if( sub.filter_->commitment_ == ::geyser::PROCESSED ) {
      /* Processed: send immediately */
      GeyserServiceImpl::updateAcctWithMarkers(
        sub.reactor_, slot, key, sig, meta, final_data, final_sz,
        end_txn_sig, geyser_msg );
    } else if( sub.filter_->commitment_ == ::geyser::CONFIRMED ) {
      if( slot_confirmed ) {
        /* Slot already confirmed, send immediately */
        GeyserServiceImpl::updateAcctWithMarkers(
          sub.reactor_, slot, key, sig, meta, final_data, final_sz,
          end_txn_sig, geyser_msg );
      } else {
        /* Collect target for buffering - store encoded data */
        confirmed_targets.push_back( sub.reactor_ );
      }
    }
  }

  /* Create single pending entry with all targets (no duplicate account storage)
     Note: For confirmed buffering, we store the original data since different
     subscriptions might have different compact_type settings */
  if( !confirmed_targets.empty() ) {
    PendingAccountUpdate pending;
    pending.key = *key;
    pending.meta = *meta;
    pending.data.assign( data, data + data_sz );
    pending.has_sig = (sig != nullptr);
    if( sig ) memcpy( &pending.sig, sig, sizeof(fd_ed25519_sig_t) );
    pending.has_end_txn_sig = (end_txn_sig != nullptr);
    if( end_txn_sig ) memcpy( &pending.end_txn_sig, end_txn_sig, sizeof(fd_ed25519_sig_t) );
    pending.has_geyser_msg = (geyser_msg != nullptr);
    if( geyser_msg ) pending.geyser_msg = *geyser_msg;
    pending.target_reactors = std::move( confirmed_targets );
    state.pending_accounts.push_back( std::move(pending) );
  }
}

void
fd_geyser_filter::notify_slot( fd_replay_slot_completed_t * msg, ::geyser::CommitmentLevel event_level ) {
  const char * level_str = (event_level == ::geyser::PROCESSED) ? "PROCESSED" : "CONFIRMED";
  FD_LOG_DEBUG(( "[NOTIFY] slot_update slot=%lu block_height=%lu txn_cnt=%lu event_level=%s",
                       msg->slot, msg->block_height, msg->transaction_count, level_str ));

  std::lock_guard<std::mutex> lock( mutex_ );

  for( auto& sub : subs_ ) {
    if( !sub.filter_->filterSlot( msg ) ) continue;

    /* Check if any slot filter has filter_by_commitment enabled */
    bool filter_by_commitment = false;
    for( auto& sf : sub.filter_->slots_ ) {
      if( sf->filter_by_commitment_ ) {
        filter_by_commitment = true;
        break;
      }
    }

    /* Determine whether to send based on commitment level matching */
    bool should_send = false;
    if( filter_by_commitment ) {
      /* Only send if event level matches subscriber's commitment level */
      should_send = (sub.filter_->commitment_ == event_level);
    } else {
      /* Without filter_by_commitment, send all events */
      should_send = true;
    }

    if( should_send ) {
      geyser::SlotStatus status = (event_level == ::geyser::PROCESSED) ?
                                   geyser::SLOT_PROCESSED : geyser::SLOT_CONFIRMED;
      GeyserServiceImpl::updateSlot( sub.reactor_, msg, status );
    }
  }
}

void
fd_geyser_filter::notify_entry( ulong slot, ulong entry_idx ) {
  std::lock_guard<std::mutex> lock( mutex_ );

  for( auto& sub : subs_ ) {
    if( sub.filter_->hasEntryFilter() ) {
      GeyserServiceImpl::updateEntry( sub.reactor_, slot, entry_idx );
    }
  }
}

void
fd_geyser_filter::notify_txn( ulong slot, fd_exec_geyser_msg_t const * msg,
                               fd_txn_t * txn, fd_pubkey_t * accts, uint total_acct_cnt,
                               fd_ed25519_sig_t const * sigs, uchar const * payload, ulong payload_sz ) {
  std::lock_guard<std::mutex> lock( mutex_ );

  /* Collect Confirmed targets that need buffering (slot not yet confirmed) */
  std::vector<GeyserSubscribeReactor*> confirmed_targets;
  auto& state = slot_states_[slot];
  bool slot_confirmed = state.confirmed;

  for( auto& sub : subs_ ) {
    if( !sub.filter_->hasTxnFilter() ) continue;
    if( !sub.filter_->filterTxn( msg, txn, accts, total_acct_cnt ) ) continue;

    /* Check subscriber's commitment level */
    if( sub.filter_->commitment_ == ::geyser::PROCESSED ) {
      /* Processed: send immediately */
      fd_replay_slot_completed_t slot_msg;
      memset( &slot_msg, 0, sizeof(slot_msg) );
      slot_msg.slot = slot;
      GeyserServiceImpl::updateTxn( sub.reactor_, &slot_msg, txn, accts, sigs, payload,
                                    msg->is_success, msg->fee,
                                    msg->txn_err, msg->instr_err,
                                    msg->instr_err_idx, msg->custom_err );
    } else if( sub.filter_->commitment_ == ::geyser::CONFIRMED ) {
      if( slot_confirmed ) {
        /* Slot already confirmed, send immediately */
        fd_replay_slot_completed_t slot_msg;
        memset( &slot_msg, 0, sizeof(slot_msg) );
        slot_msg.slot = slot;
        GeyserServiceImpl::updateTxn( sub.reactor_, &slot_msg, txn, accts, sigs, payload,
                                      msg->is_success, msg->fee,
                                      msg->txn_err, msg->instr_err,
                                      msg->instr_err_idx, msg->custom_err );
      } else {
        /* Collect target for buffering */
        confirmed_targets.push_back( sub.reactor_ );
      }
    }
  }

  /* Create single pending entry with all targets (no duplicate txn storage) */
  if( !confirmed_targets.empty() ) {
    PendingTxnUpdate pending;
    pending.slot = slot;
    ulong txn_sz = fd_txn_footprint( txn->instr_cnt, txn->addr_table_lookup_cnt );
    memcpy( pending.txn_parsed, txn, txn_sz );
    pending.txn_sz = txn_sz;
    /* Store all accounts including ALT-resolved ones */
    pending.accts.assign( accts, accts + total_acct_cnt );
    memcpy( &pending.sig, sigs, sizeof(fd_ed25519_sig_t) );
    /* Store raw payload for ALT data extraction */
    pending.payload.assign( payload, payload + payload_sz );
    pending.is_success = msg->is_success;
    pending.fee = msg->fee;
    pending.txn_err = msg->txn_err;
    pending.instr_err = msg->instr_err;
    pending.instr_err_idx = msg->instr_err_idx;
    pending.custom_err = msg->custom_err;
    pending.target_reactors = std::move( confirmed_targets );
    state.pending_txns.push_back( std::move(pending) );
  }
}

void
fd_geyser_filter::notify_block_meta( fd_replay_slot_completed_t * msg, ::geyser::CommitmentLevel event_level ) {
  const char * level_str = (event_level == ::geyser::PROCESSED) ? "PROCESSED" :
                           (event_level == ::geyser::CONFIRMED) ? "CONFIRMED" : "OTHER";
  FD_LOG_DEBUG(( "[BLOCK_META] notify_block_meta: slot=%lu event_level=%s total_subs=%lu",
                  msg->slot, level_str, (ulong)subs_.size() ));

  std::lock_guard<std::mutex> lock( mutex_ );

  ulong matched = 0;
  ulong has_filter = 0;
  for( auto& sub : subs_ ) {
    if( !sub.filter_->hasBlockMetaFilter() ) continue;
    has_filter++;
    /* Only send to subscribers matching the event's commitment level */
    if( sub.filter_->commitment_ == event_level ) {
      matched++;
      FD_LOG_DEBUG(( "[BLOCK_META] Sending to subscriber: slot=%lu sub_commitment=%d", msg->slot, (int)sub.filter_->commitment_ ));
      GeyserServiceImpl::updateBlockMeta( sub.reactor_, msg );
    }
  }
  FD_LOG_DEBUG(( "[BLOCK_META] Result: has_block_meta_filter=%lu matched=%lu", has_filter, matched ));
}

void
fd_geyser_filter::flush_confirmed_updates( SlotState& state, fd_replay_slot_completed_t * msg ) {
  /* Note: mutex_ should already be held by caller or we need to acquire it */

  /* Mark slot as confirmed */
  state.confirmed = true;

  /* Thread-local buffer for compact encoding */
  thread_local std::vector<unsigned char> compact_buf(32768);  /* 32KB, sufficient for max compact output */

  /* Flush buffered Account updates - send directly to pre-filtered targets (no re-filtering!) */
  for( auto& pending : state.pending_accounts ) {
    for( auto* reactor : pending.target_reactors ) {
      /* O(1) check if reactor is still valid (subscriber may have disconnected) */
      if( active_reactors_.find( reactor ) == active_reactors_.end() ) continue;

      /* O(1) lookup for filter to get compact type */
      CompiledFilter * target_filter = nullptr;
      auto filter_it = reactor_to_filter_.find( reactor );
      if( filter_it != reactor_to_filter_.end() ) {
        target_filter = filter_it->second;
      }

      /* Apply compact encoding if configured */
      const uchar * send_data = pending.data.data();
      ulong send_sz = pending.data.size();

      if( target_filter ) {
        /* Re-filter to get compact_type (stored data is original, need to re-encode) */
        unsigned char compact_type = target_filter->filterAccount(
          (fd_pubkey_t*)&pending.key, (fd_account_meta_t*)&pending.meta,
          pending.data.data(), pending.data.size() );
        if( compact_type != FD_COMPACT_MSG_RAW && compact_type != FD_COMPACT_MSG_NO_MATCH ) {
          ulong encoded_sz = fd_compact_encode(
            compact_type, pending.data.data(), pending.data.size(),
            compact_buf.data(), compact_buf.size() );
          if( encoded_sz > 0 ) {
            send_data = compact_buf.data();
            send_sz = encoded_sz;
          }
        }
      }

      GeyserServiceImpl::updateAcctWithMarkers(
        reactor, msg->slot, &pending.key,
        pending.has_sig ? &pending.sig : nullptr,
        &pending.meta, send_data, send_sz,
        pending.has_end_txn_sig ? &pending.end_txn_sig : nullptr,
        pending.has_geyser_msg ? &pending.geyser_msg : nullptr );
    }
  }
  state.pending_accounts.clear();

  /* Flush buffered Transaction updates - send directly to pre-filtered targets (no re-filtering!) */
  for( auto& pending : state.pending_txns ) {
    fd_txn_t * txn = (fd_txn_t *)pending.txn_parsed;

    for( auto* reactor : pending.target_reactors ) {
      /* O(1) check if reactor is still valid */
      if( active_reactors_.find( reactor ) == active_reactors_.end() ) continue;

      fd_replay_slot_completed_t slot_msg;
      memset( &slot_msg, 0, sizeof(slot_msg) );
      slot_msg.slot = msg->slot;
      GeyserServiceImpl::updateTxn( reactor, &slot_msg, txn,
                                    const_cast<fd_pubkey_t*>(pending.accts.data()),
                                    &pending.sig,
                                    pending.payload.empty() ? nullptr : pending.payload.data(),
                                    pending.is_success,
                                    pending.fee,
                                    pending.txn_err,
                                    pending.instr_err,
                                    pending.instr_err_idx,
                                    pending.custom_err );
    }
  }
  state.pending_txns.clear();
  /* Note: slot_states_ cleanup is now done by caller after flush */
}

/* Process a real-time transaction from exec_geyser */
void
fd_geyser_filter_notify_txn( fd_geyser_filter_t * filter, fd_exec_geyser_msg_t const * msg ) {
  if( !filter->need_acct_lookup_ && filter->subs_.empty() ) return;

  fd_txn_p_t const * txn_p = &msg->txn;

  /* Parse the transaction from the payload.
     Note: TXN() accesses the pre-parsed txn in txn_p->_, but for messages
     coming from exec_geyser, we need to parse fresh from the payload. */
  uchar txn_out[FD_TXN_MAX_SZ];
  ulong pay_sz = 0;
  ulong txn_sz = fd_txn_parse_core( txn_p->payload, txn_p->payload_sz, txn_out, NULL, &pay_sz, FD_TXN_INSTR_MAX );
  if( txn_sz == 0 || txn_sz > FD_TXN_MAX_SZ ) {
    FD_LOG_WARNING(( "failed to parse transaction in geyser notification" ));
    return;
  }
  fd_txn_t * txn = (fd_txn_t *)txn_out;

  /* Static accounts from payload */
  fd_pubkey_t * static_accts = (fd_pubkey_t *)( txn_p->payload + txn->acct_addr_off );
  fd_ed25519_sig_t const * sigs = (fd_ed25519_sig_t const *)( txn_p->payload + txn->signature_off );

  char sig_b58[FD_BASE58_ENCODED_64_SZ];
  fd_base58_encode_64( (uchar const *)sigs, NULL, sig_b58 );
  FD_LOG_DEBUG(( "[NOTIFY] txn_notify slot=%lu txn_idx=%lu sig=%s is_success=%d txn_err=%d alt_acct_cnt=%u",
                       msg->slot, msg->txn_idx, sig_b58, msg->is_success, msg->txn_err, msg->alt_acct_cnt ));

  /* Query funk for the transaction's funk_txn */
  fd_funk_txn_xid_t xid;
  xid.ul[0] = msg->slot;
  xid.ul[1] = msg->bank_idx;

  fd_funk_txn_map_query_t txn_query[1];
  int txn_err = fd_funk_txn_map_query_try( filter->funk_->txn_map, &xid, NULL, txn_query, 0 );
  if( txn_err != FD_MAP_SUCCESS ) {
    FD_LOG_ERR(( "failed to find funk txn for slot %lu", msg->slot ));
    return;
  }
  fd_funk_txn_t * funk_txn = fd_funk_txn_map_query_ele( txn_query );

  /* Total account count = static + ALT */
  int total_acct_cnt = (int)txn->acct_addr_cnt + (int)msg->alt_acct_cnt;

  /* Build combined account array (static accounts + ALT accounts) */
  fd_pubkey_t all_accts[FD_TXN_ACCT_ADDR_MAX];
  memcpy( all_accts, static_accts, txn->acct_addr_cnt * sizeof(fd_pubkey_t) );
  if( msg->alt_acct_cnt > 0 ) {
    memcpy( &all_accts[txn->acct_addr_cnt], msg->alt_accts, msg->alt_acct_cnt * sizeof(fd_pubkey_t) );
  }

  /* Process writable accounts (static + ALT) */
  int writable_cnt = 0;
  int writable_accts[FD_TXN_ACCT_ADDR_MAX];

  /* Static writable accounts */
  for( int i = 0; i < (int)txn->acct_addr_cnt; i++ ) {
    bool writable = (( i < (int)txn->signature_cnt - (int)txn->readonly_signed_cnt ) ||
                     (( i >= (int)txn->signature_cnt ) &&
                      ( i < (int)txn->acct_addr_cnt - (int)txn->readonly_unsigned_cnt )));
    if( writable ) {
      writable_accts[writable_cnt++] = i;
    }
  }

  /* ALT writable accounts: ALT accounts are ordered as writable first, then readonly.
     The number of writable ALT accounts is stored in txn->addr_table_adtl_writable_cnt. */
  for( int i = 0; i < (int)txn->addr_table_adtl_writable_cnt; i++ ) {
    writable_accts[writable_cnt++] = (int)txn->acct_addr_cnt + i;
  }

  /* Notify each writable account update */
  for( int w = 0; w < writable_cnt; w++ ) {
    int i = writable_accts[w];
    bool is_last_acct = (w == writable_cnt - 1);

    fd_funk_rec_key_t recid = fd_funk_acc_key( &all_accts[i] );

    fd_funk_rec_query_t rec_query[1];
    fd_funk_rec_t const * rec = fd_funk_rec_query_try( filter->funk_, fd_funk_txn_xid( funk_txn ), &recid, rec_query );
    if( rec ) {
      const uchar * val = (const uchar *)fd_funk_val_const( rec, filter->funk_->wksp );
      ulong val_sz = fd_funk_val_sz( rec );
      fd_account_meta_t * meta = (fd_account_meta_t *)val;
      const uchar * data = val + sizeof(fd_account_meta_t);
      ulong data_sz = val_sz - sizeof(fd_account_meta_t);

      /* Set end_of_txn signature only on the last account */
      fd_ed25519_sig_t const * end_txn_sig = is_last_acct ? sigs : NULL;

      filter->notify_account( msg->slot, &all_accts[i], sigs, meta, data, data_sz,
                              end_txn_sig, is_last_acct ? msg : NULL );
    }
  }

  /* If this is the last transaction in an entry, notify entry subscribers */
  if( msg->is_last_txn_in_entry ) {
    filter->notify_entry( msg->slot, msg->entry_idx );
  }

  /* Notify transaction subscribers (pass all accounts including ALT for filtering) */
  filter->notify_txn( msg->slot, msg, txn, all_accts, (uint)total_acct_cnt, sigs,
                      txn_p->payload, txn_p->payload_sz );
}

/* Notify slot completion (Processed commitment) - called from replay_out */
void
fd_geyser_filter_notify_slot_completed( fd_geyser_filter_t * filter, fd_replay_slot_completed_t * msg ) {
  FD_LOG_DEBUG(( "[NOTIFY] slot_completed slot=%lu block_height=%lu txn_cnt=%lu parent=%lu bank_hash=%02x%02x...",
                       msg->slot, msg->block_height, msg->transaction_count, msg->parent_slot,
                       msg->bank_hash.uc[0], msg->bank_hash.uc[1] ));

  /* Store the slot info for later use when Confirmed signal arrives */
  {
    std::lock_guard<std::mutex> lock( filter->mutex_ );
    auto& state = filter->slot_states_[msg->slot];
    state.has_completed_msg = true;
    state.completed_msg = *msg;  /* Cache the full message */
    /* Safety check for unexpected growth */
    filter->check_slot_states_size();
  }

  /* Notify Processed-level slot and block_meta subscribers */
  filter->notify_slot( msg, ::geyser::PROCESSED );
  filter->notify_block_meta( msg, ::geyser::PROCESSED );
}

/* Notify slot confirmed (Confirmed commitment) - called from tower_out
   This is the TRUE "Confirmed" signal (2/3+ stake voted) */
void
fd_geyser_filter_notify_slot_confirmed( fd_geyser_filter_t * filter, ulong slot ) {
  FD_LOG_DEBUG(( "[NOTIFY] slot_confirmed slot=%lu (Confirmed commitment)", slot ));

  fd_replay_slot_completed_t msg;
  memset( &msg, 0, sizeof(msg) );
  msg.slot = slot;

  /* Try to get the cached slot_completed message with full details */
  bool has_cached_msg = false;
  {
    std::lock_guard<std::mutex> lock( filter->mutex_ );
    auto it = filter->slot_states_.find( slot );
    if( it != filter->slot_states_.end() && it->second.has_completed_msg ) {
      msg = it->second.completed_msg;
      has_cached_msg = true;

      /* Flush all buffered updates for Confirmed subscribers */
      filter->flush_confirmed_updates( it->second, &msg );
      /* Immediately delete slot state after confirmed - no need to keep it */
      filter->slot_states_.erase( slot );
    }
  }
  if( !has_cached_msg ) {
    FD_LOG_WARNING(( "[NOTIFY] slot_confirmed slot=%lu: no cached slot_completed msg found (fd-geyser started after this slot was processed?)",
                     slot ));
  }
  FD_LOG_DEBUG(( "[NOTIFY] slot_confirmed slot=%lu has_cached_msg=%d block_height=%lu txn_cnt=%lu",
                       slot, has_cached_msg, msg.block_height, msg.transaction_count ));

  /* Notify Confirmed-level slot and block meta subscribers */
  filter->notify_slot( &msg, ::geyser::CONFIRMED );
  filter->notify_block_meta( &msg, ::geyser::CONFIRMED );
}
