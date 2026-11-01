#include "harness.h"
#include "fakes.h"

/* The quarter-hour record is what the page draws as "what really happened".
 * A slot filed under the wrong time, a record that does not survive a
 * restart or a ring that loses its place after a wrap would quietly draw a
 * wrong past -- so flash is faked in RAM here, NOR rules included. */
#define EM_HIST_HOST_TEST
#include "em_hist.c"

#define FLASH_MAX (64 * 1024)
static uint8_t f_flash[FLASH_MAX];
static eh_io_t s_ff_io;
static int     f_erases;
static esp_err_t ff_read(size_t o, void *b, size_t l)
{
    if (o + l > s_ff_io.size) return ESP_ERR_INVALID_SIZE;
    memcpy(b, f_flash + o, l); return ESP_OK;
}
static esp_err_t ff_write(size_t o, const void *b, size_t l)
{
    if (o + l > s_ff_io.size) return ESP_ERR_INVALID_SIZE;
    const uint8_t *p = b;
    for (size_t i = 0; i < l; i++) f_flash[o + i] &= p[i];     /* NOR: bits only clear */
    return ESP_OK;
}
static esp_err_t ff_erase(size_t o, size_t l)
{
    if (o % EH_SECTOR || l % EH_SECTOR || o + l > s_ff_io.size) return ESP_ERR_INVALID_ARG;
    memset(f_flash + o, 0xFF, l); f_erases++; return ESP_OK;
}
static eh_io_t s_ff_io = { ff_read, ff_write, ff_erase, FLASH_MAX };

/* A blank flash of `size` bytes and a module that has never run. */
static void fresh(size_t size)
{
    memset(f_flash, 0xFF, sizeof(f_flash));
    s_ff_io.size = size;
    s_eh_io = &s_ff_io;
    f_erases = 0;
    s_cur = 0;
    CHECK(em_hist_init() == ESP_OK);
}

/* A restart: RAM state gone, flash kept. */
static void reboot(void)
{
    s_cur = 0;
    CHECK(em_hist_init() == ESP_OK);
}

#define T0 1793487600LL          /* 2026-11-01 00:00 CET, a quarter hour */

static eh_sample_t smp(float soc, float grid, uint8_t act, bool acting)
{
    eh_sample_t s = { .deye_soc = soc, .byd_soc = 50.0f, .deye_bat_w = -1000.0f, .grid_w = grid,
                      .pv_w = 2000.0f, .house_w = 600.0f, .price_ct = 21.37f, .act = act,
                      .mode = 2, .acting = acting, .paused = false };
    return s;
}

/* Every 10 s through slot k with constant values. */
static void fill_slot(int k, float soc, float grid, uint8_t act, bool acting)
{
    for (int s = 0; s < 90; s++) {
        eh_sample_t x = smp(soc, grid, act, acting);
        em_hist_tick(T0 + (int64_t)k * 900 + s * 10, &x);
    }
}

static void test_a_slot_is_written_when_it_ends(void)
{
    fresh(FLASH_MAX);
    fill_slot(0, 40.0f, 500.0f, 2, true);
    CHECK_I(em_hist_count(NULL), 0);                 /* still running        */
    fill_slot(1, 45.0f, -200.0f, 0, false);
    CHECK_I(em_hist_count(NULL), 1);
    eh_slot_t h[3];
    CHECK_I(em_hist_range(T0, 3, h), 1);
    CHECK(h[0].valid);
    CHECK(!h[1].valid);                              /* slot 1 not closed yet */
    CHECK(!h[2].valid);
    CHECK_F(h[0].deye_soc, 40.0, 0.05);
    CHECK_F(h[0].grid_w, 500.0, 0.5);
    CHECK_F(h[0].price_ct, 21.37, 0.005);
    CHECK_I(h[0].act, 2);
    CHECK(h[0].flags & EH_F_ACTING);
    CHECK_I(h[0].mode, 2);
}

static void test_soc_is_the_last_value_powers_the_mean(void)
{
    fresh(FLASH_MAX);
    for (int s = 0; s < 90; s++) {
        eh_sample_t x = smp(30.0f + s * 0.1f, s < 45 ? 0.0f : 1000.0f, 1, false);
        em_hist_tick(T0 + s * 10, &x);
    }
    em_hist_tick(T0 + 900, NULL);                    /* the next slot begins */
    eh_slot_t h;
    CHECK_I(em_hist_range(T0, 1, &h), 1);
    CHECK_F(h.deye_soc, 38.9, 0.05);
    CHECK_F(h.grid_w, 500.0, 0.5);
    CHECK_I(h.act, 1);
    CHECK(!(h.flags & EH_F_ACTING));
}

static void test_majority_action_and_unknown_values(void)
{
    fresh(FLASH_MAX);
    for (int s = 0; s < 90; s++) {
        eh_sample_t x = smp(NAN, NAN, s < 30 ? 0 : 2, s >= 60);
        x.byd_soc = NAN;
        em_hist_tick(T0 + s * 10, &x);
    }
    em_hist_tick(T0 + 900, NULL);
    eh_slot_t h;
    CHECK_I(em_hist_range(T0, 1, &h), 1);
    CHECK_I(h.act, 2);                               /* 60 of 90 ticks       */
    CHECK(!(h.flags & EH_F_ACTING));                 /* 30 of 90: not most   */
    CHECK(isnan(h.deye_soc));
    CHECK(isnan(h.byd_soc));
    CHECK(isnan(h.grid_w));
    CHECK_F(h.house_w, 600.0, 0.5);
}

