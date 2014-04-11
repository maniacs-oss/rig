/*
 * Rut
 *
 * Rig Utilities
 *
 * Copyright (C) 2012, 2013 Intel Corporation.
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
 */

#include <config.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <glib.h>

#include <cogl/cogl.h>
#include <cogl/cogl-sdl.h>
#include <cogl-pango/cogl-pango.h>

#include "rut-bitmask.h"
#include "rut-global.h"
#include "rut-context.h"
#include "rut-transform-private.h"
#include "rut-text.h"
#include "rut-timeline.h"
#include "rut-text-buffer.h"
#include "rut-geometry.h"
#include "rut-scroll-bar.h"

typedef struct _RutTextureCacheEntry
{
  RutContext *ctx;
  GQuark filename_quark;
  CoglTexture *texture;
} RutTextureCacheEntry;
#define RUT_TEXTURE_CACHE_ENTRY(X) ((RutTextureCacheEntry *)X)

CoglContext *rut_cogl_context;

static CoglUserDataKey texture_cache_key;

uint8_t _rut_nine_slice_indices_data[] = {
    0,4,5,   0,5,1,  1,5,6,   1,6,2,   2,6,7,    2,7,3,
    4,8,9,   4,9,5,  5,9,10,  5,10,6,  6,10,11,  6,11,7,
    8,12,13, 8,13,9, 9,13,14, 9,14,10, 10,14,15, 10,15,11
};

RutUIEnum _rut_projection_ui_enum = {
  .nick = "Projection",
  .values = {
    { RUT_PROJECTION_PERSPECTIVE, "Perspective", "Perspective Projection" },
    { RUT_PROJECTION_ORTHOGRAPHIC, "Orthographic", "Orthographic Projection" },
    { 0 }
  }
};

typedef struct _SettingsChangedCallbackState
{
  RutSettingsChangedCallback callback;
  GDestroyNotify destroy_notify;
  void *user_data;
} SettingsChangedCallbackState;

struct _RutSettings
{
  GList *changed_callbacks;
};

static void
_rut_settings_free (RutSettings *settings)
{
  GList *l;

  for (l = settings->changed_callbacks; l; l = l->next)
    g_slice_free (SettingsChangedCallbackState, l->data);
  g_list_free (settings->changed_callbacks);
}

RutSettings *
rut_settings_new (void)
{
  return g_slice_new0 (RutSettings);
}

void
rut_settings_add_changed_callback (RutSettings *settings,
                                   RutSettingsChangedCallback callback,
                                   GDestroyNotify destroy_notify,
                                   void *user_data)
{
  GList *l;
  SettingsChangedCallbackState *state;

  for (l = settings->changed_callbacks; l; l = l->next)
    {
      state = l->data;

      if (state->callback == callback)
        {
          state->user_data = user_data;
          state->destroy_notify = destroy_notify;
          return;
        }
    }

  state = g_slice_new (SettingsChangedCallbackState);
  state->callback = callback;
  state->destroy_notify = destroy_notify;
  state->user_data = user_data;

  settings->changed_callbacks = g_list_prepend (settings->changed_callbacks,
                                                state);
}

void
rut_settings_remove_changed_callback (RutSettings *settings,
                                      RutSettingsChangedCallback callback)
{
  GList *l;

  for (l = settings->changed_callbacks; l; l = l->next)
    {
      SettingsChangedCallbackState *state = l->data;

      if (state->callback == callback)
        {
          settings->changed_callbacks =
            g_list_delete_link (settings->changed_callbacks, l);
          g_slice_free (SettingsChangedCallbackState, state);
          return;
        }
    }
}

/* FIXME HACK */
unsigned int
rut_settings_get_password_hint_time (RutSettings *settings)
{
  return 10;
}

char *
rut_settings_get_font_name (RutSettings *settings)
{
  return g_strdup ("Sans 12");
}

static void
_rut_context_free (void *object)
{
  RutContext *ctx = object;

  rut_property_context_destroy (&ctx->property_ctx);

  g_object_unref (ctx->pango_context);
  g_object_unref (ctx->pango_font_map);
  pango_font_description_free (ctx->pango_font_desc);

  g_hash_table_destroy (ctx->texture_cache);

  if (rut_cogl_context == ctx->cogl_context)
    {
      cogl_object_unref (rut_cogl_context);
      rut_cogl_context = NULL;
    }

  cogl_object_unref (ctx->cogl_context);

  _rut_settings_free (ctx->settings);

  g_slice_free (RutContext, ctx);
}

RutType rut_context_type;

static void
_rut_context_init_type (void)
{
  rut_type_init (&rut_context_type, "RutContext", _rut_context_free);
}

static void
_rut_texture_cache_entry_free (RutTextureCacheEntry *entry)
{
  g_slice_free (RutTextureCacheEntry, entry);
}

static void
_rut_texture_cache_entry_destroy_cb (void *entry)
{
  _rut_texture_cache_entry_free (entry);
}

static void
texture_destroyed_cb (void *user_data)
{
  RutTextureCacheEntry *entry = user_data;

  g_hash_table_remove (entry->ctx->texture_cache,
                       GUINT_TO_POINTER (entry->filename_quark));
}

char *
rut_find_data_file (const char *base_filename)
{
  const gchar *const *dirs = g_get_system_data_dirs ();
  const gchar *const *dir;

  for (dir = dirs; *dir; dir++)
    {
      char *full_path =
        g_build_filename (*dir, "rig", base_filename, NULL);

      if (g_file_test (full_path, G_FILE_TEST_EXISTS))
        return full_path;

      g_free (full_path);
    }

  return NULL;
}

