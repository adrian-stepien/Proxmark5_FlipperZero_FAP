#include "type2_ndef.h"
#include <string.h>


static Type2NdefResult read_ndef_message(
    const uint8_t* data, size_t size, char* text, size_t text_size) {
    bool first = true;
    bool found = false;
    while(size >= 2) {
        uint8_t flags = *data++;
        uint8_t type_size = *data++;
        size -= 2;
        if(((flags & 0x80) != 0) != first) return Type2NdefInvalid;
        if(flags & 0x20) return Type2NdefOther; // Chunked records need reassembly.
        first = false;

        size_t width = (flags & 0x10) ? 1 : 4;
        if(size < width) return Type2NdefInvalid;

        uint32_t payload_size = 0;
        for(size_t index = 0; index < width; index++) {
            payload_size = (payload_size << 8) | *data++;
        }
        size -= width;

        uint8_t id_size = 0;
        if(flags & 0x08) {
            if(!size) return Type2NdefInvalid;
            id_size = *data++;
            size--;
        }

        size_t header_size = (size_t)type_size + id_size;
        if(header_size > size || payload_size > size - header_size) return Type2NdefInvalid;
        uint8_t tnf = flags & 0x07;
        if(tnf > 5 || (!tnf && (header_size || payload_size))) return Type2NdefInvalid;
        const uint8_t* payload = data + header_size;
        if(tnf == 1 && type_size == 1 && data[0] == 'T') {
            if(!payload_size || (payload[0] & 0x40)) return Type2NdefInvalid;
            size_t language_size = payload[0] & 0x3f;
            if(language_size >= payload_size) return Type2NdefInvalid;
            if(!found && !(payload[0] & 0x80)) {
                size_t count = payload_size - 1 - language_size;
                if(count >= text_size) return Type2NdefInvalid;
                for(size_t i = 0; i < count; i++) {
                    uint8_t byte = payload[1 + language_size + i];
                    text[i] = ((byte < 0x20 && byte != '\n') || byte == 0x7f) ? '.' : byte;
                }
                text[count] = '\0';
                found = true;
            }
        }

        data = payload + payload_size;
        size -= header_size + payload_size;
        if(flags & 0x40) {
            if(size) return Type2NdefInvalid;
            return found ? Type2NdefText : Type2NdefOther;
        }
    }
    return Type2NdefInvalid;
}

Type2NdefResult type2_ndef_read_text(
    const uint8_t* data, size_t size, char* text, size_t text_size) {
    if(!text_size) return Type2NdefInvalid;
    text[0] = '\0';
    for(size_t offset = 0; offset < size;) {
        uint8_t type = data[offset++];
        if(type == 0x00) continue;
        if(type == 0xfe) break;
        if(offset == size) return Type2NdefInvalid;

        size_t length = data[offset++];
        if(length == 0xff) {
            if(size - offset < 2) return Type2NdefInvalid;
            length = ((size_t)data[offset] << 8) | data[offset + 1];
            offset += 2;
        }
        if(length > size - offset) return Type2NdefInvalid;
        if(type == 0x03) {
            if(!length) return Type2NdefEmpty;
            Type2NdefResult result = read_ndef_message(data + offset, length, text, text_size);
            if(result != Type2NdefText) text[0] = '\0';
            return result;
        }
        offset += length;
    }
    return Type2NdefEmpty;
}

bool type2_ndef_replace_text(uint8_t* data, size_t size, const char* text) {
    static const uint8_t record[] = {0x03, 0, 0xd1, 1, 0, 'T', 2, 'e', 'n'};
    size_t length = 0;
    for(; text[length]; length++) {
        if((unsigned char)text[length] < 0x20 || (unsigned char)text[length] > 0x7e) return false;
    }
    size_t end = sizeof(record) + length + 1;
    if(!length || end > size || length + 7 >= 0xff) return false;

    size_t old_end = 0;
    if(data[0] == 0x03) {
        size_t message_size = data[1];
        old_end = 2 + message_size + 1;
        if(old_end > size || data[old_end - 1] != 0xfe) return false;
        if(message_size &&
           (message_size < 5 || data[2] != 0xd1 || data[3] != 1 ||
            data[4] + 4U != message_size || data[5] != 'T' ||
            (data[6] & 0xc0) || (data[6] & 0x3f) >= data[4])) {
            return false;
        }
    } else if(data[0] == 0xfe) {
        old_end = 1;
    } else {
        for(size_t i = 0; i < size; i++) {
            if(data[i]) return false;
        }
    }

    for(size_t i = old_end; i < end; i++) {
        if(data[i]) return false;
    }
    for(size_t i = end; i < old_end; i++) data[i] = 0;
    memcpy(data, record, sizeof(record));
    data[1] = length + 7;
    data[4] = length + 3;
    memcpy(data + sizeof(record), text, length);
    data[end - 1] = 0xfe;
    return true;
}
