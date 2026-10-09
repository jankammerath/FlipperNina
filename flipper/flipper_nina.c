#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/text_input.h>
#include <gui/elements.h>
#include <notification/notification_messages.h>
#include <expansion/expansion.h>
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    ViewMainMenu,
    ViewDeviceList,
    ViewDetail,
    ViewNameInput,
    ViewAlerts,
    ViewSettings,
    ViewAbout,
} AppView;

typedef enum {
    MenuNearby,
    MenuFavorites,
    MenuSettings,
    MenuAbout,
} MenuItem;

typedef enum {
    ListNearby,
    ListFavorites,
} ListMode;

typedef enum {
    FilterAll,
    FilterWifi,
    FilterBle,
    FilterCount,
} NearbyFilter;

typedef enum {
    EventToggleFavorite,
    EventOpenAlerts,
    EventRefresh,
} AppEvent;

typedef enum {
    WorkerEvtStop = (1 << 0),
    WorkerEvtRx = (1 << 1),
} WorkerEvt;

typedef enum {
    LinkWaiting,
    LinkOk,
    LinkNoUart,
} LinkStatus;

#define MAX_DEVICES 48
// A device counts as out of range after missing this many scan rounds
#define MISSED_ROUNDS   2
#define UART_BAUD       115200
#define LINE_MAX_LEN    192
#define REFRESH_MS      500
#define LIST_ROWS       4
#define LIST_TOP        13
#define LIST_ROW_HEIGHT 12
#define ALIAS_LEN       33
#define FAVORITES_PATH  APP_DATA_PATH("favorites.txt")

// XBM, LSB = leftmost pixel
static const uint8_t icon_ble_5x9[] = {0x04, 0x0C, 0x15, 0x0E, 0x04, 0x0E, 0x15, 0x0C, 0x04};
static const uint8_t icon_wifi_7x7[] = {0x3E, 0x41, 0x00, 0x1C, 0x22, 0x00, 0x08};
static const uint8_t icon_phone_5x9[] = {0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F, 0x1B, 0x1F};

static const char* const on_off_text[] = {"Off", "On"};

static const NotificationSequence sequence_alert_sound = {
    &message_note_c7,
    &message_delay_100,
    &message_note_e7,
    &message_delay_100,
    &message_sound_off,
    NULL,
};

static const NotificationSequence sequence_alert_vibro = {
    &message_vibro_on,
    &message_delay_250,
    &message_vibro_off,
    NULL,
};

static const NotificationSequence sequence_alert_both = {
    &message_vibro_on,
    &message_note_c7,
    &message_delay_100,
    &message_note_e7,
    &message_delay_100,
    &message_sound_off,
    &message_vibro_off,
    NULL,
};

typedef struct {
    bool used;
    bool is_wifi;
    bool favorite;
    bool alert_sound;
    bool alert_vibro;
    bool visible;
    bool seen;
    uint8_t misses;
    int8_t rssi;
    uint8_t channel;
    char address[18];
    // Advertised name for BLE, SSID for WiFi
    char name[33];
    char vendor[20];
    char make[33];
    char model[33];
    char security[8];
    // Phone OS reported by the Nina: 'I' iOS, 'A' Android, 0 unknown
    char os;
    // User-given name of a favorite; overrides everything else
    char alias[ALIAS_LEN];
    char display[104];
} Device;

typedef struct {
    uint8_t id;
    bool is_wifi;
    bool is_phone;
    bool favorite;
    bool visible;
    int8_t rssi;
    char name[40];
} ListEntry;

typedef struct {
    ListMode mode;
    NearbyFilter filter;
    LinkStatus link;
    uint8_t count;
    uint8_t cursor;
    uint8_t offset;
    ListEntry entries[MAX_DEVICES];
} DeviceListModel;

