/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright © 2008 Christian Kellner, Samuel Cormier-Iijima
 * Copyright © 2009 codethink
 * Copyright © 2009 Red Hat, Inc
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Authors: Christian Kellner <gicmo@gnome.org>
 *          Samuel Cormier-Iijima <sciyoshi@gmail.com>
 *          Ryan Lortie <desrt@desrt.ca>
 *          Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"
#include "gsocketlistener.h"

#include <gio/gsimpleasyncresult.h>
#include <gio/gcancellable.h>
#include <gio/gsocketaddress.h>
#include <gio/ginetaddress.h>
#include <gio/gioerror.h>
#include <gio/gsocket.h>
#include <gio/gsocketconnection.h>
#include <gio/ginetsocketaddress.h>
#include "glibintl.h"

#include "gioalias.h"

/**
 * SECTION: gsocketlistener
 * @title: GSocketListener
 * @short_description: a high-level helper object for server sockets
 * @see_also: #GThreadedSocketService, #GSocketService.
 *
 * A #GSocketListener is an object that keeps track of a set
 * of server sockets and helps you accept sockets from any of the
 * socket, either sync or async.
 *
 * If you want to implement a network server, also look at #GSocketService
 * and #GThreadedSocketService which are subclass of #GSocketListener
 * that makes this even easier.
 *
 * Since: 2.22
 */

G_DEFINE_TYPE (GSocketListener, g_socket_listener, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_LISTEN_BACKLOG
};


static GQuark source_quark = 0;

struct _GSocketListenerPrivate
{
  GPtrArray           *sockets;
  GMainContext        *main_context;
  int                 listen_backlog;
  guint               closed : 1;
};

static void
g_socket_listener_finalize (GObject *object)
{
  GSocketListener *listener = G_SOCKET_LISTENER (object);

  if (listener->priv->main_context)
    g_main_context_unref (listener->priv->main_context);

  if (!listener->priv->closed)
    g_socket_listener_close (listener);

  g_ptr_array_free (listener->priv->sockets, TRUE);

  G_OBJECT_CLASS (g_socket_listener_parent_class)
    ->finalize (object);
}

