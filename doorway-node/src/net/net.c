/*
 * net.c -- WiFi + MQTT client implementation. See net.h for the contract.
 *
 * THREADING MODEL (the one thing to really understand in this file):
 * ESP-IDF networking is event-driven. esp-mqtt runs its OWN FreeRTOS task and
 * calls mqtt_event_handler() from there; WiFi events arrive on the system
 * event task. Neither of those contexts may touch evbuf (single-writer, owned
 * by the main task). So the handlers only record what happened into small
 * flag/word variables, and the main task picks the work up later through the
 * *_step() functions. Word-sized reads/writes are atomic on the ESP32, which
 * is all these flags need.
 */
#include "net.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "storage/evbuf.h"
#include "secrets.h" /* gitignored -- copy secrets.h.example and fill in */

static const char *TAG = "net";

#define FW_VERSION "0.4.0-phase4"
#define HEARTBEAT_INTERVAL_US (30LL * 1000 * 1000) /* 30 s */
#define PUBACK_TIMEOUT_US (10LL * 1000 * 1000)     /* give up waiting, republish */

/* NTP server; override in secrets.h if the LAN runs its own (e.g. the Pi 5). */
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif

/* Topic strings, built once from node_id in net_init(). */
static char s_topic_events[80];
static char s_topic_status[80];
static char s_topic_config[80];
static const char *s_node_id;

static esp_mqtt_client_handle_t s_client = NULL;

/* ---- flags written by handler tasks, read by the main task ---------------- */
static volatile bool s_mqtt_connected = false;

/* In-flight QoS1 publish: msg_id we're waiting on, and whether PUBACK arrived. */
static volatile int s_inflight_msg_id = -1;
static volatile bool s_inflight_acked = false;
static int64_t s_inflight_since_us = 0; /* main task only */

/* Latest raw config payload. The MQTT task copies it in and sets the flag; the
 * main task parses it inside net_motion_threshold(). If two configs arrive
 * back-to-back the older copy is simply overwritten -- last writer wins, which
 * is exactly the semantics we want for configuration. */
static char s_config_buf[256];
static volatile bool s_config_pending = false;
static int s_motion_threshold = -1; /* parsed value; -1 = nothing received yet */

static int64_t s_last_heartbeat_us = 0;

/* ---- SNTP state (written by the lwIP task's sync callback) ----------------
 * Both fields are 32-bit on purpose: the ESP32 can't write an int64 atomically,
 * and these are crossed between the lwIP task and the main task (same rule as
 * the MQTT flags above). Seconds resolution is plenty for "sync age". */
static volatile bool s_time_synced = false;
static volatile uint32_t s_last_sync_uptime_s = 0;

/* ---------------------------------------------------------------------------
 * SNTP: runs in the lwIP task -- flags only here, same contract as MQTT.
 * Fires on every (re)sync: lwIP re-polls hourly (CONFIG_LWIP_SNTP_UPDATE_DELAY)
 * to bound crystal drift (~tens of ppm => tens of ms/hour). Sync mode is the
 * default IMMED (settimeofday step): the first sync is a ~56-year jump from
 * the 1970 epoch that only a step can cover, and later corrections are small
 * enough that a step is harmless for occupancy timestamps.
 * ------------------------------------------------------------------------- */
static void time_sync_cb(struct timeval *tv)
{
    s_last_sync_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s_time_synced = true;
    ESP_LOGI(TAG, "Time synced: %lld (unix s)", (long long)tv->tv_sec);
}

