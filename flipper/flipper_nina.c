#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <gui/modules/variable_item_list.h>
#include <gui/elements.h>
#include <notification/notification_messages.h>
#include <stdlib.h>

typedef enum {
    ViewMainMenu,
    ViewDeviceList,
    ViewDetail,
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
    EventToggleFavorite,
    EventRefresh,
} AppEvent;

typedef struct {
    bool is_wifi;
    const char* name;
    const char* address;
    int8_t rssi;
    // Vendor for BLE, security/channel for WiFi
    const char* info;
    bool favorite;
    bool visible;
} MockDevice;

static MockDevice mock_devices[] = {
    {false, "iPhone", "4C:12:9A:33:F0:01", -48, "Apple", true, true},
    {false, "Galaxy Buds2", "D8:3A:11:B2:7C:44", -61, "Samsung", false, true},
    {false, "(no name)", "6E:01:5F:A9:22:9B", -77, "Microsoft", false, true},
    {false, "fenix 7", "C4:7F:20:0E:51:D3", -70, "Garmin", false, false},
    {false, "Echo Dot", "F0:81:73:4A:CC:10", -83, "Amazon", false, true},
    {true, "HomeNet", "A0:63:91:5E:02:7A", -42, "WPA2  ch 6", true, true},
    {true, "FRITZ!Box 7590", "3C:A6:2F:19:88:E1", -58, "WPA2  ch 11", false, true},
    {true, "Guest", "3E:A6:2F:19:88:E2", -66, "Open  ch 1", false, false},
    {true, "DIRECT-HP-Printer", "FA:DA:0C:73:B4:05", -80, "WPA2  ch 6", false, true},
};

#define MOCK_DEVICE_COUNT COUNT_OF(mock_devices)
#define LIST_ROWS         4
#define LIST_TOP          13
#define LIST_ROW_HEIGHT   12

// XBM, LSB = leftmost pixel
static const uint8_t icon_ble_5x9[] = {0x04, 0x0C, 0x15, 0x0E, 0x04, 0x0E, 0x15, 0x0C, 0x04};
static const uint8_t icon_wifi_7x7[] = {0x3E, 0x41, 0x00, 0x1C, 0x22, 0x00, 0x08};

static const char* const on_off_text[] = {"Off", "On"};
static const char* const interval_text[] = {"2s", "4s", "8s"};
static const uint32_t interval_ms[] = {2000, 4000, 8000};

typedef struct {
    uint8_t id;
    bool is_wifi;
    bool favorite;
    bool visible;
    int8_t rssi;
    const char* name;
} ListEntry;

typedef struct {
    ListMode mode;
    uint8_t count;
    uint8_t cursor;
    uint8_t offset;
    ListEntry entries[MOCK_DEVICE_COUNT];
} DeviceListModel;

typedef struct {
    Gui* gui;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;
    FuriTimer* refresh_timer;
    Submenu* main_menu;
    View* device_list;
    Widget* detail;
    VariableItemList* settings;
    Widget* about;
    ListMode list_mode;
    uint32_t selected;
    uint8_t beep_enabled;
    uint8_t interval_index;
    // Only turn 5V off on exit if this app turned it on
    bool otg_enabled_by_app;
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
    model->count = 0;
    for(uint8_t i = 0; i < MOCK_DEVICE_COUNT; i++) {
        const MockDevice* dev = &mock_devices[i];
        if(app->list_mode == ListFavorites ? !dev->favorite : !dev->visible) continue;

        ListEntry e;
        e.id = i;
        e.is_wifi = dev->is_wifi;
        e.favorite = dev->favorite;
        e.visible = dev->visible;
        e.rssi = dev->rssi;
        e.name = dev->name;

        uint8_t pos = model->count;
        while(pos > 0 && entry_rank(&model->entries[pos - 1]) < entry_rank(&e)) {
            model->entries[pos] = model->entries[pos - 1];
            pos--;
        }
        model->entries[pos] = e;
        model->count++;
    }

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
    canvas_draw_line(canvas, 0, 11, 127, 11);

    canvas_set_font(canvas, FontSecondary);
    if(model->count == 0) {
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, "No devices");
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
        } else {
            canvas_draw_xbm(canvas, 3, top + 2, 5, 9, icon_ble_5x9);
        }

        snprintf(buf, sizeof(buf), "%s%.16s", e->favorite ? "*" : "", e->name);
        canvas_draw_str(canvas, 12, top + 10, buf);

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
        detail_rebuild(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewDetail);
    }
    return true;
}

// Simulates a scan round; returns true if a favorite came into range.
static bool mock_scan_step(void) {
    bool favorite_appeared = false;
    for(uint8_t i = 0; i < MOCK_DEVICE_COUNT; i++) {
        MockDevice* dev = &mock_devices[i];
        if(rand() % 10 == 0) {
            dev->visible = !dev->visible;
            if(dev->visible) {
                dev->rssi = -90 + rand() % 10;
                if(dev->favorite) favorite_appeared = true;
            }
        } else if(dev->visible) {
            int rssi = dev->rssi + rand() % 7 - 3;
            dev->rssi = CLAMP(rssi, -35, -95);
        }
    }
    return favorite_appeared;
}

