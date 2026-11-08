#define _POSIX_C_SOURCE 200809L

#include "banking.h"
#include "common.h"
#include "context.h"
#include "pa2345.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int events_fd = -1;

static void log_event(const char *text) {
    fputs(text, stdout);
    fflush(stdout);

    size_t len = strlen(text);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(events_fd, text + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        off += (size_t)n;
    }
}

static void set_message(Message *msg, int type, const void *payload, uint16_t len, timestamp_t time) {
    memset(msg, 0, sizeof(*msg));
    msg->s_header.s_magic = MESSAGE_MAGIC;
    msg->s_header.s_type = (int16_t)type;
    msg->s_header.s_local_time = time;
    msg->s_header.s_payload_len = len;
    if (len > 0 && payload != NULL) memcpy(msg->s_payload, payload, len);
}

static timestamp_t clamp_time(timestamp_t time) {
    if (time < 0) return 0;
    if (time >= MAX_T) return MAX_T - 1;
    return time;
}

typedef struct {
    balance_t balance;
    BalanceHistory history;
} Account;

static void account_init(Account *account, local_id id, balance_t initial) {
    memset(account, 0, sizeof(*account));
    account->balance = initial;
    account->history.s_id = id;
    account->history.s_history[0].s_time = 0;
    account->history.s_history[0].s_balance = initial;
    account->history.s_history[0].s_balance_pending_in = 0;
    account->history.s_history_len = 1;
}

static void account_set(Account *account, timestamp_t now, balance_t value) {
    now = clamp_time(now);
    timestamp_t last = (timestamp_t)(account->history.s_history_len - 1);
    if (now < last) now = last;
    balance_t balance = account->history.s_history[last].s_balance;
    balance_t pending = account->history.s_history[last].s_balance_pending_in;
    for (timestamp_t t = (timestamp_t)(last + 1); t < now; t++) {
        account->history.s_history[t].s_time = t;
        account->history.s_history[t].s_balance = balance;
        account->history.s_history[t].s_balance_pending_in = pending;
    }
    account->history.s_history[now].s_time = now;
    account->history.s_history[now].s_balance = value;
    account->history.s_history[now].s_balance_pending_in = pending;
    if (account->history.s_history_len < (uint8_t)(now + 1))
        account->history.s_history_len = (uint8_t)(now + 1);
    account->balance = value;
}

/* Money leaves the sender at sent and is stored here at recv. Between these
 * moments it is still in the channel, so the column sum keeps it in pending. */
static void account_receive(Account *account, timestamp_t sent, timestamp_t recv, balance_t amount) {
    sent = clamp_time(sent);
    recv = clamp_time(recv);
    if (sent > recv) sent = recv;

    timestamp_t last = (timestamp_t)(account->history.s_history_len - 1);
    balance_t balance = account->history.s_history[last].s_balance;
    balance_t pending = account->history.s_history[last].s_balance_pending_in;
    for (timestamp_t t = (timestamp_t)(last + 1); t < recv; t++) {
        account->history.s_history[t].s_time = t;
        account->history.s_history[t].s_balance = balance;
        account->history.s_history[t].s_balance_pending_in = pending;
    }
    for (timestamp_t t = sent; t < recv; t++) {
        account->history.s_history[t].s_balance_pending_in =
            (balance_t)(account->history.s_history[t].s_balance_pending_in + amount);
    }
    account->history.s_history[recv].s_time = recv;
    account->history.s_history[recv].s_balance = (balance_t)(balance + amount);
    account->history.s_history[recv].s_balance_pending_in = pending;
    if (account->history.s_history_len < (uint8_t)(recv + 1))
        account->history.s_history_len = (uint8_t)(recv + 1);
    account->balance = (balance_t)(balance + amount);
}

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int bind_pipes(Context *ctx, int pipes[][MAX_PROCESS_ID + 1][2]) {
    local_id n = ctx->process_count;
    for (local_id i = 0; i < n; i++) {
        ctx->read_fd[i] = -1;
        ctx->write_fd[i] = -1;
    }
    for (local_id from = 0; from < n; from++) {
        for (local_id to = 0; to < n; to++) {
            if (from == to) continue;
            if (ctx->id == to) ctx->read_fd[from] = pipes[from][to][0];
            else if (close(pipes[from][to][0]) != 0) return -1;
            if (ctx->id == from) ctx->write_fd[to] = pipes[from][to][1];
            else if (close(pipes[from][to][1]) != 0) return -1;
        }
    }
    for (local_id i = 0; i < n; i++) {
        if (i == ctx->id) continue;
        if (set_nonblock(ctx->read_fd[i]) != 0 || set_nonblock(ctx->write_fd[i]) != 0) return -1;
    }
    return 0;
}

