// ============================================================================
// pwr_log.c -- see pwr_log.h.
//
// The Linear11 / VOUT_MODE decoding mirrors main.c's, which was validated
// against the board's own monitor. It is repeated rather than shared because
// main.c's helpers are static inside a 5,000-line file and the alternative --
// surgery on that file -- carries more risk than these forty lines.
// ============================================================================
#include "pwr_log.h"
#include "xiicps.h"
#include "xil_printf.h"
#include "xparameters.h"
#include <string.h>

extern unsigned long long ep_timer_now(void);
extern double ep_cycles_to_ms(unsigned long long c);

#define PM_MUX_ADDR   0x74U
#define PM_MUX_CH7    0x80U
#define PM_UCD0       0x34U
#define PM_UCD1       0x35U
#define PM_UCD2       0x36U
#define PM_PAGE       0x00U
#define PM_VOUT_MODE  0x20U
#define PM_READ_VOUT  0x8BU
#define PM_READ_IOUT  0x8CU
#define PM_I2C_HZ     100000U

#if defined(XPAR_XIICPS_0_DEVICE_ID)
#  define PM_IIC_ARG  XPAR_XIICPS_0_DEVICE_ID
#elif defined(XPAR_XIICPS_0_BASEADDR)
#  define PM_IIC_ARG  XPAR_XIICPS_0_BASEADDR
#else
#  error "PS I2C0 not found in xparameters.h -- enable I2C0 in the PS."
#endif

typedef struct { const char *name; uint8_t addr, page; pwr_group_t group; } rail_t;

static const rail_t s_rails[PWR_NRAILS] = {
    { "VCCINT",   PM_UCD0, 0, PWR_PL   },
    { "VCCPINT",  PM_UCD0, 1, PWR_PS   },
    { "VCCAUX",   PM_UCD0, 2, PWR_PL   },
    { "VCCPAUX",  PM_UCD0, 3, PWR_PS   },
    { "VCCADJ",   PM_UCD1, 0, PWR_MISC },
    { "VCC1V5PS", PM_UCD1, 1, PWR_DDR  },
    { "VCC_MIO",  PM_UCD1, 2, PWR_PS   },
    { "VCCBRAM",  PM_UCD1, 3, PWR_PL   },
    { "VCC3V3",   PM_UCD2, 0, PWR_MISC },
    { "VCC2V5",   PM_UCD2, 1, PWR_MISC },
};

#define PWR_MAX_SCANS 1024
static pwr_scan_t s_buf[PWR_MAX_SCANS];
static int        s_nbuf = 0;
static float      s_scan_ms = 0.0f;
static XIicPs     s_iic;
static int        s_ready = 0;

const char *pwr_rail_name(int i)
{
    return (i >= 0 && i < PWR_NRAILS) ? s_rails[i].name : "?";
}

static void iic_wait(void) { while (XIicPs_BusIsBusy(&s_iic)) { } }

static int iic_wr(uint8_t slave, uint8_t cmd, uint8_t val)
{
    uint8_t tx[2] = { cmd, val };
    int s;
    iic_wait();
    s = XIicPs_MasterSendPolled(&s_iic, tx, 2, slave);
    iic_wait();
    return s;
}

static int iic_rd(uint8_t slave, uint8_t cmd, uint8_t *buf, int len)
{
    int s;
    iic_wait();
    XIicPs_SetOptions(&s_iic, XIICPS_REP_START_OPTION);
    s = XIicPs_MasterSendPolled(&s_iic, &cmd, 1, slave);
    XIicPs_ClearOptions(&s_iic, XIICPS_REP_START_OPTION);
    if (s != XST_SUCCESS) return s;
    s = XIicPs_MasterRecvPolled(&s_iic, buf, len, slave);
    iic_wait();
    return s;
}

static int16_t sext(uint16_t v, unsigned bits)
{
    const uint16_t sb = (uint16_t)(1U << (bits - 1U));
    const uint16_t mk = (uint16_t)((1U << bits) - 1U);
    v &= mk;
    return (int16_t)((v ^ sb) - sb);
}

static float pow2f(float v, int e)
{
    if (e > 0) { while (e-- > 0) v *= 2.0f; }
    else       { while (e++ < 0) v *= 0.5f; }
    return v;
}

int pwr_log_init(void)
{
    XIicPs_Config *cfg = XIicPs_LookupConfig(PM_IIC_ARG);
    uint8_t ch = PM_MUX_CH7;
    int s;
    if (!cfg) return -1;
    s = XIicPs_CfgInitialize(&s_iic, cfg, cfg->BaseAddress);
    if (s != XST_SUCCESS) return -2;
    s = XIicPs_SetSClk(&s_iic, PM_I2C_HZ);
    if (s != XST_SUCCESS) return -3;
    iic_wait();
    s = XIicPs_MasterSendPolled(&s_iic, &ch, 1, PM_MUX_ADDR);
    iic_wait();
    if (s != XST_SUCCESS) return -4;
    s_ready = 1;
    s_nbuf = 0;
    return 0;
}

