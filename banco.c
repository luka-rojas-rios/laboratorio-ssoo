#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <semaphore.h>
#include <sys/wait.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "estructuras.h"

#define MAX_HIJOS 10

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
    pid_t pid;             /* pid of the launched gnome-terminal --wait */
    int pipe_write_fd;     /* fifo write end opened by banco */
    int account_num;
    char fifo_path[128];   /* fifo on disk; unlinked on reap */
} HijoInfo;

static int msgid = -1;
static pid_t monitor_pid = 0;
static Config cfg;
static HijoInfo hijos[MAX_HIJOS];
static sem_t *sem_cuentas = NULL;
static sem_t *sem_config = NULL;
static volatile sig_atomic_t terminate_flag = 0;
static int session_seq = 0;

static void get_timestamp(char *buf) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, 20, "%Y-%m-%d %H:%M:%S", &tm);
}

static const char *divisa_name(int d) {
    if (d == 0) return "EUR";
    if (d == 1) return "USD";
    if (d == 2) return "GBP";
    return "???";
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

static void reescribir_config(int nuevo_id) {
    FILE *in = fopen("config.txt", "r");
    if (!in) return;
    char tmpname[] = "config.txt.tmp";
    FILE *out = fopen(tmpname, "w");
    if (!out) { fclose(in); return; }
    char line[256];
    while (fgets(line, sizeof(line), in)) {
        if (!strncmp(line, "PROXIMO_ID=", 11))
            fprintf(out, "PROXIMO_ID=%d\n", nuevo_id);
        else
            fputs(line, out);
    }
    fclose(in);
    fclose(out);
    rename(tmpname, "config.txt");
}

static void write_log(const char *entry) {
    FILE *f = fopen(cfg.archivo_log, "a");
    if (!f) return;
    char ts[20];
    get_timestamp(ts);
    fprintf(f, "[%s] %s\n", ts, entry);
    fclose(f);
}

static void format_and_log(const DatosLog *d) {
    char buf[256];
    const char *div = divisa_name(d->divisa);
    switch (d->tipo_op) {
        case 1:
            snprintf(buf, sizeof(buf), "Deposit account %d: +%.2f %s%s",
                     d->cuenta_id, d->cantidad, div, d->estado ? "" : " (failed)");
            break;
        case 2:
            snprintf(buf, sizeof(buf), "Withdrawal account %d: -%.2f %s%s",
                     d->cuenta_id, d->cantidad, div, d->estado ? "" : " (failed)");
            break;
        case 3:
            if (d->estado >= ID_INICIAL)
                snprintf(buf, sizeof(buf), "Transfer %d -> %d: %.2f %s",
                         d->cuenta_id, d->estado, d->cantidad, div);
            else
                snprintf(buf, sizeof(buf), "Transfer from account %d: %.2f %s (failed)",
                         d->cuenta_id, d->cantidad, div);
            break;
        case 4:
            snprintf(buf, sizeof(buf), "Balance query account %d", d->cuenta_id);
            break;
        case 5:
            snprintf(buf, sizeof(buf), "Currency move account %d: %.2f %s%s",
                     d->cuenta_id, d->cantidad, div, d->estado ? "" : " (failed)");
            break;
        default:
            snprintf(buf, sizeof(buf), "Unknown op account %d", d->cuenta_id);
    }
    write_log(buf);
}

static void format_and_log_alert(const DatosLog *d) {
    char buf[256];
    char reason[32];
    strncpy(reason, d->timestamp, 19);
    reason[19] = '\0';
    snprintf(buf, sizeof(buf), "ALERT account %d: %s", d->cuenta_id, reason);
    write_log(buf);
}

static int buscar_cuenta(int num) {
    FILE *f = fopen(cfg.archivo_cuentas, "rb");
    if (!f) return -1;
    Cuenta c;
    int idx = 0;
    while (fread(&c, sizeof(c), 1, f) == 1) {
        if (c.numero_cuenta == num) { fclose(f); return idx; }
        idx++;
    }
    fclose(f);
    return -1;
}

static int crear_cuenta(const char *nombre) {
    sem_wait(sem_config);
    sem_wait(sem_cuentas);

    int nuevo_id = cfg.proximo_id;
    FILE *f = fopen(cfg.archivo_cuentas, "ab");
    if (!f) { sem_post(sem_cuentas); sem_post(sem_config); return -1; }
    Cuenta c;
    memset(&c, 0, sizeof(c));
    c.numero_cuenta = nuevo_id;
    snprintf(c.titular, sizeof(c.titular), "%s", nombre);
    fseek(f, 0, SEEK_END);
    fwrite(&c, sizeof(c), 1, f);
    fclose(f);

    cfg.proximo_id = nuevo_id + 1;
    reescribir_config(cfg.proximo_id);

    sem_post(sem_cuentas);
    sem_post(sem_config);
    return nuevo_id;
}

static void ensure_cuentas_file(void) {
    struct stat st;
    int exists = (stat(cfg.archivo_cuentas, &st) == 0);
    if (!exists) {
        FILE *f = fopen(cfg.archivo_cuentas, "wb");
        if (f) fclose(f);
        st.st_size = 0;
    }
    /* if accounts file is empty, reset PROXIMO_ID so new accounts start at 1001 */
    if (st.st_size == 0 && cfg.proximo_id != ID_INICIAL) {
        cfg.proximo_id = ID_INICIAL;
        reescribir_config(cfg.proximo_id);
    }
}

/* print every record in cuentas.dat */
static void listar_cuentas(void) {
    sem_wait(sem_cuentas);
    FILE *f = fopen(cfg.archivo_cuentas, "rb");
    if (!f) { sem_post(sem_cuentas); printf("No accounts file\n"); return; }
    Cuenta c;
    int count = 0;
    printf("\nAccounts registered:\n");
    printf("  %-8s %-20s %10s %10s %10s %6s\n",
           "Number", "Holder", "EUR", "USD", "GBP", "#ops");
    printf("  -----------------------------------------------------------------------\n");
    while (fread(&c, sizeof(c), 1, f) == 1) {
        printf("  %-8d %-20.20s %10.2f %10.2f %10.2f %6d\n",
               c.numero_cuenta, c.titular,
               c.saldo_eur, c.saldo_usd, c.saldo_gbp, c.num_transacciones);
        count++;
    }
    fclose(f);
    sem_post(sem_cuentas);
    if (count == 0) printf("  (no accounts yet)\n");
    else printf("Total: %d account(s)\n", count);
}

static int find_free_slot(void) {
    for (int i = 0; i < MAX_HIJOS; i++)
        if (hijos[i].pid == 0) return i;
    return -1;
}

static int find_hijo_by_pid(pid_t pid) {
    for (int i = 0; i < MAX_HIJOS; i++)
        if (hijos[i].pid == pid) return i;
    return -1;
}

static int find_hijo_by_account(int account) {
    for (int i = 0; i < MAX_HIJOS; i++)
        if (hijos[i].pid != 0 && hijos[i].account_num == account)
            return i;
    return -1;
}

static void free_hijo_slot(int i) {
    if (hijos[i].pipe_write_fd > 0) close(hijos[i].pipe_write_fd);
    if (hijos[i].fifo_path[0]) unlink(hijos[i].fifo_path);
    hijos[i].pid = 0;
    hijos[i].pipe_write_fd = -1;
    hijos[i].account_num = 0;
    hijos[i].fifo_path[0] = '\0';
}

/* launch a new usuario session in its own gnome-terminal window */
static int launch_session(int account_num) {
    int slot = find_free_slot();
    if (slot < 0) { printf("No free session slots\n"); return -1; }

    char fifo_path[128];
    snprintf(fifo_path, sizeof(fifo_path), "/tmp/securebank_%d_%d",
             (int)getpid(), session_seq++);
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0666) < 0) { perror("mkfifo"); return -1; }

    /* O_RDWR so our write end is always valid and open() does not block */
    int wfd = open(fifo_path, O_RDWR);
    if (wfd < 0) { perror("open fifo"); unlink(fifo_path); return -1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(wfd); unlink(fifo_path); return -1; }

    if (pid == 0) {
        /* child: launch a new terminal window running usuario.
           gnome-terminal inherits the server's cwd, so pass our own explicitly. */
        char acc_s[16], msg_s[16], title[64];
        char cwd[256], workdir[300];
        snprintf(acc_s, sizeof(acc_s), "%d", account_num);
        snprintf(msg_s, sizeof(msg_s), "%d", msgid);
        snprintf(title, sizeof(title), "SecureBank - account %d", account_num);
        if (!getcwd(cwd, sizeof(cwd))) { perror("getcwd"); _exit(1); }
        snprintf(workdir, sizeof(workdir), "--working-directory=%s", cwd);
        char *args[] = {
            "gnome-terminal", "--wait", workdir, "--title", title,
            "--", "./usuario", acc_s, msg_s, fifo_path, NULL
        };
        execvp("gnome-terminal", args);
        perror("execvp gnome-terminal");
        _exit(1);
    }

    /* parent */
    hijos[slot].pid = pid;
    hijos[slot].pipe_write_fd = wfd;
    hijos[slot].account_num = account_num;
    snprintf(hijos[slot].fifo_path, sizeof(hijos[slot].fifo_path), "%s", fifo_path);
    return 0;
}