static int await_from_children(Context *ctx, int type) {
    for (local_id from = 1; from < ctx->process_count; from++) {
        if (from == ctx->id) continue;
        Message msg;
        if (receive(ctx, from, &msg) != 0 || msg.s_header.s_type != type) return -1;
    }
    return 0;
}

static int handle_transfer(Context *ctx, Account *account, const Message *incoming) {
    if (incoming->s_header.s_payload_len < sizeof(TransferOrder)) return -1;

    TransferOrder order;
    memcpy(&order, incoming->s_payload, sizeof(order));
    char text[128];

    if (ctx->id == order.s_src) {
        if (send(ctx, order.s_dst, incoming) != 0) return -1;
        timestamp_t now = get_lamport_time();
        account_set(account, now, (balance_t)(account->balance - order.s_amount));
        snprintf(text, sizeof(text), log_transfer_out_fmt, (int)now, ctx->id, order.s_amount, order.s_dst);
        log_event(text);
        return 0;
    }

    if (ctx->id == order.s_dst) {
        timestamp_t sent = incoming->s_header.s_local_time;
        timestamp_t now = get_lamport_time();
        account_receive(account, sent, now, order.s_amount);
        snprintf(text, sizeof(text), log_transfer_in_fmt, (int)now, ctx->id, order.s_amount, order.s_src);
        log_event(text);

        Message ack;
        set_message(&ack, ACK, NULL, 0, 0);
        return send(ctx, PARENT_ID, &ack);
    }

    return -1;
}

static int send_history(Context *ctx, const Account *account, timestamp_t time) {
    uint16_t len = (uint16_t)(sizeof(account->history.s_id) + sizeof(account->history.s_history_len) +
                              account->history.s_history_len * sizeof(BalanceState));
    Message msg;
    set_message(&msg, BALANCE_HISTORY, &account->history, len, time);
    return send(ctx, PARENT_ID, &msg);
}

static int child_work(Context *ctx, balance_t initial) {
    Account account;
    account_init(&account, ctx->id, initial);

    char text[128];
    timestamp_t now = (timestamp_t)(get_lamport_time() + 1);
    snprintf(text, sizeof(text), log_started_fmt, (int)now, ctx->id, getpid(), getppid(), initial);
    log_event(text);

    Message msg;
    set_message(&msg, STARTED, text, (uint16_t)strlen(text), now);
    if (send_multicast(ctx, &msg) != 0) return -1;
    if (await_from_children(ctx, STARTED) != 0) return -1;

    now = get_lamport_time();
    snprintf(text, sizeof(text), log_received_all_started_fmt, (int)now, ctx->id);
    log_event(text);

    int stop = 0;
    int done_count = 0;
    int others = ctx->process_count - 2;
    while (!stop || done_count < others) {
        if (receive_any(ctx, &msg) != 0) return -1;
        if (msg.s_header.s_type == TRANSFER) {
            if (handle_transfer(ctx, &account, &msg) != 0) return -1;
        } else if (msg.s_header.s_type == STOP && !stop) {
            stop = 1;
            now = (timestamp_t)(get_lamport_time() + 1);
            snprintf(text, sizeof(text), log_done_fmt, (int)now, ctx->id, account.balance);
            log_event(text);
            set_message(&msg, DONE, text, (uint16_t)strlen(text), now);
            if (send_multicast(ctx, &msg) != 0) return -1;
            account_set(&account, get_lamport_time(), account.balance);
        } else if (msg.s_header.s_type == DONE) {
            done_count++;
        }
    }

    now = get_lamport_time();
    account_set(&account, now, account.balance);
    snprintf(text, sizeof(text), log_received_all_done_fmt, (int)now, ctx->id);
    log_event(text);
    return send_history(ctx, &account, now);
}

static void align_histories(AllHistory *all) {
    timestamp_t max_time = 0;
    for (uint8_t i = 0; i < all->s_history_len; i++) {
        BalanceHistory *history = &all->s_history[i];
        if (history->s_history_len == 0) continue;
        timestamp_t last = history->s_history[history->s_history_len - 1].s_time;
        if (last > max_time) max_time = last;
    }
    max_time = clamp_time(max_time);

    for (uint8_t i = 0; i < all->s_history_len; i++) {
        BalanceHistory *history = &all->s_history[i];
        if (history->s_history_len == 0) continue;
        timestamp_t last = (timestamp_t)(history->s_history_len - 1);
        balance_t balance = history->s_history[last].s_balance;
        balance_t pending = history->s_history[last].s_balance_pending_in;
        for (timestamp_t t = (timestamp_t)(last + 1); t <= max_time; t++) {
            history->s_history[t].s_time = t;
            history->s_history[t].s_balance = balance;
            history->s_history[t].s_balance_pending_in = pending;
        }
        history->s_history_len = (uint8_t)(max_time + 1);
    }
}

