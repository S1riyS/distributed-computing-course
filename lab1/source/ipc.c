#define _POSIX_C_SOURCE 200809L

#include "context.h"

#include <errno.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>

static int write_full(int fd, const void *buf, size_t len) {
    const char *bytes = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, bytes + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

int send(void *self, local_id dst, const Message *msg) {
    Context *ctx = self;
    if (dst < 0 || dst >= ctx->process_count || dst == ctx->id) return -1;
    if (msg->s_header.s_payload_len > MAX_PAYLOAD_LEN) return -1;

    size_t len = sizeof(MessageHeader) + msg->s_header.s_payload_len;
    return write_full(ctx->write_fd[dst], msg, len);
}

int send_multicast(void *self, const Message *msg) {
    Context *ctx = self;
    for (local_id dst = 0; dst < ctx->process_count; dst++) {
        if (dst == ctx->id) continue;
        if (send(ctx, dst, msg) != 0) return -1;
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
    ssize_t n = read(fd, ch->data + ch->filled, MAX_MESSAGE_LEN - ch->filled);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 1;
        return -1;
    }
    if (n == 0) return -1;
    ch->filled += (size_t)n;
    return 0;
}

static int receive_from(Context *ctx, local_id from, Message *msg, int wait) {
    ChannelBuf *ch = &ctx->incoming[from];
    for (;;) {
        int ready = take_message(ch, msg);
        if (ready != 0) return ready < 0 ? -1 : 0;

        int rc = read_more(ctx->read_fd[from], ch);
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
        for (local_id from = 0; from < ctx->process_count; from++) {
            if (from == ctx->id) continue;
            int rc = receive_from(ctx, from, msg, 0);
            if (rc <= 0) return rc;
        }
        sched_yield();
    }
}
