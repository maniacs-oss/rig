#include <config.h>

#include <stdlib.h>

#include <cogl/cogl-defines.h>

#ifdef CG_HAS_SDL_SUPPORT
#include <cogl/cogl-sdl.h>
#include <SDL.h>
#endif

#include "test-unit.h"
#include "test-utils.h"

#define FB_WIDTH 512
#define FB_HEIGHT 512

static bool cg_test_is_verbose;

cg_device_t *test_dev;
cg_framebuffer_t *test_fb;

static bool
check_flags(TestFlags flags, cg_renderer_t *renderer)
{
    if (flags & TEST_REQUIREMENT_GL &&
        cg_renderer_get_driver(renderer) != CG_DRIVER_GL &&
        cg_renderer_get_driver(renderer) != CG_DRIVER_GL3) {
        return FALSE;
    }

    if (flags & TEST_REQUIREMENT_NPOT &&
        !cg_has_feature(test_dev, CG_FEATURE_ID_TEXTURE_NPOT)) {
        return FALSE;
    }

    if (flags & TEST_REQUIREMENT_TEXTURE_3D &&
        !cg_has_feature(test_dev, CG_FEATURE_ID_TEXTURE_3D)) {
        return FALSE;
    }

    if (flags & TEST_REQUIREMENT_TEXTURE_RG &&
        !cg_has_feature(test_dev, CG_FEATURE_ID_TEXTURE_RG)) {
        return FALSE;
    }

    if (flags & TEST_REQUIREMENT_POINT_SPRITE &&
        !cg_has_feature(test_dev, CG_FEATURE_ID_POINT_SPRITE)) {
        return FALSE;
    }

    if (flags & TEST_REQUIREMENT_PER_VERTEX_POINT_SIZE &&
        !cg_has_feature(test_dev, CG_FEATURE_ID_PER_VERTEX_POINT_SIZE)) {
        return FALSE;
    }

    if (flags & TEST_REQUIREMENT_GLES2_CONTEXT &&
        !cg_has_feature(test_dev, CG_FEATURE_ID_GLES2_CONTEXT)) {
        return FALSE;
    }

    if (flags & TEST_REQUIREMENT_MAP_WRITE &&
        !cg_has_feature(test_dev, CG_FEATURE_ID_MAP_BUFFER_FOR_WRITE)) {
        return FALSE;
    }

    if (flags & TEST_REQUIREMENT_GLSL &&
        !cg_has_feature(test_dev, CG_FEATURE_ID_GLSL)) {
        return FALSE;
    }

    if (flags & TEST_REQUIREMENT_OFFSCREEN &&
        !cg_has_feature(test_dev, CG_FEATURE_ID_OFFSCREEN)) {
        return FALSE;
    }

    if (flags & TEST_REQUIREMENT_FENCE &&
        !cg_has_feature(test_dev, CG_FEATURE_ID_FENCE)) {
        return FALSE;
    }

    if (flags & TEST_KNOWN_FAILURE) {
        return FALSE;
    }

    return TRUE;
}

bool
is_boolean_env_set(const char *variable)
{
    char *val = getenv(variable);
    bool ret;

    if (!val)
        return FALSE;

    if (g_ascii_strcasecmp(val, "1") == 0 ||
        g_ascii_strcasecmp(val, "on") == 0 ||
        g_ascii_strcasecmp(val, "true") == 0)
        ret = TRUE;
    else if (g_ascii_strcasecmp(val, "0") == 0 ||
             g_ascii_strcasecmp(val, "off") == 0 ||
             g_ascii_strcasecmp(val, "false") == 0)
        ret = FALSE;
    else {
        g_critical("Spurious boolean environment variable value (%s=%s)",
                   variable,
                   val);
        ret = TRUE;
    }

    return ret;
}