static void
g_socket_listener_get_property (GObject    *object,
				guint       prop_id,
				GValue     *value,
				GParamSpec *pspec)
{
  GSocketListener *listener = G_SOCKET_LISTENER (object);

  switch (prop_id)
    {
      case PROP_LISTEN_BACKLOG:
        g_value_set_int (value, listener->priv->listen_backlog);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
g_socket_listener_set_property (GObject      *object,
				guint         prop_id,
				const GValue *value,
				GParamSpec   *pspec)
{
  GSocketListener *listener = G_SOCKET_LISTENER (object);

  switch (prop_id)
    {
      case PROP_LISTEN_BACKLOG:
	g_socket_listener_set_backlog (listener, g_value_get_int (value));
	break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
g_socket_listener_class_init (GSocketListenerClass *klass)
{
  GObjectClass *gobject_class G_GNUC_UNUSED = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GSocketListenerPrivate));

  gobject_class->finalize = g_socket_listener_finalize;
  gobject_class->set_property = g_socket_listener_set_property;
  gobject_class->get_property = g_socket_listener_get_property;
  g_object_class_install_property (gobject_class, PROP_LISTEN_BACKLOG,
                                   g_param_spec_int ("listen-backlog",
                                                     P_("Listen backlog"),
                                                     P_("outstanding connections in the listen queue"),
                                                     0,
                                                     2000,
                                                     10,
                                                     G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  source_quark = g_quark_from_static_string ("g-socket-listener-source");
}

static void
g_socket_listener_init (GSocketListener *listener)
{
  listener->priv = G_TYPE_INSTANCE_GET_PRIVATE (listener,
						G_TYPE_SOCKET_LISTENER,
						GSocketListenerPrivate);
  listener->priv->sockets =
    g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  listener->priv->listen_backlog = 10;
}

/**
 * g_socket_service_new:
 *
 * Creates a new #GSocketListener with no sockets to listen for.
 * New listeners can be added with e.g. g_socket_listener_add_address()
 * or g_socket_listener_add_inet_port().
 *
 * Returns: a new #GSocketListener.
 *
 * Since: 2.22
 **/
GSocketListener *
g_socket_listener_new (void)
{
  return g_object_new (G_TYPE_SOCKET_LISTENER, NULL);
}

static gboolean
check_listener (GSocketListener *listener,
		GError **error)
{
  if (listener->priv->closed)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
			   _("Listener is already closed"));
      return FALSE;
    }

  return TRUE;
}

/**
 * g_socket_listener_add_socket:
 * @listener: a #GSocketListener
 * @socket: a listening #GSocket
 * @source_object: Optional #GObject identifying this source
 * @error: #GError for error reporting, or %NULL to ignore.
 *
 * Adds @socket to the set of sockets that we try to accept
 * new clients from. The socket must be bound to a local
 * address and listened to.
 *
 * @source_object will be passed out in the various calls
 * to accept to identify this particular source, which is
 * useful if you're listening on multiple addresses and do
 * different things depending on what address is connected to.
 *
 * Returns: %TRUE on success, %FALSE on error.
 *
 * Since: 2.22
 **/
gboolean
g_socket_listener_add_socket (GSocketListener *listener,
			      GSocket *socket,
			      GObject *source_object,
			      GError **error)
{
  if (!check_listener (listener, error))
    return FALSE;

  /* TODO: Check that socket it is bound & not closed? */

  if (g_socket_is_closed (socket))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Added socket is closed"));
      return FALSE;
    }

  g_ptr_array_add (listener->priv->sockets, socket);
  g_socket_set_listen_backlog (socket, listener->priv->listen_backlog);

  if (source_object)
    g_object_set_qdata_full (G_OBJECT (socket), source_quark,
			     g_object_ref (source_object), g_object_unref);

  return TRUE;
}

/**
 * g_socket_listener_add_socket:
 * @listener: a #GSocketListener
 * @address: a #GSocketAddres
 * @type: a #GSocketType
 * @protocol: a protocol name, or %NULL
 * @source_object: Optional #GObject identifying this source
 * @error: #GError for error reporting, or %NULL to ignore.
 *
 * Creates a socket of type @type and protocol @protocol, binds
 * it to @address and adds it to the set of sockets we're accepting
 * sockets from.
 *
 * @source_object will be passed out in the various calls
 * to accept to identify this particular source, which is
 * useful if you're listening on multiple addresses and do
 * different things depending on what address is connected to.
 *
 * Returns: %TRUE on success, %FALSE on error.
 *
 * Since: 2.22
 **/
gboolean
g_socket_listener_add_address (GSocketListener *listener,
			       GSocketAddress *address,
			       GSocketType type,
			       const char *protocol,
			       GObject *source_object,
			       GError **error)
{
  GSocketFamily family;
  GSocket *socket;

  if (!check_listener (listener, error))
    return FALSE;

  family = g_socket_address_get_family (address);
  socket = g_socket_new (family, type,
			 g_socket_protocol_id_lookup_by_name (protocol), error);
  if (socket == NULL)
    return FALSE;

  if (!g_socket_bind (socket, address, TRUE, error) ||
      !g_socket_listen (socket, error) ||
      !g_socket_listener_add_socket (listener, socket,
				     source_object,
				     error))
    {
      g_object_unref (socket);
      return FALSE;
    }

  if (G_SOCKET_LISTENER_GET_CLASS (listener)->changed)
    G_SOCKET_LISTENER_GET_CLASS (listener)->changed (listener);

  return TRUE;
}

/**
 * g_socket_listener_add_inet_port:
 * @listener: a #GSocketListener
 * @port: an ip port number
 * @source_object: Optional #GObject identifying this source
 * @error: #GError for error reporting, or %NULL to ignore.
 *
 * Helper function for g_socket_listener_add_address() that
 * creates a TCP/IP socket listening on IPv4 and IPv6 (if
 * supported) on the specified port on all interfaces.
 *
 * @source_object will be passed out in the various calls
 * to accept to identify this particular source, which is
 * useful if you're listening on multiple addresses and do
 * different things depending on what address is connected to.
 *
 * Returns: %TRUE on success, %FALSE on error.
 *
 * Since: 2.22
 **/
gboolean
g_socket_listener_add_inet_port (GSocketListener *listener,
				 int port,
				 GObject *source_object,
				 GError **error)
{
  GSocketAddress *address4, *address6;
  GInetAddress *inet_address;
  gboolean res;

  if (!check_listener (listener, error))
    return FALSE;

  inet_address = g_inet_address_new_any (G_SOCKET_FAMILY_IPV4);
  address4 = g_inet_socket_address_new (inet_address, port);
  g_object_unref (inet_address);

  inet_address = g_inet_address_new_any (G_SOCKET_FAMILY_IPV6);
  address6 = g_inet_socket_address_new (inet_address, port);
  g_object_unref (inet_address);

  if (!g_socket_listener_add_address (listener,
				      address6,
				      G_SOCKET_TYPE_STREAM,
				      NULL,
				      source_object,
				      NULL))
    {
      /* Failed, to create ipv6, socket, just use ipv4,
	 return any error */
      res = g_socket_listener_add_address (listener,
					   address4,
					   G_SOCKET_TYPE_STREAM,
					   NULL,
					   source_object,
					   error);
    }
  else
    {
      /* Succeeded with ipv6, also try ipv4 in case its ipv6 only,
	 but ignore errors here */
      res = TRUE;
      g_socket_listener_add_address (listener,
				     address4,
				     G_SOCKET_TYPE_STREAM,
				     NULL,
				     source_object,
				     NULL);
    }

  g_object_unref (address4);
  g_object_unref (address6);

  return res;
}

static GList *
add_sources (GSocketListener *listener,
	     GSocketSourceFunc callback,
	     gpointer callback_data,
	     GCancellable *cancellable,
	     GMainContext *context)
{
  GSocket *socket;
  GSource *source;
  GList *sources;
  int i;

  sources = NULL;
  for (i = 0; i < listener->priv->sockets->len; i++)
    {
      socket = listener->priv->sockets->pdata[i];

      source = g_socket_create_source (socket, G_IO_IN, cancellable);
      g_source_set_callback (source,
                             (GSourceFunc) callback,
                             callback_data, NULL);
      g_source_attach (source, context);

      sources = g_list_prepend (sources, source);
    }

  return sources;
}

static void
free_sources (GList *sources)
{
  GSource *source;
  while (sources != NULL)
    {
      source = sources->data;
      sources = g_list_delete_link (sources, sources);
      g_source_destroy (source);
      g_source_unref (source);
    }
}

struct AcceptData {
  GMainLoop *loop;
  GSocket *socket;
};

static gboolean
accept_callback (GSocket *socket,
		 GIOCondition condition,
		 gpointer user_data)
{
  struct AcceptData *data = user_data;

  data->socket = socket;
  g_main_loop_quit (data->loop);

  return TRUE;
}

/**
 * g_socket_listener_accept_socket:
 * @listener: a #GSocketListener
 * @source_object: location where #GObject pointer will be stored, or %NULL
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @error: #GError for error reporting, or %NULL to ignore.
 *
 * Blocks waiting for a client to connect to any of the sockets added
 * to the listener. Returns the #GSocket that was accepted.
 *
 * If you want to accept the high-level #GSocketConnection, not a #GSocket,
 * which is often the case, then you should use g_socket_listener_accept()
 * instead.
 *
 * If @source_object is not %NULL it will be filled out with the source
 * object specified when the corresponding socket or address was added
 * to the listener.
 *
 * If @cancellable is not NULL, then the operation can be cancelled by
 * triggering the cancellable object from another thread. If the operation
 * was cancelled, the error G_IO_ERROR_CANCELLED will be returned.
 *
 * Returns: a #GSocket on success, %NULL on error.
 *
 * Since: 2.22
 **/
GSocket *
g_socket_listener_accept_socket (GSocketListener  *listener,
				 GObject         **source_object,
				 GCancellable     *cancellable,
				 GError          **error)
{
  GSocket *accept_socket, *socket;

  g_return_val_if_fail (G_IS_SOCKET_LISTENER (listener), NULL);

  if (!check_listener (listener, error))
    return NULL;

  if (listener->priv->sockets->len == 1)
    {
      accept_socket = listener->priv->sockets->pdata[0];
      if (!g_socket_condition_wait (accept_socket, G_IO_IN,
				    cancellable, error))
	return NULL;
    }
  else
    {
      GList *sources;
      struct AcceptData data;
      GMainLoop *loop;

      if (listener->priv->main_context == NULL)
	listener->priv->main_context = g_main_context_new ();

      loop = g_main_loop_new (listener->priv->main_context, FALSE);
      data.loop = loop;
      sources = add_sources (listener,
			     accept_callback,
			     &data,
			     cancellable,
			     listener->priv->main_context);
      g_main_loop_run (loop);
      accept_socket = data.socket;
      free_sources (sources);
      g_main_loop_unref (loop);
    }

  if (!(socket = g_socket_accept (accept_socket, error)))
    return NULL;

  if (source_object)
    *source_object = g_object_get_qdata (G_OBJECT (accept_socket), source_quark);

  return socket;
}

/**
 * g_socket_listener_accept:
 * @listener: a #GSocketListener
 * @source_object: location where #GObject pointer will be stored, or %NULL
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @error: #GError for error reporting, or %NULL to ignore.
 *
 * Blocks waiting for a client to connect to any of the sockets added
 * to the listener. Returns a #GSocketConnection for the socket that was
 * accepted.
 *
 * If @source_object is not %NULL it will be filled out with the source
 * object specified when the corresponding socket or address was added
 * to the listener.
 *
 * If @cancellable is not NULL, then the operation can be cancelled by
 * triggering the cancellable object from another thread. If the operation
 * was cancelled, the error G_IO_ERROR_CANCELLED will be returned.
 *
 * Returns: a #GSocketConnection on success, %NULL on error.
 *
 * Since: 2.22
 **/
GSocketConnection *
g_socket_listener_accept (GSocketListener  *listener,
			  GObject         **source_object,
			  GCancellable     *cancellable,
			  GError          **error)
{
  GSocketConnection *connection;
  GSocket *socket;

  socket = g_socket_listener_accept_socket (listener,
					    source_object,
					    cancellable,
					    error);
  if (socket == NULL)
    return NULL;

  connection = g_socket_connection_factory_create_connection (socket);
  g_object_unref (socket);

  return connection;
}

struct AcceptAsyncData {
  GSimpleAsyncResult *simple;
  GCancellable *cancellable;
  GList *sources;
};

static gboolean
accept_ready (GSocket *accept_socket,
	      GIOCondition condition,
	      gpointer _data)
{
  struct AcceptAsyncData *data = _data;
  GError *error = NULL;

  if (!g_cancellable_set_error_if_cancelled (data->cancellable,
                                             &error))
    {
      GSocket *socket;
      GObject *source_object;

      socket = g_socket_accept (accept_socket, &error);
      if (socket)
	{
	  g_simple_async_result_set_op_res_gpointer (data->simple, socket,
						     g_object_unref);
	  source_object = g_object_get_qdata (G_OBJECT (accept_socket), source_quark);
	  if (source_object)
	    g_object_set_qdata_full (G_OBJECT (data->simple),
				     source_quark,
				     g_object_ref (source_object), g_object_unref);
	}
    }

  if (error)
    {
      g_simple_async_result_set_from_error (data->simple, error);
      g_error_free (error);
    }

  g_simple_async_result_complete_in_idle (data->simple);
  g_object_unref (data->simple);
  free_sources (data->sources);
  g_free (data);

  return FALSE;
}

/**
 * g_socket_listener_accept_socket_async:
 * @listener: a #GSocketListener
 * @cancellable: a #GCancellable, or %NULL
 * @callback: a #GAsyncReadyCallback
 * @user_data: user data for the callback
 *
 * This is the asynchronous version of g_socket_listener_accept_socket().
 *
 * When the operation is finished @callback will be
 * called. You can then call g_socket_listener_accept_socket_finish() to get
 * the result of the operation.
 *
 * Since: 2.22
 **/
void
g_socket_listener_accept_socket_async (GSocketListener      *listener,
				       GCancellable         *cancellable,
				       GAsyncReadyCallback   callback,
				       gpointer              user_data)
{
  struct AcceptAsyncData *data;
  GError *error = NULL;

  if (!check_listener (listener, &error))
    {
      g_simple_async_report_gerror_in_idle (G_OBJECT (listener),
					    callback, user_data,
					    error);
      g_error_free (error);
      return;
    }

  data = g_new0 (struct AcceptAsyncData, 1);
  data->simple = g_simple_async_result_new (G_OBJECT (listener),
					    callback, user_data,
					    g_socket_listener_accept_socket_async);
  data->cancellable = cancellable;
  data->sources = add_sources (listener,
			       accept_ready,
			       data,
			       cancellable,
			       NULL);
}

/**
 * g_socket_listener_accept_socket_finish:
 * @listener: a #GSocketListener
 * @result: a #GAsyncResult.
 * @source_object: Optional #GObject identifying this source
 * @error: a #GError location to store the error occuring, or %NULL to
 * ignore.
 *
 * Finishes an async accept operation. See g_socket_listener_accept_socket_async()
 *
 * Returns: a #GSocket on success, %NULL on error.
 *
 * Since: 2.22
 **/
GSocket *
g_socket_listener_accept_socket_finish (GSocketListener   *listener,
					GAsyncResult      *result,
					GObject          **source_object,
					GError           **error)
{
  GSocket *socket;
  GSimpleAsyncResult *simple;

  g_return_val_if_fail (G_IS_SOCKET_LISTENER (listener), FALSE);

  simple = G_SIMPLE_ASYNC_RESULT (result);

  if (g_simple_async_result_propagate_error (simple, error))
    return NULL;

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == g_socket_listener_accept_socket_async);

  socket = g_simple_async_result_get_op_res_gpointer (simple);

  if (source_object)
    *source_object = g_object_get_qdata (G_OBJECT (result), source_quark);

  return g_object_ref (socket);
}

