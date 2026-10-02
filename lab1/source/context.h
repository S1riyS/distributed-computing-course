#ifndef CONTEXT_H
#define CONTEXT_H

#include "ipc.h"

typedef struct {
    char data[MAX_MESSAGE_LEN];
    size_t filled;
} ChannelBuf;

typedef struct {
    local_id id;
    local_id process_count;
    int read_fd[MAX_PROCESS_ID + 1];
    int write_fd[MAX_PROCESS_ID + 1];
    ChannelBuf incoming[MAX_PROCESS_ID + 1];
} Context;

#endif