typedef struct {
    Gui* gui;
    NotificationApp* notifications;
    Expansion* expansion;
    ViewDispatcher* view_dispatcher;
    FuriTimer* refresh_timer;
    Submenu* main_menu;
    View* device_list;
    Widget* detail;
    TextInput* name_input;
    char name_buffer[ALIAS_LEN];
    VariableItemList* alerts;
    VariableItemList* settings;
    Widget* about;
    ListMode list_mode;
    NearbyFilter nearby_filter;
    uint32_t selected;
    // Device shown in the detail, name and alerts screens
    uint32_t detail_id;
    uint8_t beep_enabled;
    // Only turn 5V off on exit if this app turned it on
    bool otg_enabled_by_app;

    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx_stream;
    FuriThread* worker;
    // Guards everything below; shared with the UART worker thread
    FuriMutex* mutex;
    Device devices[MAX_DEVICES];
    LinkStatus link;
    bool dirty;
    bool pending_sound;
    bool pending_vibro;
} FlipperNinaApp;

static int entry_rank(const ListEntry* e) {
    return e->visible ? e->rssi : -128;
}

static void list_scroll_to_cursor(DeviceListModel* model) {
    if(model->cursor < model->offset) model->offset = model->cursor;
    if(model->cursor >= model->offset + LIST_ROWS) model->offset = model->cursor - LIST_ROWS + 1;
}

// Rebuilds the list sorted by signal strength, keeping the cursor on the selected device.
static void device_list_refresh(FlipperNinaApp* app) {
    DeviceListModel* model = view_get_model(app->device_list);
    model->mode = app->list_mode;
    model->filter = app->nearby_filter;
    model->count = 0;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    model->link = app->link;
    for(uint8_t i = 0; i < MAX_DEVICES; i++) {
        const Device* dev = &app->devices[i];
        if(!dev->used) continue;
        if(app->list_mode == ListNearby) {
            if(!dev->visible) continue;
            if(app->nearby_filter == FilterWifi && !dev->is_wifi) continue;
            if(app->nearby_filter == FilterBle && dev->is_wifi) continue;
        } else if(!dev->favorite) {
            continue;
        }

        ListEntry e;
        e.id = i;
        e.is_wifi = dev->is_wifi;
        e.is_phone = dev->os != 0;
        e.favorite = dev->favorite;
        e.visible = dev->visible;
        e.rssi = dev->rssi;
        snprintf(e.name, sizeof(e.name), "%s", dev->display);

        uint8_t pos = model->count;
        while(pos > 0 && entry_rank(&model->entries[pos - 1]) < entry_rank(&e)) {
            model->entries[pos] = model->entries[pos - 1];
            pos--;
        }
        model->entries[pos] = e;
        model->count++;
    }
    furi_mutex_release(app->mutex);

    bool found = false;
    for(uint8_t i = 0; i < model->count; i++) {
        if(model->entries[i].id == app->selected) {
            model->cursor = i;
            found = true;
            break;
        }
    }
    if(!found && model->cursor >= model->count) {
        model->cursor = model->count ? model->count - 1 : 0;
    }
    if(model->count) app->selected = model->entries[model->cursor].id;

    if(model->count <= LIST_ROWS) {
        model->offset = 0;
    } else if(model->offset > model->count - LIST_ROWS) {
        model->offset = model->count - LIST_ROWS;
    }
    list_scroll_to_cursor(model);
    view_commit_model(app->device_list, true);
}

