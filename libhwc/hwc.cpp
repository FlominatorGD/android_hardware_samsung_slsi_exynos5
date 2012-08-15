/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <s3c-fb.h>

#include <EGL/egl.h>

#define HWC_REMOVE_DEPRECATED_VERSIONS 1

#include <cutils/log.h>
#include <hardware/gralloc.h>
#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>
#include <hardware_legacy/uevent.h>
#include <utils/Vector.h>

#include <sync/sync.h>

#include "ion.h"
#include "gralloc_priv.h"
#include "exynos_gscaler.h"
#include "exynos_format.h"

struct hwc_callback_entry {
    void (*callback)(void *, private_handle_t *);
    void *data;
};
typedef android::Vector<struct hwc_callback_entry> hwc_callback_queue_t;

const size_t NUM_HW_WINDOWS = 5;
const size_t NO_FB_NEEDED = NUM_HW_WINDOWS + 1;
const size_t MAX_PIXELS = 2560 * 1600 * 2;
const size_t NUM_GSC_UNITS = 3;
const size_t GSC_W_ALIGNMENT = 16;
const size_t GSC_H_ALIGNMENT = 16;
const int CAMERA_GSC_IDX = 2;

struct exynos5_hwc_composer_device_1_t;

struct exynos5_gsc_map_t {
    enum {
        GSC_NONE = 0,
        GSC_M2M,
        // TODO: GSC_LOCAL_PATH
    } mode;
    int idx;
};

struct exynos5_hwc_post_data_t {
    exynos5_hwc_composer_device_1_t *pdev;
    int                             overlay_map[NUM_HW_WINDOWS];
    exynos5_gsc_map_t               gsc_map[NUM_HW_WINDOWS];
    hwc_layer_1_t                   overlays[NUM_HW_WINDOWS];
    int                             num_overlays;
    size_t                          fb_window;
    int                             fence;
    pthread_mutex_t                 completion_lock;
    pthread_cond_t                  completion;
};

const size_t NUM_GSC_DST_BUFS = 2;
struct exynos5_gsc_data_t {
    void            *gsc;
    exynos_gsc_img  src_cfg;
    exynos_gsc_img  dst_cfg;
    buffer_handle_t dst_buf[NUM_GSC_DST_BUFS];
    size_t          current_buf;
};

struct exynos5_hwc_composer_device_1_t {
    hwc_composer_device_1_t base;

    int                     fd;
    int                     vsync_fd;
    exynos5_hwc_post_data_t bufs;

    const private_module_t  *gralloc_module;
    alloc_device_t          *alloc_device;
    hwc_procs_t             *procs;
    pthread_t               vsync_thread;

    bool hdmi_hpd;
    bool hdmi_mirroring;
    void *hdmi_gsc;

    exynos5_gsc_data_t      gsc[NUM_GSC_UNITS];
};

static void dump_handle(private_handle_t *h)
{
    ALOGV("\t\tformat = %d, width = %u, height = %u, stride = %u",
            h->format, h->width, h->height, h->stride);
}

static void dump_layer(hwc_layer_1_t const *l)
{
    ALOGV("\ttype=%d, flags=%08x, handle=%p, tr=%02x, blend=%04x, "
            "{%d,%d,%d,%d}, {%d,%d,%d,%d}",
            l->compositionType, l->flags, l->handle, l->transform,
            l->blending,
            l->sourceCrop.left,
            l->sourceCrop.top,
            l->sourceCrop.right,
            l->sourceCrop.bottom,
            l->displayFrame.left,
            l->displayFrame.top,
            l->displayFrame.right,
            l->displayFrame.bottom);

    if(l->handle && !(l->flags & HWC_SKIP_LAYER))
        dump_handle(private_handle_t::dynamicCast(l->handle));
}

static void dump_config(s3c_fb_win_config &c)
{
    ALOGV("\tstate = %u", c.state);
    if (c.state == c.S3C_FB_WIN_STATE_BUFFER) {
        ALOGV("\t\tfd = %d, offset = %u, stride = %u, "
                "x = %d, y = %d, w = %u, h = %u, "
                "format = %u",
                c.fd, c.offset, c.stride,
                c.x, c.y, c.w, c.h,
                c.format);
    }
    else if (c.state == c.S3C_FB_WIN_STATE_COLOR) {
        ALOGV("\t\tcolor = %u", c.color);
    }
}

static void dump_gsc_img(exynos_gsc_img &c)
{
    ALOGV("\tx = %u, y = %u, w = %u, h = %u, fw = %u, fh = %u",
            c.x, c.y, c.w, c.h, c.fw, c.fh);
    ALOGV("\taddr = {%u, %u, %u}, rot = %u, cacheable = %u, drmMode = %u",
            c.yaddr, c.uaddr, c.vaddr, c.rot, c.cacheable, c.drmMode);
}

inline int WIDTH(const hwc_rect &rect) { return rect.right - rect.left; }
inline int HEIGHT(const hwc_rect &rect) { return rect.bottom - rect.top; }
template<typename T> inline T max(T a, T b) { return (a > b) ? a : b; }
template<typename T> inline T min(T a, T b) { return (a < b) ? a : b; }

static bool is_transformed(const hwc_layer_1_t &layer)
{
    return layer.transform != 0;
}

static bool is_rotated(const hwc_layer_1_t &layer)
{
    return (layer.transform & HAL_TRANSFORM_ROT_90) ||
            (layer.transform & HAL_TRANSFORM_ROT_180);
}

static bool is_scaled(const hwc_layer_1_t &layer)
{
    return WIDTH(layer.displayFrame) != WIDTH(layer.sourceCrop) ||
            HEIGHT(layer.displayFrame) != HEIGHT(layer.sourceCrop);
}

