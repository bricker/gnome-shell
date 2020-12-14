/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#include <clutter/clutter.h>
#include <cogl/cogl.h>
#include <meta/display.h>
#include <meta/util.h>
#include <meta/meta-plugin.h>
#include <meta/meta-cursor-tracker.h>
#include <st/st.h>

#include "shell-global.h"
#include "shell-screenshot.h"
#include "shell-util.h"

typedef enum _ShellScreenshotFlag
{
  SHELL_SCREENSHOT_FLAG_NONE,
  SHELL_SCREENSHOT_FLAG_INCLUDE_CURSOR,
} ShellScreenshotFlag;

typedef struct _ShellScreenshotPrivate  ShellScreenshotPrivate;

struct _ShellScreenshot
{
  GObject parent_instance;

  ShellScreenshotPrivate *priv;
};

struct _ShellScreenshotPrivate
{
  ShellGlobal *global;

  GOutputStream *stream;
  ShellScreenshotFlag flags;

  GDateTime *datetime;

  cairo_surface_t *image;
  cairo_rectangle_int_t screenshot_area;

  gboolean include_frame;
};

typedef enum
{
  SHELL_SCREENSHOT_SCREEN,
  SHELL_SCREENSHOT_WINDOW,
  SHELL_SCREENSHOT_AREA,
} ShellScreenshotMode;

G_DEFINE_TYPE_WITH_PRIVATE (ShellScreenshot, shell_screenshot, G_TYPE_OBJECT);

static void
shell_screenshot_class_init (ShellScreenshotClass *screenshot_class)
{
  (void) screenshot_class;
}

static void
shell_screenshot_init (ShellScreenshot *screenshot)
{
  screenshot->priv = shell_screenshot_get_instance_private (screenshot);
  screenshot->priv->global = shell_global_get ();
}

static void
on_screenshot_written (GObject      *source,
                       GAsyncResult *task,
                       gpointer      user_data)
{
  ShellScreenshot *screenshot = SHELL_SCREENSHOT (source);
  ShellScreenshotPrivate *priv = screenshot->priv;
  GTask *result = user_data;

  g_task_return_boolean (result, g_task_propagate_boolean (G_TASK (task), NULL));
  g_object_unref (result);

  g_clear_pointer (&priv->image, cairo_surface_destroy);
  g_clear_object (&priv->stream);
  g_clear_pointer (&priv->datetime, g_date_time_unref);
}

static void
write_screenshot_thread (GTask        *result,
                         gpointer      object,
                         gpointer      task_data,
                         GCancellable *cancellable)
{
  ShellScreenshot *screenshot = SHELL_SCREENSHOT (object);
  ShellScreenshotPrivate *priv;
  g_autoptr (GOutputStream) stream = NULL;
  g_autoptr(GdkPixbuf) pixbuf = NULL;
  g_autofree char *creation_time = NULL;
  GError *error = NULL;

  g_assert (screenshot != NULL);

  priv = screenshot->priv;

  stream = g_object_ref (priv->stream);

  pixbuf = gdk_pixbuf_get_from_surface (priv->image,
                                        0, 0,
                                        cairo_image_surface_get_width (priv->image),
                                        cairo_image_surface_get_height (priv->image));
  creation_time = g_date_time_format (priv->datetime, "%c");

  if (!creation_time)
    creation_time = g_date_time_format (priv->datetime, "%FT%T%z");

  gdk_pixbuf_save_to_stream (pixbuf, stream, "png", NULL, &error,
                             "tEXt::Software", "gnome-screenshot",
                             "tEXt::Creation Time", creation_time,
                             NULL);

  if (error)
    g_task_return_error (result, error);
  else
    g_task_return_boolean (result, TRUE);
}

static void
do_grab_screenshot (ShellScreenshot     *screenshot,
                    int                  x,
                    int                  y,
                    int                  width,
                    int                  height,
                    ShellScreenshotFlag  flags)
{
  ShellScreenshotPrivate *priv = screenshot->priv;
  ClutterStage *stage = shell_global_get_stage (priv->global);
  cairo_rectangle_int_t screenshot_rect = { x, y, width, height };
  int image_width;
  int image_height;
  float scale;
  cairo_surface_t *image;
  ClutterPaintFlag paint_flags = CLUTTER_PAINT_FLAG_NONE;
  g_autoptr (GError) error = NULL;

  clutter_stage_get_capture_final_size (stage, &screenshot_rect,
                                        &image_width,
                                        &image_height,
                                        &scale);
  image = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                      image_width, image_height);

  if (flags & SHELL_SCREENSHOT_FLAG_INCLUDE_CURSOR)
    paint_flags |= CLUTTER_PAINT_FLAG_FORCE_CURSORS;
  else
    paint_flags |= CLUTTER_PAINT_FLAG_NO_CURSORS;
  if (!clutter_stage_paint_to_buffer (stage, &screenshot_rect, scale,
                                      cairo_image_surface_get_data (image),
                                      cairo_image_surface_get_stride (image),
                                      CLUTTER_CAIRO_FORMAT_ARGB32,
                                      paint_flags,
                                      &error))
    {
      cairo_surface_destroy (image);
      g_warning ("Failed to take screenshot: %s", error->message);
      return;
    }

  priv->image = image;

  priv->datetime = g_date_time_new_now_local ();
}

