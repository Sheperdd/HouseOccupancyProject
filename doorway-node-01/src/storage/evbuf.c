/*
 * evbuf.c -- NVS-backed store-and-forward ring buffer. See evbuf.h for contract.
 *
 * Layout in the "evbuf" NVS namespace:
 *   u32  "head"   oldest unsent seq (inclusive)
 *   u32  "tail"   next seq to assign         -> count = tail - head
 *   blob "e<slot>" one evbuf_record_t, slot = seq % EVBUF_CAPACITY
 *
 * head/tail are monotonic (they only ever increase); the modulo maps them onto
 * EVBUF_CAPACITY physical slots. Because head and tail differ by exactly
 * EVBUF_CAPACITY when full, tail % CAP == head % CAP at that moment -- so writing
 * the new event's slot overwrites precisely the oldest one, and we bump head past
 * it. Every mutation is followed by nvs_commit() so it survives power loss.
 */
#include "evbuf.h"

#include <stdio.h> /* snprintf */

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "evbuf";

static nvs_handle_t s_nvs;     /* open handle, lives for the program's lifetime */
static bool s_ready = false;   /* true once evbuf_init succeeded */
static uint32_t s_head = 0;    /* cached in RAM, authoritative copy is in NVS */
static uint32_t s_tail = 0;

/* Build the per-slot key "e<slot>" into buf (caller provides >= 8 bytes). */
static void slot_key(uint32_t seq, char *buf, size_t n)
{
    snprintf(buf, n, "e%lu", (unsigned long)(seq % EVBUF_CAPACITY));
}

void evbuf_init(void)
{
    if (s_ready)
        return;

    /* Bring up the NVS flash partition (standard recover-on-corruption dance). */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS needs erase (%s) -- erasing", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_open("evbuf", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    /* Load head/tail; absent on first boot -> start empty and persist. */
    if (nvs_get_u32(s_nvs, "head", &s_head) != ESP_OK)
        s_head = 0;
    if (nvs_get_u32(s_nvs, "tail", &s_tail) != ESP_OK)
        s_tail = 0;
    /* Guard against an impossible cached state (e.g. partial old format). */
    if (s_tail - s_head > EVBUF_CAPACITY)
    {
        ESP_LOGW(TAG, "head/tail inconsistent (h=%lu t=%lu) -- resetting",
                 (unsigned long)s_head, (unsigned long)s_tail);
        s_head = s_tail = 0;
    }
    nvs_set_u32(s_nvs, "head", s_head);
    nvs_set_u32(s_nvs, "tail", s_tail);
    nvs_commit(s_nvs);

    s_ready = true;
    ESP_LOGI(TAG, "ready -- %u event(s) pending from a previous run",
             (unsigned)(s_tail - s_head));
}

bool evbuf_enqueue(const evbuf_record_t *r)
{
    if (!s_ready || r == NULL)
        return false;

    bool dropped = false;
    if (s_tail - s_head >= EVBUF_CAPACITY)
    {
        /* Full: drop the oldest. Its slot (head % CAP) is the same slot the new
         * event (tail % CAP) is about to occupy, so the blob write replaces it. */
        s_head++;
        dropped = true;
    }

    evbuf_record_t rec = *r;
    rec.seq = s_tail; /* assign the monotonic id here, ignoring any caller value */

    char key[8];
    slot_key(s_tail, key, sizeof(key));
    esp_err_t err = nvs_set_blob(s_nvs, key, &rec, sizeof(rec));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set_blob %s failed: %s", key, esp_err_to_name(err));
        if (dropped)
            s_head--; /* roll back the speculative drop; nothing was committed */
        return false;
    }

    s_tail++;
    nvs_set_u32(s_nvs, "tail", s_tail);
    if (dropped)
        nvs_set_u32(s_nvs, "head", s_head);
    nvs_commit(s_nvs);

    if (dropped)
        ESP_LOGW(TAG, "buffer full -- dropped oldest event");
    return true;
}

bool evbuf_peek(evbuf_record_t *out)
{
    if (!s_ready || out == NULL || s_head == s_tail)
        return false;

    char key[8];
    slot_key(s_head, key, sizeof(key));
    size_t len = sizeof(*out);
    esp_err_t err = nvs_get_blob(s_nvs, key, out, &len);
    if (err != ESP_OK || len != sizeof(*out))
    {
        ESP_LOGE(TAG, "get_blob %s failed (%s, len=%u) -- dropping slot",
                 key, esp_err_to_name(err), (unsigned)len);
        /* Unreadable/wrong-size record: drop it so the queue can't wedge. */
        s_head++;
        nvs_set_u32(s_nvs, "head", s_head);
        nvs_commit(s_nvs);
        return false;
    }
    return true;
}

bool evbuf_pop(void)
{
    if (!s_ready || s_head == s_tail)
        return false;

    s_head++;
    nvs_set_u32(s_nvs, "head", s_head);
    nvs_commit(s_nvs);
    return true;
}

size_t evbuf_count(void)
{
    if (!s_ready)
        return 0;
    return (size_t)(s_tail - s_head);
}
