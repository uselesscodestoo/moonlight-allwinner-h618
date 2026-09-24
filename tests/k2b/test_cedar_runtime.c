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
#include <sys/wait.h>
#include <unistd.h>

/* No Cedar libraries or devices are used. Only the libc/loader boundary is
 * replaced; production validation, state, registration and mutexes all run. */
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed (errno=%d)\n", \
            __FILE__, __LINE__, #expr, errno); _exit(1); \
} } while (0)
#define ROOT "/private/cedar"
#define UNUSED __attribute__((unused))

static const char *const names[] = {
    "libcdc_base.so", "libMemAdapter.so", "libsbm.so", "libfbm.so",
    "libvdecoder.so", "libVE.so", "libvideoengine.so", "libawh264.so",
    "libvdecVcs.so", "libk2b_cedar54_compat.so"
};
static char paths[10][128];
static struct link_map maps[9];
static int opens, registrations, provider_checks, maps_checks;
static unsigned int validated_files;
static const char *pending_error;

enum fault {
    NONE, ROOT_REALPATH, ROOT_STAT, ROOT_TYPE, ROOT_LONG,
    FILE_REALPATH, FILE_OUTSIDE, FILE_STAT, FILE_TYPE,
    ENV_MISSING, ENV_WRONG, ENV_AUDIT, CONFIG_FILE, CONFIG_DIR,
    CONFIG_SYMLINK, CONFIG_ERROR, PRELOAD_FOREIGN, PRELOAD_CANONICAL,
    FINAL_MISSING, FINAL_DUPLICATE, FINAL_FOREIGN, OPEN_FAIL,
    INFO_FAIL, INFO_NULL, INFO_WRONG, SYMBOL_MISSING, SYMBOL_DLERROR,
    PROVIDER_WRONG, PROVIDER_FAIL, PROVIDER_REALPATH, REGISTER_FAIL
};
static enum fault fault;
static int target;

/* Typed stand-ins abort if the loader invokes anything except registration. */
static VideoDecoder *fake_create(void) { CHECK(0); return NULL; }
static void fake_destroy(VideoDecoder *a UNUSED) { CHECK(0); }
static int fake_initialize(VideoDecoder *a UNUSED, VideoStreamInfo *b UNUSED,
                           VConfig *c UNUSED) { CHECK(0); return 0; }
static void fake_reset(VideoDecoder *a UNUSED) { CHECK(0); }
static int fake_decode(VideoDecoder *a UNUSED, int b UNUSED, int c UNUSED,
                       int d UNUSED, int64_t e UNUSED) { CHECK(0); return 0; }
static int fake_request_stream(VideoDecoder *a UNUSED, int b UNUSED,
    char **c UNUSED, int *d UNUSED, char **e UNUSED, int *f UNUSED,
    int g UNUSED) { CHECK(0); return 0; }
static int fake_submit_stream(VideoDecoder *a UNUSED, VideoStreamDataInfo *b UNUSED,
                              int c UNUSED) { CHECK(0); return 0; }
