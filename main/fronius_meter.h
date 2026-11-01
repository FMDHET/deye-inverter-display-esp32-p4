#pragma once

/* The Deye as a Fronius Smart Meter of category "Erzeuger" (producer), over
 * Modbus TCP.
 *
 * A Fronius Symo Hybrid sees the house only through its own grid meter: what
 * the AC-coupled Deye feeds into the house (or draws from it to charge) is
 * missing from its consumption figures. The Fronius can take SECONDARY meters
 * over Modbus TCP, though, and counts the feed-in of a producer meter as
 * production. So the display answers as such a meter, with the Deye's own AC
 * measurement (registers 627..638, already polled on the Deye master bus):
 *     + = the Deye draws from the house (charging),
 *     - = the Deye feeds into the house (discharging / its own PV).
 * The Fronius' grid meter and with it every limit it enforces stay untouched.
 *
 * Register image, start sequence and timing are those of the real Fronius
 * Smart Meter as recorded and verified for The Bridge (Fronius_SM_Reader,
 * docs/fronius_smartmeter_emulation.md). The serial number is configurable
 * (frm_serial / frm_serial_txt) and defaults to The Bridge's SECOND-meter one,
 * 158028U / 829686240 -- the Fronius drops a meter whose serial number equals
 * one it already has (the real grid meter here is 829686239).
 *
 * No socket of its own: the Modbus-TCP bridge (modbus_gw.c) owns the listener
 * and hands over every request addressed to the meter's unit id. Configured in
 * mb_rtu_cfg_t (frm_*), edited in the "Fronius-Simulator" tab of /deye.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRM_DEFAULT_UNIT  2

/* Default serial number: The Bridge's SECOND meter, tested on a Symo. The
 * Fronius drops a meter whose number it already has, so a second producer
 * meter (or one already used by a Bridge) needs a different one. */
#define FRM_DEFAULT_SERIAL      829686240u
#define FRM_DEFAULT_SERIAL_TXT  "158028U"
#define FRM_SERIAL_TXT_LEN      7

/* True for a serial text of exactly FRM_SERIAL_TXT_LEN letters/digits -- the
 * shape of the real meter's. Anything else is not stored. */
static inline bool frm_serial_txt_ok(const char *t)
{
    for (int i = 0; i < FRM_SERIAL_TXT_LEN; i++) {
        char ch = t[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')))
            return false;
    }
    return t[FRM_SERIAL_TXT_LEN] == '\0';
}

/* Deye values older than this are not served: the meter then stays SILENT, the
 * way the real one would after losing its measurement, instead of repeating a
 * frozen number. The Deye master bus refreshes them every ~2.5 s. */
#define FRM_STALE_MS      10000

/* Answer one Modbus PDU ([fc][...], no MBAP/unit) addressed to the meter.
 * Writes the response PDU to `out` and returns its length, or 0 when nothing
 * must be sent (stale data, or not a well-formed request). `client_ip` is the
 * peer in network byte order, for the status only. */
int  fronius_meter_answer(const uint8_t *pdu, int len, uint8_t *out, int out_max,
                          uint32_t client_ip);

/* Called about every 500 ms by the bridge task: integrates the energy counters
 * from new Deye rounds and saves them to NVS now and then. Cheap when off. */
void fronius_meter_tick(void);

typedef struct {
    bool     enabled;      /* frm_enabled                                    */
    uint8_t  unit;         /* frm_unit in force                              */
    uint32_t serial;       /* serial number reported (number)                */
    char     serial_txt[8];/* ... and as text                                */
    bool     fresh;        /* Deye values usable -> requests get answered    */
    uint32_t requests;     /* requests to the meter's unit id since boot     */
    uint32_t answered;     /* answered with data                             */
    uint32_t exceptions;   /* answered with a Modbus exception               */
    uint32_t silent;       /* not answered because the values were stale     */
    uint32_t age_ms;       /* since the last request (0 = never)             */
    uint32_t client_ip;    /* last requesting peer, network byte order       */
    float    p[3], p_total;/* active power served (W), + = Deye draws        */
    double   import_kwh;   /* energy the Deye drew from the house            */
    double   export_kwh;   /* energy the Deye fed into the house             */
} fronius_meter_status_t;

void fronius_meter_get_status(fronius_meter_status_t *out);

#ifdef __cplusplus
}
#endif
