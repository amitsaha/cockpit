/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2014 Red Hat, Inc.
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

#include "config.h"

#include "cockpitwebresponse.h"
#include "cockpitwebserver.h"

#include "mock-io-stream.h"

#include "common/cockpittest.h"

#include "websocket/websocket.h"

#include <glib/gstdio.h>

#include <string.h>

static gchar *srcdir;

typedef struct {
    CockpitWebResponse *response;
    GOutputStream *output;
    gchar *scratch;
    gboolean response_done;
    gulong sig_done;
} TestCase;

typedef struct {
    const gchar *path;
    const gchar *header;
    const gchar *value;
} TestFixture;

static void
on_response_done (CockpitWebResponse *response,
                  gboolean reusable,
                  gpointer user_data)
{
  gboolean *response_done = user_data;
  g_assert (response_done != NULL);
  g_assert (*response_done == FALSE);
  *response_done = TRUE;
}

static void
setup (TestCase *tc,
       gconstpointer data)
{
  const TestFixture *fixture = data;
  const gchar *path = NULL;
  GHashTable *headers = NULL;
  GInputStream *input;
  GIOStream *io;

  if (fixture)
    path = fixture->path;

  input = g_memory_input_stream_new ();
  tc->output = g_memory_output_stream_new (NULL, 0, g_realloc, g_free);
  io = mock_io_stream_new (input, tc->output);
  g_object_unref (input);

  if (fixture && fixture->header)
    {
      headers = cockpit_web_server_new_table ();
      g_hash_table_insert (headers, g_strdup (fixture->header), g_strdup (fixture->value));
    }

  tc->response = cockpit_web_response_new (io, path, headers);

  if (headers)
    g_hash_table_unref (headers);
  g_object_unref (io);

  tc->sig_done = g_signal_connect (tc->response, "done",
                                   G_CALLBACK (on_response_done),
                                   &tc->response_done);
}

static void
teardown (TestCase *tc,
          gconstpointer data)
{
  while (g_main_context_iteration (NULL, FALSE));
  g_assert (tc->response_done);

  g_signal_handler_disconnect (tc->response, tc->sig_done);
  g_clear_object (&tc->output);
  g_clear_object (&tc->response);
  g_free (tc->scratch);
}

static const gchar *
output_as_string (TestCase *tc)
{
  while (!tc->response_done)
    g_main_context_iteration (NULL, TRUE);

  g_free (tc->scratch);
  tc->scratch = g_strndup (g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (tc->output)),
                           g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (tc->output)));
  return tc->scratch;
}

static void
test_return_content (TestCase *tc,
                     gconstpointer data)
{
  const gchar *resp;
  GBytes *content;

  content = g_bytes_new_static ("the content", 11);
  cockpit_web_response_content (tc->response, NULL, content, NULL);
  g_bytes_unref (content);

  resp = output_as_string (tc);
  g_assert_cmpstr (resp, ==, "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nthe content");
}

static void
test_return_content_headers (TestCase *tc,
                             gconstpointer data)
{
  const gchar *resp;
  GHashTable *headers;
  GBytes *content;

  headers = cockpit_web_server_new_table ();
  g_hash_table_insert (headers, g_strdup ("My-header"), g_strdup ("my-value"));

  content = g_bytes_new_static ("the content", 11);
  cockpit_web_response_content (tc->response, headers, content, NULL);
  g_bytes_unref (content);
  g_hash_table_destroy (headers);

  resp = output_as_string (tc);
  g_assert_cmpstr (resp, ==, "HTTP/1.1 200 OK\r\nMy-header: my-value\r\nContent-Length: 11\r\n\r\nthe content");
}


static void
test_return_error (TestCase *tc,
                   gconstpointer data)
{
  const gchar *resp;

  cockpit_expect_message ("Returning error-response 500*");

  cockpit_web_response_error (tc->response, 500, NULL, "Reason here: %s", "booyah");

  resp = output_as_string (tc);
  g_assert_cmpstr (resp, ==,
    "HTTP/1.1 500 Reason here: booyah\r\nContent-Length: 96\r\n\r\n<html><head><title>500 Reason here: booyah</title></head><body>Reason here: booyah</body></html>");
}

