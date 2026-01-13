#pragma once
#include <cstdint>
#include <cstring>

#define FD_COMPACT_MAGIC 0x504D30CFU  /* 0xFDC04D50 little-endian */

enum FdCompactMsgType : unsigned char {
    FD_COMPACT_MSG_RAW                  = 0x00,  /* 不编码 */
    FD_COMPACT_MSG_OPENORDERS           = 0x01,  /* 3228 -> 24 bytes */
    FD_COMPACT_MSG_WHIRLPOOL            = 0x02,  /* 653 -> 176 bytes */
    FD_COMPACT_MSG_CLMM_POOL            = 0x03,  /* 1544 -> 559 bytes */
    FD_COMPACT_MSG_LBPAIR               = 0x05,  /* 904 -> 355 bytes */
    FD_COMPACT_MSG_TOKEN_ACCOUNT        = 0x06,  /* 165 -> 48 bytes */
    FD_COMPACT_MSG_CLMM_TICK_ARRAY      = 0x10,  /* Raydium CLMM TickArray */
    FD_COMPACT_MSG_WHIRLPOOL_TICK_ARRAY = 0x11,  /* Orca Whirlpool TickArray */
    FD_COMPACT_MSG_METEORA_BIN_ARRAY    = 0x12,  /* Meteora DLMM BinArray */
    FD_COMPACT_MSG_NO_MATCH             = 0xFF,  /* 不匹配 */
};

/* 根据 filter name 获取对应的 compact type (在 compile 时调用一次) */
unsigned char fd_compact_get_type_for_filter( const char * filter_name );

/* 根据 type 编码账户数据
   compact_type: 预计算的类型
   data/data_sz: 原始账户数据
   out/out_max: 输出缓冲区
   返回: 编码后长度, 0 表示不需要编码(原样发送) */
unsigned long fd_compact_encode( unsigned char compact_type,
    const unsigned char * data, unsigned long data_sz,
    unsigned char * out, unsigned long out_max );

/* Compact 编码开关控制 (通过 SIGHUP 信号切换)
   默认开启。禁用时所有账户数据原样发送。 */
bool fd_compact_is_enabled( void );
void fd_compact_set_enabled( bool enabled );
void fd_compact_toggle( void );  /* 切换开关状态 */
