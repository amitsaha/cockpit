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

#include "config.h"

#include "mock-auth.h"
#include "mock-io-stream.h"
#include "cockpitws.h"
#include "cockpitcreds.h"
#include "cockpitwebservice.h"
#include "cockpitwebserver.h"

#include "common/cockpittransport.h"
#include "common/cockpitjson.h"
#include "common/cockpittest.h"

#include "websocket/websocket.h"

#include <glib.h>

#include <libssh/libssh.h>

#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>

#define TIMEOUT 30

#define WAIT_UNTIL(cond) \
  G_STMT_START \
    while (!(cond)) g_main_context_iteration (NULL, TRUE); \
  G_STMT_END

#define PASSWORD "this is the password"

typedef struct {
  /* setup_mock_sshd */
  const gchar *ssh_user;
  const gchar *ssh_password;
  GPid mock_sshd;
  guint16 ssh_port;

  /* setup_mock_webserver */
  CockpitWebServer *web_server;
  gchar *cookie;
  CockpitAuth *auth;
  CockpitCreds *creds;

  /* setup_io_pair */
  GIOStream *io_a;
  GIOStream *io_b;

  /* serve_socket */
  CockpitWebService *service;
} TestCase;

typedef struct {
  WebSocketFlavor web_socket_flavor;
  const char *origin;
} TestFixture;

static GString *
read_all_into_string (int fd)
{
  GString *input = g_string_new ("");
  gsize len;
  gssize ret;

  for (;;)
    {
      len = input->len;
      g_string_set_size (input, len + 256);
      ret = read (fd, input->str + len, 256);
      if (ret < 0)
        {
          if (errno != EAGAIN)
            {
              g_critical ("couldn't read from mock input: %s", g_strerror (errno));
              g_string_free (input, TRUE);
              return NULL;
            }
        }
      else if (ret == 0)
        {
          return input;
        }
      else
        {
          input->len = len + ret;
          input->str[input->len] = '\0';
        }
    }
}

static void
setup_mock_sshd (TestCase *test,
                 gconstpointer data)
{
  GError *error = NULL;
  GString *port;
  gchar *endptr;
  guint64 value;
  gint out_fd;

  const gchar *argv[] = {
      BUILDDIR "/mock-sshd",
      "--user", test->ssh_user ? test->ssh_user : g_get_user_name (),
      "--password", test->ssh_password ? test->ssh_password : PASSWORD,
      NULL
  };

  g_spawn_async_with_pipes (BUILDDIR, (gchar **)argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                            &test->mock_sshd, NULL, &out_fd, NULL, &error);
  g_assert_no_error (error);

  /*
   * mock-sshd prints its port on stdout, and then closes stdout
   * This also lets us know when it has initialized.
   */

  port = read_all_into_string (out_fd);
  g_assert (port != NULL);
  close (out_fd);
  g_assert_no_error (error);

  g_strstrip (port->str);
  value = g_ascii_strtoull (port->str, &endptr, 10);
  if (!endptr || *endptr != '\0' || value == 0 || value > G_MAXUSHORT)
      g_critical ("invalid port printed by mock-sshd: %s", port->str);

  test->ssh_port = (gushort)value;
  g_string_free (port, TRUE);

  cockpit_ws_specific_ssh_port = test->ssh_port;
  cockpit_ws_known_hosts = SRCDIR "/src/ws/mock_known_hosts";
}

static void
teardown_mock_sshd (TestCase *test,
                    gconstpointer data)
{
  GPid pid;
  int status;

  if (test->mock_sshd)
    {
      pid = waitpid (test->mock_sshd, &status, WNOHANG);
      g_assert_cmpint (pid, >=, 0);
      if (pid == 0)
        kill (test->mock_sshd, SIGTERM);
      else if (status != 0)
        {
          if (WIFSIGNALED (status))
            g_critical ("mock-sshd terminated: %d", WTERMSIG (status));
          else
            g_critical ("mock-sshd failed: %d", WEXITSTATUS (status));
        }
      g_spawn_close_pid (test->mock_sshd);
    }
}

static void
setup_mock_webserver (TestCase *test,
                      gconstpointer data)
{
  const gchar *roots[] = { SRCDIR "/src/ws", NULL };
  GError *error = NULL;
  const gchar *user;

  /* Zero port makes server choose its own */
  test->web_server = cockpit_web_server_new (0, NULL, roots, NULL, &error);
  g_assert_no_error (error);

  user = g_get_user_name ();
  test->auth = mock_auth_new (user, PASSWORD);

  test->creds = cockpit_creds_new (user, COCKPIT_CRED_PASSWORD, PASSWORD, NULL);
}

static void
teardown_mock_webserver (TestCase *test,
                         gconstpointer data)
{
  g_clear_object (&test->web_server);
  if (test->creds)
    cockpit_creds_unref (test->creds);
  g_clear_object (&test->auth);
  g_free (test->cookie);
}

static void
setup_io_streams (TestCase *test,
                  gconstpointer data)
{
  GSocket *socket1, *socket2;
  GError *error = NULL;
  int fds[2];

  if (socketpair (PF_UNIX, SOCK_STREAM, 0, fds) < 0)
    g_assert_not_reached ();

  socket1 = g_socket_new_from_fd (fds[0], &error);
  g_assert_no_error (error);

  socket2 = g_socket_new_from_fd (fds[1], &error);
  g_assert_no_error (error);

  test->io_a = G_IO_STREAM (g_socket_connection_factory_create_connection (socket1));
  test->io_b = G_IO_STREAM (g_socket_connection_factory_create_connection (socket2));

  g_object_unref (socket1);
  g_object_unref (socket2);

  cockpit_ws_bridge_program = BUILDDIR "/mock-echo";
}

static void
teardown_io_streams (TestCase *test,
                     gconstpointer data)
{
  g_clear_object (&test->io_a);
  g_clear_object (&test->io_b);
}

static void
setup_for_socket (TestCase *test,
                  gconstpointer data)
{
  alarm (TIMEOUT);

  setup_mock_sshd (test, data);
  setup_mock_webserver (test, data);
  setup_io_streams (test, data);
}

static void
setup_for_socket_spec (TestCase *test,
                       gconstpointer data)
{
  test->ssh_user = "user";
  test->ssh_password = "Another password";
  setup_for_socket (test, data);
}

static void
teardown_for_socket (TestCase *test,
                     gconstpointer data)
{
  teardown_mock_sshd (test, data);
  teardown_mock_webserver (test, data);
  teardown_io_streams (test, data);

  /* Reset this if changed by a test */
  cockpit_ws_session_timeout = 30;

  cockpit_assert_expected ();
  alarm (0);
}

static void
on_ready_get_result (GObject *source,
                     GAsyncResult *result,
                     gpointer data)
{
  GAsyncResult **retval = data;
  g_assert (retval != NULL);
  g_assert (*retval == NULL);
  *retval = g_object_ref (result);
}