static void
test_return_error_headers (TestCase *tc,
                           gconstpointer data)
{
  const gchar *resp;
  GHashTable *headers;

  cockpit_expect_message ("Returning error-response 500*");

  headers = g_hash_table_new (g_str_hash, g_str_equal);
  g_hash_table_insert (headers, "Header1", "value1");

  cockpit_web_response_error (tc->response, 500, headers, "Reason here: %s", "booyah");

  g_hash_table_destroy (headers);

  resp = output_as_string (tc);
  g_assert_cmpstr (resp, ==,
    "HTTP/1.1 500 Reason here: booyah\r\nHeader1: value1\r\nContent-Length: 96\r\n\r\n<html><head><title>500 Reason here: booyah</title></head><body>Reason here: booyah</body></html>");
}

static void
test_return_gerror_headers (TestCase *tc,
                            gconstpointer data)
{
  const gchar *resp;
  GHashTable *headers;
  GError *error;

  cockpit_expect_message ("Returning error-response 500*");

  headers = g_hash_table_new (g_str_hash, g_str_equal);
  g_hash_table_insert (headers, "Header1", "value1");

  error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "Reason here: %s", "booyah");
  cockpit_web_response_gerror (tc->response, headers, error);

  g_error_free (error);
  g_hash_table_destroy (headers);

  resp = output_as_string (tc);
  g_assert_cmpstr (resp, ==,
    "HTTP/1.1 500 Reason here: booyah\r\nHeader1: value1\r\nContent-Length: 96\r\n\r\n<html><head><title>500 Reason here: booyah</title></head><body>Reason here: booyah</body></html>");
}

static void
test_file_not_found (TestCase *tc,
                     gconstpointer user_data)
{
  const gchar *roots[] = { srcdir, NULL };
  cockpit_web_response_file (tc->response, "/non-existant", FALSE, roots);
  cockpit_assert_strmatch (output_as_string (tc), "HTTP/1.1 404 Not Found*");
}

static void
test_file_directory_denied (TestCase *tc,
                            gconstpointer user_data)
{
  const gchar *roots[] = { srcdir, NULL };
  cockpit_web_response_file (tc->response, "/src", FALSE, roots);
  cockpit_assert_strmatch (output_as_string (tc), "HTTP/1.1 403 Directory Listing Denied*");
}

static void
test_file_access_denied (TestCase *tc,
                         gconstpointer user_data)
{
  const gchar *roots[] = { "/tmp", NULL };
  gchar templ[] = "/tmp/test-temp.XXXXXX";

  if (!g_mkdtemp_full (templ, 0000))
    g_assert_not_reached ();

  cockpit_web_response_file (tc->response, templ + 4, FALSE, roots);
  cockpit_assert_strmatch (output_as_string (tc), "HTTP/1.1 403*");

  g_unlink (templ);
}

static void
test_file_breakout_denied (TestCase *tc,
                           gconstpointer user_data)
{
  gchar *root = realpath ( SRCDIR "/src", NULL);
  const gchar *roots[] = { root, NULL };
  const gchar *breakout = "/../Makefile.am";
  gchar *check = g_build_filename (roots[0], breakout, NULL);
  g_assert (root);
  g_assert (g_file_test (check, G_FILE_TEST_EXISTS));
  g_free (check);
  cockpit_web_response_file (tc->response, breakout, FALSE, roots);
  cockpit_assert_strmatch (output_as_string (tc), "HTTP/1.1 404*");
  free (root);
}

static void
test_file_breakout_non_existant (TestCase *tc,
                                 gconstpointer user_data)
{
  gchar *root = realpath ( SRCDIR "/src", NULL);
  const gchar *roots[] = { root, NULL };
  const gchar *breakout = "/../non-existant";
  gchar *check = g_build_filename (roots[0], breakout, NULL);
  g_assert (root);
  g_assert (!g_file_test (check, G_FILE_TEST_EXISTS));
  g_free (check);
  cockpit_web_response_file (tc->response, breakout, FALSE, roots);
  cockpit_assert_strmatch (output_as_string (tc), "HTTP/1.1 404*");
  free (root);
}