int pwr_log_scan(pwr_scan_t *out)
{
    uint8_t b[2];
    if (!s_ready || !out) return -1;

    memset(out, 0, sizeof *out);
    out->t = ep_timer_now();
    out->ok = 1;

    for (int i = 0; i < PWR_NRAILS; i++) {
        const rail_t *r = &s_rails[i];
        float v = 0.0f, a = 0.0f;
        int8_t vexp;

        if (iic_wr(r->addr, PM_PAGE, r->page) != XST_SUCCESS) { out->ok = 0; continue; }

        /* VOUT_MODE carries the per-page exponent for the ULINEAR16 voltage */
        if (iic_rd(r->addr, PM_VOUT_MODE, b, 1) != XST_SUCCESS) { out->ok = 0; continue; }
        vexp = (int8_t)sext((uint16_t)b[0], 5);

        if (iic_rd(r->addr, PM_READ_VOUT, b, 2) != XST_SUCCESS) { out->ok = 0; continue; }
        v = pow2f((float)((uint16_t)b[0] | ((uint16_t)b[1] << 8)), vexp);

        /* IOUT is Linear11: 5-bit exponent in [15:11], 11-bit mantissa */
        if (iic_rd(r->addr, PM_READ_IOUT, b, 2) != XST_SUCCESS) { out->ok = 0; continue; }
        {
            const uint16_t raw = (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
            const int      ie  = sext((uint16_t)(raw >> 11), 5);
            const int16_t  im  = sext(raw, 11);
            a = pow2f((float)im, ie);
        }

        out->rail_w[i] = v * a;
        out->group_w[r->group] += out->rail_w[i];
        out->total_w += out->rail_w[i];
    }

    s_scan_ms = (float)ep_cycles_to_ms(ep_timer_now() - out->t);
    return out->ok ? 0 : -2;
}

int pwr_log_collect(unsigned seconds, unsigned interval_ms)
{
    const unsigned long long t0 = ep_timer_now();
    int n = 0;
    if (!s_ready) return -1;
    s_nbuf = 0;
    while (ep_cycles_to_ms(ep_timer_now() - t0) < (double)seconds * 1000.0) {
        if (s_nbuf >= PWR_MAX_SCANS) break;
        if (pwr_log_scan(&s_buf[s_nbuf]) == 0) { s_nbuf++; n++; }
        /* spin out the interval; a busy wait is fine, this pass is not timed */
        {
            const unsigned long long tw = ep_timer_now();
            while (ep_cycles_to_ms(ep_timer_now() - tw) < (double)interval_ms) { }
        }
    }
    return n;
}

void pwr_log_summary(pwr_summary_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->n = s_nbuf;
    out->scan_ms = s_scan_ms;
    if (s_nbuf <= 0) return;

    double sum = 0.0;
    out->min_w = out->max_w = s_buf[0].total_w;
    for (int i = 0; i < s_nbuf; i++) {
        const float w = s_buf[i].total_w;
        sum += w;
        if (w < out->min_w) out->min_w = w;
        if (w > out->max_w) out->max_w = w;
        for (int g = 0; g < PWR_NGROUP; g++) out->group_mean_w[g] += s_buf[i].group_w[g];
    }
    out->mean_w = (float)(sum / s_nbuf);
    for (int g = 0; g < PWR_NGROUP; g++) out->group_mean_w[g] /= (float)s_nbuf;

    double var = 0.0;
    for (int i = 0; i < s_nbuf; i++) {
        const double d = s_buf[i].total_w - out->mean_w;
        var += d * d;
    }
    if (s_nbuf > 1) {
        var /= (double)(s_nbuf - 1);
        /* integer-friendly sqrt; no libm dependency in this build */
        double r = var > 1.0 ? var : 1.0;
        for (int k = 0; k < 40; k++) r = 0.5 * (r + var / r);
        out->sd_w = (float)r;
        r = (double)s_nbuf; { double q = r > 1.0 ? r : 1.0;
            for (int k = 0; k < 40; k++) q = 0.5 * (q + r / q);
            out->se_w = (float)(out->sd_w / q); }
    }
}

void pwr_log_print(const pwr_summary_t *s)
{
    static const char *gname[PWR_NGROUP] = { "PL", "PS", "DDR", "MISC" };
    if (!s) return;
    printf("#PWR,scans,%d,scan_ms,%.2f\n", s->n, (double)s->scan_ms);
    printf("#PWR,mean_w,%.4f,sd_w,%.4f,se_w,%.4f,min_w,%.4f,max_w,%.4f\n",
           (double)s->mean_w, (double)s->sd_w, (double)s->se_w,
           (double)s->min_w, (double)s->max_w);
    for (int g = 0; g < PWR_NGROUP; g++)
        printf("#PWR,group,%s,%.4f\n", gname[g], (double)s->group_mean_w[g]);
    printf("#PWR,NOTE,quote the run-to-run spread (~+/-0.05 W), not se_w\n");
    printf("#PWR,NOTE,no per-stage attribution: one scan is %.1f ms vs a 2.45 ms VQ stage\n",
           (double)s->scan_ms);
}

double pwr_energy_per_frame_mj(float mean_w, double ii_ms)
{
    return (double)mean_w * ii_ms;   /* W * ms = mJ */
}
