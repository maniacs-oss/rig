/*
 * Rig
 *
 * Copyright (C) 2014  Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <math.h>

#include <rut.h>

#include "rig-ui.h"
#include "rig-code.h"


static void
_rig_ui_free (void *object)
{
  RigUI *ui = object;
  GList *l;

  for (l = ui->suspended_controllers; l; l = l->next)
    rut_object_unref (l->data);
  g_list_free (ui->suspended_controllers);

  for (l = ui->controllers; l; l = l->next)
    rut_object_unref (l->data);
  g_list_free (ui->controllers);

  for (l = ui->assets; l; l = l->next)
    rut_object_unref (l->data);
  g_list_free (ui->assets);

  /* NB: no extra reference is held on ui->light other than the
   * reference for it being in the ->scene. */

  if (ui->scene)
    rut_object_unref (ui->scene);

  if (ui->play_camera)
    rut_object_unref (ui->play_camera);

  if (ui->play_camera_component)
    rut_object_unref (ui->play_camera_component);

  if (ui->dso_data)
    g_free (ui->dso_data);

  rut_object_free (RigUI, object);
}

RutType rig_ui_type;

static void
_rig_ui_init_type (void)
{
  rut_type_init (&rig_ui_type, "RigUI", _rig_ui_free);
}

RigUI *
rig_ui_new (RigEngine *engine)
{
  RigUI *ui = rut_object_alloc0 (RigUI, &rig_ui_type,
                                 _rig_ui_init_type);

  ui->engine = engine;

  return ui;
}

void
rig_ui_set_dso_data (RigUI *ui, uint8_t *data, int len)
{
  if (ui->dso_data)
    g_free (ui->dso_data);

  ui->dso_data = g_malloc (len);
  memcpy (ui->dso_data, data, len);
  ui->dso_len = len;
}

typedef struct
{
  const char *label;
  RutEntity *entity;
} FindEntityData;

static RutTraverseVisitFlags
find_entity_cb (RutObject *object,
                int depth,
                void *user_data)
{
  FindEntityData *data = user_data;

  if (rut_object_get_type (object) == &rut_entity_type &&
      !strcmp (data->label, rut_entity_get_label (object)))
    {
      data->entity = object;
      return RUT_TRAVERSE_VISIT_BREAK;
    }

  return RUT_TRAVERSE_VISIT_CONTINUE;
}

RutEntity *
rig_ui_find_entity (RigUI *ui, const char *label)
{
  FindEntityData data = { .label = label, .entity = NULL };

  rut_graphable_traverse (ui->scene,
                          RUT_TRAVERSE_DEPTH_FIRST,
                          find_entity_cb,
                          NULL, /* after_children_cb */
                          &data);

  return data.entity;
}

static void
initialise_play_camera_position (RigEngine *engine,
                                 RigUI *ui)
{
  float fov_y = 10; /* y-axis field of view */
  float aspect = (float) engine->device_width / (float) engine->device_height;
  float z_near = 10; /* distance to near clipping plane */
  float z_2d = 30;
  float position[3];
  float left, right, top;
  float left_2d_plane, right_2d_plane;
  float width_scale;
  float width_2d_start;

  /* Initialise the camera to the center of the device with a z
   * position that will give it pixel aligned coordinates at the
   * origin */
  top = z_near * tanf (fov_y * G_PI / 360.0);
  left = -top * aspect;
  right = top * aspect;

  left_2d_plane = left / z_near * z_2d;
  right_2d_plane = right / z_near * z_2d;

  width_2d_start = right_2d_plane - left_2d_plane;

  width_scale = width_2d_start / engine->device_width;

  position[0] = engine->device_width / 2.0f;
  position[1] = engine->device_height / 2.0f;
  position[2] = z_2d / width_scale;

  rut_entity_set_position (ui->play_camera, position);
}