static void
draw_cursor_image (cairo_surface_t       *surface,
                   cairo_rectangle_int_t  area)
{
  CoglTexture *texture;
  int width, height;
  int stride;
  guint8 *data;
  MetaDisplay *display;
  MetaCursorTracker *tracker;
  cairo_surface_t *cursor_surface;
  cairo_region_t *screenshot_region;
  cairo_t *cr;
  int x, y;
  int xhot, yhot;
  double xscale, yscale;

  display = shell_global_get_display (shell_global_get ());
  tracker = meta_cursor_tracker_get_for_display (display);
  texture = meta_cursor_tracker_get_sprite (tracker);

  if (!texture)
    return;

  screenshot_region = cairo_region_create_rectangle (&area);
  meta_cursor_tracker_get_pointer (tracker, &x, &y, NULL);

  if (!cairo_region_contains_point (screenshot_region, x, y))
    {
      cairo_region_destroy (screenshot_region);
      return;
    }

  meta_cursor_tracker_get_hot (tracker, &xhot, &yhot);
  width = cogl_texture_get_width (texture);
  height = cogl_texture_get_height (texture);
  stride = 4 * width;
  data = g_new (guint8, stride * height);
  cogl_texture_get_data (texture, CLUTTER_CAIRO_FORMAT_ARGB32, stride, data);

  /* FIXME: cairo-gl? */
  cursor_surface = cairo_image_surface_create_for_data (data,
                                                        CAIRO_FORMAT_ARGB32,
                                                        width, height,
                                                        stride);

  cairo_surface_get_device_scale (surface, &xscale, &yscale);

  if (xscale != 1.0 || yscale != 1.0)
    {
      int monitor;
      float monitor_scale;
      MetaRectangle cursor_rect = {
        .x = x, .y = y, .width = width, .height = height
      };

      monitor = meta_display_get_monitor_index_for_rect (display, &cursor_rect);
      monitor_scale = meta_display_get_monitor_scale (display, monitor);

      cairo_surface_set_device_scale (cursor_surface, monitor_scale, monitor_scale);
    }

  cr = cairo_create (surface);
  cairo_set_source_surface (cr,
                            cursor_surface,
                            x - xhot - area.x,
                            y - yhot - area.y);
  cairo_paint (cr);

  cairo_destroy (cr);
  cairo_surface_destroy (cursor_surface);
  cairo_region_destroy (screenshot_region);
  g_free (data);
}

static void
grab_screenshot (ShellScreenshot     *screenshot,
                 ShellScreenshotFlag  flags,
                 GTask               *result)
{
  ShellScreenshotPrivate *priv = screenshot->priv;
  MetaDisplay *display;
  int width, height;
  GTask *task;

  display = shell_global_get_display (priv->global);
  meta_display_get_size (display, &width, &height);

  do_grab_screenshot (screenshot,
                      0, 0, width, height,
                      flags);

  priv->screenshot_area.x = 0;
  priv->screenshot_area.y = 0;
  priv->screenshot_area.width = width;
  priv->screenshot_area.height = height;

  task = g_task_new (screenshot, NULL, on_screenshot_written, result);
  g_task_run_in_thread (task, write_screenshot_thread);
  g_object_unref (task);
}

