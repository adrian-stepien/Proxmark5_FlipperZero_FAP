#ifndef NTAG213_WRITE_PAGE_H
#define NTAG213_WRITE_PAGE_H

#include <gui/view.h>
#include "../readers/ntag213.h"

typedef struct Ntag213WritePage Ntag213WritePage;
typedef void (*Ntag213WritePageCallback)(void* context);

Ntag213WritePage* ntag213_write_page_alloc(
    Ntag213WritePageCallback on_text, Ntag213WritePageCallback on_done, void* context);
void ntag213_write_page_free(Ntag213WritePage* page);
View* ntag213_write_page_get_view(Ntag213WritePage* page);
View* ntag213_write_page_get_input_view(Ntag213WritePage* page);
void ntag213_write_page_edit(Ntag213WritePage* page, const Ntag213Pages* tag);
void ntag213_write_page_confirm(Ntag213WritePage* page);
void ntag213_write_page_wait(Ntag213WritePage* page);

#endif
