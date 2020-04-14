/* gcal-timeline.c
 *
 * Copyright 2020 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "GcalTimeline"

#include "gcal-calendar.h"
#include "gcal-calendar-monitor.h"
#include "gcal-context.h"
#include "gcal-date-time-utils.h"
#include "gcal-debug.h"
#include "gcal-event.h"
#include "gcal-range-tree.h"
#include "gcal-timeline.h"
#include "gcal-timeline-subscriber.h"

#include <libedataserver/libedataserver.h>

#define BATCH_SIZE 5

typedef enum
{
  ADD_EVENT,
  UPDATE_EVENT,
  REMOVE_EVENT,
} QueueEvent;

typedef struct
{
  GcalTimeline       *timeline;

  QueueEvent          queue_event;

  GPtrArray          *subscribers;
  GcalEvent          *event;
  GcalEvent          *old_event;
  gboolean            update_range_tree;
} QueueData;

typedef struct
{
  GSource             parent;
  GcalTimeline       *timeline;
} TimelineSource;

struct _GcalTimeline
{
  GObject             parent_instance;

  gulong              update_range_idle_id;
  GcalRange          *range;

  GcalRangeTree      *events;
  gchar              *filter;

  GHashTable         *calendars; /* GcalCalendar* -> GcalCalendarMonitor* */
  gint                completed_calendars;

  GHashTable         *subscribers; /* GcalTimelineSubscriber* -> SubscriberData* */
  GcalRangeTree      *subscriber_ranges;

  GCancellable       *cancellable;

  GQueue             *event_queue;
  GSource            *timeline_source;

  GcalContext        *context;
};

G_DEFINE_TYPE (GcalTimeline, gcal_timeline, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_COMPLETE,
  PROP_CONTEXT,
  PROP_FILTER,
  N_PROPS
};

static GParamSpec *properties [N_PROPS] = { NULL, };


/*
 * Auxiliary methods
 */

static void
queue_data_free (QueueData *queue_data)
{
  g_clear_pointer (&queue_data->subscribers, g_ptr_array_unref);
  g_clear_object (&queue_data->old_event);
  g_clear_object (&queue_data->event);
  g_free (queue_data);
}

static void
queue_event_data (GcalTimeline *self,
                  QueueEvent    queue_event,
                  GPtrArray    *subscribers,
                  GcalEvent    *event,
                  GcalEvent    *old_event,
                  gboolean      update_range_tree)
{
  QueueData *queue_data;

  queue_data = g_new0 (QueueData, 1);
  queue_data->timeline = self;
  queue_data->queue_event = queue_event;
  queue_data->subscribers = subscribers ? g_ptr_array_ref (subscribers) : NULL;
  queue_data->event = event ? g_object_ref (event) : NULL;
  queue_data->old_event = old_event ? g_object_ref (old_event) : NULL;
  queue_data->update_range_tree = update_range_tree;

  g_queue_push_tail (self->event_queue, queue_data);
}

static inline gboolean
is_timeline_complete (GcalTimeline *self)
{
  return self->completed_calendars == g_hash_table_size (self->calendars);
}

static void
reset_completed_calendars (GcalTimeline *self)
{
  gboolean was_complete = is_timeline_complete (self);

  self->completed_calendars = 0;
  if (is_timeline_complete (self) != was_complete)
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_COMPLETE]);
}

static void
increase_completed_calendars (GcalTimeline *self)
{
  self->completed_calendars++;

  g_assert (self->completed_calendars <= g_hash_table_size (self->calendars));

  if (is_timeline_complete (self))
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_COMPLETE]);
}

static void
add_event_to_subscriber (GcalTimelineSubscriber *subscriber,
                         GcalEvent              *event)
{
  GCAL_TRACE_MSG ("Adding event %s to subscriber %s",
                  gcal_event_get_uid (event),
                  G_OBJECT_TYPE_NAME (subscriber));

  GCAL_TIMELINE_SUBSCRIBER_GET_IFACE (subscriber)->add_event (subscriber, event);
}

