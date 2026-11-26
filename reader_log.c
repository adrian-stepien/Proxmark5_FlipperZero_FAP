#include <furi.h>
#include <rpc/rpc_app.h>
#include <gui/view_dispatcher.h>
#include "reader_log.h"

static RpcAppSystem* log_rpc;
static FuriMutex* log_mutex;
static unsigned log_sequence;

static void rpc_event(const RpcAppSystemEvent* event, void* context) {
    furi_mutex_acquire(log_mutex, FuriWaitForever);
    if(log_rpc) {
        const bool closing = event->type == RpcAppEventTypeSessionClose;
        const bool requested_exit = event->type == RpcAppEventTypeAppExit;
        if(closing) {
            rpc_system_app_set_callback(log_rpc, NULL, NULL);
            log_rpc = NULL;
        } else {
            rpc_system_app_confirm(log_rpc, requested_exit);
        }
        if(closing || requested_exit) view_dispatcher_stop(context);
    }
    furi_mutex_release(log_mutex);
}

void reader_log_open(const char* args, ViewDispatcher* dispatcher) {
    // The Flipper loader supplies this context on an RPC launch.
    if(!args || strncmp(args, "RPC ", 4) != 0 || strlen(args + 4) != 8 ||
       strspn(args + 4, "0123456789abcdefABCDEF") != 8)
        return;
    uintptr_t address = strtoul(args + 4, NULL, 16);
    if(!address) return;

    log_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    log_sequence = 0;
    log_rpc = (RpcAppSystem*)address;
    furi_mutex_acquire(log_mutex, FuriWaitForever);
    rpc_system_app_set_callback(log_rpc, rpc_event, dispatcher);
    rpc_system_app_send_started(log_rpc);
    furi_mutex_release(log_mutex);
}

void reader_log_close(void) {
    if(!log_mutex) return;
    furi_mutex_acquire(log_mutex, FuriWaitForever);
    if(log_rpc) {
        rpc_system_app_send_exited(log_rpc);
        rpc_system_app_set_callback(log_rpc, NULL, NULL);
        log_rpc = NULL;
    }
    furi_mutex_release(log_mutex);
    furi_mutex_free(log_mutex);
    log_mutex = NULL;
}

static void publish(const char* event, const FuriString* fields) {
    furi_mutex_acquire(log_mutex, FuriWaitForever);
    if(log_rpc) {
        FuriString* record = furi_string_alloc();
        furi_string_printf(
            record,
            "{\"version\":1,\"seq\":%u,\"event\":\"%s\"%s}\n",
            log_sequence++,
            event,
            fields ? furi_string_get_cstr(fields) : "");
        rpc_system_app_exchange_data(
            log_rpc, (const uint8_t*)furi_string_get_cstr(record), furi_string_size(record));
        furi_string_free(record);
    }
    furi_mutex_release(log_mutex);
}

void reader_log_event(const char* event) {
    if(log_mutex) publish(event, NULL);
}

static void append_hex(FuriString* fields, const char* key, const uint8_t* data, size_t size) {
    furi_string_cat_printf(fields, ",\"%s\":\"", key);
    for(size_t i = 0; i < size; i++) {
        furi_string_cat_printf(fields, "%02X", data[i]);
    }
    furi_string_push_back(fields, '"');
}

void reader_log_bytes(const char* reader, int status, const uint8_t* data, size_t size) {
    if(!log_mutex) return;
    FuriString* fields = furi_string_alloc();
    furi_string_printf(fields, ",\"reader\":\"%s\",\"status\":%d", reader, status);
    if(status == PM3_SUCCESS) append_hex(fields, "data", data, size);
    publish("read", fields);
    furi_string_free(fields);
}

void reader_log_hf14a(int status, const iso14a_card_select_t* card) {
    if(!log_mutex) return;
    FuriString* fields = furi_string_alloc();
    furi_string_printf(fields, ",\"reader\":\"hf\",\"status\":%d", status);
    if(status == PM3_SUCCESS) {
        append_hex(fields, "uid", card->uid, MIN(card->uidlen, sizeof(card->uid)));
        furi_string_cat_printf(
            fields, ",\"atqa\":\"%02X%02X\",\"sak\":\"%02X\"", card->atqa[1], card->atqa[0], card->sak);
        append_hex(fields, "ats", card->ats, MIN(card->ats_len, sizeof(card->ats)));
    }
    publish("read", fields);
    furi_string_free(fields);
}
