#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/msg.h>
#include <sys/types.h>
#include "estructuras.h"

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

typedef struct {
    int account_num;
    int tipo_op;
    float cantidad;
    int divisa;
    int cuenta_destino;
    int msgid;
} DadosOp;

static Config cfg;
static sem_t *sem_cuentas = NULL;
static sem_t *sem_config = NULL;
static int g_account = 0;

static void get_timestamp(char *buf) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, 20, "%Y-%m-%d %H:%M:%S", &tm);
}

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

static float withdrawal_limit(int divisa) {
    if (divisa == 0) return cfg.lim_ret_eur;
    if (divisa == 1) return cfg.lim_ret_usd;
    return cfg.lim_ret_gbp;
}

static float transfer_limit(int divisa) {
    if (divisa == 0) return cfg.lim_trf_eur;
    if (divisa == 1) return cfg.lim_trf_usd;
    return cfg.lim_trf_gbp;
}

static void clear_screen(void) {
    printf("\033[H\033[2J");
    fflush(stdout);
}

static void print_header(void) {
    printf("SecureBank - account %d\n", g_account);
    printf("--------------------------------\n");
}

static void show_menu(void) {
    printf("\nMenu:\n");
    printf("1. Deposit\n");
    printf("2. Withdraw\n");
    printf("3. Transfer\n");
    printf("4. Check balance\n");
    printf("5. Move currency\n");
    printf("6. Exit\n");
    printf("Option: ");
    fflush(stdout);
}

/* scan cuentas.dat for a record by account number; return its index or -1 */
static int find_record_index(FILE *f, int account_num) {
    fseek(f, 0, SEEK_SET);
    Cuenta tmp;
    int idx = 0;
    while (fread(&tmp, sizeof(tmp), 1, f) == 1) {
        if (tmp.numero_cuenta == account_num) return idx;
        idx++;
    }
    return -1;
}