static const TestFixture content_type_fixture = {
  .path = "/pkg/shell/test-dbus.html"
};

static void
test_content_type (TestCase *tc,
                   gconstpointer user_data)
{
  const gchar *roots[] = { srcdir, NULL };
  GHashTable *headers;
  const gchar *resp;
  gsize length;
  guint status;
  gssize off;

  g_assert (user_data == &content_type_fixture);

  cockpit_web_response_file (tc->response, NULL, FALSE, roots);

  resp = output_as_string (tc);
  length = strlen (resp);

  off = web_socket_util_parse_status_line (resp, length, &status, NULL);
  g_assert_cmpuint (off, >, 0);
  g_assert_cmpint (status, ==, 200);

  off = web_socket_util_parse_headers (resp + off, length - off, &headers);
  g_assert_cmpuint (off, >, 0);

  g_assert_cmpstr (g_hash_table_lookup (headers, "Content-Type"), ==, "text/html");

  g_hash_table_unref (headers);
}

static void
test_stream (TestCase *tc,
             gconstpointer data)
{
  const gchar *resp;
  GBytes *content;

  g_assert_cmpint (cockpit_web_response_get_state (tc->response), ==, COCKPIT_WEB_RESPONSE_READY);

  cockpit_web_response_headers (tc->response, 200, "OK", 11, NULL);

  g_assert_cmpint (cockpit_web_response_get_state (tc->response), ==, COCKPIT_WEB_RESPONSE_QUEUING);

  while (g_main_context_iteration (NULL, FALSE));

  content = g_bytes_new_static ("the content", 11);
  cockpit_web_response_queue (tc->response, content);
  g_bytes_unref (content);

  g_assert_cmpint (cockpit_web_response_get_state (tc->response), ==, COCKPIT_WEB_RESPONSE_QUEUING);

  cockpit_web_response_complete (tc->response);

  g_assert_cmpint (cockpit_web_response_get_state (tc->response), ==, COCKPIT_WEB_RESPONSE_COMPLETE);

  resp = output_as_string (tc);
  g_assert_cmpint (cockpit_web_response_get_state (tc->response), ==, COCKPIT_WEB_RESPONSE_SENT);

  g_assert_cmpstr (resp, ==, "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nthe content");
}

static void
test_chunked_transfer_encoding (TestCase *tc,
                                gconstpointer data)
{
  const gchar *resp;
  GBytes *content;

  g_assert_cmpint (cockpit_web_response_get_state (tc->response), ==, COCKPIT_WEB_RESPONSE_READY);

  cockpit_web_response_headers (tc->response, 200, "OK", -1, NULL);

  g_assert_cmpint (cockpit_web_response_get_state (tc->response), ==, COCKPIT_WEB_RESPONSE_QUEUING);

  while (g_main_context_iteration (NULL, FALSE));

  content = g_bytes_new_static ("Cockpit is perfect for new sysadmins, ", 38);
  cockpit_web_response_queue (tc->response, content);
  g_bytes_unref (content);

  content = g_bytes_new_static ("allowing them to easily perform simple tasks such as storage administration, ", 77);
  cockpit_web_response_queue (tc->response, content);
  g_bytes_unref (content);

  content = g_bytes_new_static ("inspecting journals and starting and stopping services.", 55);
  cockpit_web_response_queue (tc->response, content);
  g_bytes_unref (content);

  g_assert_cmpint (cockpit_web_response_get_state (tc->response), ==, COCKPIT_WEB_RESPONSE_QUEUING);

  cockpit_web_response_complete (tc->response);

  g_assert_cmpint (cockpit_web_response_get_state (tc->response), ==, COCKPIT_WEB_RESPONSE_COMPLETE);

  resp = output_as_string (tc);
  g_assert_cmpint (cockpit_web_response_get_state (tc->response), ==, COCKPIT_WEB_RESPONSE_SENT);

  g_assert_cmpstr (resp, ==, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                   "26\r\nCockpit is perfect for new sysadmins, \r\n"
                   "4d\r\nallowing them to easily perform simple tasks such as storage administration, \r\n"
                   "37\r\ninspecting journals and starting and stopping services.\r\n0\r\n\r\n");
}