static void lanzar_monitor(void) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork monitor"); exit(1); }
    if (pid == 0) {
        char msg_s[16];
        snprintf(msg_s, sizeof(msg_s), "%d", msgid);
        char *args[] = {"./monitor", msg_s, NULL};
        execv("./monitor", args);
        perror("execv monitor");
        _exit(1);
    }
    monitor_pid = pid;
}

static void cleanup(void) {
    if (monitor_pid > 0) {
        kill(monitor_pid, SIGTERM);
        waitpid(monitor_pid, NULL, 0);
        monitor_pid = 0;
    }
    for (int i = 0; i < MAX_HIJOS; i++) {
        if (hijos[i].pid > 0) {
            kill(hijos[i].pid, SIGTERM);
            waitpid(hijos[i].pid, NULL, 0);
            free_hijo_slot(i);
        }
    }
    if (sem_cuentas) sem_close(sem_cuentas);
    if (sem_config) sem_close(sem_config);
    sem_unlink("/sem_cuentas");
    sem_unlink("/sem_config");
    if (msgid >= 0) msgctl(msgid, IPC_RMID, NULL);
}

static void handle_sigint(int sig) {
    (void)sig;
    terminate_flag = 1;
    cleanup();
    _exit(0);
}

/* non-blocking drain of log messages and alerts */
static void drain_messages(void) {
    struct msgbuf m;
    while (msgrcv(msgid, &m, sizeof(m.info), 2, IPC_NOWAIT | MSG_NOERROR) > 0) {
        format_and_log(&m.info.log);
    }
    while (msgrcv(msgid, &m, sizeof(m.info), 3, IPC_NOWAIT | MSG_NOERROR) > 0) {
        format_and_log_alert(&m.info.log);
        int idx = find_hijo_by_account(m.info.log.cuenta_id);
        if (idx >= 0) {
            char pipemsg[128];
            char reason[20];
            strncpy(reason, m.info.log.timestamp, 19);
            reason[19] = '\0';
            snprintf(pipemsg, sizeof(pipemsg), "BLOQUEO: %s", reason);
            ssize_t w = write(hijos[idx].pipe_write_fd, pipemsg, strlen(pipemsg) + 1);
            (void)w;
        }
    }
}

