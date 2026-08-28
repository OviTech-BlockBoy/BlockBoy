#include "rg_system.h"
#include "rg_bluetooth.h"

#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#if defined(ESP_PLATFORM) && defined(CONFIG_BT_NIMBLE_ENABLED)

// BLE HID (HOGP) host on raw NimBLE. We deliberately do NOT use esp_hidh:
// its nimble backend performs service discovery synchronously with waits
// that have no timeout and no pairing support, which deadlocks on cheap
// controllers (verified with the ShanWan Q36). This client is fully
// callback-driven with a watchdog on every step, tries to work without
// encryption first, and only pairs when the device demands it.

#include <nvs_flash.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/ble_hs_adv.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_store.h>
#include <os/os_mbuf.h>

// Provided by the NimBLE port when CONFIG_BT_NIMBLE_NVS_PERSIST is set
void ble_store_config_init(void);

#define BT_HID_SERVICE_UUID  0x1812
#define BT_UUID_REPORT       0x2A4D
#define BT_UUID_REPORT_MAP   0x2A4B
#define BT_UUID_CCCD         0x2902
#define BT_UUID_REPORT_REF   0x2908
#define BT_APPEARANCE_HID    0x03C0 // 0x03C0-0x03CF = HID category

// ---------------------------------------------------------------------------
// HID report descriptor parsing. We extract, per input report, the bit
// locations of things a gamepad can have: buttons, X/Y axes and the hat
// switch. That is enough to drive rg_input regardless of the exact layout
// a given controller uses.
// ---------------------------------------------------------------------------

typedef struct
{
    uint16_t offset; // in bits, relative to start of report payload
    uint8_t size;    // in bits
    int32_t min, max;
    bool present;
} hid_axis_t;

typedef struct
{
    uint16_t offset;    // in bits
    uint8_t count;      // number of buttons in this group
    uint8_t usage_min;  // usage of the first button (1 = BTN_SOUTH/A)
} hid_btn_group_t;

#define MAX_INPUT_REPORTS 6
#define MAX_BTN_GROUPS    4

typedef struct
{
    uint8_t report_id;
    bool used;
    uint16_t bit_cursor;
    hid_axis_t x, y, hat;
    hid_axis_t consumer; // Consumer-page value field (Home button on many pads)
    hid_btn_group_t buttons[MAX_BTN_GROUPS];
    uint8_t num_btn_groups;
} hid_report_t;

static hid_report_t input_reports[MAX_INPUT_REPORTS];

// Runtime state
static bool bt_initialized = false;
static bool bt_task_running = false;
static bool bt_task_exited = true;
static rg_bt_state_t bt_state = RG_BT_STATE_OFF;
static uint32_t bt_gamepad_state = 0; // _Atomic
static char bt_dev_name[32] = "";
static ble_addr_t bt_found_addr;
static volatile bool bt_found = false;
static volatile bool bt_scanning = false;
static int64_t scan_started_at = 0;
static int64_t search_started_at = 0; // whole search (scan + direct attempts)
static int64_t next_direct_at = 0;    // when to (re)try a direct connect
static int64_t idle_until = 0;        // radio parked until this time (retry mode)
static int64_t search_budget = 0;     // length of the current search window

// Give up scanning after this long with no controller found
// Total search budget (scanning + direct-connect attempts). 60s matches the
// reconnect window of pads like the Q36, which sleep after ~1 minute.
#define RG_BT_SEARCH_TIMEOUT_US (60 * 1000000LL)
// While searching with a known bond: retry a direct connect this often
#define RG_BT_DIRECT_RETRY_US (8 * 1000000LL)

// After the first search window comes up empty we do NOT stop looking, as long
// as a bond exists: the pad is then simply switched off or asleep, and the user
// expects it to just work when they wake it. Stopping for good meant the only
// way back was the Bluetooth menu -- most visible right after launching an
// emulator, since every app switch reboots the chip and restarts the search.
// Retrying at a low duty cycle keeps the radio (and the battery) mostly idle.
#define RG_BT_RETRY_IDLE_US (30 * 1000000LL)   // radio off between attempts
#define RG_BT_RETRY_SEARCH_US (10 * 1000000LL) // length of each retry window

// Connection / discovery state (all accessed from the NimBLE host task,
// except the watchdog reads in bt_task)
#define MAX_REPORT_CHRS 6
typedef struct
{
    uint16_t def_handle, val_handle;
    uint16_t cccd_handle, ref_handle;
    uint8_t report_id, report_type; // Report Reference (0x2908): type 1 = input
} report_chr_t;

enum
{
    STEP_IDLE = 0,   // Nothing in flight; bt_task may scan/connect
    STEP_CONNECT,    // ble_gap_connect issued
    STEP_SVC,        // HID service discovery
    STEP_CHR,        // Characteristic discovery
    STEP_DSC,        // Descriptor discovery (per report characteristic)
    STEP_REFS,       // Reading Report Reference descriptors
    STEP_MAP,        // Reading the report map
    STEP_SUBSCRIBE,  // Writing CCCDs
    STEP_READY,      // Input reports flowing
};

static volatile int bt_step = STEP_IDLE;
static volatile int64_t step_deadline = 0;
static uint16_t bt_conn = BLE_HS_CONN_HANDLE_NONE;
static report_chr_t report_chrs[MAX_REPORT_CHRS];
static int num_report_chrs = 0;
static int walk_idx = 0;
static uint16_t hid_svc_start = 0, hid_svc_end = 0, report_map_handle = 0;
static uint8_t report_map_buf[512];
static uint16_t report_map_len = 0;
static bool security_tried = false;

static hid_report_t *get_input_report(uint8_t report_id, bool create)
{
    for (int i = 0; i < MAX_INPUT_REPORTS; ++i)
        if (input_reports[i].used && input_reports[i].report_id == report_id)
            return &input_reports[i];
    if (!create)
        return NULL;
    for (int i = 0; i < MAX_INPUT_REPORTS; ++i)
    {
        if (!input_reports[i].used)
        {
            input_reports[i] = (hid_report_t){.report_id = report_id, .used = true};
            return &input_reports[i];
        }
    }
    return NULL;
}