static int fake_stream_frames(VideoDecoder *a UNUSED, int b UNUSED) { CHECK(0); return 0; }
static VideoPicture *fake_request_picture(VideoDecoder *a UNUSED, int b UNUSED)
{ CHECK(0); return NULL; }
static int fake_return_picture(VideoDecoder *a UNUSED, VideoPicture *b UNUSED)
{ CHECK(0); return 0; }
static struct ScMemOpsS *fake_mem_ops(void) { CHECK(0); return NULL; }
static int fake_memory_begin(void) { CHECK(0); return 0; }
static int fake_memory_end(void) { CHECK(0); return 0; }
static int fake_memory_status(struct k2b_cedar_memory_status *a UNUSED)
{ CHECK(0); return 0; }
static int fake_memory_describe(const void *a UNUSED, struct k2b_cedar_buffer *b UNUSED)
{ CHECK(0); return 0; }
static int fake_memory_pin(const void *a UNUSED, struct k2b_cedar_pin *b UNUSED)
{ CHECK(0); return 0; }
static int fake_memory_unpin(const struct k2b_cedar_pin *a UNUSED) { CHECK(0); return 0; }
static DecoderInterface *fake_creator(VideoEngine *a UNUSED) { CHECK(0); return NULL; }
static int fake_register(enum EVIDEOCODECFORMAT format, char *desc,
                         VDecoderCreator *creator, int soft)
{
    CHECK(format == VIDEO_CODEC_FORMAT_H264 && format == 0x115);
    CHECK(strcmp(desc, "h264") == 0 && creator == fake_creator && soft == 0);
    CHECK(opens == 9 && provider_checks == 20 && maps_checks == 2);
    ++registrations;
    return fault == REGISTER_FAIL ? -7 : 0;
}
static int fake_ioctl(int a UNUSED, unsigned long b UNUSED, ...) { CHECK(0); return 0; }

struct symbol { const char *name; void *address; int library; };
static const struct symbol symbols[] = {
    {"CreateVideoDecoder", (void *)fake_create, 4},
    {"DestroyVideoDecoder", (void *)fake_destroy, 4},
    {"InitializeVideoDecoder", (void *)fake_initialize, 4},
    {"ResetVideoDecoder", (void *)fake_reset, 4},
    {"DecodeVideoStream", (void *)fake_decode, 4},
    {"RequestVideoStreamBuffer", (void *)fake_request_stream, 4},
    {"SubmitVideoStreamData", (void *)fake_submit_stream, 4},
    {"VideoStreamFrameNum", (void *)fake_stream_frames, 4},
    {"RequestPicture", (void *)fake_request_picture, 4},
    {"ReturnPicture", (void *)fake_return_picture, 4},
    {"MemAdapterGetOpsS", (void *)fake_mem_ops, 1},
    {"k2b_cedar_memory_begin", (void *)fake_memory_begin, 1},
    {"k2b_cedar_memory_end", (void *)fake_memory_end, 1},
    {"k2b_cedar_memory_status", (void *)fake_memory_status, 1},
    {"k2b_cedar_memory_describe", (void *)fake_memory_describe, 1},
    {"k2b_cedar_memory_pin", (void *)fake_memory_pin, 1},
    {"k2b_cedar_memory_unpin", (void *)fake_memory_unpin, 1},
    {"VDecoderRegister", (void *)fake_register, 6},
    {"CreateH264Decoder", (void *)fake_creator, 7},
    {"ioctl", (void *)fake_ioctl, 9}
};

static int path_index(const char *path)
{
    int i;
    for (i = 0; i < 10; ++i) if (!strcmp(path, paths[i])) return i;
    return -1;
}

char *__wrap_getenv(const char *name)
{
    static const char *const keys[] = {
        "LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT", "CEDAR_K2B_KERNEL54_COMPAT"
    };
    int i;
    for (i = 0; i < 4; ++i) if (!strcmp(name, keys[i])) {
        if (fault == ENV_MISSING && i == target) return NULL;
        if (fault == ENV_WRONG && i == target) return (char *)"wrong";
        if (i == 2) return fault == ENV_AUDIT ? (char *)"audit.so" : (char *)"";
        if (i == 0) return (char *)ROOT;
        if (i == 1) return paths[9];
        return (char *)"1";
    }
    CHECK(0);
    return NULL;
}