static void refresh_timer_callback(void* context) {
    FlipperNinaApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, EventRefresh);
}

static void detail_button_callback(GuiButtonType result, InputType type, void* context) {
    FlipperNinaApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, EventToggleFavorite);
    }
}

static void detail_rebuild(FlipperNinaApp* app) {
    if(app->selected >= MOCK_DEVICE_COUNT) return;
    const MockDevice* dev = &mock_devices[app->selected];
    char line[48];

    widget_reset(app->detail);
    widget_add_string_element(app->detail, 0, 0, AlignLeft, AlignTop, FontPrimary, dev->name);

    snprintf(line, sizeof(line), "%s %s", dev->is_wifi ? "BSSID:" : "MAC:", dev->address);
    widget_add_string_element(app->detail, 0, 14, AlignLeft, AlignTop, FontSecondary, line);

    if(dev->visible) {
        snprintf(line, sizeof(line), "RSSI: %d dBm", dev->rssi);
    } else {
        snprintf(line, sizeof(line), "RSSI: out of range");
    }
    widget_add_string_element(app->detail, 0, 25, AlignLeft, AlignTop, FontSecondary, line);

    snprintf(line, sizeof(line), "%s %s", dev->is_wifi ? "Sec:" : "Vendor:", dev->info);
    widget_add_string_element(app->detail, 0, 36, AlignLeft, AlignTop, FontSecondary, line);

    widget_add_button_element(
        app->detail,
        GuiButtonTypeCenter,
        dev->favorite ? "Unfav" : "Fav",
        detail_button_callback,
        app);
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

static bool custom_event_callback(void* context, uint32_t event) {
    FlipperNinaApp* app = context;
    if(event == EventToggleFavorite) {
        mock_devices[app->selected].favorite = !mock_devices[app->selected].favorite;
        detail_rebuild(app);
        device_list_refresh(app);
        return true;
    }
    if(event == EventRefresh) {
        if(mock_scan_step() && app->beep_enabled) {
            notification_message(app->notifications, &sequence_success);
        }
        device_list_refresh(app);
        detail_rebuild(app);
        return true;
    }
    return false;
}

static void beep_changed(VariableItem* item) {
    FlipperNinaApp* app = variable_item_get_context(item);
    app->beep_enabled = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, on_off_text[app->beep_enabled]);
}

static void interval_changed(VariableItem* item) {
    FlipperNinaApp* app = variable_item_get_context(item);
    app->interval_index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, interval_text[app->interval_index]);
    furi_timer_start(app->refresh_timer, furi_ms_to_ticks(interval_ms[app->interval_index]));
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

static FlipperNinaApp* app_alloc(void) {
    FlipperNinaApp* app = malloc(sizeof(FlipperNinaApp));
    app->list_mode = ListNearby;
    app->selected = 0;
    app->beep_enabled = 1;
    app->interval_index = 1;

    app->otg_enabled_by_app = !furi_hal_power_is_otg_enabled();
    if(app->otg_enabled_by_app) furi_hal_power_enable_otg();

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

    app->settings = variable_item_list_alloc();
    VariableItem* item =
        variable_item_list_add(app->settings, "Beep on favorite", 2, beep_changed, app);
    variable_item_set_current_value_index(item, app->beep_enabled);
    variable_item_set_current_value_text(item, on_off_text[app->beep_enabled]);
    item = variable_item_list_add(
        app->settings, "Scan interval", COUNT_OF(interval_text), interval_changed, app);
    variable_item_set_current_value_index(item, app->interval_index);
    variable_item_set_current_value_text(item, interval_text[app->interval_index]);
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
        "UI mockup - data is fake.\n"
        "Connect the Nano via UART\n"
        "(TX/RX/GND) on GPIO.");
    view_set_previous_callback(widget_get_view(app->about), nav_main_menu);
    view_dispatcher_add_view(app->view_dispatcher, ViewAbout, widget_get_view(app->about));

    app->refresh_timer = furi_timer_alloc(refresh_timer_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(app->refresh_timer, furi_ms_to_ticks(interval_ms[app->interval_index]));

    view_dispatcher_switch_to_view(app->view_dispatcher, ViewMainMenu);
    return app;
}

static void app_free(FlipperNinaApp* app) {
    furi_timer_stop(app->refresh_timer);
    if(app->otg_enabled_by_app) furi_hal_power_disable_otg();
    furi_timer_free(app->refresh_timer);

    view_dispatcher_remove_view(app->view_dispatcher, ViewMainMenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewDeviceList);
    view_dispatcher_remove_view(app->view_dispatcher, ViewDetail);
    view_dispatcher_remove_view(app->view_dispatcher, ViewSettings);
    view_dispatcher_remove_view(app->view_dispatcher, ViewAbout);

    submenu_free(app->main_menu);
    view_free(app->device_list);
    widget_free(app->detail);
    variable_item_list_free(app->settings);
    widget_free(app->about);
    view_dispatcher_free(app->view_dispatcher);

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
