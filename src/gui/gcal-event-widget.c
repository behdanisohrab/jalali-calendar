/* gcal-event-widget.c
 *
 * Copyright (C) 2015 Erick Pérez Castellanos <erickpc@gnome.org>
 *
 * gnome-calendar is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gnome-calendar is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define G_LOG_DOMAIN "GcalEventWidget"

#include <libecal/libecal.h>
#include <glib/gi18n.h>
#include <string.h>

#include "gcal-application.h"
#include "gcal-context.h"
#include "gcal-clock.h"
#include "gcal-debug.h"
#include "gcal-event-popover.h"
#include "gcal-event-widget.h"
#include "gcal-utils.h"

#define DESC_MAX_CHAR 200
#define INTENSITY(c)  ((c->red) * 0.30 + (c->green) * 0.59 + (c->blue) * 0.11)
#define ICON_SIZE     16

typedef struct
{
  GcalEventWidget *event_widget;
  GcalEventPreviewCallback callback;
  gpointer user_data;
} PreviewData;

struct _GcalEventWidget
{
  GtkWidget           parent;

  /* properties */
  GDateTime          *dt_start;
  GDateTime          *dt_end;

  /* widgets */
  GtkWidget          *color_box;
  GtkWidget          *horizontal_box;
  GtkWidget          *hour_label;
  GtkWidget          *stack;
  GtkWidget          *summary_label;
  GtkWidget          *vertical_box;
  GtkEventController *drag_source;

  /* internal data */
  gboolean            clock_format_24h : 1;
  gboolean            read_only : 1;
  gchar              *css_class;

  GcalEvent          *event;

  GtkOrientation      orientation;

  guint               vertical_label_source_id;
  gboolean            vertical_value_to_set;
  gboolean            vertical_labels;

  gint                old_width;
  gint                old_height;

  GcalContext        *context;
};

enum
{
  PROP_0,
  PROP_CONTEXT,
  PROP_DATE_END,
  PROP_DATE_START,
  PROP_EVENT,
  PROP_ORIENTATION,
  NUM_PROPS
};

enum
{
  ACTIVATE,
  NUM_SIGNALS
};

typedef enum
{
  CURSOR_NONE,
  CURSOR_GRAB,
  CURSOR_GRABBING
} CursorType;

static guint signals[NUM_SIGNALS] = { 0, };

G_DEFINE_TYPE_WITH_CODE (GcalEventWidget, gcal_event_widget, GTK_TYPE_WIDGET,
                         G_IMPLEMENT_INTERFACE (GTK_TYPE_ORIENTABLE, NULL));

/*
 * Auxiliary methods
 */

static gboolean
can_hold_vertical_labels_with_height (GcalEventWidget *self,
                                      gint             height)
{
  gint total_height;

  gtk_widget_measure (self->vertical_box,
                      GTK_ORIENTATION_VERTICAL,
                      -1,
                      &total_height,
                      NULL,
                      NULL,
                      NULL);

  return height >= total_height;
}

static void
set_vertical_labels (GcalEventWidget *self,
                     gboolean         vertical)
{
  if (self->vertical_labels == vertical)
    return;

  gtk_stack_set_visible_child (GTK_STACK (self->stack), vertical ? self->vertical_box : self->horizontal_box);
  self->vertical_labels = vertical;
}

static gboolean
set_vertical_labels_in_idle_cb (gpointer data)
{
  GcalEventWidget *self = (GcalEventWidget*) data;

  set_vertical_labels (self, self->vertical_value_to_set);

  self->vertical_label_source_id = 0;
  return G_SOURCE_REMOVE;
}

static void
queue_set_vertical_labels (GcalEventWidget *self,
                           gboolean         vertical)
{
  if (self->vertical_label_source_id > 0)
    {
      g_source_remove (self->vertical_label_source_id);
      self->vertical_label_source_id = 0;
    }

  if (self->vertical_labels == vertical)
    return;

  self->vertical_value_to_set = vertical;
  self->vertical_label_source_id = g_idle_add (set_vertical_labels_in_idle_cb, self);
}

