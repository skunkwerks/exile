#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "erl_nif.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef ERTS_DIRTY_SCHEDULERS
#define USE_DIRTY_IO ERL_NIF_DIRTY_JOB_IO_BOUND
#else
#define USE_DIRTY_IO 0
#endif

//#define DEBUG

#ifdef DEBUG
#define debug(...)                                                             \
  do {                                                                         \
    enif_fprintf(stderr, __VA_ARGS__);                                         \
    enif_fprintf(stderr, "\n");                                                \
  } while (0)
#define start_timing() ErlNifTime __start = enif_monotonic_time(ERL_NIF_USEC)
#define elapsed_microseconds() (enif_monotonic_time(ERL_NIF_USEC) - __start)
#else
#define debug(...)
#define start_timing()
#define elapsed_microseconds() 0
#endif

#define error(...)                                                             \
  do {                                                                         \
    enif_fprintf(stderr, __VA_ARGS__);                                         \
    enif_fprintf(stderr, "\n");                                                \
  } while (0)

#define GET_CTX(env, arg, ctx)                                                 \
  do {                                                                         \
    ExilePriv *data = enif_priv_data(env);                                     \
    if (enif_get_resource(env, arg, data->exec_ctx_rt, (void **)&ctx) ==       \
        false) {                                                               \
      return make_error(env, ATOM_INVALID_CTX);                                \
    }                                                                          \
  } while (0);

static const int PIPE_READ = 0;
static const int PIPE_WRITE = 1;
static const int PIPE_CLOSED = -1;
static const int CMD_EXIT = -1;
static const int UNBUFFERED_READ = -1;
static const int PIPE_BUF_SIZE = 65535;

/* We are choosing an exit code which is not reserved see:
 * https://www.tldp.org/LDP/abs/html/exitcodes.html. */
static const int FORK_EXEC_FAILURE = 125;

static ERL_NIF_TERM ATOM_TRUE;
static ERL_NIF_TERM ATOM_FALSE;
static ERL_NIF_TERM ATOM_OK;
static ERL_NIF_TERM ATOM_ERROR;
static ERL_NIF_TERM ATOM_UNDEFINED;
static ERL_NIF_TERM ATOM_INVALID_CTX;
static ERL_NIF_TERM ATOM_PIPE_CLOSED;
static ERL_NIF_TERM ATOM_EAGAIN;
static ERL_NIF_TERM ATOM_ALLOC_FAILED;

/* command exit types */
static ERL_NIF_TERM ATOM_EXIT;
static ERL_NIF_TERM ATOM_SIGNALED;
static ERL_NIF_TERM ATOM_STOPPED;

enum exec_status {
  SUCCESS,
  PIPE_CREATE_ERROR,
  PIPE_FLAG_ERROR,
  FORK_ERROR,
  PIPE_DUP_ERROR,
  NULL_DEV_OPEN_ERROR,
};

enum exit_type { NORMAL_EXIT, SIGNALED, STOPPED };

typedef struct ExilePriv {
  ErlNifResourceType *exec_ctx_rt;
  ErlNifResourceType *io_rt;
} ExilePriv;

typedef struct ExecContext {
  int cmd_input_fd;
  int cmd_output_fd;
  int exit_status; // can be exit status or signal number depending on exit_type
  enum exit_type exit_type;
  pid_t pid;
  // these are to hold enif_select resource objects
  int *read_resource;
  int *write_resource;
} ExecContext;

typedef struct StartProcessResult {
  bool success;
  int err;
  ExecContext context;
} StartProcessResult;

/* TODO: assert if the external process is exit (?) */
static void exec_ctx_dtor(ErlNifEnv *env, void *obj) {
  ExecContext *ctx = obj;
  enif_release_resource(ctx->read_resource);
  enif_release_resource(ctx->write_resource);
  debug("Exile exec_ctx_dtor called");
}

static void exec_ctx_stop(ErlNifEnv *env, void *obj, int fd,
                          int is_direct_call) {
  debug("Exile exec_ctx_stop called");
}

static void exec_ctx_down(ErlNifEnv *env, void *obj, ErlNifPid *pid,
                          ErlNifMonitor *monitor) {
  debug("Exile exec_ctx_down called");
}

static ErlNifResourceTypeInit exec_ctx_rt_init = {exec_ctx_dtor, exec_ctx_stop,
                                                  exec_ctx_down};

