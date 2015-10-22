/*
 * CGlib
 *
 * A Low-Level GPU Graphics and Utilities API
 *
 * Copyright (C) 2010 Intel Corporation.
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
 * Authors:
 *   Neil Roberts <neil@linux.intel.com>
 */

#if !defined(__CG_H_INSIDE__) && !defined(CG_COMPILATION)
#error "Only <cg/cg.h> can be included directly."
#endif

#ifndef __CG_TEXTURE_3D_H
#define __CG_TEXTURE_3D_H

CG_BEGIN_DECLS

/**
 * SECTION:cg-texture-3d
 * @short_description: Functions for creating and manipulating 3D textures
 *
 * These functions allow 3D textures to be used. 3D textures can be
 * thought of as layers of 2D images arranged into a cuboid
 * shape. When choosing a texel from the texture, CGlib will take into
 * account the 'r' texture coordinate to select one of the images.
 */

typedef struct _cg_texture_3d_t cg_texture_3d_t;

#define CG_TEXTURE_3D(X) ((cg_texture_3d_t *)X)

/**
 * cg_texture_3d_new_with_size:
 * @dev: A #cg_device_t
 * @width: width of the texture in pixels.
 * @height: height of the texture in pixels.
 * @depth: depth of the texture in pixels.
 *
 * Creates a low-level #cg_texture_3d_t texture with the specified
 * dimensions and pixel format.
 *
 * The storage for the texture is not allocated before this function
 * returns. You can call cg_texture_allocate() to explicitly
 * allocate the underlying storage or preferably let CGlib
 * automatically allocate storage lazily when it may know more about
 * how the texture is going to be used and can optimize how it is
 * allocated.
 *
 * The texture is still configurable until it has been allocated so
 * for example you can influence the internal format of the texture
 * using cg_texture_set_components() and
 * cg_texture_set_premultiplied().
 *
 * <note>This texture will fail to allocate later if
 * %CG_FEATURE_ID_TEXTURE_3D is not advertised. Allocation can also
 * fail if the requested dimensions are not supported by the
 * GPU.</note>
 *
 * Returns: (transfer full): A new #cg_texture_3d_t object with no storage yet
 * allocated.
 * Stability: Unstable
 */
cg_texture_3d_t *cg_texture_3d_new_with_size(cg_device_t *dev,
                                             int width,
                                             int height,
                                             int depth);

/**
 * cg_texture_3d_new_from_data:
 * @dev: A #cg_device_t
 * @width: width of the texture in pixels.
 * @height: height of the texture in pixels.
 * @depth: depth of the texture in pixels.
 * @format: the #cg_pixel_format_t the buffer is stored in in RAM
 * @rowstride: the memory offset in bytes between the starts of
 *    scanlines in @data or 0 to infer it from the width and format
 * @image_stride: the number of bytes from one image to the next. This
 *    can be used to add padding between the images in a similar way
 *    that the rowstride can be used to add padding between
 *    rows. Alternatively 0 can be passed to infer the @image_stride
 *    from the @height.
 * @data: pointer the memory region where the source buffer resides
 * @error: A cg_error_t return location.
 *
 * Creates a low-level 3D texture and initializes it with @data. The
 * data is assumed to be packed array of @depth images. There can be
 * padding between the images using @image_stride.
 *
 * <note>This api will always immediately allocate GPU memory for the
 * texture and upload the given data so that the @data pointer does
 * not need to remain valid once this function returns. This means it
 * is not possible to configure the texture before it is allocated. If
 * you do need to configure the texture before allocation (to specify
 * constraints on the internal format for example) then you can
 * instead create a #cg_bitmap_t for your data and use
 * cg_texture_3d_new_from_bitmap().</note>
 *
 * Return value: (transfer full): the newly created #cg_texture_3d_t or
 *               %NULL if there was an error and an exception will be
 *               returned through @error.
 * Stability: Unstable
 */
cg_texture_3d_t *cg_texture_3d_new_from_data(cg_device_t *dev,
                                             int width,
                                             int height,
                                             int depth,
                                             cg_pixel_format_t format,
                                             int rowstride,
                                             int image_stride,
                                             const uint8_t *data,
                                             cg_error_t **error);

/**
 * cg_texture_3d_new_from_bitmap:
 * @bitmap: A #cg_bitmap_t object.
 * @height: height of the texture in pixels.
 * @depth: depth of the texture in pixels.
 *
 * Creates a low-level 3D texture and initializes it with the images
 * in @bitmap. The images are assumed to be packed together after one
 * another in the increasing y axis. The height of individual image is
 * given as @height and the number of images is given in @depth. The
 * actual height of the bitmap can be larger than @height × @depth. In
 * this case it assumes there is padding between the images.
 *
 * The storage for the texture is not allocated before this function
 * returns. You can call cg_texture_allocate() to explicitly
 * allocate the underlying storage or preferably let CGlib
 * automatically allocate storage lazily when it may know more about
 * how the texture is going to be used and can optimize how it is
 * allocated.
 *
 * The texture is still configurable until it has been allocated so
 * for example you can influence the internal format of the texture
 * using cg_texture_set_components() and
 * cg_texture_set_premultiplied().
 *
 * <note>This texture will fail to allocate later if
 * %CG_FEATURE_ID_TEXTURE_3D is not advertised. Allocation can also
 * fail if the requested dimensions are not supported by the
 * GPU.</note>
 *
 * Return value: (transfer full): a newly created #cg_texture_3d_t
 * Stability: unstable
 */
cg_texture_3d_t *
cg_texture_3d_new_from_bitmap(cg_bitmap_t *bitmap, int height, int depth);

/**
 * cg_is_texture_3d:
 * @object: a #cg_object_t
 *
 * Checks whether the given object references a #cg_texture_3d_t
 *
 * Return value: %true if the passed object represents a 3D texture
 *   and %false otherwise
 *
 * Stability: Unstable
 */
bool cg_is_texture_3d(void *object);

CG_END_DECLS

#endif /* __CG_TEXTURE_3D_H */
