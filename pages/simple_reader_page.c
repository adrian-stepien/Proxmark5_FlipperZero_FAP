#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include "simple_reader_page.h"
#include "../reader_log.h"
#include "pm3_cmd.h"
#include "../readers/lf_capture.h"
#include "proxmark5_frame.h"

typedef struct {
    const SimpleReader* reader;
    bool running;
    int status;
    SimpleReaderResult result;
} SimpleReaderModel;

struct SimpleReaderPage {
    View* view;
    FuriThread* worker;
    // The view model lock protects the worker state.
    const SimpleReader* next_reader;
    bool worker_active;
    bool discard_result;
};

static void simple_reader_draw(Canvas* canvas, void* context) {
    const SimpleReaderModel* model = context;
    canvas_clear(canvas);
    if(!model->reader) return;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 11, model->reader->title);
    canvas_set_font(canvas, FontSecondary);

    if(model->running) {
        canvas_draw_str(canvas, 4, 27, model->reader->prompt);
        canvas_draw_str(canvas, 4, 40, "Reading...");
        return;
    }

    char line[32];
    switch(model->status) {
    case PM3_SUCCESS: {
        size_t count = MIN(model->result.line_count, SIMPLE_READER_MAX_LINES);
        uint8_t y = count == 1 ? 30 : 25;
        for(size_t i = 0; i < count; i++, y += 12) {
            canvas_draw_str(canvas, 4, y, model->result.lines[i]);
        }
        break;
    }
    case PM3_ENODATA:
        canvas_draw_str(canvas, 4, 25, model->reader->no_data);
        canvas_draw_str(canvas, 4, 38, model->reader->no_data_hint);
        break;
    case LfCaptureRestoreFailed:
        canvas_draw_str(canvas, 4, 25, "LF settings may have");
        canvas_draw_str(canvas, 4, 38, "changed. Check on PM5.");
        break;
    case PM3_EMALLOC:
        canvas_draw_str(canvas, 4, 27, "Not enough memory");
        break;
    case PM3_EIO:
        canvas_draw_str(canvas, 4, 27, "Send failed. Check link.");
        break;
    case PM3_ETIMEOUT:
        canvas_draw_str(canvas, 4, 25, "No reply from PM5");
        canvas_draw_str(canvas, 4, 38, "Check PM5 connection");
        break;
    case PM3_ENOTIMPL:
        canvas_draw_str(canvas, 4, 27, "Reader unavailable");
        break;
    case PM3_EOPABORTED:
        canvas_draw_str(canvas, 4, 27, "Read cancelled");
        break;
    default:
        snprintf(line, sizeof(line), "Read error (%d)", model->status);
        canvas_draw_str(canvas, 4, 27, line);
        break;
    }
    elements_button_center(canvas, "Read again");
    if(model->status == PM3_SUCCESS && model->reader->save) {
        elements_button_right(canvas, "Save");
    }
}

static int32_t simple_reader_worker(void* context) {
    SimpleReaderPage* page = context;
    for(;;) {
        SimpleReaderModel* model = view_get_model(page->view);
        const SimpleReader* reader = page->next_reader;
        page->next_reader = NULL;
        if(!reader) {
            page->worker_active = false;
            view_commit_model(page->view, false);
            return 0;
        }
        page->discard_result = false;
        view_commit_model(page->view, false);

        SimpleReaderResult result = {.data = lf_capture_alloc(reader->data_size)};
        proxmark5_frame_begin_operation(NULL);
        view_get_model(page->view);
        const bool left = page->discard_result;
        view_commit_model(page->view, false);
        int status = PM3_EOPABORTED;
        if(!left) status = result.data ? reader->read(&result) : PM3_EMALLOC;
        proxmark5_frame_end_operation();

        model = view_get_model(page->view);
        const bool publish = !page->discard_result;
        if(publish) {
            model->running = false;
            model->status = status;
            free(model->result.data);
            model->result = result;
        } else {
            free(result.data);
        }
        view_commit_model(page->view, publish);
        if(publish && reader->name) reader_log_bytes(reader->name, status, result.data, reader->data_size);
    }
}

void simple_reader_page_start(SimpleReaderPage* page, const SimpleReader* reader) {
    SimpleReaderModel* model = view_get_model(page->view);
    const bool start_worker = !page->worker_active;
    page->worker_active = true;
    page->next_reader = reader;
    page->discard_result = true;
    model->reader = reader;
    model->running = true;
    view_commit_model(page->view, true);
    if(start_worker) {
        if(!page->worker) {
            page->worker = furi_thread_alloc_ex("PM5Reader", 4096, simple_reader_worker, page);
        }
        furi_thread_join(page->worker);
        furi_thread_start(page->worker);
    }
}

void simple_reader_page_stop(SimpleReaderPage* page) {
    SimpleReaderModel* model = view_get_model(page->view);
    page->next_reader = NULL;
    page->discard_result = true;
    model->running = false;
    view_commit_model(page->view, false);
}

static bool simple_reader_input(InputEvent* event, void* context) {
    SimpleReaderPage* page = context;
    if(event->type != InputTypeShort) return false;
    if(event->key == InputKeyRight) {
        SimpleReaderModel* model = view_get_model(page->view);
        const SimpleReader* reader = model->reader;
        const uint8_t* data = !model->running && model->status == PM3_SUCCESS ? model->result.data : NULL;
        view_commit_model(page->view, false);
        if(data && reader && reader->save) reader->save(data);
        return true;
    }
    if(event->key != InputKeyOk) return false;
    SimpleReaderModel* model = view_get_model(page->view);
    const SimpleReader* reader = model->running ? NULL : model->reader;
    view_commit_model(page->view, false);
    if(reader) simple_reader_page_start(page, reader);
    return true;
}

SimpleReaderPage* simple_reader_page_create(void) {
    SimpleReaderPage* page = calloc(1, sizeof(SimpleReaderPage));
    page->view = view_alloc();
    view_set_context(page->view, page);
    view_set_draw_callback(page->view, simple_reader_draw);
    view_set_input_callback(page->view, simple_reader_input);
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(SimpleReaderModel));
    SimpleReaderModel* model = view_get_model(page->view);
    model->reader = NULL;
    view_commit_model(page->view, false);
    return page;
}

void simple_reader_page_stop_worker(SimpleReaderPage* page) {
    if(!page || !page->worker) return;
    simple_reader_page_stop(page);
    furi_thread_join(page->worker);
    furi_thread_free(page->worker);
    page->worker = NULL;
    SimpleReaderModel* model = view_get_model(page->view);
    free(model->result.data);
    model->result = (SimpleReaderResult){0};
    view_commit_model(page->view, false);
}

void simple_reader_page_free(SimpleReaderPage* page) {
    simple_reader_page_stop_worker(page);
    view_free(page->view);
    free(page);
}

View* simple_reader_page_get_view(SimpleReaderPage* page) {
    return page->view;
}