static void
gcal_event_widget_update_style (GcalEventWidget *self)
{
  gboolean slanted_start;
  gboolean slanted_end;
  gboolean timed;

  slanted_start = FALSE;
  slanted_end = FALSE;

  /* Clear previous style classes */
  gtk_widget_remove_css_class (GTK_WIDGET (self), "slanted");
  gtk_widget_remove_css_class (GTK_WIDGET (self), "slanted-start");
  gtk_widget_remove_css_class (GTK_WIDGET (self), "slanted-end");

  /*
   * If the event's dates differs from the widget's dates,
   * add a slanted edge class at the widget.
   */

  if (self->dt_start)
    slanted_start = g_date_time_compare (gcal_event_get_date_start (self->event), self->dt_start) != 0;

  if (self->dt_end)
    slanted_end = g_date_time_compare (gcal_event_get_date_end (self->event), self->dt_end) != 0;

  if (slanted_start && slanted_end)
    gtk_widget_add_css_class (GTK_WIDGET (self), "slanted");
  else if (slanted_start)
    gtk_widget_add_css_class (GTK_WIDGET (self), "slanted-start");
  else if (slanted_end)
    gtk_widget_add_css_class (GTK_WIDGET (self), "slanted-end");

  /* Add style classes for orientation selectors */
  if (self->orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      gtk_widget_remove_css_class (GTK_WIDGET (self), "vertical");
      gtk_widget_add_css_class (GTK_WIDGET (self), "horizontal");
    }
  else
    {
      gtk_widget_remove_css_class (GTK_WIDGET (self), "horizontal");
      gtk_widget_add_css_class (GTK_WIDGET (self), "vertical");
    }

  /*
   * If the event is a timed, single-day event, draw it differently
   * from all-day or multi-day events.
   */
  timed = !gcal_event_get_all_day (self->event) && !gcal_event_is_multiday (self->event);

  gtk_widget_set_visible (self->color_box, timed);

  if (timed)
    {
      gtk_widget_add_css_class (GTK_WIDGET (self), "timed");

      if (self->orientation == GTK_ORIENTATION_HORIZONTAL)
        {
          gtk_widget_set_margin_start (self->stack, 0);
          gtk_widget_set_margin_end (self->stack, 2);
        }
      else
        {
          gtk_widget_set_visible (self->color_box, FALSE);
        }
    }
}

static void
update_color (GcalEventWidget *self)
{
  GdkRGBA *color;
  GQuark color_id;
  gchar *color_str;
  gchar *css_class;
  GDateTime *now;
  gint date_compare;

  color = gcal_event_get_color (self->event);
  now = g_date_time_new_now_local ();
  date_compare = g_date_time_compare (self->dt_end, now);

  /* Fades out an event that's earlier than the current date */
  gtk_widget_set_opacity (GTK_WIDGET (self), date_compare < 0 ? 0.6 : 1.0);

  /* Remove the old style class */
  if (self->css_class)
    {
      gtk_widget_remove_css_class (GTK_WIDGET (self), self->css_class);
      g_clear_pointer (&self->css_class, g_free);
    }

  color_str = gdk_rgba_to_string (color);
  color_id = g_quark_from_string (color_str);
  css_class = g_strdup_printf ("color-%d", color_id);

  gtk_widget_add_css_class (GTK_WIDGET (self), css_class);

  if (INTENSITY (color) > 0.5)
    {
      gtk_widget_remove_css_class (GTK_WIDGET (self), "color-dark");
      gtk_widget_add_css_class (GTK_WIDGET (self), "color-light");
    }
  else
    {
      gtk_widget_remove_css_class (GTK_WIDGET (self), "color-light");
      gtk_widget_add_css_class (GTK_WIDGET (self), "color-dark");
    }

  /* Keep the current style around, so we can remove it later */
  self->css_class = css_class;

  g_clear_pointer (&now, g_date_time_unref);
  g_free (color_str);
}

