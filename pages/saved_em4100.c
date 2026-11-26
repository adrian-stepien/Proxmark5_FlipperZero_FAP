#include <dialogs/dialogs.h>
#include <flipper_format/flipper_format.h>
#include <furi.h>
#include <storage/storage.h>
#include "../readers/simple_readers.h"
#include "saved_em4100.h"

#define SAVED_TAGS_PATH APP_DATA_PATH("tags")
#define RFID_EXTENSION ".rfid"
#define RFID_FILE_TYPE "Flipper RFID key"
#define RFID_FILE_VERSION 1U
#define EM4100_KEY_TYPE "EM4100"

static void saved_message(const char* title, const char* text) {
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, title, 64, 12, AlignCenter, AlignCenter);
    dialog_message_set_text(message, text, 64, 31, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(dialogs, message);
    dialog_message_free(message);
    furi_record_close(RECORD_DIALOGS);
}

static void format_id(const uint8_t* id, char* text, size_t size) {
    snprintf(text, size, "%02X%02X%02X%02X%02X", id[0], id[1], id[2], id[3], id[4]);
}

static bool read_em4100(Storage* storage, const char* path, uint8_t* id) {
    FlipperFormat* file = flipper_format_file_alloc(storage);
    FuriString* value = furi_string_alloc();
    uint32_t version = 0;
    uint32_t count = 0;

    bool ok = flipper_format_file_open_existing(file, path) &&
              flipper_format_read_header(file, value, &version) &&
              furi_string_equal(value, RFID_FILE_TYPE) && version == RFID_FILE_VERSION &&
              flipper_format_read_string(file, "Key type", value) &&
              furi_string_equal(value, EM4100_KEY_TYPE) &&
              flipper_format_get_value_count(file, "Data", &count) && count == EM4100_ID_BYTES &&
              flipper_format_read_hex(file, "Data", id, EM4100_ID_BYTES);

    furi_string_free(value);
    flipper_format_free(file);
    return ok;
}

static bool write_em4100(Storage* storage, const char* path, const uint8_t* id) {
    FlipperFormat* file = flipper_format_file_alloc(storage);
    bool opened = flipper_format_file_open_new(file, path);
    bool written = opened && flipper_format_write_header_cstr(
                                 file, RFID_FILE_TYPE, RFID_FILE_VERSION) &&
                   flipper_format_write_string_cstr(file, "Key type", EM4100_KEY_TYPE) &&
                   flipper_format_write_hex(file, "Data", id, EM4100_ID_BYTES);
    bool closed = opened && flipper_format_file_close(file);
    flipper_format_free(file);

    uint8_t saved[EM4100_ID_BYTES];
    if(written && closed && read_em4100(storage, path, saved) &&
       memcmp(saved, id, sizeof(saved)) == 0) {
        return true;
    }

    if(opened) storage_common_remove(storage, path);
    return false;
}

void saved_em4100_save(const uint8_t* id) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* dir = furi_string_alloc_set(SAVED_TAGS_PATH);
    storage_common_resolve_path_and_ensure_app_directory(storage, dir);
    char text[2 * EM4100_ID_BYTES + 1];
    format_id(id, text, sizeof(text));

    const char* title = "Save failed";
    const char* body = "Check the SD card";
    if(storage_simply_mkdir(storage, furi_string_get_cstr(dir))) {
        FuriString* path =
            furi_string_alloc_printf("%s/EM4100-%s" RFID_EXTENSION, furi_string_get_cstr(dir), text);
        const char* filename = furi_string_get_cstr(path);
        uint8_t previous[EM4100_ID_BYTES];
        if(read_em4100(storage, filename, previous) &&
           memcmp(previous, id, sizeof(previous)) == 0) {
            title = "Already saved";
            body = text;
        } else if(write_em4100(storage, filename, id)) {
            title = "EM4100 saved";
            body = text;
        }
        furi_string_free(path);
    }

    furi_string_free(dir);
    furi_record_close(RECORD_STORAGE);
    saved_message(title, body);
}

void saved_em4100_browse(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* dir = furi_string_alloc_set(SAVED_TAGS_PATH);
    storage_common_resolve_path_and_ensure_app_directory(storage, dir);
    if(storage_simply_mkdir(storage, furi_string_get_cstr(dir))) {
        DialogsFileBrowserOptions options;
        dialog_file_browser_set_basic_options(&options, RFID_EXTENSION, NULL);
        options.base_path = furi_string_get_cstr(dir);
        options.hide_ext = true;

        DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
        FuriString* selected = furi_string_alloc();
        bool opened = dialog_file_browser_show(dialogs, selected, dir, &options);
        furi_record_close(RECORD_DIALOGS);

        uint8_t id[EM4100_ID_BYTES];
        if(opened && read_em4100(storage, furi_string_get_cstr(selected), id)) {
            char text[2 * EM4100_ID_BYTES + 1];
            format_id(id, text, sizeof(text));
            saved_message("Saved EM4100", text);
        } else if(opened) {
            saved_message("Unable to open", "Invalid EM4100 file");
        }
        furi_string_free(selected);
    } else {
        saved_message("Unable to open", "Check the SD card");
    }

    furi_string_free(dir);
    furi_record_close(RECORD_STORAGE);
}
