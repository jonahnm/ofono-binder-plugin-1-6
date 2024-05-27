/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2021-2022 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include "binder_log.h"
#include "binder_modem.h"
#include "binder_stk.h"
#include "binder_util.h"

#include <ofono/log.h>
#include <ofono/stk.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

enum binder_stk_events {
    STK_EVENT_PROACTIVE_COMMAND,
    STK_EVENT_SESSION_END,
    STK_EVENT_NOTIFY,
    STK_EVENT_COUNT
};

typedef struct binder_stk {
    struct ofono_stk* stk;
    char* log_prefix;
    RadioRequestGroup* g;
    gulong event_id[STK_EVENT_COUNT];
    guint register_id;
} BinderStk;

typedef struct binder_stk_cbd {
    BinderStk* self;
    union _ofono_stk_cb {
        ofono_stk_envelope_cb_t envelope;
        ofono_stk_generic_cb_t generic;
        BinderCallback ptr;
    } cb;
    gpointer data;
} BinderStkCbData;

#define DBG_(cd,fmt,args...) ofono_warn("%s" fmt, (cd)->log_prefix, ##args)

static inline BinderStk* binder_stk_get_data(struct ofono_stk* stk)
    { return ofono_stk_get_data(stk); }

static
BinderStkCbData*
binder_stk_cbd_new(
    BinderStk* self,
    BinderCallback cb,
    void* data)
{
    BinderStkCbData* cbd = g_slice_new0(BinderStkCbData);

    cbd->self = self;
    cbd->cb.ptr = cb;
    cbd->data = data;
    return cbd;
}

static
void
binder_stk_cbd_free(
    gpointer cbd)
{
    g_slice_free(BinderStkCbData, cbd);
}

static void binder_stk_envelope_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderStkCbData* cbd = user_data;
    ofono_stk_envelope_cb_t cb = cbd->cb.envelope;
    struct ofono_error err;

    DBG_(cbd->self, "");
    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_SEND_ENVELOPE) {
            if (error == RADIO_ERROR_NONE) {
                cb(binder_error_ok(&err), NULL, 0, cbd->data);
                return;
            } else {
                ofono_warn("Error sending envelope: %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected sendEnvelope response %d", resp);
        }
    }
    cb(binder_error_failure(&err), NULL, 0, cbd->data);
}

static
void
binder_stk_envelope(
    struct ofono_stk* stk,
    int length,
    const unsigned char* cmd,
    ofono_stk_envelope_cb_t cb,
    void* data)
{
    BinderStk* self = binder_stk_get_data(stk);
    char* hex = binder_encode_hex(cmd, length);
    GBinderWriter writer;

    /* sendEnvelope(int32 serial, string command); */
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_SEND_ENVELOPE, &writer,
        binder_stk_envelope_cb, binder_stk_cbd_free,
        binder_stk_cbd_new(self, BINDER_CB(cb), data));

    ofono_warn("envelope %s", hex);
    gbinder_writer_add_cleanup(&writer, g_free, hex);
    gbinder_writer_append_hidl_string(&writer, hex);
    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_stk_terminal_response_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderStkCbData* cbd = user_data;
    ofono_stk_generic_cb_t cb = cbd->cb.generic;
    struct ofono_error err;

    DBG_(cbd->self, "");
    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_SEND_TERMINAL_RESPONSE_TO_SIM) {
            if (error == RADIO_ERROR_NONE) {
                cb(binder_error_ok(&err), cbd->data);
                return;
            } else {
                ofono_warn("Error sending terminal response: %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected sendTerminalResponseToSim response %d",
                resp);
        }
    }
    cb(binder_error_failure(&err), cbd->data);
}

static
void
binder_stk_terminal_response(
    struct ofono_stk* stk,
    int length,
    const unsigned char* resp,
    ofono_stk_generic_cb_t cb,
    void* data)
{
    BinderStk* self = binder_stk_get_data(stk);
    char* hex = binder_encode_hex(resp, length);
    GBinderWriter writer;

    /* sendTerminalResponseToSim(int32 serial, string commandResponse); */
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_SEND_TERMINAL_RESPONSE_TO_SIM, &writer,
        binder_stk_terminal_response_cb, binder_stk_cbd_free,
        binder_stk_cbd_new(self, BINDER_CB(cb), data));

    DBG_(self, "terminal response: %s", hex);
    gbinder_writer_add_cleanup(&writer, g_free, hex);
    gbinder_writer_append_hidl_string(&writer, hex);
    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_stk_user_confirmation(
    struct ofono_stk* stk,
    ofono_bool_t confirm)
{
    BinderStk* self = binder_stk_get_data(stk);
    GBinderWriter writer;

    /* handleStkCallSetupRequestFromSim(int32 serial, bool accept); */
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_HANDLE_STK_CALL_SETUP_REQUEST_FROM_SIM, &writer,
        NULL, NULL, NULL);

    DBG_(self, "%d", confirm);
    gbinder_writer_append_bool(&writer, confirm);
    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_stk_proactive_command(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderStk* self = user_data;
    GBinderReader reader;
    const char* pcmd;
    void* pdu;
    guint len;

    /*
     * stkProactiveCommand(RadioIndicationType, string cmd);
     *
     * cmd - SAT/USAT proactive represented as byte array starting with
     * command tag.
     *
     * Refer to ETSI TS 102.223 section 9.4 for command types.
     */
    gbinder_reader_copy(&reader, args);
    pcmd = gbinder_reader_read_hidl_string_c(&reader);
    pdu = binder_decode_hex(pcmd, -1, &len);
    if (pdu) {
        DBG_(self, "pcmd: %s", pcmd);
        ofono_stk_proactive_command_notify(self->stk, len, pdu);
        g_free(pdu);
    } else {
        ofono_warn("Failed to parse STK command %s", pcmd);
    }
}

