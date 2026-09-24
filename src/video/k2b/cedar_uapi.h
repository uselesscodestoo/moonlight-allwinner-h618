#ifndef K2B_CEDAR_UAPI_H
#define K2B_CEDAR_UAPI_H
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint64_t u64;
#include <cedar_ve.h>
typedef char k2b_dma_size[(sizeof(struct dma_buf_param) == 8) ? 1 : -1];
typedef char k2b_dma_offset[(offsetof(struct dma_buf_param, phy_addr) == 4) ? 1 : -1];
typedef char k2b_cache_size[(sizeof(struct cache_range) == 16) ? 1 : -1];
typedef char k2b_commands[(IOCTL_ENGINE_REQ == 0x206 && IOCTL_ENGINE_REL == 0x207 &&
    IOCTL_MAP_DMA_BUF == 0x504 && IOCTL_UNMAP_DMA_BUF == 0x505 &&
    IOCTL_FLUSH_CACHE_RANGE == 0x506) ? 1 : -1];
#define K2B_CEDAR_HEAP_ALLOC _IO('C', 0x42)
#endif
