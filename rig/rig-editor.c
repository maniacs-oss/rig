/*
 * Rig
 *
 * UI Engine & Editor
 *
 * Copyright (C) 2013,2014  Intel Corporation.
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
 */

#include <config.h>

#include <stdlib.h>
#include <clib.h>

#include <rut.h>

#include "rut-renderer.h"

#include "rig-engine.h"
#include "rig-avahi.h"
#include "rig-slave-master.h"
#include "rig-inspector.h"

#include "rig.pb-c.h"

#include "components/rig-button-input.h"
#include "components/rig-camera.h"
#include "components/rig-diamond.h"
#include "components/rig-hair.h"
#include "components/rig-light.h"
#include "components/rig-material.h"
#include "components/rig-model.h"
#include "components/rig-nine-slice.h"
#include "components/rig-pointalism-grid.h"
#include "components/rig-shape.h"

static char **_rig_editor_remaining_args = NULL;

static const GOptionEntry _rig_editor_entries[] = {
    { G_OPTION_REMAINING,        0,                           0,
      G_OPTION_ARG_STRING_ARRAY, &_rig_editor_remaining_args, "Project" },
    { 0 }
};

struct _rig_editor_t {
    rut_object_base_t _base;

    rut_shell_t *shell;

    rig_frontend_t *frontend;
    rig_engine_t *engine;

    c_list_t *assets;

    char *ui_filename;

    /* The edit_to_play_object_map hash table lets us map an edit-mode
     * object into a corresponding play-mode object so we can make best
     * effort attempts to apply edit operations to the play-mode ui.
     */
    c_hash_table_t *edit_to_play_object_map;
    c_hash_table_t *play_to_edit_object_map;

    rut_queue_t *edit_ops;

    rig_engine_op_apply_context_t apply_op_ctx;
    rig_engine_op_copy_context_t copy_op_ctx;
    rig_engine_op_map_context_t map_op_ctx;
    rig_engine_op_apply_context_t play_apply_op_ctx;

    rut_ui_viewport_t *search_vp;
    rut_fold_t *search_results_fold;
    rut_box_layout_t *search_results_vbox;
    rut_flow_layout_t *entity_results;
    rut_flow_layout_t *controller_results;
    rut_flow_layout_t *assets_geometry_results;
    rut_flow_layout_t *assets_image_results;
    rut_flow_layout_t *assets_video_results;
    rut_flow_layout_t *assets_other_results;

    rig_asset_t *text_builtin_asset;
    rig_asset_t *circle_builtin_asset;
    rig_asset_t *nine_slice_builtin_asset;
    rig_asset_t *diamond_builtin_asset;
    rig_asset_t *pointalism_grid_builtin_asset;
    rig_asset_t *hair_builtin_asset;
    rig_asset_t *button_input_builtin_asset;
    c_list_t *result_input_closures;
    c_list_t *asset_enumerators;

    rut_adb_device_tracker_t *adb_tracker;
    int next_forward_port;
};

static void
nop_register_id_cb(void *object, uint64_t id, void *user_data)
{
    /* NOP */
}

#if 0
static void *
nop_id_cast_cb (uint64_t id, void *user_data)
{
    return (void *)(uintptr_t)id;
}

static void
nop_unregister_id_cb (uint64_t id, void *user_data)
{
    /* NOP */
}
#endif

static void
log_edit_op_cb(Rig__Operation *pb_op, void *user_data)
{
    rig_editor_t *editor = user_data;

    rut_queue_push_tail(editor->edit_ops, pb_op);
}

static void *
lookup_play_mode_object_cb(uint64_t edit_mode_id, void *user_data)
{
    rig_editor_t *editor = user_data;
    void *edit_mode_object = (void *)(intptr_t)edit_mode_id;
    return c_hash_table_lookup(editor->edit_to_play_object_map,
                               edit_mode_object);
}

static void
register_play_mode_object(rig_editor_t *editor,
                          uint64_t edit_mode_id,
                          void *play_mode_object)
{
    /* NB: in this case we know the ids fit inside a pointer and
     * the hash table keys are pointers
     */

    void *edit_mode_object = (void *)(intptr_t)edit_mode_id;

    c_hash_table_insert(
        editor->edit_to_play_object_map, edit_mode_object, play_mode_object);
    c_hash_table_insert(
        editor->play_to_edit_object_map, play_mode_object, edit_mode_object);
}

static void
register_play_mode_object_cb(void *play_mode_object,
                             uint64_t edit_mode_id,
                             void *user_data)
{
    rig_editor_t *editor = user_data;
    register_play_mode_object(editor, edit_mode_id, play_mode_object);
}

static uint64_t
edit_id_to_play_id(rig_editor_t *editor, uint64_t edit_id)
{
    void *ptr_edit_id = (void *)(intptr_t)edit_id;
    void *ptr_play_id =
        c_hash_table_lookup(editor->edit_to_play_object_map, ptr_edit_id);

    return (uint64_t)(intptr_t)ptr_play_id;
}

static uint64_t
map_id_cb(uint64_t id, void *user_data)
{
    rig_editor_t *editor = user_data;
    return edit_id_to_play_id(editor, id);
}

static rig_asset_t *
share_asset_cb(rig_pb_un_serializer_t *unserializer,
               Rig__Asset *pb_asset,
               void *user_data)
{
    rut_object_t *obj = (rut_object_t *)(intptr_t)pb_asset->id;
    return rut_object_ref(obj);
}

static rig_ui_t *
derive_play_mode_ui(rig_editor_t *editor)
{
    rig_engine_t *engine = editor->engine;
    rig_ui_t *src_ui = engine->edit_mode_ui;
    rig_pb_serializer_t *serializer;
    Rig__UI *pb_ui;
    rig_pb_un_serializer_t *unserializer;
    rig_ui_t *copy;

    rig_engine_set_play_mode_ui(engine, NULL);

    c_warn_if_fail(editor->edit_to_play_object_map == NULL ||
                   c_hash_table_size(editor->edit_to_play_object_map) == 0);
    c_warn_if_fail(editor->play_to_edit_object_map == NULL ||
                   c_hash_table_size(editor->play_to_edit_object_map) == 0);

    editor->edit_to_play_object_map =
        c_hash_table_new(NULL, /* direct hash */
                         NULL); /* direct key equal */
    editor->play_to_edit_object_map =
        c_hash_table_new(NULL, /* direct hash */
                         NULL); /* direct key equal */

    /* For simplicity we use a serializer and unserializer to
     * duplicate the UI, though potentially in the future we may
     * want a more direct way of handling this.
     */
    serializer = rig_pb_serializer_new(engine);

    /* We want to share references to assets between the two UIs
     * since they should be immutable and so we make sure to
     * only keep track of the ids (pointers to assets used) and
     * we will also hook into the corresponding unserialize below
     * to simply return the same objects. */
    rig_pb_serializer_set_only_asset_ids_enabled(serializer, true);

    /* By using pointers instead of an incrementing integer for the
     * object IDs when serializing we can map assets back to the
     * original asset which doesn't need to be copied. */
    rig_pb_serializer_set_use_pointer_ids_enabled(serializer, true);

    pb_ui = rig_pb_serialize_ui(serializer,
                                false, /* play mode */
                                src_ui);

    unserializer = rig_pb_unserializer_new(engine);

    rig_pb_unserializer_set_object_register_callback(
        unserializer, register_play_mode_object_cb, editor);

    rig_pb_unserializer_set_id_to_object_callback(
        unserializer, lookup_play_mode_object_cb, editor);

    rig_pb_unserializer_set_asset_unserialize_callback(
        unserializer, share_asset_cb, NULL);

    copy = rig_pb_unserialize_ui(unserializer, pb_ui);

    rig_pb_unserializer_destroy(unserializer);

    rig_pb_serialized_ui_destroy(pb_ui);

    rig_pb_serializer_destroy(serializer);

    return copy;
}

static void
delete_object_cb(rut_object_t *object, void *user_data)
{
    rig_editor_t *editor = user_data;
    void *edit_mode_object;
    void *play_mode_object;

    edit_mode_object =
        c_hash_table_lookup(editor->play_to_edit_object_map, object);
    if (edit_mode_object)
        play_mode_object = object;
    else {
        play_mode_object =
            c_hash_table_lookup(editor->edit_to_play_object_map, object);

        c_warn_if_fail(play_mode_object);

        edit_mode_object = object;
    }

    c_hash_table_remove(editor->edit_to_play_object_map, edit_mode_object);
    c_hash_table_remove(editor->play_to_edit_object_map, play_mode_object);
}

#ifdef RIG_ENABLE_DEBUG
static void
dump_left_over_object_cb(gpointer key, gpointer value, gpointer user_data)
{
    c_warning("  %s", rig_engine_get_object_debug_name(value));
}
#endif /* RIG_ENABLE_DEBUG */

static void
reset_play_mode_ui(rig_editor_t *editor)
{
    rig_engine_t *engine = editor->engine;
    rig_ui_t *play_mode_ui;
    rut_object_t *play_scene = NULL;

    if (engine->play_mode_ui)
        play_scene = engine->play_mode_ui->scene;

    /* First make sure to cleanup the current ui  */
    rig_engine_set_play_mode_ui(engine, NULL);

    /* Kick garbage collection now so that all the objects being
     * replaced are unregistered before before we load the new UI.
     */
    rig_engine_garbage_collect(engine, delete_object_cb, editor);

    /* As a special case; unregister an object id mapping for the
     * root of the scenegraph (if there was one)... */
    if (play_scene)
        delete_object_cb (play_scene, editor);

#ifdef RIG_ENABLE_DEBUG
    if (editor->edit_to_play_object_map &&
        C_UNLIKELY(c_hash_table_size(editor->edit_to_play_object_map))) {
        c_warning("BUG: The following objects weren't properly unregistered "
                  "by reset_play_mode_ui():");
        c_hash_table_foreach(
            editor->edit_to_play_object_map, dump_left_over_object_cb, NULL);
    }
#endif

    play_mode_ui = derive_play_mode_ui(editor);
    rig_engine_set_play_mode_ui(engine, play_mode_ui);
    rut_object_unref(play_mode_ui);

    /* As a special case; register an object id mapping for the root
     * of the scenegraph... */
    register_play_mode_object(editor,
                              (uint64_t)(uintptr_t)engine->edit_mode_ui->scene,
                              play_mode_ui->scene);

    rig_engine_op_apply_context_set_ui(&editor->play_apply_op_ctx,
                                       play_mode_ui);

    rig_frontend_reload_simulator_ui(
        engine->frontend, engine->play_mode_ui, true); /* play mode */
}

typedef struct _result_input_closure_t {
    rut_object_t *result;
    rig_engine_t *engine;
} result_input_closure_t;

void
rig_editor_free_result_input_closures(rig_editor_t *editor)
{
    c_list_t *l;

    for (l = editor->result_input_closures; l; l = l->next)
        c_slice_free(result_input_closure_t, l->data);
    c_list_free(editor->result_input_closures);
    editor->result_input_closures = NULL;
}