static void io_resource_dtor(ErlNifEnv *env, void *obj) {
  debug("Exile io_resource_dtor called");
}

static void io_resource_stop(ErlNifEnv *env, void *obj, int fd,
                             int is_direct_call) {
  debug("Exile io_resource_stop called %d", fd);
}

static void io_resource_down(ErlNifEnv *env, void *obj, ErlNifPid *pid,
                             ErlNifMonitor *monitor) {
  debug("Exile io_resource_down called");
}

static ErlNifResourceTypeInit io_rt_init = {io_resource_dtor, io_resource_stop,
                                            io_resource_down};

static void free_array_of_cstr(char ***arr) {
  int i;

  if (*arr) {
    for(i=0; (*arr)[i] != NULL; i++) {
      free((*arr)[i]);
      (*arr)[i] = NULL;
    }

    free(*arr);
  }
  *arr = NULL;
}

static bool read_string(ErlNifEnv *env, ERL_NIF_TERM term, char **str) {
  unsigned int length;
  *str = NULL;

  if (enif_get_list_length(env, term, &length) != true) {
    error("failed to get string length");
    return false;
  }

  *str = (char *)malloc((length + 1) * sizeof(char));

  if (enif_get_string(env, term, *str, length + 1, ERL_NIF_LATIN1) < 1) {
    error("failed to get string");

    free(str);
    *str = NULL;

    return false;
  }

  return true;
}

static bool erl_list_to_array_of_cstr(ErlNifEnv *env, ERL_NIF_TERM list,
                                      char ***arr) {
  ERL_NIF_TERM head, tail;
  unsigned int length, i;

  *arr = NULL;

  if (enif_is_list(env, list) != true) {
    error("erl term is not a list");
    goto error;
  }

  if (enif_get_list_length(env, list, &length) != true) {
    error("erl term is not a proper list");
    goto error;
  }

  *arr = (char **)malloc((length + 1) * sizeof(char *));

  for (i = 0; i < length + 1; i++)
    (*arr)[i] = NULL;

  tail = list;

  for (i = 0; i < length; i++) {
    if (enif_get_list_cell(env, tail, &head, &tail) != true) {
      error("failed to get cell from list");
      goto error;
    }

    if (read_string(env, head, &(*arr)[i]) != true)
      goto error;
  }

  return true;

error:
  free_array_of_cstr(arr);
  return false;
}

static inline ERL_NIF_TERM make_ok(ErlNifEnv *env, ERL_NIF_TERM term) {
  return enif_make_tuple2(env, ATOM_OK, term);
}

static inline ERL_NIF_TERM make_error(ErlNifEnv *env, ERL_NIF_TERM term) {
  return enif_make_tuple2(env, ATOM_ERROR, term);
}

static int set_flag(int fd, int flags) {
  return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | flags);
}

static void close_all(int pipes[2][2]) {
  for (int i = 0; i < 2; i++) {
    if (pipes[i][PIPE_READ] > 0)
      close(pipes[i][PIPE_READ]);
    if (pipes[i][PIPE_WRITE] > 0)
      close(pipes[i][PIPE_WRITE]);
  }
}

/* time is assumed to be in microseconds */
static void notify_consumed_timeslice(ErlNifEnv *env, ErlNifTime start,
                                      ErlNifTime stop) {
  ErlNifTime pct;

  pct = (ErlNifTime)((stop - start) / 10);
  if (pct > 100)
    pct = 100;
  else if (pct == 0)
    pct = 1;
  enif_consume_timeslice(env, pct);
}

/* This is not ideal, but as of now there is no portable way to do this */
static void close_all_fds() {
  int fd_limit = (int)sysconf(_SC_OPEN_MAX);
  for (int i = STDERR_FILENO + 1; i < fd_limit; i++)
    close(i);
}

