/**
 * Barcelona watch face.
 *
 * About:
 * Watch face looks similar to Barcelona's crest.
 * > Barça colours creeps up to indicate the minutes. Hours displayed in numerals.
 * > A little 'P' in the top right corner indicates it's PM.
 * > Battery-level indicator:
 *   > white card outline: charging,
 *   > yellow card: not more than 20% charge remaining,
 *   > red card: not more than 10% charge remaining.
 * > Bluetooth connection indicator:
 *   > Orange Barça ball: connected,
 *   > Grey Barça ball: disconnected.
 **/
#include <pebble.h>
#include "nums.h"

//DEBUG flags
//#define DISABLE_CONFIG

//Specify this flag to debug time display updates
//#define DEBUG_MODE 1

//Specify this flag to debug 12h style
//#define DEBUG_12H

//Specify this flag to debug bluetooth indicator
//#define DEBUG_BT

//Specify this flag to debug battery indicator
//#define DEBUG_BATT

//min height for mins at :00 min
#define MARGIN 3

//hour digit's thickness (stroke width)
#define THICKNESS 15
#define WIDTH_CAPBAND (THICKNESS + 4)
#define THICKNESS_CAPBAND (THICKNESS * 4 / 5)
#define WIDTH_TEETH (THICKNESS*9/10)

//masks for vibes:
#define MASKV_BTDC   0x20000
#define MASKV_HOURLY 0x10000
#define MASKV_FROM   0xFF00
#define MASKV_TO     0x00FF
//def: disabled, 10am to 8pm
#define DEF_VIBES    0x0A14


Window *my_window;
Layer *lay1Top, //cross flag & yellow-red stripes
    *lay2Btm,   //blue-purple stripes
    *layCard,  //battery level indication: red: <= 10%, yellow <= 20%
    *layLogo,   //signature item in crest: orange ball
    *layHourTens, *layHourOnes, //hour digits
    *layPm;     //pm indicator
static BitmapLayer *layBmLogo;   //signature item in crest: orange ball
int h = 0, m = 0, hrTens = 0, hrOnes = 0;
static GBitmap *m_spbmItem = NULL,
    *m_spbmItemDim = NULL,
    *m_spbmBallon = NULL;
static TextLayer *m_slaytxtPm = NULL;
static GFont m_sFontHour = NULL;
static char m_pchPm[] = "P";
bool bIs24hStyle = false;

static GPath *m_spathCard = NULL;
static const GPathInfo CARD_PATH_INFO = {
  .num_points = 4,
  .points = (GPoint []) {{0, 0}, {9, 0}, {9, 15}, {0, 15}}
};
static GPath *m_spathNums[NUM_NUMS];
//Captain's band:
static GPath *m_spathCapBand = NULL;
static const GPathInfo CAPBAND_PATH_INFO = {
  .num_points = 4,
  .points = (GPoint []) {{0, 0}, {WIDTH_CAPBAND, 0}, {WIDTH_CAPBAND, THICKNESS_CAPBAND}, {0, THICKNESS_CAPBAND}}
};
//Golden teeth:
static GPath *m_spathTeeth = NULL;
static const GPathInfo TEETH_PATH_INFO = {
  .num_points = 15,
  .points = (GPoint []) {
      {0, 0}, {0, THICKNESS-4}, {2, THICKNESS-2}, {4, THICKNESS}, {WIDTH_TEETH-4, THICKNESS}, {WIDTH_TEETH-2, THICKNESS-2}, {WIDTH_TEETH, THICKNESS-4}, {WIDTH_TEETH, 0},
      {WIDTH_TEETH, THICKNESS-4}, {WIDTH_TEETH+2, THICKNESS-2}, {WIDTH_TEETH+4, THICKNESS-1}, {WIDTH_TEETH*2-4, THICKNESS-1}, {WIDTH_TEETH*2-2, THICKNESS-2}, {WIDTH_TEETH*2, THICKNESS-4}, {WIDTH_TEETH*2, 0}
  }
};