static int32_t sign_extend(uint32_t value, uint8_t bits)
{
    if (bits == 0 || bits >= 32)
        return (int32_t)value;
    uint32_t sign_bit = 1UL << (bits - 1);
    return (int32_t)((value ^ sign_bit) - sign_bit);
}

static void parse_report_map(const uint8_t *map, size_t len)
{
    memset(input_reports, 0, sizeof(input_reports));

    uint16_t usage_page = 0;
    int32_t logical_min = 0, logical_max = 0;
    uint8_t report_size = 0, report_count = 0;
    uint8_t report_id = 0;
    uint16_t usages[16];
    int num_usages = 0;
    uint32_t usage_min = 0, usage_max = 0;
    bool have_usage_range = false;

    for (size_t pos = 0; pos < len;)
    {
        uint8_t prefix = map[pos++];
        if (prefix == 0xFE) // Long item: skip
        {
            if (pos >= len) break;
            uint8_t data_len = map[pos];
            pos += 2 + data_len;
            continue;
        }
        uint8_t data_size = prefix & 0x03;
        if (data_size == 3) data_size = 4;
        uint8_t type = (prefix >> 2) & 0x03; // 0=main, 1=global, 2=local
        uint8_t tag = prefix >> 4;
        if (pos + data_size > len)
            break;

        uint32_t udata = 0;
        for (int i = 0; i < data_size; ++i)
            udata |= (uint32_t)map[pos + i] << (8 * i);
        int32_t sdata = sign_extend(udata, data_size * 8);
        pos += data_size;

        if (type == 1) // Global
        {
            switch (tag)
            {
                case 0: usage_page = udata; break;
                case 1: logical_min = sdata; break;
                case 2: logical_max = (logical_min < 0) ? sdata : (int32_t)udata; break;
                case 7: report_size = udata; break;
                case 8: report_id = udata; break;
                case 9: report_count = udata; break;
            }
        }
        else if (type == 2) // Local
        {
            switch (tag)
            {
                case 0: // Usage
                    if (num_usages < (int)RG_COUNT(usages))
                        usages[num_usages++] = udata & 0xFFFF;
                    break;
                case 1: usage_min = udata; have_usage_range = true; break;
                case 2: usage_max = udata; break;
            }
        }
        else if (type == 0) // Main
        {
            if (tag == 8) // Input
            {
                hid_report_t *rep = get_input_report(report_id, true);
                if (rep)
                {
                    bool constant = udata & 1;
                    if (constant)
                    {
                        rep->bit_cursor += report_size * report_count;
                    }
                    else if (usage_page == 0x09) // Buttons
                    {
                        if (rep->num_btn_groups < MAX_BTN_GROUPS && report_size == 1)
                        {
                            rep->buttons[rep->num_btn_groups++] = (hid_btn_group_t){
                                .offset = rep->bit_cursor,
                                .count = report_count,
                                .usage_min = have_usage_range ? usage_min : (num_usages ? usages[0] : 1),
                            };
                        }
                        rep->bit_cursor += report_size * report_count;
                    }
                    else if (usage_page == 0x01) // Generic Desktop
                    {
                        for (int k = 0; k < report_count; ++k)
                        {
                            uint16_t usage = 0;
                            if (have_usage_range)
                                usage = usage_min + k;
                            else if (k < num_usages)
                                usage = usages[k];
                            else if (num_usages > 0)
                                usage = usages[num_usages - 1];
                            hid_axis_t axis = {
                                .offset = rep->bit_cursor,
                                .size = report_size,
                                .min = logical_min,
                                .max = logical_max,
                                .present = true,
                            };
                            if (usage == 0x30 && !rep->x.present) rep->x = axis;      // X
                            else if (usage == 0x31 && !rep->y.present) rep->y = axis; // Y
                            else if (usage == 0x39 && !rep->hat.present) rep->hat = axis; // Hat
                            rep->bit_cursor += report_size;
                        }
                    }
                    else if (usage_page == 0x0C) // Consumer page (Home lives here on many pads)
                    {
                        if (report_size == 1 && rep->num_btn_groups < MAX_BTN_GROUPS)
                        {
                            // Bitfield: expose as pseudo-buttons 14..16 (learnable via remap)
                            uint8_t count = report_count;
                            if (count > 3) count = 3;
                            rep->buttons[rep->num_btn_groups++] = (hid_btn_group_t){
                                .offset = rep->bit_cursor,
                                .count = count,
                                .usage_min = 14,
                            };
                        }
                        else if (!rep->consumer.present)
                        {
                            // Value field: any nonzero value acts as pseudo-button 16
                            rep->consumer = (hid_axis_t){
                                .offset = rep->bit_cursor,
                                .size = report_size,
                                .present = true,
                            };
                        }
                        rep->bit_cursor += report_size * report_count;
                    }
                    else
                    {
                        rep->bit_cursor += report_size * report_count;
                    }
                }
            }
            // Feature/Output items don't consume input report bits; nothing to do.
            num_usages = 0;
            usage_min = usage_max = 0;
            have_usage_range = false;
        }
    }

    for (int i = 0; i < MAX_INPUT_REPORTS; ++i)
    {
        hid_report_t *r = &input_reports[i];
        if (r->used)
            RG_LOGI("Input report id=%d: %d bits, x=%d y=%d hat=%d btn_groups=%d",
                    r->report_id, r->bit_cursor, r->x.present, r->y.present,
                    r->hat.present, r->num_btn_groups);
    }
}

static uint32_t get_bits(const uint8_t *data, size_t len, uint32_t offset, uint8_t size)
{
    uint32_t value = 0;
    for (uint8_t i = 0; i < size && i < 32; ++i)
    {
        uint32_t bit = offset + i;
        if (bit / 8 >= len)
            break;
        value |= (uint32_t)((data[bit / 8] >> (bit % 8)) & 1) << i;
    }
    return value;
}