CoglTexture *
rut_load_texture (RutContext *ctx, const char *filename, CoglError **error)
{
  GQuark filename_quark = g_quark_from_string (filename);
  RutTextureCacheEntry *entry =
    g_hash_table_lookup (ctx->texture_cache,
                         GUINT_TO_POINTER (filename_quark));
  CoglTexture *texture;

  if (entry)
    return cogl_object_ref (entry->texture);

  texture = (CoglTexture*)
    cogl_texture_2d_new_from_file (ctx->cogl_context, filename, error);
  if (!texture)
    return NULL;

  entry = g_slice_new0 (RutTextureCacheEntry);
  entry->ctx = ctx;
  entry->filename_quark = filename_quark;
  /* Note: we don't take a reference on the texture. The aim of this
   * cache is simply to avoid multiple loads of the same file and
   * doesn't affect the lifetime of the tracked textures. */
  entry->texture = texture;

  /* Track when the texture is freed... */
  cogl_object_set_user_data (texture,
                             &texture_cache_key,
                             entry,
                             texture_destroyed_cb);

  g_hash_table_insert (ctx->texture_cache,
                       GUINT_TO_POINTER (filename_quark),
                       entry);

  return texture;
}

CoglTexture *
rut_load_texture_from_data_file (RutContext *ctx,
                                 const char *filename,
                                 GError **error)
{
  char *full_path = rut_find_data_file (filename);
  CoglTexture *tex;

  if (full_path == NULL)
    {
      g_set_error (error,
                   G_FILE_ERROR,
                   G_FILE_ERROR_NOENT,
                   "File not found");
      return NULL;
    }

#ifndef COGL_HAS_GLIB_SUPPORT
#warning "Rig relies on Cogl being built with glib support, assuming CoglError == GError"
#endif
  tex = rut_load_texture (ctx, full_path, (CoglError **) error);

  g_free (full_path);

  return tex;
}

RutContext *
rut_context_new (RutShell *shell)
{
  RutContext *context;
  CoglError *error = NULL;

  g_return_val_if_fail (shell != NULL, NULL);

  context = rut_object_alloc0 (RutContext,
                               &rut_context_type,
                               _rut_context_init_type);

  rut_property_context_init (&context->property_ctx);
  context->shell = rut_object_ref (shell);

  _rut_matrix_entry_identity_init (&context->identity_entry);

  context->headless = rut_shell_get_headless (shell);

  context->settings = rut_settings_new ();

  if (!context->headless)
    {
#ifdef USE_SDL
      context->cogl_context = cogl_sdl_context_new (SDL_USEREVENT, &error);
#else
      context->cogl_context = cogl_context_new (NULL, &error);
#endif
      if (!context->cogl_context)
        {
          g_warning ("Failed to create Cogl Context: %s", error->message);
          g_free (context);
          return NULL;
        }

      /* We set up the first created RutContext as a global default context */
      if (rut_cogl_context == NULL)
        rut_cogl_context = cogl_object_ref (context->cogl_context);

      context->texture_cache =
        g_hash_table_new_full (g_direct_hash, g_direct_equal,
                               NULL,
                               _rut_texture_cache_entry_destroy_cb);

      context->nine_slice_indices =
        cogl_indices_new (context->cogl_context,
                          COGL_INDICES_TYPE_UNSIGNED_BYTE,
                          _rut_nine_slice_indices_data,
                          sizeof (_rut_nine_slice_indices_data) /
                          sizeof (_rut_nine_slice_indices_data[0]));

      context->single_texture_2d_template =
        cogl_pipeline_new (context->cogl_context);
      cogl_pipeline_set_layer_null_texture (context->single_texture_2d_template,
                                            0, COGL_TEXTURE_TYPE_2D);

      context->circle_texture =
        rut_create_circle_texture (context,
                                   CIRCLE_TEX_RADIUS /* radius */,
                                   CIRCLE_TEX_PADDING /* padding */);

      cogl_matrix_init_identity (&context->identity_matrix);

      context->pango_font_map =
        COGL_PANGO_FONT_MAP (cogl_pango_font_map_new (context->cogl_context));

      cogl_pango_font_map_set_use_mipmapping (context->pango_font_map, TRUE);

      context->pango_context =
        pango_font_map_create_context (PANGO_FONT_MAP (context->pango_font_map));

      context->pango_font_desc = pango_font_description_new ();
      pango_font_description_set_family (context->pango_font_desc, "Sans");
      pango_font_description_set_size (context->pango_font_desc, 14 * PANGO_SCALE);

      context->fc_config = FcInitLoadConfigAndFonts ();
      if (FT_Init_FreeType (&context->ft_library))
        {
          g_critical ("Failed to initialize freetype");
        }
    }

  _rut_shell_associate_context (shell, context);

  return context;
}

void
rut_context_init (RutContext *context)
{
  if (context->shell)
    _rut_shell_init (context->shell);
}

RutTextDirection
rut_get_text_direction (RutContext *context)
{
  return RUT_TEXT_DIRECTION_LEFT_TO_RIGHT;
}

void
rut_set_assets_location (RutContext *context,
                         const char *assets_location)
{
  context->assets_location = g_strdup (assets_location);
}

void
rut_init_tls_state (void)
{
#ifdef RUT_ENABLE_REFCOUNT_DEBUG
  rut_refcount_debug_init ();
#endif
}