static void
grab_window_screenshot (ShellScreenshot     *screenshot,
                        ShellScreenshotFlag  flags,
                        GTask               *result)
{
  ShellScreenshotPrivate *priv = screenshot->priv;
  GTask *task;
  MetaDisplay *display = shell_global_get_display (priv->global);
  MetaWindow *window = meta_display_get_focus_window (display);
  ClutterActor *window_actor;
  gfloat actor_x, actor_y;
  MetaRectangle rect;

  window_actor = CLUTTER_ACTOR (meta_window_get_compositor_private (window));
  clutter_actor_get_position (window_actor, &actor_x, &actor_y);

  meta_window_get_frame_rect (window, &rect);

  if (!priv->include_frame)
    meta_window_frame_rect_to_client_rect (window, &rect, &rect);

  priv->screenshot_area = rect;

  priv->image = meta_window_actor_get_image (META_WINDOW_ACTOR (window_actor),
                                             NULL);

  if (!priv->image)
    {
      g_task_report_new_error (screenshot, on_screenshot_written, result, NULL,
                               G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Capturing window failed");
      return;
    }

  priv->datetime = g_date_time_new_now_local ();

  if (flags & SHELL_SCREENSHOT_FLAG_INCLUDE_CURSOR)
    {
      if (meta_window_get_client_type (window) == META_WINDOW_CLIENT_TYPE_WAYLAND)
        {
          float resource_scale;
          resource_scale = clutter_actor_get_resource_scale (window_actor);

          cairo_surface_set_device_scale (priv->image, resource_scale, resource_scale);
        }

      draw_cursor_image (priv->image, priv->screenshot_area);
    }

  task = g_task_new (screenshot, NULL, on_screenshot_written, result);
  g_task_run_in_thread (task, write_screenshot_thread);
  g_object_unref (task);
}

static gboolean
finish_screenshot (ShellScreenshot        *screenshot,
                   GAsyncResult           *result,
                   cairo_rectangle_int_t **area,
                   GError                **error)
{
  ShellScreenshotPrivate *priv = screenshot->priv;

  if (!g_task_propagate_boolean (G_TASK (result), error))
    return FALSE;

  if (area)
    *area = &priv->screenshot_area;

  return TRUE;
}

static void
on_after_paint (ClutterStage     *stage,
                ClutterStageView *view,
                GTask            *result)
{
  ShellScreenshot *screenshot = g_task_get_task_data (result);
  ShellScreenshotPrivate *priv = screenshot->priv;
  MetaDisplay *display = shell_global_get_display (priv->global);

  g_signal_handlers_disconnect_by_func (stage, on_after_paint, result);

  grab_screenshot (screenshot, priv->flags, result);

  meta_enable_unredirect_for_display (display);
}

/**
 * shell_screenshot_screenshot:
 * @screenshot: the #ShellScreenshot
 * @include_cursor: Whether to include the cursor or not
 * @stream: The stream for the screenshot
 * @callback: (scope async): function to call returning success or failure
 * of the async grabbing
 * @user_data: the data to pass to callback function
 *
 * Takes a screenshot of the whole screen
 * in @stream as png image.
 *
 */
void
shell_screenshot_screenshot (ShellScreenshot     *screenshot,
                             gboolean             include_cursor,
                             GOutputStream       *stream,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
  ShellScreenshotPrivate *priv;
  GTask *result;
  ShellScreenshotFlag flags;

  g_return_if_fail (SHELL_IS_SCREENSHOT (screenshot));
  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));

  priv = screenshot->priv;

  if (priv->stream != NULL) {
    if (callback)
      g_task_report_new_error (screenshot,
                               callback,
                               user_data,
                               shell_screenshot_screenshot,
                               G_IO_ERROR,
                               G_IO_ERROR_PENDING,
                               "Only one screenshot operation at a time "
                               "is permitted");
    return;
  }

  result = g_task_new (screenshot, NULL, callback, user_data);
  g_task_set_source_tag (result, shell_screenshot_screenshot);
  g_task_set_task_data (result, screenshot, NULL);

  priv->stream = g_object_ref (stream);

  flags = SHELL_SCREENSHOT_FLAG_NONE;
  if (include_cursor)
    flags |= SHELL_SCREENSHOT_FLAG_INCLUDE_CURSOR;

  if (meta_is_wayland_compositor ())
    {
      grab_screenshot (screenshot, flags, result);
    }
  else
    {
      MetaDisplay *display = shell_global_get_display (priv->global);
      ClutterStage *stage = shell_global_get_stage (priv->global);

      meta_disable_unredirect_for_display (display);
      clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));
      priv->flags = flags;
      g_signal_connect (stage, "after-paint",
                        G_CALLBACK (on_after_paint), result);
    }
}

/**
 * shell_screenshot_screenshot_finish:
 * @screenshot: the #ShellScreenshot
 * @result: the #GAsyncResult that was provided to the callback
 * @area: (out) (transfer none): the area that was grabbed in screen coordinates
 * @error: #GError for error reporting
 *
 * Finish the asynchronous operation started by shell_screenshot_screenshot()
 * and obtain its result.
 *
 * Returns: whether the operation was successful
 *
 */
