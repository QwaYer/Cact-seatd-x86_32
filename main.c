/*
 * seatd — seat manager for CactOS.
 *
 * Role in the spirit of seatd/logind: keep a "virtual terminal" and hand out
 * the seat device descriptors of the active session via AF_UNIX + SCM_RIGHTS.
 * There are no kernel VT switches here, so seatd serves one logical
 * seat seat0 and hands out the available nodes on an "attach" request:
 *   /dev/tty, /dev/keyboard, /dev/mouse, /dev/fb0
 *
 * Protocol /run/seatd.sock (one line per connection):
 *   status   -> "ok seat0 nodes=N\n"
 *   attach   -> "ok seat0 attach nfds=N\n" + N passed fds (SCM_RIGHTS)
 *
 * Started by the cgoct supervisor as /sbin/seatd.
 *
 * /etc/seatd.conf (all keys optional; created on first start):
 *   file=/var/log/seatd.log
 *   console=0
 *   node=/dev/<name>       — can be repeated; by default
 *                            tty, keyboard, mouse, fb0
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <socket.h>
#include <uio.h>
#include <poll.h>

#define CONFIG_PATH "/etc/seatd.conf"
#define SOCK_PATH   "/run/seatd.sock"
#define LOG_DEFAULT "/var/log/seatd.log"
#define MAX_NODES   8

static char log_path[128] = LOG_DEFAULT;
static int  console_on    = 0;
static int  out_fd        = -1;

static const char *default_nodes[] = { "/dev/tty", "/dev/keyboard",
                                       "/dev/mouse", "/dev/fb0" };

static char nodes[MAX_NODES][64];
static int  nodes_n = 0;

/* Default config: written on first start if the file does not exist yet. */
static const char default_config[] =
    "# seatd config - auto-generated on first start.\n"
    "#\n"
    "# file    - event log\n"
    "# console - duplicate to /dev/console (0|1)\n"
    "# node    - seat node /dev/<name> (can be repeated);\n"
    "#           default tty, keyboard, mouse, fb0\n"
    "\n"
    "file=/var/log/seatd.log\n"
    "console=0\n"
    "\n"
    "#node=/dev/tty\n"
    "#node=/dev/keyboard\n"
    "#node=/dev/mouse\n"
    "#node=/dev/fb0\n";

static void ensure_dir(const char *path) {
    (void)mkdir(path, 0755);
}

static void config_write_default(void) {
    int fd = open(CONFIG_PATH, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return;
    write(fd, default_config, sizeof(default_config) - 1);
    close(fd);
}

static void config_load(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        config_write_default();
        f = fopen(CONFIG_PATH, "r");
        if (!f) return;
    }
    char line[192];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char *eq = p;
        while (*eq && *eq != '=' && *eq != '\n') eq++;
        if (*eq != '=') continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        int vlen = (int)strlen(val);
        while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r' ||
                            val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
            val[--vlen] = '\0';

        if (strcmp(key, "file") == 0) {
            strncpy(log_path, val, sizeof(log_path) - 1);
            log_path[sizeof(log_path) - 1] = '\0';
        } else if (strcmp(key, "console") == 0) {
            console_on = (val[0] == '1' || val[0] == 'y' || val[0] == 'Y');
        } else if (strcmp(key, "node") == 0) {
            if (nodes_n < MAX_NODES && val[0] == '/') {
                strncpy(nodes[nodes_n], val, sizeof(nodes[nodes_n]) - 1);
                nodes[nodes_n][sizeof(nodes[nodes_n]) - 1] = '\0';
                nodes_n++;
            }
        }
    }
    fclose(f);
}

static void log_event(const char *msg) {
    if (out_fd >= 0) {
        write(out_fd, msg, strlen(msg));
    }
    if (console_on) {
        int cfd = open("/dev/console", O_WRONLY);
        if (cfd >= 0) {
            write(cfd, msg, strlen(msg));
            close(cfd);
        }
    }
}

