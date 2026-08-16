/*
 * Copyright (C) 2019-2022 Jolla Ltd.
 * Copyright (C) 2026 TheKit <thekit@disroot.org>.
 *
 * You may use this file under the terms of BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * AIDL flavour of the QTI audio manager passthrough. Unlike the hidl one the
 * requests are one-way and carry a serial which the response has to repeat.
 */

#include <gbinder.h>
#include <glib-unix.h>

#include "common.h"
#include "impl.h"
#include "logging.h"
#include "dbus-comms.h"

#define BINDER_DEVICE                       GBINDER_DEFAULT_BINDER

#define QCRIL_IFACE_AIDL(x)                 "vendor.qti.hardware.radio.am." x
#define QCRIL_AUDIO_AIDL                    QCRIL_IFACE_AIDL("IQcRilAudio")
#define QCRIL_AUDIO_REQUEST_AIDL            QCRIL_IFACE_AIDL("IQcRilAudioRequest")
#define QCRIL_AUDIO_RESPONSE_AIDL           QCRIL_IFACE_AIDL("IQcRilAudioResponse")

/* Version and hash of the frozen vendor.qti.hardware.radio.am V1 interface */
#define QCRIL_AUDIO_AIDL_VERSION            (1)
#define QCRIL_AUDIO_AIDL_HASH               "7807b67b5d557d9479da5c0f34c525bf48aaec90"

/* Instances to watch for when they are not registered yet */
static const char *qcril_slots[] = { "slot1", "slot2", NULL };

enum qcril_audio_methods {
    QCRIL_AUDIO_SET_REQUEST_INTERFACE = GBINDER_FIRST_CALL_TRANSACTION,
    QCRIL_AUDIO_SET_ERROR
};

enum qcril_audio_request_methods {
    QCRIL_AUDIO_REQUEST_QUERY_PARAMETERS = GBINDER_FIRST_CALL_TRANSACTION,
    QCRIL_AUDIO_REQUEST_SET_PARAMETERS
};

enum qcril_audio_response_methods {
    QCRIL_AUDIO_RESPONSE_QUERY_PARAMETERS = GBINDER_FIRST_CALL_TRANSACTION,
    QCRIL_AUDIO_RESPONSE_SET_PARAMETERS
};

/* Meta transactions every stable AIDL interface has to implement */
#define AIDL_LAST_CALL_TRANSACTION          (0x00ffffff)
#define AIDL_GET_INTERFACE_VERSION          (AIDL_LAST_CALL_TRANSACTION - 0)
#define AIDL_GET_INTERFACE_HASH             (AIDL_LAST_CALL_TRANSACTION - 1)

/* Status header of a successful AIDL reply */
#define AIDL_EX_NONE                        (0)

typedef struct aidl_app AidlApp;

typedef struct am_client {
    AidlApp *app;
    gchar* fqname;
    gchar* slot;
    GBinderServiceManager* sm;
    /* IQcRilAudio implemented by the modem */
    GBinderRemoteObject* remote;
    GBinderClient* client;
    /* IQcRilAudioRequest implemented by us */
    GBinderLocalObject* request;
    /* IQcRilAudioResponse implemented by the modem */
    GBinderRemoteObject* response;
    GBinderClient* response_client;
    gulong wait_id;
    gulong death_id;
    /* Service was registered when we looked */
    gboolean registered;
} AmClient;

struct aidl_app {
    GMainLoop* loop;
    const AppConfig *config;
    GBinderServiceManager* sm;
    GSList* clients;
    DBusComms *dbus;
};

static const GBinderClientIfaceInfo qcril_audio_iface_info[] = {
    { QCRIL_AUDIO_AIDL, UINT_MAX }
};

static const GBinderClientIfaceInfo qcril_audio_response_iface_info[] = {
    { QCRIL_AUDIO_RESPONSE_AIDL, UINT_MAX }
};

static AidlApp _app;

static void
am_client_registration_handler(
        GBinderServiceManager* sm,
        const char* name,
        void* user_data);

static gboolean
am_client_connect(
        AmClient* am);

static void
am_client_disconnect(
        AmClient* am)
{
    if (am->response_client) {
        gbinder_client_unref(am->response_client);
        am->response_client = NULL;
    }
    if (am->response) {
        gbinder_remote_object_unref(am->response);
        am->response = NULL;
    }
    if (am->request) {
        gbinder_local_object_drop(am->request);
        am->request = NULL;
    }
    if (am->client) {
        gbinder_client_unref(am->client);
        am->client = NULL;
    }
    if (am->remote) {
        gbinder_remote_object_remove_handler(am->remote, am->death_id);
        am->death_id = 0;
        gbinder_remote_object_unref(am->remote);
        am->remote = NULL;
    }
}