gboolean
shell_screenshot_screenshot_finish (ShellScreenshot        *screenshot,
                                    GAsyncResult           *result,
                                    cairo_rectangle_int_t **area,
                                    GError                **error)
{
  g_return_val_if_fail (SHELL_IS_SCREENSHOT (screenshot), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result,
                                                  shell_screenshot_screenshot),
                        FALSE);
  return finish_screenshot (screenshot, result, area, error);
}

/**
 * shell_screenshot_screenshot_area:
 * @screenshot: the #ShellScreenshot
 * @x: The X coordinate of the area
 * @y: The Y coordinate of the area
 * @width: The width of the area
 * @height: The height of the area
 * @stream: The stream for the screenshot
 * @callback: (scope async): function to call returning success or failure
 * of the async grabbing
 * @user_data: the data to pass to callback function
 *
 * Takes a screenshot of the passed in area and saves it
 * in @stream as png image.
 *
 */
void
shell_screenshot_screenshot_area (ShellScreenshot     *screenshot,
                                  int                  x,
                                  int                  y,
                                  int                  width,
                                  int                  height,
                                  GOutputStream       *stream,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  ShellScreenshotPrivate *priv;
  GTask *result;
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (SHELL_IS_SCREENSHOT (screenshot));
  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));

  priv = screenshot->priv;

  if (priv->stream != NULL) {
    if (callback)
      g_task_report_new_error (screenshot,
                               callback,
                               NULL,
                               shell_screenshot_screenshot_area,
                               G_IO_ERROR,
                               G_IO_ERROR_PENDING,
                               "Only one screenshot operation at a time "
                               "is permitted");
    return;
  }

  result = g_task_new (screenshot, NULL, callback, user_data);
  g_task_set_source_tag (result, shell_screenshot_screenshot_area);

  priv->stream = g_object_ref (stream);
  priv->screenshot_area.x = x;
  priv->screenshot_area.y = y;
  priv->screenshot_area.width = width;
  priv->screenshot_area.height = height;

  do_grab_screenshot (screenshot,
                      priv->screenshot_area.x,
                      priv->screenshot_area.y,
                      priv->screenshot_area.width,
                      priv->screenshot_area.height,
                      SHELL_SCREENSHOT_FLAG_NONE);

  task = g_task_new (screenshot, NULL, on_screenshot_written, result);
  g_task_run_in_thread (task, write_screenshot_thread);
}

/**
 * shell_screenshot_screenshot_area_finish:
 * @screenshot: the #ShellScreenshot
 * @result: the #GAsyncResult that was provided to the callback
 * @area: (out) (transfer none): the area that was grabbed in screen coordinates
 * @error: #GError for error reporting
 *
 * Finish the asynchronous operation started by shell_screenshot_screenshot_area()
 * and obtain its result.
 *
 * Returns: whether the operation was successful
 *
 */
gboolean
shell_screenshot_screenshot_area_finish (ShellScreenshot        *screenshot,
                                         GAsyncResult           *result,
                                         cairo_rectangle_int_t **area,
                                         GError                **error)
{
  g_return_val_if_fail (SHELL_IS_SCREENSHOT (screenshot), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result,
                                                  shell_screenshot_screenshot_area),
                        FALSE);
  return finish_screenshot (screenshot, result, area, error);
}

/**
 * shell_screenshot_screenshot_window:
 * @screenshot: the #ShellScreenshot
 * @include_frame: Whether to include the frame or not
 * @include_cursor: Whether to include the cursor or not
 * @stream: The stream for the screenshot
 * @callback: (scope async): function to call returning success or failure
 * of the async grabbing
 * @user_data: the data to pass to callback function
 *
 * Takes a screenshot of the focused window (optionally omitting the frame)
 * in @stream as png image.
 *
 */
void
shell_screenshot_screenshot_window (ShellScreenshot     *screenshot,
                                    gboolean             include_frame,
                                    gboolean             include_cursor,
                                    GOutputStream       *stream,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
  ShellScreenshotPrivate *priv;
  MetaDisplay *display;
  MetaWindow *window;
  GTask *result;

  g_return_if_fail (SHELL_IS_SCREENSHOT (screenshot));
  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));

  priv = screenshot->priv;
  display = shell_global_get_display (priv->global);
  window = meta_display_get_focus_window (display);

  if (priv->stream != NULL || !window) {
    if (callback)
      g_task_report_new_error (screenshot,
                               callback,
                               NULL,
                               shell_screenshot_screenshot_window,
                               G_IO_ERROR,
                               G_IO_ERROR_PENDING,
                               "Only one screenshot operation at a time "
                               "is permitted");
    return;
  }

  result = g_task_new (screenshot, NULL, callback, user_data);
  g_task_set_source_tag (result, shell_screenshot_screenshot_window);

  priv->stream = g_object_ref (stream);
  priv->include_frame = include_frame;

  grab_window_screenshot (screenshot, include_cursor, result);
}

