#include "ci-client.h"
#include <gio/gio.h>
#include <cinet.h>
#include <memory.h>

/* TODO: come up with a good solution for log/dlog */
#define DLOG(fmt, ...)
#define LOG(fmt, ...)

struct CIClientQuery {
    guint32 guid;
    CIQueryMsgCallback callback;
    gpointer userdata;
};

struct _CIClient {
    GSocketClient *client;
    GSocketConnection *connection;
    gchar *host;
    guint port;
    CIMsgCallback callback;
    CIClientStateChangedFunc state_changed_cb;
    CIClientState state;
    GCancellable *cancel;
    guint32 query_guid;
    GList *queries;
    guint timer_source_id;
    guint stream_source_id;
    gint retry_interval;
    guint32 auto_reconnect : 1;
    guint32 config_changed : 1;
};

void ci_client_handle_connection_lost(CIClient *client);
void ci_client_stop_timer(CIClient *client);

guint32 ci_client_gen_guid(CIClient *client)
{
    /* TODO: check that next id not in queue */
    g_return_val_if_fail(client != NULL, 0);

    ++client->query_guid;
    if (G_UNLIKELY(client->query_guid == 0))
        client->query_guid = 1;
    return client->query_guid;
}

struct CIClientQuery *ci_client_query_new(CIClient *client, CIQueryMsgCallback callback, gpointer userdata)
{
    g_return_val_if_fail(client != NULL, NULL);

    struct CIClientQuery *query = g_malloc(sizeof(struct CIClientQuery));
    query->guid = ci_client_gen_guid(client);
    query->callback = callback;
    query->userdata = userdata;
    client->queries = g_list_prepend(client->queries, query);

    return query;
}

struct CIClientQuery *ci_client_query_get(CIClient *client, guint32 guid)
{
    g_return_val_if_fail(client != NULL, NULL);

    GList *tmp = client->queries;
    while (tmp) {
        if (((struct CIClientQuery*)tmp->data)->guid == guid)
            return ((struct CIClientQuery*)tmp->data);
        tmp = g_list_next(tmp);
    }
    return NULL;
}

void ci_client_query_remove(CIClient *client, struct CIClientQuery *query)
{
    client->queries = g_list_remove(client->queries, query);
    g_free(query);
}

/* return TRUE to stop further propagation */
gboolean ci_client_handle_query_msg(CIClient *client, CINetMsg *msg)
{
    g_return_val_if_fail(client != NULL, TRUE);
    g_return_val_if_fail(msg != NULL, TRUE);

    if (msg->guid == 0)
        return FALSE;

    struct CIClientQuery *query = ci_client_query_get(client, msg->guid);
    if (query == NULL)
        return FALSE;

    if (query->callback)
        query->callback(msg, query->userdata);

    ci_client_query_remove(client, query);

    return TRUE;
}

void ci_client_send_message(CIClient *client, gchar *data, gsize len)
{
    g_return_if_fail(client != NULL);

    GSocket *socket = g_socket_connection_get_socket(client->connection);
    if (!socket)
        return;
    if (!g_socket_is_connected(socket))
        return;
    gssize bytes = 0;
    gssize rc;
    while (bytes < len) {
        rc = g_socket_send(socket, &data[bytes], len-bytes, NULL, NULL);
        if (rc < 0) {
            return;
        }
        bytes += rc;
    }
}

CINetMsg *ci_client_recv_message(GSocket *socket, gboolean *conn_err)
{
    CINetMsg *msg = NULL;
    gssize bytes;
    gssize rc;
    gchar *msgdata;
    CINetMsgHeader header;
    gchar buf[32];
    if (conn_err) *conn_err = FALSE;

    if (socket == NULL)
        return NULL;
    
    bytes = g_socket_receive(socket, buf, CINET_HEADER_LENGTH, NULL, NULL);
    if (bytes <= 0)
        goto connection_error;

    if (cinet_msg_read_header(&header, buf, bytes) < CINET_HEADER_LENGTH)
        return NULL;

    msgdata = g_malloc(CINET_HEADER_LENGTH + header.msglen);
    memcpy(msgdata, buf, bytes);
    bytes = 0;
    while (bytes < header.msglen) {
        rc = g_socket_receive(socket, &msgdata[CINET_HEADER_LENGTH + bytes],
                header.msglen - bytes, NULL, NULL);
        if (rc <= 0)
            goto connection_error;
        bytes += rc;
    }

    if (cinet_msg_read_msg(&msg, msgdata, CINET_HEADER_LENGTH + header.msglen) != 0) {
        g_free(msgdata);
        return NULL;
    }
    g_free(msgdata);

    return msg;

connection_error:
    if (conn_err)
        *conn_err = TRUE;
    return NULL;
}

