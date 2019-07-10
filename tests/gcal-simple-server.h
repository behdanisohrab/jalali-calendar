/* gcal-simple-server.h
 *
 * Copyright 2019 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
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

#pragma once

#include <glib-object.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS


#define GCAL_TEST_SERVER_EMPTY_CALENDAR \
"BEGIN:VCALENDAR\n" \
"END:VCALENDAR"


#define GCAL_TYPE_SIMPLE_SERVER (gcal_simple_server_get_type())
G_DECLARE_FINAL_TYPE (GcalSimpleServer, gcal_simple_server, GCAL, SIMPLE_SERVER, GObject)

GcalSimpleServer*    gcal_simple_server_new                      (void);

void                 gcal_simple_server_start                    (GcalSimpleServer   *self);

void                 gcal_simple_server_stop                     (GcalSimpleServer   *self);

SoupURI*             gcal_simple_server_get_uri                  (GcalSimpleServer   *self);

G_END_DECLS