char *__wrap_realpath(const char *path, char *resolved)
{
    const char *value = path;
    char long_path[PATH_MAX + 2];
    int i = path_index(path);
    if (!strcmp(path, "/alias")) value = ROOT;
    if (!strcmp(path, ROOT) && fault == ROOT_REALPATH) { errno = EACCES; return NULL; }
    if (!strcmp(path, ROOT) && fault == ROOT_LONG) {
        memset(long_path, 'a', sizeof(long_path) - 1);
        long_path[0] = '/'; long_path[sizeof(long_path) - 1] = 0;
        value = long_path;
    }
    if (i == target && fault == FILE_REALPATH) { errno = ENOENT; return NULL; }
    if (i == target && fault == FILE_OUTSIDE) value = "/outside/library.so";
    if (!strcmp(path, "/provider/failure")) { errno = EACCES; return NULL; }
    if (resolved) { CHECK(strlen(value) < PATH_MAX); return strcpy(resolved, value); }
    return strdup(value);
}

int __wrap_stat(const char *path, struct stat *st)
{
    int i = path_index(path);
    memset(st, 0, sizeof(*st));
    if (!strcmp(path, ROOT)) {
        if (fault == ROOT_STAT) { errno = EACCES; return -1; }
        st->st_mode = fault == ROOT_TYPE ? S_IFREG : S_IFDIR;
    } else {
        CHECK(i >= 0);
        if (fault == FILE_STAT && target == i) { errno = EACCES; return -1; }
        st->st_mode = fault == FILE_TYPE && target == i ? S_IFDIR : S_IFREG;
        if (S_ISREG(st->st_mode)) validated_files |= 1U << i;
    }
    return 0;
}

int __wrap_lstat(const char *path, struct stat *st)
{
    CHECK(!strcmp(path, "/etc/cedarc.conf"));
    memset(st, 0, sizeof(*st));
    if (fault == CONFIG_FILE || fault == CONFIG_DIR || fault == CONFIG_SYMLINK) {
        st->st_mode = fault == CONFIG_FILE ? S_IFREG : fault == CONFIG_DIR ? S_IFDIR : S_IFLNK;
        return 0;
    }
    errno = fault == CONFIG_ERROR ? EACCES : ENOENT;
    return -1;
}

void *__wrap_dlopen(const char *path, int flags)
{
    int i = path_index(path);
    CHECK(i == opens && i < 9);
    CHECK(flags == (RTLD_NOW | RTLD_LOCAL));
    CHECK(validated_files == (1U << 10) - 1 && provider_checks == 1 && maps_checks == 1);
    ++opens;
    if (fault == OPEN_FAIL && target == i) { pending_error = "injected dlopen failure"; return NULL; }
    return &maps[i];
}

int __wrap_dlclose(void *handle UNUSED) { CHECK(0); return -1; }

int __wrap_dlinfo(void *handle, int request, void *result)
{
    int i = (struct link_map *)handle - maps;
    CHECK(i >= 0 && i < 9 && request == RTLD_DI_LINKMAP);
    if (fault == INFO_FAIL && target == i) { pending_error = "injected dlinfo failure"; return -1; }
    if (fault == INFO_WRONG && target == i) maps[i].l_name = (char *)"/outside/library.so";
    *(struct link_map **)result = fault == INFO_NULL && target == i ? NULL : &maps[i];
    return 0;
}

char *__wrap_dlerror(void)
{
    char *result = (char *)pending_error;
    pending_error = NULL;
    return result;
}

void *__wrap_dlsym(void *handle, const char *name)
{
    size_t i;
    CHECK(pending_error == NULL);
    for (i = 0; i < sizeof(symbols) / sizeof(*symbols); ++i) {
        if (strcmp(name, symbols[i].name)) continue;
        CHECK(handle == (i == 19 ? RTLD_DEFAULT : (void *)&maps[symbols[i].library]));
        if (i != 19) CHECK(opens == 9 && maps_checks == 2);
        if (fault == SYMBOL_MISSING && target == (int)i) return NULL;
        if (fault == SYMBOL_DLERROR && target == (int)i) pending_error = "injected dlsym failure";
        return symbols[i].address;
    }
    CHECK(0); /* In particular: never AddVDPlugin or CedarPluginVDInit. */
    return NULL;
}