/**
 * g_socket_listener_accept_socket_async:
 * @listener: a #GSocketListener
 * @cancellable: a #GCancellable, or %NULL
 * @callback: a #GAsyncReadyCallback
 * @user_data: user data for the callback
 *
 * This is the asynchronous version of g_socket_listener_accept().
 *
 * When the operation is finished @callback will be
 * called. You can then call g_socket_listener_accept_socket() to get
 * the result of the operation.
 *
 * Since: 2.22
 **/
void
g_socket_listener_accept_async (GSocketListener     *listener,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  g_socket_listener_accept_socket_async (listener,
					 cancellable,
					 callback,
					 user_data);
}

/**
 * g_socket_listener_accept_finish:
 * @listener: a #GSocketListener
 * @result: a #GAsyncResult.
 * @source_object: Optional #GObject identifying this source
 * @error: a #GError location to store the error occuring, or %NULL to
 * ignore.
 *
 * Finishes an async accept operation. See g_socket_listener_accept_async()
 *
 * Returns: a #GSocketConnection on success, %NULL on error.
 *
 * Since: 2.22
 **/
GSocketConnection *
g_socket_listener_accept_finish (GSocketListener *listener,
				 GAsyncResult *result,
				 GObject **source_object,
				 GError **error)
{
  GSocket *socket;
  GSocketConnection *connection;

  socket = g_socket_listener_accept_socket_finish (listener,
						   result,
						   source_object,
						   error);
  if (socket == NULL)
    return NULL;

  connection = g_socket_connection_factory_create_connection (socket);
  g_object_unref (socket);
  return connection;
}