void ci_client_handle_message(CIClient *client, CINetMsg *msg)
{
    g_return_if_fail(client != NULL);
    g_return_if_fail(msg != NULL);

    if (msg->msgtype == CI_NET_MSG_LEAVE) {
        DLOG("received leave reply\n");
        client->state = CIClientStateInitialized;
    }
    else if (msg->msgtype == CI_NET_MSG_SHUTDOWN) {
        DLOG("server shutdown\n");
    }

    if (!ci_client_handle_query_msg(client, msg) && client->callback)
        client->callback(msg);
}

gboolean ci_client_incoming_data(GSocket *socket, GIOCondition cond, CIClient *client)
{
    /* TODO: take this from more modular ciservice3 code */
    CINetMsg *msg = NULL;
    gboolean conn_err = FALSE;

    if (cond == G_IO_IN) {
        msg = ci_client_recv_message(socket, &conn_err);
        if (msg == NULL) {
            if (conn_err) {
                ci_client_handle_connection_lost(client);
                return FALSE;
            }
            return TRUE;
        }

        ci_client_handle_message(client, msg);

        cinet_msg_free(msg);
        return TRUE;
    }
    else if ((cond & G_IO_ERR) || (cond & G_IO_HUP)) {
        DLOG("err || hup\n");
        ci_client_handle_connection_lost(client);
        return FALSE;
    }

    return TRUE;
}

void ci_client_connected_func(GSocketClient *source, GAsyncResult *result, CIClient *client)
{
    g_return_if_fail(client != NULL);

    if (client->cancel && g_cancellable_is_cancelled(client->cancel)) {
        DLOG("client_connected_func: was cancelled\n");
        g_object_unref(client->cancel);
        client->cancel = NULL;
        return;
    }
    GError *err = NULL;
    client->connection = g_socket_client_connect_to_host_finish(source, result, &err);
    if (client->connection == NULL) {
        LOG("no connection: %s\n", err->message);
        g_error_free(err);
        client->state = CIClientStateInitialized;
        ci_client_handle_connection_lost(client);
        return;
    }
    DLOG("connection established\n");

    ci_client_stop_timer(client);
    g_tcp_connection_set_graceful_disconnect(G_TCP_CONNECTION(client->connection), TRUE);

    GSource *sock_source = NULL;
    GSocket *client_sock = g_socket_connection_get_socket(client->connection);

    sock_source = g_socket_create_source(G_SOCKET(client_sock),
            G_IO_IN | G_IO_HUP | G_IO_ERR, NULL);
    if (sock_source) {
        g_source_set_callback(sock_source, (GSourceFunc)ci_client_incoming_data,
                client, NULL);
        g_source_attach(sock_source, NULL);
        client->stream_source_id = g_source_get_id(sock_source);
    }

    client->state = CIClientStateConnected;

    gchar *buffer = NULL;
    gsize len = 0;
    if (cinet_message_new_for_data(&buffer, &len, CI_NET_MSG_VERSION, 
            "major", 3, "minor", 0, "patch", 0, NULL, NULL) == 0) {
        ci_client_send_message(client, buffer, len);
        g_free(buffer);
    }

    g_object_unref(client->cancel);
    client->cancel = NULL;

    if (client->state_changed_cb)
        client->state_changed_cb(CIClientStateConnected);
}

CIClient *ci_client_new(void)
{
    CIClient *client = g_malloc0(sizeof(CIClient));

    client->client = g_socket_client_new();
    client->retry_interval = 10;
    client->state = CIClientStateUnknown;

    return client;
}

