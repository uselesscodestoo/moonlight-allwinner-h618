/* Link-only evidence, never run by the build or tests. Volatile references
 * retain real header-checked API relocations even in optimized builds. */
#include "vdecoder.h"
#include "cedar_memory.h"

#define REFERENCE(api) static __typeof__(api) *volatile ref_##api = api
REFERENCE(CreateVideoDecoder);
REFERENCE(InitializeVideoDecoder);
REFERENCE(DestroyVideoDecoder);
REFERENCE(RequestVideoStreamBuffer);
REFERENCE(SubmitVideoStreamData);
REFERENCE(DecodeVideoStream);
REFERENCE(RequestPicture);
REFERENCE(ReturnPicture);
REFERENCE(MemAdapterGetOpsS);
REFERENCE(k2b_cedar_memory_begin);
REFERENCE(k2b_cedar_memory_end);
#undef REFERENCE

int main(void)
{
    return !(ref_CreateVideoDecoder && ref_InitializeVideoDecoder &&
        ref_DestroyVideoDecoder && ref_RequestVideoStreamBuffer &&
        ref_SubmitVideoStreamData && ref_DecodeVideoStream &&
        ref_RequestPicture && ref_ReturnPicture && ref_MemAdapterGetOpsS &&
        ref_k2b_cedar_memory_begin && ref_k2b_cedar_memory_end);
}
