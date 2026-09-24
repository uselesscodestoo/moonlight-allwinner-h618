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

int main(void)
{
  return 0;
}