static enum s3c_fb_pixel_format exynos5_format_to_s3c_format(int format)
{
    switch (format) {
    case HAL_PIXEL_FORMAT_RGBA_8888:
        return S3C_FB_PIXEL_FORMAT_RGBA_8888;
    case HAL_PIXEL_FORMAT_RGBX_8888:
        return S3C_FB_PIXEL_FORMAT_RGBX_8888;
    case HAL_PIXEL_FORMAT_RGBA_5551:
        return S3C_FB_PIXEL_FORMAT_RGBA_5551;
    case HAL_PIXEL_FORMAT_RGBA_4444:
        return S3C_FB_PIXEL_FORMAT_RGBA_4444;

    default:
        return S3C_FB_PIXEL_FORMAT_MAX;
    }
}

static bool exynos5_format_is_supported(int format)
{
    return exynos5_format_to_s3c_format(format) < S3C_FB_PIXEL_FORMAT_MAX;
}

static bool exynos5_format_is_supported_by_gscaler(int format)
{
    switch (format) {
    case HAL_PIXEL_FORMAT_RGBX_8888:
    case HAL_PIXEL_FORMAT_RGB_565:
    case HAL_PIXEL_FORMAT_YV12:
    case HAL_PIXEL_FORMAT_YCbCr_420_P:
    case HAL_PIXEL_FORMAT_YCbCr_422_SP:
    case HAL_PIXEL_FORMAT_CUSTOM_YCbCr_422_SP:
    case HAL_PIXEL_FORMAT_YCbCr_420_SP:
    case HAL_PIXEL_FORMAT_CUSTOM_YCbCr_420_SP:
    case HAL_PIXEL_FORMAT_YCbCr_422_I:
    case HAL_PIXEL_FORMAT_CUSTOM_YCbCr_422_I:
    case HAL_PIXEL_FORMAT_YCbCr_422_P:
    case HAL_PIXEL_FORMAT_CbYCrY_422_I:
    case HAL_PIXEL_FORMAT_CUSTOM_CbYCrY_422_I:
    case HAL_PIXEL_FORMAT_YCrCb_422_SP:
    case HAL_PIXEL_FORMAT_CUSTOM_YCrCb_422_SP:
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:
    case HAL_PIXEL_FORMAT_CUSTOM_YCrCb_420_SP:
    case HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED:
    case HAL_PIXEL_FORMAT_CUSTOM_YCbCr_420_SP_TILED:
    case HAL_PIXEL_FORMAT_CUSTOM_YCrCb_422_I:
    case HAL_PIXEL_FORMAT_CUSTOM_CrYCbY_422_I:
        return true;

    default:
        return false;
    }
}

static bool exynos5_format_is_ycrcb(int format)
{
    return format == HAL_PIXEL_FORMAT_YV12;
}

static bool exynos5_format_requires_gscaler(int format)
{
    return exynos5_format_is_supported_by_gscaler(format) &&
            format != HAL_PIXEL_FORMAT_RGBX_8888;
}

static uint8_t exynos5_format_to_bpp(int format)
{
    switch (format) {
    case HAL_PIXEL_FORMAT_RGBA_8888:
    case HAL_PIXEL_FORMAT_RGBX_8888:
        return 32;

    case HAL_PIXEL_FORMAT_RGBA_5551:
    case HAL_PIXEL_FORMAT_RGBA_4444:
        return 16;

    default:
        ALOGW("unrecognized pixel format %u", format);
        return 0;
    }
}

static bool exynos5_supports_gscaler(hwc_layer_1_t &layer, int format,
        bool local_path)
{
    private_handle_t *handle = private_handle_t::dynamicCast(layer.handle);

    int max_w = is_rotated(layer) ? 2048 : 4800;
    int max_h = is_rotated(layer) ? 2048 : 3344;

    bool rot90or270 = !!(layer.transform & HAL_TRANSFORM_ROT_90);
    // n.b.: HAL_TRANSFORM_ROT_270 = HAL_TRANSFORM_ROT_90 |
    //                               HAL_TRANSFORM_ROT_180

    int src_w = WIDTH(layer.sourceCrop), src_h = HEIGHT(layer.sourceCrop);
    int dest_w, dest_h;
    if (rot90or270) {
        dest_w = HEIGHT(layer.displayFrame);
        dest_h = WIDTH(layer.displayFrame);
    } else {
        dest_w = WIDTH(layer.displayFrame);
        dest_h = HEIGHT(layer.displayFrame);
    }
    int max_downscale = local_path ? 4 : 16;
    const int max_upscale = 8;

    return exynos5_format_is_supported_by_gscaler(format) &&
            handle->stride <= max_w &&
            handle->stride % GSC_W_ALIGNMENT == 0 &&
            src_w <= dest_w * max_downscale &&
            dest_w <= src_w * max_upscale &&
            handle->height <= max_h &&
            handle->height % GSC_H_ALIGNMENT == 0 &&
            src_h <= dest_h * max_downscale &&
            dest_h <= src_h * max_upscale &&
            // per 46.2
            (!rot90or270 || layer.sourceCrop.top % 2 == 0) &&
            (!rot90or270 || layer.sourceCrop.left % 2 == 0);
            // per 46.3.1.6
}