static void
apply_asset_input_with_entity(rig_engine_t *engine,
                              rig_asset_t *asset,
                              rig_entity_t *entity)
{
    rig_editor_t *editor = engine->editor;
    rig_undo_journal_t *sub_journal;
    rig_asset_type_t type = rig_asset_get_type(asset);
    rig_material_t *material;
    rut_object_t *geom;

    rig_editor_push_undo_subjournal(engine);

    switch (type) {
    case RIG_ASSET_TYPE_TEXTURE:
    case RIG_ASSET_TYPE_NORMAL_MAP:
    case RIG_ASSET_TYPE_ALPHA_MASK: {
        material =
            rig_entity_get_component(entity, RUT_COMPONENT_TYPE_MATERIAL);

        if (!material) {
            material = rig_material_new(engine->shell, asset);
            rig_undo_journal_add_component(
                engine->undo_journal, entity, material);
            rut_object_unref(material);
        }

        if (type == RIG_ASSET_TYPE_TEXTURE)
            rig_material_set_color_source_asset(material, asset);
        else if (type == RIG_ASSET_TYPE_NORMAL_MAP)
            rig_material_set_normal_map_asset(material, asset);
        else if (type == RIG_ASSET_TYPE_ALPHA_MASK)
            rig_material_set_alpha_mask_asset(material, asset);

        rut_renderer_notify_entity_changed(engine->renderer, entity);

        geom = rig_entity_get_component(entity, RUT_COMPONENT_TYPE_GEOMETRY);
        if (!geom) {
            int width, height;
            rig_shape_t *shape;
            rig_asset_get_image_size(asset, &width, &height);
            shape = rig_shape_new(engine->shell, true, width, height);
            rig_undo_journal_add_component(engine->undo_journal, entity, shape);
            rut_object_unref(shape);
            geom = shape;
        }

        break;
    }
    case RIG_ASSET_TYPE_MESH: {
        rig_model_t *model;
        float x_range, y_range, z_range, max_range;

        material =
            rig_entity_get_component(entity, RUT_COMPONENT_TYPE_MATERIAL);

        if (!material) {
            material = rig_material_new(engine->shell, asset);
            rig_undo_journal_add_component(
                engine->undo_journal, entity, material);
            rut_object_unref(material);
        }

        geom = rig_entity_get_component(entity, RUT_COMPONENT_TYPE_GEOMETRY);

        if (geom && rut_object_get_type(geom) == &rig_model_type) {
            model = geom;
            if (rig_model_get_asset(model) == asset)
                break;
            else
                rig_undo_journal_delete_component(engine->undo_journal, model);
        } else if (geom)
            rig_undo_journal_delete_component(engine->undo_journal, geom);

        model = rig_model_new_from_asset(engine->shell, asset);
        rig_undo_journal_add_component(engine->undo_journal, entity, model);
        rut_object_unref(model);

        x_range = model->max_x - model->min_x;
        y_range = model->max_y - model->min_y;
        z_range = model->max_z - model->min_z;

        max_range = x_range;
        if (y_range > max_range)
            max_range = y_range;
        if (z_range > max_range)
            max_range = z_range;

        rig_entity_set_scale(entity, 200.0 / max_range);

        rut_renderer_notify_entity_changed(engine->renderer, entity);

        break;
    }
    case RIG_ASSET_TYPE_BUILTIN:
        if (asset == editor->text_builtin_asset) {
            rut_text_t *text;
            cg_color_t color;
            rig_hair_t *hair;

            hair = rig_entity_get_component(entity, RUT_COMPONENT_TYPE_HAIR);

            if (hair)
                rig_undo_journal_delete_component(engine->undo_journal, hair);

            geom =
                rig_entity_get_component(entity, RUT_COMPONENT_TYPE_GEOMETRY);

            if (geom && rut_object_get_type(geom) == &rut_text_type)
                break;
            else if (geom)
                rig_undo_journal_delete_component(engine->undo_journal, geom);

            text = rut_text_new_with_text(engine->shell, "Sans 60px", "text");
            cg_color_init_from_4f(&color, 1, 1, 1, 1);
            rut_text_set_color(text, &color);
            rig_undo_journal_add_component(engine->undo_journal, entity, text);
            rut_object_unref(text);

            rut_renderer_notify_entity_changed(engine->renderer, entity);
        } else if (asset == editor->circle_builtin_asset) {
            rig_shape_t *shape;
            int tex_width = 200, tex_height = 200;

            geom =
                rig_entity_get_component(entity, RUT_COMPONENT_TYPE_GEOMETRY);

            if (geom && rut_object_get_type(geom) == &rig_shape_type)
                break;
            else if (geom)
                rig_undo_journal_delete_component(engine->undo_journal, geom);

            material =
                rig_entity_get_component(entity, RUT_COMPONENT_TYPE_MATERIAL);

            if (material) {
                rig_asset_t *texture_asset =
                    rig_material_get_color_source_asset(material);
                if (texture_asset)
                    rig_asset_get_image_size(
                        texture_asset, &tex_width, &tex_height);
            }

            shape = rig_shape_new(engine->shell, true, tex_width, tex_height);
            rig_undo_journal_add_component(engine->undo_journal, entity, shape);
            rut_object_unref(shape);

            rut_renderer_notify_entity_changed(engine->renderer, entity);
        } else if (asset == editor->diamond_builtin_asset) {
            rig_diamond_t *diamond;

            geom =
                rig_entity_get_component(entity, RUT_COMPONENT_TYPE_GEOMETRY);

            if (geom && rut_object_get_type(geom) == &rig_diamond_type)
                break;
            else if (geom)
                rig_undo_journal_delete_component(engine->undo_journal, geom);

            diamond = rig_diamond_new(engine->shell, 200);
            rig_undo_journal_add_component(
                engine->undo_journal, entity, diamond);
            rut_object_unref(diamond);

            rut_renderer_notify_entity_changed(engine->renderer, entity);
        } else if (asset == editor->nine_slice_builtin_asset) {
            rig_nine_slice_t *nine_slice;
            int tex_width = 200, tex_height = 200;

            geom =
                rig_entity_get_component(entity, RUT_COMPONENT_TYPE_GEOMETRY);

            if (geom && rut_object_get_type(geom) == &rig_nine_slice_type)
                break;
            else if (geom)
                rig_undo_journal_delete_component(engine->undo_journal, geom);

            material =
                rig_entity_get_component(entity, RUT_COMPONENT_TYPE_MATERIAL);

            if (material) {
                rig_asset_t *color_source_asset =
                    rig_material_get_color_source_asset(material);
                if (color_source_asset) {
                    rig_asset_get_image_size(
                        color_source_asset, &tex_width, &tex_height);
                }
            }

            nine_slice = rig_nine_slice_new(
                engine->shell, NULL, 0, 0, 0, 0, tex_width, tex_height);
            rig_undo_journal_add_component(
                engine->undo_journal, entity, nine_slice);
            rut_object_unref(nine_slice);

            rut_renderer_notify_entity_changed(engine->renderer, entity);
        } else if (asset == editor->pointalism_grid_builtin_asset) {
            rig_pointalism_grid_t *grid;

            geom =
                rig_entity_get_component(entity, RUT_COMPONENT_TYPE_GEOMETRY);

            if (geom &&
                rut_object_get_type(geom) == &rig_pointalism_grid_type) {
                break;
            } else if (geom)
                rig_undo_journal_delete_component(engine->undo_journal, geom);

            grid = rig_pointalism_grid_new(engine->shell, 20);

            rig_undo_journal_add_component(engine->undo_journal, entity, grid);
            rut_object_unref(grid);

            rut_renderer_notify_entity_changed(engine->renderer, entity);
        } else if (asset == editor->hair_builtin_asset) {
            rig_hair_t *hair =
                rig_entity_get_component(entity, RUT_COMPONENT_TYPE_HAIR);
            if (hair)
                break;

            hair = rig_hair_new(engine->shell);
            rig_undo_journal_add_component(engine->undo_journal, entity, hair);
            rut_object_unref(hair);
            geom =
                rig_entity_get_component(entity, RUT_COMPONENT_TYPE_GEOMETRY);

            if (geom && rut_object_get_type(geom) == &rig_model_type) {
                rig_model_t *hair_geom = rig_model_new_for_hair(geom);

                rig_hair_set_length(
                    hair, rig_model_get_default_hair_length(hair_geom));

                rig_undo_journal_delete_component(engine->undo_journal, geom);
                rig_undo_journal_add_component(
                    engine->undo_journal, entity, hair_geom);
                rut_object_unref(hair_geom);
            }

            rut_renderer_notify_entity_changed(engine->renderer, entity);
        } else if (asset == editor->button_input_builtin_asset) {
            rig_button_input_t *button_input =
                rig_entity_get_component(entity, RUT_COMPONENT_TYPE_INPUT);
            if (button_input)
                break;

            button_input = rig_button_input_new(engine->shell);
            rig_undo_journal_add_component(
                engine->undo_journal, entity, button_input);
            rut_object_unref(button_input);

            rut_renderer_notify_entity_changed(engine->renderer, entity);
        }
        break;
    }

    sub_journal = rig_editor_pop_undo_subjournal(engine);

    if (rig_undo_journal_is_empty(sub_journal))
        rig_undo_journal_free(sub_journal);
    else
        rig_undo_journal_log_subjournal(engine->undo_journal, sub_journal);
}

static void
apply_result_input_with_entity(rig_entity_t *entity,
                               result_input_closure_t *closure)
{
    if (rut_object_get_type(closure->result) == &rig_asset_type)
        apply_asset_input_with_entity(closure->engine, closure->result, entity);
    else if (rut_object_get_type(closure->result) == &rig_entity_type)
        rig_select_object(
            closure->engine, closure->result, RUT_SELECT_ACTION_REPLACE);
    else if (rut_object_get_type(closure->result) == &rig_controller_type)
        rig_select_object(
            closure->engine, closure->result, RUT_SELECT_ACTION_REPLACE);
}

static rut_input_event_status_t
result_input_cb(rut_input_region_t *region,
                rut_input_event_t *event,
                void *user_data)
{
    result_input_closure_t *closure = user_data;
    rut_input_event_status_t status = RUT_INPUT_EVENT_STATUS_UNHANDLED;

    if (rut_input_event_get_type(event) == RUT_INPUT_EVENT_TYPE_MOTION) {
        if (rut_motion_event_get_action(event) == RUT_MOTION_EVENT_ACTION_UP) {
            rig_engine_t *engine = closure->engine;

            if (engine->objects_selection->objects) {
                c_list_foreach(engine->objects_selection->objects,
                               (GFunc)apply_result_input_with_entity,
                               closure);
            } else {
                rig_entity_t *entity = rig_entity_new(engine->shell);
                rig_undo_journal_add_entity(
                    engine->undo_journal, engine->edit_mode_ui->scene, entity);
                rig_select_object(engine, entity, RUT_SELECT_ACTION_REPLACE);
                apply_result_input_with_entity(entity, closure);
            }

            rig_editor_update_inspector(engine);
            rut_shell_queue_redraw(engine->shell);
            status = RUT_INPUT_EVENT_STATUS_HANDLED;
        }
    }

    return status;
}

void
rig_editor_clear_search_results(rig_editor_t *editor)
{
    if (editor->search_results_vbox) {
        rut_fold_set_child(editor->search_results_fold, NULL);
        rig_editor_free_result_input_closures(editor);

        /* NB: We don't maintain any additional references on asset
         * result widgets beyond the references for them being in the
         * scene graph and so setting a NULL fold child should release
         * everything underneath...
         */

        editor->search_results_vbox = NULL;

        editor->entity_results = NULL;
        editor->controller_results = NULL;
        editor->assets_geometry_results = NULL;
        editor->assets_image_results = NULL;
        editor->assets_video_results = NULL;
        editor->assets_other_results = NULL;
    }
}

static rut_flow_layout_t *
add_results_flow(rut_shell_t *shell, const char *label,
                 rut_box_layout_t *vbox)
{
    rut_flow_layout_t *flow =
        rut_flow_layout_new(shell, RUT_FLOW_LAYOUT_PACKING_LEFT_TO_RIGHT);
    rut_text_t *text = rut_text_new_with_text(shell, "Bold Sans 15px", label);
    cg_color_t color;
    rut_bin_t *label_bin = rut_bin_new(shell);
    rut_bin_t *flow_bin = rut_bin_new(shell);

    rut_bin_set_left_padding(label_bin, 10);
    rut_bin_set_top_padding(label_bin, 10);
    rut_bin_set_bottom_padding(label_bin, 10);
    rut_bin_set_child(label_bin, text);
    rut_object_unref(text);

    rut_color_init_from_uint32(&color, 0xffffffff);
    rut_text_set_color(text, &color);

    rut_box_layout_add(vbox, false, label_bin);
    rut_object_unref(label_bin);

    rut_flow_layout_set_x_padding(flow, 5);
    rut_flow_layout_set_y_padding(flow, 5);
    rut_flow_layout_set_max_child_height(flow, 100);

    // rut_bin_set_left_padding (flow_bin, 5);
    rut_bin_set_child(flow_bin, flow);
    rut_object_unref(flow);

    rut_box_layout_add(vbox, true, flow_bin);
    rut_object_unref(flow_bin);

    return flow;
}