static void
gcal_event_widget_set_event_tooltip (GcalEventWidget *self,
                                     GcalEvent       *event)
{
  g_autoptr (GDateTime) tooltip_start, tooltip_end;
  g_autofree gchar *start, *end, *escaped_summary;
  GString *tooltip_mesg;
  gboolean allday, multiday, is_ltr;
  guint description_len;

  tooltip_mesg = g_string_new (NULL);
  escaped_summary = g_markup_escape_text (gcal_event_get_summary (event), -1);
  g_string_append_printf (tooltip_mesg, "<b>%s</b>", escaped_summary);

  allday = gcal_event_get_all_day (event);
  multiday = gcal_event_is_multiday (event);

  is_ltr = gtk_widget_get_direction (GTK_WIDGET (self)) != GTK_TEXT_DIR_RTL;

  if (allday)
    {
      /* All day events span from [ start, end - 1 day ] */
      tooltip_start = g_date_time_ref (gcal_event_get_date_start (event));
      tooltip_end = g_date_time_add_days (gcal_event_get_date_end (event), -1);

      if (multiday)
        {
          start = g_date_time_format (tooltip_start, "%x");
          end = g_date_time_format (tooltip_end, "%x");
        }
      else
        {
          start = g_date_time_format (tooltip_start, "%x");
          end = NULL;
        }
    }
  else
    {
      tooltip_start = g_date_time_to_local (gcal_event_get_date_start (event));
      tooltip_end = g_date_time_to_local (gcal_event_get_date_end (event));

      if (multiday)
        {
          if (self->clock_format_24h)
            {
              if (is_ltr)
                {
                  start = g_date_time_format (tooltip_start, "%x %R");
                  end = g_date_time_format (tooltip_end, "%x %R");
                }
              else
                {
                  start = g_date_time_format (tooltip_start, "%R %x");
                  end = g_date_time_format (tooltip_end, "%R %x");
                }
            }
          else
            {
              if (is_ltr)
                {
                  start = g_date_time_format (tooltip_start, "%x %I:%M %P");
                  end = g_date_time_format (tooltip_end, "%x %I:%M %P");
                }
              else
                {
                  start = g_date_time_format (tooltip_start, "%P %M:%I %x");
                  end = g_date_time_format (tooltip_end, "%P %M:%I %x");
                }
            }
        }
      else
        {
          if (self->clock_format_24h)
            {
              if (is_ltr)
                {
                  start = g_date_time_format (tooltip_start, "%x, %R");
                  end = g_date_time_format (tooltip_end, "%R");
                }
              else
                {
                  start = g_date_time_format (tooltip_start, "%R ,%x");
                  end = g_date_time_format (tooltip_end, "%R");
                }
            }
          else
            {
              if (is_ltr)
                {
                  start = g_date_time_format (tooltip_start, "%x, %I:%M %P");
                  end = g_date_time_format (tooltip_end, "%I:%M %P");
                }
              else
                {
                  start = g_date_time_format (tooltip_start, "%P %M:%I ,%x");
                  end = g_date_time_format (tooltip_end, "%P %M:%I");
                }
            }
        }
    }

  if (allday && !multiday)
    {
      g_string_append_printf (tooltip_mesg, "\n%s", start);
    }
  else
    {
      g_string_append_printf (tooltip_mesg,
                              "\n%s - %s",
                              is_ltr ? start : end,
                              is_ltr ? end : start);
    }

  /* Append event location */
  if (g_utf8_strlen (gcal_event_get_location (event), -1) > 0)
    {
      g_autofree gchar *escaped_location;

      escaped_location = g_markup_escape_text (gcal_event_get_location (event), -1);

      g_string_append (tooltip_mesg, "\n\n");

      /* Translators: %s is the location of the event (e.g. "Downtown, 3rd Avenue") */
      g_string_append_printf (tooltip_mesg, _("At %s"), escaped_location);
    }

  description_len = g_utf8_strlen (gcal_event_get_description (event), -1);

  /* Truncate long descriptions at a white space and ellipsize */
  if (description_len > 0)
    {
      g_autofree gchar *escaped_description;
      GString *tooltip_desc;

      tooltip_desc = g_string_new (gcal_event_get_description (event));

      /* If the description is larger than DESC_MAX_CHAR, ellipsize it */
      if (description_len > DESC_MAX_CHAR)
        {
          g_string_truncate (tooltip_desc, DESC_MAX_CHAR - 1);
          g_string_append (tooltip_desc, "…");
        }

      escaped_description = g_markup_escape_text (tooltip_desc->str, -1);

      g_string_append_printf (tooltip_mesg, "\n\n%s", escaped_description);

      g_string_free (tooltip_desc, TRUE);
    }

  gtk_widget_set_tooltip_markup (GTK_WIDGET (self), tooltip_mesg->str);

  g_string_free (tooltip_mesg, TRUE);
}