static void
am_remote_died(
        GBinderRemoteObject* obj,
        void* user_data)
{
    AmClient* am = user_data;

    DBG("%s has died", am->fqname);
    am_client_disconnect(am);

    /* Wait for it to re-appear */
    am->wait_id = gbinder_servicemanager_add_registration_handler(am->sm,
        am->fqname, am_client_registration_handler, am);
}

/* oneway IQcRilAudioResponse::queryParametersResponse(int32_t serial, String result) */
static void
am_client_query_parameters_response(
        AmClient* am,
        gint32 serial,
        const char* result)
{
    GBinderLocalRequest* req;
    GBinderWriter writer;
    int status;

    if (!am->response_client) {
        ERR("No response interface for %s, dropping serial %d", am->slot, serial);
        return;
    }

    req = gbinder_client_new_request2(am->response_client,
        QCRIL_AUDIO_RESPONSE_QUERY_PARAMETERS);
    gbinder_local_request_init_writer(req, &writer);
    gbinder_writer_append_int32(&writer, serial);
    gbinder_writer_append_string16(&writer, result ? result : "");
    status = gbinder_client_transact_sync_oneway(am->response_client,
        QCRIL_AUDIO_RESPONSE_QUERY_PARAMETERS, req);
    gbinder_local_request_unref(req);
    DBG("IQcRilAudioResponse::queryParametersResponse %s %d %s status %d",
        am->slot, serial, result ? result : "", status);
}

/* oneway IQcRilAudioResponse::setParametersResponse(int32_t serial, AudioError error) */
static void
am_client_set_parameters_response(
        AmClient* am,
        gint32 serial,
        gint32 error)
{
    GBinderLocalRequest* req;
    GBinderWriter writer;
    int status;

    if (!am->response_client) {
        ERR("No response interface for %s, dropping serial %d", am->slot, serial);
        return;
    }

    req = gbinder_client_new_request2(am->response_client,
        QCRIL_AUDIO_RESPONSE_SET_PARAMETERS);
    gbinder_local_request_init_writer(req, &writer);
    gbinder_writer_append_int32(&writer, serial);
    gbinder_writer_append_int32(&writer, error);
    status = gbinder_client_transact_sync_oneway(am->response_client,
        QCRIL_AUDIO_RESPONSE_SET_PARAMETERS, req);
    gbinder_local_request_unref(req);
    DBG("IQcRilAudioResponse::setParametersResponse %s %d %d status %d",
        am->slot, serial, error, status);
}

/* oneway IQcRilAudioRequest::queryParameters(int32_t serial, String query) */
static void
am_client_request_query_parameters(
        AmClient* am,
        gint32 serial,
        const char* str)
{
    gchar* result = NULL;

    if (am->app->config->dummy_mode) {
        am_client_query_parameters_response(am, serial, "");
        return;
    }

    if (str)
        dbus_comms_get_parameters(am->app->dbus, str, &result);

    am_client_query_parameters_response(am, serial, result ? result : "");
    g_free(result);
}

/* oneway IQcRilAudioRequest::setParameters(int32_t serial, String kv_pairs) */
static void
am_client_request_set_parameters(
        AmClient* am,
        gint32 serial,
        const char* str)
{
    gint32 result = 0;

    if (!am->app->config->dummy_mode && str)
        result = dbus_comms_set_parameters(am->app->dbus, str);

    am_client_set_parameters_response(am, serial, result);
}

/* Reply of a void method, only the status header */
static GBinderLocalReply*
am_client_reply_ok(
        GBinderLocalObject* obj)
{
    GBinderLocalReply* reply = gbinder_local_object_new_reply(obj);
    GBinderWriter writer;

    gbinder_local_reply_init_writer(reply, &writer);
    gbinder_writer_append_int32(&writer, AIDL_EX_NONE);

    return reply;
}

static GBinderLocalReply*
am_client_meta_reply_int32(
        GBinderLocalObject* obj,
        gint32 value)
{
    GBinderLocalReply* reply = gbinder_local_object_new_reply(obj);
    GBinderWriter writer;

    gbinder_local_reply_init_writer(reply, &writer);
    gbinder_writer_append_int32(&writer, AIDL_EX_NONE);
    gbinder_writer_append_int32(&writer, value);

    return reply;
}

static GBinderLocalReply*
am_client_meta_reply_string(
        GBinderLocalObject* obj,
        const char* value)
{
    GBinderLocalReply* reply = gbinder_local_object_new_reply(obj);
    GBinderWriter writer;

    gbinder_local_reply_init_writer(reply, &writer);
    gbinder_writer_append_int32(&writer, AIDL_EX_NONE);
    gbinder_writer_append_string16(&writer, value);

    return reply;
}