// Button usage (1-based, HID button page) to retro-go key. Loaded from
// settings so it can be remapped from the Bluetooth menu; the default swaps
// A/B relative to the Xbox-style labels so it matches the Nintendo layout
// retro-go emulates (label "A"/south acts as B, label "B"/east acts as A).
static uint32_t btn_key_map[RG_BT_MAX_BUTTONS];
static volatile uint32_t bt_raw_buttons = 0; // bit N = usage N+1 held
static volatile bool bt_capture = false;

#define BT_SETTING_BTN_MAP "BTBtnMap"

static void load_default_btn_map(void)
{
    memset(btn_key_map, 0, sizeof(btn_key_map));
    btn_key_map[0]  = RG_KEY_B;      // Usage 1, BTN_SOUTH, labelled A
    btn_key_map[1]  = RG_KEY_A;      // Usage 2, BTN_EAST,  labelled B
    btn_key_map[3]  = RG_KEY_Y;      // Usage 4, BTN_NORTH, labelled X
    btn_key_map[4]  = RG_KEY_X;      // Usage 5, BTN_WEST,  labelled Y
    btn_key_map[6]  = RG_KEY_L;      // L1
    btn_key_map[7]  = RG_KEY_R;      // R1
    btn_key_map[8]  = RG_KEY_L;      // L2 (doubles as L; remappable)
    btn_key_map[9]  = RG_KEY_R;      // R2 (doubles as R; remappable)
    btn_key_map[10] = RG_KEY_SELECT;
    btn_key_map[11] = RG_KEY_START;
    // No MENU mapping: the menu opens with Start+Select (virtual map in
    // rg_input), Home behaves inconsistently across pads. Usage 13 and the
    // consumer pseudo-button 16 stay assignable via the remap dialog.
}

static void save_btn_map(void)
{
    char buf[RG_BT_MAX_BUTTONS * 7 + 1] = "";
    for (int i = 0; i < RG_BT_MAX_BUTTONS; ++i)
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%u", i ? "," : "", (unsigned)btn_key_map[i]);
    rg_settings_set_string(NS_GLOBAL, BT_SETTING_BTN_MAP, buf);
    rg_settings_commit();
}

static void load_btn_map(void)
{
    load_default_btn_map();
    char *str = rg_settings_get_string(NS_GLOBAL, BT_SETTING_BTN_MAP, NULL);
    if (!str)
        return;
    char *p = str;
    for (int i = 0; i < RG_BT_MAX_BUTTONS && p && *p; ++i)
    {
        btn_key_map[i] = strtoul(p, &p, 10);
        if (*p == ',')
            p++;
    }
    free(str);
    RG_LOGI("Loaded custom button map from settings");
}

static uint32_t map_button(uint8_t usage)
{
    if (usage >= 1 && usage <= RG_BT_MAX_BUTTONS)
        return btn_key_map[usage - 1];
    return 0;
}

// Fixed fallback layout for when the report map is unavailable or empty.
// This is the layout the ShanWan Q36 uses in "D" mode (byte 0: buttons,
// byte 1 bit 2: Home, byte 2: hat, bytes 3-4: left stick).
static uint32_t parse_fixed_layout(const uint8_t *data, size_t len)
{
    if (len < 3)
        return 0;
    // Map the fixed byte-0 bits onto the same usage numbers the Android
    // layout uses, so the remappable btn_key_map applies uniformly.
    static const uint8_t bit_to_usage[8] = {1, 2, 4, 5, 7, 8, 11, 12};
    uint32_t keys = 0, raw = 0;
    for (int bit = 0; bit < 8; ++bit)
    {
        if (data[0] & (1 << bit))
        {
            raw |= 1u << (bit_to_usage[bit] - 1);
            keys |= map_button(bit_to_usage[bit]);
        }
    }
    if (data[1] & (1 << 2)) // Home
    {
        raw |= 1u << (13 - 1);
        keys |= map_button(13);
    }
    bt_raw_buttons = raw;
    switch (data[2] & 0x0F)
    {
        case 0: keys |= RG_KEY_UP; break;
        case 1: keys |= RG_KEY_UP | RG_KEY_RIGHT; break;
        case 2: keys |= RG_KEY_RIGHT; break;
        case 3: keys |= RG_KEY_DOWN | RG_KEY_RIGHT; break;
        case 4: keys |= RG_KEY_DOWN; break;
        case 5: keys |= RG_KEY_DOWN | RG_KEY_LEFT; break;
        case 6: keys |= RG_KEY_LEFT; break;
        case 7: keys |= RG_KEY_UP | RG_KEY_LEFT; break;
        default: break;
    }
    if (len >= 5)
    {
        if (data[3] < 64) keys |= RG_KEY_LEFT;
        if (data[3] > 192) keys |= RG_KEY_RIGHT;
        if (data[4] < 64) keys |= RG_KEY_UP;
        if (data[4] > 192) keys |= RG_KEY_DOWN;
    }
    return keys;
}