static int hdmi_enable(struct exynos5_hwc_composer_device_1_t *dev)
{
    if (dev->hdmi_mirroring)
        return 0;

    exynos_gsc_img src_info;
    exynos_gsc_img dst_info;

    // TODO: Don't hardcode
    int src_w = 2560;
    int src_h = 1600;
    int dst_w = 1920;
    int dst_h = 1080;

    dev->hdmi_gsc = exynos_gsc_create_exclusive(3, GSC_OUTPUT_MODE, GSC_OUT_TV);
    if (!dev->hdmi_gsc) {
        ALOGE("%s: exynos_gsc_create_exclusive failed", __func__);
        return -ENODEV;
    }

    memset(&src_info, 0, sizeof(src_info));
    memset(&dst_info, 0, sizeof(dst_info));

    src_info.w = src_w;
    src_info.h = src_h;
    src_info.fw = src_w;
    src_info.fh = src_h;
    src_info.format = HAL_PIXEL_FORMAT_BGRA_8888;

    dst_info.w = dst_w;
    dst_info.h = dst_h;
    dst_info.fw = dst_w;
    dst_info.fh = dst_h;
    dst_info.format = HAL_PIXEL_FORMAT_YV12;

    int ret = exynos_gsc_config_exclusive(dev->hdmi_gsc, &src_info, &dst_info);
    if (ret < 0) {
        ALOGE("%s: exynos_gsc_config_exclusive failed %d", __func__, ret);
        exynos_gsc_destroy(dev->hdmi_gsc);
        dev->hdmi_gsc = NULL;
        return ret;
    }

    dev->hdmi_mirroring = true;
    return 0;
}

static void hdmi_disable(struct exynos5_hwc_composer_device_1_t *dev)
{
    if (!dev->hdmi_mirroring)
        return;
    exynos_gsc_destroy(dev->hdmi_gsc);
    dev->hdmi_gsc = NULL;
    dev->hdmi_mirroring = false;
}

static int hdmi_output(struct exynos5_hwc_composer_device_1_t *dev, private_handle_t *fb)
{
    exynos_gsc_img src_info;
    exynos_gsc_img dst_info;

    memset(&src_info, 0, sizeof(src_info));
    memset(&dst_info, 0, sizeof(dst_info));

    src_info.yaddr = fb->fd;

    int ret = exynos_gsc_run_exclusive(dev->hdmi_gsc, &src_info, &dst_info);
    if (ret < 0) {
        ALOGE("%s: exynos_gsc_run_exclusive failed %d", __func__, ret);
        return ret;
    }

    return 0;
}

bool exynos5_supports_overlay(hwc_layer_1_t &layer, size_t i)
{
    if (layer.flags & HWC_SKIP_LAYER) {
        ALOGV("\tlayer %u: skipping", i);
        return false;
    }

    private_handle_t *handle = private_handle_t::dynamicCast(layer.handle);

    if (!handle) {
        ALOGV("\tlayer %u: handle is NULL", i);
        return false;
    }
    if (exynos5_format_requires_gscaler(handle->format)) {
        if (!exynos5_supports_gscaler(layer, handle->format, false)) {
            ALOGV("\tlayer %u: gscaler required but not supported", i);
            return false;
        }
    } else {
        if (!exynos5_format_is_supported(handle->format)) {
            ALOGV("\tlayer %u: pixel format %u not supported", i, handle->format);
            return false;
        }
        if (is_scaled(layer)) {
            ALOGV("\tlayer %u: scaling not supported", i);
            return false;
        }
        if (is_transformed(layer)) {
            ALOGV("\tlayer %u: transformations not supported", i);
            return false;
        }
    }
    if (layer.blending != HWC_BLENDING_NONE) {
        // TODO: support this
        ALOGV("\tlayer %u: blending not supported", i);
        return false;
    }

    return true;
}

inline bool intersect(const hwc_rect &r1, const hwc_rect &r2)
{
    return !(r1.left > r2.right ||
        r1.right < r2.left ||
        r1.top > r2.bottom ||
        r1.bottom < r2.top);
}

inline hwc_rect intersection(const hwc_rect &r1, const hwc_rect &r2)
{
    hwc_rect i;
    i.top = max(r1.top, r2.top);
    i.bottom = min(r1.bottom, r2.bottom);
    i.left = max(r1.left, r2.left);
    i.right = min(r1.right, r2.right);
    return i;
}