static void device_list_draw(Canvas* canvas, void* _model) {
    DeviceListModel* model = _model;
    char buf[24];

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    if(model->mode == ListFavorites) {
        snprintf(buf, sizeof(buf), "Favorites (%u)", model->count);
    } else {
        snprintf(buf, sizeof(buf), "Nearby (%u)", model->count);
    }
    canvas_draw_str(canvas, 2, 10, buf);
    if(model->mode == ListNearby) {
        if(model->filter == FilterAll) {
            canvas_draw_xbm(canvas, 110, 2, 7, 7, icon_wifi_7x7);
            canvas_draw_xbm(canvas, 121, 1, 5, 9, icon_ble_5x9);
        } else if(model->filter == FilterWifi) {
            canvas_draw_xbm(canvas, 119, 2, 7, 7, icon_wifi_7x7);
        } else {
            canvas_draw_xbm(canvas, 121, 1, 5, 9, icon_ble_5x9);
        }
    }
    canvas_draw_line(canvas, 0, 11, 127, 11);

    canvas_set_font(canvas, FontSecondary);
    if(model->count == 0) {
        const char* text = "No devices";
        if(model->link == LinkWaiting) text = "Waiting for Nina...";
        if(model->link == LinkNoUart) text = "UART busy";
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, text);
        return;
    }

    for(uint8_t row = 0; row < LIST_ROWS; row++) {
        uint8_t idx = model->offset + row;
        if(idx >= model->count) break;
        const ListEntry* e = &model->entries[idx];
        int32_t top = LIST_TOP + row * LIST_ROW_HEIGHT;

        canvas_set_color(canvas, ColorBlack);
        if(idx == model->cursor) {
            canvas_draw_rbox(canvas, 0, top, 124, LIST_ROW_HEIGHT, 2);
            canvas_set_color(canvas, ColorWhite);
        }

        if(e->is_wifi) {
            canvas_draw_xbm(canvas, 2, top + 3, 7, 7, icon_wifi_7x7);
        } else if(e->is_phone) {
            canvas_draw_xbm(canvas, 3, top + 2, 5, 9, icon_phone_5x9);
        } else {
            canvas_draw_xbm(canvas, 3, top + 2, 5, 9, icon_ble_5x9);
        }

        FuriString* label = furi_string_alloc_printf("%s%s", e->favorite ? "*" : "", e->name);
        elements_string_fit_width(canvas, label, 88);
        canvas_draw_str(canvas, 12, top + 10, furi_string_get_cstr(label));
        furi_string_free(label);

        if(e->visible) {
            snprintf(buf, sizeof(buf), "%d", e->rssi);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        canvas_draw_str_aligned(canvas, 121, top + 10, AlignRight, AlignBottom, buf);
    }

    canvas_set_color(canvas, ColorBlack);
    elements_scrollbar_pos(canvas, 128, LIST_TOP, 64 - LIST_TOP, model->cursor, model->count);
}

static void detail_rebuild(FlipperNinaApp* app);

static bool device_list_input(InputEvent* event, void* context) {
    FlipperNinaApp* app = context;
    if(event->key == InputKeyBack) return false;
    if(event->key == InputKeyRight) {
        if(event->type == InputTypeShort && app->list_mode == ListNearby) {
            app->nearby_filter = (app->nearby_filter + 1) % FilterCount;
            device_list_refresh(app);
        }
        return true;
    }
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    bool open = false;
    DeviceListModel* model = view_get_model(app->device_list);
    if(model->count) {
        if(event->key == InputKeyUp) {
            model->cursor = model->cursor ? model->cursor - 1 : model->count - 1;
        } else if(event->key == InputKeyDown) {
            model->cursor = (model->cursor + 1) % model->count;
        } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
            open = true;
        }
        list_scroll_to_cursor(model);
        app->selected = model->entries[model->cursor].id;
    }
    view_commit_model(app->device_list, true);

    if(open) {
        app->detail_id = app->selected;
        detail_rebuild(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewDetail);
    }
    return true;
}

static void copy_field(char* dst, size_t size, const char* src) {
    snprintf(dst, size, "%s", src);
}

// Picks the most readable name from what the Nina reported.
static void device_update_display(Device* d) {
    char* out = d->display;
    size_t size = sizeof(d->display);
    const char* short_addr = strlen(d->address) >= 17 ? d->address + 9 : d->address;

    if(d->alias[0]) {
        copy_field(out, size, d->alias);
    } else if(d->is_wifi) {
        if(d->name[0]) {
            copy_field(out, size, d->name);
        } else {
            snprintf(out, size, "Hidden %s", short_addr);
        }
    } else if(d->name[0] && d->make[0] && d->model[0]) {
        snprintf(out, size, "%s - %s %s", d->name, d->make, d->model);
    } else if(d->name[0]) {
        copy_field(out, size, d->name);
    } else if(d->make[0] && d->model[0]) {
        snprintf(out, size, "%s %s", d->make, d->model);
    } else if(d->model[0]) {
        copy_field(out, size, d->model);
    } else if(d->make[0]) {
        snprintf(out, size, "%s %s", d->make, short_addr);
    } else if(d->os) {
        snprintf(out, size, "%s %s", d->os == 'I' ? "iOS" : "Android", short_addr);
    } else if(d->vendor[0]) {
        snprintf(out, size, "%s %s", d->vendor, short_addr);
    } else {
        copy_field(out, size, d->address);
    }
}