static uint32_t parse_input_report(uint8_t report_id, const uint8_t *data, size_t len)
{
    hid_report_t *rep = get_input_report(report_id, false);
    if (!rep || (rep->num_btn_groups == 0 && !rep->hat.present && !rep->x.present))
        return parse_fixed_layout(data, len);

    uint32_t keys = 0, raw = 0;

    for (int g = 0; g < rep->num_btn_groups; ++g)
    {
        const hid_btn_group_t *grp = &rep->buttons[g];
        for (int i = 0; i < grp->count; ++i)
        {
            if (get_bits(data, len, grp->offset + i, 1))
            {
                uint32_t usage = grp->usage_min + i;
                if (usage >= 1 && usage <= RG_BT_MAX_BUTTONS)
                    raw |= 1u << (usage - 1);
                keys |= map_button(usage);
            }
        }
    }
    if (rep->consumer.present && get_bits(data, len, rep->consumer.offset, rep->consumer.size) != 0)
    {
        raw |= 1u << (16 - 1); // Pseudo-button 16
        keys |= map_button(16);
    }
    bt_raw_buttons = raw;

    if (rep->hat.present)
    {
        static const uint32_t hat_dirs[8] = {
            RG_KEY_UP, RG_KEY_UP | RG_KEY_RIGHT, RG_KEY_RIGHT, RG_KEY_DOWN | RG_KEY_RIGHT,
            RG_KEY_DOWN, RG_KEY_DOWN | RG_KEY_LEFT, RG_KEY_LEFT, RG_KEY_UP | RG_KEY_LEFT,
        };
        int32_t raw = (int32_t)get_bits(data, len, rep->hat.offset, rep->hat.size);
        if (rep->hat.min < 0)
            raw = sign_extend(raw, rep->hat.size);
        int32_t v = raw - rep->hat.min;
        if (v >= 0 && v < 8 && raw >= rep->hat.min && raw <= rep->hat.max)
            keys |= hat_dirs[v];
    }

    // Left stick doubles as dpad, with a 25% deadzone on each side of center
    const hid_axis_t *axes[2] = {&rep->x, &rep->y};
    const uint32_t neg_keys[2] = {RG_KEY_LEFT, RG_KEY_UP};
    const uint32_t pos_keys[2] = {RG_KEY_RIGHT, RG_KEY_DOWN};
    for (int i = 0; i < 2; ++i)
    {
        const hid_axis_t *a = axes[i];
        if (!a->present || a->max <= a->min)
            continue;
        int32_t raw = (int32_t)get_bits(data, len, a->offset, a->size);
        if (a->min < 0)
            raw = sign_extend(raw, a->size);
        int32_t span = a->max - a->min;
        if (raw < a->min + span / 4)
            keys |= neg_keys[i];
        else if (raw > a->max - span / 4)
            keys |= pos_keys[i];
    }

    return keys;
}

// ---------------------------------------------------------------------------
// GATT client: HID service discovery + subscription state machine.
// Every callback either advances the state machine or aborts the connection;
// bt_task watches step_deadline so nothing can hang forever.
// ---------------------------------------------------------------------------

static int gap_event_cb(struct ble_gap_event *event, void *arg);
static void subscribe_next(void);
static void read_next_ref(void);

static void set_step(int step)
{
    bt_step = step;
    step_deadline = rg_system_timer() + 10 * 1000000;
}

static void bt_abort_connection(const char *why)
{
    RG_LOGW("Aborting connection: %s", why);
    if (bt_conn != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(bt_conn, BLE_ERR_REM_USER_CONN_TERM);
    // Cleanup happens in the DISCONNECT event
}

static bool is_security_error(int status)
{
    return status == BLE_HS_ERR_ATT_BASE + 0x05 || // Insufficient authentication
           status == BLE_HS_ERR_ATT_BASE + 0x08 || // Insufficient authorization
           status == BLE_HS_ERR_ATT_BASE + 0x0F;   // Insufficient encryption
}

// Try to elevate security once when a GATT operation is refused. Returns true
// if pairing was initiated (caller should stop; ENC_CHANGE restarts discovery).
static bool maybe_elevate_security(int status)
{
    if (!is_security_error(status) || security_tried)
        return false;
    security_tried = true;
    RG_LOGI("Controller requires encryption, pairing...");
    set_step(STEP_CONNECT); // Repurposed as "waiting for encryption"
    int rc = ble_gap_security_initiate(bt_conn);
    if (rc != 0)
    {
        RG_LOGE("ble_gap_security_initiate failed: %d", rc);
        bt_abort_connection("pairing could not start");
    }
    return true;
}

static void finish_map_and_subscribe(void)
{
    memset(input_reports, 0, sizeof(input_reports));
    if (report_map_len > 0)
    {
        RG_LOGI("Report map: %d bytes", report_map_len);
        parse_report_map(report_map_buf, report_map_len);
    }
    else
    {
        RG_LOGW("No report map; using fixed Q36 layout");
    }
    walk_idx = 0;
    set_step(STEP_SUBSCRIBE);
    subscribe_next();
}

static int map_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0 && attr && attr->om)
    {
        uint16_t l = OS_MBUF_PKTLEN(attr->om);
        if (report_map_len + l <= sizeof(report_map_buf))
        {
            ble_hs_mbuf_to_flat(attr->om, report_map_buf + report_map_len, l, NULL);
            report_map_len += l;
        }
        return 0; // Keep reading
    }
    if (error->status != BLE_HS_EDONE)
    {
        if (maybe_elevate_security(error->status))
            return 0;
        RG_LOGW("Report map read failed (%d), using fallback layout", error->status);
        report_map_len = 0;
    }
    finish_map_and_subscribe();
    return 0;
}

static void start_map_read(void)
{
    report_map_len = 0;
    set_step(STEP_MAP);
    if (report_map_handle == 0 ||
        ble_gattc_read_long(bt_conn, report_map_handle, 0, map_read_cb, NULL) != 0)
        finish_map_and_subscribe();
}

static int ref_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0 && attr && attr->om)
    {
        uint8_t buf[2] = {0};
        ble_hs_mbuf_to_flat(attr->om, buf, sizeof(buf), NULL);
        report_chrs[walk_idx].report_id = buf[0];
        report_chrs[walk_idx].report_type = buf[1];
        RG_LOGI("Report chr %d: id=%d type=%d", walk_idx, buf[0], buf[1]);
    }
    else if (error->status != BLE_HS_EDONE)
    {
        if (maybe_elevate_security(error->status))
            return 0;
    }
    walk_idx++;
    read_next_ref();
    return 0;
}

static void read_next_ref(void)
{
    while (walk_idx < num_report_chrs && report_chrs[walk_idx].ref_handle == 0)
        walk_idx++;
    if (walk_idx >= num_report_chrs)
    {
        start_map_read();
        return;
    }
    set_step(STEP_REFS);
    if (ble_gattc_read(bt_conn, report_chrs[walk_idx].ref_handle, ref_read_cb, NULL) != 0)
    {
        walk_idx++;
        read_next_ref();
    }
}

static int cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0)
    {
        if (maybe_elevate_security(error->status))
            return 0;
        RG_LOGW("CCCD write failed: %d", error->status);
    }
    walk_idx++;
    subscribe_next();
    return 0;
}