static int exynos5_prepare(hwc_composer_device_1_t *dev,
        size_t numDisplays, hwc_display_contents_1_t** displays)
{
    if (!numDisplays || !displays)
        return 0;

    ALOGV("preparing %u layers", displays[0]->numHwLayers);

    exynos5_hwc_composer_device_1_t *pdev =
            (exynos5_hwc_composer_device_1_t *)dev;
    memset(pdev->bufs.overlays, 0, sizeof(pdev->bufs.overlays));
    memset(pdev->bufs.gsc_map, 0, sizeof(pdev->bufs.gsc_map));

    bool force_fb = false;
    if (pdev->hdmi_hpd) {
        hdmi_enable(pdev);
        force_fb = true;
    } else {
        hdmi_disable(pdev);
    }

    for (size_t i = 0; i < NUM_HW_WINDOWS; i++)
        pdev->bufs.overlay_map[i] = -1;

    bool fb_needed = false;
    size_t first_fb = 0, last_fb = 0;

    // find unsupported overlays
    for (size_t i = 0; i < displays[0]->numHwLayers; i++) {
        hwc_layer_1_t &layer = displays[0]->hwLayers[i];

        if (layer.compositionType == HWC_BACKGROUND && !force_fb) {
            ALOGV("\tlayer %u: background supported", i);
            dump_layer(&displays[0]->hwLayers[i]);
            continue;
        }

        if (exynos5_supports_overlay(displays[0]->hwLayers[i], i) && !force_fb) {
            ALOGV("\tlayer %u: overlay supported", i);
            layer.compositionType = HWC_OVERLAY;
            dump_layer(&displays[0]->hwLayers[i]);
            continue;
        }

        if (!fb_needed) {
            first_fb = i;
            fb_needed = true;
        }
        last_fb = i;
        layer.compositionType = HWC_FRAMEBUFFER;

        dump_layer(&displays[0]->hwLayers[i]);
    }

    // can't composite overlays sandwiched between framebuffers
    if (fb_needed)
        for (size_t i = first_fb; i < last_fb; i++)
            displays[0]->hwLayers[i].compositionType = HWC_FRAMEBUFFER;

    // Incrementally try to add our supported layers to hardware windows.
    // If adding a layer would violate a hardware constraint, force it
    // into the framebuffer and try again.  (Revisiting the entire list is
    // necessary because adding a layer to the framebuffer can cause other
    // windows to retroactively violate constraints.)
    bool changed;
    do {
        android::Vector<hwc_rect> rects;
        android::Vector<hwc_rect> overlaps;
        size_t pixels_left, windows_left, gsc_left = NUM_GSC_UNITS;

        if (fb_needed) {
            hwc_rect_t fb_rect;
            fb_rect.top = fb_rect.left = 0;
            fb_rect.right = pdev->gralloc_module->xres - 1;
            fb_rect.bottom = pdev->gralloc_module->yres - 1;
            pixels_left = MAX_PIXELS - pdev->gralloc_module->xres *
                    pdev->gralloc_module->yres;
            windows_left = NUM_HW_WINDOWS - 1;
            rects.push_back(fb_rect);
        }
        else {
            pixels_left = MAX_PIXELS;
            windows_left = NUM_HW_WINDOWS;
        }
        if (pdev->hdmi_mirroring)
            gsc_left--;

        changed = false;

        for (size_t i = 0; i < displays[0]->numHwLayers; i++) {
            hwc_layer_1_t &layer = displays[0]->hwLayers[i];
            if (layer.flags & HWC_SKIP_LAYER)
                continue;

            private_handle_t *handle = private_handle_t::dynamicCast(
                    layer.handle);

            // we've already accounted for the framebuffer above
            if (layer.compositionType == HWC_FRAMEBUFFER)
                continue;

            // only layer 0 can be HWC_BACKGROUND, so we can
            // unconditionally allow it without extra checks
            if (layer.compositionType == HWC_BACKGROUND) {
                windows_left--;
                continue;
            }

            size_t pixels_needed = WIDTH(layer.displayFrame) *
                    HEIGHT(layer.displayFrame);
            bool can_compose = windows_left && pixels_needed <= pixels_left;
            bool gsc_required = exynos5_format_requires_gscaler(handle->format);
            if (gsc_required)
                can_compose = can_compose && gsc_left;

            // hwc_rect_t right and bottom values are normally exclusive;
            // the intersection logic is simpler if we make them inclusive
            hwc_rect_t visible_rect = layer.displayFrame;
            visible_rect.right--; visible_rect.bottom--;

            // no more than 2 layers can overlap on a given pixel
            for (size_t j = 0; can_compose && j < overlaps.size(); j++) {
                if (intersect(visible_rect, overlaps.itemAt(j)))
                    can_compose = false;
            }

            if (!can_compose) {
                layer.compositionType = HWC_FRAMEBUFFER;
                if (!fb_needed) {
                    first_fb = last_fb = i;
                    fb_needed = true;
                }
                else {
                    first_fb = min(i, first_fb);
                    last_fb = max(i, last_fb);
                }
                changed = true;
                break;
            }

            for (size_t j = 0; j < rects.size(); j++) {
                const hwc_rect_t &other_rect = rects.itemAt(j);
                if (intersect(visible_rect, other_rect))
                    overlaps.push_back(intersection(visible_rect, other_rect));
            }
            rects.push_back(visible_rect);
            pixels_left -= pixels_needed;
            windows_left--;
            if (gsc_required)
                gsc_left--;
        }

        if (changed)
            for (size_t i = first_fb; i < last_fb; i++)
                displays[0]->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
    } while(changed);

    unsigned int nextWindow = 0;
    int nextGsc = 0;

    for (size_t i = 0; i < displays[0]->numHwLayers; i++) {
        hwc_layer_1_t &layer = displays[0]->hwLayers[i];
        if (layer.flags & HWC_SKIP_LAYER)
            continue;

        if (fb_needed && i == first_fb) {
            ALOGV("assigning framebuffer to window %u\n",
                    nextWindow);
            nextWindow++;
            continue;
        }

        if (layer.compositionType != HWC_FRAMEBUFFER) {
            ALOGV("assigning layer %u to window %u", i, nextWindow);
            pdev->bufs.overlay_map[nextWindow] = i;
            if (layer.compositionType == HWC_OVERLAY) {
                private_handle_t *handle =
                        private_handle_t::dynamicCast(layer.handle);
                if (exynos5_format_requires_gscaler(handle->format)) {
                    ALOGV("\tusing gscaler %u", nextGsc);
                    pdev->bufs.gsc_map[i].mode =
                            exynos5_gsc_map_t::GSC_M2M;
                    pdev->bufs.gsc_map[i].idx = nextGsc++;
                    if (nextGsc == CAMERA_GSC_IDX)
                        nextGsc++;
                }
            }
            nextWindow++;
        }
    }

    for (size_t i = nextGsc; i < NUM_GSC_UNITS; i++) {
        for (size_t j = 0; j < NUM_GSC_DST_BUFS; j++)
            if (pdev->gsc[i].dst_buf[j])
                pdev->alloc_device->free(pdev->alloc_device,
                        pdev->gsc[i].dst_buf[j]);
        memset(&pdev->gsc[i], 0, sizeof(pdev->gsc[i]));
    }

    if (fb_needed)
        pdev->bufs.fb_window = first_fb;
    else
        pdev->bufs.fb_window = NO_FB_NEEDED;

    return 0;
}