// Reuses the stalest out-of-range, non-favorite slot when the table is full.
static Device* device_find_or_add(FlipperNinaApp* app, bool is_wifi, const char* address) {
    Device* free_slot = NULL;
    Device* stale = NULL;
    for(uint8_t i = 0; i < MAX_DEVICES; i++) {
        Device* d = &app->devices[i];
        if(!d->used) {
            if(!free_slot) free_slot = d;
            continue;
        }
        if(d->is_wifi == is_wifi && strcmp(d->address, address) == 0) return d;
        if(!d->visible && !d->favorite && (!stale || d->misses > stale->misses)) stale = d;
    }

    Device* d = free_slot ? free_slot : stale;
    if(!d) return NULL;
    memset(d, 0, sizeof(Device));
    d->used = true;
    d->is_wifi = is_wifi;
    copy_field(d->address, sizeof(d->address), address);
    return d;
}

static void device_mark_seen(FlipperNinaApp* app, Device* d) {
    if(!d->visible && d->favorite) {
        if(d->alert_sound) app->pending_sound = true;
        if(d->alert_vibro) app->pending_vibro = true;
    }
    d->visible = true;
    d->seen = true;
    d->misses = 0;
}

static void devices_end_round(FlipperNinaApp* app, bool is_wifi) {
    for(uint8_t i = 0; i < MAX_DEVICES; i++) {
        Device* d = &app->devices[i];
        if(!d->used || d->is_wifi != is_wifi) continue;
        if(d->seen) {
            d->seen = false;
        } else {
            if(d->misses < UINT8_MAX) d->misses++;
            if(d->misses >= MISSED_ROUNDS) d->visible = false;
        }
    }
}

// Splits in place on tabs, keeping empty fields.
static size_t split_fields(char* line, char** fields, size_t max) {
    size_t n = 0;
    fields[n++] = line;
    for(char* p = line; *p && n < max; p++) {
        if(*p == '\t') {
            *p = '\0';
            fields[n++] = p + 1;
        }
    }
    return n;
}

static void handle_line(FlipperNinaApp* app, char* line) {
    char* f[8];
    size_t n = split_fields(line, f, COUNT_OF(f));
    bool is_record = f[0][0] != '\0' && f[0][1] == '\0';
    if(!is_record) return;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->link = LinkOk;
    if(f[0][0] == 'B' && n >= 7) {
        Device* d = device_find_or_add(app, false, f[1]);
        if(d) {
            d->rssi = atoi(f[2]);
            copy_field(d->name, sizeof(d->name), f[3]);
            copy_field(d->vendor, sizeof(d->vendor), f[4]);
            copy_field(d->make, sizeof(d->make), f[5]);
            copy_field(d->model, sizeof(d->model), f[6]);
            if(n >= 8 && (f[7][0] == 'I' || f[7][0] == 'A')) d->os = f[7][0];
            device_update_display(d);
            device_mark_seen(app, d);
        }
    } else if(f[0][0] == 'W' && n >= 6) {
        Device* d = device_find_or_add(app, true, f[1]);
        if(d) {
            d->rssi = atoi(f[2]);
            copy_field(d->name, sizeof(d->name), f[3]);
            copy_field(d->security, sizeof(d->security), f[4]);
            d->channel = atoi(f[5]);
            device_update_display(d);
            device_mark_seen(app, d);
        }
    } else if(f[0][0] == 'E' && n >= 2) {
        devices_end_round(app, f[1][0] == 'W');
    }
    app->dirty = true;
    furi_mutex_release(app->mutex);
}

static void serial_rx_callback(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    FlipperNinaApp* app = context;
    if(event == FuriHalSerialRxEventData) {
        uint8_t data = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(app->rx_stream, &data, 1, 0);
        furi_thread_flags_set(furi_thread_get_id(app->worker), WorkerEvtRx);
    }
}