static GBinderLocalReply*
am_client_request(
        GBinderLocalObject* obj,
        GBinderRemoteRequest* req,
        guint code,
        guint flags,
        int* status,
        void* user_data)
{
    AmClient* am = user_data;
    const char* iface = gbinder_remote_request_interface(req);

    if (!g_strcmp0(iface, QCRIL_AUDIO_REQUEST_AIDL)) {
        GBinderReader reader;
        gint32 serial = 0;
        char* str;

        gbinder_remote_request_init_reader(req, &reader);

        switch (code) {
        case QCRIL_AUDIO_REQUEST_QUERY_PARAMETERS:
            gbinder_reader_read_int32(&reader, &serial);
            str = gbinder_reader_read_string16(&reader);
            DBG("IQcRilAudioRequest::queryParameters %s %d %s",
                am->slot, serial, str ? str : "");
            am_client_request_query_parameters(am, serial, str);
            g_free(str);
            *status = GBINDER_STATUS_OK;
            return (flags & GBINDER_TX_FLAG_ONEWAY) ? NULL :
                am_client_reply_ok(obj);

        case QCRIL_AUDIO_REQUEST_SET_PARAMETERS:
            gbinder_reader_read_int32(&reader, &serial);
            str = gbinder_reader_read_string16(&reader);
            DBG("IQcRilAudioRequest::setParameters %s %d %s",
                am->slot, serial, str ? str : "");
            am_client_request_set_parameters(am, serial, str);
            g_free(str);
            *status = GBINDER_STATUS_OK;
            return (flags & GBINDER_TX_FLAG_ONEWAY) ? NULL :
                am_client_reply_ok(obj);

        case AIDL_GET_INTERFACE_VERSION:
            DBG("IQcRilAudioRequest::getInterfaceVersion %s", am->slot);
            *status = GBINDER_STATUS_OK;
            return am_client_meta_reply_int32(obj, QCRIL_AUDIO_AIDL_VERSION);

        case AIDL_GET_INTERFACE_HASH:
            DBG("IQcRilAudioRequest::getInterfaceHash %s", am->slot);
            *status = GBINDER_STATUS_OK;
            return am_client_meta_reply_string(obj, QCRIL_AUDIO_AIDL_HASH);
        }
    }

    ERR("Unexpected request %s %u", iface, code);
    *status = GBINDER_STATUS_FAILED;
    return NULL;
}

/* IQcRilAudioResponse IQcRilAudio::setRequestInterface(IQcRilAudioRequest req) */
static gboolean
am_client_set_request_interface(
        AmClient* am)
{
    GBinderLocalRequest* req;
    GBinderRemoteReply* reply;
    GBinderWriter writer;
    int status = 0;
    gboolean ok = FALSE;

    am->request = gbinder_servicemanager_new_local_object(am->sm,
        QCRIL_AUDIO_REQUEST_AIDL, am_client_request, am);
    gbinder_local_object_set_stability(am->request, GBINDER_STABILITY_VINTF);

    req = gbinder_client_new_request2(am->client,
        QCRIL_AUDIO_SET_REQUEST_INTERFACE);
    gbinder_local_request_init_writer(req, &writer);
    gbinder_writer_append_local_object(&writer, am->request);
    reply = gbinder_client_transact_sync_reply(am->client,
        QCRIL_AUDIO_SET_REQUEST_INTERFACE, req, &status);
    gbinder_local_request_unref(req);

    if (reply) {
        GBinderReader reader;
        gint32 exception = -1;

        gbinder_remote_reply_init_reader(reply, &reader);
        if (gbinder_reader_read_int32(&reader, &exception) &&
            exception == AIDL_EX_NONE) {
            am->response = gbinder_reader_read_object(&reader);
            if (am->response) {
                am->response_client = gbinder_client_new2(am->response,
                    qcril_audio_response_iface_info,
                    G_N_ELEMENTS(qcril_audio_response_iface_info));
                ok = TRUE;
            } else {
                ERR("%s returned no response interface", am->fqname);
            }
        } else {
            ERR("setRequestInterface %s failed with exception %d",
                am->slot, exception);
        }
        gbinder_remote_reply_unref(reply);
    } else {
        ERR("setRequestInterface %s got no reply, status %d", am->slot, status);
    }

    if (!ok) {
        gbinder_local_object_drop(am->request);
        am->request = NULL;
    }

    return ok;
}

static gboolean
am_client_connect(
        AmClient* am)
{
    int status = 0;

    am->remote = gbinder_servicemanager_get_service_sync(am->sm,
        am->fqname, &status); /* auto-released reference */

    if (!am->remote) {
        DBG("Couldn't connect to %s", am->fqname);
        return FALSE;
    }

    DBG("Connected to %s", am->fqname);
    gbinder_remote_object_ref(am->remote);
    am->client = gbinder_client_new2(am->remote, qcril_audio_iface_info,
        G_N_ELEMENTS(qcril_audio_iface_info));
    am->death_id = gbinder_remote_object_add_death_handler(am->remote,
        am_remote_died, am);

    if (!am_client_set_request_interface(am)) {
        am_client_disconnect(am);
        return FALSE;
    }

    DBG("setRequestInterface %s done", am->slot);

    return TRUE;
}

