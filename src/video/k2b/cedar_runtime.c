#define _GNU_SOURCE
#include "cedar_runtime.h"
#include <videoengine.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { BASE, MEMORY, SBM, FBM, VDECODER, VE, ENGINE, H264, VCS, H265, SHIM, LIB_COUNT };
static const char *const libraries[LIB_COUNT] = {
    "libcdc_base.so", "libMemAdapter.so", "libsbm.so", "libfbm.so",
    "libvdecoder.so", "libVE.so", "libvideoengine.so", "libawh264.so",
    "libvdecVcs.so", "libawh265.so", "libk2b_cedar54_compat.so"
};

static pthread_mutex_t runtime_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct {
    struct k2b_cedar_runtime_status status;
    char directory[PATH_MAX];
    char paths[LIB_COUNT][PATH_MAX];
    void *handles[SHIM];
    struct k2b_cedar_api api;
} runtime;

/* Called only while holding runtime_mutex. Identifiers remain visible even
 * when a dynamic-loader error is very long; no unbounded path enters detail. */
static int fail(int error, const char *stage, const char *name, const char *reason)
{
    if (!runtime.status.error) {
        runtime.status.error = error ? error : EIO;
        snprintf(runtime.status.detail, sizeof(runtime.status.detail),
                 "%.40s %.64s: %.140s", stage, name, reason ? reason : "failed");
    }
    errno = runtime.status.error;
    return -1;
}

/* realpath(NULL) avoids a caller-owned buffer overflow on unusual filesystems;
 * reject any result that cannot fit the bounded process-lifetime path store. */