CIClient *ci_client_new_with_remote(gchar *host, guint port)
{
    CIClient *client = ci_client_new();
    ci_client_set_remote(client, host, port);

    client->state = CIClientStateInitialized;

    return client;
}

CIClient *ci_client_new_full(gchar *host, guint port, CIMsgCallback msgcb, CIClientStateChangedFunc statecb)
{
    CIClient *client = ci_client_new_with_remote(host, port);
    client->callback = msgcb;
    client->state_changed_cb = statecb;

    return client;
}

void ci_client_set_retry_interval(CIClient *client, gint seconds)
{
    g_return_if_fail(client != NULL);

    client->retry_interval = seconds;
}

void ci_client_set_remote(CIClient *client, gchar *host, guint port)
{
    g_return_if_fail(client != NULL);

    if (g_strcmp0(client->host, host) != 0 ||
            client->port != port) {
        g_free(client->host);
        client->host = g_strdup(host);
        client->port = port;
        client->config_changed = 1;
    }
}

void ci_client_set_message_callback(CIClient *client, CIMsgCallback callback)
{
    g_return_if_fail(client != NULL);

    client->callback = callback;
}

/*void ci_client_start(CIClient *client, CIMsgCallback callback)
{
    g_return_if_fail(client != NULL);
    DLOG("client_start\n");

    client->client = g_socket_client_new();
    client->host = ci_config_get_string("client:host");
    client->port = ci_config_get_uint("client:port");
    client->callback = callback;
    client->connection = NULL;

    client->state = CIClientStateInitialized;

    ci_client_connect(client);
}
*/
void ci_client_set_state_changed_callback(CIClient *client, CIClientStateChangedFunc func)
{
    g_return_if_fail(client != NULL);

    client->state_changed_cb = func;
}

void ci_client_connect(CIClient *client)
{
    g_return_if_fail(client != NULL);
    DLOG("client_connect\n");

    client->auto_reconnect = 1;

    if (client->state == CIClientStateConnecting || 
        client->state == CIClientStateConnected) {
        ci_client_stop(client);
    }
    client->cancel = g_cancellable_new();

    g_socket_client_connect_to_host_async(client->client,
            client->host, client->port, client->cancel,
            (GAsyncReadyCallback)ci_client_connected_func, client);
    client->state = CIClientStateConnecting;
}

void ci_client_stop(CIClient *client)
{
    g_return_if_fail(client != NULL);
    DLOG("client_stop\n");
    gchar *buffer = NULL;
    gsize len;

    if (client->state == CIClientStateConnecting) {
        DLOG("cancel connecting operation\n");
        if (client->cancel) {
            g_cancellable_cancel(client->cancel);
            g_object_unref(client->cancel);
            client->cancel = NULL;
        }
    }
    else if (client->state == CIClientStateConnected) {
        DLOG("disconnect\n");
        if (cinet_message_new_for_data(&buffer, &len, CI_NET_MSG_LEAVE, NULL, NULL) == 0) {
            ci_client_send_message(client, buffer, len);
            g_free(buffer);
        }

        DLOG("shutdown socket\n");
        if (client->stream_source_id) {
            g_source_remove(client->stream_source_id);
            client->stream_source_id = 0;
        }
        g_io_stream_close(G_IO_STREAM(client->connection), NULL, NULL);
        g_object_unref(client->connection);
        client->connection = NULL;
    }

    DLOG("end client_stop\n");
    client->state = CIClientStateInitialized;

    if (client->state_changed_cb)
        client->state_changed_cb(CIClientStateDisconnected);
}

void ci_client_disconnect(CIClient *client)
{
    g_return_if_fail(client != NULL);
    /* This function only gets called by the user, so don’t reconnect again. */
    client->auto_reconnect = 0;

    ci_client_stop_timer(client);
    ci_client_stop(client);
}

void ci_client_restart(CIClient *client, gboolean force)
{
    g_return_if_fail(client != NULL);

    if (client->state == CIClientStateUnknown && !client->config_changed) {
        /* action was to start the client, i.e. initalize and connect, but we have no chance of
         * initializing it from inside anymore */
        return;
    }

    if (client->config_changed ||
            force == TRUE) {
        client->config_changed = 0;
        ci_client_connect(client);
    }
}