static void
on_error_not_reached (WebSocketConnection *ws,
                      GError *error,
                      gpointer user_data)
{
  g_assert (error != NULL);

  /* At this point we know this will fail, but is informative */
  g_assert_no_error (error);
}

static void
on_error_copy (WebSocketConnection *ws,
               GError *error,
               gpointer user_data)
{
  GError **result = user_data;
  g_assert (error != NULL);
  g_assert (result != NULL);
  g_assert (*result == NULL);
  *result = g_error_copy (error);
}

static gboolean
on_timeout_fail (gpointer data)
{
  g_error ("timeout during test: %s", (gchar *)data);
  return FALSE;
}

#define BUILD_INTS GINT_TO_POINTER(1)

static GBytes *
build_control_va (const gchar *command,
                  const gchar *channel,
                  va_list va)
{
  JsonBuilder *builder;
  gchar *data;
  gsize length;
  const gchar *option;
  GBytes *bytes;
  JsonNode *node;
  gboolean strings = TRUE;

  builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "command");
  json_builder_add_string_value (builder, command);
  if (channel)
    {
      json_builder_set_member_name (builder, "channel");
      json_builder_add_string_value (builder, channel);
    }

  for (;;)
    {
      option = va_arg (va, const gchar *);
      if (option == BUILD_INTS)
        {
          strings = FALSE;
          option = va_arg (va, const gchar *);
        }
      if (!option)
        break;
      json_builder_set_member_name (builder, option);
      if (strings)
        json_builder_add_string_value (builder, va_arg (va, const gchar *));
      else
        json_builder_add_int_value (builder, va_arg (va, gint));
    }

  json_builder_end_object (builder);
  node = json_builder_get_root (builder);
  data = cockpit_json_write (node, &length);
  data = g_realloc (data, length + 1);
  memmove (data + 1, data, length);
  memcpy (data, "\n", 1);
  bytes = g_bytes_new_take (data, length + 1);
  json_node_free (node);
  g_object_unref (builder);

  return bytes;
}

static void
send_control_message (WebSocketConnection *ws,
                      const gchar *command,
                      const gchar *channel,
                      ...) G_GNUC_NULL_TERMINATED;

static void
send_control_message (WebSocketConnection *ws,
                      const gchar *command,
                      const gchar *channel,
                      ...)
{
  GBytes *payload;
  va_list va;

  va_start (va, channel);
  payload = build_control_va (command, channel, va);
  va_end (va);

  web_socket_connection_send (ws, WEB_SOCKET_DATA_TEXT, NULL, payload);
  g_bytes_unref (payload);
}

static void
expect_control_message (GBytes *message,
                        const gchar *command,
                        const gchar *expected_channel,
                        ...) G_GNUC_NULL_TERMINATED;

static void
expect_control_message (GBytes *message,
                        const gchar *expected_command,
                        const gchar *expected_channel,
                        ...)
{
  gchar *outer_channel;
  const gchar *message_command;
  const gchar *message_channel;
  JsonObject *options;
  GBytes *payload;
  const gchar *expect_option;
  const gchar *expect_value;
  const gchar *value;
  va_list va;

  payload = cockpit_transport_parse_frame (message, &outer_channel);
  g_assert (payload != NULL);
  g_assert_cmpstr (outer_channel, ==, NULL);
  g_free (outer_channel);

  g_assert (cockpit_transport_parse_command (payload, &message_command,
                                             &message_channel, &options));
  g_bytes_unref (payload);

  g_assert_cmpstr (expected_command, ==, message_command);
  g_assert_cmpstr (expected_channel, ==, expected_channel);

  va_start (va, expected_channel);
  for (;;) {
      expect_option = va_arg (va, const gchar *);
      if (!expect_option)
        break;
      expect_value = va_arg (va, const gchar *);
      g_assert (expect_value != NULL);
      value = NULL;
      if (json_object_has_member (options, expect_option))
        value = json_object_get_string_member (options, expect_option);
      g_assert_cmpstr (value, ==, expect_value);
  }
  va_end (va);

  json_object_unref (options);
}

static void
start_web_service_and_create_client (TestCase *test,
                                     const TestFixture *fixture,
                                     WebSocketConnection **ws,
                                     CockpitWebService **service)
{
  const char *origin = fixture ? fixture->origin : NULL;
  if (!origin)
    origin = "http://127.0.0.1";

  /* This is web_socket_client_new_for_stream() with a flavor passed in fixture */
  *ws = g_object_new (WEB_SOCKET_TYPE_CLIENT,
                     "url", "ws://127.0.0.1/unused",
                     "origin", origin,
                     "io-stream", test->io_a,
                     "flavor", fixture ? fixture->web_socket_flavor : 0,
                     NULL);

  g_signal_connect (*ws, "error", G_CALLBACK (on_error_not_reached), NULL);
  web_socket_client_include_header (WEB_SOCKET_CLIENT (*ws), "Cookie", test->cookie);

  /* Matching the above origin */
  cockpit_ws_default_host_header = "127.0.0.1";

  *service = cockpit_web_service_new (test->creds, NULL);

  /* Note, we are forcing the websocket to parse its own headers */
  cockpit_web_service_socket (*service, test->io_b, NULL, NULL);
}

static void
start_web_service_and_connect_client (TestCase *test,
                                      const TestFixture *fixture,
                                      WebSocketConnection **ws,
                                      CockpitWebService **service)
{
  start_web_service_and_create_client (test, fixture, ws, service);
  WAIT_UNTIL (web_socket_connection_get_ready_state (*ws) != WEB_SOCKET_STATE_CONNECTING);
  g_assert (web_socket_connection_get_ready_state (*ws) == WEB_SOCKET_STATE_OPEN);

  /* Send the open control message that starts the bridge. */
  send_control_message (*ws, "init", NULL, BUILD_INTS, "version", 0, NULL);
  send_control_message (*ws, "open", "4", "payload", "test-text", NULL);
}

static void
close_client_and_stop_web_service (TestCase *test,
                                   WebSocketConnection *ws,
                                   CockpitWebService *service)
{
  guint timeout;

  if (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN)
    {
      web_socket_connection_close (ws, 0, NULL);
      WAIT_UNTIL (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_CLOSED);
    }

  g_object_unref (ws);

  /* Wait until service is done */
  timeout = g_timeout_add_seconds (20, on_timeout_fail, "closing web service");
  g_object_add_weak_pointer (G_OBJECT (service), (gpointer *)&service);
  g_object_unref (service);
  while (service != NULL)
    g_main_context_iteration (NULL, TRUE);
  g_source_remove (timeout);
}

static void
test_handshake_and_auth (TestCase *test,
                         gconstpointer data)
{
  WebSocketConnection *ws;
  CockpitWebService *service;

  start_web_service_and_connect_client (test, data, &ws, &service);
  close_client_and_stop_web_service (test, ws, service);
}