static void
update_subscriber_event (GcalTimelineSubscriber *subscriber,
                         GcalEvent              *event)
{
  GCAL_TRACE_MSG ("Updating event '%s' (%s) at subscriber %s",
                  gcal_event_get_summary (event),
                  gcal_event_get_uid (event),
                  G_OBJECT_TYPE_NAME (subscriber));

  GCAL_TIMELINE_SUBSCRIBER_GET_IFACE (subscriber)->update_event (subscriber, event);
}
static void
remove_event_from_subscriber (GcalTimelineSubscriber *subscriber,
                              GcalEvent              *event)
{
  GCAL_TRACE_MSG ("Removing event '%s' (%s) from subscriber %s",
                  gcal_event_get_summary (event),
                  gcal_event_get_uid (event),
                  G_OBJECT_TYPE_NAME (subscriber));

  GCAL_TIMELINE_SUBSCRIBER_GET_IFACE (subscriber)->remove_event (subscriber, event);
}

static void
update_range (GcalTimeline *self)
{
  GcalTimelineSubscriber *subscriber;
  GHashTableIter iter;
  gboolean has_subscribers;
  gboolean range_changed;

  GCAL_ENTRY;

  range_changed = FALSE;
  has_subscribers = g_hash_table_size (self->subscribers) > 0;

  if (has_subscribers)
    {
      g_autoptr (GcalRange) new_range = NULL;

      g_hash_table_iter_init (&iter, self->subscribers);
      while (g_hash_table_iter_next (&iter, (gpointer*) &subscriber, NULL))
        {
          g_autoptr (GcalRange) subscriber_range = NULL;
          g_autoptr (GcalRange) union_range = NULL;

          subscriber_range = gcal_timeline_subscriber_get_range (subscriber);

          if (new_range)
            {
              union_range = gcal_range_union (subscriber_range, new_range);

              g_clear_pointer (&new_range, gcal_range_unref);
              new_range = g_steal_pointer (&union_range);
            }
          else
            {
              new_range = g_steal_pointer (&subscriber_range);
            }
        }

      if (!self->range || gcal_range_compare (self->range, new_range) != 0)
        {
          g_clear_pointer (&self->range, gcal_range_unref);
          self->range = g_steal_pointer (&new_range);
          range_changed = TRUE;
        }

    }
  else if (self->range)
    {
      g_clear_pointer (&self->range, gcal_range_unref);
      range_changed = TRUE;
    }

  if (range_changed)
    {
      GcalCalendarMonitor *monitor;

      reset_completed_calendars (self);

      g_hash_table_iter_init (&iter, self->calendars);
      while (g_hash_table_iter_next (&iter, NULL, (gpointer*) &monitor))
        gcal_calendar_monitor_set_range (monitor, self->range);
    }

  GCAL_EXIT;
}