bool m_bIsAm = false;

static time_t m_nLastHourlyShake = 0;
static int m_nVibes = DEF_VIBES;
// Vibe pattern for loss of BT connection: ON for 400ms, OFF for 100ms, ON for 300ms, OFF 100ms, 100ms:
static const uint32_t const VIBE_SEG_BT_LOSS[] = { 400, 200, 200, 400, 100 };
static const VibePattern VIBE_PAT_BT_LOSS = {
  .durations = VIBE_SEG_BT_LOSS,
  .num_segments = ARRAY_LENGTH(VIBE_SEG_BT_LOSS),
};

//forward declaration
void display_time(struct tm* tick_time);


//
//Configuration stuff via AppMessage API
//
#define KEY_VIBES 0
#define KEY_ROW2  1
#define KEY_ROW4  2
#define KEY_TIMEZONE2 3

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
    // Get the first pair
    Tuple *t = dict_read_first(iterator);
    int nNewValue;
    bool bNeedUpdate = false;

    // Process all pairs present
    while(t != NULL) {
        // Process this pair's key
        //APP_LOG(APP_LOG_LEVEL_DEBUG, "Key:%d received with value:%d", (int)t->key, (int)t->value->int32);
        switch (t->key) {
            case KEY_VIBES:
                nNewValue = t->value->int32;
                if (m_nVibes != nNewValue)
                {
                    m_nVibes = nNewValue;
                }
                break;
        }

        // Get next pair, if any
        t = dict_read_next(iterator);
    }
    if (bNeedUpdate)
    {
        time_t now = time(NULL);
        struct tm *tick_time = localtime(&now);
        display_time(tick_time);
    }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped!");
}

static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send failed!");
}

static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
    APP_LOG(APP_LOG_LEVEL_INFO, "Outbox send success!");
}


//
//Bluetooth stuff
//
bool m_bBtConnected = false;
static void bt_handler(bool connected) {
    if (!connected && m_bBtConnected //vibrate once upon BT connection lost
        && (m_nVibes & MASKV_BTDC)) //only if option enabled
    {
        vibes_enqueue_custom_pattern(VIBE_PAT_BT_LOSS);
    }
    m_bBtConnected = connected;
    bitmap_layer_set_bitmap(layBmLogo,
        m_bBtConnected? (h != 10? m_spbmItem: m_spbmBallon): m_spbmItemDim);
}

//
// Battery stuff
//
BatteryChargeState m_sBattState = {
    0,      //charge_percent
    false,  //is_charging
    false   //is_plugged
};

static void battery_handler(BatteryChargeState new_state) {
    m_sBattState = new_state;
    layer_mark_dirty(layCard);
}

//
// Display stuff
//
void update1Top(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    int xMid = bounds.size.w / 2,
        wCross = xMid / 3,
        xCrossVertL = wCross,
        yCrossHorT = wCross;

    //1. white background
    GRect r = GRect(0, 0, bounds.size.w, bounds.size.h);
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, r, 0, GCornerNone);

    //2. cross
    //2a. red cross vertical part
    r = GRect(xCrossVertL, 0, wCross, bounds.size.h);
    graphics_context_set_stroke_color(ctx, GColorRed);
    graphics_context_set_fill_color(ctx, GColorRed);
    graphics_fill_rect(ctx, r, 0, GCornerNone);

    //2b. red cross horizontal part
    r = GRect(0, yCrossHorT, xMid, wCross);
    graphics_context_set_stroke_color(ctx, GColorRed);
    graphics_context_set_fill_color(ctx, GColorRed);
    graphics_fill_rect(ctx, r, 0, GCornerNone);

    //3. yellow-red stripes
    //3a. yellow background
    r = GRect(xMid, 0, xMid, bounds.size.h);
    graphics_context_set_stroke_color(ctx, GColorYellow);
    graphics_context_set_fill_color(ctx, GColorYellow);
    graphics_fill_rect(ctx, r, 0, GCornerNone);

    //3b. 4x red stripes
    int wStripe = xMid / 9,
        x = wStripe;
    graphics_context_set_stroke_color(ctx, GColorRed); //GColorFolly
    graphics_context_set_fill_color(ctx, GColorRed);
    for (int i = 0; i < 4; ++i)
    {
        r = GRect(xMid + x, 0, wStripe, bounds.size.h);
        graphics_fill_rect(ctx, r, 0, GCornerNone);
        x += 2 * wStripe;
    }
}