static void subscribe_next(void)
{
    // Subscribe to input reports (type 1); if the report type is unknown
    // (no Report Reference descriptor) subscribe anyway.
    while (walk_idx < num_report_chrs &&
           (report_chrs[walk_idx].cccd_handle == 0 ||
            (report_chrs[walk_idx].ref_handle != 0 && report_chrs[walk_idx].report_type != 1)))
        walk_idx++;

    if (walk_idx >= num_report_chrs)
    {
        bt_step = STEP_READY;
        bt_state = RG_BT_STATE_CONNECTED;
        RG_LOGI("Controller connected: '%s'", bt_dev_name);
        return;
    }
    set_step(STEP_SUBSCRIBE);
    uint8_t cccd_val[2] = {0x01, 0x00};
    if (ble_gattc_write_flat(bt_conn, report_chrs[walk_idx].cccd_handle,
                             cccd_val, sizeof(cccd_val), cccd_write_cb, NULL) != 0)
        bt_abort_connection("CCCD write could not start");
}

static void disc_dscs_for_current(void);

static int dsc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    if (error->status == 0 && dsc)
    {
        uint16_t uuid = ble_uuid_u16(&dsc->uuid.u);
        if (uuid == BT_UUID_CCCD)
            report_chrs[walk_idx].cccd_handle = dsc->handle;
        else if (uuid == BT_UUID_REPORT_REF)
            report_chrs[walk_idx].ref_handle = dsc->handle;
    }
    else if (error->status == BLE_HS_EDONE)
    {
        walk_idx++;
        if (walk_idx < num_report_chrs)
        {
            disc_dscs_for_current();
        }
        else
        {
            walk_idx = 0;
            read_next_ref();
        }
    }
    else
    {
        bt_abort_connection("descriptor discovery failed");
    }
    return 0;
}

static void disc_dscs_for_current(void)
{
    uint16_t end = (walk_idx + 1 < num_report_chrs)
                       ? report_chrs[walk_idx + 1].def_handle - 1
                       : hid_svc_end;
    set_step(STEP_DSC);
    if (ble_gattc_disc_all_dscs(bt_conn, report_chrs[walk_idx].val_handle, end,
                                dsc_disc_cb, NULL) != 0)
        bt_abort_connection("descriptor discovery could not start");
}

static int chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr)
    {
        uint16_t uuid = ble_uuid_u16(&chr->uuid.u);
        if (uuid == BT_UUID_REPORT && num_report_chrs < MAX_REPORT_CHRS)
        {
            report_chrs[num_report_chrs++] = (report_chr_t){
                .def_handle = chr->def_handle,
                .val_handle = chr->val_handle,
                .report_type = 1, // Assume input until the Report Reference says otherwise
            };
        }
        else if (uuid == BT_UUID_REPORT_MAP)
        {
            report_map_handle = chr->val_handle;
        }
    }
    else if (error->status == BLE_HS_EDONE)
    {
        RG_LOGI("HID service: %d report characteristic(s), map=%d",
                num_report_chrs, report_map_handle != 0);
        if (num_report_chrs > 0)
        {
            walk_idx = 0;
            disc_dscs_for_current();
        }
        else
        {
            bt_abort_connection("no report characteristics");
        }
    }
    else
    {
        if (!maybe_elevate_security(error->status))
            bt_abort_connection("characteristic discovery failed");
    }
    return 0;
}

static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg)
{
    if (error->status == 0 && service)
    {
        hid_svc_start = service->start_handle;
        hid_svc_end = service->end_handle;
    }
    else if (error->status == BLE_HS_EDONE)
    {
        if (hid_svc_start != 0)
        {
            set_step(STEP_CHR);
            if (ble_gattc_disc_all_chrs(bt_conn, hid_svc_start, hid_svc_end,
                                        chr_disc_cb, NULL) != 0)
                bt_abort_connection("characteristic discovery could not start");
        }
        else
        {
            bt_abort_connection("no HID service");
        }
    }
    else
    {
        bt_abort_connection("service discovery failed");
    }
    return 0;
}

static void start_discovery(void)
{
    num_report_chrs = 0;
    memset(report_chrs, 0, sizeof(report_chrs));
    hid_svc_start = hid_svc_end = report_map_handle = 0;
    report_map_len = 0;
    walk_idx = 0;
    set_step(STEP_SVC);
    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(BT_HID_SERVICE_UUID);
    if (ble_gattc_disc_svc_by_uuid(bt_conn, &hid_uuid.u, svc_disc_cb, NULL) != 0)
        bt_abort_connection("service discovery could not start");
}

// ---------------------------------------------------------------------------
// Scanning + GAP events
// ---------------------------------------------------------------------------

static bool adv_is_hid(const struct ble_hs_adv_fields *fields)
{
    for (int i = 0; i < fields->num_uuids16; ++i)
        if (fields->uuids16[i].value == BT_HID_SERVICE_UUID)
            return true;
    if (fields->appearance_is_present && (fields->appearance & 0xFFF0) == BT_APPEARANCE_HID)
        return true;
    return false;
}

static bool addr_is_bonded(const ble_addr_t *addr)
{
    ble_addr_t peers[4];
    int num_peers = 0;
    if (ble_store_util_bonded_peers(peers, &num_peers, RG_COUNT(peers)) != 0)
        return false;
    for (int i = 0; i < num_peers; ++i)
        if (ble_addr_cmp(&peers[i], addr) == 0)
            return true;
    return false;
}

// Tolerant AD-structure walk. Cheap controllers (the ShanWan Q36 among them)
// ship malformed advertisement payloads that ble_hs_adv_parse_fields rejects
// wholesale — this extracts what it can instead of giving up.
static void adv_extract_tolerant(const uint8_t *data, uint8_t len,
                                 char *name, size_t name_size, bool *is_hid)
{
    for (uint8_t pos = 0; pos + 1 < len;)
    {
        uint8_t field_len = data[pos];
        if (field_len == 0)
            break;
        uint8_t type = data[pos + 1];
        const uint8_t *val = &data[pos + 2];
        // Clamp against buffer end instead of rejecting (that's the point)
        uint8_t vlen = field_len - 1;
        if (pos + 2 + vlen > len)
            vlen = len - pos - 2;
        if ((type == 0x08 || type == 0x09) && vlen > 0 && !name[0]) // Shortened/complete local name
        {
            size_t n = RG_MIN(vlen, name_size - 1);
            memcpy(name, val, n);
            name[n] = 0;
        }
        else if (type == 0x02 || type == 0x03) // Incomplete/complete 16-bit UUID list
        {
            for (int i = 0; i + 1 < vlen; i += 2)
                if (val[i] == (BT_HID_SERVICE_UUID & 0xFF) && val[i + 1] == (BT_HID_SERVICE_UUID >> 8))
                    *is_hid = true;
        }
        else if (type == 0x19 && vlen >= 2) // Appearance
        {
            uint16_t appearance = val[0] | (val[1] << 8);
            if ((appearance & 0xFFF0) == BT_APPEARANCE_HID)
                *is_hid = true;
        }
        pos += 1 + field_len;
    }
}