static int32_t uart_worker(void* context) {
    FlipperNinaApp* app = context;
    char line[LINE_MAX_LEN];
    size_t len = 0;
    bool overflow = false;
    uint8_t buf[64];

    while(true) {
        uint32_t events =
            furi_thread_flags_wait(WorkerEvtStop | WorkerEvtRx, FuriFlagWaitAny, FuriWaitForever);
        furi_check((events & FuriFlagError) == 0);
        if(events & WorkerEvtStop) break;

        size_t n;
        while((n = furi_stream_buffer_receive(app->rx_stream, buf, sizeof(buf), 0)) > 0) {
            for(size_t i = 0; i < n; i++) {
                char c = buf[i];
                if(c == '\n') {
                    if(!overflow && len > 0) {
                        line[len] = '\0';
                        handle_line(app, line);
                    }
                    len = 0;
                    overflow = false;
                } else if(c == '\r') {
                    continue;
                } else if(len < LINE_MAX_LEN - 1) {
                    line[len++] = c;
                } else {
                    overflow = true;
                }
            }
        }
    }
    return 0;
}

static void refresh_timer_callback(void* context) {
    FlipperNinaApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, EventRefresh);
}

// One line per favorite: <B|W> <address> <alias> <sound 0|1> <vibrate 0|1>, tab-separated.
static void favorites_save(FlipperNinaApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);
    if(file_stream_open(stream, FAVORITES_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        for(uint8_t i = 0; i < MAX_DEVICES; i++) {
            const Device* d = &app->devices[i];
            if(!d->used || !d->favorite) continue;
            stream_write_format(
                stream,
                "%c\t%s\t%s\t%u\t%u\n",
                d->is_wifi ? 'W' : 'B',
                d->address,
                d->alias,
                d->alert_sound,
                d->alert_vibro);
        }
        furi_mutex_release(app->mutex);
    }
    file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}

static void favorites_load(FlipperNinaApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);
    FuriString* line = furi_string_alloc();
    char buf[LINE_MAX_LEN];
    char* f[5];

    if(file_stream_open(stream, FAVORITES_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        while(stream_read_line(stream, line)) {
            furi_string_trim(line);
            copy_field(buf, sizeof(buf), furi_string_get_cstr(line));
            size_t n = split_fields(buf, f, COUNT_OF(f));
            if(n < 2 || f[0][1] != '\0' || (f[0][0] != 'B' && f[0][0] != 'W')) continue;

            Device* d = device_find_or_add(app, f[0][0] == 'W', f[1]);
            if(!d) break;
            d->favorite = true;
            if(n >= 3) copy_field(d->alias, sizeof(d->alias), f[2]);
            d->alert_sound = n < 4 || f[3][0] != '0';
            d->alert_vibro = n < 5 || f[4][0] != '0';
            device_update_display(d);
        }
    }
    file_stream_close(stream);
    stream_free(stream);
    furi_string_free(line);
    furi_record_close(RECORD_STORAGE);
}

static void detail_button_callback(GuiButtonType result, InputType type, void* context) {
    FlipperNinaApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, EventToggleFavorite);
    } else if(type == InputTypeShort && result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(app->view_dispatcher, EventOpenAlerts);
    }
}