static StartProcessResult start_process(char **args, bool stderr_to_console,
                                        char *dir, char *const exec_env[]) {
  StartProcessResult result = {.success = false};
  pid_t pid;
  int pipes[2][2] = {{0, 0}, {0, 0}};

  if (pipe(pipes[STDIN_FILENO]) == -1 || pipe(pipes[STDOUT_FILENO]) == -1) {
    result.err = errno;
    perror("[exile] failed to create pipes");
    close_all(pipes);
    return result;
  }

  const int r_cmdin = pipes[STDIN_FILENO][PIPE_READ];
  const int w_cmdin = pipes[STDIN_FILENO][PIPE_WRITE];

  const int r_cmdout = pipes[STDOUT_FILENO][PIPE_READ];
  const int w_cmdout = pipes[STDOUT_FILENO][PIPE_WRITE];

  if (set_flag(r_cmdin, O_CLOEXEC) < 0 || set_flag(w_cmdout, O_CLOEXEC) < 0 ||
      set_flag(w_cmdin, O_CLOEXEC | O_NONBLOCK) < 0 ||
      set_flag(r_cmdout, O_CLOEXEC | O_NONBLOCK) < 0) {
    result.err = errno;
    perror("[exile] failed to set flags for pipes");
    close_all(pipes);
    return result;
  }

  switch (pid = fork()) {

  case -1:
    result.err = errno;
    perror("[exile] failed to fork");
    close_all(pipes);
    return result;

  case 0: // child

    if (dir[0] && chdir(dir) != 0) {
      perror("[exile] failed to change directory");
      _exit(FORK_EXEC_FAILURE);
    }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);

    if (dup2(r_cmdin, STDIN_FILENO) < 0) {
      perror("[exile] failed to dup to stdin");

      /* We are assuming FORK_EXEC_FAILURE exit code wont be used by the command
       * we are running. Technically we can not assume any exit code here. The
       * parent can not differentiate between exit before `exec` and the normal
       * command exit.
       * One correct way to solve this might be to have a separate
       * pipe shared between child and parent and signaling the parent by
       * closing it or writing to it. */
      _exit(FORK_EXEC_FAILURE);
    }
    if (dup2(w_cmdout, STDOUT_FILENO) < 0) {
      perror("[exile] failed to dup to stdout");
      _exit(FORK_EXEC_FAILURE);
    }

    if (stderr_to_console != true) {
      close(STDERR_FILENO);
      int dev_null = open("/dev/null", O_WRONLY);

      if (dev_null == -1) {
        perror("[exile] failed to open /dev/null");
        _exit(FORK_EXEC_FAILURE);
      }

      if (dup2(dev_null, STDERR_FILENO) < 0) {
        perror("[exile] failed to dup stderr");
        _exit(FORK_EXEC_FAILURE);
      }

      close(dev_null);
    }

    close_all_fds();

    execve(args[0], args, exec_env);
    perror("[exile] execvp(): failed");

    _exit(FORK_EXEC_FAILURE);

  default: // parent
    /* close file descriptors used by child */
    close(r_cmdin);
    close(w_cmdout);

    result.success = true;
    result.context.pid = pid;
    result.context.cmd_input_fd = w_cmdin;
    result.context.cmd_output_fd = r_cmdout;

    return result;
  }
}

/* TODO: return appropriate error instead returning generic "badarg" error */
static ERL_NIF_TERM execute(ErlNifEnv *env, int argc,
                            const ERL_NIF_TERM argv[]) {
  ErlNifTime start;
  int stderr_to_console_int;
  ERL_NIF_TERM term;
  char **exec_args, **exec_env;
  char *dir;
  bool stderr_to_console = true;
  struct ExilePriv *data;
  StartProcessResult result;
  ExecContext *ctx = NULL;

  exec_args = NULL;
  exec_env = NULL;
  dir = NULL;

  start = enif_monotonic_time(ERL_NIF_USEC);

  if (argc != 4) {
    error("number of arguments for `execute` must be 4");
    term = enif_make_badarg(env);
    goto exit;
  }

  if (erl_list_to_array_of_cstr(env, argv[0], &exec_args) != true) {
    error("failed to read command arguments");
    term = enif_make_badarg(env);
    goto exit;
  }

  if (erl_list_to_array_of_cstr(env, argv[1], &exec_env) != true) {
    error("failed to read env list");
    term = enif_make_badarg(env);
    goto exit;
  }

  if (read_string(env, argv[2], &dir) != true) {
    error("failed to get `dir`");
    term = enif_make_badarg(env);
    goto exit;
  }

  if (enif_get_int(env, argv[3], &stderr_to_console_int) != true) {
    error("failed to read stderr_to_console int");
    term = enif_make_badarg(env);
    goto exit;
  }
  stderr_to_console = stderr_to_console_int == 1 ? true : false;

  data = enif_priv_data(env);
  result = start_process(exec_args, stderr_to_console, dir, exec_env);

  if (result.success) {
    ctx = enif_alloc_resource(data->exec_ctx_rt, sizeof(ExecContext));
    ctx->cmd_input_fd = result.context.cmd_input_fd;
    ctx->cmd_output_fd = result.context.cmd_output_fd;
    ctx->read_resource = enif_alloc_resource(data->io_rt, sizeof(int));
    ctx->write_resource = enif_alloc_resource(data->io_rt, sizeof(int));
    ctx->pid = result.context.pid;

    debug("pid: %d  cmd_in_fd: %d  cmd_out_fd: %d", ctx->pid, ctx->cmd_input_fd,
          ctx->cmd_output_fd);

    term = enif_make_resource(env, ctx);

    /* resource should be collected beam GC when there are no more references */
    enif_release_resource(ctx);

    notify_consumed_timeslice(env, start, enif_monotonic_time(ERL_NIF_USEC));

    term = make_ok(env, term);
  } else {
    term = make_error(env, enif_make_int(env, result.err));
  }

exit:
  free_array_of_cstr(&exec_args);
  free_array_of_cstr(&exec_env);

  if (dir)
    free(dir);

  return term;
}