void update2Btm(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);

    //compute top y position based on mins
    int yTop = (bounds.size.h * (60-m)) / 60 - MARGIN;
    //blue-purple stripes
    //a. blue background
    GRect r = GRect(0, yTop, bounds.size.w, bounds.size.h + MARGIN);
    graphics_context_set_stroke_color(ctx, GColorCobaltBlue);
    graphics_context_set_fill_color(ctx, GColorCobaltBlue);
    graphics_fill_rect(ctx, r, 0, GCornerNone);

    //b. 3x purple stripes
    int wStripe = bounds.size.w / 7,
        x = wStripe;
    graphics_context_set_stroke_color(ctx, GColorJazzberryJam);
    graphics_context_set_fill_color(ctx, GColorJazzberryJam);
    for (int i = 0; i < 3; ++i)
    {
        r = GRect(x, yTop, wStripe, bounds.size.h + MARGIN);
        graphics_fill_rect(ctx, r, 0, GCornerNone);
        x += 2 * wStripe;
    }
}

void updateLogo(Layer *layer, GContext *ctx) {
#ifdef DEBUG_BT
    if (m < 50)
    {
        bitmap_layer_set_bitmap(layBmLogo, m_spbmItem);
    }
    else
    {
        bitmap_layer_set_bitmap(layBmLogo, m_spbmItemDim);
    }
#endif //DEBUG_BT
}

void updateTens(Layer *layer, GContext *ctx)
{
    if (hrTens < 0) return;
    // Fill the path:
    //graphics_context_set_fill_color(ctx, GColorClear);
    //gpath_draw_filled(ctx, m_spathNums[hrTens]);
    // Stroke the path:
    graphics_context_set_stroke_width(ctx, THICKNESS);
    graphics_context_set_stroke_color(ctx, bIs24hStyle? GColorOxfordBlue: GColorBlack);
    bool bIs11 = (h == 11) || (!bIs24hStyle && (h == 23));
    //gpath_draw_outline(ctx, m_spathNums[hrTens]);
    gpath_draw_outline(ctx, !bIs11? m_spathNums[hrTens]: m_spathNums[10]);
}

void updateOnes(Layer *layer, GContext *ctx)
{
    bitmap_layer_set_bitmap(layBmLogo,
        m_bBtConnected? (h != 10? m_spbmItem: m_spbmBallon): m_spbmItemDim);
    // Fill the path:
    //graphics_context_set_fill_color(ctx, GColorBlack); //GColorClear);
    //gpath_draw_filled(ctx, m_spathNums[hrOnes]);
    // Stroke the path:
    graphics_context_set_stroke_width(ctx, THICKNESS);
    graphics_context_set_stroke_color(ctx, GColorBlack); //GColorBlue);
    bool bIs11 = (h == 11) || (!bIs24hStyle && (h == 23));
    gpath_draw_outline(ctx, !bIs11? m_spathNums[hrOnes]: m_spathNums[10]);
    if (h == 8) //Captain
    {
        graphics_context_set_stroke_width(ctx, 1);
        graphics_context_set_stroke_color(ctx, GColorPurple);
        gpath_draw_outline(ctx, m_spathCapBand);
        graphics_context_set_fill_color(ctx, GColorJazzberryJam);
        gpath_draw_filled(ctx, m_spathCapBand);
    }
    else if (h == 9) //MSN S
    {
        graphics_context_set_stroke_width(ctx, 1);
        graphics_context_set_stroke_color(ctx, GColorBlack); //GColorDarkGray); //GColorWhite);
        gpath_draw_outline_open(ctx, m_spathTeeth);
        graphics_context_set_fill_color(ctx, GColorIcterine); //GColorChromeYellow); //GColorYellow);
        gpath_draw_filled(ctx, m_spathTeeth);
    }
}

