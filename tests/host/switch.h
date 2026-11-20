#pragma once
#include <cstdint>

// Only the platform services used by the real background Baidu client are stubbed.
struct AccountUid { uint64_t uid[2]; };
struct FsSaveDataInfo {};
struct SocketInitConfig
{
    unsigned tcp_tx_buf_size, tcp_rx_buf_size, tcp_tx_buf_max_size, tcp_rx_buf_max_size;
    unsigned udp_tx_buf_size, udp_rx_buf_size, sb_efficiency, num_bsd_sessions, bsd_service_type;
};
constexpr unsigned BsdServiceType_User = 0;
constexpr unsigned NifmServiceType_User = 0;
#define R_SUCCEEDED(result) ((result) == 0)
inline int socketInitialize(const SocketInitConfig *) { return 0; }
inline int nifmInitialize(unsigned) { return 0; }
inline void socketExit() {}
inline void nifmExit() {}