static
void
binder_stk_event_notify(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderStk* self = user_data;
    GBinderReader reader;
    const char* pcmd;
    void* pdu;
    guint len;

    /*
     * stkEventNotify(RadioIndicationType, string cmd);
     *
     * cmd - SAT/USAT commands or responses sent by ME to SIM or commands
     * handled by ME, represented as byte array starting with first byte
     * of response data for command tag.
     *
     * Refer to ETSI TS 102.223 section 9.4 for command types.
     */
    gbinder_reader_copy(&reader, args);
    pcmd = gbinder_reader_read_hidl_string_c(&reader);
    pdu = binder_decode_hex(pcmd, -1, &len);
    if (pdu) {
        DBG_(self, "pcmd: %s", pcmd);
        ofono_stk_proactive_command_handled_notify(self->stk, len, pdu);
        g_free(pdu);
    } else {
        ofono_warn("Failed to parse STK event %s", pcmd);
    }
}

static
void
binder_stk_session_end_notify(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderStk* self = user_data;

    DBG_(self, "");
    /* stkSessionEnd(RadioIndicationType); */
    ofono_stk_proactive_session_end_notify(self->stk);
}

static
void
binder_stk_agent_ready(
    struct ofono_stk* stk)
{
    BinderStk* self = binder_stk_get_data(stk);
    RadioClient* client = self->g->client;

    DBG_(self, "");

    if (!self->event_id[STK_EVENT_PROACTIVE_COMMAND]) {
        DBG_(self, "Subscribing for notifications");
        self->event_id[STK_EVENT_PROACTIVE_COMMAND] =
            radio_client_add_indication_handler(client,
                RADIO_IND_STK_PROACTIVE_COMMAND,
                binder_stk_proactive_command, self);

        GASSERT(!self->event_id[STK_EVENT_SESSION_END]);
        self->event_id[STK_EVENT_SESSION_END] =
            radio_client_add_indication_handler(client,
                RADIO_IND_STK_SESSION_END,
                binder_stk_session_end_notify, self);

        GASSERT(!self->event_id[STK_EVENT_NOTIFY]);
        self->event_id[STK_EVENT_NOTIFY] =
            radio_client_add_indication_handler(client,
                RADIO_IND_STK_EVENT_NOTIFY,
                binder_stk_event_notify, self);

        /* reportStkServiceIsRunning(int32 serial); */
        binder_submit_request(self->g, RADIO_REQ_REPORT_STK_SERVICE_IS_RUNNING);
    }
}

static
gboolean binder_stk_register(
    gpointer user_data)
{
    BinderStk* self = user_data;

    ofono_warn("");
    GASSERT(self->register_id);
    self->register_id = 0;

    ofono_stk_register(self->stk);

    return G_SOURCE_REMOVE;
}

static
int
binder_stk_probe(
    struct ofono_stk* stk,
    unsigned int vendor,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderStk* self = g_new0(BinderStk, 1);

    self->stk = stk;
    self->g = radio_request_group_new(modem->client); /* Keeps ref to client */
    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    self->register_id = g_idle_add(binder_stk_register, self);

    DBG_(self, "");
    ofono_stk_set_data(stk, self);
    return 0;
}

static
void
binder_stk_remove(
    struct ofono_stk* stk)
{
    BinderStk* self = binder_stk_get_data(stk);

    DBG_(self, "");

    if (self->register_id) {
        g_source_remove(self->register_id);
    }

    radio_client_remove_all_handlers(self->g->client, self->event_id);
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);

    g_free(self->log_prefix);
    g_free(self);

    ofono_stk_set_data(stk, NULL);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_stk_driver binder_stk_driver = {
    .name                   = BINDER_DRIVER,
    .probe                  = binder_stk_probe,
    .remove                 = binder_stk_remove,
    .envelope               = binder_stk_envelope,
    .terminal_response      = binder_stk_terminal_response,
    .user_confirmation      = binder_stk_user_confirmation,
    .ready                  = binder_stk_agent_ready
};

void
binder_stk_init()
{
    ofono_stk_driver_register(&binder_stk_driver);
}

void
binder_stk_cleanup()
{
    ofono_stk_driver_unregister(&binder_stk_driver);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