static void
add_search_result(rig_engine_t *engine, rut_object_t *result)
{
    rig_editor_t *editor = engine->editor;
    result_input_closure_t *closure;
    rut_stack_t *stack;
    rut_bin_t *bin;
    cg_texture_t *texture;
    rut_input_region_t *region;
    rut_drag_bin_t *drag_bin;

    closure = c_slice_new(result_input_closure_t);
    closure->result = result;
    closure->engine = engine;

    bin = rut_bin_new(engine->shell);

    drag_bin = rut_drag_bin_new(engine->shell);
    rut_drag_bin_set_payload(drag_bin, result);
    rut_bin_set_child(bin, drag_bin);
    rut_object_unref(drag_bin);

    stack = rut_stack_new(engine->shell, 0, 0);
    rut_drag_bin_set_child(drag_bin, stack);
    rut_object_unref(stack);

    region = rut_input_region_new_rectangle(
        0, 0, 100, 100, result_input_cb, closure);
    rut_stack_add(stack, region);
    rut_object_unref(region);

    if (rut_object_get_type(result) == &rig_asset_type) {
        rig_asset_t *asset = result;

        texture = rig_asset_get_texture(asset);

        if (texture) {
            rut_image_t *image = rut_image_new(engine->shell, texture);
            rut_stack_add(stack, image);
            rut_object_unref(image);
        } else {
            char *basename = g_path_get_basename(rig_asset_get_path(asset));
            rut_text_t *text =
                rut_text_new_with_text(engine->shell, NULL, basename);
            rut_stack_add(stack, text);
            rut_object_unref(text);
            c_free(basename);
        }
    } else if (rut_object_get_type(result) == &rig_entity_type) {
        rig_entity_t *entity = result;
        rut_box_layout_t *vbox = rut_box_layout_new(
            engine->shell, RUT_BOX_LAYOUT_PACKING_TOP_TO_BOTTOM);
        rut_image_t *image;
        rut_text_t *text;

        rut_stack_add(stack, vbox);
        rut_object_unref(vbox);

#warning "Create a sensible icon to represent entities"
        texture = rut_load_texture_from_data_file(
            engine->shell, "transparency-grid.png", NULL);
        image = rut_image_new(engine->shell, texture);
        cg_object_unref(texture);

        rut_box_layout_add(vbox, false, image);
        rut_object_unref(image);

        text = rut_text_new_with_text(engine->shell, NULL, entity->label);
        rut_box_layout_add(vbox, false, text);
        rut_object_unref(text);
    } else if (rut_object_get_type(result) == &rig_controller_type) {
        rig_controller_t *controller = result;
        rut_box_layout_t *vbox = rut_box_layout_new(
            engine->shell, RUT_BOX_LAYOUT_PACKING_TOP_TO_BOTTOM);
        rut_image_t *image;
        rut_text_t *text;

        rut_stack_add(stack, vbox);
        rut_object_unref(vbox);

#warning "Create a sensible icon to represent controllers"
        texture = rut_load_texture_from_data_file(
            engine->shell, "transparency-grid.png", NULL);
        image = rut_image_new(engine->shell, texture);
        cg_object_unref(texture);

        rut_box_layout_add(vbox, false, image);
        rut_object_unref(image);

        text = rut_text_new_with_text(engine->shell, NULL, controller->label);
        rut_box_layout_add(vbox, false, text);
        rut_object_unref(text);
    }

    if (rut_object_get_type(result) == &rig_asset_type) {
        rig_asset_t *asset = result;

        if (rig_asset_has_tag(asset, "geometry")) {
            if (!editor->assets_geometry_results) {
                editor->assets_geometry_results = add_results_flow(
                    engine->shell, "Geometry", editor->search_results_vbox);
            }

            rut_flow_layout_add(editor->assets_geometry_results, bin);
            rut_object_unref(bin);
        } else if (rig_asset_has_tag(asset, "image")) {
            if (!editor->assets_image_results) {
                editor->assets_image_results = add_results_flow(
                    engine->shell, "Images", editor->search_results_vbox);
            }

            rut_flow_layout_add(editor->assets_image_results, bin);
            rut_object_unref(bin);
        } else if (rig_asset_has_tag(asset, "video")) {
            if (!editor->assets_video_results) {
                editor->assets_video_results = add_results_flow(
                    engine->shell, "Video", editor->search_results_vbox);
            }

            rut_flow_layout_add(editor->assets_video_results, bin);
            rut_object_unref(bin);
        } else {
            if (!editor->assets_other_results) {
                editor->assets_other_results = add_results_flow(
                    engine->shell, "Other", editor->search_results_vbox);
            }

            rut_flow_layout_add(editor->assets_other_results, bin);
            rut_object_unref(bin);
        }
    } else if (rut_object_get_type(result) == &rig_entity_type) {
        if (!editor->entity_results) {
            editor->entity_results = add_results_flow(
                engine->shell, "Entity", editor->search_results_vbox);
        }

        rut_flow_layout_add(editor->entity_results, bin);
        rut_object_unref(bin);
    } else if (rut_object_get_type(result) == &rig_controller_type) {
        if (!editor->controller_results) {
            editor->controller_results = add_results_flow(
                engine->shell, "Controllers", editor->search_results_vbox);
        }

        rut_flow_layout_add(editor->controller_results, bin);
        rut_object_unref(bin);
    }

    /* XXX: It could be nicer to have some form of weak pointer
     * mechanism to manage the lifetime of these closures... */
    editor->result_input_closures =
        c_list_prepend(editor->result_input_closures, closure);
}

typedef struct _search_state_t {
    rig_engine_t *engine;
    const char *search;
    bool found;
} search_state_t;

static rut_traverse_visit_flags_t
add_matching_entity_cb(rut_object_t *object, int depth, void *user_data)
{
    if (rut_object_get_type(object) == &rig_entity_type) {
        rig_entity_t *entity = object;
        search_state_t *state = user_data;

        if (state->search == NULL) {
            state->found = true;
            add_search_result(state->engine, entity);
        } else if (entity->label && strncmp(entity->label, "rig:", 4) != 0) {
            char *entity_label = c_ascii_strdown(entity->label, -1);
#warning "FIXME: handle utf8 string comparisons!"

            if (strstr(entity_label, state->search)) {
                state->found = true;
                add_search_result(state->engine, entity);
            }

            c_free(entity_label);
        }
    }
    return RUT_TRAVERSE_VISIT_CONTINUE;
}

static void
add_matching_controller(rig_controller_t *controller,
                        search_state_t *state)
{
    char *controller_label = c_ascii_strdown(controller->label, -1);
#warning "FIXME: handle utf8 string comparisons!"

    if (state->search == NULL || strstr(controller_label, state->search)) {
        state->found = true;
        add_search_result(state->engine, controller);
    }

    c_free(controller_label);
}

static bool
asset_matches_search(rig_engine_t *engine,
                     rig_asset_t *asset,
                     const char *search)
{
    c_list_t *l;
    bool found = false;
    const c_list_t *inferred_tags;
    char **tags;
    const char *path;
    int i;

    for (l = engine->required_search_tags; l; l = l->next) {
        if (rig_asset_has_tag(asset, l->data)) {
            found = true;
            break;
        }
    }

    if (engine->required_search_tags && found == false)
        return false;

    if (!search)
        return true;

    inferred_tags = rig_asset_get_inferred_tags(asset);
    tags = c_strsplit_set(search, " \t", 0);

    path = rig_asset_get_path(asset);
    if (path) {
        if (strstr(path, search))
            return true;
    }

    for (i = 0; tags[i]; i++) {
        const c_list_t *l;
        bool found = false;

        for (l = inferred_tags; l; l = l->next) {
            if (strcmp(tags[i], l->data) == 0) {
                found = true;
                break;
            }
        }

        if (!found) {
            c_strfreev(tags);
            return false;
        }
    }

    c_strfreev(tags);
    return true;
}

static bool
rig_search_with_text(rig_engine_t *engine, const char *user_search)
{
    rig_editor_t *editor = engine->editor;
    c_list_t *l;
    int i;
    bool found = false;
    search_state_t state;
    char *search;

    if (user_search)
        search = c_ascii_strdown(user_search, -1);
    else
        search = NULL;
#warning "FIXME: handle non-ascii searches!"

    rig_editor_clear_search_results(editor);

    editor->search_results_vbox =
        rut_box_layout_new(engine->shell,
                           RUT_BOX_LAYOUT_PACKING_TOP_TO_BOTTOM);
    rut_fold_set_child(editor->search_results_fold,
                       editor->search_results_vbox);
    rut_object_unref(editor->search_results_vbox);

    for (l = editor->assets, i = 0; l; l = l->next, i++) {
        rig_asset_t *asset = l->data;

        if (!asset_matches_search(engine, asset, search))
            continue;

        found = true;
        add_search_result(engine, asset);
    }

    state.engine = engine;
    state.search = search;
    state.found = false;

    if (!engine->required_search_tags ||
        rut_util_find_tag(engine->required_search_tags, "entity")) {
        rut_graphable_traverse(engine->edit_mode_ui->scene,
                               RUT_TRAVERSE_DEPTH_FIRST,
                               add_matching_entity_cb,
                               NULL, /* post visit */
                               &state);
    }

    if (!engine->required_search_tags ||
        rut_util_find_tag(engine->required_search_tags, "controller")) {
        for (l = engine->edit_mode_ui->controllers; l; l = l->next)
            add_matching_controller(l->data, &state);
    }

    c_free(search);

    if (!engine->required_search_tags)
        return found | state.found;
    else {
        /* If the user has toggled on certain search
         * tag constraints then we don't want to
         * fallback to matching everything when there
         * are no results from the search so we
         * always claim that something was found...
         */
        return true;
    }
}

static void
rig_run_search(rig_engine_t *engine)
{
    if (!rig_search_with_text(engine, rut_text_get_text(engine->search_text)))
        rig_search_with_text(engine, NULL);
}

void
rig_editor_refresh_thumbnails(rig_asset_t *video, void *user_data)
{
    rig_run_search(user_data);
}

static void
asset_search_update_cb(rut_text_t *text, void *user_data)
{
    rig_run_search(user_data);
}

static void
add_asset(rig_engine_t *engine, GFileInfo *info, GFile *asset_file)
{
    rig_editor_t *editor = engine->editor;
    GFile *assets_dir = g_file_new_for_path(engine->shell->assets_location);
    char *path = g_file_get_relative_path(assets_dir, asset_file);
    c_list_t *l;
    rig_asset_t *asset = NULL;
    rut_exception_t *catch = NULL;

    /* Avoid loading duplicate assets... */
    for (l = editor->assets; l; l = l->next) {
        rig_asset_t *existing = l->data;

        if (strcmp(rig_asset_get_path(existing), path) == 0)
            return;
    }

    asset = rig_asset_new_from_file(engine, info, asset_file, &catch);
    if (!asset) {
        c_warning(
            "Failed to load asset from file %s: %s", path, catch->message);
        rut_exception_free(catch);
    } else {
        editor->assets = c_list_prepend(editor->assets, asset);
    }
}

#if 0
static c_list_t *
copy_tags (c_list_t *tags)
{
    c_list_t *l, *copy = NULL;
    for (l = tags; l; l = l->next)
    {
        char *tag = c_intern_string (l->data);
        copy = c_list_prepend (copy, tag);
    }
    return copy;
}
#endif

static void enumerate_dir_for_assets(rig_engine_t *engine, GFile *directory);

void
enumerate_file_info(rig_engine_t *engine, GFile *parent, GFileInfo *info)
{
    GFileType type = g_file_info_get_file_type(info);
    const char *name = g_file_info_get_name(info);

    if (name[0] == '.')
        return;

    if (type == G_FILE_TYPE_DIRECTORY) {
        GFile *directory = g_file_get_child(parent, name);

        enumerate_dir_for_assets(engine, directory);

        g_object_unref(directory);
    } else if (type == G_FILE_TYPE_REGULAR ||
               type == G_FILE_TYPE_SYMBOLIC_LINK) {
        if (rut_file_info_is_asset(info, name)) {
            GFile *image = g_file_get_child(parent, name);
            add_asset(engine, info, image);
            g_object_unref(image);
        }
    }
}

#ifdef USE_ASYNC_IO
typedef struct _asset_enumerator_state_t {
    rig_engine_t *engine;
    GFile *directory;
    GFileEnumerator *enumerator;
    GCancellable *cancellable;
    c_list_t *tags;
} asset_enumerator_state_t;

static void
cleanup_assets_enumerator(asset_enumerator_state_t *state)
{
    if (state->enumerator)
        g_object_unref(state->enumerator);

    g_object_unref(state->cancellable);
    g_object_unref(state->directory);
    c_list_free(state->tags);

    state->engine->asset_enumerators =
        c_list_remove(state->engine->asset_enumerators, state);

    c_slice_free(asset_enumerator_state_t, state);
}

static void
assets_found_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    asset_enumerator_state_t *state = user_data;
    c_list_t *infos;
    c_list_t *l;

    infos = g_file_enumerator_next_files_finish(state->enumerator, res, NULL);
    if (!infos) {
        cleanup_assets_enumerator(state);
        return;
    }

    for (l = infos; l; l = l->next)
        enumerate_file_info(state->engine, state->directory, l->data);

    c_list_free(infos);

    g_file_enumerator_next_files_async(state->enumerator,
                                       5, /* what's a good number here? */
                                       G_PRIORITY_DEFAULT,
                                       state->cancellable,
                                       asset_found_cb,
                                       state);
}

static void
assets_enumerator_cb(GObject *source_object,
                     GAsyncResult *res,
                     gpointer user_data)
{
    asset_enumerator_state_t *state = user_data;
    GError *error = NULL;

    state->enumerator =
        g_file_enumerate_children_finish(state->directory, res, &error);
    if (!state->enumerator) {
        c_warning("Error while looking for assets: %s", error->message);
        g_error_free(error);
        cleanup_assets_enumerator(state);
        return;
    }

    g_file_enumerator_next_files_async(state->enumerator,
                                       5, /* what's a good number here? */
                                       G_PRIORITY_DEFAULT,
                                       state->cancellable,
                                       assets_found_cb,
                                       state);
}

static void
enumerate_dir_for_assets_async(rig_engine_t *engine,
                               GFile *directory)
{
    asset_enumerator_state_t *state = c_slice_new0(asset_enumerator_state_t);

    state->engine = engine;
    state->directory = g_object_ref(directory);

    state->cancellable = g_cancellable_new();

    /* NB: we can only use asynchronous IO if we are running with a Glib
     * mainloop */
    g_file_enumerate_children_async(file,
                                    "standard::*",
                                    G_FILE_QUERY_INFO_NONE,
                                    G_PRIORITY_DEFAULT,
                                    state->cancellable,
                                    assets_enumerator_cb,
                                    engine);

    engine->asset_enumerators =
        c_list_prepend(engine->asset_enumerators, state);
}

#else /* USE_ASYNC_IO */