static void handle_disc_event(struct ble_gap_event *event)
{
    char name[32] = "";
    bool is_hid = false;
    struct ble_hs_adv_fields fields;
    int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
    if (rc == 0)
    {
        if (fields.name_len)
            snprintf(name, sizeof(name), "%.*s", fields.name_len, (const char *)fields.name);
        is_hid = adv_is_hid(&fields);
    }
    // Run the tolerant walk as well: catches payloads the strict parser
    // rejects entirely (rc != 0) or fields it skipped.
    adv_extract_tolerant(event->disc.data, event->disc.length_data, name, sizeof(name), &is_hid);

    // Known gamepads that advertise without HID service/appearance
    if (!is_hid && name[0] &&
        (strstr(name, "Q36") || strstr(name, "ShanWan") || strstr(name, "Gamepad") ||
         strstr(name, "gamepad")))
        is_hid = true;

    // Log everything we see: with filter_duplicates on, each device shows up
    // once per scan, so this stays quiet but tells us exactly what a
    // misbehaving controller is (not) advertising.
    const uint8_t *a = event->disc.addr.val;
    if (rc != 0)
    {
        RG_LOGI("Adv %02X:%02X:%02X:%02X:%02X:%02X ev=%d nonstd name='%s' hid=%d",
                a[5], a[4], a[3], a[2], a[1], a[0], event->disc.event_type, name, is_hid);
    }
    else
    {
        RG_LOGI("Adv %02X:%02X:%02X:%02X:%02X:%02X ev=%d name='%s' hid=%d",
                a[5], a[4], a[3], a[2], a[1], a[0], event->disc.event_type, name, is_hid);
    }

    if (!bt_found && (is_hid || addr_is_bonded(&event->disc.addr)))
    {
        bt_found_addr = event->disc.addr;
        bt_found = true;
        if (name[0])
            snprintf(bt_dev_name, sizeof(bt_dev_name), "%s", name);
        else
            snprintf(bt_dev_name, sizeof(bt_dev_name), "Gamepad");
        ble_gap_disc_cancel();
        RG_LOGI("Found HID device '%s'", bt_dev_name);
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type)
    {
        case BLE_GAP_EVENT_DISC:
            handle_disc_event(event);
            break;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            bt_scanning = false;
            break;

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0)
            {
                bt_conn = event->connect.conn_handle;
                RG_LOGI("Connected, starting discovery");
                start_discovery();
            }
            else
            {
                RG_LOGW("Connect failed: %d", event->connect.status);
                bt_conn = BLE_HS_CONN_HANDLE_NONE;
                bt_step = STEP_IDLE;
                if (bt_task_running)
                    bt_state = RG_BT_STATE_SCANNING;
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
        {
            int reason = event->disconnect.reason;
            RG_LOGI("Disconnected (reason=0x%X)", reason);
            // MIC failure means the stored bond is stale/mismatched: forget
            // it so the next attempt pairs fresh.
            if ((reason & 0xFF) == 0x3D) // BLE_ERR_CONN_TERM_MIC
            {
                RG_LOGW("MIC failure: deleting stored bond");
                ble_store_util_delete_peer(&event->disconnect.conn.peer_id_addr);
            }
            bt_conn = BLE_HS_CONN_HANDLE_NONE;
            bt_step = STEP_IDLE;
            bt_gamepad_state = 0;
            if (bt_state == RG_BT_STATE_CONNECTED || bt_state == RG_BT_STATE_CONNECTING)
                bt_state = bt_task_running ? RG_BT_STATE_SCANNING : RG_BT_STATE_OFF;
            break;
        }

        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.status == 0)
            {
                RG_LOGI("Link encrypted, restarting discovery");
                start_discovery();
            }
            else
            {
                RG_LOGW("Encryption failed: %d", event->enc_change.status);
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0)
                    ble_store_util_delete_peer(&desc.peer_id_addr);
                bt_abort_connection("encryption failed");
            }
            break;

        case BLE_GAP_EVENT_REPEAT_PAIRING:
        {
            // Device lost its bond but we still have ours: delete and re-pair
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
                ble_store_util_delete_peer(&desc.peer_id_addr);
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

        case BLE_GAP_EVENT_NOTIFY_RX:
        {
            if (event->notify_rx.conn_handle != bt_conn)
                break;
            uint8_t buf[32];
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (len > sizeof(buf))
                len = sizeof(buf);
            ble_hs_mbuf_to_flat(event->notify_rx.om, buf, len, NULL);
            uint8_t report_id = 0;
            for (int i = 0; i < num_report_chrs; ++i)
            {
                if (report_chrs[i].val_handle == event->notify_rx.attr_handle)
                {
                    report_id = report_chrs[i].report_id;
                    break;
                }
            }
            uint32_t keys = parse_input_report(report_id, buf, len);
            // Debug: dump the raw report whenever its bytes change, so
            // unmapped buttons (e.g. Home variants) are easy to identify.
            static uint8_t last_report[32];
            static uint16_t last_len = 0;
            if (len != last_len || memcmp(buf, last_report, len) != 0)
            {
                char hex[70] = "";
                for (int i = 0; i < len && i < 31; ++i)
                    snprintf(hex + i * 2, sizeof(hex) - i * 2, "%02X", buf[i]);
                RG_LOGD("Report %d: %s -> keys %04X", report_id, hex, (unsigned)keys);
                memcpy(last_report, buf, len);
                last_len = len;
            }
            bt_gamepad_state = keys;
            break;
        }

        default:
            break;
    }
    return 0;
}