/* non-blocking reap of finished sessions */
static void reap_sessions(void) {
    pid_t r;
    while ((r = waitpid(-1, NULL, WNOHANG)) > 0) {
        if (r == monitor_pid) { monitor_pid = 0; continue; }
        int idx = find_hijo_by_pid(r);
        if (idx >= 0) {
            int acc = hijos[idx].account_num;
            free_hijo_slot(idx);
            printf("\n[info] Session for account %d closed\n", acc);
            printf("Account number (0 = create, -1 = quit): ");
            fflush(stdout);
        }
    }
}

static void clear_screen(void) {
    printf("\033[H\033[2J");
    fflush(stdout);
}

static void print_banco_header(void) {
    printf("SecureBank - login service\n");
    printf("--------------------------------\n");
}

static void prompt_banco(void) {
    printf("\nAccount number (0 = create, -2 = list, -1 = quit): ");
    fflush(stdout);
}

static void handle_login_input(void) {
    int cuenta;
    if (scanf("%d", &cuenta) != 1) {
        int c; while ((c = getchar()) != '\n' && c != EOF);
        clear_screen();
        print_banco_header();
        printf("Invalid input\n");
        prompt_banco();
        return;
    }
    if (cuenta == -1) { terminate_flag = 1; return; }
    clear_screen();
    print_banco_header();

    if (cuenta == -2) {
        listar_cuentas();
        prompt_banco();
        return;
    }

    int account_num;
    if (cuenta == 0) {
        char nombre[50];
        printf("Holder name: "); fflush(stdout);
        if (scanf("%49s", nombre) != 1) { prompt_banco(); return; }
        account_num = crear_cuenta(nombre);
        if (account_num < 0) { printf("Could not create account\n"); prompt_banco(); return; }
        printf("Account created: %d\n", account_num);
    } else {
        if (buscar_cuenta(cuenta) < 0) {
            printf("Account not found\n");
            prompt_banco();
            return;
        }
        account_num = cuenta;
    }

    if (find_hijo_by_account(account_num) >= 0) {
        printf("Account %d already has an open session\n", account_num);
        prompt_banco();
        return;
    }

    if (launch_session(account_num) == 0)
        printf("Session launched for account %d (new window)\n", account_num);

    prompt_banco();
}

static void bucle_principal(void) {
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;

    clear_screen();
    print_banco_header();
    printf("SecureBank started. msgid=%d\n", msgid);
    prompt_banco();

    while (!terminate_flag) {
        int r = poll(&pfd, 1, 50);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("poll"); break;
        }
        if (r > 0 && (pfd.revents & POLLIN)) {
            handle_login_input();
        }
        drain_messages();
        reap_sessions();
    }
}

int main(void) {
    memset(&cfg, 0, sizeof(cfg));
    memset(hijos, 0, sizeof(hijos));
    for (int i = 0; i < MAX_HIJOS; i++) hijos[i].pipe_write_fd = -1;

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    signal(SIGPIPE, SIG_IGN);

    leer_config();
    ensure_cuentas_file();

    sem_unlink("/sem_cuentas");
    sem_unlink("/sem_config");
    sem_cuentas = sem_open("/sem_cuentas", O_CREAT, 0666, 1);
    sem_config = sem_open("/sem_config", O_CREAT, 0666, 1);
    if (sem_cuentas == SEM_FAILED || sem_config == SEM_FAILED) {
        perror("sem_open");
        exit(1);
    }

    msgid = msgget(IPC_PRIVATE, 0666 | IPC_CREAT);
    if (msgid < 0) { perror("msgget"); exit(1); }

    write_log("System started");

    lanzar_monitor();
    bucle_principal();

    write_log("System stopped");
    cleanup();
    return 0;
}
