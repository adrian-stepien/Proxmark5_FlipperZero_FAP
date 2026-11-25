#ifndef SIMPLE_READERS_H
#define SIMPLE_READERS_H

#include <stddef.h>

#define SIMPLE_READER_MAX_LINES 3U
#define SIMPLE_READER_LINE_SIZE 32U

enum { SimpleReaderSamplingRestoreFailed = 1 };

typedef struct {
    char lines[SIMPLE_READER_MAX_LINES][SIMPLE_READER_LINE_SIZE];
    size_t line_count;
} SimpleReaderResult;

typedef struct {
    const char* title;
    const char* prompt;
    const char* no_data;
    const char* no_data_hint;
    int (*read)(SimpleReaderResult* result);
} SimpleReader;

extern const SimpleReader* const simple_readers[];
extern const size_t simple_reader_count;

#endif
