#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/msg.h>
#include <sys/types.h>
#include "estructuras.h"

#define HIST_SIZE 50
#define MAX_ACCOUNTS 1000

typedef struct {
    int proximo_id;
    float lim_ret_eur, lim_ret_usd, lim_ret_gbp;
    float lim_trf_eur, lim_trf_usd, lim_trf_gbp;
    int umbral_retiros;
    int umbral_transferencias;
    int num_hilos;
    char archivo_cuentas[128];
    char archivo_log[128];
    float cambio_usd;
    float cambio_gbp;
} Config;

static Config cfg;
static volatile sig_atomic_t stop_flag = 0;
static int msgid = -1;

/* circular history */
static DatosMonitor history[HIST_SIZE];
static int history_idx = 0;
static int history_count = 0;

/* consecutive large-withdrawal counter per account index */
static int withdraw_counter[MAX_ACCOUNTS];

static void leer_config(void) {
    FILE *f = fopen("config.txt", "r");
    if (!f) { perror("config.txt"); exit(1); }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64], val[128];
        if (sscanf(line, "%63[^=]=%127s", key, val) != 2) continue;
        if (!strcmp(key, "PROXIMO_ID")) cfg.proximo_id = atoi(val);
        else if (!strcmp(key, "LIM_RET_EUR")) cfg.lim_ret_eur = atof(val);
        else if (!strcmp(key, "LIM_RET_USD")) cfg.lim_ret_usd = atof(val);
        else if (!strcmp(key, "LIM_RET_GBP")) cfg.lim_ret_gbp = atof(val);
        else if (!strcmp(key, "LIM_TRF_EUR")) cfg.lim_trf_eur = atof(val);
        else if (!strcmp(key, "LIM_TRF_USD")) cfg.lim_trf_usd = atof(val);
        else if (!strcmp(key, "LIM_TRF_GBP")) cfg.lim_trf_gbp = atof(val);
        else if (!strcmp(key, "UMBRAL_RETIROS")) cfg.umbral_retiros = atoi(val);
        else if (!strcmp(key, "UMBRAL_TRANSFERENCIAS")) cfg.umbral_transferencias = atoi(val);
        else if (!strcmp(key, "NUM_HILOS")) cfg.num_hilos = atoi(val);
        else if (!strcmp(key, "ARCHIVO_CUENTAS")) snprintf(cfg.archivo_cuentas, sizeof(cfg.archivo_cuentas), "%s", val);
        else if (!strcmp(key, "ARCHIVO_LOG")) snprintf(cfg.archivo_log, sizeof(cfg.archivo_log), "%s", val);
        else if (!strcmp(key, "CAMBIO_USD")) cfg.cambio_usd = atof(val);
        else if (!strcmp(key, "CAMBIO_GBP")) cfg.cambio_gbp = atof(val);
    }
    fclose(f);
}

static void handle_sigterm(int sig) {
    (void)sig;
    stop_flag = 1;
}

/* send alert to banco. reason fits in DatosLog.timestamp (19 chars + null). */
static void send_alert(int account_num, const char *reason) {
    struct msgbuf m;
    memset(&m, 0, sizeof(m));
    m.mtype = 3;
    m.info.log.cuenta_id = account_num;
    m.info.log.pid_hijo = getpid();
    m.info.log.tipo_op = -1;
    m.info.log.cantidad = 0;
    m.info.log.divisa = 0;
    m.info.log.estado = 1;
    strncpy(m.info.log.timestamp, reason, 19);
    m.info.log.timestamp[19] = '\0';
    if (msgsnd(msgid, &m, sizeof(m.info), 0) < 0) perror("msgsnd alert");
    printf("[monitor] alert: account %d: %s\n", account_num, reason);
}

static float withdrawal_threshold(int divisa) {
    if (divisa == 0) return cfg.lim_ret_eur;
    if (divisa == 1) return cfg.lim_ret_usd;
    return cfg.lim_ret_gbp;
}

static void analizar_transaccion(DatosMonitor *d) {
    int idx = d->cuenta_origen - ID_INICIAL;
    if (idx < 0 || idx >= MAX_ACCOUNTS) return;

    /* (a) consecutive large withdrawals */
    if (d->tipo_op == 2) {
        float lim = withdrawal_threshold(d->divisa);
        if (d->cantidad >= lim * 0.8f) {
            withdraw_counter[idx]++;
            if (withdraw_counter[idx] >= cfg.umbral_retiros) {
                send_alert(d->cuenta_origen, "consec withdrawals");
                withdraw_counter[idx] = 0;
            }
        } else {
            withdraw_counter[idx] = 0;
        }
    } else if (d->tipo_op != 4) {
        /* any other write op breaks the withdrawal streak */
        withdraw_counter[idx] = 0;
    }

    /* (b) repeated transfers between same pair */
    if (d->tipo_op == 3) {
        int matches = 1;
        int lookup = history_count < HIST_SIZE ? history_count : HIST_SIZE;
        for (int i = 0; i < lookup; i++) {
            int pos = (history_idx - 1 - i + HIST_SIZE) % HIST_SIZE;
            if (history[pos].tipo_op == 3 &&
                history[pos].cuenta_origen == d->cuenta_origen &&
                history[pos].cuenta_destino == d->cuenta_destino) {
                matches++;
            }
        }
        if (matches >= cfg.umbral_transferencias) {
            send_alert(d->cuenta_origen, "repeat transfers");
        }
    }

    /* (c) same account, different pid, recent (for non-transfer only) */
    if (d->tipo_op != 3) {
        pid_t pid = (pid_t)d->cuenta_destino;
        int lookup = history_count < 5 ? history_count : 5;
        for (int i = 0; i < lookup; i++) {
            int pos = (history_idx - 1 - i + HIST_SIZE) % HIST_SIZE;
            if (history[pos].cuenta_origen == d->cuenta_origen &&
                history[pos].tipo_op != 3) {
                pid_t other = (pid_t)history[pos].cuenta_destino;
                if (other != 0 && other != pid) {
                    send_alert(d->cuenta_origen, "concurrent access");
                    break;
                }
            }
        }
    }

    /* store in history */
    history[history_idx] = *d;
    history_idx = (history_idx + 1) % HIST_SIZE;
    if (history_count < HIST_SIZE) history_count++;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: monitor <msgid>\n");
        return 1;
    }
    msgid = atoi(argv[1]);

    signal(SIGTERM, handle_sigterm);

    leer_config();
    memset(history, 0, sizeof(history));
    memset(withdraw_counter, 0, sizeof(withdraw_counter));

    printf("[monitor] started with msgid=%d\n", msgid);

    struct msgbuf m;
    while (!stop_flag) {
        ssize_t r = msgrcv(msgid, &m, sizeof(m.info), 1, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EIDRM) break;
            perror("msgrcv");
            break;
        }
        analizar_transaccion(&m.info.monitor);
    }

    printf("[monitor] stopping\n");
    return 0;
}
