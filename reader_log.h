#ifndef READER_LOG_H
#define READER_LOG_H

#include <gui/view_dispatcher.h>
#include "pm3_cmd.h"

void reader_log_open(const char* args, ViewDispatcher* dispatcher);
void reader_log_close(void);
void reader_log_event(const char* event);
void reader_log_bytes(const char* reader, int status, const uint8_t* data, size_t size);
void reader_log_hf14a(int status, const iso14a_card_select_t* card);

#endif