static void
on_message_get_bytes (WebSocketConnection *ws,
                      WebSocketDataType type,
                      GBytes *message,
                      gpointer user_data)
{
  GBytes **received = user_data;
  g_assert_cmpint (type, ==, WEB_SOCKET_DATA_TEXT);
  if (*received != NULL)
    {
      gsize length;
      gconstpointer data = g_bytes_get_data (message, &length);
      g_test_message ("received unexpected extra message: %.*s", (int)length, (gchar *)data);
      g_assert_not_reached ();
    }
  *received = g_bytes_ref (message);
}

static void
on_message_get_non_control (WebSocketConnection *ws,
                            WebSocketDataType type,
                            GBytes *message,
                            gpointer user_data)
{
  GBytes **received = user_data;
  g_assert_cmpint (type, ==, WEB_SOCKET_DATA_TEXT);
  /* Control messages have this prefix: ie: a zero channel */
  if (g_str_has_prefix (g_bytes_get_data (message, NULL), "\n"))
      return;
  g_assert (*received == NULL);
  *received = g_bytes_ref (message);
}

static void
test_handshake_and_echo (TestCase *test,
                         gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  CockpitWebService *service;
  GBytes *sent;
  gulong handler;

  start_web_service_and_connect_client (test, data, &ws, &service);

  sent = g_bytes_new_static ("4\nthe message", 13);
  handler = g_signal_connect (ws, "message", G_CALLBACK (on_message_get_non_control), &received);
  web_socket_connection_send (ws, WEB_SOCKET_DATA_TEXT, NULL, sent);

  WAIT_UNTIL (received != NULL);

  g_assert (g_bytes_equal (received, sent));
  g_bytes_unref (sent);
  g_bytes_unref (received);
  received = NULL;

  g_signal_handler_disconnect (ws, handler);

  close_client_and_stop_web_service (test, ws, service);
}

static void
test_echo_large (TestCase *test,
                 gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  CockpitWebService *service;
  gchar *contents;
  GBytes *sent;
  gulong handler;

  start_web_service_and_connect_client (test, data, &ws, &service);
  handler = g_signal_connect (ws, "message", G_CALLBACK (on_message_get_non_control), &received);

  /* Medium length */
  contents = g_strnfill (1020, '!');
  contents[0] = '4'; /* channel */
  contents[1] = '\n';
  sent = g_bytes_new_take (contents, 1020);
  web_socket_connection_send (ws, WEB_SOCKET_DATA_TEXT, NULL, sent);
  WAIT_UNTIL (received != NULL);
  g_assert (g_bytes_equal (received, sent));
  g_bytes_unref (sent);
  g_bytes_unref (received);
  received = NULL;

  /* Extra large */
  contents = g_strnfill (100 * 1000, '?');
  contents[0] = '4'; /* channel */
  contents[1] = '\n';
  sent = g_bytes_new_take (contents, 100 * 1000);
  web_socket_connection_send (ws, WEB_SOCKET_DATA_TEXT, NULL, sent);
  WAIT_UNTIL (received != NULL);
  g_assert (g_bytes_equal (received, sent));
  g_bytes_unref (sent);
  g_bytes_unref (received);
  received = NULL;

  g_signal_handler_disconnect (ws, handler);
  close_client_and_stop_web_service (test, ws, service);
}

static void
test_close_error (TestCase *test,
                  gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  CockpitWebService *service;

  start_web_service_and_connect_client (test, data, &ws, &service);
  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);

  WAIT_UNTIL (received != NULL);
  expect_control_message (received, "init", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  WAIT_UNTIL (received != NULL);
  expect_control_message (received, "open", "4", NULL);
  g_bytes_unref (received);
  received = NULL;

  /* Trigger a failure message */
  kill (test->mock_sshd, SIGTERM);
  test->mock_sshd = 0;

  /* We should now get a close command */
  WAIT_UNTIL (received != NULL);
  expect_control_message (received, "close", "4", "reason", "terminated", NULL);
  g_bytes_unref (received);
  received = NULL;

  close_client_and_stop_web_service (test, ws, service);
}

static void
test_no_init (TestCase *test,
              gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  CockpitWebService *service;

  start_web_service_and_create_client (test, data, &ws, &service);
  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);

  while (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_CONNECTING)
    g_main_context_iteration (NULL, TRUE);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);

  cockpit_expect_message ("*socket did not send*init*");
  cockpit_expect_log ("WebSocket", G_LOG_LEVEL_MESSAGE, "connection unexpectedly closed*");

  /* Sending an open message before init, should cause problems */
  send_control_message (ws, "ping", NULL, NULL);

  /* The init from the other end */
  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "init", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  /* We should now get a failure */
  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "close", NULL, "reason", "protocol-error", NULL);
  g_bytes_unref (received);
  received = NULL;

  close_client_and_stop_web_service (test, ws, service);
}

static void
test_wrong_init_version (TestCase *test,
                         gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  CockpitWebService *service;

  start_web_service_and_create_client (test, data, &ws, &service);
  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);

  while (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_CONNECTING)
    g_main_context_iteration (NULL, TRUE);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);

  cockpit_expect_message ("*socket used unsupported*");
  cockpit_expect_log ("WebSocket", G_LOG_LEVEL_MESSAGE, "connection unexpectedly closed*");

  send_control_message (ws, "init", NULL, BUILD_INTS, "version", 888, NULL);

  /* The init from the other end */
  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "init", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  /* We should now get a failure */
  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "close", NULL, "reason", "protocol-error", NULL);
  g_bytes_unref (received);
  received = NULL;

  close_client_and_stop_web_service (test, ws, service);
}

static void
test_bad_init_version (TestCase *test,
                       gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  CockpitWebService *service;

  start_web_service_and_create_client (test, data, &ws, &service);
  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);

  while (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_CONNECTING)
    g_main_context_iteration (NULL, TRUE);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);

  cockpit_expect_message ("*socket used unsupported*");
  cockpit_expect_log ("WebSocket", G_LOG_LEVEL_MESSAGE, "connection unexpectedly closed*");

  send_control_message (ws, "init", NULL, BUILD_INTS, "version", "blah", NULL);

  /* The init from the other end */
  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "init", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  /* We should now get a failure */
  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "close", NULL, "reason", "protocol-error", NULL);
  g_bytes_unref (received);
  received = NULL;

  close_client_and_stop_web_service (test, ws, service);
}

static void
test_specified_creds (TestCase *test,
                      gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  GBytes *sent;
  CockpitWebService *service;

  start_web_service_and_create_client (test, data, &ws, &service);
  WAIT_UNTIL (web_socket_connection_get_ready_state (ws) != WEB_SOCKET_STATE_CONNECTING);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);

  /* Open a channel with a non-standard command */
  send_control_message (ws, "init", NULL, BUILD_INTS, "version", 0, NULL);
  send_control_message (ws, "open", "4",
                        "payload", "test-text",
                        "user", "user", "password",
                        "Another password",
                        NULL);

  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_non_control), &received);

  sent = g_bytes_new_static ("4\nwheee", 7);
  web_socket_connection_send (ws, WEB_SOCKET_DATA_TEXT, NULL, sent);
  WAIT_UNTIL (received != NULL);
  g_assert (g_bytes_equal (received, sent));
  g_bytes_unref (sent);
  g_bytes_unref (received);
  received = NULL;

  close_client_and_stop_web_service (test, ws, service);
}