static inline bool gsc_dst_cfg_changed(exynos_gsc_img &c1, exynos_gsc_img &c2)
{
    return c1.x != c2.x ||
            c1.y != c2.y ||
            c1.w != c2.w ||
            c1.h != c2.h ||
            c1.format != c2.format ||
            c1.rot != c2.rot ||
            c1.cacheable != c2.cacheable ||
            c1.drmMode != c2.drmMode;
}

static inline bool gsc_src_cfg_changed(exynos_gsc_img &c1, exynos_gsc_img &c2)
{
    return gsc_dst_cfg_changed(c1, c2) ||
            c1.fw != c2.fw ||
            c1.fh != c2.fh;
}

static int exynos5_config_gsc_m2m(hwc_layer_1_t &layer,
        alloc_device_t* alloc_device, exynos5_gsc_data_t *gsc_data,
        int gsc_idx)
{
    ALOGV("configuring gscaler %u for memory-to-memory", gsc_idx);

    private_handle_t *src_handle = private_handle_t::dynamicCast(layer.handle);
    buffer_handle_t dst_buf;
    private_handle_t *dst_handle;
    int ret = 0;

    exynos_gsc_img src_cfg, dst_cfg;
    memset(&src_cfg, 0, sizeof(src_cfg));
    memset(&dst_cfg, 0, sizeof(dst_cfg));

    src_cfg.x = layer.sourceCrop.left;
    src_cfg.y = layer.sourceCrop.top;
    src_cfg.w = WIDTH(layer.sourceCrop);
    src_cfg.fw = src_handle->stride;
    src_cfg.h = HEIGHT(layer.sourceCrop);
    src_cfg.fh = src_handle->height;
    src_cfg.yaddr = src_handle->fd;
    if (exynos5_format_is_ycrcb(src_handle->format)) {
        src_cfg.uaddr = src_handle->fd2;
        src_cfg.vaddr = src_handle->fd1;
    } else {
        src_cfg.uaddr = src_handle->fd1;
        src_cfg.vaddr = src_handle->fd2;
    }
    src_cfg.format = src_handle->format;

    dst_cfg.x = 0;
    dst_cfg.y = 0;
    dst_cfg.w = WIDTH(layer.displayFrame);
    dst_cfg.h = HEIGHT(layer.displayFrame);
    dst_cfg.format = HAL_PIXEL_FORMAT_BGRA_8888;
    dst_cfg.rot = layer.transform;

    ALOGV("source configuration:");
    dump_gsc_img(src_cfg);

    if (gsc_src_cfg_changed(src_cfg, gsc_data->src_cfg) ||
            gsc_dst_cfg_changed(dst_cfg, gsc_data->dst_cfg)) {
        int dst_stride;
        int usage = GRALLOC_USAGE_SW_READ_NEVER |
                GRALLOC_USAGE_SW_WRITE_NEVER |
                GRALLOC_USAGE_HW_COMPOSER;
        // TODO: add GRALLOC_USAGE_PROTECTED if source buffer is also protected

        int w = ALIGN(WIDTH(layer.displayFrame), GSC_W_ALIGNMENT);
        int h = ALIGN(HEIGHT(layer.displayFrame), GSC_H_ALIGNMENT);

        for (size_t i = 0; i < NUM_GSC_DST_BUFS; i++) {
            if (gsc_data->dst_buf[i]) {
                alloc_device->free(alloc_device, gsc_data->dst_buf[i]);
                gsc_data->dst_buf[i] = NULL;
            }

            int ret = alloc_device->alloc(alloc_device, w, h,
                    HAL_PIXEL_FORMAT_RGBX_8888, usage, &gsc_data->dst_buf[i],
                    &dst_stride);
            if (ret < 0) {
                ALOGE("failed to allocate destination buffer: %s",
                        strerror(-ret));
                goto err_alloc;
            }
        }

        gsc_data->current_buf = 0;
    }

    dst_buf = gsc_data->dst_buf[gsc_data->current_buf];
    dst_handle = private_handle_t::dynamicCast(dst_buf);

    dst_cfg.fw = dst_handle->stride;
    dst_cfg.fh = dst_handle->height;
    dst_cfg.yaddr = dst_handle->fd;

    ALOGV("destination configuration:");
    dump_gsc_img(dst_cfg);

    gsc_data->gsc = exynos_gsc_create_exclusive(gsc_idx, GSC_M2M_MODE,
            GSC_DUMMY);
    if (!gsc_data->gsc) {
        ALOGE("failed to create gscaler handle");
        ret = -1;
        goto err_alloc;
    }

    ret = exynos_gsc_config_exclusive(gsc_data->gsc, &src_cfg, &dst_cfg);
    if (ret < 0) {
        ALOGE("failed to configure gscaler %u", gsc_idx);
        goto err_gsc_config;
    }

    ret = exynos_gsc_run_exclusive(gsc_data->gsc, &src_cfg, &dst_cfg);
    if (ret < 0) {
        ALOGE("failed to run gscaler %u", gsc_idx);
        goto err_gsc_config;
    }

    gsc_data->src_cfg = src_cfg;
    gsc_data->dst_cfg = dst_cfg;

    return 0;

err_gsc_config:
    exynos_gsc_destroy(gsc_data->gsc);
    gsc_data->gsc = NULL;
err_alloc:
    for (size_t i = 0; i < NUM_GSC_DST_BUFS; i++) {
        if (gsc_data->dst_buf[i]) {
           alloc_device->free(alloc_device, gsc_data->dst_buf[i]);
           gsc_data->dst_buf[i] = NULL;
       }
    }
    return ret;
}