void
test_utils_init(TestFlags requirement_flags, TestFlags known_failure_flags)
{
    static int counter = 0;
    cg_error_t *error = NULL;
    cg_onscreen_t *onscreen = NULL;
    cg_display_t *display;
    cg_renderer_t *renderer;
    bool missing_requirement;
    bool known_failure;

    if (counter != 0)
        g_critical("We don't support running more than one test at a time\n"
                   "in a single test run due to the state leakage that can\n"
                   "cause subsequent tests to fail.\n"
                   "\n"
                   "If you want to run all the tests you should run\n"
                   "$ make test-report");
    counter++;

    if (is_boolean_env_set("CG_TEST_VERBOSE") || is_boolean_env_set("V"))
        cg_test_is_verbose = TRUE;

    /* NB: This doesn't have any effect since commit 47444dac of glib
     * because the environment variable is read in a magic constructor
     * so it is too late to set them here */
    if (g_getenv("G_DEBUG")) {
        char *debug = g_strconcat(g_getenv("G_DEBUG"), ",fatal-warnings", NULL);
        g_setenv("G_DEBUG", debug, TRUE);
        g_free(debug);
    } else
        g_setenv("G_DEBUG", "fatal-warnings", TRUE);

    g_setenv("CG_X11_SYNC", "1", 0);

    renderer = cg_renderer_new();

#ifdef CG_HAS_SDL_SUPPORT
    cg_sdl_renderer_set_event_type(renderer, SDL_USEREVENT);
#endif

    if (!cg_renderer_connect(renderer, &error))
        c_error("Failed to create a cg_renderer_t: %s", error->message);

    display = cg_display_new(renderer, NULL);
    if (!cg_display_setup(display, &error))
        c_error("Failed to setup a cg_display_t: %s", error->message);

    test_dev = cg_device_new();
    cg_device_set_display(test_dev, display);
    if (!cg_device_connect(test_dev, &error))
        c_error("Failed to create a cg_device_t: %s", error->message);

    missing_requirement = !check_flags(requirement_flags, renderer);
    known_failure = !check_flags(known_failure_flags, renderer);

    if (is_boolean_env_set("CG_TEST_ONSCREEN")) {
        onscreen = cg_onscreen_new(test_dev, 640, 480);
        test_fb = CG_FRAMEBUFFER(onscreen);
    } else {
        cg_offscreen_t *offscreen;
        cg_texture_2d_t *tex =
            cg_texture_2d_new_with_size(test_dev, FB_WIDTH, FB_HEIGHT);
        offscreen = cg_offscreen_new_with_texture(CG_TEXTURE(tex));
        test_fb = CG_FRAMEBUFFER(offscreen);
    }

    if (!cg_framebuffer_allocate(test_fb, &error))
        g_critical("Failed to allocate framebuffer: %s", error->message);

    if (onscreen)
        cg_onscreen_show(onscreen);

    cg_framebuffer_clear4f(test_fb,
                           CG_BUFFER_BIT_COLOR | CG_BUFFER_BIT_DEPTH |
                           CG_BUFFER_BIT_STENCIL,
                           0,
                           0,
                           0,
                           1);

    if (missing_requirement)
        g_print("WARNING: Missing required feature[s] for this test\n");
    else if (known_failure)
        g_print("WARNING: Test is known to fail\n");
}

void
test_utils_fini(void)
{
    if (test_fb)
        cg_object_unref(test_fb);

    if (test_dev)
        cg_object_unref(test_dev);
}

static bool
compare_component(int a, int b)
{
    return ABS(a - b) <= 1;
}

void
test_utils_compare_pixel_and_alpha(const uint8_t *screen_pixel,
                                   uint32_t expected_pixel)
{
    /* Compare each component with a small fuzz factor */
    if (!compare_component(screen_pixel[0], expected_pixel >> 24) ||
        !compare_component(screen_pixel[1], (expected_pixel >> 16) & 0xff) ||
        !compare_component(screen_pixel[2], (expected_pixel >> 8) & 0xff) ||
        !compare_component(screen_pixel[3], (expected_pixel >> 0) & 0xff)) {
        uint32_t screen_pixel_num = GUINT32_FROM_BE(*(uint32_t *)screen_pixel);
        char *screen_pixel_string = g_strdup_printf("#%08x", screen_pixel_num);
        char *expected_pixel_string = g_strdup_printf("#%08x", expected_pixel);

        g_assert_cmpstr(screen_pixel_string, ==, expected_pixel_string);

        g_free(screen_pixel_string);
        g_free(expected_pixel_string);
    }
}

void
test_utils_compare_pixel(const uint8_t *screen_pixel,
                         uint32_t expected_pixel)
{
    /* Compare each component with a small fuzz factor */
    if (!compare_component(screen_pixel[0], expected_pixel >> 24) ||
        !compare_component(screen_pixel[1], (expected_pixel >> 16) & 0xff) ||
        !compare_component(screen_pixel[2], (expected_pixel >> 8) & 0xff)) {
        uint32_t screen_pixel_num = GUINT32_FROM_BE(*(uint32_t *)screen_pixel);
        char *screen_pixel_string =
            g_strdup_printf("#%06x", screen_pixel_num >> 8);
        char *expected_pixel_string =
            g_strdup_printf("#%06x", expected_pixel >> 8);

        g_assert_cmpstr(screen_pixel_string, ==, expected_pixel_string);

        g_free(screen_pixel_string);
        g_free(expected_pixel_string);
    }
}