static gchar*
get_hour_label (GcalEventWidget *self)
{
  g_autoptr (GDateTime) local_start_time;

  local_start_time = g_date_time_to_local (gcal_event_get_date_start (self->event));

  if (self->clock_format_24h)
    return g_date_time_format (local_start_time, "%R");
  else
    return g_date_time_format (local_start_time, "%I:%M %P");
}

static void
gcal_event_widget_set_event_internal (GcalEventWidget *self,
                                      GcalEvent       *event)
{
  g_autofree gchar *hour_str = NULL;

  /*
   * This function is called only once, since the property is
   * set as CONSTRUCT_ONLY. Any other attempt to set an event
   * will be ignored.
   *
   * Because of that condition, we don't really have to care about
   * disconnecting functions or cleaning up the previous event.
   */

  /* The event spawns with a floating reference, and we take it's ownership */
  g_set_object (&self->event, event);

  /*
   * Initially, the widget's start and end dates are the same
   * of the event's ones. We may change it afterwards.
   */
  gcal_event_widget_set_date_start (self, gcal_event_get_date_start (event));
  gcal_event_widget_set_date_end (self, gcal_event_get_date_end (event));

  /* Update color */
  update_color (self);

  g_signal_connect_object (event,
                           "notify::color",
                           G_CALLBACK (update_color),
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (event,
                           "notify::summary",
                           G_CALLBACK (gtk_widget_queue_draw),
                           self,
                           G_CONNECT_SWAPPED);

  /* Tooltip */
  gcal_event_widget_set_event_tooltip (self, event);

  /* Hour label */
  hour_str = get_hour_label (self);

  gtk_widget_set_visible (self->hour_label, !gcal_event_get_all_day (event));
  gtk_label_set_label (GTK_LABEL (self->hour_label), hour_str);

  /* Summary label */
  g_object_bind_property (event,
                          "summary",
                          self->summary_label,
                          "label",
                          G_BINDING_DEFAULT | G_BINDING_SYNC_CREATE);
}

static void
reply_preview_callback (GtkWidget              *event_popover,
                        PreviewData            *data,
                        GcalEventPreviewAction  action)
{
  if (data->callback)
    data->callback (data->event_widget, action, data->user_data);

  g_signal_handlers_disconnect_by_data (event_popover, data);

  gtk_popover_popdown (GTK_POPOVER (event_popover));
  gtk_widget_unparent (event_popover);

  g_clear_pointer (&data, g_free);
}


/*
 * Callbacks
 */

static void
on_click_gesture_pressed_cb (GtkGestureClick *click_gesture,
                             gint             n_press,
                             gdouble          x,
                             gdouble          y,
                             GcalEventWidget *self)
{
  gtk_gesture_set_state (GTK_GESTURE (click_gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_click_gesture_release_cb (GtkGestureClick *click_gesture,
                             gint             n_press,
                             gdouble          x,
                             gdouble          y,
                             GcalEventWidget *self)
{
  gtk_gesture_set_state (GTK_GESTURE (click_gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  g_signal_emit (self, signals[ACTIVATE], 0);
}

static void
on_drag_source_begin_cb (GtkDragSource   *source,
                         GdkDrag         *drag,
                         GcalEventWidget *self)
{
  g_autoptr (GdkPaintable) paintable = NULL;

  paintable = gtk_widget_paintable_new (GTK_WIDGET (self));
  gtk_drag_source_set_icon (source, paintable, 0, 0);
}

static GdkContentProvider*
on_drag_source_prepare_cb (GtkDragSource   *source,
                           gdouble          x,
                           gdouble          y,
                           GcalEventWidget *self)
{
  return gdk_content_provider_new_typed (GCAL_TYPE_EVENT_WIDGET, self);
}

static void
on_event_popover_closed_cb (GtkWidget   *event_popover,
                            PreviewData *data)
{
  reply_preview_callback (event_popover, data, GCAL_EVENT_PREVIEW_ACTION_NONE);
}

static void
on_event_popover_edit_cb (GtkWidget   *event_popover,
                               PreviewData *data)
{
  reply_preview_callback (event_popover, data, GCAL_EVENT_PREVIEW_ACTION_EDIT);
}


/*
 * GtkWidget overrides
 */

static void
gcal_event_widget_size_allocate (GtkWidget *widget,
                                 gint       width,
                                 gint       height,
                                 gint       baseline)
{
  GcalEventWidget *self = GCAL_EVENT_WIDGET (widget);

  gtk_widget_allocate (GTK_WIDGET (self->stack), width, height, baseline, NULL);

  if (self->old_width != width || self->old_height != height)
    {
      if (self->orientation == GTK_ORIENTATION_HORIZONTAL || gcal_event_get_all_day (self->event))
        return;

      queue_set_vertical_labels (self, can_hold_vertical_labels_with_height (self, height));

      self->old_width = width;
      self->old_height = height;
    }
}

static void
gcal_event_widget_measure (GtkWidget      *widget,
                           GtkOrientation  orientation,
                           gint            for_size,
                           gint           *minimum,
                           gint           *natural,
                           gint           *minimum_baseline,
                           gint           *natural_baseline)
{
  GcalEventWidget *self = GCAL_EVENT_WIDGET (widget);

  gtk_widget_measure (GTK_WIDGET (self->stack),
                      orientation,
                      for_size,
                      minimum,
                      natural,
                      minimum_baseline,
                      natural_baseline);
}


/*
 * GObject overrides
 */

static void
gcal_event_widget_set_property (GObject      *object,
                                guint         property_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  GcalEventWidget *self = GCAL_EVENT_WIDGET (object);

  switch (property_id)
    {
    case PROP_CONTEXT:
      g_assert (self->context == NULL);
      self->context = g_value_dup_object (value);

      g_signal_connect_object (gcal_context_get_clock (self->context),
                               "minute-changed",
                               G_CALLBACK (update_color),
                               self,
                               G_CONNECT_SWAPPED);
      break;

    case PROP_DATE_END:
      gcal_event_widget_set_date_end (self, g_value_get_boxed (value));
      break;

    case PROP_DATE_START:
      gcal_event_widget_set_date_start (self, g_value_get_boxed (value));
      break;

    case PROP_EVENT:
      gcal_event_widget_set_event_internal (self, g_value_get_object (value));
      break;

    case PROP_ORIENTATION:
      self->orientation = g_value_get_enum (value);
      gtk_widget_set_visible (self->vertical_box, self->orientation == GTK_ORIENTATION_VERTICAL);
      gcal_event_widget_update_style (self);
      g_object_notify (object, "orientation");
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
gcal_event_widget_get_property (GObject      *object,
                                guint         property_id,
                                GValue       *value,
                                GParamSpec   *pspec)
{
  GcalEventWidget *self = GCAL_EVENT_WIDGET (object);

  switch (property_id)
    {
    case PROP_CONTEXT:
      g_value_set_object (value, self->context);
      break;

    case PROP_DATE_END:
      g_value_set_boxed (value, self->dt_end);
      break;

    case PROP_DATE_START:
      g_value_set_boxed (value, self->dt_start);
      break;

    case PROP_EVENT:
      g_value_set_object (value, self->event);
      break;

    case PROP_ORIENTATION:
      g_value_set_enum (value, self->orientation);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
gcal_event_widget_dispose (GObject *object)
{
  GcalEventWidget *self = GCAL_EVENT_WIDGET (object);

  g_clear_handle_id (&self->vertical_label_source_id, g_source_remove);
  g_clear_pointer (&self->stack, gtk_widget_unparent);

  G_OBJECT_CLASS (gcal_event_widget_parent_class)->dispose (object);
}

static void
gcal_event_widget_finalize (GObject *object)
{
  GcalEventWidget *self;

  self = GCAL_EVENT_WIDGET (object);

  /* disconnect signals */
  g_signal_handlers_disconnect_by_func (self->event, update_color, self);
  g_signal_handlers_disconnect_by_func (self->event, gtk_widget_queue_draw, self);

  /* releasing properties */
  g_clear_pointer (&self->css_class, g_free);
  g_clear_object (&self->event);
  g_clear_object (&self->context);

  G_OBJECT_CLASS (gcal_event_widget_parent_class)->finalize (object);
}

static void
gcal_event_widget_class_init (GcalEventWidgetClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->set_property = gcal_event_widget_set_property;
  object_class->get_property = gcal_event_widget_get_property;
  object_class->dispose = gcal_event_widget_dispose;
  object_class->finalize = gcal_event_widget_finalize;

  widget_class->measure = gcal_event_widget_measure;
  widget_class->size_allocate = gcal_event_widget_size_allocate;

  /**
   * GcalEventWidget::context:
   *
   * The context of the event.
   */
  g_object_class_install_property (object_class,
                                   PROP_CONTEXT,
                                   g_param_spec_object ("context",
                                                        "Context",
                                                        "Context",
                                                        GCAL_TYPE_CONTEXT,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS));
  /**
   * GcalEventWidget::date-end:
   *
   * The end date this widget represents. Notice that this may
   * differ from the event's end date. For example, if the event
   * spans more than one month and we're in Month View, the end
   * date marks the last day this event widget is visible.
   */
  g_object_class_install_property (object_class,
                                   PROP_DATE_END,
                                   g_param_spec_boxed ("date-end",
                                                       "End date",
                                                       "The end date of the widget",
                                                       G_TYPE_DATE_TIME,
                                                       G_PARAM_READWRITE));

  /**
   * GcalEventWidget::date-start:
   *
   * The start date this widget represents. Notice that this may
   * differ from the event's start date. For example, if the event
   * spans more than one month and we're in Month View, the start
   * date marks the first day this event widget is visible.
   */
  g_object_class_install_property (object_class,
                                   PROP_DATE_START,
                                   g_param_spec_boxed ("date-start",
                                                       "Start date",
                                                       "The start date of the widget",
                                                       G_TYPE_DATE_TIME,
                                                       G_PARAM_READWRITE));

  /**
   * GcalEventWidget::event:
   *
   * The event this widget represents.
   */
  g_object_class_install_property (object_class,
                                   PROP_EVENT,
                                   g_param_spec_object ("event",
                                                        "Event",
                                                        "The event this widget represents",
                                                        GCAL_TYPE_EVENT,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  /**
   * GcalEventWidget::orientation:
   *
   * The orientation of the event widget.
   */
  g_object_class_override_property (object_class, PROP_ORIENTATION, "orientation");

  signals[ACTIVATE] = g_signal_new ("activate",
                                     GCAL_TYPE_EVENT_WIDGET,
                                     G_SIGNAL_RUN_LAST,
                                     0,
                                     NULL, NULL,
                                     g_cclosure_marshal_VOID__VOID,
                                     G_TYPE_NONE,
                                     0);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/calendar/ui/gui/gcal-event-widget.ui");

  gtk_widget_class_bind_template_child (widget_class, GcalEventWidget, color_box);
  gtk_widget_class_bind_template_child (widget_class, GcalEventWidget, drag_source);
  gtk_widget_class_bind_template_child (widget_class, GcalEventWidget, horizontal_box);
  gtk_widget_class_bind_template_child (widget_class, GcalEventWidget, hour_label);
  gtk_widget_class_bind_template_child (widget_class, GcalEventWidget, stack);
  gtk_widget_class_bind_template_child (widget_class, GcalEventWidget, summary_label);
  gtk_widget_class_bind_template_child (widget_class, GcalEventWidget, vertical_box);

  gtk_widget_class_bind_template_callback (widget_class, on_click_gesture_pressed_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_click_gesture_release_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_drag_source_begin_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_drag_source_prepare_cb);

  gtk_widget_class_set_css_name (widget_class, "event");
}

static void
gcal_event_widget_init (GcalEventWidget *self)
{
  GtkWidget *widget;

  widget = GTK_WIDGET (self);
  self->clock_format_24h = is_clock_format_24h ();

  gtk_widget_init_template (widget);

  gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "pointer");

  /* Starts with horizontal */
  self->orientation = GTK_ORIENTATION_HORIZONTAL;
  gtk_widget_add_css_class (widget, "horizontal");
}

GtkWidget*
gcal_event_widget_new (GcalContext *context,
                       GcalEvent   *event)
{
  return g_object_new (GCAL_TYPE_EVENT_WIDGET,
                       "context", context,
                       "event", event,
                       NULL);
}

void
gcal_event_widget_set_read_only (GcalEventWidget *event,
                                 gboolean         read_only)
{
  g_return_if_fail (GCAL_IS_EVENT_WIDGET (event));

  gtk_event_controller_set_propagation_phase (event->drag_source,
                                              read_only ? GTK_PHASE_NONE : GTK_PHASE_BUBBLE);

  event->read_only = read_only;
}

/**
 * gcal_event_widget_get_date_end:
 * @self: a #GcalEventWidget
 *
 * Retrieves the visible end date of this widget. This
 * may differ from the event's end date.
 *
 * Returns: (transfer none): a #GDateTime
 */
GDateTime*
gcal_event_widget_get_date_end (GcalEventWidget *self)
{
  g_return_val_if_fail (GCAL_IS_EVENT_WIDGET (self), NULL);

  return self->dt_end ? self->dt_end : self->dt_start;
}

/**
 * gcal_event_widget_set_date_end:
 * @self: a #GcalEventWidget
 * @date_end: the end date of this widget
 *
 * Sets the visible end date of this widget. This
 * may differ from the event's end date, but cannot
 * be after it.
 */
void
gcal_event_widget_set_date_end (GcalEventWidget *self,
                                GDateTime       *date_end)
{
  g_return_if_fail (GCAL_IS_EVENT_WIDGET (self));

  if (self->dt_end != date_end &&
      (!self->dt_end || !date_end ||
       (self->dt_end && date_end && !g_date_time_equal (self->dt_end, date_end))))
    {
      /* The end date should never be after the event's end date */
      if (date_end && g_date_time_compare (date_end, gcal_event_get_date_end (self->event)) > 0)
        return;

      g_clear_pointer (&self->dt_end, g_date_time_unref);
      self->dt_end = g_date_time_ref (date_end);

      gcal_event_widget_update_style (self);

      g_object_notify (G_OBJECT (self), "date-end");
    }
}

/**
 * gcal_event_widget_get_date_start:
 * @self: a #GcalEventWidget
 *
 * Retrieves the visible start date of this widget. This
 * may differ from the event's start date.
 *
 * Returns: (transfer none): a #GDateTime
 */
GDateTime*
gcal_event_widget_get_date_start (GcalEventWidget *self)
{
  g_return_val_if_fail (GCAL_IS_EVENT_WIDGET (self), NULL);

  return self->dt_start;
}

/**
 * gcal_event_widget_set_date_start:
 * @self: a #GcalEventWidget
 * @date_end: the start date of this widget
 *
 * Sets the visible start date of this widget. This
 * may differ from the event's start date, but cannot
 * be before it.
 */
void
gcal_event_widget_set_date_start (GcalEventWidget *self,
                                  GDateTime       *date_start)
{
  g_return_if_fail (GCAL_IS_EVENT_WIDGET (self));

  if (self->dt_start != date_start &&
      (!self->dt_start || !date_start ||
       (self->dt_start && date_start && !g_date_time_equal (self->dt_start, date_start))))
    {
      /* The start date should never be before the event's start date */
      if (date_start && g_date_time_compare (date_start, gcal_event_get_date_start (self->event)) < 0)
        return;

      g_clear_pointer (&self->dt_start, g_date_time_unref);
      self->dt_start = g_date_time_ref (date_start);

      gcal_event_widget_update_style (self);

      g_object_notify (G_OBJECT (self), "date-start");
    }
}

/**
 * gcal_event_widget_show_preview:
 * @self: a #GcalEventWidget
 * @callback: (nullable): callback for the event preview
 * @user_data: (nullable): user data for @callback
 *
 * Shows an event preview popover for this event widget, and
 * calls @callback when the popover is either dismissed, or
 * the edit button is clicked.
 */
void
gcal_event_widget_show_preview (GcalEventWidget          *self,
                                GcalEventPreviewCallback  callback,
                                gpointer                  user_data)
{
  PreviewData *data;
  GtkWidget *event_popover;

  GCAL_ENTRY;

  data = g_new0 (PreviewData, 1);
  data->event_widget = self;
  data->callback = callback;
  data->user_data = user_data;

  event_popover = gcal_event_popover_new (self->context, self->event);
  gtk_widget_set_parent (event_popover, GTK_WIDGET (self));
  g_signal_connect (event_popover, "closed", G_CALLBACK (on_event_popover_closed_cb), data);
  g_signal_connect (event_popover, "edit", G_CALLBACK (on_event_popover_edit_cb), data);
  gtk_popover_popup (GTK_POPOVER (event_popover));

  GCAL_EXIT;
}

/**
 * gcal_event_widget_get_event:
 * @self: a #GcalEventWidget
 *
 * Retrieves the @GcalEvent this widget represents.
 *
 * Returns: (transfer none): a #GcalEvent
 */
GcalEvent*
gcal_event_widget_get_event (GcalEventWidget *self)
{
  g_return_val_if_fail (GCAL_IS_EVENT_WIDGET (self), NULL);

  return self->event;
}

GtkWidget*
gcal_event_widget_clone (GcalEventWidget *widget)
{
  GtkWidget *new_widget;

  new_widget = gcal_event_widget_new (widget->context, widget->event);
  gcal_event_widget_set_read_only (GCAL_EVENT_WIDGET (new_widget), widget->read_only);

  return new_widget;
}

/**
 * gcal_event_widget_equal:
 * @widget1: an #GcalEventWidget representing an event
 * @widget2: an #GcalEventWidget representing an event
 *
 * Check if two widget represent the same event.
 *
 * Returns: %TRUE if both widget represent the same event,
 *          false otherwise
 **/
gboolean
gcal_event_widget_equal (GcalEventWidget *widget1,
                         GcalEventWidget *widget2)
{
  return g_strcmp0 (gcal_event_get_uid (widget1->event), gcal_event_get_uid (widget2->event)) == 0;
}

/**
 * gcal_event_widget_compare_by_length:
 * @widget1:
 * @widget2:
 *
 * Compare two widgets by the duration of the events they represent. From shortest to longest span.
 *
 * Returns: negative value if a < b ; zero if a = b ; positive value if a > b
 **/
gint
gcal_event_widget_compare_by_length (GcalEventWidget *widget1,
                                     GcalEventWidget *widget2)
{
  time_t time_s1, time_s2;
  time_t time_e1, time_e2;

  time_e1 = time_s1 = g_date_time_to_unix (widget1->dt_start);
  time_e2 = time_s2 = g_date_time_to_unix (widget2->dt_start);

  if (widget1->dt_end != NULL)
    time_e1 = g_date_time_to_unix (widget1->dt_end);
  if (widget2->dt_end)
    time_e2 = g_date_time_to_unix (widget2->dt_end);

  return (time_e2 - time_s2) - (time_e1 - time_s1);
}

gint
gcal_event_widget_compare_by_start_date (GcalEventWidget *widget1,
                                         GcalEventWidget *widget2)
{
  return g_date_time_compare (widget1->dt_start, widget2->dt_start);
}

gint
gcal_event_widget_sort_events (GcalEventWidget *widget1,
                               GcalEventWidget *widget2)
{
  g_autoptr (GDateTime) dt_time1 = NULL;
  g_autoptr (GDateTime) dt_time2 = NULL;
  ICalTime *itt;
  gint diff;

  diff = gcal_event_is_multiday (widget2->event) - gcal_event_is_multiday (widget1->event);

  if (diff != 0)
    return diff;

  diff = gcal_event_widget_compare_by_start_date (widget1, widget2);

  if (diff != 0)
    return diff;

  diff = gcal_event_widget_compare_by_length (widget1, widget2);

  if (diff != 0)
    return diff;

  itt = e_cal_component_get_last_modified (gcal_event_get_component (widget1->event));
  if (itt)
    dt_time1 = gcal_date_time_from_icaltime (itt);
  g_clear_object (&itt);

  itt = e_cal_component_get_last_modified (gcal_event_get_component (widget2->event));
  if (itt)
    dt_time2 = gcal_date_time_from_icaltime (itt);
  g_clear_object (&itt);

  return dt_time1 && dt_time2 ? g_date_time_compare (dt_time2, dt_time1) : 0;
}
