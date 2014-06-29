/*
 * Cogl
 *
 * A Low-Level GPU Graphics and Utilities API
 *
 * Copyright (C) 2007,2008,2009 Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cogl-util.h"
#include "cogl-debug.h"
#include "cogl-private.h"
#include "cogl-bitmap-private.h"
#include "cogl-buffer-private.h"
#include "cogl-pixel-buffer.h"
#include "cogl-context-private.h"
#include "cogl-buffer-gl-private.h"
#include "cogl-error-private.h"

#include <string.h>

static void _cg_bitmap_free(cg_bitmap_t *bmp);

CG_OBJECT_DEFINE(Bitmap, bitmap);

static void
_cg_bitmap_free(cg_bitmap_t *bmp)
{
    c_assert(!bmp->mapped);
    c_assert(!bmp->bound);

    if (bmp->shared_bmp)
        cg_object_unref(bmp->shared_bmp);

    if (bmp->buffer)
        cg_object_unref(bmp->buffer);

    c_slice_free(cg_bitmap_t, bmp);
}

bool
_cg_bitmap_convert_premult_status(cg_bitmap_t *bmp,
                                  cg_pixel_format_t dst_format,
                                  cg_error_t **error)
{
    /* Do we need to unpremultiply? */
    if ((bmp->format & CG_PREMULT_BIT) > 0 &&
        (dst_format & CG_PREMULT_BIT) == 0 &&
        CG_PIXEL_FORMAT_CAN_HAVE_PREMULT(dst_format))
        return _cg_bitmap_unpremult(bmp, error);

    /* Do we need to premultiply? */
    if ((bmp->format & CG_PREMULT_BIT) == 0 &&
        CG_PIXEL_FORMAT_CAN_HAVE_PREMULT(bmp->format) &&
        (dst_format & CG_PREMULT_BIT) > 0)
        /* Try premultiplying using imaging library */
        return _cg_bitmap_premult(bmp, error);

    return true;
}

cg_bitmap_t *
_cg_bitmap_copy(cg_bitmap_t *src_bmp, cg_error_t **error)
{
    cg_bitmap_t *dst_bmp;
    cg_pixel_format_t src_format = cg_bitmap_get_format(src_bmp);
    int width = cg_bitmap_get_width(src_bmp);
    int height = cg_bitmap_get_height(src_bmp);

    dst_bmp = _cg_bitmap_new_with_malloc_buffer(
        src_bmp->context, width, height, src_format, error);
    if (!dst_bmp)
        return NULL;

    if (!_cg_bitmap_copy_subregion(src_bmp,
                                   dst_bmp,
                                   0,
                                   0, /* src_x/y */
                                   0,
                                   0, /* dst_x/y */
                                   width,
                                   height,
                                   error)) {
        cg_object_unref(dst_bmp);
        return NULL;
    }

    return dst_bmp;
}

bool
_cg_bitmap_copy_subregion(cg_bitmap_t *src,
                          cg_bitmap_t *dst,
                          int src_x,
                          int src_y,
                          int dst_x,
                          int dst_y,
                          int width,
                          int height,
                          cg_error_t **error)
{
    uint8_t *srcdata;
    uint8_t *dstdata;
    int bpp;
    int line;
    bool succeeded = false;

    /* Intended only for fast copies when format is equal! */
    _CG_RETURN_VAL_IF_FAIL((src->format & ~CG_PREMULT_BIT) ==
                           (dst->format & ~CG_PREMULT_BIT),
                           false);

    bpp = _cg_pixel_format_get_bytes_per_pixel(src->format);

    if ((srcdata = _cg_bitmap_map(src, CG_BUFFER_ACCESS_READ, 0, error))) {
        if ((dstdata = _cg_bitmap_map(dst, CG_BUFFER_ACCESS_WRITE, 0, error))) {
            srcdata += src_y * src->rowstride + src_x * bpp;
            dstdata += dst_y * dst->rowstride + dst_x * bpp;

            for (line = 0; line < height; ++line) {
                memcpy(dstdata, srcdata, width * bpp);
                srcdata += src->rowstride;
                dstdata += dst->rowstride;
            }

            succeeded = true;

            _cg_bitmap_unmap(dst);
        }

        _cg_bitmap_unmap(src);
    }

    return succeeded;
}

bool
cg_bitmap_get_size_from_file(const char *filename, int *width, int *height)
{
    return _cg_bitmap_get_size_from_file(filename, width, height);
}

cg_bitmap_t *
cg_bitmap_new_for_data(cg_context_t *context,
                       int width,
                       int height,
                       cg_pixel_format_t format,
                       int rowstride,
                       uint8_t *data)
{
    cg_bitmap_t *bmp;

    c_return_val_if_fail(cg_is_context(context), NULL);

    /* Rowstride from width if not given */
    if (rowstride == 0)
        rowstride = width * _cg_pixel_format_get_bytes_per_pixel(format);

    bmp = c_slice_new(cg_bitmap_t);
    bmp->context = context;
    bmp->format = format;
    bmp->width = width;
    bmp->height = height;
    bmp->rowstride = rowstride;
    bmp->data = data;
    bmp->mapped = false;
    bmp->bound = false;
    bmp->shared_bmp = NULL;
    bmp->buffer = NULL;

    return _cg_bitmap_object_new(bmp);
}

