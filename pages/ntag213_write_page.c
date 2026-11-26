#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/modules/text_input.h>
#include "ntag213_write_page.h"

typedef enum {
    WritePageConfirm,
    WritePageBusy,
    WritePageResult,
} WritePageState;

typedef struct {
    const Ntag213WritePage* page;
    WritePageState state;
    Ntag213WriteStage stage;
    Ntag213WriteResult result;
    bool cleanup_ok;
    size_t done;
    size_t total;
} WritePageModel;

struct Ntag213WritePage {
    View* view;
    TextInput* input;
    FuriThread* worker;
    Ntag213WritePageCallback on_text;
    Ntag213WritePageCallback on_done;
    void* context;
    Ntag213Pages tag;
    char uid[2 * NTAG213_UID_BYTES + 1];
    char text[NTAG213_TEXT_MAX + 1];
    char preview[21];
};

static void draw_result(Canvas* canvas, Ntag213WriteResult result, bool cleanup_ok) {
    switch(result) {
    case Ntag213WriteVerified:
        canvas_draw_str(canvas, 4, 31, cleanup_ok ? "Write verified" : "Readback matches");
        if(cleanup_ok) canvas_draw_str(canvas, 4, 43, "Readback matches");
        break;
    case Ntag213WriteTagChanged:
        canvas_draw_str(canvas, 4, 31, "Tag missing or changed");
        canvas_draw_str(canvas, 4, 43, "Rescan before writing");
        break;
    case Ntag213WriteProtected:
        canvas_draw_str(canvas, 4, 31, "Tag is protected");
        canvas_draw_str(canvas, 4, 43, "No pages written");
        break;
    case Ntag213WriteUnsupported:
        canvas_draw_str(canvas, 4, 31, "Unsupported NDEF layout");
        canvas_draw_str(canvas, 4, 43, "No pages written");
        break;
    case Ntag213WriteLinkError:
        canvas_draw_str(canvas, 4, 31, "Could not check tag");
        canvas_draw_str(canvas, 4, 43, "No pages written");
        break;
    case Ntag213WriteIncomplete:
        canvas_draw_str(canvas, 4, 31, "Write not verified");
        canvas_draw_str(canvas, 4, 42, "Tag may have changed");
        break;
    }
    if(!cleanup_ok) canvas_draw_str(canvas, 4, 53, "Field off unconfirmed");
    elements_button_center(canvas, "Rescan");
}

static void ntag213_write_draw(Canvas* canvas, void* context) {
    const WritePageModel* model = context;
    const Ntag213WritePage* page = model->page;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 11, "Write NTAG213");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 4, 23, "UID");
    canvas_draw_str(canvas, 23, 23, page->uid);

    switch(model->state) {
    case WritePageConfirm:
        canvas_draw_str(canvas, 4, 35, page->preview);
        canvas_draw_str(canvas, 4, 46, "Replace NDEF text?");
        elements_button_center(canvas, "Write");
        break;
    case WritePageBusy:
        if(model->stage == Ntag213WritePages) {
            char line[28];
            snprintf(line, sizeof(line), "Writing %zu/%zu pages", model->done, model->total);
            canvas_draw_str(canvas, 4, 37, line);
        } else {
            canvas_draw_str(
                canvas,
                4,
                37,
                model->stage == Ntag213WriteVerifying ? "Verifying readback" : "Checking tag");
        }
        canvas_draw_str(canvas, 4, 52, "Keep tag in place");
        break;
    case WritePageResult:
        draw_result(canvas, model->result, model->cleanup_ok);
        break;
    }
}

static void ntag213_write_progress(
    Ntag213WriteStage stage, size_t done, size_t total, void* context) {
    Ntag213WritePage* page = context;
    WritePageModel* model = view_get_model(page->view);
    model->stage = stage;
    model->done = done;
    model->total = total;
    view_commit_model(page->view, true);
}

static int32_t ntag213_write_worker(void* context) {
    Ntag213WritePage* page = context;
    bool cleanup_ok;
    Ntag213WriteResult result =
        ntag213_write_text(&page->tag, page->text, ntag213_write_progress, page, &cleanup_ok);
    WritePageModel* model = view_get_model(page->view);
    model->state = WritePageResult;
    model->result = result;
    model->cleanup_ok = cleanup_ok;
    view_commit_model(page->view, true);
    return 0;
}

static bool ntag213_write_input(InputEvent* event, void* context) {
    if(event->type != InputTypeShort) return false;
    Ntag213WritePage* page = context;
    WritePageModel* model = view_get_model(page->view);
    WritePageState state = model->state;
    bool start = state == WritePageConfirm && event->key == InputKeyOk;
    if(start) {
        model->state = WritePageBusy;
        model->stage = Ntag213WriteChecking;
    }
    view_commit_model(page->view, start);

    if(start) {
        page->worker = furi_thread_alloc_ex("Ntag213Write", 4096, ntag213_write_worker, page);
        furi_thread_start(page->worker);
        return true;
    }
    if(state == WritePageResult && (event->key == InputKeyOk || event->key == InputKeyBack)) {
        page->on_done(page->context);
        return true;
    }
    return state == WritePageBusy;
}

Ntag213WritePage* ntag213_write_page_alloc(
    Ntag213WritePageCallback on_text, Ntag213WritePageCallback on_done, void* context) {
    Ntag213WritePage* page = calloc(1, sizeof(Ntag213WritePage));
    page->on_text = on_text;
    page->on_done = on_done;
    page->context = context;
    page->input = text_input_alloc();
    page->view = view_alloc();
    view_set_context(page->view, page);
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(WritePageModel));
    WritePageModel* model = view_get_model(page->view);
    model->page = page;
    view_commit_model(page->view, false);
    view_set_input_callback(page->view, ntag213_write_input);
    view_set_draw_callback(page->view, ntag213_write_draw);
    return page;
}

View* ntag213_write_page_get_view(Ntag213WritePage* page) {
    return page->view;
}

View* ntag213_write_page_get_input_view(Ntag213WritePage* page) {
    return text_input_get_view(page->input);
}

void ntag213_write_page_edit(Ntag213WritePage* page, const Ntag213Pages* tag) {
    page->tag = *tag;
    page->text[0] = '\0';
    text_input_reset(page->input);
    text_input_set_header_text(page->input, "NDEF text");
    text_input_set_minimum_length(page->input, 1);
    text_input_set_result_callback(
        page->input, page->on_text, page->context, page->text, sizeof(page->text), false);

    uint8_t uid[NTAG213_UID_BYTES];
    ntag213_pages_uid(tag, uid);
    for(size_t i = 0; i < sizeof(uid); i++) {
        snprintf(page->uid + 2 * i, 3, "%02X", uid[i]);
    }
}

void ntag213_write_page_confirm(Ntag213WritePage* page) {
    size_t length = strlen(page->text);
    if(length < sizeof(page->preview)) {
        memcpy(page->preview, page->text, length + 1);
    } else {
        snprintf(page->preview, sizeof(page->preview), "%.17s...", page->text);
    }
    WritePageModel* model = view_get_model(page->view);
    model->state = WritePageConfirm;
    view_commit_model(page->view, true);
}

void ntag213_write_page_wait(Ntag213WritePage* page) {
    if(!page->worker) return;
    furi_thread_join(page->worker);
    furi_thread_free(page->worker);
    page->worker = NULL;
}

void ntag213_write_page_free(Ntag213WritePage* page) {
    ntag213_write_page_wait(page);
    view_free(page->view);
    text_input_free(page->input);
    free(page);
}