static int select_write(ErlNifEnv *env, ExecContext *ctx) {
  int retval = enif_select(env, ctx->cmd_input_fd, ERL_NIF_SELECT_WRITE,
                           ctx->write_resource, NULL, ATOM_UNDEFINED);
  if (retval != 0)
    perror("select_write()");

  return retval;
}

static ERL_NIF_TERM sys_write(ErlNifEnv *env, int argc,
                              const ERL_NIF_TERM argv[]) {
  if (argc != 2)
    enif_make_badarg(env);

  ErlNifTime start;
  start = enif_monotonic_time(ERL_NIF_USEC);

  ExecContext *ctx = NULL;
  GET_CTX(env, argv[0], ctx);

  if (ctx->cmd_input_fd == PIPE_CLOSED)
    return make_error(env, ATOM_PIPE_CLOSED);

  ErlNifBinary bin;
  if (enif_inspect_binary(env, argv[1], &bin) != true)
    return enif_make_badarg(env);

  if (bin.size == 0)
    return enif_make_badarg(env);

  /* should we limit the bin.size here? */
  ssize_t result = write(ctx->cmd_input_fd, bin.data, bin.size);
  int write_errno = errno;

  notify_consumed_timeslice(env, start, enif_monotonic_time(ERL_NIF_USEC));

  /* TODO: branching is ugly, cleanup required */
  if (result >= (ssize_t)bin.size) { // request completely satisfied
    return make_ok(env, enif_make_int(env, result));
  } else if (result >= 0) { // request partially satisfied
    int retval = select_write(env, ctx);
    if (retval != 0)
      return make_error(env, enif_make_int(env, retval));
    return make_ok(env, enif_make_int(env, result));
  } else if (write_errno == EAGAIN || write_errno == EWOULDBLOCK) { // busy
    int retval = select_write(env, ctx);
    if (retval != 0)
      return make_error(env, enif_make_int(env, retval));
    return make_error(env, ATOM_EAGAIN);
  } else { // Error
    perror("write()");
    return make_error(env, enif_make_int(env, write_errno));
  }
}

static ERL_NIF_TERM sys_close(ErlNifEnv *env, int argc,
                              const ERL_NIF_TERM argv[]) {
  ExecContext *ctx = NULL;
  GET_CTX(env, argv[0], ctx);

  int kind;
  enif_get_int(env, argv[1], &kind);

  int result;
  switch (kind) {
  case 0:
    if (ctx->cmd_input_fd == PIPE_CLOSED) {
      return ATOM_OK;
    } else {
      enif_select(env, ctx->cmd_input_fd, ERL_NIF_SELECT_STOP,
                  ctx->write_resource, NULL, ATOM_UNDEFINED);
      result = close(ctx->cmd_input_fd);
      if (result == 0) {
        ctx->cmd_input_fd = PIPE_CLOSED;
        return ATOM_OK;
      } else {
        perror("cmd_input_fd close()");
        return make_error(env, enif_make_int(env, errno));
      }
    }
  case 1:
    if (ctx->cmd_output_fd == PIPE_CLOSED) {
      return ATOM_OK;
    } else {
      enif_select(env, ctx->cmd_output_fd, ERL_NIF_SELECT_STOP,
                  ctx->read_resource, NULL, ATOM_UNDEFINED);
      result = close(ctx->cmd_output_fd);
      if (result == 0) {
        ctx->cmd_output_fd = PIPE_CLOSED;
        return ATOM_OK;
      } else {
        perror("cmd_output_fd close()");
        return make_error(env, enif_make_int(env, errno));
      }
    }
  default:
    debug("invalid file descriptor type");
    return enif_make_badarg(env);
  }
}

