#include "fd_compact_encoder.h"
#include <cstring>
#include <atomic>

extern "C" {
#include "../../util/log/fd_log.h"
}

/* 全局开关：控制是否启用 compact 编码 (默认关闭) */
static std::atomic<bool> g_compact_enabled{false};

bool fd_compact_is_enabled( void ) {
    return g_compact_enabled.load( std::memory_order_relaxed );
}

void fd_compact_set_enabled( bool enabled ) {
    g_compact_enabled.store( enabled, std::memory_order_relaxed );
}

void fd_compact_toggle( void ) {
    bool old = g_compact_enabled.load( std::memory_order_relaxed );
    g_compact_enabled.store( !old, std::memory_order_relaxed );
}

/* 写入通用 Header (8 bytes) */
static inline unsigned char *
write_header( unsigned char * ptr, FdCompactMsgType type ) {
    *(uint32_t*)ptr = FD_COMPACT_MAGIC;
    ptr[4] = (unsigned char)type;
    ptr[5] = 0;  /* flags */
    ptr[6] = 0;  /* reserved */
    ptr[7] = 0;
    return ptr + 8;
}

/* OpenOrders: 只提取 native_coin_total 和 native_pc_total */
static unsigned long
encode_openorders( const unsigned char * data,
                   unsigned char * out, unsigned long out_max ) {
    if( out_max < 24 ) {
        FD_LOG_CRIT(( "[COMPACT] OpenOrders encode failed: out_max=%lu < 24", out_max ));
        return 0;
    }
    unsigned char * ptr = write_header( out, FD_COMPACT_MSG_OPENORDERS );
    memcpy( ptr, data + 85, 8 );      /* native_coin_total */
    memcpy( ptr + 8, data + 101, 8 ); /* native_pc_total */
    return 24;
}

/* CLMM TickArray 稀疏编码
   Max output: 8 + 32 + 4 + 2 + 60*(1+64) = 3946 bytes */
static unsigned long
encode_clmm_tick_array( const unsigned char * data,
                        unsigned char * out, unsigned long out_max ) {
    if( out_max < 4096 ) {
        FD_LOG_CRIT(( "[COMPACT] CLMM TickArray encode failed: out_max=%lu < 4096", out_max ));
        return 0;
    }
    unsigned char * ptr = write_header( out, FD_COMPACT_MSG_CLMM_TICK_ARRAY );

    /* pool_id: offset 8, 32 bytes */
    memcpy( ptr, data + 8, 32 ); ptr += 32;
    /* start_tick_index: offset 40, 4 bytes */
    memcpy( ptr, data + 40, 4 ); ptr += 4;
    /* 预留 count 位置 */
    uint16_t * count_ptr = (uint16_t*)ptr; ptr += 2;

    /* 遍历 60 个 tick (每个 168 bytes, 从 offset 44 开始) */
    uint16_t valid_count = 0;
    const unsigned char * ticks = data + 44;

    for( int i = 0; i < 60; i++ ) {
        const unsigned char * tick = ticks + i * 168;
        /* liquidity_gross: offset 20 in tick, 16 bytes (u128) */
        uint64_t lg_low = *(uint64_t*)(tick + 20);
        uint64_t lg_high = *(uint64_t*)(tick + 28);
        if( lg_low == 0 && lg_high == 0 ) continue;

        *ptr++ = (unsigned char)i;  /* local_index */
        memcpy( ptr, tick + 4, 16 ); ptr += 16;   /* liquidity_net */
        memcpy( ptr, tick + 20, 16 ); ptr += 16;  /* liquidity_gross */
        memcpy( ptr, tick + 36, 16 ); ptr += 16;  /* fee_growth_outside_0 */
        memcpy( ptr, tick + 52, 16 ); ptr += 16;  /* fee_growth_outside_1 */
        valid_count++;
    }
    *count_ptr = valid_count;
    return (unsigned long)(ptr - out);
}

/* Whirlpool TickArray 稀疏编码
   Max output: 8 + 4 + 32 + 2 + 88*(1+64) = 5766 bytes */
