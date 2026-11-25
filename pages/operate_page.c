#include <furi.h>
#include "operate_page.h"
#include "../readers/simple_readers.h"

OperatePage* operate_page_create(SubmenuItemCallback callback, void* context) {
    OperatePage* operate_page = malloc(sizeof(OperatePage));
    operate_page->submenu = submenu_alloc();
    Submenu* menu = operate_page->submenu;
    submenu_set_header(menu, "Functions Menu");
    submenu_add_item(menu, "HF 14a Reader", OperateReadHf14a, callback, context);
    submenu_add_item(menu, "Hitag2 Reader", OperateReadHitag2, callback, context);
    for(size_t i = 0; i < simple_reader_count; i++) {
        submenu_add_item(
            menu, simple_readers[i]->title, OperateSimpleReaderFirst + i, callback, context);
    }
    submenu_add_item(menu, "Hardware Status", OperateOpenStatus, callback, context);
    return operate_page;
}

void operate_page_free(OperatePage* operate_page) {
    submenu_free(operate_page->submenu);
    free(operate_page);
}