cg_bitmap_t *
_cg_bitmap_new_with_malloc_buffer(cg_context_t *context,
                                  int width,
                                  int height,
                                  cg_pixel_format_t format,
                                  cg_error_t **error)
{
    static cg_user_data_key_t bitmap_free_key;
    int bpp = _cg_pixel_format_get_bytes_per_pixel(format);
    int rowstride = ((width * bpp) + 3) & ~3;
    uint8_t *data = c_try_malloc(rowstride * height);
    cg_bitmap_t *bitmap;

    if (!data) {
        _cg_set_error(error,
                      CG_SYSTEM_ERROR,
                      CG_SYSTEM_ERROR_NO_MEMORY,
                      "Failed to allocate memory for bitmap");
        return NULL;
    }

    bitmap =
        cg_bitmap_new_for_data(context, width, height, format, rowstride, data);
    cg_object_set_user_data(CG_OBJECT(bitmap), &bitmap_free_key, data, c_free);

    return bitmap;
}

cg_bitmap_t *
_cg_bitmap_new_shared(cg_bitmap_t *shared_bmp,
                      cg_pixel_format_t format,
                      int width,
                      int height,
                      int rowstride)
{
    cg_bitmap_t *bmp;

    bmp = cg_bitmap_new_for_data(
        shared_bmp->context, width, height, format, rowstride, NULL /* data */);

    bmp->shared_bmp = cg_object_ref(shared_bmp);

    return bmp;
}

cg_bitmap_t *
cg_bitmap_new_from_file(cg_context_t *ctx,
                        const char *filename,
                        cg_error_t **error)
{
    _CG_RETURN_VAL_IF_FAIL(filename != NULL, NULL);
    _CG_RETURN_VAL_IF_FAIL(error == NULL || *error == NULL, NULL);

    return _cg_bitmap_from_file(ctx, filename, error);
}

cg_bitmap_t *
cg_bitmap_new_from_buffer(cg_buffer_t *buffer,
                          cg_pixel_format_t format,
                          int width,
                          int height,
                          int rowstride,
                          int offset)
{
    cg_bitmap_t *bmp;

    _CG_RETURN_VAL_IF_FAIL(cg_is_buffer(buffer), NULL);

    bmp = cg_bitmap_new_for_data(
        buffer->context, width, height, format, rowstride, NULL /* data */);

    bmp->buffer = cg_object_ref(buffer);
    bmp->data = GINT_TO_POINTER(offset);

    return bmp;
}

cg_bitmap_t *
cg_bitmap_new_with_size(cg_context_t *context,
                        int width,
                        int height,
                        cg_pixel_format_t format)
{
    cg_pixel_buffer_t *pixel_buffer;
    cg_bitmap_t *bitmap;
    int rowstride;

    /* creating a buffer to store "any" format does not make sense */
    _CG_RETURN_VAL_IF_FAIL(format != CG_PIXEL_FORMAT_ANY, NULL);

    /* for now we fallback to cg_pixel_buffer_new, later, we could ask
     * libdrm a tiled buffer for instance */
    rowstride = width * _cg_pixel_format_get_bytes_per_pixel(format);

    pixel_buffer = cg_pixel_buffer_new(context,
                                       height * rowstride,
                                       NULL, /* data */
                                       NULL); /* don't catch errors */

    _CG_RETURN_VAL_IF_FAIL(pixel_buffer != NULL, NULL);

    bitmap = cg_bitmap_new_from_buffer(CG_BUFFER(pixel_buffer),
                                       format,
                                       width,
                                       height,
                                       rowstride,
                                       0 /* offset */);

    cg_object_unref(pixel_buffer);

    return bitmap;
}

#ifdef CG_HAS_ANDROID_SUPPORT
cg_bitmap_t *
cg_android_bitmap_new_from_asset(cg_context_t *ctx,
                                 AAssetManager *manager,
                                 const char *filename,
                                 cg_error_t **error)
{
    _CG_RETURN_VAL_IF_FAIL(ctx != NULL, NULL);
    _CG_RETURN_VAL_IF_FAIL(manager != NULL, NULL);
    _CG_RETURN_VAL_IF_FAIL(filename != NULL, NULL);
    _CG_RETURN_VAL_IF_FAIL(error == NULL || *error == NULL, NULL);

    return _cg_android_bitmap_new_from_asset(ctx, manager, filename, error);
}
#endif

cg_pixel_format_t
cg_bitmap_get_format(cg_bitmap_t *bitmap)
{
    return bitmap->format;
}