static void exynos5_config_handle(private_handle_t *handle,
        hwc_rect_t &sourceCrop, hwc_rect_t &displayFrame,
        s3c_fb_win_config &cfg)
{
    cfg.state = cfg.S3C_FB_WIN_STATE_BUFFER;
    cfg.fd = handle->fd;
    cfg.x = displayFrame.left;
    cfg.y = displayFrame.top;
    cfg.w = WIDTH(displayFrame);
    cfg.h = HEIGHT(displayFrame);
    cfg.format = exynos5_format_to_s3c_format(handle->format);
    uint8_t bpp = exynos5_format_to_bpp(handle->format);
    cfg.offset = (sourceCrop.top * handle->stride + sourceCrop.left) * bpp / 8;
    cfg.stride = handle->stride * bpp / 8;
}

static void exynos5_config_overlay(hwc_layer_1_t *layer, s3c_fb_win_config &cfg,
        const private_module_t *gralloc_module)
{
    if (layer->compositionType == HWC_BACKGROUND) {
        hwc_color_t color = layer->backgroundColor;
        cfg.state = cfg.S3C_FB_WIN_STATE_COLOR;
        cfg.color = (color.r << 16) | (color.g << 8) | color.b;
        cfg.x = 0;
        cfg.y = 0;
        cfg.w = gralloc_module->xres;
        cfg.h = gralloc_module->yres;
        return;
    }

    private_handle_t *handle = private_handle_t::dynamicCast(layer->handle);
    exynos5_config_handle(handle, layer->sourceCrop, layer->displayFrame, cfg);
}

static void exynos5_post_callback(void *data, private_handle_t *fb)
{
    exynos5_hwc_post_data_t *pdata = (exynos5_hwc_post_data_t *)data;

    struct s3c_fb_win_config_data win_data;
    struct s3c_fb_win_config *config = win_data.config;
    memset(config, 0, sizeof(win_data.config));

    for (size_t i = 0; i < NUM_HW_WINDOWS; i++) {
        if ( pdata->overlay_map[i] != -1) {
            hwc_layer_1_t &layer = pdata->overlays[i];
            private_handle_t *handle =
                    private_handle_t::dynamicCast(layer.handle);

            if (layer.acquireFenceFd != -1) {
                int err = sync_wait(layer.acquireFenceFd, 100);
                if (err != 0)
                    ALOGW("fence for layer %zu didn't signal in 100 ms: %s",
                          i, strerror(errno));
                close(layer.acquireFenceFd);
            }

            if (pdata->gsc_map[i].mode == exynos5_gsc_map_t::GSC_M2M) {
                int gsc_idx = pdata->gsc_map[i].idx;
                exynos5_config_gsc_m2m(layer, pdata->pdev->alloc_device,
                        &pdata->pdev->gsc[gsc_idx], gsc_idx);
            }
        }
    }

    for (size_t i = 0; i < NUM_HW_WINDOWS; i++) {
        if (i == pdata->fb_window) {
            hwc_rect_t rect = { 0, 0, fb->width, fb->height };
            exynos5_config_handle(fb, rect, rect, config[i]);
        } else if ( pdata->overlay_map[i] != -1) {
            hwc_layer_1_t &layer = pdata->overlays[i];
            private_handle_t *handle =
                    private_handle_t::dynamicCast(layer.handle);

            if (pdata->gsc_map[i].mode == exynos5_gsc_map_t::GSC_M2M) {
                int gsc_idx = pdata->gsc_map[i].idx;
                exynos5_gsc_data_t &gsc = pdata->pdev->gsc[gsc_idx];

                if (!gsc.gsc) {
                    ALOGE("failed to queue gscaler %u input for layer %u",
                            gsc_idx, i);
                    continue;
                }

                int err = exynos_gsc_stop_exclusive(gsc.gsc);
                exynos_gsc_destroy(gsc.gsc);
                gsc.gsc = NULL;
                if (err < 0) {
                    ALOGE("failed to dequeue gscaler output for layer %u", i);
                    continue;
                }

                buffer_handle_t dst_buf = gsc.dst_buf[gsc.current_buf];
                gsc.current_buf = (gsc.current_buf + 1) % NUM_GSC_DST_BUFS;
                private_handle_t *dst_handle =
                        private_handle_t::dynamicCast(dst_buf);
                exynos5_config_handle(dst_handle, layer.sourceCrop,
                        layer.displayFrame, config[i]);
            }
            else {
                exynos5_config_overlay(&layer, config[i],
                        pdata->pdev->gralloc_module);
            }
        }
        ALOGV("window %u configuration:", i);
        dump_config(config[i]);
    }

    int ret = ioctl(pdata->pdev->fd, S3CFB_WIN_CONFIG, &win_data);
    if (ret < 0)
        ALOGE("ioctl S3CFB_WIN_CONFIG failed: %d", errno);

    if (pdata->pdev->hdmi_mirroring)
        hdmi_output(pdata->pdev, fb);

    pthread_mutex_lock(&pdata->completion_lock);
    pdata->fence = win_data.fence;
    pthread_cond_signal(&pdata->completion);
    pthread_mutex_unlock(&pdata->completion_lock);
}