static void
enumerate_dir_for_assets(rig_engine_t *engine, GFile *file)
{
    GFileEnumerator *enumerator;
    GError *error = NULL;
    GFileInfo *file_info;

    enumerator = g_file_enumerate_children(
        file, "standard::*", G_FILE_QUERY_INFO_NONE, NULL, &error);
    if (!enumerator) {
        char *path = g_file_get_path(file);
        c_warning(
            "Failed to enumerator assets dir %s: %s", path, error->message);
        c_free(path);
        g_error_free(error);
        return;
    }

    while (
        (file_info = g_file_enumerator_next_file(enumerator, NULL, &error))) {
        enumerate_file_info(engine, file, file_info);
    }

    g_object_unref(enumerator);
}
#endif /* USE_ASYNC_IO */

static void
load_asset_list(rig_editor_t *editor)
{
    rig_engine_t *engine = editor->engine;
    GFile *assets_dir = g_file_new_for_path(engine->shell->assets_location);

    enumerate_dir_for_assets(engine, assets_dir);

    rut_object_ref(editor->nine_slice_builtin_asset);
    editor->assets =
        c_list_prepend(editor->assets, editor->nine_slice_builtin_asset);

    rut_object_ref(editor->diamond_builtin_asset);
    editor->assets =
        c_list_prepend(editor->assets, editor->diamond_builtin_asset);

    rut_object_ref(editor->circle_builtin_asset);
    editor->assets =
        c_list_prepend(editor->assets, editor->circle_builtin_asset);

    rut_object_ref(editor->pointalism_grid_builtin_asset);
    editor->assets =
        c_list_prepend(editor->assets, editor->pointalism_grid_builtin_asset);

    rut_object_ref(editor->text_builtin_asset);
    editor->assets = c_list_prepend(editor->assets, editor->text_builtin_asset);

    rut_object_ref(editor->hair_builtin_asset);
    editor->assets = c_list_prepend(editor->assets, editor->hair_builtin_asset);

    rut_object_ref(editor->button_input_builtin_asset);
    editor->assets =
        c_list_prepend(editor->assets, editor->button_input_builtin_asset);

    g_object_unref(assets_dir);

    rig_run_search(engine);
}

/* These should be sorted in descending order of size to
 * avoid gaps due to attributes being naturally aligned. */
static rut_ply_attribute_t ply_attributes[] = {
    { .name = "cg_position_in",
      .properties = { { "x" }, { "y" }, { "z" }, },
      .n_properties = 3,
      .min_components = 1, },
    { .name = "cg_normal_in",
      .properties = { { "nx" }, { "ny" }, { "nz" }, },
      .n_properties = 3,
      .min_components = 3,
      .pad_n_components = 3,
      .pad_type = RUT_ATTRIBUTE_TYPE_FLOAT, },
    { .name = "cg_tex_coord0_in",
      .properties = { { "s" }, { "t" }, { "r" }, },
      .n_properties = 3,
      .min_components = 2,
      .pad_n_components = 3,
      .pad_type = RUT_ATTRIBUTE_TYPE_FLOAT, },
    { .name = "tangent_in",
      .properties = { { "tanx" }, { "tany" }, { "tanz" } },
      .n_properties = 3,
      .min_components = 3,
      .pad_n_components = 3,
      .pad_type = RUT_ATTRIBUTE_TYPE_FLOAT, },
    { .name = "cg_color_in",
      .properties = { { "red" }, { "green" }, { "blue" }, { "alpha" } },
      .n_properties = 4,
      .normalized = true,
      .min_components = 3, }
};

static void
add_light_handle(rig_engine_t *engine, rig_ui_t *ui)
{
    // rig_camera_t *camera =
    //  rig_entity_get_component (ui->light, RUT_COMPONENT_TYPE_CAMERA);
    rut_ply_attribute_status_t padding_status[C_N_ELEMENTS(ply_attributes)];
    char *full_path = rut_find_data_file("light.ply");
    c_error_t *error = NULL;
    rut_mesh_t *mesh;

    if (full_path == NULL)
        c_critical("could not find model \"light.ply\"");

    mesh = rut_mesh_new_from_ply(engine->shell,
                                 full_path,
                                 ply_attributes,
                                 C_N_ELEMENTS(ply_attributes),
                                 padding_status,
                                 &error);
    if (mesh) {
        rig_model_t *model =
            rig_model_new_from_asset_mesh(engine->shell, mesh, false, false);
        rig_material_t *material = rig_material_new(engine->shell, NULL);

        engine->light_handle = rig_entity_new(engine->shell);
        rig_entity_set_label(engine->light_handle, "rig:light_handle");
        rig_entity_set_scale(engine->light_handle, 100);
        rut_graphable_add_child(ui->light, engine->light_handle);

        rig_entity_add_component(engine->light_handle, model);

        rig_entity_add_component(engine->light_handle, material);
        rig_material_set_receive_shadow(material, false);
        rig_material_set_cast_shadow(material, false);

        rut_object_unref(model);
        rut_object_unref(material);
    } else {
        c_critical("could not load model %s: %s", full_path, error->message);
        c_error_free(error);
    }

    c_free(full_path);
}

static void
add_play_camera_handle(rig_engine_t *engine, rig_ui_t *ui)
{
    rut_ply_attribute_status_t padding_status[C_N_ELEMENTS(ply_attributes)];
    char *model_path;
    rut_mesh_t *mesh;
    c_error_t *error = NULL;

    model_path = rut_find_data_file("camera-model.ply");
    if (model_path == NULL) {
        c_error("could not find model \"camera-model.ply\"");
        return;
    }

    mesh = rut_mesh_new_from_ply(engine->shell,
                                 model_path,
                                 ply_attributes,
                                 C_N_ELEMENTS(ply_attributes),
                                 padding_status,
                                 &error);
    if (mesh == NULL) {
        c_critical("could not load model %s: %s", model_path, error->message);
        c_clear_error(&error);
    } else {
/* XXX: we'd like to show a model for the camera that
 * can be used as a handle to select the camera in the
 * editor but for the camera model tends to get in the
 * way of editing so it's been disable for now */
#if 0
        rig_model_t *model = rig_model_new_from_mesh (engine->shell, mesh);
        rig_material_t *material = rig_material_new (engine->shell, NULL);

        engine->play_camera_handle = rig_entity_new (engine->shell);
        rig_entity_set_label (engine->play_camera_handle,
                              "rig:play_camera_handle");

        rig_entity_add_component (engine->play_camera_handle,
                                  model);

        rig_entity_add_component (engine->play_camera_handle,
                                  material);
        rig_material_set_receive_shadow (material, false);
        rig_material_set_cast_shadow (material, false);
        rut_graphable_add_child (engine->play_camera,
                                 engine->play_camera_handle);

        rut_object_unref (model);
        rut_object_unref (material);
        rut_object_unref (mesh);
#endif
    }
}

static void
on_ui_load_cb(void *user_data)
{
    rig_editor_t *editor = user_data;
    rig_engine_t *engine = editor->engine;
    rig_ui_t *ui = engine->edit_mode_ui;

    /* TODO: move controller_view into rig_editor_t */

    rig_controller_view_update_controller_list(engine->controller_view);

    rig_controller_view_set_controller(engine->controller_view,
                                       ui->controllers->data);

    engine->grid_prim = rut_create_create_grid(engine->shell,
                                               engine->device_width,
                                               engine->device_height, 100,
                                               100);

    load_asset_list(editor);

    add_light_handle(engine, ui);
    add_play_camera_handle(engine, ui);

    rig_engine_op_apply_context_set_ui(&editor->apply_op_ctx, ui);

    /* Whenever we replace the edit mode graph that implies we need
     * to scrap and update the play mode graph, with a snapshot of
     * the new edit mode graph.
     */
    reset_play_mode_ui(editor);
}

static void
simulator_connected_cb(void *user_data)
{
    rig_editor_t *editor = user_data;
    rig_engine_t *engine = editor->engine;
    rig_frontend_t *frontend = editor->frontend;

    /* Note: as opposed to letting the simulator copy the edit mode
     * UI itself to create a play mode UI we explicitly serialize
     * both the edit and play mode UIs so we can forward pointer ids
     * for all objects in both UIs...
     */

    rig_frontend_reload_simulator_ui(frontend, engine->edit_mode_ui, false);

    /* Whenever we connect to the simulator that implies we need to
     * update the play mode graph, with a snapshot of the edit mode
     * graph.
     */
    reset_play_mode_ui(editor);
}

static rig_nine_slice_t *
load_gradient_image(rut_shell_t *shell,
                    const char *filename)
{
    c_error_t *error = NULL;
    cg_texture_t *gradient =
        rut_load_texture_from_data_file(shell, filename, &error);
    if (gradient) {
        return rig_nine_slice_new(shell, gradient, 0, 0, 0, 0, 0, 0);
    } else {
        c_error("Failed to load gradient %s: %s", filename, error->message);
        c_error_free(error);
        return NULL;
    }
}

static void
connect_pressed_cb(rut_icon_button_t *button, void *user_data)
{
    rig_engine_t *engine = user_data;
    c_list_t *l;

    for (l = engine->slave_addresses; l; l = l->next)
        rig_connect_to_slave(engine, l->data);
}

static void
create_top_bar(rig_engine_t *engine)
{
    rut_stack_t *top_bar_stack = rut_stack_new(engine->shell, 123, 0);
    rut_icon_button_t *connect_button =
        rut_icon_button_new(engine->shell,
                            NULL,
                            RUT_ICON_BUTTON_POSITION_BELOW,
                            "connect.png",
                            "connect.png",
                            "connect-white.png",
                            "connect.png");
    rut_icon_t *icon = rut_icon_new(engine->shell, "settings-icon.png");
    rig_nine_slice_t *gradient =
        load_gradient_image(engine->shell, "top-bar-gradient.png");

    rut_box_layout_add(engine->top_vbox, false, top_bar_stack);

    rut_stack_add(top_bar_stack, gradient);
    rut_object_unref(gradient);

    engine->top_bar_hbox =
        rut_box_layout_new(engine->shell, RUT_BOX_LAYOUT_PACKING_LEFT_TO_RIGHT);
    engine->top_bar_hbox_ltr =
        rut_box_layout_new(engine->shell, RUT_BOX_LAYOUT_PACKING_LEFT_TO_RIGHT);
    rut_box_layout_add(engine->top_bar_hbox, true, engine->top_bar_hbox_ltr);

    engine->top_bar_hbox_rtl =
        rut_box_layout_new(engine->shell, RUT_BOX_LAYOUT_PACKING_RIGHT_TO_LEFT);
    rut_box_layout_add(engine->top_bar_hbox, true, engine->top_bar_hbox_rtl);

    rut_box_layout_add(engine->top_bar_hbox_rtl, false, icon);

    rut_stack_add(top_bar_stack, engine->top_bar_hbox);

    rut_icon_button_add_on_click_callback(connect_button,
                                          connect_pressed_cb,
                                          engine,
                                          NULL); /* destroy callback */
    rut_box_layout_add(engine->top_bar_hbox_ltr, false, connect_button);
    rut_object_unref(connect_button);
}
static void
create_camera_view(rig_engine_t *engine)
{
    rut_stack_t *stack = rut_stack_new(engine->shell, 0, 0);
    rut_bin_t *bin = rut_bin_new(engine->shell);
    rig_nine_slice_t *gradient =
        load_gradient_image(engine->shell, "document-bg-gradient.png");
    cg_texture_t *left_drop_shadow;
    cg_texture_t *bottom_drop_shadow;
    rut_box_layout_t *hbox =
        rut_box_layout_new(engine->shell, RUT_BOX_LAYOUT_PACKING_LEFT_TO_RIGHT);
    rut_box_layout_t *vbox =
        rut_box_layout_new(engine->shell, RUT_BOX_LAYOUT_PACKING_TOP_TO_BOTTOM);
    rig_nine_slice_t *left_drop;
    rut_stack_t *left_stack;
    rut_bin_t *left_shim;
    rig_nine_slice_t *bottom_drop;
    rut_stack_t *bottom_stack;
    rut_bin_t *bottom_shim;

    rut_stack_add(stack, gradient);
    rut_stack_add(stack, bin);

    engine->main_camera_view = rig_camera_view_new(engine);

    left_drop_shadow = rut_load_texture_from_data_file(
        engine->shell, "left-drop-shadow.png", NULL);
    bottom_drop_shadow = rut_load_texture_from_data_file(
        engine->shell, "bottom-drop-shadow.png", NULL);

    /* Instead of creating one big drop-shadow that extends
     * underneath the document we simply create a thin drop
     * shadow for the left and bottom where the shadow is
     * actually visible...
     */

    left_drop = rig_nine_slice_new(engine->shell,
                                   left_drop_shadow,
                                   10 /* top */,
                                   0, /* right */
                                   10, /* bottom */
                                   0, /* left */
                                   0,
                                   0);
    left_stack = rut_stack_new(engine->shell, 0, 0);
    left_shim = rut_bin_new(engine->shell);
    bottom_drop =
        rig_nine_slice_new(engine->shell, bottom_drop_shadow, 0, 10, 0, 0, 0, 0);
    bottom_stack = rut_stack_new(engine->shell, 0, 0);
    bottom_shim = rut_bin_new(engine->shell);

    rut_bin_set_left_padding(left_shim, 10);
    rut_bin_set_bottom_padding(bottom_shim, 10);

    rut_bin_set_child(bin, hbox);
    rut_box_layout_add(hbox, false, left_stack);

    rut_stack_add(left_stack, left_shim);
    rut_stack_add(left_stack, left_drop);

    rut_box_layout_add(hbox, true, vbox);
    rut_box_layout_add(vbox, true, engine->main_camera_view);
    rut_box_layout_add(vbox, false, bottom_stack);

    rut_stack_add(bottom_stack, bottom_shim);
    rut_stack_add(bottom_stack, bottom_drop);

    rut_bin_set_top_padding(bin, 5);

    rut_box_layout_add(engine->asset_panel_hbox, true, stack);

    rut_object_unref(bottom_shim);
    rut_object_unref(bottom_stack);
    rut_object_unref(bottom_drop);

    rut_object_unref(left_shim);
    rut_object_unref(left_stack);
    rut_object_unref(left_drop);

    cg_object_unref(bottom_drop_shadow);
    cg_object_unref(left_drop_shadow);

    rut_object_unref(vbox);
    rut_object_unref(hbox);
    rut_object_unref(gradient);
    rut_object_unref(bin);
    rut_object_unref(stack);
}

