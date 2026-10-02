#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "context.h"
#include "pa1.h"

#include <errno.h>
#include <fcntl.h>
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
      if (errno == EINTR)
        continue;
      return;
    }
    off += (size_t)n;
  }
}

static void fill_message(Message *msg, MessageType type, const char *text) {
  memset(msg, 0, sizeof(*msg));
  msg->s_header.s_magic = MESSAGE_MAGIC;
  msg->s_header.s_type = (int16_t)type;
  msg->s_header.s_local_time = 0;
  msg->s_header.s_payload_len = (uint16_t)strlen(text);
  memcpy(msg->s_payload, text, msg->s_header.s_payload_len);
}

static int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0)
    return -1;
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
      // Skip pipe to self
      if (from == to)
        continue;

      if (ctx->id == to)
        ctx->read_fd[from] = pipes[from][to][0];
      else if (close(pipes[from][to][0]) != 0)
        return -1;

      if (ctx->id == from)
        ctx->write_fd[to] = pipes[from][to][1];
      else if (close(pipes[from][to][1]) != 0)
        return -1;
    }
  }
  for (local_id from = 0; from < n; from++) {
    if (from != ctx->id && set_nonblock(ctx->read_fd[from]) != 0)
      return -1;
  }
  return 0;
}

static int await_from_children(Context *ctx, int type) {
  for (local_id from = 1; from < ctx->process_count; from++) {
    if (from == ctx->id)
      continue;
    Message msg;
    if (receive(ctx, from, &msg) != 0 || msg.s_header.s_type != type)
      return -1;
  }
  return 0;
}

static int child_work(Context *ctx) {
  char text[128];
  Message msg;

  snprintf(text, sizeof(text), log_started_fmt, ctx->id, getpid(), getppid());
  log_event(text);
  fill_message(&msg, STARTED, text);
  if (send_multicast(ctx, &msg) != 0)
    return -1;
  if (await_from_children(ctx, STARTED) != 0)
    return -1;

  snprintf(text, sizeof(text), log_received_all_started_fmt, ctx->id);
  log_event(text);

  snprintf(text, sizeof(text), log_done_fmt, ctx->id);
  log_event(text);
  fill_message(&msg, DONE, text);
  if (send_multicast(ctx, &msg) != 0)
    return -1;
  if (await_from_children(ctx, DONE) != 0)
    return -1;

  snprintf(text, sizeof(text), log_received_all_done_fmt, ctx->id);
  log_event(text);
  return 0;
}

static int parent_work(Context *ctx, const pid_t *pids) {
  if (await_from_children(ctx, STARTED) != 0)
    return -1;
  if (await_from_children(ctx, DONE) != 0)
    return -1;

  int status = 0;
  for (local_id i = 1; i < ctx->process_count; i++) {
    int child_status;
    if (waitpid(pids[i], &child_status, 0) < 0)
      return -1;
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
      status = -1;
  }
  return status;
}

static int read_child_count(int argc, char *argv[]) {
  int child_count = -1;
  opterr = 0;
  int opt;
  while ((opt = getopt(argc, argv, "p:")) != -1) {
    if (opt == 'p')
      child_count = atoi(optarg);
    else
      return -1;
  }
  if (child_count < 0 || child_count >= MAX_PROCESS_ID)
    return -1;
  return child_count;
}

int main(int argc, char *argv[]) {
  int child_count = read_child_count(argc, argv);
  if (child_count < 0)
    return 1;

  local_id process_count = (local_id)(child_count + 1);
  int pipes[MAX_PROCESS_ID + 1][MAX_PROCESS_ID + 1][2];

  FILE *pipes_file = fopen(pipes_log, "w");
  if (pipes_file == NULL)
    return 1;
  for (local_id from = 0; from < process_count; from++) {
    for (local_id to = 0; to < process_count; to++) {
      if (from == to)
        continue;
      if (pipe(pipes[from][to]) != 0)
        return 1;
      fprintf(pipes_file, "%d -> %d read=%d write=%d\n", (int)from, (int)to,
              pipes[from][to][0], pipes[from][to][1]);
    }
  }
  if (fclose(pipes_file) != 0)
    return 1;

  events_fd = open(events_log, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
  if (events_fd < 0)
    return 1;

  pid_t pids[MAX_PROCESS_ID + 1] = {0};
  local_id id = PARENT_ID;
  for (local_id i = 1; i < process_count; i++) {
    pid_t pid = fork();
    if (pid < 0)
      return 1;
    if (pid == 0) {
      id = i;
      break;
    }
    pids[i] = pid;
  }

  Context ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.id = id;
  ctx.process_count = process_count;
  if (bind_pipes(&ctx, pipes) != 0)
    return 1;

  int rc = (id == PARENT_ID) ? parent_work(&ctx, pids) : child_work(&ctx);
  return rc == 0 ? 0 : 1;
}