static void detail_rebuild(FlipperNinaApp* app) {
    if(app->detail_id >= MAX_DEVICES) return;
    char line[72];

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    const Device* dev = &app->devices[app->detail_id];
    if(!dev->used) {
        furi_mutex_release(app->mutex);
        return;
    }

    widget_reset(app->detail);
    widget_add_string_element(app->detail, 0, 0, AlignLeft, AlignTop, FontPrimary, dev->display);

    snprintf(line, sizeof(line), "%s %s", dev->is_wifi ? "BSSID:" : "MAC:", dev->address);
    widget_add_string_element(app->detail, 0, 13, AlignLeft, AlignTop, FontSecondary, line);

    if(dev->visible) {
        snprintf(line, sizeof(line), "RSSI: %d dBm", dev->rssi);
    } else {
        snprintf(line, sizeof(line), "RSSI: out of range");
    }
    widget_add_string_element(app->detail, 0, 23, AlignLeft, AlignTop, FontSecondary, line);

    if(dev->is_wifi) {
        snprintf(line, sizeof(line), "Security: %s", dev->security);
        widget_add_string_element(app->detail, 0, 33, AlignLeft, AlignTop, FontSecondary, line);
        snprintf(line, sizeof(line), "Channel: %u", dev->channel);
        widget_add_string_element(app->detail, 0, 43, AlignLeft, AlignTop, FontSecondary, line);
    } else {
        snprintf(
            line,
            sizeof(line),
            "Vendor: %s%s",
            dev->vendor[0] ? dev->vendor : "-",
            dev->os == 'I' ? " (iOS)" :
            dev->os == 'A' ? " (Android)" :
                             "");
        widget_add_string_element(app->detail, 0, 33, AlignLeft, AlignTop, FontSecondary, line);
        if(dev->make[0] || dev->model[0]) {
            snprintf(
                line,
                sizeof(line),
                "Model: %s%s%s",
                dev->make,
                dev->make[0] && dev->model[0] ? " " : "",
                dev->model);
            widget_add_string_element(
                app->detail, 0, 43, AlignLeft, AlignTop, FontSecondary, line);
        }
    }

    widget_add_button_element(
        app->detail,
        GuiButtonTypeCenter,
        dev->favorite ? "Unfav" : "Fav",
        detail_button_callback,
        app);
    if(dev->favorite) {
        widget_add_button_element(
            app->detail, GuiButtonTypeRight, "Alerts", detail_button_callback, app);
    }
    furi_mutex_release(app->mutex);
}

static void main_menu_callback(void* context, uint32_t index) {
    FlipperNinaApp* app = context;
    switch(index) {
    case MenuNearby:
    case MenuFavorites:
        app->list_mode = index == MenuNearby ? ListNearby : ListFavorites;
        with_view_model(
            app->device_list, DeviceListModel * model, { model->cursor = 0; }, false);
        app->selected = UINT32_MAX;
        device_list_refresh(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewDeviceList);
        break;
    case MenuSettings:
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewSettings);
        break;
    case MenuAbout:
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewAbout);
        break;
    }
}

static void name_input_done(void* context) {
    FlipperNinaApp* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(app->detail_id < MAX_DEVICES) {
        Device* d = &app->devices[app->detail_id];
        if(!d->favorite) {
            d->alert_sound = true;
            d->alert_vibro = true;
        }
        d->favorite = true;
        copy_field(d->alias, sizeof(d->alias), app->name_buffer);
        device_update_display(d);
    }
    furi_mutex_release(app->mutex);

    favorites_save(app);
    detail_rebuild(app);
    device_list_refresh(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewDetail);
}

static void alert_changed(VariableItem* item, bool is_sound) {
    FlipperNinaApp* app = variable_item_get_context(item);
    uint8_t on = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, on_off_text[on]);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(app->detail_id < MAX_DEVICES) {
        Device* d = &app->devices[app->detail_id];
        if(is_sound) {
            d->alert_sound = on;
        } else {
            d->alert_vibro = on;
        }
    }
    furi_mutex_release(app->mutex);
    favorites_save(app);
}

static void alert_sound_changed(VariableItem* item) {
    alert_changed(item, true);
}

static void alert_vibro_changed(VariableItem* item) {
    alert_changed(item, false);
}

static void alerts_open(FlipperNinaApp* app) {
    if(app->detail_id >= MAX_DEVICES) return;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    uint8_t sound = app->devices[app->detail_id].alert_sound;
    uint8_t vibro = app->devices[app->detail_id].alert_vibro;
    furi_mutex_release(app->mutex);

    variable_item_list_reset(app->alerts);
    VariableItem* item = variable_item_list_add(app->alerts, "Sound", 2, alert_sound_changed, app);
    variable_item_set_current_value_index(item, sound);
    variable_item_set_current_value_text(item, on_off_text[sound]);
    item = variable_item_list_add(app->alerts, "Vibrate", 2, alert_vibro_changed, app);
    variable_item_set_current_value_index(item, vibro);
    variable_item_set_current_value_text(item, on_off_text[vibro]);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewAlerts);
}