static void
tool_changed_cb(rut_icon_toggle_set_t *toggle_set,
                int selection,
                void *user_data)
{
    rig_engine_t *engine = user_data;
    rut_closure_list_invoke(&engine->tool_changed_cb_list,
                            rig_tool_changed_callback_t,
                            user_data,
                            selection);
}

void
rig_add_tool_changed_callback(rig_engine_t *engine,
                              rig_tool_changed_callback_t callback,
                              void *user_data,
                              rut_closure_destroy_callback_t destroy_notify)
{
    rut_closure_list_add(
        &engine->tool_changed_cb_list, callback, user_data, destroy_notify);
}

static void
create_toolbar(rig_engine_t *engine)
{
    rut_stack_t *stack = rut_stack_new(engine->shell, 0, 0);
    rig_nine_slice_t *gradient =
        load_gradient_image(engine->shell, "toolbar-bg-gradient.png");
    rut_icon_t *icon = rut_icon_new(engine->shell, "chevron-icon.png");
    rut_bin_t *bin = rut_bin_new(engine->shell);
    rut_icon_toggle_t *pointer_toggle;
    rut_icon_toggle_t *rotate_toggle;
    rut_icon_toggle_set_t *toggle_set;

    rut_stack_add(stack, gradient);
    rut_object_unref(gradient);

    engine->toolbar_vbox =
        rut_box_layout_new(engine->shell, RUT_BOX_LAYOUT_PACKING_TOP_TO_BOTTOM);
    rut_bin_set_child(bin, engine->toolbar_vbox);

    rut_bin_set_left_padding(bin, 5);
    rut_bin_set_right_padding(bin, 5);
    rut_bin_set_top_padding(bin, 5);

    rut_box_layout_add(engine->toolbar_vbox, false, icon);

    pointer_toggle =
        rut_icon_toggle_new(engine->shell, "pointer-white.png", "pointer.png");
    rotate_toggle =
        rut_icon_toggle_new(engine->shell, "rotate-white.png", "rotate.png");
    toggle_set = rut_icon_toggle_set_new(
        engine->shell, RUT_ICON_TOGGLE_SET_PACKING_TOP_TO_BOTTOM);
    rut_icon_toggle_set_add(toggle_set, pointer_toggle, RIG_TOOL_ID_SELECTION);
    rut_object_unref(pointer_toggle);
    rut_icon_toggle_set_add(toggle_set, rotate_toggle, RIG_TOOL_ID_ROTATION);
    rut_object_unref(rotate_toggle);

    rut_icon_toggle_set_set_selection(toggle_set, RIG_TOOL_ID_SELECTION);

    rut_icon_toggle_set_add_on_change_callback(
        toggle_set, tool_changed_cb, engine, NULL); /* destroy notify */

    rut_box_layout_add(engine->toolbar_vbox, false, toggle_set);
    rut_object_unref(toggle_set);

    rut_stack_add(stack, bin);

    rut_box_layout_add(engine->top_hbox, false, stack);
}

static void
create_properties_bar(rig_engine_t *engine)
{
    rut_stack_t *stack0 = rut_stack_new(engine->shell, 0, 0);
    rut_stack_t *stack1 = rut_stack_new(engine->shell, 0, 0);
    rut_bin_t *bin = rut_bin_new(engine->shell);
    rig_nine_slice_t *gradient =
        load_gradient_image(engine->shell, "document-bg-gradient.png");
    rut_ui_viewport_t *properties_vp;
    rut_rectangle_t *bg;

    rut_stack_add(stack0, gradient);
    rut_object_unref(gradient);

    rut_bin_set_left_padding(bin, 10);
    rut_bin_set_right_padding(bin, 5);
    rut_bin_set_bottom_padding(bin, 10);
    rut_bin_set_top_padding(bin, 5);
    rut_stack_add(stack0, bin);
    rut_object_unref(bin);

    rut_bin_set_child(bin, stack1);

    bg = rut_rectangle_new4f(engine->shell,
                             0,
                             0, /* size */
                             1,
                             1,
                             1,
                             1);
    rut_stack_add(stack1, bg);
    rut_object_unref(bg);

    properties_vp = rut_ui_viewport_new(engine->shell, 0, 0);
    engine->properties_vp = properties_vp;

    rut_stack_add(stack1, properties_vp);
    rut_object_unref(properties_vp);

    rut_ui_viewport_set_x_pannable(properties_vp, false);
    rut_ui_viewport_set_y_pannable(properties_vp, true);

    engine->inspector_bin = rut_bin_new(engine->shell);
    rut_ui_viewport_add(engine->properties_vp, engine->inspector_bin);

    rut_ui_viewport_set_sync_widget(properties_vp, engine->inspector_bin);

    rut_box_layout_add(engine->properties_hbox, false, stack0);
    rut_object_unref(stack0);
}

typedef struct _search_toggle_state_t {
    rig_engine_t *engine;
    char *required_tag;
} search_toggle_state_t;

static void
asset_search_toggle_cb(rut_icon_toggle_t *toggle, bool state, void *user_data)
{
    search_toggle_state_t *toggle_state = user_data;
    rig_engine_t *engine = toggle_state->engine;

    if (state) {
        engine->required_search_tags = c_list_prepend(
            engine->required_search_tags, toggle_state->required_tag);
    } else {
        engine->required_search_tags = c_list_remove(
            engine->required_search_tags, toggle_state->required_tag);
    }

    rig_run_search(engine);
}

static void
free_search_toggle_state(void *user_data)
{
    search_toggle_state_t *state = user_data;

    state->engine->required_search_tags =
        c_list_remove(state->engine->required_search_tags, state->required_tag);

    c_free(state->required_tag);

    c_slice_free(search_toggle_state_t, state);
}

static rut_icon_toggle_t *
create_search_toggle(rig_engine_t *engine,
                     const char *set_icon,
                     const char *unset_icon,
                     const char *required_tag)
{
    rut_icon_toggle_t *toggle =
        rut_icon_toggle_new(engine->shell, set_icon, unset_icon);
    search_toggle_state_t *state = c_slice_new0(search_toggle_state_t);

    state->engine = engine;
    state->required_tag = c_strdup(required_tag);

    rut_icon_toggle_add_on_toggle_callback(
        toggle, asset_search_toggle_cb, state, free_search_toggle_state);

    return toggle;
}

static void
create_asset_selectors(rig_engine_t *engine,
                       rut_stack_t *icons_stack)
{
    rut_box_layout_t *hbox =
        rut_box_layout_new(engine->shell,
                           RUT_BOX_LAYOUT_PACKING_LEFT_TO_RIGHT);
    rut_icon_toggle_t *toggle;

    toggle = create_search_toggle(
        engine, "geometry-white.png", "geometry.png", "geometry");
    rut_box_layout_add(hbox, false, toggle);
    rut_object_unref(toggle);

    toggle =
        create_search_toggle(engine, "image-white.png", "image.png", "image");
    rut_box_layout_add(hbox, false, toggle);
    rut_object_unref(toggle);

    toggle =
        create_search_toggle(engine, "video-white.png", "video.png", "video");
    rut_box_layout_add(hbox, false, toggle);
    rut_object_unref(toggle);

    toggle = create_search_toggle(
        engine, "entity-white.png", "entity.png", "entity");
    rut_box_layout_add(hbox, false, toggle);
    rut_object_unref(toggle);

    toggle =
        create_search_toggle(engine, "logic-white.png", "logic.png", "logic");
    rut_box_layout_add(hbox, false, toggle);
    rut_object_unref(toggle);

    rut_stack_add(icons_stack, hbox);
    rut_object_unref(hbox);
}

static void
create_assets_view(rig_engine_t *engine)
{
    rig_editor_t *editor = engine->editor;
    rut_box_layout_t *vbox =
        rut_box_layout_new(engine->shell, RUT_BOX_LAYOUT_PACKING_TOP_TO_BOTTOM);
    rut_stack_t *search_stack = rut_stack_new(engine->shell, 0, 0);
    rut_bin_t *search_bin = rut_bin_new(engine->shell);
    rut_stack_t *icons_stack = rut_stack_new(engine->shell, 0, 0);
    rut_stack_t *stack = rut_stack_new(engine->shell, 0, 0);
    rig_nine_slice_t *gradient =
        load_gradient_image(engine->shell, "toolbar-bg-gradient.png");
    rut_rectangle_t *bg;
    rut_entry_t *entry;
    rut_text_t *text;
    rut_icon_t *search_icon;
    cg_color_t color;

    bg = rut_rectangle_new4f(engine->shell, 0, 0, 0.2, 0.2, 0.2, 1);
    rut_stack_add(search_stack, bg);
    rut_object_unref(bg);

    entry = rut_entry_new(engine->shell);

    text = rut_entry_get_text(entry);
    engine->search_text = text;
    rut_text_set_single_line_mode(text, true);
    rut_text_set_hint_text(text, "Search...");

    search_icon = rut_icon_new(engine->shell, "magnifying-glass.png");
    rut_entry_set_icon(entry, search_icon);

    rut_text_add_text_changed_callback(
        text, asset_search_update_cb, engine, NULL);

    rut_bin_set_child(search_bin, entry);
    rut_object_unref(entry);

    rut_stack_add(search_stack, search_bin);
    rut_bin_set_left_padding(search_bin, 10);
    rut_bin_set_right_padding(search_bin, 10);
    rut_bin_set_top_padding(search_bin, 2);
    rut_bin_set_bottom_padding(search_bin, 2);
    rut_object_unref(search_bin);

    rut_box_layout_add(vbox, false, search_stack);
    rut_object_unref(search_stack);

    bg = rut_rectangle_new4f(engine->shell, 0, 0, 0.57, 0.57, 0.57, 1);
    rut_stack_add(icons_stack, bg);
    rut_object_unref(bg);

    create_asset_selectors(engine, icons_stack);

    rut_box_layout_add(vbox, false, icons_stack);
    rut_object_unref(icons_stack);

    rut_box_layout_add(vbox, true, stack);
    rut_object_unref(stack);

    rut_stack_add(stack, gradient);
    rut_object_unref(gradient);

    editor->search_vp = rut_ui_viewport_new(engine->shell, 0, 0);
    rut_stack_add(stack, editor->search_vp);

    editor->search_results_fold = rut_fold_new(engine->shell, "Results");

    rut_color_init_from_uint32(&color, 0x79b8b0ff);
    rut_fold_set_label_color(editor->search_results_fold, &color);

    rut_fold_set_font_name(editor->search_results_fold, "Bold Sans 20px");

    rut_ui_viewport_add(editor->search_vp, editor->search_results_fold);
    rut_ui_viewport_set_sync_widget(editor->search_vp,
                                    editor->search_results_fold);

    rut_ui_viewport_set_x_pannable(editor->search_vp, false);

    rut_box_layout_add(engine->asset_panel_hbox, false, vbox);
    rut_object_unref(vbox);
}

static void
reload_animated_inspector_properties_cb(rig_controller_prop_data_t *prop_data,
                                        void *user_data)
{
    rig_engine_t *engine = user_data;

    rig_reload_inspector_property(engine, prop_data->property);
}

static void
reload_animated_inspector_properties(rig_engine_t *engine)
{
    if (engine->inspector && engine->selected_controller)
        rig_controller_foreach_property(engine->selected_controller,
                                        reload_animated_inspector_properties_cb,
                                        engine);
}

static void
controller_progress_changed_cb(rut_property_t *progress_prop,
                               void *user_data)
{
    reload_animated_inspector_properties(user_data);
}

static void
set_selected_controller(rig_engine_t *engine,
                        rig_controller_t *controller)
{
    if (engine->selected_controller == controller)
        return;

    if (engine->selected_controller) {
        rut_property_closure_destroy(engine->controller_progress_closure);
        rut_object_unref(engine->selected_controller);
    }

    engine->selected_controller = controller;

    if (controller) {
        rut_object_ref(controller);

        engine->controller_progress_closure = rut_property_connect_callback(
            &controller->props[RIG_CONTROLLER_PROP_PROGRESS],
            controller_progress_changed_cb,
            engine);
    }
}
static void
controller_changed_cb(rig_controller_view_t *view,
                      rig_controller_t *controller,
                      void *user_data)
{
    rig_engine_t *engine = user_data;

    set_selected_controller(engine, controller);
}

