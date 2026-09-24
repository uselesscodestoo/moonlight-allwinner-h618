/* Compile-only gate for the pinned aarch64-none-linux-gnu CedarC headers.
 * No library is loaded and no device is opened by this check. It does not
 * establish binary provenance, decoder behavior, or the kernel ioctl ABI. */
#include <stddef.h>
#include "vdecoder.h"

#if !defined(__linux__) || !defined(__aarch64__)
#error "K2B CedarC ABI check requires Linux AArch64"
#endif
#if !defined(TINA_LINUX_SUPPORT) || TINA_LINUX_SUPPORT != 0
#error "K2B CedarC ABI requires TINA_LINUX_SUPPORT=0"
#endif

typedef char k2b_vconfig_size[(sizeof(VConfig) == 224) ? 1 : -1];
typedef char k2b_stream_size[(sizeof(VideoStreamInfo) == 72) ? 1 : -1];
typedef char k2b_veops_offset[(offsetof(VConfig, veOpsS) == 152) ? 1 : -1];
typedef char k2b_veself_offset[(offsetof(VConfig, pVeOpsSelf) == 160) ? 1 : -1];

/* The memory callbacks are consumed across the private library boundary.
 * Freeze every slot, not just the table size: two swapped pointers have the
 * same total size but invoke incompatible functions. */
typedef char k2b_memops_size[(sizeof(struct ScMemOpsS) == 192) ? 1 : -1];
#define K2B_MEMOPS_SLOT(field, slot) \
  typedef char k2b_memops_slot_##field[ \
    (offsetof(struct ScMemOpsS, field) == (slot) * 8u) ? 1 : -1]
K2B_MEMOPS_SLOT(open, 0);
K2B_MEMOPS_SLOT(open2, 1);
K2B_MEMOPS_SLOT(close, 2);
K2B_MEMOPS_SLOT(total_size, 3);
K2B_MEMOPS_SLOT(palloc, 4);
K2B_MEMOPS_SLOT(palloc_no_cache, 5);
K2B_MEMOPS_SLOT(pfree, 6);
K2B_MEMOPS_SLOT(flush_cache, 7);
K2B_MEMOPS_SLOT(ve_get_phyaddr, 8);
K2B_MEMOPS_SLOT(ve_get_viraddr, 9);
K2B_MEMOPS_SLOT(cpu_get_phyaddr, 10);
K2B_MEMOPS_SLOT(cpu_get_viraddr, 11);
K2B_MEMOPS_SLOT(mem_set, 12);
K2B_MEMOPS_SLOT(mem_cpy, 13);
K2B_MEMOPS_SLOT(mem_read, 14);
K2B_MEMOPS_SLOT(mem_write, 15);
K2B_MEMOPS_SLOT(setup, 16);
K2B_MEMOPS_SLOT(shutdown, 17);
K2B_MEMOPS_SLOT(get_ve_addr_offset, 18);
K2B_MEMOPS_SLOT(get_debug_info, 19);
K2B_MEMOPS_SLOT(get_vir_by_fd, 20);
K2B_MEMOPS_SLOT(get_phy_by_fd, 21);
K2B_MEMOPS_SLOT(free_phy_by_fd, 22);
K2B_MEMOPS_SLOT(get_fd_by_vir, 23);
#undef K2B_MEMOPS_SLOT

int main(void)
{
  return 0;
}
