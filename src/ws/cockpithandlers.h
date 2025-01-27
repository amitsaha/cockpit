/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __COCKPIT_HANDLERS_H__
#define __COCKPIT_HANDLERS_H__

#include "cockpitauth.h"
#include "cockpitwebserver.h"
#include "cockpitwebresponse.h"

typedef struct {
  CockpitAuth *auth;
  const gchar **static_roots;
} CockpitHandlerData;

gboolean       cockpit_handler_socket            (CockpitWebServer *server,
                                                  const gchar *path,
                                                  GIOStream *io_stream,
                                                  GHashTable *headers,
                                                  GByteArray *input,
                                                  guint in_length,
                                                  CockpitHandlerData *data);

gboolean       cockpit_handler_login             (CockpitWebServer *server,
                                                  const gchar *path,
                                                  GHashTable *headers,
                                                  CockpitWebResponse *response,
                                                  CockpitHandlerData *data);

gboolean       cockpit_handler_index             (CockpitWebServer *server,
                                                  const gchar *path,
                                                  GHashTable *headers,
                                                  CockpitWebResponse *response,
                                                  CockpitHandlerData *data);

gboolean       cockpit_handler_root              (CockpitWebServer *server,
                                                  const gchar *path,
                                                  GHashTable *headers,
                                                  CockpitWebResponse *response,
                                                  CockpitHandlerData *ws);

gboolean       cockpit_handler_static            (CockpitWebServer *server,
                                                  const gchar *path,
                                                  GHashTable *headers,
                                                  CockpitWebResponse *response,
                                                  CockpitHandlerData *ws);

gboolean       cockpit_handler_resource          (CockpitWebService *server,
                                                  const gchar *path,
                                                  GHashTable *headers,
                                                  CockpitWebResponse *response,
                                                  CockpitHandlerData *ws);

gboolean       cockpit_handler_ping              (CockpitWebServer *server,
                                                  const gchar *path,
                                                  GHashTable *headers,
                                                  CockpitWebResponse *response,
                                                  CockpitHandlerData *ws);

#endif /* __COCKPIT_HANDLERS_H__ */