static void
create_controller_view(rig_engine_t *engine)
{
    engine->controller_view =
        rig_controller_view_new(engine, engine->undo_journal);

    rig_controller_view_add_controller_changed_callback(
        engine->controller_view, controller_changed_cb, engine, NULL);

    rig_split_view_set_child1(engine->splits[0], engine->controller_view);
    rut_object_unref(engine->controller_view);
}

static void
init_resize_handle(rig_engine_t *engine)
{
#ifdef __APPLE__
    cg_texture_t *resize_handle_texture;
    c_error_t *error = NULL;

    resize_handle_texture = rut_load_texture_from_data_file(
        engine->shell, "resize-handle.png", &error);

    if (resize_handle_texture == NULL) {
        c_warning("Failed to load resize-handle.png: %s", error->message);
        c_error_free(error);
    } else {
        rut_image_t *resize_handle;

        resize_handle = rut_image_new(engine->shell, resize_handle_texture);

        engine->resize_handle_transform = rut_transform_new(engine->shell);

        rut_graphable_add_child(engine->root, engine->resize_handle_transform);

        rut_object_unref(engine->resize_handle_transform);
        rut_object_unref(resize_handle);
        cg_object_unref(resize_handle_texture);
    }

#endif /* __APPLE__ */
}

static rut_image_t *
load_transparency_grid(rut_shell_t *shell)
{
    c_error_t *error = NULL;
    cg_texture_t *texture =
        rut_load_texture_from_data_file(shell, "transparency-grid.png",
                                        &error);
    rut_image_t *ret;

    if (texture == NULL) {
        c_warning("Failed to load transparency-grid.png: %s", error->message);
        c_error_free(error);
    } else {
        ret = rut_image_new(shell, texture);

        rut_image_set_draw_mode(ret, RUT_IMAGE_DRAW_MODE_REPEAT);
        rut_sizable_set_size(ret, 1000000.0f, 1000000.0f);

        cg_object_unref(texture);
    }

    return ret;
}

static void
create_ui(rig_editor_t *editor)
{
    rig_engine_t *engine = editor->engine;

    engine->properties_hbox =
        rut_box_layout_new(engine->shell, RUT_BOX_LAYOUT_PACKING_LEFT_TO_RIGHT);

    /* controllers on the bottom, everything else above */
    engine->splits[0] =
        rig_split_view_new(engine, RIG_SPLIT_VIEW_SPLIT_HORIZONTAL, 100, 100);

    /* assets on the left, main area on the right */
    engine->asset_panel_hbox =
        rut_box_layout_new(engine->shell, RUT_BOX_LAYOUT_PACKING_LEFT_TO_RIGHT);

    create_assets_view(engine);

    create_camera_view(engine);

    create_controller_view(engine);

    rig_split_view_set_child0(engine->splits[0], engine->asset_panel_hbox);

    rut_box_layout_add(engine->properties_hbox, true, engine->splits[0]);
    create_properties_bar(engine);

    rig_split_view_set_split_fraction(engine->splits[0], 0.75);

    engine->top_vbox =
        rut_box_layout_new(engine->shell, RUT_BOX_LAYOUT_PACKING_TOP_TO_BOTTOM);
    create_top_bar(engine);

    /* FIXME: originally I'd wanted to make this a RIGHT_TO_LEFT box
     * layout but it didn't work so I guess I guess there is a bug
     * in the box-layout allocate code. */
    engine->top_hbox =
        rut_box_layout_new(engine->shell, RUT_BOX_LAYOUT_PACKING_LEFT_TO_RIGHT);
    rut_box_layout_add(engine->top_vbox, true, engine->top_hbox);

    rut_box_layout_add(engine->top_hbox, true, engine->properties_hbox);
    create_toolbar(engine);

    rut_stack_add(engine->top_stack, engine->top_vbox);

    engine->transparency_grid = load_transparency_grid(engine->shell);

    init_resize_handle(engine);
}

static Rig__Operation **
serialize_ops(rig_editor_t *editor,
              rig_pb_serializer_t *serializer)
{
    Rig__Operation **pb_ops;
    rut_queue_item_t *item;
    int n_ops = editor->edit_ops->len;
    int i;

    if (!n_ops)
        return NULL;

    pb_ops = rut_memory_stack_memalign(
        serializer->stack, sizeof(void *) * n_ops, RUT_UTIL_ALIGNOF(void *));

    i = 0;
    rut_list_for_each(item, &editor->edit_ops->items, list_node)
    {
        pb_ops[i++] = item->data;
    }

    return pb_ops;
}

static void
handle_edit_operations(rig_editor_t *editor,
                       rig_pb_serializer_t *serializer,
                       Rig__FrameSetup *pb_frame_setup)
{
    rig_engine_t *engine = editor->engine;
    Rig__UIEdit *play_edits;
    c_list_t *l;

    if (!editor->edit_ops->len)
        return;

    pb_frame_setup->edit =
        rig_pb_new(serializer, Rig__UIEdit, rig__uiedit__init);
    pb_frame_setup->edit->n_ops = editor->edit_ops->len;
    pb_frame_setup->edit->ops = serialize_ops(editor, serializer);

    pb_frame_setup->play_edit = NULL;

    /* XXX:
     * Edit operations are applied as they are made so we don't need to
     * apply them here.
     */

    /* Here we try and map edits into corresponding edits of the
     * play-mode ui state...
     *
     * Note: that operations that modify existing objects will refer to
     * play-mode object ids after this mapping, but operations that
     * create new objects will use the original edit-mode ids.
     *
     * This allows us to maintain a mapping from edit-mode objects
     * to new play-mode objects via the register/unregister callbacks
     * given when applying these operations to the play-mode ui
     */
    play_edits =
        rig_engine_copy_pb_ui_edit(&editor->copy_op_ctx, pb_frame_setup->edit);

    /* Forward both sets of edits to the simulator... */

    if (rig_engine_map_pb_ui_edit(
            &editor->map_op_ctx, &editor->play_apply_op_ctx, play_edits)) {
        pb_frame_setup->play_edit = play_edits;
    } else {
        /* Note: it's always possible that applying edits directly to
         * the play-mode ui can fail and in that case we simply reset
         * the play mode ui...
         */
        reset_play_mode_ui(editor);
    }

    /* Forward edits to all slaves... */
    for (l = engine->slave_masters; l; l = l->next)
        rig_slave_master_forward_pb_ui_edit(l->data, pb_frame_setup->edit);

    rut_queue_clear(editor->edit_ops);
}

static void
rig_editor_redraw(rut_shell_t *shell, void *user_data)
{
    rig_editor_t *editor = user_data;
    rig_engine_t *engine = editor->engine;
    rig_frontend_t *frontend = engine->frontend;

    rut_shell_start_redraw(shell);

    rut_shell_update_timelines(shell);

    /* XXX: These are a bit of a misnomer, since they happen before
     * input handling. Typical pre-paint callbacks are allocation
     * callbacks which we want run before painting and since we want
     * input to be consistent with what we paint we want to make sure
     * allocations are also up to date before input handling.
     */
    rut_shell_run_pre_paint_callbacks(shell);

    /* Again we are immediately about to start painting but this is
     * another set of callbacks that can hook into the start of
     * processing a frame with the difference (compared to pre-paint
     * callbacks) that they aren't unregistered each frame and they
     * aren't sorted with respect to a node in a graph.
     */
    rut_shell_run_start_paint_callbacks(shell);

    rut_shell_dispatch_input_events(shell);

    if (!frontend->ui_update_pending) {
        Rig__FrameSetup pb_frame_setup = RIG__FRAME_SETUP__INIT;
        rut_input_queue_t *input_queue = engine->simulator_input_queue;
        rig_pb_serializer_t *serializer;
        float x, y, z;

        serializer = rig_pb_serializer_new(engine);

        pb_frame_setup.n_events = input_queue->n_events;
        pb_frame_setup.events =
            rig_pb_serialize_input_events(serializer, input_queue);

        if (frontend->has_resized) {
            pb_frame_setup.has_view_width = true;
            pb_frame_setup.view_width = frontend->pending_width;
            pb_frame_setup.has_view_height = true;
            pb_frame_setup.view_height = frontend->pending_height;
            frontend->has_resized = false;
        }

        handle_edit_operations(editor, serializer, &pb_frame_setup);

        /* Inform the simulator of the offset position of the main camera
         * view so that it can transform its input events accordingly...
         */
        x = y = z = 0;
        rut_graphable_fully_transform_point(
            engine->main_camera_view, engine->camera_2d, &x, &y, &z);
        pb_frame_setup.has_view_x = true;
        pb_frame_setup.view_x = RUT_UTIL_NEARBYINT(x);

        pb_frame_setup.has_view_y = true;
        pb_frame_setup.view_y = RUT_UTIL_NEARBYINT(y);

        if (engine->play_mode != frontend->pending_play_mode_enabled) {
            pb_frame_setup.has_play_mode = true;
            pb_frame_setup.play_mode = frontend->pending_play_mode_enabled;
        }

        rig_frontend_run_simulator_frame(frontend, serializer, &pb_frame_setup);

        rig_pb_serializer_destroy(serializer);

        rut_input_queue_clear(input_queue);

        rut_memory_stack_rewind(engine->sim_frame_stack);
    }

    rig_engine_paint(engine);

    rig_engine_garbage_collect(engine, delete_object_cb, editor);

    rut_shell_run_post_paint_callbacks(shell);

    rut_memory_stack_rewind(engine->frame_stack);

    rut_shell_end_redraw(shell);

    /* FIXME: we should hook into an asynchronous notification of
     * when rendering has finished for determining when a frame is
     * finished. */
    rut_shell_finish_frame(shell);

    if (rut_shell_check_timelines(shell))
        rut_shell_queue_redraw(shell);
}

static void
_rig_editor_free(rut_object_t *object)
{
    rig_editor_t *editor = object;

    rig_editor_free_builtin_assets(editor);

    rig_engine_op_apply_context_destroy(&editor->apply_op_ctx);
    rig_engine_op_copy_context_destroy(&editor->copy_op_ctx);
    rig_engine_op_map_context_destroy(&editor->map_op_ctx);
    rig_engine_op_apply_context_destroy(&editor->play_apply_op_ctx);

    rut_queue_clear(editor->edit_ops);
    rut_queue_free(editor->edit_ops);

    rut_object_unref(editor->frontend);
    rut_object_unref(editor->shell);
    rut_object_unref(editor->shell);

    rut_object_free(rig_editor_t, editor);
}

rut_type_t rig_editor_type;

static void
_rig_editor_init_type(void)
{
    rut_type_init(&rig_editor_type, "rig_editor_t", _rig_editor_free);
}

static void
create_debug_gradient(rig_engine_t *engine)
{
    cg_vertex_p2c4_t quad[] = { { 0, 0, 0xff, 0x00, 0x00, 0xff },
                                { 0, 200, 0x00, 0xff, 0x00, 0xff },
                                { 200, 200, 0x00, 0x00, 0xff, 0xff },
                                { 200, 0, 0xff, 0xff, 0xff, 0xff } };
    cg_offscreen_t *offscreen;
    cg_primitive_t *prim = cg_primitive_new_p2c4(
        engine->shell->cg_device, CG_VERTICES_MODE_TRIANGLE_FAN, 4, quad);
    cg_pipeline_t *pipeline = cg_pipeline_new(engine->shell->cg_device);

    engine->gradient =
        cg_texture_2d_new_with_size(engine->shell->cg_device, 200, 200);

    offscreen = cg_offscreen_new_with_texture(engine->gradient);

    cg_framebuffer_orthographic(offscreen, 0, 0, 200, 200, -1, 100);
    cg_framebuffer_clear4f(
        offscreen, CG_BUFFER_BIT_COLOR | CG_BUFFER_BIT_DEPTH, 0, 0, 0, 1);
    cg_primitive_draw(prim, offscreen, pipeline);

    cg_object_unref(prim);
    cg_object_unref(offscreen);
}