static int exynos5_set(struct hwc_composer_device_1 *dev,
        size_t numDisplays, hwc_display_contents_1_t** displays)
{
    exynos5_hwc_composer_device_1_t *pdev =
            (exynos5_hwc_composer_device_1_t *)dev;

    if (!numDisplays || !displays || !displays[0] || !displays[0]->dpy || !displays[0]->sur)
        return 0;

    hwc_callback_queue_t *queue = NULL;
    pthread_mutex_t *lock = NULL;
    exynos5_hwc_post_data_t *data = NULL;

    if (displays[0]->numHwLayers) {
        for (size_t i = 0; i < NUM_HW_WINDOWS; i++) {
            if (pdev->bufs.overlay_map[i] != -1) {
                pdev->bufs.overlays[i] =
                    displays[0]->hwLayers[pdev->bufs.overlay_map[i]];
            }
        }

        data = (exynos5_hwc_post_data_t *)
                malloc(sizeof(exynos5_hwc_post_data_t));
        memcpy(data, &pdev->bufs, sizeof(pdev->bufs));

        data->fence = -1;
        pthread_mutex_init(&data->completion_lock, NULL);
        pthread_cond_init(&data->completion, NULL);

        if (pdev->bufs.fb_window == NO_FB_NEEDED) {
            exynos5_post_callback(data, NULL);
        } else {

            struct hwc_callback_entry entry;
            entry.callback = exynos5_post_callback;
            entry.data = data;

            queue = reinterpret_cast<hwc_callback_queue_t *>(
                pdev->gralloc_module->queue);
            lock = const_cast<pthread_mutex_t *>(
                &pdev->gralloc_module->queue_lock);

            pthread_mutex_lock(lock);
            queue->push_front(entry);
            pthread_mutex_unlock(lock);

            EGLBoolean success = eglSwapBuffers((EGLDisplay)displays[0]->dpy,
                    (EGLSurface)displays[0]->sur);
            if (!success) {
                ALOGE("HWC_EGL_ERROR");
                if (displays[0]) {
                    pthread_mutex_lock(lock);
                    queue->removeAt(0);
                    pthread_mutex_unlock(lock);
                    free(data);
                }
                return HWC_EGL_ERROR;
            }
        }
    }


    pthread_mutex_lock(&data->completion_lock);
    while (data->fence == -1)
        pthread_cond_wait(&data->completion, &data->completion_lock);
    pthread_mutex_unlock(&data->completion_lock);

    for (size_t i = 0; i < NUM_HW_WINDOWS; i++) {
        if (pdev->bufs.overlay_map[i] != -1) {
            int dup_fd = dup(data->fence);
            if (dup_fd < 0)
                ALOGW("release fence dup failed: %s", strerror(errno));
            displays[0]->hwLayers[pdev->bufs.overlay_map[i]].releaseFenceFd = dup_fd;
        }
    }
    close(data->fence);
    free(data);
    return 0;
}

static void exynos5_registerProcs(struct hwc_composer_device_1* dev,
        hwc_procs_t const* procs)
{
    struct exynos5_hwc_composer_device_1_t* pdev =
            (struct exynos5_hwc_composer_device_1_t*)dev;
    pdev->procs = const_cast<hwc_procs_t *>(procs);
}

static int exynos5_query(struct hwc_composer_device_1* dev, int what, int *value)
{
    struct exynos5_hwc_composer_device_1_t *pdev =
            (struct exynos5_hwc_composer_device_1_t *)dev;

    switch (what) {
    case HWC_BACKGROUND_LAYER_SUPPORTED:
        // we support the background layer
        value[0] = 1;
        break;
    case HWC_VSYNC_PERIOD:
        // vsync period in nanosecond
        value[0] = 1000000000.0 / pdev->gralloc_module->fps;
        break;
    default:
        // unsupported query
        return -EINVAL;
    }
    return 0;
}

static int exynos5_eventControl(struct hwc_composer_device_1 *dev, int dpy,
        int event, int enabled)
{
    struct exynos5_hwc_composer_device_1_t *pdev =
            (struct exynos5_hwc_composer_device_1_t *)dev;

    switch (event) {
    case HWC_EVENT_VSYNC:
        __u32 val = !!enabled;
        int err = ioctl(pdev->fd, S3CFB_SET_VSYNC_INT, &val);
        if (err < 0) {
            ALOGE("vsync ioctl failed");
            return -errno;
        }

        return 0;
    }

    return -EINVAL;
}

static void handle_hdmi_uevent(struct exynos5_hwc_composer_device_1_t *pdev,
        const char *buff, int len)
{
    const char *s = buff;
    s += strlen(s) + 1;

    while (*s) {
        if (!strncmp(s, "SWITCH_STATE=", strlen("SWITCH_STATE=")))
            pdev->hdmi_hpd = atoi(s + strlen("SWITCH_STATE=")) == 1;

        s += strlen(s) + 1;
        if (s - buff >= len)
            break;
    }

    ALOGV("HDMI HPD changed to %s", pdev->hdmi_hpd ? "enabled" : "disabled");

    if (pdev->procs && pdev->procs->invalidate)
        pdev->procs->invalidate(pdev->procs);
}

static void handle_vsync_event(struct exynos5_hwc_composer_device_1_t *pdev)
{
    if (!pdev->procs || !pdev->procs->vsync)
        return;

    int err = lseek(pdev->vsync_fd, 0, SEEK_SET);
    if (err < 0) {
        ALOGE("error seeking to vsync timestamp: %s", strerror(errno));
        return;
    }

    char buf[4096];
    err = read(pdev->vsync_fd, buf, sizeof(buf));
    if (err < 0) {
        ALOGE("error reading vsync timestamp: %s", strerror(errno));
        return;
    }
    buf[sizeof(buf) - 1] = '\0';

    errno = 0;
    uint64_t timestamp = strtoull(buf, NULL, 0);
    if (!errno)
        pdev->procs->vsync(pdev->procs, 0, timestamp);
}