static void
test_specified_creds_fail (TestCase *test,
                           gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  CockpitWebService *service;

  start_web_service_and_create_client (test, data, &ws, &service);
  WAIT_UNTIL (web_socket_connection_get_ready_state (ws) != WEB_SOCKET_STATE_CONNECTING);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);

  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);

  /* Open a channel with a non-standard command, but a bad password */
  send_control_message (ws, "init", NULL, BUILD_INTS, "version", 0, NULL);
  send_control_message (ws, "open", "4",
                        "payload", "test-text",
                        "user", "user",
                        "password", "Wrong password",
                        NULL);

  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "init", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  /* We should now get a close command */
  WAIT_UNTIL (received != NULL);

  /* Should have gotten a failure message, about the credentials */
  expect_control_message (received, "close", "4", "reason", "not-authorized", NULL);
  g_bytes_unref (received);

  close_client_and_stop_web_service (test, ws, service);
}

static void
test_socket_null_creds (TestCase *test,
                        gconstpointer data)
{
  CockpitWebService *service;
  CockpitPipe *session;
  int pair[2];

  /*
   * These are tests double checking that we *never*
   * open up a real CockpitWebService for NULL creds.
   *
   * Other code paths do the real checks, but these are
   * the last resorts.
   */

  cockpit_expect_critical ("*assertion*failed*");

  service = cockpit_web_service_new (NULL, NULL);
  g_assert (service == NULL);

  cockpit_expect_critical ("*assertion*failed*");

  g_assert (pipe(pair) >= 0);
  session = cockpit_pipe_new ("dummy", pair[0], pair[1]);
  service = cockpit_web_service_new (NULL, session);
  g_assert (service == NULL);
  g_object_unref (session);
}

static void
test_socket_unauthenticated (TestCase *test,
                             gconstpointer data)
{
  WebSocketConnection *client;
  GBytes *received = NULL;

  client = g_object_new (WEB_SOCKET_TYPE_CLIENT,
                         "url", "ws://127.0.0.1/unused",
                         "origin", "http://127.0.0.1",
                         "io-stream", test->io_a,
                         NULL);

  g_signal_connect (client, "error", G_CALLBACK (on_error_not_reached), NULL);

  /* Matching the above origin */
  cockpit_ws_default_host_header = "127.0.0.1";

  cockpit_web_service_noauth (test->io_b, NULL, NULL);

  g_signal_connect (client, "message", G_CALLBACK (on_message_get_bytes), &received);

  /* Should close right after opening */
  while (web_socket_connection_get_ready_state (client) != WEB_SOCKET_STATE_CLOSED)
    g_main_context_iteration (NULL, TRUE);

  /* And we should have received a message */
  g_assert (received != NULL);
  expect_control_message (received, "close", "4", "reason", "no-session", NULL);
  g_bytes_unref (received);
  received = NULL;

  g_object_unref (client);

  while (g_main_context_iteration (NULL, FALSE));
}

static const gchar MOCK_RSA_KEY[] = "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQCYzo07OA0H6f7orVun9nIVjGYrkf8AuPDScqWGzlKpAqSipoQ9oY/mwONwIOu4uhKh7FTQCq5p+NaOJ6+Q4z++xBzSOLFseKX+zyLxgNG28jnF06WSmrMsSfvPdNuZKt9rZcQFKn9fRNa8oixa+RsqEEVEvTYhGtRf7w2wsV49xIoIza/bln1ABX1YLaCByZow+dK3ZlHn/UU0r4ewpAIZhve4vCvAsMe5+6KJH8ft/OKXXQY06h6jCythLV4h18gY/sYosOa+/4XgpmBiE7fDeFRKVjP3mvkxMpxce+ckOFae2+aJu51h513S9kxY2PmKaV/JU9HBYO+yO4j+j24v";

static const gchar MOCK_RSA_FP[] = "0e:6a:c8:b1:07:72:e2:04:95:9f:0e:b3:56:af:48:e2";

static void
test_unknown_host_key (TestCase *test,
                       gconstpointer data)
{
  WebSocketConnection *ws;
  CockpitWebService *service;
  GBytes *received = NULL;
  gchar *knownhosts = g_strdup_printf ("[127.0.0.1]:%d %s", (int)test->ssh_port, MOCK_RSA_KEY);

  cockpit_expect_info ("*New connection from*");
  cockpit_expect_log ("cockpit-protocol", G_LOG_LEVEL_MESSAGE, "*host key for server is not known*");

  /* No known hosts */
  cockpit_ws_known_hosts = "/dev/null";

  start_web_service_and_connect_client (test, data, &ws, &service);
  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);

  /* Should get an init message */
  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "init", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  /* Should close right after opening */
  while (received == NULL && web_socket_connection_get_ready_state (ws) != WEB_SOCKET_STATE_CLOSED)
    g_main_context_iteration (NULL, TRUE);

  /* And we should have received a close message */
  g_assert (received != NULL);
  expect_control_message (received, "close", "4", "reason", "unknown-hostkey",
                          "host-key", knownhosts,
                          "host-fingerprint", MOCK_RSA_FP,
                          NULL);
  g_bytes_unref (received);
  received = NULL;

  close_client_and_stop_web_service (test, ws, service);
  g_free (knownhosts);
}

static void
test_expect_host_key (TestCase *test,
                      gconstpointer data)
{
  WebSocketConnection *ws;
  CockpitWebService *service;
  GBytes *received = NULL;
  gchar *knownhosts = g_strdup_printf ("[127.0.0.1]:%d %s", (int)test->ssh_port, MOCK_RSA_KEY);

  /* No known hosts */
  cockpit_ws_known_hosts = "/dev/null";

  start_web_service_and_create_client (test, data, &ws, &service);
  WAIT_UNTIL (web_socket_connection_get_ready_state (ws) != WEB_SOCKET_STATE_CONNECTING);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);

  send_control_message (ws, "init", NULL, BUILD_INTS, "version", 0, NULL);
  send_control_message (ws, "open", "4",
                        "payload", "test-text",
                        "host-key", knownhosts,
                        NULL);

  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);

  /* Should get an init message */
  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "init", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  /* Should close right after opening */
  while (received == NULL && web_socket_connection_get_ready_state (ws) != WEB_SOCKET_STATE_CLOSED)
    g_main_context_iteration (NULL, TRUE);

  /* And we should have received an open message even though no known hosts */
  g_assert (received != NULL);
  expect_control_message (received, "open", "4", "payload", "test-text", NULL);
  g_bytes_unref (received);
  received = NULL;

  close_client_and_stop_web_service (test, ws, service);
  g_free (knownhosts);
}

static const TestFixture fixture_bad_origin_rfc6455 = {
  .web_socket_flavor = WEB_SOCKET_FLAVOR_RFC6455,
  .origin = "http://another-place.com",
};

static const TestFixture fixture_bad_origin_hixie76 = {
  .web_socket_flavor = WEB_SOCKET_FLAVOR_HIXIE76,
  .origin = "http://another-place.com",
};