static unsigned long
encode_whirlpool_tick_array( const unsigned char * data,
                             unsigned char * out, unsigned long out_max ) {
    if( out_max < 6144 ) {
        FD_LOG_CRIT(( "[COMPACT] Whirlpool TickArray encode failed: out_max=%lu < 6144", out_max ));
        return 0;
    }
    unsigned char * ptr = write_header( out, FD_COMPACT_MSG_WHIRLPOOL_TICK_ARRAY );

    /* start_tick_index: offset 8, 4 bytes */
    memcpy( ptr, data + 8, 4 ); ptr += 4;
    /* whirlpool: offset 9956, 32 bytes */
    memcpy( ptr, data + 9956, 32 ); ptr += 32;
    /* 预留 count */
    uint16_t * count_ptr = (uint16_t*)ptr; ptr += 2;

    /* 遍历 88 个 tick (每个 113 bytes, 从 offset 12 开始) */
    uint16_t valid_count = 0;
    const unsigned char * ticks = data + 12;

    for( int i = 0; i < 88; i++ ) {
        const unsigned char * tick = ticks + i * 113;
        /* initialized: offset 0, 1 byte */
        if( tick[0] == 0 ) continue;

        *ptr++ = (unsigned char)i;  /* local_index */
        memcpy( ptr, tick + 1, 16 ); ptr += 16;   /* liquidity_gross */
        memcpy( ptr, tick + 17, 16 ); ptr += 16;  /* liquidity_net */
        memcpy( ptr, tick + 33, 16 ); ptr += 16;  /* fee_growth_outside_a */
        memcpy( ptr, tick + 49, 16 ); ptr += 16;  /* fee_growth_outside_b */
        valid_count++;
    }
    *count_ptr = valid_count;
    return (unsigned long)(ptr - out);
}

/* Meteora BinArray 稀疏编码
   BinArray layout (from IDL):
     discriminator: offset 0, 8 bytes
     index: offset 8, 8 bytes (i64)
     version: offset 16, 1 byte
     padding: offset 17, 7 bytes
     lb_pair: offset 24, 32 bytes
     bins: offset 56, 70 * 144 bytes
   Max output: 8 + 8 + 32 + 2 + 70*(1+48) = 3480 bytes */
static unsigned long
encode_meteora_bin_array( const unsigned char * data,
                          unsigned char * out, unsigned long out_max ) {
    if( out_max < 4096 ) {
        FD_LOG_CRIT(( "[COMPACT] Meteora BinArray encode failed: out_max=%lu < 4096", out_max ));
        return 0;
    }
    unsigned char * ptr = write_header( out, FD_COMPACT_MSG_METEORA_BIN_ARRAY );

    /* index: offset 8, 8 bytes (i64) */
    memcpy( ptr, data + 8, 8 ); ptr += 8;
    /* lb_pair: offset 24, 32 bytes */
    memcpy( ptr, data + 24, 32 ); ptr += 32;
    /* 预留 count */
    uint16_t * count_ptr = (uint16_t*)ptr; ptr += 2;

    /* 遍历 70 个 bin (每个 144 bytes, 从 offset 56 开始) */
    uint16_t valid_count = 0;
    const unsigned char * bins = data + 56;

    for( int i = 0; i < 70; i++ ) {
        const unsigned char * bin = bins + i * 144;
        /* amount_x: offset 0, amount_y: offset 8 */
        uint64_t ax = *(uint64_t*)(bin);
        uint64_t ay = *(uint64_t*)(bin + 8);
        if( ax == 0 && ay == 0 ) continue;

        *ptr++ = (unsigned char)i;  /* local_index */
        memcpy( ptr, bin, 8 ); ptr += 8;       /* amount_x */
        memcpy( ptr, bin + 8, 8 ); ptr += 8;   /* amount_y */
        memcpy( ptr, bin + 16, 16 ); ptr += 16; /* price */
        memcpy( ptr, bin + 128, 16 ); ptr += 16; /* liquidity_supply */
        valid_count++;
    }
    *count_ptr = valid_count;
    return (unsigned long)(ptr - out);
}

/* Whirlpool: 多段截取, 只提取需要的字段 (169 bytes) */
static unsigned long
encode_whirlpool( const unsigned char * data,
                  unsigned char * out, unsigned long out_max ) {
    if( out_max < 177 ) {
        FD_LOG_CRIT(( "[COMPACT] Whirlpool encode failed: out_max=%lu < 177", out_max ));
        return 0;
    }
    unsigned char * ptr = write_header( out, FD_COMPACT_MSG_WHIRLPOOL );
    /* whirlpool_bump: offset 40, 1 byte */
    *ptr++ = data[40];
    /* tick_spacing: offset 41, 2 bytes */
    memcpy( ptr, data + 41, 2 ); ptr += 2;
    /* fee_rate: offset 45, 2 bytes */
    memcpy( ptr, data + 45, 2 ); ptr += 2;
    /* liquidity: offset 49, 16 bytes */
    memcpy( ptr, data + 49, 16 ); ptr += 16;
    /* sqrt_price: offset 65, 16 bytes */
    memcpy( ptr, data + 65, 16 ); ptr += 16;
    /* tick_current_index: offset 81, 4 bytes */
    memcpy( ptr, data + 81, 4 ); ptr += 4;
    /* token_mint_a: offset 101, 32 bytes */
    memcpy( ptr, data + 101, 32 ); ptr += 32;
    /* token_vault_a: offset 133, 32 bytes */
    memcpy( ptr, data + 133, 32 ); ptr += 32;
    /* token_mint_b: offset 181, 32 bytes */
    memcpy( ptr, data + 181, 32 ); ptr += 32;
    /* token_vault_b: offset 213, 32 bytes */
    memcpy( ptr, data + 213, 32 ); ptr += 32;
    return 177;  /* 8 header + 169 fields */
}