void updateCard(Layer *layer, GContext *ctx)
{
    if (m_sBattState.charge_percent > 20)
    {
        return;
    }
    // Fill the path:
    graphics_context_set_fill_color(ctx, m_sBattState.charge_percent <= 10? GColorRed: GColorYellow);
    gpath_draw_filled(ctx, m_spathCard);
    // Stroke the path:
    //graphics_context_set_stroke_width(ctx, 2);
    graphics_context_set_stroke_color(ctx, m_sBattState.is_charging? GColorWhite: GColorBlack);
    gpath_draw_outline(ctx, m_spathCard);
}

/**
 * Convert string to uppercase.
 * @param a_pchStr string to be converted.
 * @param a_nMaxLen size of string.
 **/
void toUpperCase(char *a_pchStr, int a_nMaxLen)
{
    for (int i = 0; (i < a_nMaxLen) && (a_pchStr[i] != 0); ++i)
    {
        if ((a_pchStr[i] >= 'a') && (a_pchStr[i] <= 'z'))
        {
            a_pchStr[i] -= 32;
        }
    }
}

void display_time(struct tm* tick_time) {
    h = tick_time->tm_hour;
    m = tick_time->tm_min;
    //APP_LOG(APP_LOG_LEVEL_DEBUG, "Time is %d:%d:%d", h, m, tick_time->tm_sec);
#ifdef DEBUG_MODE
    //h = m % 24;
    h = 11;
    //m = 30;
    //h = tick_time->tm_sec % 24;
    //h = tick_time->tm_sec % 72 / 3; //for testing tougue-in-cheeks
    //h = ((tick_time->tm_min * 60) + tick_time->tm_sec) % 72 / 3; //for testing tougue-in-cheeks
    m = tick_time->tm_sec;
#endif

#ifdef DEBUG_12H
    bIs24hStyle = false;
#else
    bIs24hStyle = clock_is_24h_style();
#endif //DEBUG_12H
    m_bIsAm = bIs24hStyle //don't show PM indicator in 24h style
        || (h < 12);

    // hour text
    if (bIs24hStyle)
    {
        // Use 24 hour format
        hrTens = h / 10;
        hrOnes = h % 10;
    }
    else //12h style
    {
        int hours = h;
        if (hours > 12)
        {
            hours -= 12;
        }
        // Use 12 hour format
        if (hours >= 10)
        {
            hrTens = 1;
            hrOnes = hours % 10;
        }
        else if (hours > 0)
        {
            //no leading "0""
            hrTens = -1;
            hrOnes = hours;
        }
        else
        {
            hrTens = 1;
            hrOnes = 2;
        }
    }
    // Display this time on the TextLayer
    text_layer_set_text(m_slaytxtPm, m_bIsAm? NULL: m_pchPm);

    time_t now = time(NULL); //seconds since epoch
    now -= now % 60; //remove the seconds component
    if ((m_nVibes & MASKV_HOURLY) //option enabled to vibrate hourly
        && (m == 0) //hourly mark reached
        && (m_nLastHourlyShake != now)) //shake only once per hour (e.g. don't shake again upon flick)!
    {
        int from = (m_nVibes & MASKV_FROM) >> 8,
            to = m_nVibes & MASKV_TO;
        bool bShake = false;
        if (from <= to)
        {
            bShake = (h >= from) && (h <= to);
        }
        else
        {
            bShake = (h >= from) || (h <= to);
        }
        if (bShake)
        {
            m_nLastHourlyShake = now;
            vibes_double_pulse();
        }
    }



    layer_mark_dirty(window_get_root_layer(my_window));
    //layer_mark_dirty(lay1Top);
    //layer_mark_dirty(lay2Btm);

#ifdef DEBUG_BATT
    m_sBattState.charge_percent = 8;
    //m_sBattState.is_charging = true;
    battery_handler(m_sBattState); //layer_mark_dirty(layCard);
#endif //DEBUG_BATT
}