static void
on_response_done_not_resuable (CockpitWebResponse *response,
                               gboolean reusable,
                               gpointer user_data)
{
  g_assert (reusable == FALSE);
}

static void
test_abort (TestCase *tc,
            gconstpointer data)
{
  const gchar *resp;
  GBytes *content;

  cockpit_web_response_headers (tc->response, 200, "OK", 11, NULL);
  g_signal_connect (tc->response, "done", G_CALLBACK (on_response_done_not_resuable), NULL);

  while (g_main_context_iteration (NULL, FALSE));

  content = g_bytes_new_static ("the content", 11);
  cockpit_web_response_queue (tc->response, content);
  g_bytes_unref (content);

  cockpit_web_response_abort (tc->response);
  g_assert_cmpint (cockpit_web_response_get_state (tc->response), ==, COCKPIT_WEB_RESPONSE_SENT);

  resp = output_as_string (tc);

  g_assert_cmpstr (resp, ==, "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n");
}

static const TestFixture fixture_connection_close = {
  .header = "Connection",
  .value = "close",
};

static void
test_connection_close (TestCase *tc,
                       gconstpointer data)
{
  const gchar *resp;
  GBytes *content;

  g_assert (data == &fixture_connection_close);

  g_signal_connect (tc->response, "done", G_CALLBACK (on_response_done_not_resuable), NULL);

  content = g_bytes_new_static ("the content", 11);
  cockpit_web_response_content (tc->response, NULL, content, NULL);
  g_bytes_unref (content);

  resp = output_as_string (tc);
  g_assert_cmpstr (resp, ==, "HTTP/1.1 200 OK\r\nContent-Length: 11\r\nConnection: close\r\n\r\nthe content");
}


int
main (int argc,
      char *argv[])
{
  gint ret;

  srcdir = realpath (SRCDIR, NULL);
  g_assert (srcdir != NULL);

  cockpit_test_init (&argc, &argv);

  g_test_add ("/web-response/return-content", TestCase, NULL,
              setup, test_return_content, teardown);
  g_test_add ("/web-response/return-content-headers", TestCase, NULL,
              setup, test_return_content_headers, teardown);
  g_test_add ("/web-response/return-error", TestCase, NULL,
              setup, test_return_error, teardown);
  g_test_add ("/web-response/return-error-headers", TestCase, NULL,
              setup, test_return_error_headers, teardown);
  g_test_add ("/web-response/return-gerror-headers", TestCase, NULL,
              setup, test_return_gerror_headers, teardown);
  g_test_add ("/web-response/file/not-found", TestCase, NULL,
              setup, test_file_not_found, teardown);
  g_test_add ("/web-response/file/directory-denied", TestCase, NULL,
              setup, test_file_directory_denied, teardown);
  g_test_add ("/web-response/file/access-denied", TestCase, NULL,
              setup, test_file_access_denied, teardown);
  g_test_add ("/web-response/file/breakout-denied", TestCase, NULL,
              setup, test_file_breakout_denied, teardown);
  g_test_add ("/web-response/file/breakout-non-existant", TestCase, NULL,
              setup, test_file_breakout_non_existant, teardown);
  g_test_add ("/web-response/content-type", TestCase, &content_type_fixture,
              setup, test_content_type, teardown);
  g_test_add ("/web-response/stream", TestCase, NULL,
              setup, test_stream, teardown);
  g_test_add ("/web-response/chunked-transfer-encoding", TestCase, NULL,
              setup, test_chunked_transfer_encoding, teardown);
  g_test_add ("/web-response/abort", TestCase, NULL,
              setup, test_abort, teardown);
  g_test_add ("/web-response/connection-close", TestCase, &fixture_connection_close,
              setup, test_connection_close, teardown);

  ret = g_test_run ();

  free (srcdir);

  return ret;
}
