/*
 * abshim - the bionic surface, for arm64 NDK code that has to run outside a
 * full Android userspace.
 *
 * Why this exists: with the full guest rootfs (path A in the README) nothing
 * here is needed - bionic is the real thing.  This library is for path B, when
 * you extract an arm64 ELF from an APK and want to execute it against the
 * host's glibc/musl runtime (e.g. after patchelf --set-interpreter).
 *
 * What it provides:
 *   - the android/log.h entry points
 *   - property_get / __system_property_get backed by /system/build.prop, then
 *     the process environment, then a small built-in table
 *   - __errno(), which bionic exports as a function rather than a macro
 *   - a handful of NDK-only helpers that have no libc equivalent
 *
 * What it cannot provide (and that is why path A exists):
 *   - bionic's __libc_init / constructor ordering, AT_SECURE handling
 *   - bionic stdio's `__sF` symbol table
 *   - the tls-model differences of bionic's pthread impl
 * A binary that needs those must run inside the guest rootfs.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- logging ------------------------------------------------------------ */
#define LOG_TAG_CONST 0 /* not used; tag comes from the call site */

static int log_prio_from_env(void)
{
    const char *v = getenv("ANDROID_LOG_PRIORITY");
    return v ? atoi(v) : 4; /* ANDROID_LOG_INFO */
}

static const char *prio_name(int p)
{
    static const char *n[] = {"?", "?", "V", "D", "I", "W", "E", "F", "S"};
    return (p >= 2 && p <= 8) ? n[p] : "?";
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    char buf[4096];
    if (prio < log_prio_from_env())
        return 0;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s/%-8s: %s\n", prio_name(prio), tag ? tag : "-", buf);
    return n;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    char buf[4096];
    if (prio < log_prio_from_env())
        return 0;
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    fprintf(stderr, "%s/%-8s: %s\n", prio_name(prio), tag ? tag : "-", buf);
    return n;
}

int __android_log_write(int prio, const char *tag, const char *text)
{
    return __android_log_print(prio, tag, "%s", text);
}

int __android_log_buf_print(int id, int prio, const char *tag, const char *fmt, ...)
{
    (void)id;
    va_list ap;
    va_start(ap, fmt);
    int n = __android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return n;
}

void __android_log_assert(const char *cond, const char *tag, const char *fmt, ...)
{
    va_list ap;
    char buf[2048];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt ? fmt : "%s", ap);
    va_end(ap);
    fprintf(stderr, "FATAL/%s: %s (assertion: %s)\n", tag ? tag : "-", buf,
            cond ? cond : "?");
    abort();
}

void android_set_abort_message(const char *msg)
{
    fprintf(stderr, "abort message: %s\n", msg ? msg : "(null)");
}

/* ---- properties --------------------------------------------------------- */
static const struct {
    const char *k, *v;
} builtin_props[] = {
    {"ro.build.version.sdk", "34"},
    {"ro.build.version.release", "14"},
    {"ro.product.cpu.abi", "arm64-v8a"},
    {"ro.product.cpu.abilist", "arm64-v8a"},
    {"ro.debuggable", "1"},
    {"ro.kernel.qemu", "1"},
    {NULL, NULL},
};

static int read_build_prop(const char *key, char *out, size_t n)
{
    static char cache[64 * 1024];
    static size_t cached;
    if (!cached) {
        FILE *f = fopen("/system/build.prop", "r");
        if (f) {
            cached = fread(cache, 1, sizeof cache - 1, f);
            cache[cached] = 0;
            fclose(f);
        }
    }
    if (!cached)
        return 0;
    size_t klen = strlen(key);
    char *p = cache;
    while (p && *p) {
        char *eol = strchr(p, '\n');
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            const char *val = p + klen + 1;
            size_t vlen = eol ? (size_t)(eol - val) : strlen(val);
            if (vlen >= n)
                vlen = n - 1;
            memcpy(out, val, vlen);
            out[vlen] = 0;
            return 1;
        }
        p = eol ? eol + 1 : NULL;
    }
    return 0;
}

static char prop_buf[128];

char *__system_property_get(const char *key, char *value)
{
    if (!key || !value)
        return NULL;
    const char *env;
    if (read_build_prop(key, value, 92 /* PROP_VALUE_MAX */))
        return value;
    env = getenv(key);
    if (env) {
        strncpy(value, env, 91);
        value[91] = 0;
        return value;
    }
    for (int i = 0; builtin_props[i].k; i++) {
        if (!strcmp(builtin_props[i].k, key)) {
            strncpy(value, builtin_props[i].v, 91);
            value[91] = 0;
            return value;
        }
    }
    value[0] = 0;
    return value;
}

int property_get(const char *key, char *value, const char *default_value)
{
    if (!__system_property_get(key, value)[0] && default_value) {
        strncpy(value, default_value, 91);
        value[91] = 0;
    }
    return (int)strlen(value);
}

int property_set(const char *key, const char *value)
{
    /* no property service outside the guest; export so children can see it */
    return setenv(key, value ? value : "", 1);
}

const char *__system_property_find(const char *name)
{
    (void)__system_property_get(name, prop_buf);
    return prop_buf[0] ? prop_buf : NULL;
}

int __system_property_read(const void *pi, char *name, char *value)
{
    (void)pi;
    if (name)
        name[0] = 0;
    return __system_property_get("", value) ? 0 : 0;
}

int android_get_device_api_level(void)
{
    char v[92];
    __system_property_get("ro.build.version.sdk", v);
    return atoi(v);
}

/* ---- misc --------------------------------------------------------------- */
int *__errno(void)
{
    return __errno_location();
}

const char *android_get_abort_message(void)
{
    return NULL;
}

void __libc_android_log_set_logger(void *f)
{
    (void)f;
}

int __cxa_thread_atexit_impl(void (*dtor)(void *), void *obj, void *dso)
{
    (void)dtor;
    (void)obj;
    (void)dso;
    return 0;
}