/* CLMM PoolState: 多段截取 */
static unsigned long
encode_clmm_pool( const unsigned char * data,
                  unsigned char * out, unsigned long out_max ) {
    if( out_max < 560 ) {
        FD_LOG_CRIT(( "[COMPACT] CLMM Pool encode failed: out_max=%lu < 560", out_max ));
        return 0;
    }
    unsigned char * ptr = write_header( out, FD_COMPACT_MSG_CLMM_POOL );
    /* bump: offset 8, 1 byte */
    *ptr++ = data[8];
    /* amm_config: offset 9, 32 bytes */
    memcpy( ptr, data + 9, 32 ); ptr += 32;
    /* token_mint_0: offset 73, 32 bytes */
    memcpy( ptr, data + 73, 32 ); ptr += 32;
    /* token_mint_1: offset 105, 32 bytes */
    memcpy( ptr, data + 105, 32 ); ptr += 32;
    /* token_vault_0: offset 137, 32 bytes */
    memcpy( ptr, data + 137, 32 ); ptr += 32;
    /* token_vault_1: offset 169, 32 bytes */
    memcpy( ptr, data + 169, 32 ); ptr += 32;
    /* observation_key: offset 201, 32 bytes */
    memcpy( ptr, data + 201, 32 ); ptr += 32;
    /* tick_spacing: offset 235, 2 bytes */
    memcpy( ptr, data + 235, 2 ); ptr += 2;
    /* liquidity: offset 237, 16 bytes */
    memcpy( ptr, data + 237, 16 ); ptr += 16;
    /* sqrt_price_x64: offset 253, 16 bytes */
    memcpy( ptr, data + 253, 16 ); ptr += 16;
    /* tick_current: offset 269, 4 bytes */
    memcpy( ptr, data + 269, 4 ); ptr += 4;
    /* reward_infos: offset 397, 每个 RewardInfo 169 bytes
       只取 token_mint(offset 57) + token_vault(offset 89), 共3个 */
    for( int i = 0; i < 3; i++ ) {
        int base = 397 + i * 169;
        memcpy( ptr, data + base + 57, 32 ); ptr += 32;  /* token_mint */
        memcpy( ptr, data + base + 89, 32 ); ptr += 32;  /* token_vault */
    }
    /* tick_array_bitmap: offset 904 (397 + 3*169 = 904), 128 bytes */
    memcpy( ptr, data + 904, 128 ); ptr += 128;
    return (unsigned long)(ptr - out);
}

/* LbPair: 多段截取 (按需提取)
   已验证的账户布局 (包含8字节discriminator):
   - parameters: offset 8, 32 bytes
   - v_parameters: offset 40, 32 bytes (只取前20字节有效数据)
   - bump_seed: offset 72, 1 byte
   - bin_step: offset 80, 2 bytes
   - active_id: offset 76, 4 bytes
   - token_x_mint: offset 88, 32 bytes
   - token_y_mint: offset 120, 32 bytes
   - reserve_x: offset 152, 32 bytes
   - reserve_y: offset 184, 32 bytes
   - bin_array_bitmap: offset 744, 128 bytes (904-160)
   - oracle: offset 872, 32 bytes (904-32) */
static unsigned long
encode_lbpair( const unsigned char * data,
               unsigned char * out, unsigned long out_max ) {
    if( out_max < 360 ) {
        FD_LOG_CRIT(( "[COMPACT] LbPair encode failed: out_max=%lu < 360", out_max ));
        return 0;
    }
    unsigned char * ptr = write_header( out, FD_COMPACT_MSG_LBPAIR );
    /* parameters: offset 8, 32 bytes */
    memcpy( ptr, data + 8, 32 ); ptr += 32;
    /* v_parameters: offset 40, 只取前20字节有效数据 (跳过padding) */
    memcpy( ptr, data + 40, 20 ); ptr += 20;
    /* bump_seed: offset 72, 1 byte */
    *ptr++ = data[72];
    /* bin_step: offset 80, 2 bytes */
    memcpy( ptr, data + 80, 2 ); ptr += 2;
    /* active_id: offset 76, 4 bytes */
    memcpy( ptr, data + 76, 4 ); ptr += 4;
    /* token_x_mint: offset 88, 32 bytes */
    memcpy( ptr, data + 88, 32 ); ptr += 32;
    /* token_y_mint: offset 120, 32 bytes */
    memcpy( ptr, data + 120, 32 ); ptr += 32;
    /* reserve_x: offset 152, 32 bytes */
    memcpy( ptr, data + 152, 32 ); ptr += 32;
    /* reserve_y: offset 184, 32 bytes */
    memcpy( ptr, data + 184, 32 ); ptr += 32;
    /* bin_array_bitmap: offset 744, 128 bytes */
    memcpy( ptr, data + 744, 128 ); ptr += 128;
    /* oracle: offset 872, 32 bytes */
    memcpy( ptr, data + 872, 32 ); ptr += 32;
    return (unsigned long)(ptr - out);
}

