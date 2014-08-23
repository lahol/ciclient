#ifndef __CLIENT_H__
#define __CLIENT_H__

#include <glib.h>
#include <cinet.h>

typedef enum {
    CIClientStateUnknown = 0,
    CIClientStateInitialized,
    CIClientStateConnecting,
    CIClientStateConnected,
    CIClientStateDisconnected
} CIClientState;

typedef enum {
    CIClientQueryNumCalls,
    CIClientQueryCallList,
    CIClientQueryGetCaller,
    CIClientQueryAddCaller,
    CIClientQueryDelCaller,
    CIClientQueryGetCallerList
} CIClientQueryType;

typedef struct _CIClient CIClient;

typedef void (*CIMsgCallback)(CINetMsg *);
typedef void (*CIClientStateChangedFunc)(CIClientState);

CIClient *ci_client_new(void);
CIClient *ci_client_new_with_remote(gchar *host, guint port);
CIClient *ci_client_new_full(gchar *host, guint port, CIMsgCallback msgcb, CIClientStateChangedFunc statecb);
void ci_client_set_retry_interval(CIClient *client, gint seconds);
void ci_client_set_remote(CIClient *client, gchar *host, guint port);
void ci_client_set_message_callback(CIClient *client, CIMsgCallback callback);
/*void ci_client_start(CIClient *client, CIMsgCallback callback);*/
void ci_client_set_state_changed_callback(CIClient *client, CIClientStateChangedFunc func);
void ci_client_connect(CIClient *client);
void ci_client_disconnect(CIClient *client);
void ci_client_restart(CIClient *client, gboolean force);
void ci_client_stop(CIClient *client);
void ci_client_shutdown(CIClient *client);

CIClientState ci_client_get_state(CIClient *client);

typedef void (*CIQueryMsgCallback)(CINetMsg *, gpointer);
gboolean ci_client_query(CIClient *client, CIClientQueryType type, CIQueryMsgCallback callback, gpointer userdata, ...);

#endif