static bool custom_event_callback(void* context, uint32_t event) {
    FlipperNinaApp* app = context;
    if(event == EventOpenAlerts) {
        alerts_open(app);
        return true;
    }
    if(event == EventToggleFavorite) {
        if(app->detail_id >= MAX_DEVICES) return true;

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        Device* d = &app->devices[app->detail_id];
        bool unfav = d->favorite;
        if(unfav) {
            d->favorite = false;
            d->alias[0] = '\0';
            device_update_display(d);
        } else {
            copy_field(app->name_buffer, sizeof(app->name_buffer), d->display);
        }
        furi_mutex_release(app->mutex);

        if(unfav) {
            favorites_save(app);
            detail_rebuild(app);
            device_list_refresh(app);
        } else {
            // Re-set after prefilling so the cursor lands at the end of the text
            text_input_set_result_callback(
                app->name_input,
                name_input_done,
                app,
                app->name_buffer,
                sizeof(app->name_buffer),
                false);
            view_dispatcher_switch_to_view(app->view_dispatcher, ViewNameInput);
        }
        return true;
    }
    if(event == EventRefresh) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        bool dirty = app->dirty;
        bool sound = app->pending_sound;
        bool vibro = app->pending_vibro;
        app->dirty = false;
        app->pending_sound = false;
        app->pending_vibro = false;
        furi_mutex_release(app->mutex);

        if(app->beep_enabled) {
            if(sound && vibro) {
                notification_message(app->notifications, &sequence_alert_both);
            } else if(sound) {
                notification_message(app->notifications, &sequence_alert_sound);
            } else if(vibro) {
                notification_message(app->notifications, &sequence_alert_vibro);
            }
        }
        if(dirty) {
            device_list_refresh(app);
            detail_rebuild(app);
        }
        return true;
    }
    return false;
}

static void beep_changed(VariableItem* item) {
    FlipperNinaApp* app = variable_item_get_context(item);
    app->beep_enabled = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, on_off_text[app->beep_enabled]);
}

static uint32_t nav_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t nav_main_menu(void* context) {
    UNUSED(context);
    return ViewMainMenu;
}

static uint32_t nav_device_list(void* context) {
    UNUSED(context);
    return ViewDeviceList;
}

static uint32_t nav_detail(void* context) {
    UNUSED(context);
    return ViewDetail;
}