void
test_utils_check_pixel(cg_framebuffer_t *fb,
                       int x,
                       int y,
                       uint32_t expected_pixel)
{
    uint8_t pixel[4];

    cg_framebuffer_read_pixels(
        fb, x, y, 1, 1, CG_PIXEL_FORMAT_RGBA_8888_PRE, pixel);

    test_utils_compare_pixel(pixel, expected_pixel);
}

void
test_utils_check_pixel_and_alpha(cg_framebuffer_t *fb,
                                 int x,
                                 int y,
                                 uint32_t expected_pixel)
{
    uint8_t pixel[4];

    cg_framebuffer_read_pixels(
        fb, x, y, 1, 1, CG_PIXEL_FORMAT_RGBA_8888_PRE, pixel);

    test_utils_compare_pixel_and_alpha(pixel, expected_pixel);
}

void
test_utils_check_pixel_rgb(
    cg_framebuffer_t *fb, int x, int y, int r, int g, int b)
{
    test_utils_check_pixel(fb, x, y, (r << 24) | (g << 16) | (b << 8));
}

void
test_utils_check_region(cg_framebuffer_t *fb,
                        int x,
                        int y,
                        int width,
                        int height,
                        uint32_t expected_rgba)
{
    uint8_t *pixels, *p;

    pixels = p = g_malloc(width * height * 4);
    cg_framebuffer_read_pixels(
        fb, x, y, width, height, CG_PIXEL_FORMAT_RGBA_8888, p);

    /* Check whether the center of each division is the right color */
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++) {
            test_utils_compare_pixel(p, expected_rgba);
            p += 4;
        }

    g_free(pixels);
}

cg_texture_t *
test_utils_create_color_texture(cg_device_t *dev,
                                uint32_t color)
{
    cg_texture_2d_t *tex_2d;

    color = GUINT32_TO_BE(color);

    tex_2d = cg_texture_2d_new_from_data(dev,
                                         1,
                                         1, /* width/height */
                                         CG_PIXEL_FORMAT_RGBA_8888_PRE,
                                         4, /* rowstride */
                                         (uint8_t *)&color,
                                         NULL);

    return CG_TEXTURE(tex_2d);
}

bool
cg_test_verbose(void)
{
    return cg_test_is_verbose;
}

static void
set_auto_mipmap_cb(cg_texture_t *sub_texture,
                   const float *sub_texture_coords,
                   const float *meta_coords,
                   void *user_data)
{
    cg_primitive_texture_set_auto_mipmap(CG_PRIMITIVE_TEXTURE(sub_texture),
                                         FALSE);
}

cg_texture_t *
test_utils_texture_new_with_size(cg_device_t *dev,
                                 int width,
                                 int height,
                                 TestUtilsTextureFlags flags,
                                 cg_texture_components_t components)
{
    cg_texture_t *tex;
    cg_error_t *skip_error = NULL;

    if ((test_utils_is_pot(width) && test_utils_is_pot(height)) ||
        (cg_has_feature(dev, CG_FEATURE_ID_TEXTURE_NPOT_BASIC) &&
         cg_has_feature(dev, CG_FEATURE_ID_TEXTURE_NPOT_MIPMAP))) {
        /* First try creating a fast-path non-sliced texture */
        tex = CG_TEXTURE(cg_texture_2d_new_with_size(dev, width, height));

        cg_texture_set_components(tex, components);

        if (!cg_texture_allocate(tex, &skip_error)) {
            cg_error_free(skip_error);
            cg_object_unref(tex);
            tex = NULL;
        }
    } else
        tex = NULL;

    if (!tex) {
        /* If it fails resort to sliced textures */
        int max_waste =
            flags & TEST_UTILS_TEXTURE_NO_SLICING ? -1 : CG_TEXTURE_MAX_WASTE;
        cg_texture_2d_sliced_t *tex_2ds =
            cg_texture_2d_sliced_new_with_size(dev, width, height,
                                               max_waste);
        tex = CG_TEXTURE(tex_2ds);

        cg_texture_set_components(tex, components);
    }

    if (flags & TEST_UTILS_TEXTURE_NO_AUTO_MIPMAP) {
        /* To be able to iterate the slices of a #cg_texture_2d_sliced_t we
         * need to ensure the texture is allocated... */
        cg_texture_allocate(tex, NULL); /* don't catch exceptions */

        cg_meta_texture_foreach_in_region(CG_META_TEXTURE(tex),
                                          0,
                                          0,
                                          1,
                                          1,
                                          CG_PIPELINE_WRAP_MODE_CLAMP_TO_EDGE,
                                          CG_PIPELINE_WRAP_MODE_CLAMP_TO_EDGE,
                                          set_auto_mipmap_cb,
                                          NULL); /* don't catch exceptions */
    }

    cg_texture_allocate(tex, NULL);

    return tex;
}