static void
test_bad_origin (TestCase *test,
                 gconstpointer data)
{
  WebSocketConnection *ws;
  CockpitWebService *service;
  GError *error = NULL;

  cockpit_expect_log ("WebSocket", G_LOG_LEVEL_MESSAGE, "*received request from bad Origin*");
  cockpit_expect_log ("cockpit-ws", G_LOG_LEVEL_MESSAGE, "*invalid handshake*");
  cockpit_expect_log ("WebSocket", G_LOG_LEVEL_MESSAGE, "*unexpected status: 403*");

  start_web_service_and_create_client (test, data, &ws, &service);

  g_signal_handlers_disconnect_by_func (ws, on_error_not_reached, NULL);
  g_signal_connect (ws, "error", G_CALLBACK (on_error_copy), &error);

  while (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_CONNECTING ||
         web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_CLOSING)
    g_main_context_iteration (NULL, TRUE);
  g_assert_cmpint (web_socket_connection_get_ready_state (ws), ==, WEB_SOCKET_STATE_CLOSED);
  g_assert_error (error, WEB_SOCKET_ERROR, WEB_SOCKET_CLOSE_PROTOCOL);

  close_client_and_stop_web_service (test, ws, service);
  g_clear_error (&error);
}

static void
test_fail_spawn (TestCase *test,
                 gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  CockpitWebService *service;

  /* Fail to spawn this program */
  cockpit_ws_bridge_program = "/nonexistant";

  start_web_service_and_connect_client (test, data, &ws, &service);
  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);
  g_signal_handlers_disconnect_by_func (ws, on_error_not_reached, NULL);

  /* Should get an init message */
  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "init", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  /* Channel should close immediately */
  WAIT_UNTIL (received != NULL);

  /* But we should have gotten failure message, about the spawn */
  expect_control_message (received, "close", "4", "reason", "no-cockpit", NULL);
  g_bytes_unref (received);

  close_client_and_stop_web_service (test, ws, service);
}

static gboolean
on_timeout_dummy (gpointer unused)
{
  return TRUE;
}

static void
test_timeout_session (TestCase *test,
                      gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  CockpitWebService *service;
  GError *error = NULL;
  JsonObject *object;
  GBytes *payload;
  gchar *unused;
  pid_t pid;
  guint sig;
  guint tag;

  cockpit_ws_session_timeout = 1;

  /* This sends us a mesage with a pid in it on channel ' ' */
  cockpit_ws_bridge_program = SRCDIR "/src/ws/mock-pid-cat";

  /* Start the client */
  start_web_service_and_create_client (test, data, &ws, &service);
  while (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_CONNECTING)
    g_main_context_iteration (NULL, TRUE);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);
  sig = g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);

  /* Queue channel open/close, so we can guarantee having a session */
  send_control_message (ws, "init", NULL, BUILD_INTS, "version", 0, NULL);
  send_control_message (ws, "open", "11x", "payload", "test-text", NULL);

  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "init", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  /* First we should receive the pid message from mock-pid-cat */
  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);

  payload = cockpit_transport_parse_frame (received, &unused);
  g_assert (payload);
  g_bytes_unref (received);
  g_free (unused);

  object = cockpit_json_parse_bytes (payload, &error);
  g_assert_no_error (error);
  pid = json_object_get_int_member (object, "pid");
  json_object_unref (object);
  g_bytes_unref (payload);

  g_signal_handler_disconnect (ws, sig);

  send_control_message (ws, "close", "11x", NULL);

  /* The process should exit shortly */
  tag = g_timeout_add_seconds (1, on_timeout_dummy, NULL);
  while (kill (pid, 0) == 0)
    g_main_context_iteration (NULL, TRUE);
  g_source_remove (tag);

  g_assert_cmpint (errno, ==, ESRCH);

  close_client_and_stop_web_service (test, ws, service);
}

static void
on_idling_set_flag (CockpitWebService *service,
                    gpointer data)
{
  gboolean *flag = data;
  g_assert (*flag == FALSE);
  *flag = TRUE;
}

static void
test_idling (TestCase *test,
             gconstpointer data)
{
  WebSocketConnection *client;
  CockpitWebService *service;
  gboolean flag = FALSE;
  CockpitPipe *pipe;

  const gchar *argv[] = {
    BUILDDIR "/cockpit-bridge",
    NULL
  };

  cockpit_ws_default_host_header = "127.0.0.1";

  /* This is web_socket_client_new_for_stream() with a flavor passed in fixture */
  client = g_object_new (WEB_SOCKET_TYPE_CLIENT,
                         "url", "ws://127.0.0.1/unused",
                         "origin", "http://127.0.0.1",
                         "io-stream", test->io_a,
                         "flavor", 0,
                         NULL);

  pipe = cockpit_pipe_spawn (argv, NULL, NULL);
  service = cockpit_web_service_new (test->creds, pipe);
  g_object_unref (pipe);

  g_signal_connect (service, "idling", G_CALLBACK (on_idling_set_flag), &flag);
  g_assert (cockpit_web_service_get_idling (service));

  cockpit_web_service_socket (service, test->io_b, NULL, NULL);
  g_assert (!cockpit_web_service_get_idling (service));

  while (web_socket_connection_get_ready_state (client) == WEB_SOCKET_STATE_CONNECTING)
    g_main_context_iteration (NULL, TRUE);
  g_assert (web_socket_connection_get_ready_state (client) == WEB_SOCKET_STATE_OPEN);

  web_socket_connection_close (client, WEB_SOCKET_CLOSE_NORMAL, "aoeuaoeuaoeu");
  while (web_socket_connection_get_ready_state (client) != WEB_SOCKET_STATE_CLOSED)
    g_main_context_iteration (NULL, TRUE);

  /* Now the web service should go idle and fire idling signal */
  while (!flag)
    g_main_context_iteration (NULL, TRUE);

  g_assert (cockpit_web_service_get_idling (service));

  g_object_unref (service);
  g_object_unref (client);
}

static void
test_dispose (TestCase *test,
              gconstpointer data)
{
  WebSocketConnection *client;
  CockpitWebService *service;
  CockpitPipe *pipe;

  const gchar *argv[] = {
    BUILDDIR "/cockpit-bridge",
    NULL
  };

  cockpit_ws_default_host_header = "127.0.0.1";

  /* This is web_socket_client_new_for_stream() with a flavor passed in fixture */
  client = g_object_new (WEB_SOCKET_TYPE_CLIENT,
                         "url", "ws://127.0.0.1/unused",
                         "origin", "http://127.0.0.1",
                         "io-stream", test->io_a,
                         "flavor", 0,
                         NULL);

  pipe = cockpit_pipe_spawn (argv, NULL, NULL);
  service = cockpit_web_service_new (test->creds, pipe);
  g_object_unref (pipe);

  cockpit_web_service_socket (service, test->io_b, NULL, NULL);

  while (web_socket_connection_get_ready_state (client) == WEB_SOCKET_STATE_CONNECTING)
    g_main_context_iteration (NULL, TRUE);
  g_assert (web_socket_connection_get_ready_state (client) == WEB_SOCKET_STATE_OPEN);

  /* Dispose the WebSocket ... this is what happens on forceful logout */
  g_object_run_dispose (G_OBJECT (service));

  while (web_socket_connection_get_ready_state (client) != WEB_SOCKET_STATE_CLOSED)
    g_main_context_iteration (NULL, TRUE);

  g_object_unref (service);
  g_object_unref (client);
}