static void
calculate_changed_events (GcalTimeline            *self,
                          GcalTimelineSubscriber  *subscriber,
                          GcalRange               *old_range,
                          GcalRange               *new_range)
{
  g_autoptr (GPtrArray) subscriber_array = NULL;
  g_autoptr (GPtrArray) events_to_remove = NULL;
  g_autoptr (GPtrArray) events_to_add = NULL;
  g_autoptr (GDateTime) old_range_start = NULL;
  g_autoptr (GDateTime) old_range_end = NULL;
  g_autoptr (GDateTime) new_range_start = NULL;
  g_autoptr (GDateTime) new_range_end = NULL;
  gint range_diff;
  gint i;

  events_to_add = g_ptr_array_new ();
  events_to_remove = g_ptr_array_new ();

  old_range_start = gcal_range_get_start (old_range);
  old_range_end = gcal_range_get_end (old_range);
  new_range_start = gcal_range_get_start (new_range);
  new_range_end = gcal_range_get_end (new_range);

  /* Start ranges diff */
  range_diff = g_date_time_compare (old_range_start, new_range_start);
  if (range_diff != 0)
    {
      g_autoptr (GPtrArray) events = NULL;

      if (range_diff < 0)
        {
          g_autoptr (GcalRange) range = gcal_range_new (old_range_start, new_range_start, GCAL_RANGE_DEFAULT);

          /* Removed */
          events = gcal_range_tree_get_data_at_range (self->events, range);
          if (events)
            g_ptr_array_extend (events_to_remove, events, NULL, NULL);
        }
      else if (range_diff > 0)
        {
          g_autoptr (GcalRange) range = gcal_range_new (new_range_start, old_range_start, GCAL_RANGE_DEFAULT);

          /* Added */
          events = gcal_range_tree_get_data_at_range (self->events, range);
          if (events)
            g_ptr_array_extend (events_to_add, events, NULL, NULL);
        }
    }

  /* End ranges diff */
  range_diff = g_date_time_compare (old_range_end, new_range_end);
  if (range_diff != 0)
    {
      g_autoptr (GPtrArray) events = NULL;

      if (range_diff < 0)
        {
          g_autoptr (GcalRange) range = gcal_range_new (old_range_end, new_range_end, GCAL_RANGE_DEFAULT);

          events = gcal_range_tree_get_data_at_range (self->events, range);
          if (events)
            g_ptr_array_extend (events_to_add, events, NULL, NULL);
        }
      else if (range_diff > 0)
        {
          g_autoptr (GcalRange) range = gcal_range_new (new_range_end, old_range_end, GCAL_RANGE_DEFAULT);

          events = gcal_range_tree_get_data_at_range (self->events, range);
          if (events)
            g_ptr_array_extend (events_to_remove, events, NULL, NULL);
        }
    }

  subscriber_array = g_ptr_array_new ();
  g_ptr_array_add (subscriber_array, subscriber);

  for (i = 0; i < events_to_remove->len; i++)
    {
      GcalEvent *event = g_ptr_array_index (events_to_remove, i);

      GCAL_TRACE_MSG ("Removing event from subscriber %s due to time range change (event: '%s' (%s))",
                      G_OBJECT_TYPE_NAME (subscriber),
                      gcal_event_get_summary (event),
                      gcal_event_get_uid (event));

      /* XXX: remove immediately, but add in idle */
      remove_event_from_subscriber (subscriber, event);
    }

  for (i = 0; i < events_to_add->len; i++)
    {
      GcalEvent *event = g_ptr_array_index (events_to_add, i);

      GCAL_TRACE_MSG ("Queueing event addition for subscriber %s (event: '%s' (%s))",
                      G_OBJECT_TYPE_NAME (subscriber),
                      gcal_event_get_summary (event),
                      gcal_event_get_uid (event));

      queue_event_data (self, ADD_EVENT, subscriber_array, event, NULL, FALSE);
    }
}

static void
add_cached_events_to_subscriber (GcalTimeline           *self,
                                 GcalTimelineSubscriber *subscriber)
{
  g_autoptr (GcalRange) subscriber_range = NULL;
  g_autoptr (GPtrArray) subscriber_array = NULL;
  g_autoptr (GPtrArray) events_to_add = NULL;
  gint i;

  GCAL_ENTRY;

  subscriber_range = gcal_timeline_subscriber_get_range (subscriber);

  events_to_add = gcal_range_tree_get_data_at_range (self->events, subscriber_range);

  subscriber_array = g_ptr_array_new ();
  g_ptr_array_add (subscriber_array, subscriber);

  for (i = 0; events_to_add && i < events_to_add->len; i++)
    {
      GcalEvent *event = g_ptr_array_index (events_to_add, i);

      GCAL_TRACE_MSG ("Queueing event addition for subscriber %s (event: '%s' (%s))",
                      G_OBJECT_TYPE_NAME (subscriber),
                      gcal_event_get_summary (event),
                      gcal_event_get_uid (event));

      queue_event_data (self, ADD_EVENT, subscriber_array, event, NULL, FALSE);
    }

  GCAL_EXIT;
}

static void
update_subscriber_range (GcalTimeline           *self,
                         GcalTimelineSubscriber *subscriber)
{
  g_autoptr (GcalRange) old_range = NULL;
  g_autoptr (GcalRange) new_range = NULL;

  GCAL_ENTRY;

  old_range = g_hash_table_lookup (self->subscribers, subscriber);
  g_assert (old_range != NULL);

  new_range = gcal_timeline_subscriber_get_range (subscriber);

  /* Diff new and old event ranges */
  calculate_changed_events (self, subscriber, old_range, new_range);

  /* Update the subscriber range */
  gcal_range_tree_remove_data (self->subscriber_ranges, subscriber);
  gcal_range_tree_add_range (self->subscriber_ranges, new_range, subscriber);

  g_hash_table_insert (self->subscribers, g_object_ref (subscriber), g_steal_pointer (&new_range));
  g_steal_pointer (&old_range);

  GCAL_EXIT;
}