static void *ejecutar_operacion(void *arg) {
    DadosOp *op = (DadosOp *)arg;
    int ok = 1;
    Cuenta c;
    memset(&c, 0, sizeof(c));

    sem_wait(sem_cuentas);

    FILE *f = fopen(cfg.archivo_cuentas, "rb+");
    if (!f) {
        sem_post(sem_cuentas);
        printf("Cannot open accounts file\n");
        free(op);
        return NULL;
    }

    int src_idx = find_record_index(f, op->account_num);
    long offset = (long)src_idx * sizeof(Cuenta);
    if (src_idx < 0) {
        printf("Cannot read account record\n");
        ok = 0;
    } else {
        fseek(f, offset, SEEK_SET);
        if (fread(&c, sizeof(c), 1, f) != 1) {
            printf("Cannot read account record\n");
            ok = 0;
        }
    }
    if (ok) {
        float *saldos[3] = {&c.saldo_eur, &c.saldo_usd, &c.saldo_gbp};

        switch (op->tipo_op) {
            case 1:
                *saldos[op->divisa] += op->cantidad;
                c.num_transacciones++;
                printf("Deposit ok. New balance: %.2f %s\n",
                       *saldos[op->divisa],
                       op->divisa == 0 ? "EUR" : op->divisa == 1 ? "USD" : "GBP");
                break;

            case 2: {
                float lim = withdrawal_limit(op->divisa);
                if (op->cantidad > lim) {
                    printf("Amount exceeds withdrawal limit (%.2f)\n", lim);
                    ok = 0;
                } else if (*saldos[op->divisa] < op->cantidad) {
                    printf("Insufficient funds\n");
                    ok = 0;
                } else {
                    *saldos[op->divisa] -= op->cantidad;
                    c.num_transacciones++;
                    printf("Withdrawal ok. New balance: %.2f\n", *saldos[op->divisa]);
                }
                break;
            }

            case 3: {
                float lim = transfer_limit(op->divisa);
                if (op->cantidad > lim) {
                    printf("Amount exceeds transfer limit (%.2f)\n", lim);
                    ok = 0; break;
                }
                if (*saldos[op->divisa] < op->cantidad) {
                    printf("Insufficient funds\n");
                    ok = 0; break;
                }
                int dst_idx = find_record_index(f, op->cuenta_destino);
                if (dst_idx < 0) {
                    printf("Destination account not found\n");
                    ok = 0; break;
                }
                long offset_d = (long)dst_idx * sizeof(Cuenta);
                Cuenta d;
                fseek(f, offset_d, SEEK_SET);
                if (fread(&d, sizeof(d), 1, f) != 1) {
                    printf("Destination account not found\n");
                    ok = 0; break;
                }
                *saldos[op->divisa] -= op->cantidad;
                c.num_transacciones++;
                float *d_saldos[3] = {&d.saldo_eur, &d.saldo_usd, &d.saldo_gbp};
                *d_saldos[op->divisa] += op->cantidad;
                d.num_transacciones++;
                fseek(f, offset_d, SEEK_SET);
                fwrite(&d, sizeof(d), 1, f);
                printf("Transfer ok: %d -> %d %.2f %s\n",
                       op->account_num, op->cuenta_destino, op->cantidad,
                       op->divisa == 0 ? "EUR" : op->divisa == 1 ? "USD" : "GBP");
                break;
            }

            case 4:
                printf("Account %d balances:\n", c.numero_cuenta);
                printf("  EUR: %.2f\n", c.saldo_eur);
                printf("  USD: %.2f\n", c.saldo_usd);
                printf("  GBP: %.2f\n", c.saldo_gbp);
                {
                    float total_eur = c.saldo_eur;
                    if (cfg.cambio_usd > 0) total_eur += c.saldo_usd / cfg.cambio_usd;
                    if (cfg.cambio_gbp > 0) total_eur += c.saldo_gbp / cfg.cambio_gbp;
                    printf("  Total in EUR: %.2f\n", total_eur);
                }
                break;

            case 5: {
                int from = op->divisa;
                int to = op->cuenta_destino;
                if (from == to || from < 0 || from > 2 || to < 0 || to > 2) {
                    printf("Invalid currency pair\n"); ok = 0; break;
                }
                if (*saldos[from] < op->cantidad) {
                    printf("Insufficient funds in source currency\n"); ok = 0; break;
                }
                float eur_amt = op->cantidad;
                if (from == 1) eur_amt = op->cantidad / cfg.cambio_usd;
                else if (from == 2) eur_amt = op->cantidad / cfg.cambio_gbp;
                float to_amt = eur_amt;
                if (to == 1) to_amt = eur_amt * cfg.cambio_usd;
                else if (to == 2) to_amt = eur_amt * cfg.cambio_gbp;
                *saldos[from] -= op->cantidad;
                *saldos[to] += to_amt;
                c.num_transacciones++;
                printf("Moved %.2f -> %.2f\n", op->cantidad, to_amt);
                break;
            }

            default:
                printf("Invalid operation\n"); ok = 0;
        }

        if (ok && op->tipo_op != 4) {
            fseek(f, offset, SEEK_SET);
            fwrite(&c, sizeof(c), 1, f);
        }
    }
    fclose(f);
    sem_post(sem_cuentas);

    char ts[20];
    get_timestamp(ts);

    if (op->tipo_op != 4) {
        struct msgbuf m;
        memset(&m, 0, sizeof(m));
        m.mtype = 1;
        m.info.monitor.cuenta_origen = op->account_num;
        m.info.monitor.cuenta_destino = (op->tipo_op == 3) ? op->cuenta_destino : (int)getpid();
        m.info.monitor.tipo_op = op->tipo_op;
        m.info.monitor.cantidad = op->cantidad;
        m.info.monitor.divisa = op->divisa;
        strncpy(m.info.monitor.timestamp, ts, 19);
        m.info.monitor.timestamp[19] = '\0';
        if (msgsnd(op->msgid, &m, sizeof(m.info), 0) < 0) perror("msgsnd monitor");
    }

    {
        struct msgbuf m;
        memset(&m, 0, sizeof(m));
        m.mtype = 2;
        m.info.log.cuenta_id = op->account_num;
        m.info.log.pid_hijo = getpid();
        m.info.log.tipo_op = op->tipo_op;
        m.info.log.cantidad = op->cantidad;
        m.info.log.divisa = op->divisa;
        if (op->tipo_op == 3)
            m.info.log.estado = ok ? op->cuenta_destino : 0;
        else
            m.info.log.estado = ok ? 1 : 0;
        strncpy(m.info.log.timestamp, ts, 19);
        m.info.log.timestamp[19] = '\0';
        if (msgsnd(op->msgid, &m, sizeof(m.info), 0) < 0) perror("msgsnd log");
    }

    free(op);
    return NULL;
}

static int read_int(const char *prompt, int *out) {
    printf("%s", prompt);
    fflush(stdout);
    if (scanf("%d", out) != 1) {
        int c; while ((c = getchar()) != '\n' && c != EOF);
        return 0;
    }
    return 1;
}