static FlipperNinaApp* app_alloc(void) {
    FlipperNinaApp* app = malloc(sizeof(FlipperNinaApp));
    memset(app, 0, sizeof(FlipperNinaApp));
    app->list_mode = ListNearby;
    app->selected = UINT32_MAX;
    app->detail_id = UINT32_MAX;
    app->beep_enabled = 1;

    app->otg_enabled_by_app = !furi_hal_power_is_otg_enabled();
    if(app->otg_enabled_by_app) furi_hal_power_enable_otg();

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    favorites_load(app);
    app->rx_stream = furi_stream_buffer_alloc(1024, 1);
    app->worker = furi_thread_alloc_ex("NinaUartWorker", 2048, uart_worker, app);
    furi_thread_start(app->worker);

    // The expansion module service would otherwise hold the GPIO USART
    app->expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(app->expansion);
    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(app->serial) {
        app->link = LinkWaiting;
        furi_hal_serial_init(app->serial, UART_BAUD);
        furi_hal_serial_async_rx_start(app->serial, serial_rx_callback, app, false);
    } else {
        app->link = LinkNoUart;
    }

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, custom_event_callback);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->main_menu = submenu_alloc();
    submenu_set_header(app->main_menu, "FlipperNina");
    submenu_add_item(app->main_menu, "Nearby Devices", MenuNearby, main_menu_callback, app);
    submenu_add_item(app->main_menu, "Favorites", MenuFavorites, main_menu_callback, app);
    submenu_add_item(app->main_menu, "Settings", MenuSettings, main_menu_callback, app);
    submenu_add_item(app->main_menu, "About", MenuAbout, main_menu_callback, app);
    view_set_previous_callback(submenu_get_view(app->main_menu), nav_exit);
    view_dispatcher_add_view(app->view_dispatcher, ViewMainMenu, submenu_get_view(app->main_menu));

    app->device_list = view_alloc();
    view_allocate_model(app->device_list, ViewModelTypeLocking, sizeof(DeviceListModel));
    view_set_context(app->device_list, app);
    view_set_draw_callback(app->device_list, device_list_draw);
    view_set_input_callback(app->device_list, device_list_input);
    view_set_previous_callback(app->device_list, nav_main_menu);
    view_dispatcher_add_view(app->view_dispatcher, ViewDeviceList, app->device_list);

    app->detail = widget_alloc();
    view_set_previous_callback(widget_get_view(app->detail), nav_device_list);
    view_dispatcher_add_view(app->view_dispatcher, ViewDetail, widget_get_view(app->detail));

    app->name_input = text_input_alloc();
    text_input_set_header_text(app->name_input, "Name this favorite");
    view_set_previous_callback(text_input_get_view(app->name_input), nav_detail);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewNameInput, text_input_get_view(app->name_input));

    app->alerts = variable_item_list_alloc();
    view_set_previous_callback(variable_item_list_get_view(app->alerts), nav_detail);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewAlerts, variable_item_list_get_view(app->alerts));

    app->settings = variable_item_list_alloc();
    VariableItem* item =
        variable_item_list_add(app->settings, "Favorite alerts", 2, beep_changed, app);
    variable_item_set_current_value_index(item, app->beep_enabled);
    variable_item_set_current_value_text(item, on_off_text[app->beep_enabled]);
    view_set_previous_callback(variable_item_list_get_view(app->settings), nav_main_menu);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewSettings, variable_item_list_get_view(app->settings));

    app->about = widget_alloc();
    widget_add_text_scroll_element(
        app->about,
        0,
        0,
        128,
        64,
        "\e#FlipperNina\n"
        "WiFi and BLE companion for\n"
        "Arduino Nano RP2040 Connect.\n\n"
        "Connect the Nano via UART\n"
        "(TX/RX/GND) and 5V on GPIO.");
    view_set_previous_callback(widget_get_view(app->about), nav_main_menu);
    view_dispatcher_add_view(app->view_dispatcher, ViewAbout, widget_get_view(app->about));

    app->refresh_timer = furi_timer_alloc(refresh_timer_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(app->refresh_timer, furi_ms_to_ticks(REFRESH_MS));

    view_dispatcher_switch_to_view(app->view_dispatcher, ViewMainMenu);
    return app;
}

static void app_free(FlipperNinaApp* app) {
    furi_timer_stop(app->refresh_timer);
    furi_timer_free(app->refresh_timer);

    if(app->serial) {
        furi_hal_serial_async_rx_stop(app->serial);
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
    }
    expansion_enable(app->expansion);
    furi_record_close(RECORD_EXPANSION);

    furi_thread_flags_set(furi_thread_get_id(app->worker), WorkerEvtStop);
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);
    furi_stream_buffer_free(app->rx_stream);

    if(app->otg_enabled_by_app) furi_hal_power_disable_otg();

    view_dispatcher_remove_view(app->view_dispatcher, ViewMainMenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewDeviceList);
    view_dispatcher_remove_view(app->view_dispatcher, ViewDetail);
    view_dispatcher_remove_view(app->view_dispatcher, ViewNameInput);
    view_dispatcher_remove_view(app->view_dispatcher, ViewAlerts);
    view_dispatcher_remove_view(app->view_dispatcher, ViewSettings);
    view_dispatcher_remove_view(app->view_dispatcher, ViewAbout);

    submenu_free(app->main_menu);
    view_free(app->device_list);
    widget_free(app->detail);
    text_input_free(app->name_input);
    variable_item_list_free(app->alerts);
    variable_item_list_free(app->settings);
    widget_free(app->about);
    view_dispatcher_free(app->view_dispatcher);
    furi_mutex_free(app->mutex);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t flipper_nina_app(void* p) {
    UNUSED(p);
    FlipperNinaApp* app = app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    app_free(app);
    return 0;
}