void
_cg_bitmap_set_format(cg_bitmap_t *bitmap, cg_pixel_format_t format)
{
    bitmap->format = format;
}

int
cg_bitmap_get_width(cg_bitmap_t *bitmap)
{
    return bitmap->width;
}

int
cg_bitmap_get_height(cg_bitmap_t *bitmap)
{
    return bitmap->height;
}

int
cg_bitmap_get_rowstride(cg_bitmap_t *bitmap)
{
    return bitmap->rowstride;
}

cg_pixel_buffer_t *
cg_bitmap_get_buffer(cg_bitmap_t *bitmap)
{
    while (bitmap->shared_bmp)
        bitmap = bitmap->shared_bmp;

    return CG_PIXEL_BUFFER(bitmap->buffer);
}

uint32_t
cg_bitmap_error_domain(void)
{
    return c_quark_from_static_string("cogl-bitmap-error-quark");
}

uint8_t *
_cg_bitmap_map(cg_bitmap_t *bitmap,
               cg_buffer_access_t access,
               cg_buffer_map_hint_t hints,
               cg_error_t **error)
{
    /* Divert to another bitmap if this data is shared */
    if (bitmap->shared_bmp)
        return _cg_bitmap_map(bitmap->shared_bmp, access, hints, error);

    c_assert(!bitmap->mapped);

    if (bitmap->buffer) {
        uint8_t *data = cg_buffer_map(bitmap->buffer, access, hints, error);

        CG_NOTE(BITMAP,
                "A pixel array is being mapped from a bitmap. This "
                "usually means that some conversion on the pixel array is "
                "needed so a sub-optimal format is being used.");

        if (data) {
            bitmap->mapped = true;

            return data + GPOINTER_TO_INT(bitmap->data);
        } else
            return NULL;
    } else {
        bitmap->mapped = true;

        return bitmap->data;
    }
}

void
_cg_bitmap_unmap(cg_bitmap_t *bitmap)
{
    /* Divert to another bitmap if this data is shared */
    if (bitmap->shared_bmp) {
        _cg_bitmap_unmap(bitmap->shared_bmp);
        return;
    }

    c_assert(bitmap->mapped);
    bitmap->mapped = false;

    if (bitmap->buffer)
        cg_buffer_unmap(bitmap->buffer);
}

uint8_t *
_cg_bitmap_gl_bind(cg_bitmap_t *bitmap,
                   cg_buffer_access_t access,
                   cg_buffer_map_hint_t hints,
                   cg_error_t **error)
{
    uint8_t *ptr;
    cg_error_t *internal_error = NULL;

    c_return_val_if_fail(
        access & (CG_BUFFER_ACCESS_READ | CG_BUFFER_ACCESS_WRITE), NULL);

    /* Divert to another bitmap if this data is shared */
    if (bitmap->shared_bmp)
        return _cg_bitmap_gl_bind(bitmap->shared_bmp, access, hints, error);

    _CG_RETURN_VAL_IF_FAIL(!bitmap->bound, NULL);

    /* If the bitmap wasn't created from a buffer then the
       implementation of bind is the same as map */
    if (bitmap->buffer == NULL) {
        uint8_t *data = _cg_bitmap_map(bitmap, access, hints, error);
        if (data)
            bitmap->bound = true;
        return data;
    }

    if (access == CG_BUFFER_ACCESS_READ)
        ptr = _cg_buffer_gl_bind(bitmap->buffer,
                                 CG_BUFFER_BIND_TARGET_PIXEL_UNPACK,
                                 &internal_error);
    else if (access == CG_BUFFER_ACCESS_WRITE)
        ptr = _cg_buffer_gl_bind(
            bitmap->buffer, CG_BUFFER_BIND_TARGET_PIXEL_PACK, &internal_error);
    else {
        ptr = NULL;
        c_assert_not_reached();
        return NULL;
    }

    /* NB: _cg_buffer_gl_bind() may return NULL in non-error
     * conditions so we have to explicitly check internal_error to see
     * if an exception was thrown */
    if (internal_error) {
        _cg_propagate_error(error, internal_error);
        return NULL;
    }

    bitmap->bound = true;

    /* The data pointer actually stores the offset */
    return ptr + GPOINTER_TO_INT(bitmap->data);
}

void
_cg_bitmap_gl_unbind(cg_bitmap_t *bitmap)
{
    /* Divert to another bitmap if this data is shared */
    if (bitmap->shared_bmp) {
        _cg_bitmap_gl_unbind(bitmap->shared_bmp);
        return;
    }

    c_assert(bitmap->bound);
    bitmap->bound = false;

    /* If the bitmap wasn't created from a pixel array then the
       implementation of unbind is the same as unmap */
    if (bitmap->buffer)
        _cg_buffer_gl_unbind(bitmap->buffer);
    else
        _cg_bitmap_unmap(bitmap);
}

cg_context_t *
_cg_bitmap_get_context(cg_bitmap_t *bitmap)
{
    return bitmap->context;
}