static int read_float(const char *prompt, float *out) {
    printf("%s", prompt);
    fflush(stdout);
    if (scanf("%f", out) != 1) {
        int c; while ((c = getchar()) != '\n' && c != EOF);
        return 0;
    }
    return 1;
}

static const char *op_title(int opcion) {
    switch (opcion) {
        case 1: return "Deposit";
        case 2: return "Withdraw";
        case 3: return "Transfer";
        case 4: return "Balance query";
        case 5: return "Move currency";
    }
    return "Operation";
}

static void procesar_opcion(int opcion, int account, int msgid) {
    if (opcion < 1 || opcion > 5) {
        printf("Invalid option\n");
        return;
    }

    DadosOp *op = malloc(sizeof(DadosOp));
    if (!op) return;
    memset(op, 0, sizeof(*op));
    op->account_num = account;
    op->msgid = msgid;
    op->tipo_op = opcion;

    print_header();
    printf("%s\n\n", op_title(opcion));

    int ok_in = 1;
    switch (opcion) {
        case 1: case 2:
            if (!read_float("Amount: ", &op->cantidad)) ok_in = 0;
            if (ok_in && !read_int("Currency (0=EUR 1=USD 2=GBP): ", &op->divisa)) ok_in = 0;
            break;
        case 3:
            if (!read_int("Destination account: ", &op->cuenta_destino)) ok_in = 0;
            if (ok_in && !read_float("Amount: ", &op->cantidad)) ok_in = 0;
            if (ok_in && !read_int("Currency (0=EUR 1=USD 2=GBP): ", &op->divisa)) ok_in = 0;
            break;
        case 4:
            break;
        case 5:
            if (!read_int("From currency (0=EUR 1=USD 2=GBP): ", &op->divisa)) ok_in = 0;
            if (ok_in && !read_int("To currency (0=EUR 1=USD 2=GBP): ", &op->cuenta_destino)) ok_in = 0;
            if (ok_in && !read_float("Amount: ", &op->cantidad)) ok_in = 0;
            break;
    }
    if (!ok_in) { free(op); printf("Invalid input\n"); return; }

    printf("\n");

    pthread_t t;
    if (pthread_create(&t, NULL, ejecutar_operacion, op) != 0) {
        perror("pthread_create"); free(op); return;
    }
    pthread_join(t, NULL);
}

static void handle_term(int sig) {
    (void)sig;
    printf("\nSession terminated by signal\n");
    _exit(0);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <account> <msgid> <fifo_path>\n", argv[0]);
        return 1;
    }
    int account = atoi(argv[1]);
    int msgid = atoi(argv[2]);
    const char *fifo_path = argv[3];
    g_account = account;

    signal(SIGHUP, handle_term);
    signal(SIGTERM, handle_term);
    signal(SIGPIPE, SIG_IGN);

    leer_config();

    sem_cuentas = sem_open("/sem_cuentas", O_CREAT, 0666, 1);
    sem_config = sem_open("/sem_config", O_CREAT, 0666, 1);
    if (sem_cuentas == SEM_FAILED || sem_config == SEM_FAILED) {
        perror("sem_open"); return 1;
    }

    int pipe_fd = open(fifo_path, O_RDONLY);
    if (pipe_fd < 0) { perror("open fifo"); return 1; }

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO; fds[0].events = POLLIN;
    fds[1].fd = pipe_fd;      fds[1].events = POLLIN;

    clear_screen();
    print_header();
    show_menu();

    while (1) {
        int r = poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("poll"); break;
        }

        if (fds[1].revents & POLLIN) {
            char buf[256];
            ssize_t n = read(pipe_fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                printf("\n[BANK ALERT]: %s\n", buf);
                if (strstr(buf, "BLOQUEO")) {
                    printf("Account blocked. Terminating session in 2 seconds.\n");
                    fflush(stdout);
                    sleep(2);
                    exit(0);
                }
                show_menu();
            }
        }

        if (fds[0].revents & POLLIN) {
            int opcion;
            if (scanf("%d", &opcion) != 1) {
                int c; while ((c = getchar()) != '\n' && c != EOF);
                clear_screen();
                print_header();
                printf("Invalid input\n");
                show_menu();
                continue;
            }
            if (opcion == 6) {
                clear_screen();
                print_header();
                printf("\nSession closed. Goodbye.\n");
                fflush(stdout);
                sleep(1);
                exit(0);
            }
            clear_screen();
            procesar_opcion(opcion, account, msgid);
            show_menu();
        }
    }

    return 0;
}