static void
update_calendar_monitor_filters (GcalTimeline *self)
{
  GcalCalendarMonitor *monitor;
  GHashTableIter iter;

  reset_completed_calendars (self);

  g_hash_table_iter_init (&iter, self->calendars);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer*) &monitor))
    gcal_calendar_monitor_set_filter (monitor, self->filter);
}


/*
 * Callbacks
 */

static void
on_calendar_monitor_event_added_cb (GcalCalendarMonitor *monitor,
                                    GcalEvent           *event,
                                    GcalTimeline        *self)
{
  g_autoptr (GPtrArray) subscribers_at_range = NULL;
  GcalRange *event_range;

  GCAL_ENTRY;

  event_range = gcal_event_get_range (event);

  /* Add to all subscribers within the event range */
  subscribers_at_range = gcal_range_tree_get_data_at_range (self->subscriber_ranges, event_range);

  if (subscribers_at_range)
    queue_event_data (self, ADD_EVENT, subscribers_at_range, event, NULL, TRUE);

  GCAL_EXIT;
}

static void
on_calendar_monitor_event_updated_cb (GcalCalendarMonitor *monitor,
                                      GcalEvent           *old_event,
                                      GcalEvent           *event,
                                      GcalTimeline        *self)
{
  g_autoptr (GPtrArray) subscribers_at_range = NULL;
  GcalRange *event_range;

  GCAL_ENTRY;

  /* Add the updated event to the (potentially new) range */
  event_range = gcal_event_get_range (event);

  /* Add to all subscribers within the event range */
  subscribers_at_range = gcal_range_tree_get_data_at_range (self->subscriber_ranges, event_range);

  if (subscribers_at_range)
    queue_event_data (self, UPDATE_EVENT, subscribers_at_range, event, old_event, TRUE);

  GCAL_EXIT;
}

static void
on_calendar_monitor_event_removed_cb (GcalCalendarMonitor *monitor,
                                      GcalEvent           *event,
                                      GcalTimeline        *self)
{
  g_autoptr (GPtrArray) subscribers_at_range = NULL;
  GcalRange *event_range;

  GCAL_ENTRY;

  event_range = gcal_event_get_range (event);

  /* Add to all subscribers within the event range */
  subscribers_at_range = gcal_range_tree_get_data_at_range (self->subscriber_ranges, event_range);

  if (subscribers_at_range)
    queue_event_data (self, REMOVE_EVENT, subscribers_at_range, event, NULL, TRUE);

  GCAL_EXIT;
}

static void
on_calendar_monitor_completed_cb (GcalCalendarMonitor *monitor,
                                  GcalTimeline        *self)
{
  GCAL_ENTRY;

  increase_completed_calendars (self);

  GCAL_EXIT;
}

static gboolean
update_timeline_range_in_idle_cb (gpointer user_data)
{
  GcalTimeline *self;

  GCAL_ENTRY;

  self = GCAL_TIMELINE (user_data);

  update_range (self);

  self->update_range_idle_id = 0;

  GCAL_RETURN (G_SOURCE_REMOVE);
}

static void
on_subscriber_range_changed_cb (GcalTimelineSubscriber *subscriber,
                                GcalTimeline           *self)
{
  update_subscriber_range (self, subscriber);

  if (self->update_range_idle_id)
    return;

  self->update_range_idle_id = g_idle_add (update_timeline_range_in_idle_cb, self);
}

static gboolean
timeline_source_prepare (GSource *source,
                         gint    *timeout)
{
  TimelineSource *timeline_source;
  GcalTimeline *self;

  timeline_source = (TimelineSource*) source;
  self = timeline_source->timeline;

  return self->event_queue->length > 0;
}