static void
load_builtin_assets(rig_editor_t *editor)
{
    editor->nine_slice_builtin_asset =
        rig_asset_new_builtin(editor->shell, "nine-slice.png");
    rig_asset_add_inferred_tag(editor->nine_slice_builtin_asset, "nine-slice");
    rig_asset_add_inferred_tag(editor->nine_slice_builtin_asset, "builtin");
    rig_asset_add_inferred_tag(editor->nine_slice_builtin_asset, "geom");
    rig_asset_add_inferred_tag(editor->nine_slice_builtin_asset, "geometry");

    editor->diamond_builtin_asset =
        rig_asset_new_builtin(editor->shell, "diamond.png");
    rig_asset_add_inferred_tag(editor->diamond_builtin_asset, "diamond");
    rig_asset_add_inferred_tag(editor->diamond_builtin_asset, "builtin");
    rig_asset_add_inferred_tag(editor->diamond_builtin_asset, "geom");
    rig_asset_add_inferred_tag(editor->diamond_builtin_asset, "geometry");

    editor->circle_builtin_asset =
        rig_asset_new_builtin(editor->shell, "circle.png");
    rig_asset_add_inferred_tag(editor->circle_builtin_asset, "shape");
    rig_asset_add_inferred_tag(editor->circle_builtin_asset, "circle");
    rig_asset_add_inferred_tag(editor->circle_builtin_asset, "builtin");
    rig_asset_add_inferred_tag(editor->circle_builtin_asset, "geom");
    rig_asset_add_inferred_tag(editor->circle_builtin_asset, "geometry");

    editor->pointalism_grid_builtin_asset =
        rig_asset_new_builtin(editor->shell, "pointalism.png");
    rig_asset_add_inferred_tag(editor->pointalism_grid_builtin_asset, "grid");
    rig_asset_add_inferred_tag(editor->pointalism_grid_builtin_asset,
                               "pointalism");
    rig_asset_add_inferred_tag(editor->pointalism_grid_builtin_asset,
                               "builtin");
    rig_asset_add_inferred_tag(editor->pointalism_grid_builtin_asset, "geom");
    rig_asset_add_inferred_tag(editor->pointalism_grid_builtin_asset,
                               "geometry");

    editor->text_builtin_asset =
        rig_asset_new_builtin(editor->shell, "fonts.png");
    rig_asset_add_inferred_tag(editor->text_builtin_asset, "text");
    rig_asset_add_inferred_tag(editor->text_builtin_asset, "label");
    rig_asset_add_inferred_tag(editor->text_builtin_asset, "builtin");
    rig_asset_add_inferred_tag(editor->text_builtin_asset, "geom");
    rig_asset_add_inferred_tag(editor->text_builtin_asset, "geometry");

    editor->hair_builtin_asset = rig_asset_new_builtin(editor->shell, "hair.png");
    rig_asset_add_inferred_tag(editor->hair_builtin_asset, "hair");
    rig_asset_add_inferred_tag(editor->hair_builtin_asset, "builtin");

    editor->button_input_builtin_asset =
        rig_asset_new_builtin(editor->shell, "button.png");
    rig_asset_add_inferred_tag(editor->button_input_builtin_asset, "button");
    rig_asset_add_inferred_tag(editor->button_input_builtin_asset, "builtin");
    rig_asset_add_inferred_tag(editor->button_input_builtin_asset, "input");
}

void
rig_editor_free_builtin_assets(rig_editor_t *editor)
{
    rut_object_unref(editor->nine_slice_builtin_asset);
    rut_object_unref(editor->diamond_builtin_asset);
    rut_object_unref(editor->circle_builtin_asset);
    rut_object_unref(editor->pointalism_grid_builtin_asset);
    rut_object_unref(editor->text_builtin_asset);
    rut_object_unref(editor->hair_builtin_asset);
    rut_object_unref(editor->button_input_builtin_asset);
}

/* TODO: move corresponding state into rig_editor_t */
static void
init_editor_engine(rig_editor_t *editor)
{
    rig_engine_t *engine = editor->engine;

    engine->objects_selection = _rig_objects_selection_new(engine);

    rut_list_init(&engine->tool_changed_cb_list);

    rig_editor_push_undo_subjournal(engine);

    /* NB: in device mode we assume all inputs need to got to the
     * simulator and we don't need a separate queue. */
    engine->simulator_input_queue = rut_input_queue_new(engine->shell);

    /* Create a color gradient texture that can be used for debugging
     * shadow mapping.
     *
     * XXX: This should probably simply be #ifdef DEBUG code.
     */
    create_debug_gradient(engine);

    load_builtin_assets(editor);

    create_ui(editor);
}

static void
adb_devices_cb(const char **serials, int n_devices, void *user_data)
{
    rig_editor_t *editor = user_data;
    rig_engine_t *engine = editor->engine;
    rut_exception_t *catch = NULL;
    int i;
    c_list_t *l, *next;

    for (l = engine->slave_addresses; l; l = next) {
        rig_slave_address_t *slave_address = l->data;

        next = l->next;

        if (slave_address->type == RIG_SLAVE_ADDRESS_TYPE_ADB_SERIAL) {
            engine->slave_addresses =
                c_list_delete_link(engine->slave_addresses, l);
            rut_object_unref(slave_address);
        }
    }

    /* FIXME: first use :list-forward and only remove the forwards we own */
    if (!rut_adb_command(NULL, &catch, "host:killforward-all")) {
        c_warning("Failed to clear ADB daemon port forwards");
        rut_exception_free(catch);
        return;
    }

    editor->next_forward_port = 64872;

    g_message("ADB devices update:");
    for (i = 0; i < n_devices; i++) {
        rig_slave_address_t *slave_address;
        char *model = rut_adb_getprop(serials[i], "ro.product.model", &catch);
        char *abi = rut_adb_getprop(serials[i], "ro.product.cpu.abi", &catch);
        char *abi2 = rut_adb_getprop(serials[i], "ro.product.cpu.abi2", &catch);
        int forward_port = editor->next_forward_port++;

        if (!rut_adb_command(serials[i],
                             &catch,
                             "host:forward:tcp:%d;localabstract:rig-slave",
                             forward_port)) {
            c_warning(
                "Failed to forward port 64872 for device %s via ADB daemon: %s",
                serials[i],
                catch->message);
            rut_exception_free(catch);
            catch = NULL;
            continue;
        }

        slave_address =
            rig_slave_address_new_adb(model, serials[i], forward_port);
        engine->slave_addresses =
            c_list_prepend(engine->slave_addresses, slave_address);

        g_message("  serial=%s model=\"%s\" abi=%s/%s local port=%d",
                  serials[i],
                  model,
                  abi,
                  abi2,
                  forward_port);
    }
}

static void
rig_editor_init(rut_shell_t *shell, void *user_data)
{
    rig_editor_t *editor = user_data;
    rig_engine_t *engine;

    /* TODO: rig_frontend_t should be a trait of the engine */
    editor->frontend = rig_frontend_new(
        editor->shell, RIG_FRONTEND_ID_EDITOR, false /* start in edit mode */);

    engine = editor->frontend->engine;
    editor->engine = engine;

    /* TODO: rig_editor_t should be a trait of the engine */
    engine->editor = editor;

    init_editor_engine(editor);

    /* Initialize the current mode */
    rig_engine_set_play_mode_enabled(engine, false /* start in edit mode */);

    rig_frontend_post_init_engine(editor->frontend, editor->ui_filename);

    rig_frontend_set_simulator_connected_callback(
        editor->frontend, simulator_connected_cb, editor);

    rig_engine_set_log_op_callback(engine, log_edit_op_cb, editor);

    /* XXX: we should have a better way of handling this ui load
     * callback. Currently it's not possible to set the callback until
     * after we have created a rig_frontend_t which creates our rig_engine_t,
     * but since we pass a filename in when creating the engine we can
     * actually load a UI before we register our callback.
     */
    on_ui_load_cb(editor);
    rig_engine_set_ui_load_callback(engine, on_ui_load_cb, editor);

    rig_engine_op_apply_context_init(&editor->apply_op_ctx,
                                     engine,
                                     nop_register_id_cb,
                                     NULL, /* unregister id */
                                     editor); /* user data */
    rig_engine_set_apply_op_context(engine, &editor->apply_op_ctx);

    rig_engine_op_copy_context_init(&editor->copy_op_ctx, engine);

    rig_engine_op_map_context_init(
        &editor->map_op_ctx, engine, map_id_cb, editor); /* user data */

    rig_engine_op_apply_context_init(&editor->play_apply_op_ctx,
                                     engine,
                                     register_play_mode_object_cb,
                                     NULL, /* unregister id */
                                     editor); /* user data */
#ifdef linux
    /* TODO move into editor */
    rig_avahi_run_browser(engine);
#endif

    editor->adb_tracker =
        rut_adb_device_tracker_new(editor->shell, adb_devices_cb, editor);

    if (getenv("RIG_SLAVE_ADDRESS")) {
        const char *slave_addr = getenv("RIG_SLAVE_ADDRESS");
        char **slave_addrv = c_strsplit(slave_addr, ":", 2);
        if (slave_addrv[0] && slave_addrv[1]) {
            rig_slave_address_t *slave_address = rig_slave_address_new_tcp(
                slave_addrv[0], /* name */
                slave_addrv[0], /* address */
                c_ascii_strtoull(slave_addrv[1], NULL, 10)); /* port */
            engine->slave_addresses =
                c_list_prepend(engine->slave_addresses, slave_address);
        }
        c_strfreev(slave_addrv);
    }

    rut_shell_add_input_callback(
        editor->shell, rig_engine_input_handler, engine, NULL);
}

rig_editor_t *
rig_editor_new(const char *filename)
{
    rig_editor_t *editor = rut_object_alloc0(
        rig_editor_t, &rig_editor_type, _rig_editor_init_type);
    char *assets_location;

    editor->shell = rut_shell_new(rig_editor_redraw,
                                  editor);

    rut_shell_set_on_run_callback(editor->shell,
                                  rig_editor_init,
                                  editor);

    editor->ui_filename = c_strdup(filename);

    assets_location = c_path_get_dirname(editor->ui_filename);
    rut_shell_set_assets_location(editor->shell, assets_location);
    c_free(assets_location);

    editor->edit_ops = rut_queue_new();

    return editor;
}

void
rig_editor_load_file(rig_editor_t *editor, const char *filename)
{
    /* FIXME: report an error to the user! */
    c_return_if_fail(editor->engine->play_mode == false);

    if (editor->ui_filename)
        c_free(editor->ui_filename);

    editor->ui_filename = c_strdup(filename);
    rig_engine_load_file(editor->engine, filename);
}

void
rig_editor_run(rig_editor_t *editor)
{
    rut_shell_main(editor->shell);
}

#if 0
static bool
object_deleted_cb (uint63_t id, void *user_data)
{
    rig_engine_t *engine = user_data;
    void *object (void *) (intptr_t) id;

    c_hash_table_remove (engine->edit_to_play_object_map, object);
}
#endif

static void
inspector_property_changed_cb(rut_property_t *inspected_property,
                              rut_property_t *inspector_property,
                              bool mergeable,
                              void *user_data)
{
    rig_engine_t *engine = user_data;
    rut_boxed_t new_value;

    rut_property_box(inspector_property, &new_value);

    rig_controller_view_edit_property(
        engine->controller_view, mergeable, inspected_property, &new_value);

    rut_boxed_destroy(&new_value);
}

static void
inspector_controlled_changed_cb(rut_property_t *property,
                                bool value,
                                void *user_data)
{
    rig_engine_t *engine = user_data;

    rig_undo_journal_set_controlled(
        engine->undo_journal, engine->selected_controller, property, value);
}

typedef struct {
    rig_engine_t *engine;
    rig_inspector_t *inspector;
} init_controlled_state_data_t;

static void
init_property_controlled_state_cb(rut_property_t *property,
                                  void *user_data)
{
    init_controlled_state_data_t *data = user_data;

    /* XXX: how should we handle showing whether a property is
    * controlled or not when we have multiple objects selected and the
    * property is controlled for some of them, but not all? */
    if (property->spec->animatable) {
        rig_controller_prop_data_t *prop_data;
        rig_controller_t *controller = data->engine->selected_controller;

        prop_data =
            rig_controller_find_prop_data_for_property(controller, property);

        if (prop_data)
            rig_inspector_set_property_controlled(
                data->inspector, property, true);
    }
}

static rig_inspector_t *
create_inspector(rig_engine_t *engine,
                 c_list_t *objects)
{
    rut_object_t *reference_object = objects->data;
    rig_inspector_t *inspector =
        rig_inspector_new(engine->shell,
                          objects,
                          inspector_property_changed_cb,
                          inspector_controlled_changed_cb,
                          engine);

    if (rut_object_is(reference_object, RUT_TRAIT_ID_INTROSPECTABLE)) {
        init_controlled_state_data_t controlled_data;

        controlled_data.engine = engine;
        controlled_data.inspector = inspector;

        rut_introspectable_foreach_property(reference_object,
                                            init_property_controlled_state_cb,
                                            &controlled_data);
    }

    return inspector;
}

typedef struct _delete_button_state_t {
    rig_engine_t *engine;
    c_list_t *components;
} delete_button_state_t;

static void
free_delete_button_state(void *user_data)
{
    delete_button_state_t *state = user_data;

    c_list_free(state->components);
    c_slice_free(delete_button_state_t, user_data);
}

static void
delete_button_click_cb(rut_icon_button_t *button, void *user_data)
{
    delete_button_state_t *state = user_data;
    c_list_t *l;

    for (l = state->components; l; l = l->next) {
        rig_undo_journal_delete_component(state->engine->undo_journal, l->data);
    }

    rut_shell_queue_redraw(state->engine->shell);
}