int __wrap_dladdr(const void *address, Dl_info *info)
{
    size_t i;
    for (i = 0; i < sizeof(symbols) / sizeof(*symbols); ++i) {
        if (address != symbols[i].address) continue;
        ++provider_checks;
        memset(info, 0, sizeof(*info));
        info->dli_fname = paths[symbols[i].library];
        if (target == (int)i) {
            if (fault == PROVIDER_FAIL) return 0;
            if (fault == PROVIDER_WRONG) info->dli_fname = "/foreign/provider.so";
            if (fault == PROVIDER_REALPATH) info->dli_fname = "/provider/failure";
        }
        return 1;
    }
    CHECK(0);
    return 0;
}

static int visit(int (*callback)(struct dl_phdr_info *, size_t, void *),
                 void *data, const char *path)
{
    struct dl_phdr_info info;
    memset(&info, 0, sizeof(info));
    info.dlpi_name = path;
    return callback(&info, sizeof(info), data);
}

int __wrap_dl_iterate_phdr(int (*callback)(struct dl_phdr_info *, size_t, void *), void *data)
{
    int i, rc;
    char foreign[128];
    ++maps_checks;
    rc = visit(callback, data, "");
    if (!rc) rc = visit(callback, data, "linux-vdso.so.1");
    if (!rc) rc = visit(callback, data, "/lib/libc.so.6");
    if (rc) return rc;
    if (!opens) {
        if (fault == PRELOAD_FOREIGN) {
            snprintf(foreign, sizeof(foreign), "/foreign/%s", names[target]);
            return visit(callback, data, foreign);
        }
        if (fault == PRELOAD_CANONICAL) return visit(callback, data, paths[target]);
        return 0;
    }
    for (i = 0; i < 9; ++i) {
        if (fault == FINAL_MISSING && target == i) continue;
        snprintf(foreign, sizeof(foreign), "/foreign/%s", names[i]);
        rc = visit(callback, data, fault == FINAL_FOREIGN && target == i ? foreign : paths[i]);
        if (!rc && fault == FINAL_DUPLICATE && target == i) rc = visit(callback, data, paths[i]);
        if (rc) return rc;
    }
    return 0;
}

static const struct k2b_cedar_api sentinel;

static void check_api(const struct k2b_cedar_api *api)
{
    CHECK(api && api != &sentinel);
    CHECK(api->create == fake_create && api->destroy == fake_destroy);
    CHECK(api->initialize == fake_initialize && api->reset == fake_reset);
    CHECK(api->decode == fake_decode && api->request_stream == fake_request_stream);
    CHECK(api->submit_stream == fake_submit_stream && api->stream_frames == fake_stream_frames);
    CHECK(api->request_picture == fake_request_picture && api->return_picture == fake_return_picture);
    CHECK(api->mem_ops == fake_mem_ops && api->memory_begin == fake_memory_begin);
    CHECK(api->memory_end == fake_memory_end && api->memory_status == fake_memory_status);
    CHECK(api->memory_describe == fake_memory_describe && api->memory_pin == fake_memory_pin);
    CHECK(api->memory_unpin == fake_memory_unpin);
}

static void success(void)
{
    const struct k2b_cedar_api *api = &sentinel, *again = NULL;
    struct k2b_cedar_runtime_status st;
    CHECK(k2b_cedar_runtime_status(&st) == 0 && st.ready == 0 && st.error == 0 && st.detail[0] == 0);
    CHECK(k2b_cedar_runtime_load(ROOT, &api) == 0);
    check_api(api);
    CHECK(k2b_cedar_runtime_load("/alias", &again) == 0 && again == api);
    CHECK(k2b_cedar_runtime_load(ROOT, &again) == 0 && again == api);
    CHECK(opens == 9 && registrations == 1);
    CHECK(k2b_cedar_runtime_status(&st) == 0 && st.ready == 1 && st.error == 0);
    again = &sentinel;
    CHECK(k2b_cedar_runtime_load("/other", &again) == -1 && errno == EXDEV && again == &sentinel);
    CHECK(k2b_cedar_runtime_status(&st) == 0 && st.ready == 1 && st.error == 0);
}