static int canonical_path(const char *path, char result[PATH_MAX])
{
    char *resolved = realpath(path, NULL);
    size_t length;
    if (!resolved) return -1;
    length = strlen(resolved);
    if (length >= PATH_MAX) {
        free(resolved);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(result, resolved, length + 1);
    free(resolved);
    return 0;
}

static int require_path(const char *path, int library, const char *stage, const char *name)
{
    char resolved[PATH_MAX];
    if (!path || !path[0]) return fail(ELIBBAD, stage, name, "missing object path");
    if (canonical_path(path, resolved) < 0) return fail(errno, stage, name, "realpath failed");
    if (strcmp(resolved, runtime.paths[library]))
        return fail(EXDEV, stage, name, "provider is outside the exact private path");
    return 0;
}

static int check_files(const char *directory)
{
    struct stat st;
    int i;
    if (canonical_path(directory, runtime.directory) < 0)
        return fail(errno, "root", "directory", "realpath failed");
    if (stat(runtime.directory, &st) < 0)
        return fail(errno, "root", "directory", "stat failed");
    if (!S_ISDIR(st.st_mode)) return fail(ENOTDIR, "root", "directory", "not a directory");
    for (i = 0; i < LIB_COUNT; ++i) {
        int length = snprintf(runtime.paths[i], sizeof(runtime.paths[i]), "%s%s%s",
                              runtime.directory, !strcmp(runtime.directory, "/") ? "" : "/",
                              libraries[i]);
        if (length < 0 || (size_t)length >= sizeof(runtime.paths[i]))
            return fail(ENAMETOOLONG, "file", libraries[i], "path exceeds limit");
        if (require_path(runtime.paths[i], i, "file", libraries[i]) < 0) return -1;
        if (stat(runtime.paths[i], &st) < 0)
            return fail(errno, "file", libraries[i], "stat failed");
        if (!S_ISREG(st.st_mode)) return fail(EINVAL, "file", libraries[i], "not a regular file");
    }
    return 0;
}

static int require_environment(const char *name, const char *expected)
{
    const char *value = getenv(name);
    if (!value || strcmp(value, expected))
        return fail(EINVAL, "environment", name, "requires the exact private-runtime value");
    return 0;
}

static int check_environment(void)
{
    const char *audit;
    struct stat st;
    if (require_environment("LD_LIBRARY_PATH", runtime.directory) < 0 ||
        require_environment("LD_PRELOAD", runtime.paths[SHIM]) < 0 ||
        require_environment("CEDAR_K2B_KERNEL54_COMPAT", "1") < 0) return -1;
    audit = getenv("LD_AUDIT");
    if (audit && audit[0]) return fail(EINVAL, "environment", "LD_AUDIT", "must be absent or empty");
    if (lstat("/etc/cedarc.conf", &st) == 0)
        return fail(EEXIST, "configuration", "/etc/cedarc.conf", "must not exist, including symlinks");
    if (errno != ENOENT) return fail(errno, "configuration", "/etc/cedarc.conf", "lstat failed");
    return 0;
}

static void *resolve_symbol(void *handle, int library, const char *name)
{
    void *address;
    const char *error;
    Dl_info info;
    dlerror();
    address = dlsym(handle, name);
    error = dlerror();
    if (error || !address) {
        fail(ELIBBAD, "symbol", name, error ? error : "null address");
        return NULL;
    }
    memset(&info, 0, sizeof(info));
    if (!dladdr(address, &info)) {
        fail(ELIBBAD, "symbol provider", name, "dladdr failed");
        return NULL;
    }
    if (require_path(info.dli_fname, library, "symbol provider", name) < 0) return NULL;
    return address;
}

struct mapped_set { unsigned int count[SHIM]; int complete, error; };

static int check_mapped_object(struct dl_phdr_info *info, size_t size, void *opaque)
{
    struct mapped_set *set = opaque;
    const char *name, *slash;
    int i;
    (void)size;
    if (!info->dlpi_name || !info->dlpi_name[0]) return 0;
    slash = strrchr(info->dlpi_name, '/');
    name = slash ? slash + 1 : info->dlpi_name;
    for (i = 0; i < SHIM; ++i) {
        if (strcmp(name, libraries[i])) continue;
        if (require_path(info->dlpi_name, i, set->complete ? "final mapping" : "preload mapping", name) < 0) {
            set->error = 1;
            return 1;
        }
        if (++set->count[i] > 1 && set->complete) {
            fail(ELIBBAD, "final mapping", name, "duplicate library");
            set->error = 1;
            return 1;
        }
    }
    return 0;
}

static int check_mapped_set(int complete)
{
    struct mapped_set set = {{0}, 0, 0};
    int i, rc;
    set.complete = complete;
    rc = dl_iterate_phdr(check_mapped_object, &set);
    if (set.error) return -1;
    if (rc) return fail(EIO, "mapping", "dl_iterate_phdr", "iteration failed");
    if (complete) for (i = 0; i < SHIM; ++i)
        if (set.count[i] != 1) return fail(ELIBBAD, "final mapping", libraries[i], "missing library");
    return 0;
}

static int load_libraries(void)
{
    int i;
    for (i = 0; i < SHIM; ++i) {
        struct link_map *map = NULL;
        const char *error;
        dlerror();
        /* Retain every acquired handle forever, even when any later gate fails. */
        runtime.handles[i] = dlopen(runtime.paths[i], RTLD_NOW | RTLD_LOCAL);
        error = dlerror();
        if (!runtime.handles[i] || error) return fail(ELIBBAD, "dlopen", libraries[i], error);
        dlerror();
        if (dlinfo(runtime.handles[i], RTLD_DI_LINKMAP, &map) != 0) {
            error = dlerror();
            return fail(ELIBBAD, "dlinfo", libraries[i], error);
        }
        error = dlerror();
        if (error || !map) return fail(ELIBBAD, "dlinfo", libraries[i], error);
        if (require_path(map->l_name, i, "dlinfo", libraries[i]) < 0) return -1;
    }
    return check_mapped_set(1);
}

static int initialize(const char *directory)
{
    /* GNU typeof obtains the exact types from the fixed vendor declarations.
     * POSIX dlsym function-pointer conversions are supported by this target. */
    __typeof__(&VDecoderRegister) register_decoder;
    VDecoderCreator *creator, *hevc_creator;
    struct k2b_cedar_api candidate;
    if (check_files(directory) < 0 || check_environment() < 0) return -1;
    if (!resolve_symbol(RTLD_DEFAULT, SHIM, "ioctl")) return -1;
    if (check_mapped_set(0) < 0 || load_libraries() < 0) return -1;

#define RESOLVE(member, library, symbol) do { \
    candidate.member = (__typeof__(candidate.member)) \
        resolve_symbol(runtime.handles[library], library, #symbol); \
    if (!candidate.member) return -1; \
} while (0)
    RESOLVE(create, VDECODER, CreateVideoDecoder);
    RESOLVE(destroy, VDECODER, DestroyVideoDecoder);
    RESOLVE(initialize, VDECODER, InitializeVideoDecoder);
    RESOLVE(reset, VDECODER, ResetVideoDecoder);
    RESOLVE(decode, VDECODER, DecodeVideoStream);
    RESOLVE(request_stream, VDECODER, RequestVideoStreamBuffer);
    RESOLVE(submit_stream, VDECODER, SubmitVideoStreamData);
    RESOLVE(stream_frames, VDECODER, VideoStreamFrameNum);
    RESOLVE(request_picture, VDECODER, RequestPicture);
    RESOLVE(return_picture, VDECODER, ReturnPicture);
    RESOLVE(mem_ops, MEMORY, MemAdapterGetOpsS);
    RESOLVE(memory_begin, MEMORY, k2b_cedar_memory_begin);
    RESOLVE(memory_end, MEMORY, k2b_cedar_memory_end);
    RESOLVE(memory_status, MEMORY, k2b_cedar_memory_status);
    RESOLVE(memory_describe, MEMORY, k2b_cedar_memory_describe);
    RESOLVE(memory_pin, MEMORY, k2b_cedar_memory_pin);
    RESOLVE(memory_unpin, MEMORY, k2b_cedar_memory_unpin);
#undef RESOLVE
    register_decoder = (__typeof__(register_decoder))
        resolve_symbol(runtime.handles[ENGINE], ENGINE, "VDecoderRegister");
    if (!register_decoder) return -1;
    creator = (VDecoderCreator *)resolve_symbol(runtime.handles[H264], H264, "CreateH264Decoder");
    if (!creator) return -1;
    hevc_creator = (VDecoderCreator *)resolve_symbol(runtime.handles[H265], H265, "CreateH265Decoder");
    if (!hevc_creator) return -1;
    /* The fixed vendor registration routine has unchecked allocation on OOM.
     * A nonzero return is handled; this cannot recover a vendor-internal crash. */
    if (register_decoder(VIDEO_CODEC_FORMAT_H264, "h264", creator, 0) != 0)
        return fail(ELIBBAD, "registration", "VDecoderRegister", "H.264 registration returned nonzero");
    if (register_decoder(VIDEO_CODEC_FORMAT_H265, "h265", hevc_creator, 0) != 0)
        return fail(ELIBBAD, "registration", "VDecoderRegister", "H.265 registration returned nonzero");
    runtime.api = candidate;
    runtime.status.ready = 1;
    return 0;
}

int k2b_cedar_runtime_load(const char *directory, const struct k2b_cedar_api **out)
{
    int rc, error;
    if (!directory || directory[0] != '/' || !out) { errno = EINVAL; return -1; }
    error = pthread_mutex_lock(&runtime_mutex);
    if (error) { errno = error; return -1; }
    if (runtime.status.error) {
        errno = runtime.status.error;
        rc = -1;
    } else if (runtime.status.ready) {
        char resolved[PATH_MAX];
        rc = canonical_path(directory, resolved);
        if (!rc && strcmp(resolved, runtime.directory)) { errno = EXDEV; rc = -1; }
    } else {
        rc = initialize(directory);
    }
    error = errno;
    if (!rc) *out = &runtime.api;
    pthread_mutex_unlock(&runtime_mutex);
    errno = error;
    return rc;
}

int k2b_cedar_runtime_status(struct k2b_cedar_runtime_status *out)
{
    int error;
    if (!out) { errno = EINVAL; return -1; }
    error = pthread_mutex_lock(&runtime_mutex);
    if (error) { errno = error; return -1; }
    *out = runtime.status;
    pthread_mutex_unlock(&runtime_mutex);
    return 0;
}
