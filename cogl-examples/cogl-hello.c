#include <cogl/cogl.h>
#include <glib.h>
#include <stdio.h>

typedef struct _Data {
    cg_device_t *dev;
    cg_framebuffer_t *fb;
    cg_primitive_t *triangle;
    cg_pipeline_t *pipeline;

    unsigned int redraw_idle;
    bool is_dirty;
    bool draw_ready;
} Data;

static gboolean
paint_cb(void *user_data)
{
    Data *data = user_data;

    data->redraw_idle = 0;
    data->is_dirty = FALSE;
    data->draw_ready = FALSE;

    cg_framebuffer_clear4f(data->fb, CG_BUFFER_BIT_COLOR, 0, 0, 0, 1);
    cg_primitive_draw(data->triangle, data->fb, data->pipeline);
    cg_onscreen_swap_buffers(data->fb);

    return G_SOURCE_REMOVE;
}

static void
maybe_redraw(Data *data)
{
    if (data->is_dirty && data->draw_ready && data->redraw_idle == 0) {
        /* We'll draw on idle instead of drawing immediately so that
         * if Cogl reports multiple dirty rectangles we won't
         * redundantly draw multiple frames */
        data->redraw_idle = g_idle_add(paint_cb, data);
    }
}

static void
frame_event_cb(cg_onscreen_t *onscreen,
               cg_frame_event_t event,
               cg_frame_info_t *info,
               void *user_data)
{
    Data *data = user_data;

    if (event == CG_FRAME_EVENT_SYNC) {
        data->draw_ready = TRUE;
        maybe_redraw(data);
    }
}

static void
dirty_cb(cg_onscreen_t *onscreen,
         const cg_onscreen_dirty_info_t *info,
         void *user_data)
{
    Data *data = user_data;

    data->is_dirty = TRUE;
    maybe_redraw(data);
}

int
main(int argc, char **argv)
{
    Data data;
    cg_onscreen_t *onscreen;
    cg_error_t *error = NULL;
    cg_vertex_p2c4_t triangle_vertices[] = {
        { 0, 0.7, 0xff, 0x00, 0x00, 0xff },
        { -0.7, -0.7, 0x00, 0xff, 0x00, 0xff },
        { 0.7, -0.7, 0x00, 0x00, 0xff, 0xff }
    };
    GSource *cg_source;
    GMainLoop *loop;

    data.redraw_idle = 0;
    data.is_dirty = FALSE;
    data.draw_ready = TRUE;

    data.dev = cg_device_new(NULL, &error);
    if (!data.dev) {
        fprintf(stderr, "Failed to create context: %s\n", error->message);
        return 1;
    }

    onscreen = cg_onscreen_new(data.dev, 640, 480);
    cg_onscreen_show(onscreen);
    data.fb = onscreen;

    cg_onscreen_set_resizable(onscreen, TRUE);

    data.triangle = cg_primitive_new_p2c4(
        data.dev, CG_VERTICES_MODE_TRIANGLES, 3, triangle_vertices);
    data.pipeline = cg_pipeline_new(data.dev);

    cg_source = cg_glib_source_new(data.dev, G_PRIORITY_DEFAULT);

    g_source_attach(cg_source, NULL);

    cg_onscreen_add_frame_callback(
        data.fb, frame_event_cb, &data, NULL); /* destroy notify */
    cg_onscreen_add_dirty_callback(
        data.fb, dirty_cb, &data, NULL); /* destroy notify */

    loop = g_main_loop_new(NULL, TRUE);
    g_main_loop_run(loop);

    return 0;
}