/* TokenAccount: 多段截取 (40 bytes) */
static unsigned long
encode_token_account( const unsigned char * data,
                      unsigned char * out, unsigned long out_max ) {
    if( out_max < 48 ) {
        FD_LOG_CRIT(( "[COMPACT] TokenAccount encode failed: out_max=%lu < 48", out_max ));
        return 0;
    }
    unsigned char * ptr = write_header( out, FD_COMPACT_MSG_TOKEN_ACCOUNT );
    /* mint: offset 0, 32 bytes */
    memcpy( ptr, data, 32 ); ptr += 32;
    /* amount: offset 64, 8 bytes */
    memcpy( ptr, data + 64, 8 ); ptr += 8;
    return 48;  /* 8 header + 40 fields */
}

/* 根据 filter name 获取对应的 compact type (在 compile 时调用一次) */
unsigned char
fd_compact_get_type_for_filter( const char * filter_name ) {
    if( !filter_name ) return FD_COMPACT_MSG_RAW;

    if( strcmp( filter_name, "ray-clmm-ticks" ) == 0 ) return FD_COMPACT_MSG_CLMM_TICK_ARRAY;
    if( strcmp( filter_name, "orca-ticks" ) == 0 ) return FD_COMPACT_MSG_WHIRLPOOL_TICK_ARRAY;
    if( strcmp( filter_name, "meteora-bins" ) == 0 ) return FD_COMPACT_MSG_METEORA_BIN_ARRAY;
    if( strcmp( filter_name, "openorders" ) == 0 ) return FD_COMPACT_MSG_OPENORDERS;
    if( strcmp( filter_name, "orca-pools" ) == 0 ) return FD_COMPACT_MSG_WHIRLPOOL;
    if( strcmp( filter_name, "ray-clmm-pools" ) == 0 ) return FD_COMPACT_MSG_CLMM_POOL;
    if( strcmp( filter_name, "meteora-pools" ) == 0 ) return FD_COMPACT_MSG_LBPAIR;
    if( strcmp( filter_name, "ray-amm-vaults" ) == 0 ) return FD_COMPACT_MSG_TOKEN_ACCOUNT;

    return FD_COMPACT_MSG_RAW;  /* 不编码 */
}

/* 主入口: 根据预计算的 type 编码 (无 strcmp) */
unsigned long
fd_compact_encode( unsigned char compact_type,
                   const unsigned char * data, unsigned long,
                   unsigned char * out, unsigned long out_max ) {
    /* 如果 compact 编码已禁用，返回 0 表示原样发送 */
    if( !g_compact_enabled.load( std::memory_order_relaxed ) ) {
        return 0;
    }

    switch( compact_type ) {
        /* 稀疏数组编码 */
        case FD_COMPACT_MSG_CLMM_TICK_ARRAY:
            return encode_clmm_tick_array( data, out, out_max );
        case FD_COMPACT_MSG_WHIRLPOOL_TICK_ARRAY:
            return encode_whirlpool_tick_array( data, out, out_max );
        case FD_COMPACT_MSG_METEORA_BIN_ARRAY:
            return encode_meteora_bin_array( data, out, out_max );
        /* 多段截取编码 */
        case FD_COMPACT_MSG_OPENORDERS:
            return encode_openorders( data, out, out_max );
        case FD_COMPACT_MSG_WHIRLPOOL:
            return encode_whirlpool( data, out, out_max );
        case FD_COMPACT_MSG_CLMM_POOL:
            return encode_clmm_pool( data, out, out_max );
        case FD_COMPACT_MSG_LBPAIR:
            return encode_lbpair( data, out, out_max );
        case FD_COMPACT_MSG_TOKEN_ACCOUNT:
            return encode_token_account( data, out, out_max );
        case FD_COMPACT_MSG_RAW:
            return 0;  /* 不编码，原样发送 */
        default:
            /* 未知类型是 bug，应该立即暴露 */
            FD_LOG_CRIT(( "[COMPACT] Unknown compact_type=%d, this is a bug!", (int)compact_type ));
            return 0;
    }
}
