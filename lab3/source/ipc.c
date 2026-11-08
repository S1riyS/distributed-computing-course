#define _POSIX_C_SOURCE 200809L

#include "context.h"

#include <errno.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>

static timestamp_t lamport_time = 0;

timestamp_t get_lamport_time(void) {
    return lamport_time;
}

static void lamport_on_send(Message *msg) {
    lamport_time++;
    msg->s_header.s_local_time = lamport_time;
}

static void lamport_on_receive(timestamp_t msg_time) {
    if (msg_time > lamport_time) lamport_time = msg_time;
    lamport_time++;
}

static int write_full(int fd, const void *buf, size_t len) {
    const char *bytes = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, bytes + off, len - off);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                sched_yield();
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int write_msg(Context *ctx, local_id dst, const Message *msg) {
    if (dst < 0 || dst >= ctx->process_count || dst == ctx->id) return -1;
    if (msg->s_header.s_payload_len > MAX_PAYLOAD_LEN) return -1;

    size_t len = sizeof(MessageHeader) + msg->s_header.s_payload_len;
    return write_full(ctx->write_fd[dst], msg, len);
}

int send(void *self, local_id dst, const Message *msg) {
    Message copy = *msg;
    lamport_on_send(&copy);
    return write_msg(self, dst, &copy);
}

int send_multicast(void *self, const Message *msg) {
    Context *ctx = self;
    Message copy = *msg;
    lamport_on_send(&copy);
    for (local_id dst = 0; dst < ctx->process_count; dst++) {
        if (dst == ctx->id) continue;
        if (write_msg(ctx, dst, &copy) != 0) return -1;
    }
    return 0;
}

static int take_message(ChannelBuf *ch, Message *msg) {
    if (ch->filled < sizeof(MessageHeader)) return 0;

    MessageHeader header;
    memcpy(&header, ch->data, sizeof(header));
    if (header.s_magic != MESSAGE_MAGIC || header.s_payload_len > MAX_PAYLOAD_LEN)
        return -1;

    size_t total = sizeof(MessageHeader) + header.s_payload_len;
    if (ch->filled < total) return 0;

    msg->s_header = header;
    memcpy(msg->s_payload, ch->data + sizeof(MessageHeader), header.s_payload_len);
    ch->filled -= total;
    memmove(ch->data, ch->data + total, ch->filled);
    return 1;
}

static int read_more(int fd, ChannelBuf *ch) {
    size_t need;
    if (ch->filled < sizeof(MessageHeader)) {
        need = sizeof(MessageHeader) - ch->filled;
    } else {
        MessageHeader header;
        memcpy(&header, ch->data, sizeof(header));
        if (header.s_magic != MESSAGE_MAGIC || header.s_payload_len > MAX_PAYLOAD_LEN) return -1;
        size_t total = sizeof(MessageHeader) + header.s_payload_len;
        if (ch->filled >= total) return 0;
        need = total - ch->filled;
    }

    ssize_t n = read(fd, ch->data + ch->filled, need);
    int err = errno;

    if (n < 0) {
        if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK) return 1;
        return -1;
    }
    if (n == 0) return ch->filled > 0 ? -1 : -2;
    ch->filled += (size_t)n;
    return 0;
}

static int receive_from(Context *ctx, local_id from, Message *msg, int wait) {
    ChannelBuf *ch = &ctx->incoming[from];
    for (;;) {
        int ready = take_message(ch, msg);
        if (ready < 0) return -1;
        if (ready > 0) {
            lamport_on_receive(msg->s_header.s_local_time);
            return 0;
        }

        int rc = read_more(ctx->read_fd[from], ch);
        if (rc == -2) {
            ctx->read_fd[from] = -1;
            if (wait) return -1;
            return 1;
        }
        if (rc < 0) return -1;
        if (rc > 0) {
            if (!wait) return 1;
            sched_yield();
        }
    }
}

int receive(void *self, local_id from, Message *msg) {
    Context *ctx = self;
    if (from < 0 || from >= ctx->process_count || from == ctx->id) return -1;
    return receive_from(ctx, from, msg, 1);
}

int receive_any(void *self, Message *msg) {
    Context *ctx = self;
    for (;;) {
        int alive = 0;
        for (local_id from = 0; from < ctx->process_count; from++) {
            if (from == ctx->id || ctx->read_fd[from] < 0) continue;
            alive = 1;
            int rc = receive_from(ctx, from, msg, 0);
            if (rc <= 0) return rc;
        }
        if (!alive) return -1;
        sched_yield();
    }
}