static void
test_logout (TestCase *test,
             gconstpointer data)
{
  WebSocketConnection *ws;
  CockpitWebService *service;
  GBytes *message = NULL;

  start_web_service_and_create_client (test, data, &ws, &service);
  WAIT_UNTIL (web_socket_connection_get_ready_state (ws) != WEB_SOCKET_STATE_CONNECTING);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);

  /* Send the logout control message */
  send_control_message (ws, "init", NULL, BUILD_INTS, "version", 0, NULL);

  data = "\n{ \"command\": \"logout\", \"disconnect\": true }";
  message = g_bytes_new_static (data, strlen (data));
  web_socket_connection_send (ws, WEB_SOCKET_DATA_TEXT, NULL, message);
  g_bytes_unref (message);

  while (web_socket_connection_get_ready_state (ws) != WEB_SOCKET_STATE_CLOSED)
    g_main_context_iteration (NULL, TRUE);

  close_client_and_stop_web_service (test, ws, service);
}

typedef struct {
  CockpitWebService *service;
  GIOStream *io;
  GMemoryOutputStream *output;
  CockpitPipe *pipe;
} TestResourceCase;

static void
setup_resource (TestResourceCase *tc,
                gconstpointer data)
{
  GInputStream *input;
  GOutputStream *output;
  CockpitCreds *creds;
  gchar **environ;
  const gchar *user;

  const gchar *argv[] = {
    BUILDDIR "/cockpit-bridge",
    NULL
  };

  environ = g_get_environ ();
  environ = g_environ_setenv (environ, "XDG_DATA_DIRS", SRCDIR "/src/bridge/mock-resource/system", TRUE);
  environ = g_environ_setenv (environ, "XDG_DATA_HOME", SRCDIR "/src/bridge/mock-resource/home", TRUE);

  /* Start up a cockpit-bridge here */
  tc->pipe = cockpit_pipe_spawn (argv, (const gchar **)environ, NULL);

  g_strfreev (environ);

  user = g_get_user_name ();
  creds = cockpit_creds_new (user, COCKPIT_CRED_PASSWORD, PASSWORD, NULL);

  tc->service = cockpit_web_service_new (creds, tc->pipe);

  cockpit_creds_unref (creds);

  input = g_memory_input_stream_new_from_data ("", 0, NULL);
  output = g_memory_output_stream_new (NULL, 0, g_realloc, g_free);
  tc->io = mock_io_stream_new (input, output);
  tc->output = G_MEMORY_OUTPUT_STREAM (output);
  g_object_unref (input);
}

static void
teardown_resource (TestResourceCase *tc,
                   gconstpointer data)
{
  cockpit_assert_expected ();

  g_object_add_weak_pointer (G_OBJECT (tc->service), (gpointer *)&tc->service);
  g_object_unref (tc->service);
  g_assert (tc->service == NULL);

  g_object_unref (tc->io);
  g_object_unref (tc->output);
  g_object_unref (tc->pipe);
}

static void
test_resource_simple (TestResourceCase *tc,
                      gconstpointer data)
{
  CockpitWebResponse *response;
  GError *error = NULL;
  GBytes *bytes;

  response = cockpit_web_response_new (tc->io, "/cockpit/another/test.html", NULL);

  cockpit_web_service_resource (tc->service, response);

  while (cockpit_web_response_get_state (response) != COCKPIT_WEB_RESPONSE_SENT)
    g_main_context_iteration (NULL, TRUE);

  g_output_stream_close (G_OUTPUT_STREAM (tc->output), NULL, &error);
  g_assert_no_error (error);

  bytes = g_memory_output_stream_steal_as_bytes (tc->output);
  cockpit_assert_bytes_eq (bytes,
                           "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/html\r\n"
                           "Transfer-Encoding: chunked\r\n"
                           "\r\n"
                           "52\r\n"
                           "<html>\n"
                           "<head>\n"
                           "<title>In home dir</title>\n"
                           "</head>\n"
                           "<body>In home dir</body>\n"
                           "</html>\n"
                           "\r\n"
                           "0\r\n\r\n", -1);
  g_bytes_unref (bytes);
  g_object_unref (response);
}

static void
test_resource_host (TestResourceCase *tc,
                    gconstpointer data)
{
  CockpitWebResponse *response;
  GError *error = NULL;
  GBytes *bytes;

  response = cockpit_web_response_new (tc->io, "/cockpit/another@localhost/test.html", NULL);

  cockpit_web_service_resource (tc->service, response);

  while (cockpit_web_response_get_state (response) != COCKPIT_WEB_RESPONSE_SENT)
    g_main_context_iteration (NULL, TRUE);

  g_output_stream_close (G_OUTPUT_STREAM (tc->output), NULL, &error);
  g_assert_no_error (error);

  bytes = g_memory_output_stream_steal_as_bytes (tc->output);
  cockpit_assert_bytes_eq (bytes,
                           "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/html\r\n"
                           "Transfer-Encoding: chunked\r\n"
                           "\r\n"
                           "52\r\n"
                           "<html>\n"
                           "<head>\n"
                           "<title>In home dir</title>\n"
                           "</head>\n"
                           "<body>In home dir</body>\n"
                           "</html>\n"
                           "\r\n"
                           "0\r\n\r\n", -1);
  g_bytes_unref (bytes);
  g_object_unref (response);
}

static void
test_resource_not_found (TestResourceCase *tc,
                         gconstpointer data)
{
  CockpitWebResponse *response;
  GError *error = NULL;
  GBytes *bytes;

  response = cockpit_web_response_new (tc->io, "/cockpit/another@localhost/not-exist", NULL);

  cockpit_web_service_resource (tc->service, response);

  while (cockpit_web_response_get_state (response) != COCKPIT_WEB_RESPONSE_SENT)
    g_main_context_iteration (NULL, TRUE);

  g_output_stream_close (G_OUTPUT_STREAM (tc->output), NULL, &error);
  g_assert_no_error (error);

  bytes = g_memory_output_stream_steal_as_bytes (tc->output);
  cockpit_assert_bytes_eq (bytes,
                           "HTTP/1.1 404 Not Found\r\n"
                           "Content-Length: 76\r\n"
                           "\r\n"
                           "<html><head><title>404 Not Found</title></head><body>Not Found</body></html>", -1);
  g_bytes_unref (bytes);
  g_object_unref (response);
}