static void invalid(void)
{
    const struct k2b_cedar_api *api = &sentinel;
    struct k2b_cedar_runtime_status st;
    CHECK(k2b_cedar_runtime_load(NULL, &api) == -1 && errno == EINVAL && api == &sentinel);
    CHECK(k2b_cedar_runtime_load("", &api) == -1 && errno == EINVAL && api == &sentinel);
    CHECK(k2b_cedar_runtime_load("relative", &api) == -1 && errno == EINVAL && api == &sentinel);
    CHECK(k2b_cedar_runtime_load(ROOT, NULL) == -1 && errno == EINVAL);
    CHECK(k2b_cedar_runtime_status(NULL) == -1 && errno == EINVAL);
    CHECK(k2b_cedar_runtime_status(&st) == 0 && st.ready == 0 && st.error == 0);
    CHECK(opens == 0 && registrations == 0);
    success();
}

static void failure(void)
{
    const struct k2b_cedar_api *api = &sentinel;
    struct k2b_cedar_runtime_status first, again;
    int saved_errno, saved_opens, saved_maps, saved_providers;
    CHECK(k2b_cedar_runtime_load(ROOT, &api) == -1 && api == &sentinel);
    saved_errno = errno;
    CHECK(saved_errno != 0 && saved_errno != ENOSYS);
    CHECK(k2b_cedar_runtime_status(&first) == 0);
    CHECK(first.ready == 0 && first.error == saved_errno && first.detail[0]);
    CHECK(memchr(first.detail, 0, sizeof(first.detail)) != NULL);
    if ((fault >= ROOT_REALPATH && fault <= CONFIG_ERROR) || fault == PRELOAD_FOREIGN ||
        ((fault >= SYMBOL_MISSING && fault <= PROVIDER_REALPATH) && target == 19))
        CHECK(opens == 0);
    if (fault == OPEN_FAIL || fault == INFO_FAIL || fault == INFO_NULL || fault == INFO_WRONG)
        CHECK(opens == target + 1);
    if (fault >= FILE_REALPATH && fault <= FILE_TYPE) CHECK(strstr(first.detail, names[target]));
    if (fault == OPEN_FAIL || fault == INFO_FAIL || fault == INFO_NULL || fault == INFO_WRONG)
        CHECK(strstr(first.detail, names[target]));
    if (fault >= SYMBOL_MISSING && fault <= PROVIDER_REALPATH)
        CHECK(strstr(first.detail, symbols[target].name));
    if (fault == REGISTER_FAIL) CHECK(strstr(first.detail, "VDecoderRegister"));
    CHECK(registrations == (fault == REGISTER_FAIL ? 1 : 0));
    saved_opens = opens; saved_maps = maps_checks; saved_providers = provider_checks;
    fault = NONE;
    CHECK(k2b_cedar_runtime_load(ROOT, &api) == -1 && errno == saved_errno && api == &sentinel);
    CHECK(k2b_cedar_runtime_load("/other", &api) == -1 && errno == saved_errno && api == &sentinel);
    CHECK(opens == saved_opens && maps_checks == saved_maps && provider_checks == saved_providers);
    CHECK(k2b_cedar_runtime_status(&again) == 0);
    CHECK(first.ready == again.ready && first.error == again.error && !strcmp(first.detail, again.detail));
    CHECK(k2b_cedar_runtime_load(NULL, &api) == -1 && errno == EINVAL);
}