cg_texture_t *
test_utils_texture_new_from_bitmap(cg_bitmap_t *bitmap,
                                   TestUtilsTextureFlags flags,
                                   bool premultiplied)
{
    cg_atlas_texture_t *atlas_tex;
    cg_texture_t *tex;
    cg_error_t *internal_error = NULL;

    if (!flags) {
        /* First try putting the texture in the atlas */
        atlas_tex = cg_atlas_texture_new_from_bitmap(bitmap);

        cg_texture_set_premultiplied(CG_TEXTURE(atlas_tex), premultiplied);

        if (cg_texture_allocate(CG_TEXTURE(atlas_tex), &internal_error))
            return CG_TEXTURE(atlas_tex);

        cg_error_free(internal_error);
        cg_object_unref(atlas_tex);
        internal_error = NULL;
    }

    /* If that doesn't work try a fast path 2D texture */
    if ((test_utils_is_pot(cg_bitmap_get_width(bitmap)) &&
         test_utils_is_pot(cg_bitmap_get_height(bitmap))) ||
        (cg_has_feature(test_dev, CG_FEATURE_ID_TEXTURE_NPOT_BASIC) &&
         cg_has_feature(test_dev, CG_FEATURE_ID_TEXTURE_NPOT_MIPMAP))) {
        tex = CG_TEXTURE(cg_texture_2d_new_from_bitmap(bitmap));

        cg_texture_set_premultiplied(tex, premultiplied);

        if (cg_error_matches(
                internal_error, CG_SYSTEM_ERROR, CG_SYSTEM_ERROR_NO_MEMORY)) {
            g_assert_not_reached();
            return NULL;
        }

        if (!tex) {
            cg_error_free(internal_error);
            internal_error = NULL;
        }
    } else
        tex = NULL;

    if (!tex) {
        /* Otherwise create a sliced texture */
        int max_waste =
            flags & TEST_UTILS_TEXTURE_NO_SLICING ? -1 : CG_TEXTURE_MAX_WASTE;
        cg_texture_2d_sliced_t *tex_2ds =
            cg_texture_2d_sliced_new_from_bitmap(bitmap, max_waste);
        tex = CG_TEXTURE(tex_2ds);

        cg_texture_set_premultiplied(tex, premultiplied);
    }

    if (flags & TEST_UTILS_TEXTURE_NO_AUTO_MIPMAP) {
        cg_meta_texture_foreach_in_region(CG_META_TEXTURE(tex),
                                          0,
                                          0,
                                          1,
                                          1,
                                          CG_PIPELINE_WRAP_MODE_CLAMP_TO_EDGE,
                                          CG_PIPELINE_WRAP_MODE_CLAMP_TO_EDGE,
                                          set_auto_mipmap_cb,
                                          NULL); /* don't catch exceptions */
    }

    cg_texture_allocate(tex, NULL);

    return tex;
}

cg_texture_t *
test_utils_texture_new_from_data(cg_device_t *ctx,
                                 int width,
                                 int height,
                                 TestUtilsTextureFlags flags,
                                 cg_pixel_format_t format,
                                 int rowstride,
                                 const uint8_t *data)
{
    cg_bitmap_t *bmp;
    cg_texture_t *tex;

    g_assert_cmpint(format, !=, CG_PIXEL_FORMAT_ANY);
    g_assert(data != NULL);

    /* Wrap the data into a bitmap */
    bmp = cg_bitmap_new_for_data(
        ctx, width, height, format, rowstride, (uint8_t *)data);

    tex = test_utils_texture_new_from_bitmap(bmp, flags, TRUE);

    cg_object_unref(bmp);

    return tex;
}