static void
test_resource_no_path (TestResourceCase *tc,
                       gconstpointer data)
{
  CockpitWebResponse *response;
  GError *error = NULL;
  GBytes *bytes;

  /* Missing path after package */
  response = cockpit_web_response_new (tc->io, "/cockpit/another@localhost", NULL);

  cockpit_web_service_resource (tc->service, response);

  while (cockpit_web_response_get_state (response) != COCKPIT_WEB_RESPONSE_SENT)
    g_main_context_iteration (NULL, TRUE);

  g_output_stream_close (G_OUTPUT_STREAM (tc->output), NULL, &error);
  g_assert_no_error (error);

  bytes = g_memory_output_stream_steal_as_bytes (tc->output);
  cockpit_assert_bytes_eq (bytes,
                           "HTTP/1.1 404 Not Found\r\n"
                           "Content-Length: 76\r\n"
                           "\r\n"
                           "<html><head><title>404 Not Found</title></head><body>Not Found</body></html>", -1);
  g_bytes_unref (bytes);
  g_object_unref (response);
}


static void
test_resource_failure (TestResourceCase *tc,
                       gconstpointer data)
{
  CockpitWebResponse *response;
  GError *error = NULL;
  GBytes *bytes;
  GPid pid;

  cockpit_expect_message ("*: failed to retrieve resource: terminated");

  response = cockpit_web_response_new (tc->io, "/cockpit/another/test.html", NULL);

  /* Now kill the bridge */
  g_assert (cockpit_pipe_get_pid (tc->pipe, &pid));
  g_assert_cmpint (pid, >, 0);
  g_assert_cmpint (kill (pid, SIGTERM), ==, 0);

  cockpit_web_service_resource (tc->service, response);

  while (cockpit_web_response_get_state (response) != COCKPIT_WEB_RESPONSE_SENT)
    g_main_context_iteration (NULL, TRUE);

  g_output_stream_close (G_OUTPUT_STREAM (tc->output), NULL, &error);
  g_assert_no_error (error);

  bytes = g_memory_output_stream_steal_as_bytes (tc->output);
  cockpit_assert_bytes_eq (bytes,
                           "HTTP/1.1 500 Internal Server Error\r\n"
                           "Content-Length: 100\r\n"
                           "\r\n"
                           "<html><head><title>500 Internal Server Error</title></head><body>Internal Server Error</body></html>", -1);
  g_bytes_unref (bytes);
  g_object_unref (response);
}

static void
test_resource_packages (TestResourceCase *tc,
                        gconstpointer data)
{
  GAsyncResult *result = NULL;
  JsonArray *packages;

  cockpit_web_service_packages (tc->service, "localhost", on_ready_get_result, &result);

  while (result == NULL)
    g_main_context_iteration (NULL, TRUE);

  packages = cockpit_web_service_packages_finish (tc->service, result);
  g_object_unref (result);

  g_assert (packages != NULL);
  cockpit_assert_json_eq (packages,
                          "["
                          " {"
                          "  \"id\": [\"$2362deb82fad54aca51092c505a5660ac6c45a9f\",\"one\",\"second\",\"two\"],"
                          "  \"manifest\": { \"description\": \"second dummy description\"}"
                          " },{"
                          "  \"id\": [ \"$fec489a692ee808950f34f6c519803aed65e1849\", \"test\" ],"
                          "  \"manifest\" : { \"description\" : \"dummy\"}"
                          " },{"
                          "  \"id\": [ \"another\", \"marmalade\" ],"
                          "  \"manifest\" : { \"description\" : \"another\"}"
                          " }"
                          "]");

  json_array_unref (packages);
}

static void
test_resource_packages_failure (TestResourceCase *tc,
                                gconstpointer data)
{
  GAsyncResult *result = NULL;
  JsonArray *packages;
  GPid pid;

  cockpit_expect_message ("*: transport closed while listing cockpit packages: *");

  /* Now kill the bridge */
  g_assert (cockpit_pipe_get_pid (tc->pipe, &pid));
  g_assert_cmpint (pid, >, 0);
  g_assert_cmpint (kill (pid, SIGTERM), ==, 0);

  cockpit_web_service_packages (tc->service, "localhost", on_ready_get_result, &result);

  while (result == NULL)
    g_main_context_iteration (NULL, TRUE);

  packages = cockpit_web_service_packages_finish (tc->service, result);
  g_object_unref (result);

  g_assert (packages == NULL);
}

static void
test_resource_checksum (TestResourceCase *tc,
                        gconstpointer data)
{
  GAsyncResult *result = NULL;
  CockpitWebResponse *response;
  GError *error = NULL;
  GBytes *bytes;

  /* Do a package listing so that the web service knows the checksums for localhost */
  cockpit_web_service_packages (tc->service, "localhost", on_ready_get_result, &result);
  while (result == NULL)
    g_main_context_iteration (NULL, TRUE);
  json_array_unref (cockpit_web_service_packages_finish (tc->service, result));
  g_object_unref (result);

  response = cockpit_web_response_new (tc->io, "/cockpit/$fec489a692ee808950f34f6c519803aed65e1849/sub/file.ext", NULL);
  cockpit_web_service_resource (tc->service, response);

  while (cockpit_web_response_get_state (response) != COCKPIT_WEB_RESPONSE_SENT)
    g_main_context_iteration (NULL, TRUE);

  g_output_stream_close (G_OUTPUT_STREAM (tc->output), NULL, &error);
  g_assert_no_error (error);

  bytes = g_memory_output_stream_steal_as_bytes (tc->output);
  cockpit_assert_bytes_eq (bytes,
                           "HTTP/1.1 200 OK\r\n"
                           "Cache-Control: max-age=31556926, public\r\n"
                           "Transfer-Encoding: chunked\r\n"
                           "\r\n"
                           "2d\r\n"
                           "This is the minified file.ext Oh marmalaaade\n"
                           "\r\n"
                           "0\r\n\r\n", -1);
  g_bytes_unref (bytes);
  g_object_unref (response);
}

static void
test_resource_no_checksum (TestResourceCase *tc,
                           gconstpointer data)
{
  CockpitWebResponse *response;
  GError *error = NULL;
  GBytes *bytes;

  /* Missing checksum */
  response = cockpit_web_response_new (tc->io, "/cockpit/", NULL);

  cockpit_web_service_resource (tc->service, response);

  while (cockpit_web_response_get_state (response) != COCKPIT_WEB_RESPONSE_SENT)
    g_main_context_iteration (NULL, TRUE);

  g_output_stream_close (G_OUTPUT_STREAM (tc->output), NULL, &error);
  g_assert_no_error (error);

  bytes = g_memory_output_stream_steal_as_bytes (tc->output);
  cockpit_assert_bytes_eq (bytes,
                           "HTTP/1.1 404 Not Found\r\n"
                           "Content-Length: 76\r\n"
                           "\r\n"
                           "<html><head><title>404 Not Found</title></head><body>Not Found</body></html>", -1);
  g_bytes_unref (bytes);
  g_object_unref (response);
}