/**
 * g_socket_listener_accept_finish:
 * @listener: a #GSocketListener
 * @listen_backlog: an integer
 *
 * Sets the listen backlog on the sockets in the listener.
 *
 * See g_socket_set_listen_backlog() for details
 *
 * Since: 2.22
 **/
void
g_socket_listener_set_backlog (GSocketListener *listener,
			       int listen_backlog)
{
  GSocket *socket;
  int i;

  if (listener->priv->closed)
    return;

  listener->priv->listen_backlog = listen_backlog;

  for (i = 0; i < listener->priv->sockets->len; i++)
    {
      socket = listener->priv->sockets->pdata[i];
      g_socket_set_listen_backlog (socket, listen_backlog);
    }
}

/**
 * g_socket_listener_close:
 * @listener: a #GSocketListener
 *
 * Closes all the sockets in the listener.
 *
 * Since: 2.22
 **/
void
g_socket_listener_close (GSocketListener *listener)
{
  GSocket *socket;
  int i;

  g_return_if_fail (G_IS_SOCKET_LISTENER (listener));

  if (listener->priv->closed)
    return;

  for (i = 0; i < listener->priv->sockets->len; i++)
    {
      socket = listener->priv->sockets->pdata[i];
      g_socket_close (socket, NULL);
    }
  listener->priv->closed = TRUE;
}

#define __G_SOCKET_LISTENER_C__
#include "gioaliasdef.c"