static gboolean
timeline_source_dispatch (GSource     *source,
                          GSourceFunc  callback,
                          gpointer     user_data)
{
  TimelineSource *timeline_source;
  GcalTimeline *self;
  gint processed_events;

  GCAL_ENTRY;

  processed_events = 0;
  timeline_source = (TimelineSource*) source;
  self = timeline_source->timeline;

  while (processed_events < BATCH_SIZE && !g_queue_is_empty (self->event_queue))
    {
      GcalRange *event_range;
      QueueData *queue_data;
      GcalEvent *event;
      gint i;

      queue_data = g_queue_pop_head (self->event_queue);

      event = queue_data->event;
      event_range = gcal_event_get_range (event);

      switch (queue_data->queue_event)
        {
        case ADD_EVENT:
          GCAL_TRACE_MSG ("Processing ADD_EVENT for event '%s' (%s)",
                          gcal_event_get_summary (event),
                          gcal_event_get_uid (event));

          if (queue_data->update_range_tree)
            gcal_range_tree_add_range (self->events, event_range, g_object_ref (event));

          for (i = 0; queue_data->subscribers && i < queue_data->subscribers->len; i++)
            {
              GcalTimelineSubscriber *subscriber = g_ptr_array_index (queue_data->subscribers, i);

              add_event_to_subscriber (subscriber, event);
            }
          break;

        case UPDATE_EVENT:
          {
            GCAL_TRACE_MSG ("Processing UPDATE_EVENT for event '%s' (%s)",
                            gcal_event_get_summary (event),
                            gcal_event_get_uid (event));

            if (queue_data->update_range_tree)
              {
                GcalRange *old_event_range;

                /* Remove the old event */
                old_event_range = gcal_event_get_range (queue_data->old_event);

                gcal_range_tree_remove_range (self->events, old_event_range, queue_data->old_event);
                gcal_range_tree_add_range (self->events, event_range, g_object_ref (event));
              }

            for (i = 0; queue_data->subscribers && i < queue_data->subscribers->len; i++)
              {
                GcalTimelineSubscriber *subscriber = g_ptr_array_index (queue_data->subscribers, i);

                update_subscriber_event (subscriber, event);
              }
          }
          break;

        case REMOVE_EVENT:
          GCAL_TRACE_MSG ("Processing REMOVE_EVENT for event '%s' (%s)",
                          gcal_event_get_summary (event),
                          gcal_event_get_uid (event));

          for (i = 0; queue_data->subscribers && i < queue_data->subscribers->len; i++)
            {
              GcalTimelineSubscriber *subscriber = g_ptr_array_index (queue_data->subscribers, i);
              remove_event_from_subscriber (subscriber, event);
            }

          if (queue_data->update_range_tree)
            gcal_range_tree_remove_range (self->events, event_range, event);
          break;
        }

      queue_data_free (queue_data);

      processed_events++;
    }

  GCAL_RETURN (G_SOURCE_CONTINUE);
}

static GSourceFuncs timeline_source_funcs =
{
  timeline_source_prepare,
  NULL,
  timeline_source_dispatch,
  NULL,
};

/*
 * GObject overrides
 */

static void
gcal_timeline_finalize (GObject *object)
{
  GcalTimeline *self = (GcalTimeline *)object;

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  if (self->update_range_idle_id > 0)
    {
      g_source_remove (self->update_range_idle_id);
      self->update_range_idle_id = 0;
    }

  g_clear_pointer (&self->events, gcal_range_tree_unref);
  g_clear_pointer (&self->calendars, g_hash_table_destroy);
  g_clear_pointer (&self->subscribers, g_hash_table_destroy);
  g_clear_pointer (&self->subscriber_ranges, gcal_range_tree_unref);
  g_clear_pointer (&self->timeline_source, g_source_destroy);

  if (self->event_queue)
    {
      g_queue_free_full (self->event_queue, (GDestroyNotify) queue_data_free);
      self->event_queue = NULL;
    }

  G_OBJECT_CLASS (gcal_timeline_parent_class)->finalize (object);
}