static void test_no_plan_is_none(void)
{
    fresh(FLASH_MAX);
    fill_slot(0, 50.0f, 0.0f, EH_ACT_NONE, false);
    em_hist_tick(T0 + 900, NULL);
    eh_slot_t h;
    CHECK_I(em_hist_range(T0, 1, &h), 1);
    CHECK_I(h.act, EH_ACT_NONE);
}

static void test_a_restart_keeps_the_past(void)
{
    fresh(FLASH_MAX);
    for (int k = 0; k < 10; k++) fill_slot(k, (float)k, 0.0f, 0, false);
    CHECK_I(em_hist_count(NULL), 9);
    reboot();
    CHECK_I(em_hist_count(NULL), 9);
    /* The display was off for slot 10 and part of 11. */
    for (int s = 30; s < 90; s++) { eh_sample_t x = smp(77.0f, 0, 0, false); em_hist_tick(T0 + 11 * 900 + s * 10, &x); }
    fill_slot(12, 12.0f, 0.0f, 0, false);
    eh_slot_t h[13];
    CHECK_I(em_hist_range(T0, 13, h), 10);
    for (int k = 0; k < 9; k++) CHECK_F(h[k].deye_soc, k, 0.05);
    CHECK(h[8].valid);
    CHECK(!h[9].valid);                              /* lost in the restart  */
    CHECK(!h[10].valid);                             /* display off          */
    CHECK_F(h[11].deye_soc, 77.0, 0.05);
    CHECK(!h[12].valid);
    int64_t oldest;
    em_hist_count(&oldest);
    CHECK_I(oldest, T0);
}

static void test_the_ring(void)
{
    /* Three sectors: 384 records. 1000 slots wrap it twice. */
    fresh(3 * EH_SECTOR);
    for (int k = 0; k < 1000; k++) fill_slot(k, (float)(k % 100), 0.0f, 0, false);
    int n = em_hist_count(NULL);
    CHECK(n >= 2 * EH_PER_SECTOR && n <= 3 * EH_PER_SECTOR);
    eh_slot_t h[96];
    CHECK_I(em_hist_range(T0 + 903LL * 900, 96, h), 96);      /* slots 903..998 */
    for (int i = 0; i < 96; i++) CHECK_F(h[i].deye_soc, (903 + i) % 100, 0.05);
    CHECK_I(em_hist_range(T0, 96, h), 0);                     /* long gone   */
    int64_t oldest;
    em_hist_count(&oldest);
    CHECK_I(oldest, T0 + (int64_t)(999 - n) * 900);
    /* After a restart the ring continues where it was. */
    reboot();
    CHECK_I(em_hist_count(NULL), n);
    fill_slot(1000, 1.0f, 0.0f, 0, false);
    fill_slot(1001, 2.0f, 0.0f, 0, false);
    /* 998 from before, 999 was running at the restart, 1000 is new. */
    CHECK_I(em_hist_range(T0 + 998LL * 900, 3, h), 2);
    CHECK_F(h[0].deye_soc, 98.0, 0.05);
    CHECK(!h[1].valid);
    CHECK_F(h[2].deye_soc, 1.0, 0.05);
}

static void test_a_torn_record_is_stepped_over(void)
{
    fresh(FLASH_MAX);
    for (int k = 0; k < 4; k++) fill_slot(k, 10.0f, 0.0f, 0, false);   /* 3 written */
    /* Power cut halfway through writing the fourth record. */
    memset(f_flash + 3 * EH_REC_SIZE, 0x00, EH_REC_SIZE / 2);
    reboot();
    CHECK_I(em_hist_count(NULL), 3);
    fill_slot(4, 44.0f, 0.0f, 0, false);
    fill_slot(5, 55.0f, 0.0f, 0, false);
    CHECK_I(em_hist_count(NULL), 4);
    eh_slot_t h[5];
    CHECK_I(em_hist_range(T0, 5, h), 4);
    CHECK_F(h[4].deye_soc, 44.0, 0.05);
    CHECK(!h[3].valid);
    reboot();
    CHECK_I(em_hist_count(NULL), 4);
}

static void test_without_flash(void)
{
    s_eh_io = NULL;
    s_eh_newest = -1;
    s_cur = 0;
    CHECK(em_hist_init() != ESP_OK);
    fill_slot(0, 10.0f, 0.0f, 0, false);
    fill_slot(1, 10.0f, 0.0f, 0, false);             /* the write fails quietly */
    eh_slot_t h[2];
    CHECK_I(em_hist_range(T0, 2, h), 0);
    CHECK(!h[0].valid);
}

static void test_range_rounds_and_clamps(void)
{
    fresh(FLASH_MAX);
    for (int k = 0; k < 5; k++) fill_slot(k, (float)k, 0.0f, 0, false);
    eh_slot_t h[2];
    CHECK_I(em_hist_range(T0 + 2 * 900 + 400, 2, h), 2);     /* rounds down to slot 2 */
    CHECK_F(h[0].deye_soc, 2.0, 0.05);
    CHECK_F(h[1].deye_soc, 3.0, 0.05);
    CHECK_I(em_hist_range(T0, 0, h), 0);
}

int main(void)
{
    RUN(test_a_slot_is_written_when_it_ends);
    RUN(test_soc_is_the_last_value_powers_the_mean);
    RUN(test_majority_action_and_unknown_values);
    RUN(test_no_plan_is_none);
    RUN(test_a_restart_keeps_the_past);
    RUN(test_the_ring);
    RUN(test_a_torn_record_is_stepped_over);
    RUN(test_without_flash);
    RUN(test_range_rounds_and_clamps);
    return t_report("em_hist");
}
