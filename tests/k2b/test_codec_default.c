#include "platform.h"

int main(void)
{
    if (!platform_prefers_codec(K2B, CODEC_HEVC)) {
        fputs("FAIL: K2B auto codec must prefer HEVC\n", stderr);
        return 1;
    }
    if (!platform_prefers_codec(K2B, CODEC_H264) ||
        platform_prefers_codec(K2B, CODEC_AV1) ||
        platform_prefers_codec(SDL, CODEC_HEVC)) {
        fputs("FAIL: codec fallback or other platform preference changed\n", stderr);
        return 1;
    }
    puts("PASS: K2B auto prefers HEVC; AVC retained; other platforms unchanged");
    return 0;
}