/* Open the available seat nodes. Returns the number opened. */
static int open_seat_nodes(int fds[MAX_NODES]) {
    int n = 0;
    int i;
    for (i = 0; i < nodes_n; i++) {
        int fd = open(nodes[i], O_RDWR);
        if (fd < 0) fd = open(nodes[i], O_RDONLY);
        if (fd >= 0 && n < MAX_NODES) {
            fds[n++] = fd;
        }
    }
    return n;
}

/* Pass the seat device fds to the client (SCM_RIGHTS) together with the text. */
static int send_fds(int cl, const int fds[MAX_NODES], int nfds) {
    char payload[64];
    int  plen = snprintf(payload, sizeof(payload), "ok seat0 attach nfds=%d\n", nfds);

    struct iovec iov;
    iov.iov_base = payload;
    iov.iov_len  = (size_t)plen;

    unsigned char ctrl[CMSG_SPACE(sizeof(int32_t) * MAX_NODES)];
    memset(ctrl, 0, sizeof(ctrl));

    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov      = &iov;
    mh.msg_iovlen   = 1;
    mh.msg_control  = ctrl;
    mh.msg_controllen = (nfds > 0) ? CMSG_SPACE(sizeof(int32_t) * (size_t)nfds) : 0;

    if (nfds > 0) {
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type  = SCM_RIGHTS;
        c->cmsg_len   = CMSG_LEN(sizeof(int32_t) * (size_t)nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int32_t) * (size_t)nfds);
    }

    ssize_t r = sendmsg(cl, &mh, 0);
    return (r >= 0) ? 0 : -1;
}

static void handle_client(int cl) {
    char req[64];
    char b;
    int  got = 0;
    int  n;

    while (got < (int)sizeof(req) - 1) {
        n = (int)recv(cl, &b, 1, 0);
        if (n <= 0) break;
        if (b == '\n' || b == '\r') break;
        req[got++] = b;
    }
    req[got] = '\0';

    if (got == 0) return;

    if (strcmp(req, "status") == 0) {
        const char *r = "ok seat0 nodes=1\n";
        send(cl, r, (uint32_t)strlen(r), 0);
        log_event("seatd: status requested\n");
        return;
    }

    if (strcmp(req, "attach") == 0) {
        int fds[MAX_NODES];
        int nfds = open_seat_nodes(fds);
        if (send_fds(cl, fds, nfds) < 0) {
            log_event("seatd: attach failed\n");
        } else {
            char line[64];
            snprintf(line, sizeof(line), "seatd: attach granted (nfds=%d)\n", nfds);
            log_event(line);
            printf("%s", line);
        }
        while (nfds-- > 0) {
            close(fds[nfds]);
        }
        return;
    }

    const char *err = "ERR unknown command\n";
    send(cl, err, (uint32_t)strlen(err), 0);
}

static int bind_listener(void) {
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) return -1;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, SOCK_PATH, sizeof(sa.sun_path) - 1);

    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(srv);
        return -1;
    }
    if (listen(srv, 4) < 0) {
        close(srv);
        return -1;
    }
    return srv;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("seatd: starting\n");
    config_load();
    ensure_dir("/var/log");
    ensure_dir("/run");

    if (nodes_n == 0) {
        int i;
        for (i = 0; i < MAX_NODES && i < 4; i++) {
            strncpy(nodes[nodes_n], default_nodes[i], sizeof(nodes[nodes_n]) - 1);
            nodes[nodes_n][sizeof(nodes[nodes_n]) - 1] = '\0';
            nodes_n++;
        }
    }

    out_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (out_fd < 0) {
        printf("seatd: cannot open %s\n", log_path);
    }
    log_event("seatd: starting\n");

    int srv = -1;
    for (;;) {
        if (srv < 0) {
            srv = bind_listener();
            if (srv < 0) {
                sleep(3);
                continue;
            }
            printf("seatd: listening on %s\n", SOCK_PATH);
            log_event("seatd: listening\n");
        }

        struct pollfd pfd;
        pfd.fd = srv;
        pfd.events = POLLIN;
        pfd.revents = 0;

        if (poll(&pfd, 1, 1000) > 0 && (pfd.revents & POLLIN)) {
            int cl = accept(srv, 0, 0);
            if (cl >= 0) {
                handle_client(cl);
                close(cl);
            }
        }
    }
    return 0;
}