void handle_minute_tick(struct tm* tick_time, TimeUnits units_changed) {
    display_time(tick_time);
}

void readConfig()
{
    if (persist_exists(KEY_VIBES))
    {
        m_nVibes = persist_read_int(KEY_VIBES);
    }
}

void saveConfig()
{
    persist_write_int(KEY_VIBES, m_nVibes);
}

void resetDisplay()
{
    time_t now = time(NULL);
    struct tm *tick_time = localtime(&now);
    display_time(tick_time);
}

static void tap_handler(AccelAxisType axis, int32_t direction)
{
    //APP_LOG(APP_LOG_LEVEL_DEBUG, "Reseting display!");
    resetDisplay();
}

//
//Window setup sutff
//
void handle_init(void)
{
#ifndef DISABLE_CONFIG
    readConfig();
    // Register callbacks
    app_message_register_inbox_received(inbox_received_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_register_outbox_failed(outbox_failed_callback);
    app_message_register_outbox_sent(outbox_sent_callback);
    // Open AppMessage
    app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());
#endif

    my_window = window_create();
    window_stack_push(my_window, true);
    window_set_background_color(my_window, GColorBlack);

    //load numbers
    GPoint shift = GPoint(THICKNESS, THICKNESS);
    for (int i = 0; i < NUM_NUMS; ++i)
    {
        m_spathNums[i] = gpath_create(&(NUMS_PATH_INFO[i]));
        // Translate:
        gpath_move_to(m_spathNums[i], shift);
    }

    Layer *root_layer = window_get_root_layer(my_window);
    GRect frame = layer_get_frame(root_layer);

    lay1Top = layer_create(frame);
    layer_set_update_proc(lay1Top, update1Top);
    layer_add_child(root_layer, lay1Top);

    lay2Btm = layer_create(frame);
    layer_set_update_proc(lay2Btm, update2Btm);
    layer_add_child(root_layer, lay2Btm);

    //Create card
    m_spathCard = gpath_create(&CARD_PATH_INFO);
    // Rotate 30 degrees clockwise:
    gpath_rotate_to(m_spathCard, TRIG_MAX_ANGLE / 360 * 30);
    // Translate to top right quadrant of ball:
    gpath_move_to(m_spathCard, GPoint(126, 105));
    //Create Captain's Armband
    m_spathCapBand = gpath_create(&CAPBAND_PATH_INFO);
    // Rotate 26 degrees anti-clockwise:
    gpath_rotate_to(m_spathCapBand, TRIG_MAX_ANGLE / 360 * -26);
    // Translate to right curve ('shoulder') of number '8':
    gpath_move_to(m_spathCapBand, GPoint(81, 78));
    //Create Golden teeth
    m_spathTeeth = gpath_create(&TEETH_PATH_INFO);
    // Rotate 26 degrees clockwise:
    gpath_rotate_to(m_spathTeeth, TRIG_MAX_ANGLE / 360 * 26);
    // Translate to lower left curve ('mouth') of number '9':
    gpath_move_to(m_spathTeeth, GPoint(26, 85));

    layCard = layer_create(frame);
    layer_set_update_proc(layCard, updateCard);
    layer_add_child(root_layer, layCard);

    layLogo = layer_create(GRect(102, 111, 50, 50));
    layBmLogo = bitmap_layer_create(layer_get_bounds(layLogo));
    layer_set_update_proc(layLogo, updateLogo);
    layer_add_child(root_layer, layLogo);
    m_spbmItem = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_ITEM);
    m_spbmItemDim = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_ITEM_DIM);
    m_spbmBallon = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_BALLON);
    bitmap_layer_set_bitmap(layBmLogo, m_spbmItem);
    //bitmap_layer_set_background_color(m_spbmLayerBg, GColorClear);
    bitmap_layer_set_compositing_mode(layBmLogo, GCompOpSet);
    layer_add_child(layLogo, bitmap_layer_get_layer(layBmLogo));

    layHourTens = layer_create(
        GRect(-50, 6, frame.size.w, frame.size.h));
    layer_set_update_proc(layHourTens, updateTens);
    layer_add_child(root_layer, layHourTens);
    layHourOnes = layer_create(
        GRect(10, 6, frame.size.w, frame.size.h));
    layer_set_update_proc(layHourOnes, updateOnes);
    layer_add_child(root_layer, layHourOnes);

    layPm = layer_create(frame);
    layer_add_child(root_layer, layPm);
    m_slaytxtPm = text_layer_create(
        GRect(frame.size.w * 4 / 5, frame.size.h / 9, 20, 20));
    text_layer_set_background_color(m_slaytxtPm, GColorClear);
    text_layer_set_text_color(m_slaytxtPm, GColorBlack);
    text_layer_set_font(m_slaytxtPm, fonts_get_system_font(
        FONT_KEY_ROBOTO_CONDENSED_21));
    text_layer_set_text_alignment(m_slaytxtPm, GTextAlignmentCenter);
    layer_add_child(layPm, text_layer_get_layer(m_slaytxtPm));

    //Example: view live log using "pebble logs --phone=192.168.1.X" command
    //APP_LOG(APP_LOG_LEVEL_DEBUG, "parameter: %d", value);

    time_t now = time(NULL);
    struct tm *tick_time = localtime(&now);
    display_time(tick_time);