static void
create_components_inspector(rig_engine_t *engine,
                            c_list_t *components)
{
    rut_component_t *reference_component = components->data;
    rig_inspector_t *inspector = create_inspector(engine, components);
    const char *name = rut_object_get_type_name(reference_component);
    char *label;
    rut_fold_t *fold;
    rut_bin_t *button_bin;
    rut_icon_button_t *delete_button;
    delete_button_state_t *button_state;

    if (strncmp(name, "Rig", 3) == 0)
        name += 3;

    label = c_strconcat(name, " Component", NULL);

    fold = rut_fold_new(engine->shell, label);

    c_free(label);

    rut_fold_set_child(fold, inspector);
    rut_object_unref(inspector);

    button_bin = rut_bin_new(engine->shell);
    rut_bin_set_left_padding(button_bin, 10);
    rut_fold_set_header_child(fold, button_bin);

    /* FIXME: we need better assets here so we can see a visual change
     * when the button is pressed down */
    delete_button = rut_icon_button_new(engine->shell,
                                        NULL, /* no label */
                                        RUT_ICON_BUTTON_POSITION_BELOW,
                                        "component-delete.png", /* normal */
                                        "component-delete.png", /* hover */
                                        "component-delete.png", /* active */
                                        "component-delete.png"); /* disabled */
    button_state = c_slice_new(delete_button_state_t);
    button_state->engine = engine;
    button_state->components = c_list_copy(components);
    rut_icon_button_add_on_click_callback(
        delete_button,
        delete_button_click_cb,
        button_state,
        free_delete_button_state); /* destroy notify */
    rut_bin_set_child(button_bin, delete_button);
    rut_object_unref(delete_button);

    rut_box_layout_add(engine->inspector_box_layout, false, fold);
    rut_object_unref(fold);

    engine->all_inspectors = c_list_prepend(engine->all_inspectors, inspector);
}

rut_object_t *
find_component(rig_entity_t *entity, rut_component_type_t type)
{
    int i;

    for (i = 0; i < entity->components->len; i++) {
        rut_object_t *component = g_ptr_array_index(entity->components, i);
        rut_componentable_props_t *component_props =
            rut_object_get_properties(component, RUT_TRAIT_ID_COMPONENTABLE);

        if (component_props->type == type)
            return component;
    }

    return NULL;
}

typedef struct _match_and_list_state_t {
    rig_engine_t *engine;
    c_list_t *entities;
} match_and_list_state_t;

static bool
match_and_create_components_inspector_cb(rut_object_t *reference_component,
                                         void *user_data)
{
    match_and_list_state_t *state = user_data;
    rut_componentable_props_t *component_props = rut_object_get_properties(
        reference_component, RUT_TRAIT_ID_COMPONENTABLE);
    rut_component_type_t type = component_props->type;
    c_list_t *l;
    c_list_t *components = NULL;

    for (l = state->entities; l; l = l->next) {
        /* XXX: we will need to update this if we ever allow attaching
         * multiple components of the same type to an entity. */

        /* If there is no component of the same type attached to all the
         * other entities then don't list the component */
        rut_component_t *component = rig_entity_get_component(l->data, type);
        if (!component)
            goto EXIT;

        /* Or of the component doesn't also have the same rut_object_t type
         * don't list the component */
        if (rut_object_get_type(component) !=
            rut_object_get_type(reference_component))
            goto EXIT;

        components = c_list_prepend(components, component);
    }

    if (components)
        create_components_inspector(state->engine, components);

EXIT:

    c_list_free(components);

    return true; /* continue */
}

/* TODO: Update to take a rig_editor_t */
void
rig_editor_update_inspector(rig_engine_t *engine)
{
    c_list_t *objects = engine->objects_selection->objects;

    /* This will drop the last reference to any current
     * engine->inspector_box_layout and also any indirect references
     * to existing rig_inspector_ts */
    rut_bin_set_child(engine->inspector_bin, NULL);

    engine->inspector_box_layout =
        rut_box_layout_new(engine->shell,
                           RUT_BOX_LAYOUT_PACKING_TOP_TO_BOTTOM);
    rut_bin_set_child(engine->inspector_bin, engine->inspector_box_layout);

    engine->inspector = NULL;
    c_list_free(engine->all_inspectors);
    engine->all_inspectors = NULL;

    if (objects) {
        rut_object_t *reference_object = objects->data;
        match_and_list_state_t state;

        engine->inspector = create_inspector(engine, objects);

        rut_box_layout_add(
            engine->inspector_box_layout, false, engine->inspector);
        engine->all_inspectors =
            c_list_prepend(engine->all_inspectors, engine->inspector);

        if (rut_object_get_type(reference_object) == &rig_entity_type) {
            state.engine = engine;
            state.entities = objects;

            rig_entity_foreach_component(
                reference_object,
                match_and_create_components_inspector_cb,
                &state);
        }
    }
}

void
rig_reload_inspector_property(rig_engine_t *engine,
                              rut_property_t *property)
{
    if (engine->inspector) {
        c_list_t *l;

        for (l = engine->all_inspectors; l; l = l->next)
            rig_inspector_reload_property(l->data, property);
    }
}

void
rig_reload_position_inspector(rig_engine_t *engine, rig_entity_t *entity)
{
    if (engine->inspector) {
        rut_property_t *property =
            rut_introspectable_lookup_property(entity, "position");

        rig_inspector_reload_property(engine->inspector, property);
    }
}

static void
_rig_objects_selection_cancel(rut_object_t *object)
{
    rig_objects_selection_t *selection = object;
    c_list_free_full(selection->objects, (c_destroy_func_t)rut_object_unref);
    selection->objects = NULL;
}

static rut_object_t *
_rig_objects_selection_copy(rut_object_t *object)
{
    rig_objects_selection_t *selection = object;
    rig_objects_selection_t *copy =
        _rig_objects_selection_new(selection->engine);
    c_list_t *l;

    for (l = selection->objects; l; l = l->next) {
        if (rut_object_get_type(l->data) == &rig_entity_type) {
            copy->objects =
                c_list_prepend(copy->objects, rig_entity_copy(l->data));
        } else {
#warning                                                                       \
            "todo: Create a copyable interface for anything that can be selected for copy and paste"
            c_warn_if_reached();
        }
    }

    return copy;
}

static void
_rig_objects_selection_delete(rut_object_t *object)
{
    rig_objects_selection_t *selection = object;

    if (selection->objects) {
        rig_engine_t *engine = selection->engine;

        /* XXX: It's assumed that a selection either corresponds to
         * engine->objects_selection or to a derived selection due to
         * the selectable::copy vfunc.
         *
         * A copy should contain deep-copied entities that don't need to
         * be directly deleted with
         * rig_undo_journal_delete_entity() because they won't
         * be part of the scenegraph.
         */

        if (selection == engine->objects_selection) {
            c_list_t *l, *next;
            int len = c_list_length(selection->objects);

            for (l = selection->objects; l; l = next) {
                next = l->next;
                rig_undo_journal_delete_entity(engine->undo_journal, l->data);
            }

            /* NB: that rig_undo_journal_delete_component() will
             * remove the entity from the scenegraph*/

            /* XXX: make sure that
             * rig_undo_journal_delete_entity () doesn't change
             * the selection, since it used to. */
            c_warn_if_fail(len == c_list_length(selection->objects));
        }

        c_list_free_full(selection->objects, (c_destroy_func_t)rut_object_unref);
        selection->objects = NULL;

        c_warn_if_fail(selection->objects == NULL);
    }
}

static void
_rig_objects_selection_free(void *object)
{
    rig_objects_selection_t *selection = object;

    _rig_objects_selection_cancel(selection);

    rut_closure_list_disconnect_all(&selection->selection_events_cb_list);

    rut_object_free(rig_objects_selection_t, selection);
}

rut_type_t rig_objects_selection_type;

static void
_rig_objects_selection_init_type(void)
{
    static rut_selectable_vtable_t selectable_vtable = {
        .cancel = _rig_objects_selection_cancel,
        .copy = _rig_objects_selection_copy,
        .del = _rig_objects_selection_delete,
    };
    static rut_mimable_vtable_t mimable_vtable = {
        .copy = _rig_objects_selection_copy
    };

    rut_type_t *type = &rig_objects_selection_type;
#define TYPE rig_objects_selection_t

    rut_type_init(type, C_STRINGIFY(TYPE), _rig_objects_selection_free);
    rut_type_add_trait(type,
                       RUT_TRAIT_ID_SELECTABLE,
                       0, /* no associated properties */
                       &selectable_vtable);
    rut_type_add_trait(type,
                       RUT_TRAIT_ID_MIMABLE,
                       0, /* no associated properties */
                       &mimable_vtable);

#undef TYPE
}

rig_objects_selection_t *
_rig_objects_selection_new(rig_engine_t *engine)
{
    rig_objects_selection_t *selection =
        rut_object_alloc0(rig_objects_selection_t,
                          &rig_objects_selection_type,
                          _rig_objects_selection_init_type);

    selection->engine = engine;
    selection->objects = NULL;

    rut_list_init(&selection->selection_events_cb_list);

    return selection;
}

rut_closure_t *
rig_objects_selection_add_event_callback(
    rig_objects_selection_t *selection,
    rig_objects_selection_event_callback_t callback,
    void *user_data,
    rut_closure_destroy_callback_t destroy_cb)
{
    return rut_closure_list_add(
        &selection->selection_events_cb_list, callback, user_data, destroy_cb);
}

static void
remove_selection_cb(rut_object_t *object,
                    rig_objects_selection_t *selection)
{
    rut_closure_list_invoke(&selection->selection_events_cb_list,
                            rig_objects_selection_event_callback_t,
                            selection,
                            RIG_OBJECTS_SELECTION_REMOVE_EVENT,
                            object);
    rut_object_unref(object);
}

void
rig_select_object(rig_engine_t *engine,
                  rut_object_t *object,
                  rut_select_action_t action)
{
    rig_objects_selection_t *selection = engine->objects_selection;

    /* For now we only support selecting multiple entities... */
    if (object && rut_object_get_type(object) != &rig_entity_type)
        action = RUT_SELECT_ACTION_REPLACE;

#if RIG_EDITOR_ENABLED
    if (object == engine->light_handle)
        object = engine->edit_mode_ui->light;
#endif

#if 0
    else if (entity == engine->play_camera_handle)
        entity = engine->play_camera;
#endif

    switch (action) {
    case RUT_SELECT_ACTION_REPLACE: {
        c_list_t *old = selection->objects;

        selection->objects = NULL;

        c_list_foreach(old, (GFunc)remove_selection_cb, selection);
        c_list_free(old);

        if (object) {
            selection->objects =
                c_list_prepend(selection->objects, rut_object_ref(object));
            rut_closure_list_invoke(&selection->selection_events_cb_list,
                                    rig_objects_selection_event_callback_t,
                                    selection,
                                    RIG_OBJECTS_SELECTION_ADD_EVENT,
                                    object);
        }
        break;
    }
    case RUT_SELECT_ACTION_TOGGLE: {
        c_list_t *link = c_list_find(selection->objects, object);

        if (link) {
            selection->objects = c_list_remove_link(selection->objects, link);

            rut_closure_list_invoke(&selection->selection_events_cb_list,
                                    rig_objects_selection_event_callback_t,
                                    selection,
                                    RIG_OBJECTS_SELECTION_REMOVE_EVENT,
                                    link->data);
            rut_object_unref(link->data);
        } else if (object) {
            rut_closure_list_invoke(&selection->selection_events_cb_list,
                                    rig_objects_selection_event_callback_t,
                                    selection,
                                    RIG_OBJECTS_SELECTION_ADD_EVENT,
                                    object);

            rut_object_ref(object);
            selection->objects = c_list_prepend(selection->objects, object);
        }
    } break;
    }

    if (selection->objects)
        rut_shell_set_selection(engine->shell, engine->objects_selection);

    rut_shell_queue_redraw(engine->shell);

    if (engine->frontend)
        rig_editor_update_inspector(engine);
}

void
rig_editor_push_undo_subjournal(rig_engine_t *engine)
{
    rig_undo_journal_t *subjournal = rig_undo_journal_new(engine);

    rig_undo_journal_set_apply_on_insert(subjournal, true);

    engine->undo_journal_stack =
        c_list_prepend(engine->undo_journal_stack, subjournal);
    engine->undo_journal = subjournal;
}

rig_undo_journal_t *
rig_editor_pop_undo_subjournal(rig_engine_t *engine)
{
    rig_undo_journal_t *head_journal = engine->undo_journal;

    engine->undo_journal_stack = c_list_delete_link(engine->undo_journal_stack,
                                                    engine->undo_journal_stack);
    c_return_val_if_fail(engine->undo_journal_stack, NULL);

    engine->undo_journal = engine->undo_journal_stack->data;

    return head_journal;
}

static void
print_mapping_cb(gpointer key, gpointer value, gpointer user_data)
{
    char *a = rig_engine_get_object_debug_name(key);
    char *b = rig_engine_get_object_debug_name(value);

    c_print("  [%50s] -> [%50s]\n", a, b);

    c_free(a);
    c_free(b);
}

void
rig_editor_print_mappings(rig_editor_t *editor)
{
    c_print("Edit to play mode mappings:\n");
    c_hash_table_foreach(
        editor->edit_to_play_object_map, print_mapping_cb, NULL);

    c_print("\n\n");
    c_print("Play to edit mode mappings:\n");
    c_hash_table_foreach(
        editor->play_to_edit_object_map, print_mapping_cb, NULL);
}
