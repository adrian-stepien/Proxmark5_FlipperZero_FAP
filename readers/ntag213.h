#ifndef NTAG213_H
#define NTAG213_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NTAG213_UID_BYTES 7U
#define NTAG213_VERSION_BYTES 8U
#define NTAG213_PAGE_BYTES 4U
#define NTAG213_READ_PAGES 40U
#define NTAG213_TOTAL_PAGES 45U
#define NTAG213_USER_START 4U
#define NTAG213_USER_BYTES ((NTAG213_READ_PAGES - NTAG213_USER_START) * NTAG213_PAGE_BYTES)
#define NTAG213_TEXT_MAX (NTAG213_USER_BYTES - 10U)
#define NTAG213_READ_BYTES (NTAG213_READ_PAGES * NTAG213_PAGE_BYTES)

typedef struct {
    uint8_t bytes[NTAG213_READ_BYTES];
} Ntag213Pages;

extern const uint8_t ntag213_version[NTAG213_VERSION_BYTES];

void ntag213_pages_uid(const Ntag213Pages* pages, uint8_t uid[NTAG213_UID_BYTES]);
bool ntag213_pages_valid(const Ntag213Pages* pages);

typedef enum {
    Ntag213WriteChecking,
    Ntag213WritePages,
    Ntag213WriteVerifying,
} Ntag213WriteStage;

typedef enum {
    Ntag213WriteVerified,
    Ntag213WriteLinkError,
    Ntag213WriteTagChanged,
    Ntag213WriteProtected,
    Ntag213WriteUnsupported,
    Ntag213WriteIncomplete,
} Ntag213WriteResult;

typedef void (*Ntag213WriteProgress)(
    Ntag213WriteStage stage, size_t done, size_t total, void* context);

Ntag213WriteResult ntag213_write_text(
    const Ntag213Pages* expected,
    const char* text,
    Ntag213WriteProgress progress,
    void* context,
    bool* cleanup_ok);

#endif
