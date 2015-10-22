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

#ifndef __CG_TEXTURE_3D_PRIVATE_H
#define __CG_TEXTURE_3D_PRIVATE_H

#include "cg-object-private.h"
#include "cg-pipeline-private.h"
#include "cg-texture-private.h"
#include "cg-texture-3d.h"

struct _cg_texture_3d_t {
    cg_texture_t _parent;

    /* The internal format of the texture represented as a
       cg_pixel_format_t */
    cg_pixel_format_t internal_format;
    int depth;
    bool auto_mipmap;
    bool mipmaps_dirty;

    /* TODO: factor out these OpenGL specific members into some form
     * of driver private state. */

    /* The internal format of the GL texture represented as a GL enum */
    GLenum gl_format;
    /* The texture object number */
    GLuint gl_texture;
    GLenum gl_legacy_texobj_min_filter;
    GLenum gl_legacy_texobj_mag_filter;
    GLint gl_legacy_texobj_wrap_mode_s;
    GLint gl_legacy_texobj_wrap_mode_t;
    GLint gl_legacy_texobj_wrap_mode_p;
};

#endif /* __CG_TEXTURE_3D_PRIVATE_H */