static void
test_resource_bad_checksum (TestResourceCase *tc,
                           gconstpointer data)
{
  CockpitWebResponse *response;
  GError *error = NULL;
  GBytes *bytes;

  /* Missing checksum */
  response = cockpit_web_response_new (tc->io, "/cockpit/09323094823029348/path", NULL);

  cockpit_web_service_resource (tc->service, response);

  while (cockpit_web_response_get_state (response) != COCKPIT_WEB_RESPONSE_SENT)
    g_main_context_iteration (NULL, TRUE);

  g_output_stream_close (G_OUTPUT_STREAM (tc->output), NULL, &error);
  g_assert_no_error (error);

  bytes = g_memory_output_stream_steal_as_bytes (tc->output);
  cockpit_assert_bytes_eq (bytes,
                           "HTTP/1.1 404 Not Found\r\n"
                           "Content-Length: 76\r\n"
                           "\r\n"
                           "<html><head><title>404 Not Found</title></head><body>Not Found</body></html>", -1);
  g_bytes_unref (bytes);
  g_object_unref (response);
}

static gboolean
on_hack_raise_sigchld (gpointer user_data)
{
  raise (SIGCHLD);
  return TRUE;
}

int
main (int argc,
      char *argv[])
{
  cockpit_test_init (&argc, &argv);

  /*
   * HACK: Work around races in glib SIGCHLD handling.
   *
   * https://bugzilla.gnome.org/show_bug.cgi?id=731771
   * https://bugzilla.gnome.org/show_bug.cgi?id=711090
   */
  g_timeout_add_seconds (1, on_hack_raise_sigchld, NULL);

  /* We don't want to test the ping functionality in these tests */
  cockpit_ws_ping_interval = G_MAXUINT;

  static const TestFixture fixture_rfc6455 = {
      .web_socket_flavor = WEB_SOCKET_FLAVOR_RFC6455,
  };

  static const TestFixture fixture_hixie76 = {
      .web_socket_flavor = WEB_SOCKET_FLAVOR_HIXIE76,
  };

  g_test_add ("/web-service/handshake-and-auth/rfc6455", TestCase,
              &fixture_rfc6455, setup_for_socket,
              test_handshake_and_auth, teardown_for_socket);
  g_test_add ("/web-service/handshake-and-auth/hixie76", TestCase,
              &fixture_hixie76, setup_for_socket,
              test_handshake_and_auth, teardown_for_socket);

  g_test_add ("/web-service/echo-message/rfc6455", TestCase,
              &fixture_rfc6455, setup_for_socket,
              test_handshake_and_echo, teardown_for_socket);
  g_test_add ("/web-service/echo-message/hixie76", TestCase,
              &fixture_hixie76, setup_for_socket,
              test_handshake_and_echo, teardown_for_socket);
  g_test_add ("/web-service/echo-message/large", TestCase,
              &fixture_rfc6455, setup_for_socket,
              test_echo_large, teardown_for_socket);

  g_test_add ("/web-service/close-error", TestCase,
              NULL, setup_for_socket,
              test_close_error, teardown_for_socket);
  g_test_add ("/web-service/unauthenticated", TestCase,
              NULL, setup_for_socket,
              test_socket_unauthenticated, teardown_for_socket);
  g_test_add ("/web-service/null-creds", TestCase, NULL,
              setup_for_socket, test_socket_null_creds, teardown_for_socket);
  g_test_add ("/web-service/unknown-hostkey", TestCase,
              NULL, setup_for_socket,
              test_unknown_host_key, teardown_for_socket);
  g_test_add ("/web-service/expect-host-key", TestCase,
              NULL, setup_for_socket,
              test_expect_host_key, teardown_for_socket);
  g_test_add ("/web-service/no-init", TestCase, NULL,
              setup_for_socket, test_no_init, teardown_for_socket);
  g_test_add ("/web-service/wrong-init-version", TestCase, NULL,
              setup_for_socket, test_wrong_init_version, teardown_for_socket);
  g_test_add ("/web-service/bad-init-version", TestCase, NULL,
              setup_for_socket, test_bad_init_version, teardown_for_socket);

  g_test_add ("/web-service/bad-origin/rfc6455", TestCase,
              &fixture_bad_origin_rfc6455, setup_for_socket,
              test_bad_origin, teardown_for_socket);
  g_test_add ("/web-service/bad-origin/hixie76", TestCase,
              &fixture_bad_origin_hixie76, setup_for_socket,
              test_bad_origin, teardown_for_socket);

  g_test_add ("/web-service/fail-spawn/rfc6455", TestCase,
              &fixture_rfc6455, setup_for_socket,
              test_fail_spawn, teardown_for_socket);
  g_test_add ("/web-service/fail-spawn/hixie76", TestCase,
              &fixture_hixie76, setup_for_socket,
              test_fail_spawn, teardown_for_socket);

  g_test_add ("/web-service/specified-creds", TestCase,
              &fixture_rfc6455, setup_for_socket_spec,
              test_specified_creds, teardown_for_socket);
  g_test_add ("/web-service/specified-creds-fail", TestCase,
              &fixture_rfc6455, setup_for_socket_spec,
              test_specified_creds_fail, teardown_for_socket);

  g_test_add ("/web-service/timeout-session", TestCase, NULL,
              setup_for_socket, test_timeout_session, teardown_for_socket);
  g_test_add ("/web-service/idling-signal", TestCase, NULL,
              setup_for_socket, test_idling, teardown_for_socket);
  g_test_add ("/web-service/force-dispose", TestCase, NULL,
              setup_for_socket, test_dispose, teardown_for_socket);
  g_test_add ("/web-service/logout", TestCase, NULL,
              setup_for_socket, test_logout, teardown_for_socket);

  g_test_add ("/web-service/resource/simple", TestResourceCase, NULL,
              setup_resource, test_resource_simple, teardown_resource);
  g_test_add ("/web-service/resource/host", TestResourceCase, NULL,
              setup_resource, test_resource_host, teardown_resource);
  g_test_add ("/web-service/resource/not-found", TestResourceCase, NULL,
              setup_resource, test_resource_not_found, teardown_resource);
  g_test_add ("/web-service/resource/no-path", TestResourceCase, NULL,
              setup_resource, test_resource_no_path, teardown_resource);
  g_test_add ("/web-service/resource/failure", TestResourceCase, NULL,
              setup_resource, test_resource_failure, teardown_resource);
  g_test_add ("/web-service/resource/packages", TestResourceCase, NULL,
              setup_resource, test_resource_packages, teardown_resource);
  g_test_add ("/web-service/resource/packages-failure", TestResourceCase, NULL,
              setup_resource, test_resource_packages_failure, teardown_resource);
  g_test_add ("/web-service/resource/checksum", TestResourceCase, NULL,
              setup_resource, test_resource_checksum, teardown_resource);
  g_test_add ("/web-service/resource/no-checksum", TestResourceCase, NULL,
              setup_resource, test_resource_no_checksum, teardown_resource);
  g_test_add ("/web-service/resource/bad-checksum", TestResourceCase, NULL,
              setup_resource, test_resource_bad_checksum, teardown_resource);

  return g_test_run ();
}
