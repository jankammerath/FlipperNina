#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <gui/modules/variable_item_list.h>
#include <notification/notification_messages.h>

typedef enum {
    ViewMainMenu,
    ViewDeviceList,
    ViewDetail,
    ViewSettings,
    ViewAbout,
} AppView;

typedef enum {
    MenuBle,
    MenuWifi,
    MenuFavorites,
    MenuSettings,
    MenuAbout,
} MenuItem;

typedef enum {
    ListBle,
    ListWifi,
    ListFavorites,
} ListMode;

typedef enum {
    EventToggleFavorite,
} AppEvent;

typedef struct {
    bool is_wifi;
    const char* name;
    const char* address;
    int8_t rssi;
    // Vendor for BLE, security/channel for WiFi
    const char* info;
    bool favorite;
} MockDevice;

static MockDevice mock_devices[] = {
    {false, "iPhone", "4C:12:9A:33:F0:01", -48, "Apple", true},
    {false, "Galaxy Buds2", "D8:3A:11:B2:7C:44", -61, "Samsung", false},
    {false, "(no name)", "6E:01:5F:A9:22:9B", -77, "Microsoft", false},
    {false, "fenix 7", "C4:7F:20:0E:51:D3", -70, "Garmin", false},
    {false, "Echo Dot", "F0:81:73:4A:CC:10", -83, "Amazon", false},
    {true, "HomeNet", "A0:63:91:5E:02:7A", -42, "WPA2  ch 6", true},
    {true, "FRITZ!Box 7590", "3C:A6:2F:19:88:E1", -58, "WPA2  ch 11", false},
    {true, "Guest", "3E:A6:2F:19:88:E2", -66, "Open  ch 1", false},
    {true, "DIRECT-HP-Printer", "FA:DA:0C:73:B4:05", -80, "WPA2  ch 6", false},
};

#define MOCK_DEVICE_COUNT COUNT_OF(mock_devices)
#define NO_ITEM           UINT32_MAX

static const char* const on_off_text[] = {"Off", "On"};
static const char* const interval_text[] = {"2s", "4s", "8s"};

typedef struct {
    Gui* gui;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;
    Submenu* main_menu;
    Submenu* device_list;
    Widget* detail;
    VariableItemList* settings;
    Widget* about;
    ListMode list_mode;
    uint32_t selected;
    uint8_t beep_enabled;
    uint8_t interval_index;
} FlipperNinaApp;

static bool list_matches(ListMode mode, const MockDevice* dev) {
    switch(mode) {
    case ListBle:
        return !dev->is_wifi;
    case ListWifi:
        return dev->is_wifi;
    case ListFavorites:
        return dev->favorite;
    }
    return false;
}

static void device_list_callback(void* context, uint32_t index);

static void device_list_populate(FlipperNinaApp* app) {
    static const char* const headers[] = {"BLE Devices", "WiFi Networks", "Favorites"};
    submenu_reset(app->device_list);
    submenu_set_header(app->device_list, headers[app->list_mode]);

    char label[40];
    bool any = false;
    for(uint32_t i = 0; i < MOCK_DEVICE_COUNT; i++) {
        const MockDevice* dev = &mock_devices[i];
        if(!list_matches(app->list_mode, dev)) continue;
        snprintf(label, sizeof(label), "%s%d %s", dev->favorite ? "*" : " ", dev->rssi, dev->name);
        submenu_add_item(app->device_list, label, i, device_list_callback, app);
        any = true;
    }
    if(!any) submenu_add_item(app->device_list, "(empty)", NO_ITEM, NULL, NULL);
    submenu_set_selected_item(app->device_list, app->selected);
}

static void detail_button_callback(GuiButtonType result, InputType type, void* context) {
    FlipperNinaApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, EventToggleFavorite);
    }
}

static void detail_rebuild(FlipperNinaApp* app) {
    const MockDevice* dev = &mock_devices[app->selected];
    char line[48];

    widget_reset(app->detail);
    widget_add_string_element(app->detail, 0, 0, AlignLeft, AlignTop, FontPrimary, dev->name);

    snprintf(line, sizeof(line), "%s %s", dev->is_wifi ? "BSSID:" : "MAC:", dev->address);
    widget_add_string_element(app->detail, 0, 14, AlignLeft, AlignTop, FontSecondary, line);

    snprintf(line, sizeof(line), "RSSI: %d dBm", dev->rssi);
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

static void device_list_callback(void* context, uint32_t index) {
    FlipperNinaApp* app = context;
    if(index >= MOCK_DEVICE_COUNT) return;
    app->selected = index;
    detail_rebuild(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewDetail);
}

static void main_menu_callback(void* context, uint32_t index) {
    FlipperNinaApp* app = context;
    switch(index) {
    case MenuBle:
    case MenuWifi:
    case MenuFavorites:
        app->list_mode = (ListMode)index;
        app->selected = 0;
        device_list_populate(app);
        if(app->list_mode == ListBle && app->beep_enabled) {
            for(uint32_t i = 0; i < MOCK_DEVICE_COUNT; i++) {
                if(!mock_devices[i].is_wifi && mock_devices[i].favorite) {
                    notification_message(app->notifications, &sequence_success);
                    break;
                }
            }
        }
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
        device_list_populate(app);
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
    app->list_mode = ListBle;
    app->selected = 0;
    app->beep_enabled = 1;
    app->interval_index = 1;

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, custom_event_callback);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->main_menu = submenu_alloc();
    submenu_set_header(app->main_menu, "FlipperNina");
    submenu_add_item(app->main_menu, "BLE Devices", MenuBle, main_menu_callback, app);
    submenu_add_item(app->main_menu, "WiFi Networks", MenuWifi, main_menu_callback, app);
    submenu_add_item(app->main_menu, "Favorites", MenuFavorites, main_menu_callback, app);
    submenu_add_item(app->main_menu, "Settings", MenuSettings, main_menu_callback, app);
    submenu_add_item(app->main_menu, "About", MenuAbout, main_menu_callback, app);
    view_set_previous_callback(submenu_get_view(app->main_menu), nav_exit);
    view_dispatcher_add_view(app->view_dispatcher, ViewMainMenu, submenu_get_view(app->main_menu));

    app->device_list = submenu_alloc();
    view_set_previous_callback(submenu_get_view(app->device_list), nav_main_menu);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewDeviceList, submenu_get_view(app->device_list));

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

    view_dispatcher_switch_to_view(app->view_dispatcher, ViewMainMenu);
    return app;
}

static void app_free(FlipperNinaApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, ViewMainMenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewDeviceList);
    view_dispatcher_remove_view(app->view_dispatcher, ViewDetail);
    view_dispatcher_remove_view(app->view_dispatcher, ViewSettings);
    view_dispatcher_remove_view(app->view_dispatcher, ViewAbout);

    submenu_free(app->main_menu);
    submenu_free(app->device_list);
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
