/*
 * hwc2_shim.h - compile-time stand-in for <hardware/hwcomposer2.h>.
 *
 * The composer module is normally built against the platform header, which is
 * authoritative for both struct layout and the HWC2_PFN_* descriptor numbers.
 * This shim exists so `gcc -fsyntax-only` can validate the module on a machine
 * with no AOSP tree.  The descriptor values below follow the AOSP header as of
 * Android 13 (android-13.0.0_r2) but MUST NOT be trusted for a real build - the
 * module #errors if you try to enable QANDROID_ALLOW_SHIM_BUILD and ship it.
 */
#ifndef QANDROID_HWC2_SHIM_H
#define QANDROID_HWC2_SHIM_H

#include <stdint.h>
#include "qandroid_hw.h"

#ifdef __cplusplus
extern "C" {
#endif

struct hwc2_device;
typedef struct hwc2_device hwc2_device_t;
typedef uint64_t hwc2_display_t;
typedef uint64_t hwc2_layer_t;
typedef uint64_t hwc2_config_t;
typedef intptr_t hwc2_function_pointer_t;

struct hwc_rect { int32_t left, top, right, bottom; };
struct hwc_frect { float left, top, right, bottom; };
struct hwc_region { size_t numRects; struct hwc_rect *rects; };

#define HWC2_DEVICE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)

/* function descriptors (Android 13 values; AOSP calls these HWC2_FUNCTION_*) */
enum {
    HWC2_FUNCTION_CREATE_LAYER = 0,
    HWC2_FUNCTION_DESTROY_LAYER = 1,
    HWC2_FUNCTION_SET_LAYER_BUFFER = 2,
    HWC2_FUNCTION_SET_LAYER_SOURCE_CROP = 3,
    HWC2_FUNCTION_SET_LAYER_DISPLAY_FRAME = 4,
    HWC2_FUNCTION_SET_LAYER_COMPOSITION_TYPE = 5,
    HWC2_FUNCTION_SET_LAYER_BLEND_MODE = 6,
    HWC2_FUNCTION_SET_LAYER_PLANE_ALPHA = 7,
    HWC2_FUNCTION_SET_LAYER_TRANSFORM = 8,
    HWC2_FUNCTION_SET_LAYER_Z_ORDER = 9,
    HWC2_FUNCTION_SET_LAYER_VISIBLE_REGION = 11,
    HWC2_FUNCTION_SET_DISPLAY_LAYER_STACK = 18,
    HWC2_FUNCTION_VALIDATE_DISPLAY = 19,
    HWC2_FUNCTION_ACCEPT_DISPLAY_CHANGES = 20,
    HWC2_FUNCTION_PRESENT_DISPLAY = 22,
    HWC2_FUNCTION_GET_RELEASE_FENCES = 23,
    HWC2_FUNCTION_SET_DISPLAY_CLIENT_TARGET = 26,
    HWC2_FUNCTION_SET_ACTIVE_CONFIG = 29,
    HWC2_FUNCTION_GET_ACTIVE_CONFIG = 30,
    HWC2_FUNCTION_SET_DISPLAY_POWER_MODE = 31,
    HWC2_FUNCTION_GET_DISPLAY_ATTRIBUTE = 33,
    HWC2_FUNCTION_GET_DISPLAY_CONFIGS = 34,
};

/* capability bits */
enum { HWC2_CAPABILITY_SKIP_CLIENT_COLOR_TRANSFORM = 0, HWC2_CAPABILITY_PRESENT_FENCE_IS_NOT_RELIABLE };

/* composition types */
enum {
    HWC2_COMPOSITION_INVALID = 0,
    HWC2_COMPOSITION_CLIENT = 1,
    HWC2_COMPOSITION_DEVICE = 2,
    HWC2_COMPOSITION_SOLID_COLOR = 3,
    HWC2_COMPOSITION_CURSOR = 4,
    HWC2_COMPOSITION_SIDEBAND = 5,
};
enum { HWC2_BLEND_MODE_NONE = 0, HWC2_BLEND_MODE_PREMULTIPLIED = 1, HWC2_BLEND_MODE_COVERAGE = 2 };
enum {
    HWC2_ATTRIBUTE_INVALID = 0,
    HWC2_ATTRIBUTE_WIDTH = 1,
    HWC2_ATTRIBUTE_HEIGHT = 2,
    HWC2_ATTRIBUTE_VSYNC_PERIOD = 3,
    HWC2_ATTRIBUTE_DPI_X = 4,
    HWC2_ATTRIBUTE_DPI_Y = 5,
};
enum {
    HWC2_POWER_MODE_OFF = 0,
    HWC2_POWER_MODE_ON = 1,
    HWC2_POWER_MODE_DOZE = 2,
    HWC2_POWER_MODE_DOZE_SUSPEND = 3,
    HWC2_POWER_MODE_ON_SUSPEND = 4,
};
enum { HWC2_TRANSFORM_NORMAL = 0 };
enum {
    HWC2_ERROR_NONE = 0,
    HWC2_ERROR_BAD_CONFIG = 1,
    HWC2_ERROR_BAD_DISPLAY = 2,
    HWC2_ERROR_BAD_LAYER = 3,
    HWC2_ERROR_BAD_PARAMETER = 4,
    HWC2_ERROR_HAS_CHANGES = 5,
    HWC2_ERROR_NO_RESOURCES = 6,
    HWC2_ERROR_NOT_VALIDATED = 7,
    HWC2_ERROR_UNSUPPORTED = 8,
};

typedef int (*HWC2_PFN_CREATE_LAYER)(hwc2_device_t *device, hwc2_display_t display,
                                     hwc2_layer_t *out_layer);
typedef int (*HWC2_PFN_DESTROY_LAYER)(hwc2_device_t *device, hwc2_layer_t layer);
typedef int (*HWC2_PFN_SET_LAYER_BUFFER)(hwc2_device_t *device, hwc2_layer_t layer,
                                         buffer_handle_t buffer, int acquire_fence);
typedef int (*HWC2_PFN_SET_LAYER_SOURCE_CROP)(hwc2_device_t *device, hwc2_layer_t layer,
                                              const struct hwc_frect *crop);
typedef int (*HWC2_PFN_SET_LAYER_DISPLAY_FRAME)(hwc2_device_t *device, hwc2_layer_t layer,
                                                const struct hwc_rect *frame);
typedef int (*HWC2_PFN_SET_LAYER_COMPOSITION_TYPE)(hwc2_device_t *device, hwc2_layer_t layer,
                                                   int32_t type);
typedef int (*HWC2_PFN_SET_LAYER_BLEND_MODE)(hwc2_device_t *device, hwc2_layer_t layer,
                                             int32_t mode);
typedef int (*HWC2_PFN_SET_LAYER_PLANE_ALPHA)(hwc2_device_t *device, hwc2_layer_t layer,
                                              float alpha);
typedef int (*HWC2_PFN_SET_LAYER_TRANSFORM)(hwc2_device_t *device, hwc2_layer_t layer,
                                            int32_t transform);
typedef int (*HWC2_PFN_SET_LAYER_Z_ORDER)(hwc2_device_t *device, hwc2_layer_t layer, uint32_t z);
typedef int (*HWC2_PFN_SET_LAYER_VISIBLE_REGION)(hwc2_device_t *device, hwc2_layer_t layer,
                                                 const struct hwc_region *region);
typedef int (*HWC2_PFN_SET_DISPLAY_LAYER_STACK)(hwc2_device_t *device, hwc2_display_t display,
                                                uint32_t num_layers, const hwc2_layer_t *layers);
typedef int (*HWC2_PFN_VALIDATE_DISPLAY)(hwc2_device_t *device, hwc2_display_t display,
                                         uint32_t *out_num_types, uint32_t *out_num_requests);
typedef int (*HWC2_PFN_ACCEPT_DISPLAY_CHANGES)(hwc2_device_t *device, hwc2_display_t display);
typedef int (*HWC2_PFN_PRESENT_DISPLAY)(hwc2_device_t *device, hwc2_display_t display,
                                        int *out_present_fence);
typedef int (*HWC2_PFN_GET_RELEASE_FENCES)(hwc2_device_t *device, hwc2_display_t display,
                                           uint32_t *out_num_elements, int *out_fences,
                                           hwc2_layer_t *out_layers);
typedef int (*HWC2_PFN_SET_DISPLAY_CLIENT_TARGET)(hwc2_device_t *device, hwc2_display_t display,
                                                  buffer_handle_t target, int acquire_fence);
typedef int (*HWC2_PFN_SET_ACTIVE_CONFIG)(hwc2_device_t *device, hwc2_display_t display,
                                          hwc2_config_t config);
typedef hwc2_config_t (*HWC2_PFN_GET_ACTIVE_CONFIG)(hwc2_device_t *device, hwc2_display_t display);
typedef int (*HWC2_PFN_SET_DISPLAY_POWER_MODE)(hwc2_device_t *device, hwc2_display_t display,
                                               int32_t mode);
typedef int (*HWC2_PFN_GET_DISPLAY_ATTRIBUTE)(hwc2_device_t *device, hwc2_display_t display,
                                              hwc2_config_t config, int32_t attribute,
                                              int32_t *out_value);
typedef int (*HWC2_PFN_GET_DISPLAY_CONFIGS)(hwc2_device_t *device, hwc2_display_t display,
                                            uint32_t *out_num_configs, hwc2_config_t *out_configs);

/* callbacks SurfaceFlinger installs before any layer work happens */
typedef int (*HWC2_PFN_HOTPLUG)(hwc2_display_t display, int32_t connected);
typedef int (*HWC2_PFN_REFRESH)(hwc2_display_t display);
typedef int (*HWC2_PFN_VSYNC)(hwc2_display_t display, int64_t timestamp,
                              int32_t /*hwc2_vsync_period_t*/ period);
typedef int (*HWC2_PFN_VSYNC_2_4)(hwc2_display_t display, int64_t timestamp,
                                  int32_t /*hwc2_vsync_period_t*/ period,
                                  uint32_t /*hwc2_vsync_t*/ vsync_id);

struct hwc2_device {
    struct hw_device_t common;
    void (*getCapabilities)(struct hwc2_device *device, uint32_t *out_count,
                            int32_t *out_capabilities);
    int32_t (*getFunction)(struct hwc2_device *device, int32_t descriptor,
                           int32_t *out_version, hwc2_function_pointer_t *out_function);
};

#ifdef __cplusplus
}
#endif
#endif /* QANDROID_HWC2_SHIM_H */