static pthread_barrier_t barrier;
static const struct k2b_cedar_api *thread_apis[16];
static void *load_thread(void *arg)
{
    size_t index = (size_t)arg;
    struct k2b_cedar_runtime_status st;
    int rc = pthread_barrier_wait(&barrier);
    CHECK(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
    CHECK(k2b_cedar_runtime_load(index % 2 ? ROOT : "/alias", &thread_apis[index]) == 0);
    CHECK(k2b_cedar_runtime_status(&st) == 0 && st.ready && !st.error);
    return NULL;
}
static void concurrent(void)
{
    pthread_t threads[16];
    size_t i;
    CHECK(pthread_barrier_init(&barrier, NULL, 16) == 0);
    for (i = 0; i < 16; ++i) CHECK(pthread_create(&threads[i], NULL, load_thread, (void *)i) == 0);
    for (i = 0; i < 16; ++i) CHECK(pthread_join(threads[i], NULL) == 0);
    for (i = 0; i < 16; ++i) CHECK(thread_apis[i] == thread_apis[0]);
    check_api(thread_apis[0]);
    CHECK(opens == 9 && registrations == 1);
    CHECK(pthread_barrier_destroy(&barrier) == 0);
}

static unsigned int total, failed;
static void run(const char *label, void (*test)(void), enum fault selected, int index)
{
    pid_t pid;
    int status;
    fflush(NULL);
    pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        int i;
        fault = selected; target = index;
        for (i = 0; i < 10; ++i) {
            snprintf(paths[i], sizeof(paths[i]), ROOT "/%s", names[i]);
            if (i < 9) maps[i].l_name = paths[i];
        }
        test();
        _exit(0);
    }
    CHECK(waitpid(pid, &status, 0) == pid);
    ++total;
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        ++failed;
        fprintf(stderr, "FAIL %s[%d] (status=%d)\n", label, index, status);
    }
}

int main(void)
{
    int i;
    run("success and one-time typed registration", success, NONE, 0);
    run("invalid arguments do not poison", invalid, NONE, 0);
    run("concurrent publication", concurrent, NONE, 0);
    for (i = ROOT_REALPATH; i <= ROOT_LONG; ++i) run("root gate", failure, (enum fault)i, 0);
    for (i = 0; i < 10; ++i) {
        run("file realpath", failure, FILE_REALPATH, i);
        run("outside symlink", failure, FILE_OUTSIDE, i);
        run("file stat", failure, FILE_STAT, i);
        run("file type", failure, FILE_TYPE, i);
    }
    for (i = 0; i < 4; ++i) {
        if (i != 2) run("missing environment", failure, ENV_MISSING, i);
        run("wrong environment", failure, ENV_WRONG, i);
    }
    run("absent audit accepted", success, ENV_MISSING, 2);
    run("audit rejected", failure, ENV_AUDIT, 2);
    for (i = CONFIG_FILE; i <= CONFIG_ERROR; ++i) run("config gate", failure, (enum fault)i, 0);
    for (i = 0; i < 9; ++i) {
        run("foreign already mapped", failure, PRELOAD_FOREIGN, i);
        run("canonical already mapped", success, PRELOAD_CANONICAL, i);
        run("final missing", failure, FINAL_MISSING, i);
        run("final duplicate", failure, FINAL_DUPLICATE, i);
        run("final foreign", failure, FINAL_FOREIGN, i);
        run("dlopen failure", failure, OPEN_FAIL, i);
        run("dlinfo failure", failure, INFO_FAIL, i);
        run("dlinfo null", failure, INFO_NULL, i);
        run("dlinfo wrong path", failure, INFO_WRONG, i);
    }
    for (i = 0; i < 20; ++i) {
        run("missing symbol", failure, SYMBOL_MISSING, i);
        run("dlerror despite nonnull symbol", failure, SYMBOL_DLERROR, i);
        run("wrong symbol provider", failure, PROVIDER_WRONG, i);
        run("dladdr failure", failure, PROVIDER_FAIL, i);
        run("provider realpath failure", failure, PROVIDER_REALPATH, i);
    }
    run("registration nonzero", failure, REGISTER_FAIL, 0);
    printf("cedar runtime: %u scenarios, %u failures\n", total, failed);
    return failed ? 1 : 0;
}