/* ---------------------------------------------------------------------------
 * WiFi: join as a station, reconnect forever on drop.
 * ------------------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        // Just retry. esp-mqtt notices the dead socket on its own and waits;
        // it resumes once the IP comes back. Crossings keep landing in evbuf
        // the whole time, so nothing is lost during an outage.
        ESP_LOGW(TAG, "WiFi disconnected -- retrying");
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        // (Re)start SNTP now that DNS + routing exist. Safe to call on every
        // reconnect -- a restart just triggers a fresh poll, which is exactly
        // what we want after an outage of unknown length.
        esp_netif_sntp_start();
        // Start the MQTT client on the FIRST IP only; after that it manages
        // its own reconnects.
        static bool mqtt_started = false;
        if (!mqtt_started)
        {
            mqtt_started = true;
            esp_mqtt_client_start(s_client);
        }
    }
}

/* ---------------------------------------------------------------------------
 * MQTT event handler -- runs in esp-mqtt's task; flags only, no evbuf.
 * ------------------------------------------------------------------------- */
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        // Subscribe to our retained config: the broker replays the last pushed
        // config immediately, so a reboot picks parameters back up with no
        // action from the backend.
        esp_mqtt_client_subscribe(s_client, s_topic_config, 1);
        // Announce ourselves: retained, so the backend sees this node exists
        // (with firmware + sensor info) whenever it looks, not only if it was
        // listening at the moment we booted. Doubles as the registration
        // message. Overwrites the LWT "offline" from any previous death.
        {
            char msg[192];
            snprintf(msg, sizeof msg,
                     "{\"node_id\":\"%s\",\"online\":true,\"fw\":\"%s\","
                     "\"sensor\":\"vl53l5cx\"}",
                     s_node_id, FW_VERSION);
            esp_mqtt_client_publish(s_client, s_topic_status, msg, 0, 1, 1);
        }
        s_mqtt_connected = true;
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_mqtt_connected = false;
        break;

    case MQTT_EVENT_PUBLISHED:
        // PUBACK for a QoS1 publish. If it's the event we're draining, tell
        // the main task it may now pop it from flash.
        if (ev->msg_id == s_inflight_msg_id)
            s_inflight_acked = true;
        break;

    case MQTT_EVENT_DATA:
        // Retained config (the only topic we subscribe to). Payload may arrive
        // fragmented for large messages; ours are tiny, so take the first
        // fragment only (current_data_offset == 0) and ignore pathological ones.
        if (ev->current_data_offset == 0 && ev->data_len < (int)sizeof(s_config_buf))
        {
            memcpy(s_config_buf, ev->data, ev->data_len);
            s_config_buf[ev->data_len] = '\0';
            s_config_pending = true;
        }
        break;

    case MQTT_EVENT_ERROR:
        if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
            ESP_LOGE(TAG, "MQTT connection refused -- check MQTT_USER/MQTT_PASS");
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
void net_init(const char *node_id)
{
    s_node_id = node_id;
    snprintf(s_topic_events, sizeof s_topic_events, "home/doorways/%s/events", node_id);
    snprintf(s_topic_status, sizeof s_topic_status, "home/doorways/%s/status", node_id);
    snprintf(s_topic_config, sizeof s_topic_config, "home/doorways/%s/config", node_id);

    // --- WiFi station bring-up (standard ESP-IDF sequence) ---
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    // Modem power save (radio sleeps between AP beacons, connection stays up).
    // This is the Phase 3 power posture -- see POWER_LIGHT_SLEEP in main.c.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    ESP_ERROR_CHECK(esp_wifi_start());

    // --- SNTP (configured now, started when WiFi gets an IP) ---
    // start = false: starting before the netif has an IP would just burn DNS
    // lookups against a dead interface; the got-IP handler starts it.
    {
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
        sntp_cfg.start = false;
        sntp_cfg.sync_cb = time_sync_cb;
        ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_cfg));
    }

    // --- MQTT client (created now, started when WiFi gets an IP) ---
    // LWT: if this node dies without a clean DISCONNECT (power pulled, crash,
    // out of WiFi range), the BROKER publishes this on our behalf. Retained,
    // so the dashboard's "node offline" survives broker-side too.
    static char lwt_msg[96];
    snprintf(lwt_msg, sizeof lwt_msg, "{\"node_id\":\"%s\",\"online\":false}", node_id);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_URI,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS,
        .credentials.client_id = node_id,
        .session.last_will = {
            .topic = s_topic_status,
            .msg = lwt_msg,
            .qos = 1,
            .retain = 1,
        },
        .session.keepalive = 30, // s; broker declares us dead at ~1.5x this
    };
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL));
    ESP_LOGI(TAG, "net up: broker %s, events -> %s", MQTT_URI, s_topic_events);
}

bool net_connected(void)
{
    return s_mqtt_connected;
}