static int select_read(ErlNifEnv *env, ExecContext *ctx) {
  int retval = enif_select(env, ctx->cmd_output_fd, ERL_NIF_SELECT_READ,
                           ctx->read_resource, NULL, ATOM_UNDEFINED);
  if (retval != 0)
    perror("select_read()");
  return retval;
}

static ERL_NIF_TERM sys_read(ErlNifEnv *env, int argc,
                             const ERL_NIF_TERM argv[]) {
  if (argc != 2)
    enif_make_badarg(env);

  ErlNifTime start;
  start = enif_monotonic_time(ERL_NIF_USEC);

  ExecContext *ctx = NULL;
  GET_CTX(env, argv[0], ctx);

  if (ctx->cmd_output_fd == PIPE_CLOSED)
    return make_error(env, ATOM_PIPE_CLOSED);

  int size, request;

  enif_get_int(env, argv[1], &request);
  size = request;

  if (request == UNBUFFERED_READ) {
    size = PIPE_BUF_SIZE;
  } else if (request < 1) {
    enif_make_badarg(env);
  } else if (request > PIPE_BUF_SIZE) {
    size = PIPE_BUF_SIZE;
  }

  unsigned char buf[size];
  ssize_t result = read(ctx->cmd_output_fd, buf, size);
  int read_errno = errno;

  ERL_NIF_TERM bin_term = 0;
  if (result >= 0) {
    /* no need to release this binary */
    unsigned char *temp = enif_make_new_binary(env, result, &bin_term);
    memcpy(temp, buf, result);
  }

  notify_consumed_timeslice(env, start, enif_monotonic_time(ERL_NIF_USEC));

  if (result >= 0) {
    /* we do not 'select' if request completely satisfied OR EOF OR its
     * UNBUFFERED_READ */
    if (result == request || result == 0 || request == UNBUFFERED_READ) {
      return make_ok(env, bin_term);
    } else { // request partially satisfied
      int retval = select_read(env, ctx);
      if (retval != 0)
        return make_error(env, enif_make_int(env, retval));
      return make_ok(env, bin_term);
    }
  } else {
    if (read_errno == EAGAIN || read_errno == EWOULDBLOCK) { // busy
      int retval = select_read(env, ctx);
      if (retval != 0)
        return make_error(env, enif_make_int(env, retval));
      return make_error(env, ATOM_EAGAIN);
    } else { // Error
      perror("read()");
      return make_error(env, enif_make_int(env, read_errno));
    }
  }
}

static ERL_NIF_TERM is_alive(ErlNifEnv *env, int argc,
                             const ERL_NIF_TERM argv[]) {
  ExecContext *ctx = NULL;
  GET_CTX(env, argv[0], ctx);

  if (ctx->pid == CMD_EXIT)
    return make_ok(env, ATOM_TRUE);

  int result = kill(ctx->pid, 0);

  if (result == 0) {
    return make_ok(env, ATOM_TRUE);
  } else {
    return make_ok(env, ATOM_FALSE);
  }
}

static ERL_NIF_TERM sys_terminate(ErlNifEnv *env, int argc,
                                  const ERL_NIF_TERM argv[]) {
  ExecContext *ctx = NULL;
  GET_CTX(env, argv[0], ctx);
  if (ctx->pid == CMD_EXIT)
    return make_ok(env, enif_make_int(env, 0));

  return make_ok(env, enif_make_int(env, kill(ctx->pid, SIGTERM)));
}

static ERL_NIF_TERM sys_kill(ErlNifEnv *env, int argc,
                             const ERL_NIF_TERM argv[]) {
  ExecContext *ctx = NULL;
  GET_CTX(env, argv[0], ctx);
  if (ctx->pid == CMD_EXIT)
    return make_ok(env, enif_make_int(env, 0));

  return make_ok(env, enif_make_int(env, kill(ctx->pid, SIGKILL)));
}