static void
am_client_registration_handler(
        GBinderServiceManager* sm,
        const char* name,
        void* user_data)
{
    AmClient* am = user_data;

    if (!strcmp(name, am->fqname) && am_client_connect(am)) {
        DBG("%s has reanimated", am->fqname);
        gbinder_servicemanager_remove_handler(am->sm, am->wait_id);
        am->wait_id = 0;
    } else {
        DBG("%s appeared", name);
    }
}

static AmClient*
am_client_new(
        AidlApp *app,
        const char* slot,
        gboolean registered)
{
    AmClient* am = g_new0(AmClient, 1);

    am->app = app;
    am->registered = registered;
    am->slot = g_strdup(slot);
    am->fqname = g_strconcat(QCRIL_AUDIO_AIDL, "/", slot, NULL);
    am->sm = gbinder_servicemanager_ref(app->sm);
    return am;
}

static void
am_client_connect_all(
        GSList *clients)
{
    GSList *i;

    for (i = clients; i; i = i->next) {
        AmClient *am = i->data;

        if (!am->registered || !am_client_connect(am)) {
            DBG("Waiting for %s", am->fqname);
            am->wait_id = gbinder_servicemanager_add_registration_handler(am->sm,
                am->fqname, am_client_registration_handler, am);
        }
    }
}

static void
am_client_free(
        gpointer data)
{
    AmClient* am = data;

    am_client_disconnect(am);
    gbinder_servicemanager_remove_handler(am->sm, am->wait_id);
    gbinder_servicemanager_unref(am->sm);
    g_free(am->fqname);
    g_free(am->slot);
    g_free(am);
}

static gboolean
app_has_slot(
        AidlApp *app,
        const gchar *slot)
{
    GSList *i;

    for (i = app->clients; i; i = i->next) {
        AmClient *am = i->data;

        if (!g_strcmp0(slot, am->slot))
            return TRUE;
    }

    return FALSE;
}

static void
app_add_slot(
        AidlApp *app,
        const gchar *slot,
        gboolean registered)
{
    if (slot && slot[0] && !app_has_slot(app, slot)) {
        DBG("%s slot %s", registered ? "Found" : "Watching for", slot);
        app->clients = g_slist_append(app->clients,
            am_client_new(app, slot, registered));
    }
}

/* Slots of the services which are already registered */
static void
app_parse_registered_slots(
        AidlApp *app)
{
    char** services = gbinder_servicemanager_list_sync(app->sm);
    char** service;

    for (service = services; service && *service; service++) {
        if (g_str_has_prefix(*service, QCRIL_AUDIO_AIDL "/"))
            app_add_slot(app, *service + strlen(QCRIL_AUDIO_AIDL "/"), TRUE);
    }

    g_strfreev(services);
}

static void
app_find_slots(
        AidlApp *app)
{
    const char **slot;

    app_parse_registered_slots(app);

    for (slot = qcril_slots; *slot; slot++)
        app_add_slot(app, *slot, FALSE);
}

static void
dbus_connected_cb(
        DBusComms *c,
        gboolean connected,
        void *userdata)
{
    AidlApp *app = userdata;

    if (connected) {
        DBG("DBus up, connect clients");
        am_client_connect_all(app->clients);
    }
}

gboolean
app_qti_aidl_init(
        GMainLoop *mainloop,
        const AppConfig *config)
{
    memset(&_app, 0, sizeof(_app));
    _app.loop = mainloop;
    _app.config = config;

    _app.sm = gbinder_servicemanager_new(config->device ? config->device : BINDER_DEVICE);
    if (!_app.sm) {
        ERR("Couldn't open service manager");
        return FALSE;
    }

    app_find_slots(&_app);

    if (_app.config->dummy_mode) {
        ERR("Notice! AIDL helper doesn't have proper dummy mode.");
        am_client_connect_all(_app.clients);
    } else {
        _app.dbus = dbus_comms_new(_app.config->address);
        dbus_comms_init_delayed(_app.dbus, dbus_connected_cb, &_app);
    }

    return TRUE;
}

gboolean
app_qti_aidl_wait(
        void)
{
    return gbinder_servicemanager_wait(_app.sm, -1);
}

gint
app_qti_aidl_done(
        void)
{
    if (_app.dbus)
        dbus_comms_done(_app.dbus);
    g_slist_free_full(_app.clients, am_client_free);
    if (_app.sm)
        gbinder_servicemanager_unref(_app.sm);

    return 0;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