#ifdef DEBUG_MODE
    tick_timer_service_subscribe(SECOND_UNIT, &handle_minute_tick);
#else
    tick_timer_service_subscribe(MINUTE_UNIT, &handle_minute_tick);
#endif

    // Subscribe to Bluetooth updates
    bluetooth_connection_service_subscribe(bt_handler);
    // Show current connection state
    bt_handler(bluetooth_connection_service_peek());

    // Subscribe to the Battery State Service
    battery_state_service_subscribe(battery_handler);
    // Get the current battery level
    battery_handler(battery_state_service_peek());

    // Subscribe to the Tap Service
    accel_tap_service_subscribe(tap_handler);
}

void handle_deinit(void)
{
    accel_tap_service_unsubscribe();
    bluetooth_connection_service_unsubscribe();
    battery_state_service_unsubscribe();
    tick_timer_service_unsubscribe();

#ifndef DISABLE_CONFIG
    app_message_deregister_callbacks();
    saveConfig();
#endif

    text_layer_destroy(m_slaytxtPm);
    layer_destroy(layPm);
    layer_destroy(layHourOnes);
    layer_destroy(layHourTens);
    layer_destroy(layLogo);
    layer_destroy(lay2Btm);
    layer_destroy(lay1Top);
    gbitmap_destroy(m_spbmBallon);
    gbitmap_destroy(m_spbmItemDim);
    gbitmap_destroy(m_spbmItem);
    bitmap_layer_destroy(layBmLogo);
    for (int i = 0; i < NUM_NUMS; ++i)
    {
        gpath_destroy(m_spathNums[i]);
    }
    gpath_destroy(m_spathTeeth);
    gpath_destroy(m_spathCapBand);
    gpath_destroy(m_spathCard);
    fonts_unload_custom_font(m_sFontHour);
    window_destroy(my_window);
}

int main(void) {
    handle_init();
    app_event_loop();
    handle_deinit();
}

