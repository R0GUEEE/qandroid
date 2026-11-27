/*
 * qandroid_hw.h - subset of <hardware/hardware.h> + <hardware/gralloc.h>.
 *
 * Only used when building *outside* an AOSP tree, so the composer module can be
 * compile-checked anywhere.  Inside a tree, the platform headers are used
 * instead (see the include switch in hwcomposer_qandroid.c) - the platform ones
 * are authoritative for struct layout and version numbers.
 */
#ifndef QANDROID_HW_H
#define QANDROID_HW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hw_device_t;
struct android_ycbcr;

/* ---- <hardware/hardware.h> --------------------------------------------- */
typedef struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    struct hw_module_methods_t *methods;
    void *dso;
    uint32_t reserved[32 - 7];
} hw_module_t;

typedef struct hw_module_methods_t {
    int (*open)(const struct hw_module_t *module, const char *id,
                struct hw_device_t **device);
} hw_module_methods_t;

typedef struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct hw_module_t *module;
    uint32_t reserved[12];
    int (*close)(struct hw_device_t *device);
} hw_device_t;

#define HARDWARE_MODULE_TAG 0x48574d44u
#define HARDWARE_DEVICE_TAG 0x48574456u
#define HARDWARE_MAKE_API_VERSION(maj, min) ((((maj) & 0xff) << 8) | ((min) & 0xff))

/* the module entry point of every hw module is the symbol HMI */
#define HAL_MODULE_INFO_SYM HMI

#define HWC_HARDWARE_MODULE_ID "hwcomposer"
#define HWC_HARDWARE_COMPOSER "composer"
#define GRALLOC_HARDWARE_MODULE_ID "gralloc"
#define GRALLOC_HARDWARE_GPU0 "gpu0"

/* ---- <cutils/native_handle.h> ------------------------------------------ */
typedef const struct native_handle *buffer_handle_t;

typedef struct native_handle {
    int version;
    int numFds;
    int numInts;
    int data[0];
} native_handle_t;

/* ---- <hardware/gralloc.h> (gralloc0) ----------------------------------- */
enum {
    GRALLOC_USAGE_SW_READ_OFTEN = 0x00000003,
    GRALLOC_USAGE_SW_WRITE_OFTEN = 0x00000030,
    GRALLOC_USAGE_HW_TEXTURE = 0x00000100,
    GRALLOC_USAGE_HW_RENDER = 0x00000200,
};

typedef struct gralloc_module_t {
    struct hw_module_t common;
    int (*registerBuffer)(struct gralloc_module_t const *module, buffer_handle_t handle);
    int (*unregisterBuffer)(struct gralloc_module_t const *module, buffer_handle_t handle);
    int (*lock)(struct gralloc_module_t const *module, buffer_handle_t handle, int usage,
                int l, int t, int w, int h, void **vaddr);
    int (*unlock)(struct gralloc_module_t const *module, buffer_handle_t handle);
    int (*perform)(struct gralloc_module_t const *module, int op, ...);
    int (*lock_ycbcr)(struct gralloc_module_t const *module, buffer_handle_t handle, int usage,
                      int l, int t, int w, int h, struct android_ycbcr *ycbcr);
    void *reserved_proc[6];
} gralloc_module_t;

int hw_get_module(const char *id, const struct hw_module_t **module);
#ifdef __cplusplus
}
#endif
#endif