static int parent_work(Context *ctx) {
    if (await_from_children(ctx, STARTED) != 0) return -1;

    bank_robbery(ctx, (local_id)(ctx->process_count - 1));

    Message stop;
    set_message(&stop, STOP, NULL, 0, 0);
    if (send_multicast(ctx, &stop) != 0) return -1;
    if (await_from_children(ctx, DONE) != 0) return -1;

    AllHistory all;
    memset(&all, 0, sizeof(all));
    all.s_history_len = (uint8_t)(ctx->process_count - 1);
    for (local_id from = 1; from < ctx->process_count; from++) {
        Message msg;
        if (receive(ctx, from, &msg) != 0 || msg.s_header.s_type != BALANCE_HISTORY) return -1;
        if (msg.s_header.s_payload_len < sizeof(local_id) + sizeof(uint8_t)) return -1;

        BalanceHistory history;
        memset(&history, 0, sizeof(history));
        memcpy(&history, msg.s_payload, msg.s_header.s_payload_len);
        if (history.s_id < 1 || history.s_id >= ctx->process_count) return -1;
        all.s_history[history.s_id - 1] = history;
    }

    align_histories(&all);
    print_history(&all);

    for (local_id i = 1; i < ctx->process_count; i++) {
        int status = 0;
        if (wait(&status) < 0) return -1;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    }
    return 0;
}

void transfer(void *parent_data, local_id src, local_id dst, balance_t amount) {
    TransferOrder order;
    order.s_src = src;
    order.s_dst = dst;
    order.s_amount = amount;

    Message msg;
    set_message(&msg, TRANSFER, &order, sizeof(order), 0);
    if (send(parent_data, src, &msg) != 0) _exit(1);
    if (receive(parent_data, dst, &msg) != 0 || msg.s_header.s_type != ACK) _exit(1);
}

static int read_child_count(int argc, char *argv[], balance_t initial[]) {
    int child_count = -1;
    opterr = 0;
    int opt;
    while ((opt = getopt(argc, argv, "p:")) != -1) {
        if (opt == 'p') child_count = atoi(optarg);
        else return -1;
    }
    if (child_count < 1 || child_count >= MAX_PROCESS_ID) return -1;
    if (optind + child_count != argc) return -1;

    for (int i = 1; i <= child_count; i++) {
        int value = atoi(argv[optind + i - 1]);
        if (value <= 0 || value > 32767) return -1;
        initial[i] = (balance_t)value;
    }
    return child_count;
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);

    balance_t initial[MAX_PROCESS_ID + 1];
    memset(initial, 0, sizeof(initial));
    int child_count = read_child_count(argc, argv, initial);
    if (child_count < 0) return 1;

    local_id process_count = (local_id)(child_count + 1);
    int pipes[MAX_PROCESS_ID + 1][MAX_PROCESS_ID + 1][2];

    FILE *pipes_file = fopen(pipes_log, "w");
    if (pipes_file == NULL) return 1;
    for (local_id from = 0; from < process_count; from++) {
        for (local_id to = 0; to < process_count; to++) {
            if (from == to) continue;
            if (pipe(pipes[from][to]) != 0) return 1;
            if (set_nonblock(pipes[from][to][0]) != 0 || set_nonblock(pipes[from][to][1]) != 0) return 1;
            fprintf(pipes_file, "%d -> %d read=%d write=%d\n", (int)from, (int)to, pipes[from][to][0],
                    pipes[from][to][1]);
        }
    }
    if (fclose(pipes_file) != 0) return 1;

    events_fd = open(events_log, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (events_fd < 0) return 1;

    local_id id = PARENT_ID;
    for (local_id i = 1; i < process_count; i++) {
        pid_t pid = fork();
        if (pid < 0) return 1;
        if (pid == 0) {
            id = i;
            break;
        }
    }

    Context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.id = id;
    ctx.process_count = process_count;
    if (bind_pipes(&ctx, pipes) != 0) return 1;

    int rc = (id == PARENT_ID) ? parent_work(&ctx) : child_work(&ctx, initial[id]);
    return rc == 0 ? 0 : 1;
}