static void start_scan(void)
{
    uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
    ble_hs_id_infer_auto(0, &own_addr_type);

    struct ble_gap_disc_params params = {
        .itvl = 0x60,
        .window = 0x40,
        .passive = 0,
        .filter_duplicates = 1,
    };
    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if (rc == 0)
    {
        RG_LOGI("Scanning for controllers...");
        bt_scanning = true;
        bt_state = RG_BT_STATE_SCANNING;
        scan_started_at = rg_system_timer();
    }
    else if (rc != BLE_HS_EALREADY)
    {
        RG_LOGE("ble_gap_disc failed: %d", rc);
    }
}

// Most recently stored bond, if any (identity address of the controller).
static bool get_last_bonded(ble_addr_t *out)
{
    ble_addr_t peers[8];
    int num_peers = 0;
    if (ble_store_util_bonded_peers(peers, &num_peers, RG_COUNT(peers)) != 0 || num_peers < 1)
        return false;
    *out = peers[num_peers - 1];
    return true;
}

// Direct LL initiate to the bonded controller's stored address. Needed because
// pads like the ShanWan Q36 use DIRECTED advertising in their reconnect mode
// (slow LED blink): those packets never show up in scan reports, so the scan
// path can't find them — but an initiator listening for the stored address
// connects the moment it hears one. Returns false when there is no bond or the
// attempt couldn't be started (caller falls back to scanning).
static bool try_direct_connect(void)
{
    ble_addr_t peer;
    if (!get_last_bonded(&peer))
        return false;

    bt_state = RG_BT_STATE_CONNECTING;
    security_tried = false;
    set_step(STEP_CONNECT);
    RG_LOGI("Direct connect to bonded controller...");
    uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
    ble_hs_id_infer_auto(0, &own_addr_type);
    // 4s listen window, then fall back to scanning (bt_task alternates)
    int rc = ble_gap_connect(own_addr_type, &peer, 4000, NULL, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY)
    {
        RG_LOGW("ble_gap_connect (direct) failed: %d", rc);
        bt_step = STEP_IDLE;
        bt_state = RG_BT_STATE_SCANNING;
        return false;
    }
    return true;
}

static void bt_task(void *arg)
{
    int64_t sync_warn_at = rg_system_timer() + 5 * 1000000;

    bt_task_exited = false;

    while (bt_task_running)
    {
        if (!ble_hs_synced())
        {
            if (rg_system_timer() > sync_warn_at)
            {
                RG_LOGW("BLE host still not synced with controller!");
                sync_warn_at = rg_system_timer() + 5 * 1000000;
            }
            rg_task_delay(250);
            continue;
        }
        if (bt_step == STEP_READY)
        {
            search_started_at = 0; // connected: next search gets a fresh budget
            next_direct_at = 0;
            idle_until = 0;
            search_budget = RG_BT_SEARCH_TIMEOUT_US;
            rg_task_delay(250);
            continue;
        }
        if (bt_step != STEP_IDLE)
        {
            // Connection/discovery in progress: watchdog it
            if (rg_system_timer() > step_deadline)
            {
                RG_LOGW("Step %d timed out", bt_step);
                if (bt_conn != BLE_HS_CONN_HANDLE_NONE)
                {
                    ble_gap_terminate(bt_conn, BLE_ERR_REM_USER_CONN_TERM);
                }
                else
                {
                    ble_gap_conn_cancel();
                    bt_step = STEP_IDLE;
                }
                set_step(bt_step); // Rearm deadline so we don't spam terminate
            }
            rg_task_delay(100);
            continue;
        }
        if (idle_until)
        {
            // Retry mode: radio parked between search windows. Nothing can set
            // bt_found here (we are not scanning), so just wait it out.
            if (rg_system_timer() < idle_until)
            {
                rg_task_delay(250);
                continue;
            }
            idle_until = 0; // next iteration starts a fresh search window
        }
        if (bt_found)
        {
            bt_found = false;
            bt_scanning = false;
            bt_state = RG_BT_STATE_CONNECTING;
            security_tried = false;
            set_step(STEP_CONNECT);
            RG_LOGI("Connecting to controller...");
            uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
            ble_hs_id_infer_auto(0, &own_addr_type);
            int rc = ble_gap_connect(own_addr_type, &bt_found_addr, 10000, NULL, gap_event_cb, NULL);
            if (rc != 0 && rc != BLE_HS_EALREADY)
            {
                RG_LOGW("ble_gap_connect failed: %d", rc);
                bt_step = STEP_IDLE;
            }
        }
        else if (!bt_scanning)
        {
            if (!search_started_at)
                search_started_at = rg_system_timer();
            // Bonded pad first: a direct connect catches its directed reconnect
            // advertising (invisible to scan reports). Falls back to scanning,
            // and the branch below pauses the scan to retry this periodically.
            if (rg_system_timer() >= next_direct_at && try_direct_connect())
                next_direct_at = rg_system_timer() + RG_BT_DIRECT_RETRY_US;
            else
                start_scan();
        }
        else if (next_direct_at && rg_system_timer() >= next_direct_at)
        {
            // Time for another direct-connect attempt: pause the scan (a
            // cancelled scan delivers no DISC_COMPLETE, so clear the flag
            // ourselves); the next loop iteration lands in the branch above.
            ble_gap_disc_cancel();
            bt_scanning = false;
        }
        else if (rg_system_timer() - search_started_at > search_budget)
        {
            ble_gap_disc_cancel();
            bt_scanning = false;
            // With a bond the controller is just off or asleep, so keep looking
            // at a low duty cycle: waking the pad then reconnects on its own,
            // no menu needed. Without a bond nothing can ever answer, so stop
            // and clear the setting (the user never paired anything).
            ble_addr_t bonded;
            if (get_last_bonded(&bonded))
            {
                RG_LOGI("No controller found, retrying in %ds",
                        (int)(RG_BT_RETRY_IDLE_US / 1000000));
                idle_until = rg_system_timer() + RG_BT_RETRY_IDLE_US;
                search_budget = RG_BT_RETRY_SEARCH_US;
                search_started_at = 0;
                next_direct_at = 0;
                bt_state = RG_BT_STATE_SCANNING;
            }
            else
            {
                RG_LOGI("No controller found after 60s and nothing paired, giving up");
                bt_state = RG_BT_STATE_OFF;
                rg_settings_set_boolean(NS_GLOBAL, RG_SETTING_BT_CONTROLLER, false);
                rg_settings_commit();
                bt_task_running = false;
            }
        }
        rg_task_delay(250);
    }

    bt_task_exited = true;
}