gboolean ci_client_try_connect_func(CIClient *client)
{
    g_return_val_if_fail(client != NULL, FALSE);

    DLOG("client_try_connect\n");
    switch (client->state) {
        case CIClientStateConnecting:
            /* still trying last one, do nothing */
            DLOG("client_try_connect: connecting\n");
            return TRUE;
        case CIClientStateConnected:
            /* connected, remove timer */
            DLOG("client_try_connect: connected\n");
            client->timer_source_id = 0;
            return FALSE;
        case CIClientStateDisconnected:
        case CIClientStateInitialized:
            DLOG("client_try_connect: start client\n");
            ci_client_connect(client);
            return TRUE;
        default:
            DLOG("client_try_connect: unhandled state\n");
            return TRUE;
    }
}

void ci_client_handle_connection_lost(CIClient *client)
{
    g_return_if_fail(client != NULL);

    if (client->timer_source_id || client->auto_reconnect == 0)
        return;

    ci_client_stop(client);

    DLOG("retry interval: %d\n", retry_interval);
    if (client->retry_interval > 0) {
        client->timer_source_id = g_timeout_add_seconds(client->retry_interval,
                (GSourceFunc)ci_client_try_connect_func, client);
    }
}

void ci_client_stop_timer(CIClient *client)
{
    g_return_if_fail(client != NULL);

    if (client->timer_source_id) {
        g_source_remove(client->timer_source_id);
        client->timer_source_id = 0;
    }
}

void ci_client_shutdown(CIClient *client)
{
    g_return_if_fail(client != NULL);

    DLOG("client_shutdown\n");
    if (client->state == CIClientStateInitialized) {
        ci_client_stop_timer(client);
        g_free(client->host);
        client->state = CIClientStateUnknown;
        g_list_free_full(client->queries, g_free);
    }
}

CIClientState ci_client_get_state(CIClient *client)
{
    g_return_val_if_fail(client != NULL, CIClientStateUnknown);

    return client->state;
}

CINetMsg *ci_client_query_make_message(CINetMsgType type, guint32 guid, va_list ap)
{
    CINetMsg *msg = cinet_message_new(type, "guid", guid, NULL, NULL);
    gchar *key;
    gpointer val;

    do {
        key = va_arg(ap, gchar*);
        val = va_arg(ap, gpointer);
        if (key)
            cinet_message_set_value(msg, key, val);
    } while (key);

    return msg;
}

gboolean ci_client_query(CIClient *client, CIClientQueryType type, CIQueryMsgCallback callback, gpointer userdata, ...)
{
    g_return_val_if_fail(client != NULL, FALSE);

    va_list ap;
    gchar *msgdata = NULL;
    gsize msglen = 0;
    CINetMsg *msg = NULL;

    CINetMsgType msgtype = CI_NET_MSG_INVALID;

    if (client->state != CIClientStateConnected)
        return FALSE;

    struct CIClientQuery *query = ci_client_query_new(client, callback, userdata);

    va_start(ap, userdata);
    switch (type) {
        case CIClientQueryNumCalls: msgtype = CI_NET_MSG_DB_NUM_CALLS; break;
        case CIClientQueryCallList: msgtype = CI_NET_MSG_DB_CALL_LIST; break;
        case CIClientQueryGetCaller: msgtype = CI_NET_MSG_DB_GET_CALLER; break;
        case CIClientQueryGetCallerList: msgtype = CI_NET_MSG_DB_GET_CALLER_LIST; break;
        case CIClientQueryAddCaller: msgtype = CI_NET_MSG_DB_ADD_CALLER; break;
        case CIClientQueryDelCaller: msgtype = CI_NET_MSG_DB_DEL_CALLER; break;
        default:
            break;
    }
    msg = ci_client_query_make_message(msgtype, query->guid, ap);
    va_end(ap);

    if (msg) {
        cinet_msg_write_msg(&msgdata, &msglen, msg);

        ci_client_send_message(client, msgdata, msglen);

        g_free(msgdata);
        cinet_msg_free(msg);
    }

    return TRUE;
}