static void
gcal_timeline_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  GcalTimeline *self = GCAL_TIMELINE (object);

  switch (prop_id)
    {
    case PROP_COMPLETE:
      g_value_set_boolean (value, is_timeline_complete (self));
      break;

    case PROP_CONTEXT:
      g_value_set_object (value, self->context);
      break;

    case PROP_FILTER:
      g_value_set_string (value, self->filter);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gcal_timeline_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  GcalTimeline *self = GCAL_TIMELINE (object);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      g_assert (self->context == NULL);
      self->context = g_value_get_object (value);
      break;

    case PROP_FILTER:
      gcal_timeline_set_filter (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gcal_timeline_class_init (GcalTimelineClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gcal_timeline_finalize;
  object_class->get_property = gcal_timeline_get_property;
  object_class->set_property = gcal_timeline_set_property;

  /**
   * GcalTimeline::complete:
   *
   * Whether all calendars in this timeline are completed.
   */
  properties[PROP_COMPLETE] = g_param_spec_boolean ("complete",
                                                    "Complete",
                                                    "Complete",
                                                    FALSE,
                                                    G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  /**
   * GcalSearchEngine::context:
   *
   * The #GcalContext of the application.
   */
  properties[PROP_CONTEXT] = g_param_spec_object ("context",
                                                  "Data context",
                                                  "Data context",
                                                  GCAL_TYPE_CONTEXT,
                                                  G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  /**
   * GcalTimeline::filter:
   *
   * The search filter.
   */
  properties[PROP_FILTER] = g_param_spec_string ("filter",
                                                 "Filter",
                                                 "Filter",
                                                 NULL,
                                                 G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
gcal_timeline_init (GcalTimeline *self)
{
  TimelineSource *timeline_source;

  self->cancellable = g_cancellable_new ();
  self->events = gcal_range_tree_new_with_free_func (g_object_unref);
  self->calendars = g_hash_table_new_full (NULL, NULL, NULL, g_object_unref);
  self->subscribers = g_hash_table_new_full (NULL, NULL, g_object_unref, (GDestroyNotify) gcal_range_unref);
  self->subscriber_ranges = gcal_range_tree_new ();
  self->event_queue = g_queue_new ();

  /* Timeline source */
  timeline_source = (TimelineSource*) g_source_new (&timeline_source_funcs, sizeof (TimelineSource));
  timeline_source->timeline = self;

  self->timeline_source = (GSource*) timeline_source;
  g_source_set_name (self->timeline_source, "Timeline Source");
  g_source_set_priority (self->timeline_source, G_PRIORITY_DEFAULT_IDLE);
  g_source_attach (self->timeline_source, g_main_context_default ());
}

/**
 * gcal_timeline_new:
 * @context: a #GcalContext
 *
 * Creates a new #GcalTimeline.
 *
 * Returns: (transfer full): a #GcalTimeline
 */
GcalTimeline*
gcal_timeline_new (GcalContext *context)
{
  return g_object_new (GCAL_TYPE_TIMELINE,
                       "context", context,
                       NULL);
}

/**
 * gcal_timeline_add_calendar:
 * @self: a #GcalTimeline
 * @calendar: a #GcalCalendar
 *
 * Adds @calendar to @self. The events in @calendar will be added
 * respectively to
 *
 */
void
gcal_timeline_add_calendar (GcalTimeline *self,
                            GcalCalendar *calendar)
{
  g_autoptr (GcalCalendarMonitor) monitor = NULL;

  g_return_if_fail (GCAL_IS_TIMELINE (self));
  g_return_if_fail (GCAL_IS_CALENDAR (calendar));

  GCAL_ENTRY;

  if (g_hash_table_contains (self->calendars, calendar))
    return;

  GCAL_TRACE_MSG ("Adding calendar '%s' to timeline %p", gcal_calendar_get_name (calendar), self);

  monitor = gcal_calendar_monitor_new (calendar);
  g_signal_connect (monitor, "event-added", G_CALLBACK (on_calendar_monitor_event_added_cb), self);
  g_signal_connect (monitor, "event-updated", G_CALLBACK (on_calendar_monitor_event_updated_cb), self);
  g_signal_connect (monitor, "event-removed", G_CALLBACK (on_calendar_monitor_event_removed_cb), self);
  g_signal_connect (monitor, "completed", G_CALLBACK (on_calendar_monitor_completed_cb), self);
  g_hash_table_insert (self->calendars, calendar, g_object_ref (monitor));

  if (self->range)
    gcal_calendar_monitor_set_range (monitor, self->range);

  GCAL_EXIT;
}

void
gcal_timeline_remove_calendar (GcalTimeline *self,
                               GcalCalendar *calendar)
{
  g_return_if_fail (GCAL_IS_TIMELINE (self));
  g_return_if_fail (GCAL_IS_CALENDAR (calendar));

  GCAL_ENTRY;

  g_object_ref (calendar);

  if (g_hash_table_remove (self->calendars, calendar))
    GCAL_TRACE_MSG ("Removing calendar '%s' from timeline %p", gcal_calendar_get_name (calendar), self);

  g_object_unref (calendar);

  GCAL_EXIT;
}

void
gcal_timeline_add_subscriber (GcalTimeline           *self,
                              GcalTimelineSubscriber *subscriber)
{
  g_autoptr (GcalRange) subscriber_range = NULL;

  g_return_if_fail (GCAL_IS_TIMELINE (self));
  g_return_if_fail (GCAL_IS_TIMELINE_SUBSCRIBER (subscriber));

  GCAL_ENTRY;

  if (g_hash_table_contains (self->subscribers, subscriber))
    GCAL_RETURN ();

  g_debug ("Adding subscriber %s to timeline %p", G_OBJECT_TYPE_NAME (subscriber), self);

  subscriber_range = gcal_timeline_subscriber_get_range (subscriber);

  g_hash_table_insert (self->subscribers, g_object_ref (subscriber), gcal_range_ref (subscriber_range));
  g_signal_connect_object (subscriber,
                           "range-changed",
                           G_CALLBACK (on_subscriber_range_changed_cb),
                           self,
                           0);

  add_cached_events_to_subscriber (self, subscriber);
  update_subscriber_range (self, subscriber);
  update_range (self);

  GCAL_EXIT;
}

void
gcal_timeline_remove_subscriber (GcalTimeline           *self,
                                 GcalTimelineSubscriber *subscriber)
{
  g_return_if_fail (GCAL_IS_TIMELINE (self));

  GCAL_ENTRY;

  if (!g_hash_table_contains (self->subscribers, subscriber))
    GCAL_RETURN ();

  g_debug ("Removing subscriber %s from timeline %p", G_OBJECT_TYPE_NAME (subscriber), self);

  g_signal_handlers_disconnect_by_func (subscriber, on_subscriber_range_changed_cb, self);
  g_hash_table_remove (self->subscribers, subscriber);

  /* If all subscribers were removed, reset the complete counter */
  if (g_hash_table_size (self->subscribers) == 0)
    reset_completed_calendars (self);

  update_range (self);

  GCAL_EXIT;
}

GPtrArray*
gcal_timeline_get_events_at_range (GcalTimeline *self,
                                   GDateTime    *range_start,
                                   GDateTime    *range_end)
{
  g_autoptr (GPtrArray) events_at_range = NULL;
  g_autoptr (GcalRange) range = NULL;

  g_return_val_if_fail (GCAL_IS_TIMELINE (self), NULL);
  g_return_val_if_fail (range_start != NULL, NULL);
  g_return_val_if_fail (range_end != NULL, NULL);

  range = gcal_range_new (range_start, range_end, GCAL_RANGE_DEFAULT);
  events_at_range = gcal_range_tree_get_data_at_range (self->events, range);

  return g_steal_pointer (&events_at_range);
}

const gchar*
gcal_timeline_get_filter (GcalTimeline *self)
{
  g_return_val_if_fail (GCAL_IS_TIMELINE (self), NULL);

  return self->filter;
}

void
gcal_timeline_set_filter (GcalTimeline *self,
                          const gchar  *filter)
{
  g_return_if_fail (GCAL_IS_TIMELINE (self));

  GCAL_ENTRY;

  if (g_strcmp0 (self->filter, filter) == 0)
    GCAL_RETURN ();

  g_debug ("Setting timeline filter to \"%s\"", filter);

  g_clear_pointer (&self->filter, g_free);
  self->filter = g_strdup (filter);

  update_calendar_monitor_filters (self);

  GCAL_EXIT;
}

gboolean
gcal_timeline_is_complete (GcalTimeline *self)
{
  g_return_val_if_fail (GCAL_IS_TIMELINE (self), FALSE);

  return is_timeline_complete (self);
}