static void nimble_host_task(void *arg)
{
    nimble_port_run(); // Returns when nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool rg_bluetooth_supported(void)
{
    return true;
}

bool rg_bluetooth_init(void)
{
    if (bt_task_running)
        return true;

    // The NimBLE stack is started once and never torn down: a full
    // deinit/reinit cycle is unreliable in ESP-IDF (the second init fails).
    // "Off" just stops scanning/connections; the RAM is reclaimed on the
    // next reboot, and every app switch is a reboot anyway.
    if (bt_initialized)
    {
        bt_scanning = false;
        bt_found = false;
        search_started_at = 0;
        next_direct_at = 0;
        idle_until = 0;
        search_budget = RG_BT_SEARCH_TIMEOUT_US;
        bt_task_exited = false; // the task sets this too, but deinit may look first
        bt_task_running = true;
        bt_state = RG_BT_STATE_SCANNING;
        rg_task_create("rg_btpad", &bt_task, NULL, 5 * 1024, RG_TASK_PRIORITY_2, 1);
        return true;
    }

    RG_LOGI("Starting BLE gamepad support...");

    load_btn_map();

    // The BLE stack stores pairing keys in NVS. Tolerate a full/corrupt partition.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }

    if ((err = nimble_port_init()) != ESP_OK)
    {
        RG_LOGE("nimble_port_init failed: 0x%X (out of memory?)", err);
        return false;
    }

    // Accept pairing without user interaction (we have no keyboard/display
    // during pairing) and bond so the controller reconnects automatically.
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_store_config_init();
    nimble_port_freertos_init(nimble_host_task);

    // Reset scan state from a previous session: cancelling a scan does NOT
    // deliver DISC_COMPLETE, so bt_scanning would stay true after a
    // deinit/init cycle and the new task would never start scanning.
    bt_scanning = false;
    bt_found = false;
    search_started_at = 0;
    next_direct_at = 0;
    idle_until = 0;
    search_budget = RG_BT_SEARCH_TIMEOUT_US;
    bt_task_exited = false;

    bt_initialized = true;
    bt_task_running = true;
    bt_state = RG_BT_STATE_SCANNING;
    rg_task_create("rg_btpad", &bt_task, NULL, 5 * 1024, RG_TASK_PRIORITY_2, 1);

    return true;
}

void rg_bluetooth_deinit(void)
{
    if (!bt_initialized || !bt_task_running)
        return;

    RG_LOGI("Stopping BLE gamepad support...");
    bt_task_running = false;

    // Abort whatever the connect task is doing so it can exit
    ble_gap_disc_cancel();
    ble_gap_conn_cancel();
    for (int i = 0; i < 50 && !bt_task_exited; ++i)
        rg_task_delay(100);

    if (bt_conn != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(bt_conn, BLE_ERR_REM_USER_CONN_TERM);

    // The stack itself stays up (see rg_bluetooth_init) — only the scan task
    // and the connection are stopped.
    bt_gamepad_state = 0;
    bt_state = RG_BT_STATE_OFF;
    bt_scanning = false;
    bt_found = false;
}

rg_bt_state_t rg_bluetooth_get_state(void)
{
    return bt_state;
}

const char *rg_bluetooth_get_device_name(void)
{
    return bt_dev_name;
}

uint32_t rg_bluetooth_read_gamepad(void)
{
    return bt_capture ? 0 : bt_gamepad_state;
}

uint32_t rg_bluetooth_read_raw_buttons(void)
{
    return bt_raw_buttons;
}

uint32_t rg_bluetooth_peek_gamepad(void)
{
    return bt_gamepad_state;
}

void rg_bluetooth_set_capture(bool on)
{
    bt_capture = on;
}

void rg_bluetooth_set_button_map(const uint32_t map[RG_BT_MAX_BUTTONS])
{
    memcpy(btn_key_map, map, sizeof(btn_key_map));
    save_btn_map();
}

const uint32_t *rg_bluetooth_get_button_map(void)
{
    return btn_key_map;
}

void rg_bluetooth_reset_button_map(void)
{
    load_default_btn_map();
    save_btn_map();
}

bool rg_bluetooth_forget_bonds(void)
{
    if (!bt_initialized)
        return false;
    if (bt_conn != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(bt_conn, BLE_ERR_REM_USER_CONN_TERM);
    return ble_store_clear() == 0;
}

#else // Stubs for builds without BLE (SDL2, or CONFIG_BT_NIMBLE_ENABLED unset)

bool rg_bluetooth_supported(void) { return false; }
bool rg_bluetooth_init(void) { return false; }
void rg_bluetooth_deinit(void) {}
rg_bt_state_t rg_bluetooth_get_state(void) { return RG_BT_STATE_OFF; }
const char *rg_bluetooth_get_device_name(void) { return ""; }
uint32_t rg_bluetooth_read_gamepad(void) { return 0; }
bool rg_bluetooth_forget_bonds(void) { return false; }
uint32_t rg_bluetooth_read_raw_buttons(void) { return 0; }
uint32_t rg_bluetooth_peek_gamepad(void) { return 0; }
void rg_bluetooth_set_capture(bool on) { (void)on; }
void rg_bluetooth_set_button_map(const uint32_t map[RG_BT_MAX_BUTTONS]) { (void)map; }
const uint32_t *rg_bluetooth_get_button_map(void) { static const uint32_t m[RG_BT_MAX_BUTTONS]; return m; }
void rg_bluetooth_reset_button_map(void) {}

#endif