static void *hwc_vsync_thread(void *data)
{
    struct exynos5_hwc_composer_device_1_t *pdev =
            (struct exynos5_hwc_composer_device_1_t *)data;
    char uevent_desc[4096];
    memset(uevent_desc, 0, sizeof(uevent_desc));

    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    uevent_init();

    char temp[4096];
    int err = read(pdev->vsync_fd, temp, sizeof(temp));
    if (err < 0) {
        ALOGE("error reading vsync timestamp: %s", strerror(errno));
        return NULL;
    }

    struct pollfd fds[2];
    fds[0].fd = pdev->vsync_fd;
    fds[0].events = POLLPRI;
    fds[1].fd = uevent_get_fd();
    fds[1].events = POLLIN;

    while (true) {
        int err = poll(fds, 2, -1);

        if (err > 0) {
            if (fds[0].revents & POLLPRI) {
                handle_vsync_event(pdev);
            }
            else if (fds[1].revents & POLLIN) {
                int len = uevent_next_event(uevent_desc,
                        sizeof(uevent_desc) - 2);

                bool hdmi = !strcmp(uevent_desc,
                        "change@/devices/virtual/switch/hdmi");
                if (hdmi)
                    handle_hdmi_uevent(pdev, uevent_desc, len);
            }
        }
        else if (err == -1) {
            if (errno == EINTR)
                break;
            ALOGE("error in vsync thread: %s", strerror(errno));
        }
    }

    return NULL;
}

static int exynos5_blank(struct hwc_composer_device_1 *dev, int dpy, int blank)
{
    struct exynos5_hwc_composer_device_1_t *pdev =
            (struct exynos5_hwc_composer_device_1_t *)dev;

    int fb_blank = blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK;
    int err = ioctl(pdev->fd, FBIOBLANK, fb_blank);
    if (err < 0) {
        ALOGE("%sblank ioctl failed", blank ? "" : "un");
        return -errno;
    }

    return 0;
}

struct hwc_methods_1 exynos5_methods = {
    eventControl: exynos5_eventControl,
    blank: exynos5_blank,
};

static int exynos5_close(hw_device_t* device);

static int exynos5_open(const struct hw_module_t *module, const char *name,
        struct hw_device_t **device)
{
    int ret;
    int sw_fd;

    if (strcmp(name, HWC_HARDWARE_COMPOSER)) {
        return -EINVAL;
    }

    struct exynos5_hwc_composer_device_1_t *dev;
    dev = (struct exynos5_hwc_composer_device_1_t *)malloc(sizeof(*dev));
    memset(dev, 0, sizeof(*dev));

    if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
            (const struct hw_module_t **)&dev->gralloc_module)) {
        ALOGE("failed to get gralloc hw module");
        ret = -EINVAL;
        goto err_get_module;
    }

    if (gralloc_open((const hw_module_t *)dev->gralloc_module,
            &dev->alloc_device)) {
        ALOGE("failed to open gralloc");
        ret = -EINVAL;
        goto err_get_module;
    }

    dev->fd = open("/dev/graphics/fb0", O_RDWR);
    if (dev->fd < 0) {
        ALOGE("failed to open framebuffer");
        ret = dev->fd;
        goto err_open_fb;
    }

    dev->vsync_fd = open("/sys/devices/platform/exynos5-fb.1/vsync", O_RDONLY);
    if (dev->vsync_fd < 0) {
        ALOGE("failed to open vsync attribute");
        ret = dev->vsync_fd;
        goto err_ioctl;
    }

    sw_fd = open("/sys/class/switch/hdmi/state", O_RDONLY);
    if (sw_fd) {
        char val;
        if (read(sw_fd, &val, 1) == 1 && val == '1')
            dev->hdmi_hpd = true;
    }

    dev->base.common.tag = HARDWARE_DEVICE_TAG;
    dev->base.common.version = HWC_DEVICE_API_VERSION_1_0;
    dev->base.common.module = const_cast<hw_module_t *>(module);
    dev->base.common.close = exynos5_close;

    dev->base.prepare = exynos5_prepare;
    dev->base.set = exynos5_set;
    dev->base.registerProcs = exynos5_registerProcs;
    dev->base.query = exynos5_query;
    dev->base.methods = &exynos5_methods;

    dev->bufs.pdev = dev;

    *device = &dev->base.common;

    ret = pthread_create(&dev->vsync_thread, NULL, hwc_vsync_thread, dev);
    if (ret) {
        ALOGE("failed to start vsync thread: %s", strerror(ret));
        ret = -ret;
        goto err_vsync;
    }

    return 0;

err_vsync:
    close(dev->vsync_fd);
err_ioctl:
    close(dev->fd);
err_open_fb:
    gralloc_close(dev->alloc_device);
err_get_module:
    free(dev);
    return ret;
}

static int exynos5_close(hw_device_t *device)
{
    struct exynos5_hwc_composer_device_1_t *dev =
            (struct exynos5_hwc_composer_device_1_t *)device;
    pthread_kill(dev->vsync_thread, SIGTERM);
    pthread_join(dev->vsync_thread, NULL);
    for (size_t i = 0; i < NUM_GSC_UNITS; i++) {
        if (dev->gsc[i].gsc)
            exynos_gsc_destroy(dev->gsc[i].gsc);
        for (size_t j = 0; i < NUM_GSC_DST_BUFS; j++)
            if (dev->gsc[i].dst_buf[j])
                dev->alloc_device->free(dev->alloc_device, dev->gsc[i].dst_buf[j]);
    }
    gralloc_close(dev->alloc_device);
    close(dev->vsync_fd);
    close(dev->fd);
    return 0;
}

static struct hw_module_methods_t exynos5_hwc_module_methods = {
    open: exynos5_open,
};

hwc_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        module_api_version: HWC_MODULE_API_VERSION_0_1,
        hal_api_version: HARDWARE_HAL_API_VERSION,
        id: HWC_HARDWARE_MODULE_ID,
        name: "Samsung exynos5 hwcomposer module",
        author: "Google",
        methods: &exynos5_hwc_module_methods,
    }
};