static ERL_NIF_TERM make_exit_term(ErlNifEnv *env, ExecContext *ctx) {
  switch (ctx->exit_type) {
  case NORMAL_EXIT:
    return make_ok(env, enif_make_tuple2(env, ATOM_EXIT,
                                         enif_make_int(env, ctx->exit_status)));
  case SIGNALED:
    /* exit_status here points to signal number */
    return make_ok(env, enif_make_tuple2(env, ATOM_SIGNALED,
                                         enif_make_int(env, ctx->exit_status)));
  case STOPPED:
    return make_ok(env, enif_make_tuple2(env, ATOM_STOPPED,
                                         enif_make_int(env, ctx->exit_status)));
  default:
    error("Invalid wait status");
    return make_error(env, ATOM_UNDEFINED);
  }
}

static ERL_NIF_TERM sys_wait(ErlNifEnv *env, int argc,
                             const ERL_NIF_TERM argv[]) {
  ExecContext *ctx = NULL;
  GET_CTX(env, argv[0], ctx);

  if (ctx->pid == CMD_EXIT)
    return make_exit_term(env, ctx);

  int status = 0;
  int wpid = waitpid(ctx->pid, &status, WNOHANG);

  if (wpid == ctx->pid) {
    ctx->pid = CMD_EXIT;

    if (WIFEXITED(status)) {
      ctx->exit_type = NORMAL_EXIT;
      ctx->exit_status = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      ctx->exit_type = SIGNALED;
      ctx->exit_status = WTERMSIG(status);
    } else if (WIFSTOPPED(status)) {
      ctx->exit_type = STOPPED;
      ctx->exit_status = 0;
    }

    return make_exit_term(env, ctx);
  } else if (wpid != 0) {
    perror("waitpid()");
  }
  ERL_NIF_TERM term = enif_make_tuple2(env, enif_make_int(env, wpid),
                                       enif_make_int(env, status));
  return make_error(env, term);
}

static ERL_NIF_TERM os_pid(ErlNifEnv *env, int argc,
                           const ERL_NIF_TERM argv[]) {
  ExecContext *ctx = NULL;
  GET_CTX(env, argv[0], ctx);
  if (ctx->pid == CMD_EXIT)
    return make_ok(env, enif_make_int(env, 0));

  return make_ok(env, enif_make_int(env, ctx->pid));
}

static int on_load(ErlNifEnv *env, void **priv, ERL_NIF_TERM load_info) {
  struct ExilePriv *data = enif_alloc(sizeof(struct ExilePriv));
  if (!data)
    return 1;

  data->exec_ctx_rt =
      enif_open_resource_type_x(env, "exile_resource", &exec_ctx_rt_init,
                                ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER, NULL);
  data->io_rt =
      enif_open_resource_type_x(env, "exile_resource", &io_rt_init,
                                ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER, NULL);

  ATOM_TRUE = enif_make_atom(env, "true");
  ATOM_FALSE = enif_make_atom(env, "false");
  ATOM_OK = enif_make_atom(env, "ok");
  ATOM_ERROR = enif_make_atom(env, "error");
  ATOM_UNDEFINED = enif_make_atom(env, "undefined");
  ATOM_INVALID_CTX = enif_make_atom(env, "invalid_exile_exec_ctx");
  ATOM_PIPE_CLOSED = enif_make_atom(env, "closed_pipe");
  ATOM_EXIT = enif_make_atom(env, "exit");
  ATOM_SIGNALED = enif_make_atom(env, "signaled");
  ATOM_STOPPED = enif_make_atom(env, "stopped");
  ATOM_EAGAIN = enif_make_atom(env, "eagain");
  ATOM_ALLOC_FAILED = enif_make_atom(env, "alloc_failed");

  *priv = (void *)data;

  return 0;
}

static void on_unload(ErlNifEnv *env, void *priv) {
  debug("exile unload");
  enif_free(priv);
}

static ErlNifFunc nif_funcs[] = {
    {"execute", 4, execute, USE_DIRTY_IO},
    {"sys_write", 2, sys_write, USE_DIRTY_IO},
    {"sys_read", 2, sys_read, USE_DIRTY_IO},
    {"sys_close", 2, sys_close, USE_DIRTY_IO},
    {"sys_terminate", 1, sys_terminate, USE_DIRTY_IO},
    {"sys_wait", 1, sys_wait, USE_DIRTY_IO},
    {"sys_kill", 1, sys_kill, USE_DIRTY_IO},
    {"alive?", 1, is_alive, USE_DIRTY_IO},
    {"os_pid", 1, os_pid, USE_DIRTY_IO},
};

ERL_NIF_INIT(Elixir.Exile.ProcessNif, nif_funcs, &on_load, NULL, NULL,
             &on_unload)