/**
 * shell_screenshot_screenshot_window_finish:
 * @screenshot: the #ShellScreenshot
 * @result: the #GAsyncResult that was provided to the callback
 * @area: (out) (transfer none): the area that was grabbed in screen coordinates
 * @error: #GError for error reporting
 *
 * Finish the asynchronous operation started by shell_screenshot_screenshot_window()
 * and obtain its result.
 *
 * Returns: whether the operation was successful
 *
 */
gboolean
shell_screenshot_screenshot_window_finish (ShellScreenshot        *screenshot,
                                           GAsyncResult           *result,
                                           cairo_rectangle_int_t **area,
                                           GError                **error)
{
  g_return_val_if_fail (SHELL_IS_SCREENSHOT (screenshot), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result,
                                                  shell_screenshot_screenshot_window),
                        FALSE);
  return finish_screenshot (screenshot, result, area, error);
}

/**
 * shell_screenshot_pick_color:
 * @screenshot: the #ShellScreenshot
 * @x: The X coordinate to pick
 * @y: The Y coordinate to pick
 * @callback: (scope async): function to call returning success or failure
 * of the async grabbing
 *
 * Picks the pixel at @x, @y and returns its color as #ClutterColor.
 *
 */
void
shell_screenshot_pick_color (ShellScreenshot     *screenshot,
                             int                  x,
                             int                  y,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
  ShellScreenshotPrivate *priv;
  g_autoptr (GTask) result = NULL;

  g_return_if_fail (SHELL_IS_SCREENSHOT (screenshot));

  result = g_task_new (screenshot, NULL, callback, user_data);
  g_task_set_source_tag (result, shell_screenshot_pick_color);

  priv = screenshot->priv;

  priv->screenshot_area.x = x;
  priv->screenshot_area.y = y;
  priv->screenshot_area.width = 1;
  priv->screenshot_area.height = 1;

  do_grab_screenshot (screenshot,
                      priv->screenshot_area.x,
                      priv->screenshot_area.y,
                      1,
                      1,
                      SHELL_SCREENSHOT_FLAG_NONE);

  g_task_return_boolean (result, TRUE);
}

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
#define INDEX_A 3
#define INDEX_R 2
#define INDEX_G 1
#define INDEX_B 0
#else
#define INDEX_A 0
#define INDEX_R 1
#define INDEX_G 2
#define INDEX_B 3
#endif

/**
 * shell_screenshot_pick_color_finish:
 * @screenshot: the #ShellScreenshot
 * @result: the #GAsyncResult that was provided to the callback
 * @color: (out caller-allocates): the picked color
 * @error: #GError for error reporting
 *
 * Finish the asynchronous operation started by shell_screenshot_pick_color()
 * and obtain its result.
 *
 * Returns: whether the operation was successful
 *
 */
gboolean
shell_screenshot_pick_color_finish (ShellScreenshot  *screenshot,
                                    GAsyncResult     *result,
                                    ClutterColor     *color,
                                    GError          **error)
{
  ShellScreenshotPrivate *priv;

  g_return_val_if_fail (SHELL_IS_SCREENSHOT (screenshot), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (color != NULL, FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result,
                                                  shell_screenshot_pick_color),
                        FALSE);

  if (!g_task_propagate_boolean (G_TASK (result), error))
    return FALSE;

  priv = screenshot->priv;

  /* protect against mutter changing the format used for stage captures */
  g_assert (cairo_image_surface_get_format (priv->image) == CAIRO_FORMAT_ARGB32);

  if (color)
    {
      uint8_t *data = cairo_image_surface_get_data (priv->image);

      color->alpha = data[INDEX_A];
      color->red   = data[INDEX_R];
      color->green = data[INDEX_G];
      color->blue  = data[INDEX_B];
    }

  return TRUE;
}

#undef INDEX_A
#undef INDEX_R
#undef INDEX_G
#undef INDEX_B

ShellScreenshot *
shell_screenshot_new (void)
{
  return g_object_new (SHELL_TYPE_SCREENSHOT, NULL);
}