void
rig_ui_prepare (RigUI *ui)
{
  RigEngine *engine = ui->engine;
  RigController *controller;
  RutCamera *light_camera;

  if (!ui->scene)
    ui->scene = rut_graph_new (engine->ctx);

  if (!ui->light)
    {
      RutLight *light;
      float vector3[3];
      CoglColor color;

      ui->light = rut_entity_new (engine->ctx);
      rut_entity_set_label (ui->light, "light");

      vector3[0] = 0;
      vector3[1] = 0;
      vector3[2] = 500;
      rut_entity_set_position (ui->light, vector3);

      rut_entity_rotate_x_axis (ui->light, 20);
      rut_entity_rotate_y_axis (ui->light, -20);

      light = rut_light_new (engine->ctx);
      cogl_color_init_from_4f (&color, .2f, .2f, .2f, 1.f);
      rut_light_set_ambient (light, &color);
      cogl_color_init_from_4f (&color, .6f, .6f, .6f, 1.f);
      rut_light_set_diffuse (light, &color);
      cogl_color_init_from_4f (&color, .4f, .4f, .4f, 1.f);
      rut_light_set_specular (light, &color);

      rut_entity_add_component (ui->light, light);

      rut_graphable_add_child (ui->scene, ui->light);
    }

  light_camera = rut_entity_get_component (ui->light,
                                           RUT_COMPONENT_TYPE_CAMERA);
  if (!light_camera)
    {
      light_camera = rut_camera_new (engine->ctx,
                                     1000, /* ortho/vp width */
                                     1000, /* ortho/vp height */
                                     NULL);

      rut_camera_set_background_color4f (light_camera, 0.f, .3f, 0.f, 1.f);
      rut_camera_set_projection_mode (light_camera,
                                      RUT_PROJECTION_ORTHOGRAPHIC);
      rut_camera_set_orthographic_coordinates (light_camera,
                                               -1000, -1000, 1000, 1000);
      rut_camera_set_near_plane (light_camera, 1.1f);
      rut_camera_set_far_plane (light_camera, 1500.f);

      rut_entity_add_component (ui->light, light_camera);
    }

  if (engine->frontend)
    {
      CoglFramebuffer *fb = engine->shadow_fb;
      int width = cogl_framebuffer_get_width (fb);
      int height = cogl_framebuffer_get_height (fb);
      rut_camera_set_framebuffer (light_camera, fb);
      rut_camera_set_viewport (light_camera, 0, 0, width, height);
    }

  if (!ui->controllers)
    {
      controller = rig_controller_new (engine, "Controller 0");
      rig_controller_set_active (controller, true);
      ui->controllers = g_list_prepend (ui->controllers, controller);
    }

  if (!ui->play_camera)
    {
      /* Check if there is already an entity labelled ‘play-camera’
       * in the scene graph */
      ui->play_camera = rig_ui_find_entity (ui, "play-camera");

      if (ui->play_camera)
        ui->play_camera = rut_object_ref (ui->play_camera);
      else
        {
          ui->play_camera = rut_entity_new (engine->ctx);
          rut_entity_set_label (ui->play_camera, "play-camera");

          initialise_play_camera_position (engine, ui);

          rut_graphable_add_child (ui->scene, ui->play_camera);
        }
    }

  if (!ui->play_camera_component)
    {
      ui->play_camera_component =
        rut_entity_get_component (ui->play_camera, RUT_COMPONENT_TYPE_CAMERA);

      if (ui->play_camera_component)
        rut_object_ref (ui->play_camera_component);
      else
        {
          ui->play_camera_component =
            rut_camera_new (engine->ctx,
                            -1, /* ortho/vp width */
                            -1, /* ortho/vp height */
                            engine->onscreen);

          rut_entity_add_component (ui->play_camera, ui->play_camera_component);
        }
    }

  rut_camera_set_clear (ui->play_camera_component, false);

  rig_ui_suspend (ui);
}

void
rig_ui_suspend (RigUI *ui)
{
  GList *l;

  if (ui->suspended)
    return;

  for (l = ui->controllers; l; l = l->next)
    {
      RigController *controller = l->data;

      rig_controller_set_suspended (controller, true);

      ui->suspended_controllers =
        g_list_prepend (ui->suspended_controllers, controller);

      /* We take a reference on all suspended controllers so we
       * don't need to worry if any of the controllers are deleted
       * while in edit mode. */
      rut_object_ref (controller);
    }

  ui->suspended = true;
}

void
rig_ui_resume (RigUI *ui)
{
  GList *l;

  if (!ui->suspended)
    return;

  for (l = ui->suspended_controllers; l; l = l->next)
    {
      RigController *controller = l->data;

      rig_controller_set_suspended (controller, false);
      rut_object_unref (controller);
    }

  g_list_free (ui->suspended_controllers);
  ui->suspended_controllers = NULL;

  ui->suspended = false;
}