void net_drain_step(void)
{
    if (!s_mqtt_connected)
        return;

    // Phase 1 of the drain: a publish is in flight -- resolve it before
    // touching the next event (strictly one at a time keeps ordering and makes
    // "which event does this PUBACK belong to" trivial).
    if (s_inflight_msg_id >= 0)
    {
        if (s_inflight_acked)
        {
            evbuf_pop(); // broker has it durably -- NOW it may leave flash
            s_inflight_msg_id = -1;
        }
        else if (esp_timer_get_time() - s_inflight_since_us > PUBACK_TIMEOUT_US)
        {
            // No PUBACK (connection blipped mid-publish?). Forget the attempt;
            // next call re-peeks the SAME event and publishes again. Worst
            // case the broker receives a duplicate -- seq lets Phase 4 dedup.
            ESP_LOGW(TAG, "PUBACK timeout -- will republish seq head");
            s_inflight_msg_id = -1;
        }
        return;
    }

    evbuf_record_t r;
    if (!evbuf_peek(&r))
        return; // nothing buffered

    // The Phase 2 doc's event shape, plus seq. t_unix_ms is the wall-clock at
    // DETECTION (0 = clock wasn't synced when the event fired -- backend
    // stamps arrival time for those); t_us stays boot-relative for intra-boot
    // ordering and debugging.
    char json[256];
    snprintf(json, sizeof json,
             "{\"node_id\":\"%s\",\"seq\":%u,\"event\":\"crossing\","
             "\"direction\":\"%s\",\"t_unix_ms\":%lld,\"t_us\":%lld,"
             "\"net\":%.3f,\"confidence\":%.3f,\"peak_blob\":%u}",
             s_node_id, (unsigned)r.seq, r.direction ? "in" : "out",
             r.t_unix_ms, r.t_us, r.net, r.confidence, (unsigned)r.peak_blob);

    s_inflight_acked = false;
    int msg_id = esp_mqtt_client_publish(s_client, s_topic_events, json, 0, 1, 0);
    if (msg_id > 0) // QoS1 always returns a real id; <=0 means enqueue failed
    {
        s_inflight_msg_id = msg_id;
        s_inflight_since_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Publishing seq %u (%s)", (unsigned)r.seq, r.direction ? "in" : "out");
    }
}

void net_heartbeat_step(void)
{
    if (!s_mqtt_connected)
        return;
    int64_t now = esp_timer_get_time();
    if (now - s_last_heartbeat_us < HEARTBEAT_INTERVAL_US)
        return;
    s_last_heartbeat_us = now;

    wifi_ap_record_t ap;
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;

    // Sync quality for the backend/dashboard (Phase 4): synced at all, and how
    // stale. age grows ~1 hr between lwIP re-polls; an age of many hours means
    // the NTP server is unreachable and this node's timestamps are drifting.
    bool synced = s_time_synced;
    long sync_age_s = synced
                          ? (long)((uint32_t)(now / 1000000) - s_last_sync_uptime_s)
                          : -1;

    // Retained: a dashboard connecting at any moment gets the latest health
    // snapshot instantly instead of waiting up to 30 s for the next beat.
    char msg[288];
    snprintf(msg, sizeof msg,
             "{\"node_id\":\"%s\",\"online\":true,\"fw\":\"%s\","
             "\"uptime_s\":%lld,\"heap_free\":%u,\"pending\":%u,\"rssi\":%d,"
             "\"t_unix_ms\":%lld,\"time_synced\":%s,\"sync_age_s\":%ld}",
             s_node_id, FW_VERSION, now / 1000000,
             (unsigned)esp_get_free_heap_size(), (unsigned)evbuf_count(), rssi,
             net_epoch_ms(), synced ? "true" : "false", sync_age_s);
    esp_mqtt_client_publish(s_client, s_topic_status, msg, 0, 1, 1);
}

bool net_time_synced(void)
{
    return s_time_synced;
}

int64_t net_epoch_ms(void)
{
    if (!s_time_synced)
        return 0; // pre-sync system time is the 1970 epoch -- worse than nothing
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int net_motion_threshold(int fallback)
{
    // Parse lazily, in the main task, only when a new payload arrived. cJSON
    // allocates; keeping it out of the MQTT task also keeps that task's stack
    // small and the failure modes in one place.
    if (s_config_pending)
    {
        s_config_pending = false;
        cJSON *root = cJSON_Parse(s_config_buf);
        if (root)
        {
            const cJSON *mt = cJSON_GetObjectItem(root, "motion_threshold");
            if (cJSON_IsNumber(mt) && mt->valueint > 0 && mt->valueint < 256)
            {
                s_motion_threshold = mt->valueint;
                ESP_LOGI(TAG, "Config: motion_threshold = %d", s_motion_threshold);
            }
            cJSON_Delete(root);
        }
        else
        {
            ESP_LOGW(TAG, "Config payload is not valid JSON: %s", s_config_buf);
        }
    }
    return (s_motion_threshold > 0) ? s_motion_threshold : fallback;
}
