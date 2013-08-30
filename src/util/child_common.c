/*
    SSSD

    Common helper functions to be used in child processes

    Authors:
        Sumit Bose   <sbose@redhat.com>

    Copyright (C) 2009 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <sys/types.h>
#include <fcntl.h>
#include <tevent.h>
#include <sys/wait.h>
#include <errno.h>

#include "util/util.h"
#include "util/find_uid.h"
#include "db/sysdb.h"
#include "util/child_common.h"

struct sss_sigchild_ctx {
    struct tevent_context *ev;
    hash_table_t *children;
    int options;
};

struct sss_child_ctx {
    pid_t pid;
    sss_child_fn_t cb;
    void *pvt;
    struct sss_sigchild_ctx *sigchld_ctx;
};

errno_t sss_sigchld_init(TALLOC_CTX *mem_ctx,
                         struct tevent_context *ev,
                         struct sss_sigchild_ctx **child_ctx)
{
    errno_t ret;
    struct sss_sigchild_ctx *sigchld_ctx;
    struct tevent_signal *tes;

    sigchld_ctx = talloc_zero(mem_ctx, struct sss_sigchild_ctx);
    if (!sigchld_ctx) {
        DEBUG(0, ("fatal error initializing sss_sigchild_ctx\n"));
        return ENOMEM;
    }
    sigchld_ctx->ev = ev;

    ret = sss_hash_create(sigchld_ctx, 10, &sigchld_ctx->children);
    if (ret != EOK) {
        DEBUG(SSSDBG_FATAL_FAILURE,
              ("fatal error initializing children hash table: [%s]\n",
               strerror(ret)));
        talloc_free(sigchld_ctx);
        return ret;
    }

    BlockSignals(false, SIGCHLD);
    tes = tevent_add_signal(ev, sigchld_ctx, SIGCHLD, SA_SIGINFO,
                            sss_child_handler, sigchld_ctx);
    if (tes == NULL) {
        talloc_free(sigchld_ctx);
        return EIO;
    }

    *child_ctx = sigchld_ctx;
    return EOK;
}

static int sss_child_destructor(void *ptr)
{
    struct sss_child_ctx *child_ctx;
    hash_key_t key;
    int error;

    child_ctx = talloc_get_type(ptr, struct sss_child_ctx);
    key.type = HASH_KEY_ULONG;
    key.ul = child_ctx->pid;

    error = hash_delete(child_ctx->sigchld_ctx->children, &key);
    if (error != HASH_SUCCESS && error != HASH_ERROR_KEY_NOT_FOUND) {
        DEBUG(SSSDBG_TRACE_INTERNAL,
              ("failed to delete child_ctx from hash table [%d]: %s\n",
               error, hash_error_string(error)));
    }

    return 0;
}

errno_t sss_child_register(TALLOC_CTX *mem_ctx,
                           struct sss_sigchild_ctx *sigchld_ctx,
                           pid_t pid,
                           sss_child_fn_t cb,
                           void *pvt,
                           struct sss_child_ctx **child_ctx)
{
    struct sss_child_ctx *child;
    hash_key_t key;
    hash_value_t value;
    int error;

    child = talloc_zero(mem_ctx, struct sss_child_ctx);
    if (child == NULL) {
        return ENOMEM;
    }

    child->pid = pid;
    child->cb = cb;
    child->pvt = pvt;
    child->sigchld_ctx = sigchld_ctx;

    key.type = HASH_KEY_ULONG;
    key.ul = pid;

    value.type = HASH_VALUE_PTR;
    value.ptr = child;

    error = hash_enter(sigchld_ctx->children, &key, &value);
    if (error != HASH_SUCCESS) {
        talloc_free(child);
        return ENOMEM;
    }

    talloc_set_destructor((TALLOC_CTX *) child, sss_child_destructor);

    *child_ctx = child;
    return EOK;
}

struct sss_child_cb_pvt {
    struct sss_child_ctx *child_ctx;
    int wait_status;
};

static void sss_child_invoke_cb(struct tevent_context *ev,
                                struct tevent_immediate *imm,
                                void *pvt)
{
    struct sss_child_cb_pvt *cb_pvt;
    struct sss_child_ctx *child_ctx;
    hash_key_t key;
    int error;

    cb_pvt = talloc_get_type(pvt, struct sss_child_cb_pvt);
    child_ctx = cb_pvt->child_ctx;

    key.type = HASH_KEY_ULONG;
    key.ul = child_ctx->pid;

    error = hash_delete(child_ctx->sigchld_ctx->children, &key);
    if (error != HASH_SUCCESS && error != HASH_ERROR_KEY_NOT_FOUND) {
        DEBUG(SSSDBG_OP_FAILURE,
              ("failed to delete child_ctx from hash table [%d]: %s\n",
               error, hash_error_string(error)));
    }

    if (child_ctx->cb) {
        child_ctx->cb(child_ctx->pid, cb_pvt->wait_status, child_ctx->pvt);
    }
}

void sss_child_handler(struct tevent_context *ev,
                       struct tevent_signal *se,
                       int signum,
                       int count,
                       void *siginfo,
                       void *private_data)
{
    struct sss_sigchild_ctx *sigchld_ctx;
    struct tevent_immediate *imm;
    struct sss_child_cb_pvt *invoke_pvt;
    struct sss_child_ctx *child_ctx;
    hash_key_t key;
    hash_value_t value;
    int error;
    int wait_status;
    pid_t pid;

    sigchld_ctx = talloc_get_type(private_data, struct sss_sigchild_ctx);
    key.type = HASH_KEY_ULONG;

    do {
        do {
            errno = 0;
            pid = waitpid(-1, &wait_status, WNOHANG | sigchld_ctx->options);
        } while (pid == -1 && errno == EINTR);

        if (pid == -1) {
            DEBUG(SSSDBG_TRACE_INTERNAL,
                  ("waitpid failed [%d]: %s\n", errno, strerror(errno)));
            return;
        } else if (pid == 0) continue;

        key.ul = pid;
        error = hash_lookup(sigchld_ctx->children, &key, &value);
        if (error == HASH_SUCCESS) {
            child_ctx = talloc_get_type(value.ptr, struct sss_child_ctx);

            imm = tevent_create_immediate(sigchld_ctx->ev);
            if (imm == NULL) {
                DEBUG(SSSDBG_CRIT_FAILURE,
                      ("Out of memory invoking SIGCHLD callback\n"));
                return;
            }

            invoke_pvt = talloc_zero(child_ctx, struct sss_child_cb_pvt);
            if (invoke_pvt == NULL) {
                DEBUG(SSSDBG_CRIT_FAILURE,
                      ("out of memory invoking SIGCHLD callback\n"));
                return;
            }
            invoke_pvt->child_ctx = child_ctx;
            invoke_pvt->wait_status = wait_status;

            tevent_schedule_immediate(imm, sigchld_ctx->ev,
                                      sss_child_invoke_cb, invoke_pvt);
        } else if (error == HASH_ERROR_KEY_NOT_FOUND) {
            DEBUG(SSSDBG_TRACE_LIBS,
                 ("BUG: waitpid() returned [%d] but it was not in the table. "
                  "This could be due to a linked library creating processes "
                  "without registering them with the sigchld handler\n",
                  pid));
            /* We will simply ignore this and return to the loop
             * This will prevent a zombie, but may cause unexpected
             * behavior in the code that was trying to handle this
             * pid.
             */
        } else {
            DEBUG(SSSDBG_OP_FAILURE,
                  ("SIGCHLD hash table error [%d]: %s\n",
                   error, hash_error_string(error)));
            /* This is bad, but we should try to check for other
             * children anyway, to avoid potential zombies.
             */
        }
    } while (pid != 0);
}

struct sss_child_ctx_old {
    struct tevent_signal *sige;
    pid_t pid;
    int child_status;
    sss_child_callback_t cb;
    void *pvt;
};

int child_handler_setup(struct tevent_context *ev, int pid,
                        sss_child_callback_t cb, void *pvt,
                        struct sss_child_ctx_old **_child_ctx)
{
    struct sss_child_ctx_old *child_ctx;

    DEBUG(8, ("Setting up signal handler up for pid [%d]\n", pid));

    child_ctx = talloc_zero(ev, struct sss_child_ctx_old);
    if (child_ctx == NULL) {
        return ENOMEM;
    }

    child_ctx->sige = tevent_add_signal(ev, child_ctx, SIGCHLD, SA_SIGINFO,
                                        child_sig_handler, child_ctx);
    if(!child_ctx->sige) {
        /* Error setting up signal handler */
        talloc_free(child_ctx);
        return ENOMEM;
    }

    child_ctx->pid = pid;
    child_ctx->cb = cb;
    child_ctx->pvt = pvt;

    DEBUG(8, ("Signal handler set up for pid [%d]\n", pid));

    if (_child_ctx != NULL) {
        *_child_ctx = child_ctx;
    }

    return EOK;
}

void child_handler_destroy(struct sss_child_ctx_old *ctx)
{
    errno_t ret;

    /* We still want to wait for the child to finish, but the caller is not
     * interested in the result anymore (e.g. timeout was reached). */
    ctx->cb = NULL;
    ctx->pvt = NULL;

    ret = kill(ctx->pid, SIGKILL);
    if (ret == -1) {
        ret = errno;
        DEBUG(SSSDBG_MINOR_FAILURE, ("kill failed [%d][%s].\n", ret, strerror(ret)));
    }
}

/* Async communication with the child process via a pipe */

struct write_pipe_state {
    int fd;
    uint8_t *buf;
    size_t len;
    size_t written;
};

static void write_pipe_handler(struct tevent_context *ev,
                               struct tevent_fd *fde,
                               uint16_t flags, void *pvt);

struct tevent_req *write_pipe_send(TALLOC_CTX *mem_ctx,
                                   struct tevent_context *ev,
                                   uint8_t *buf, size_t len, int fd)
{
    struct tevent_req *req;
    struct write_pipe_state *state;
    struct tevent_fd *fde;

    req = tevent_req_create(mem_ctx, &state, struct write_pipe_state);
    if (req == NULL) return NULL;

    state->fd = fd;
    state->buf = buf;
    state->len = len;
    state->written = 0;

    fde = tevent_add_fd(ev, state, fd, TEVENT_FD_WRITE,
                        write_pipe_handler, req);
    if (fde == NULL) {
        DEBUG(1, ("tevent_add_fd failed.\n"));
        goto fail;
    }

    return req;

fail:
    talloc_zfree(req);
    return NULL;
}

static void write_pipe_handler(struct tevent_context *ev,
                               struct tevent_fd *fde,
                               uint16_t flags, void *pvt)
{
    struct tevent_req *req = talloc_get_type(pvt, struct tevent_req);
    struct write_pipe_state *state = tevent_req_data(req,
                                                     struct write_pipe_state);
    errno_t ret;

    if (flags & TEVENT_FD_READ) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("write_pipe_done called with TEVENT_FD_READ,"
               " this should not happen.\n"));
        tevent_req_error(req, EINVAL);
        return;
    }

    errno = 0;
    state->written = sss_atomic_write_s(state->fd, state->buf, state->len);
    if (state->written == -1) {
        ret = errno;
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("write failed [%d][%s].\n", ret, strerror(ret)));
        tevent_req_error(req, ret);
        return;
    }

    if (state->len != state->written) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("Wrote %d bytes, expected %d\n",
              state->written, state->len));
        tevent_req_error(req, EIO);
        return;
    }

    DEBUG(SSSDBG_TRACE_FUNC, ("All data has been sent!\n"));
    tevent_req_done(req);
    return;
}

int write_pipe_recv(struct tevent_req *req)
{
    TEVENT_REQ_RETURN_ON_ERROR(req);

    return EOK;
}

struct read_pipe_state {
    int fd;
    uint8_t *buf;
    size_t len;
};

static void read_pipe_handler(struct tevent_context *ev,
                              struct tevent_fd *fde,
                              uint16_t flags, void *pvt);

struct tevent_req *read_pipe_send(TALLOC_CTX *mem_ctx,
                                  struct tevent_context *ev, int fd)
{
    struct tevent_req *req;
    struct read_pipe_state *state;
    struct tevent_fd *fde;

    req = tevent_req_create(mem_ctx, &state, struct read_pipe_state);
    if (req == NULL) return NULL;

    state->fd = fd;
    state->buf = NULL;
    state->len = 0;

    fde = tevent_add_fd(ev, state, fd, TEVENT_FD_READ,
                        read_pipe_handler, req);
    if (fde == NULL) {
        DEBUG(1, ("tevent_add_fd failed.\n"));
        goto fail;
    }

    return req;

fail:
    talloc_zfree(req);
    return NULL;
}

static void read_pipe_handler(struct tevent_context *ev,
                              struct tevent_fd *fde,
                              uint16_t flags, void *pvt)
{
    struct tevent_req *req = talloc_get_type(pvt, struct tevent_req);
    struct read_pipe_state *state = tevent_req_data(req,
                                                    struct read_pipe_state);
    ssize_t size;
    errno_t err;
    uint8_t buf[CHILD_MSG_CHUNK];

    if (flags & TEVENT_FD_WRITE) {
        DEBUG(1, ("read_pipe_done called with TEVENT_FD_WRITE,"
                  " this should not happen.\n"));
        tevent_req_error(req, EINVAL);
        return;
    }

    size = sss_atomic_read_s(state->fd,
                buf,
                CHILD_MSG_CHUNK);
    if (size == -1) {
        err = errno;
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("read failed [%d][%s].\n", err, strerror(err)));
        tevent_req_error(req, err);
        return;

    } else if (size > 0) {
        state->buf = talloc_realloc(state, state->buf, uint8_t,
                                    state->len + size);
        if(!state->buf) {
            tevent_req_error(req, ENOMEM);
            return;
        }

        safealign_memcpy(&state->buf[state->len], buf,
                         size, &state->len);
        return;

    } else if (size == 0) {
        DEBUG(6, ("EOF received, client finished\n"));
        tevent_req_done(req);
        return;

    } else {
        DEBUG(1, ("unexpected return value of read [%d].\n", size));
        tevent_req_error(req, EINVAL);
        return;
    }
}

int read_pipe_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
                   uint8_t **buf, ssize_t *len)
{
    struct read_pipe_state *state;
    state = tevent_req_data(req, struct read_pipe_state);

    TEVENT_REQ_RETURN_ON_ERROR(req);

    *buf = talloc_steal(mem_ctx, state->buf);
    *len = state->len;

    return EOK;
}

/* The pipes to communicate with the child must be nonblocking */
void fd_nonblocking(int fd)
{
    int flags;
    int ret;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        ret = errno;
        DEBUG(1, ("F_GETFL failed [%d][%s].\n", ret, strerror(ret)));
        return;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        ret = errno;
        DEBUG(1, ("F_SETFL failed [%d][%s].\n", ret, strerror(ret)));
    }

    return;
}

static void child_invoke_callback(struct tevent_context *ev,
                                  struct tevent_immediate *imm,
                                  void *pvt);
void child_sig_handler(struct tevent_context *ev,
                       struct tevent_signal *sige, int signum,
                       int count, void *__siginfo, void *pvt)
{
    int ret, err;
    struct sss_child_ctx_old *child_ctx;
    struct tevent_immediate *imm;

    if (count <= 0) {
        DEBUG(0, ("SIGCHLD handler called with invalid child count\n"));
        return;
    }

    child_ctx = talloc_get_type(pvt, struct sss_child_ctx_old);
    DEBUG(7, ("Waiting for child [%d].\n", child_ctx->pid));

    errno = 0;
    ret = waitpid(child_ctx->pid, &child_ctx->child_status, WNOHANG);

    if (ret == -1) {
        err = errno;
        DEBUG(1, ("waitpid failed [%d][%s].\n", err, strerror(err)));
    } else if (ret == 0) {
        DEBUG(1, ("waitpid did not found a child with changed status.\n"));
    } else {
        if WIFEXITED(child_ctx->child_status) {
            if (WEXITSTATUS(child_ctx->child_status) != 0) {
                DEBUG(1, ("child [%d] failed with status [%d].\n", ret,
                          WEXITSTATUS(child_ctx->child_status)));
            } else {
                DEBUG(4, ("child [%d] finished successfully.\n", ret));
            }
        } else if WIFSIGNALED(child_ctx->child_status) {
            DEBUG(1, ("child [%d] was terminated by signal [%d].\n", ret,
                      WTERMSIG(child_ctx->child_status)));
        } else {
            if WIFSTOPPED(child_ctx->child_status) {
                DEBUG(7, ("child [%d] was stopped by signal [%d].\n", ret,
                          WSTOPSIG(child_ctx->child_status)));
            }
            if WIFCONTINUED(child_ctx->child_status) {
                DEBUG(7, ("child [%d] was resumed by delivery of SIGCONT.\n",
                          ret));
            }

            return;
        }

        /* Invoke the callback in a tevent_immediate handler
         * so that it is safe to free the tevent_signal *
         */
        imm = tevent_create_immediate(ev);
        if (imm == NULL) {
            DEBUG(0, ("Out of memory invoking sig handler callback\n"));
            return;
        }

        tevent_schedule_immediate(imm, ev,child_invoke_callback,
                                  child_ctx);
    }

    return;
}

static void child_invoke_callback(struct tevent_context *ev,
                                  struct tevent_immediate *imm,
                                  void *pvt)
{
    struct sss_child_ctx_old *child_ctx =
            talloc_get_type(pvt, struct sss_child_ctx_old);
    if (child_ctx->cb) {
        child_ctx->cb(child_ctx->child_status, child_ctx->sige, child_ctx->pvt);
    }

    /* Stop monitoring for this child */
    talloc_free(child_ctx);
}

static errno_t prepare_child_argv(TALLOC_CTX *mem_ctx,
                                  int child_debug_fd,
                                  const char *binary,
                                  char ***_argv)
{
    /*
     * program name, debug_level, debug_timestamps,
     * debug_microseconds and NULL
     */
    uint_t argc = 5;
    char ** argv;
    errno_t ret = EINVAL;

    /* Save the current state in case an interrupt changes it */
    bool child_debug_to_file = debug_to_file;
    bool child_debug_timestamps = debug_timestamps;
    bool child_debug_microseconds = debug_microseconds;

    if (child_debug_to_file) argc++;

    /*
     * program name, debug_level, debug_to_file, debug_timestamps,
     * debug_microseconds and NULL
     */
    argv  = talloc_array(mem_ctx, char *, argc);
    if (argv == NULL) {
        DEBUG(1, ("talloc_array failed.\n"));
        return ENOMEM;
    }

    argv[--argc] = NULL;

    argv[--argc] = talloc_asprintf(argv, "--debug-level=%#.4x",
                              debug_level);
    if (argv[argc] == NULL) {
        ret = ENOMEM;
        goto fail;
    }

    if (child_debug_to_file) {
        argv[--argc] = talloc_asprintf(argv, "--debug-fd=%d",
                                       child_debug_fd);
        if (argv[argc] == NULL) {
            ret = ENOMEM;
            goto fail;
        }
    }

    argv[--argc] = talloc_asprintf(argv, "--debug-timestamps=%d",
                                   child_debug_timestamps);
    if (argv[argc] == NULL) {
        ret = ENOMEM;
        goto fail;
    }

    argv[--argc] = talloc_asprintf(argv, "--debug-microseconds=%d",
                                       child_debug_microseconds);
    if (argv[argc] == NULL) {
        ret = ENOMEM;
        goto fail;
    }

    argv[--argc] = talloc_strdup(argv, binary);
    if (argv[argc] == NULL) {
        ret = ENOMEM;
        goto fail;
    }

    if (argc != 0) {
        ret = EINVAL;
        goto fail;
    }

    *_argv = argv;

    return EOK;

fail:
    talloc_free(argv);
    return ret;
}

errno_t exec_child(TALLOC_CTX *mem_ctx,
                   int *pipefd_to_child, int *pipefd_from_child,
                   const char *binary, int debug_fd)
{
    int ret;
    errno_t err;
    char **argv;

    close(pipefd_to_child[1]);
    ret = dup2(pipefd_to_child[0], STDIN_FILENO);
    if (ret == -1) {
        err = errno;
        DEBUG(1, ("dup2 failed [%d][%s].\n", err, strerror(err)));
        return err;
    }

    close(pipefd_from_child[0]);
    ret = dup2(pipefd_from_child[1], STDOUT_FILENO);
    if (ret == -1) {
        err = errno;
        DEBUG(1, ("dup2 failed [%d][%s].\n", err, strerror(err)));
        return err;
    }

    ret = prepare_child_argv(mem_ctx, debug_fd,
                             binary, &argv);
    if (ret != EOK) {
        DEBUG(1, ("prepare_child_argv.\n"));
        return ret;
    }

    execv(binary, argv);
    err = errno;
    DEBUG(SSSDBG_OP_FAILURE, ("execv failed [%d][%s].\n", err, strerror(err)));
    return err;
}

void child_cleanup(int readfd, int writefd)
{
    int ret;

    if (readfd != -1) {
        ret = close(readfd);
        if (ret != EOK) {
            ret = errno;
            DEBUG(1, ("close failed [%d][%s].\n", ret, strerror(ret)));
        }
    }
    if (writefd != -1) {
        ret = close(writefd);
        if (ret != EOK) {
            ret = errno;
            DEBUG(1, ("close failed [%d][%s].\n", ret, strerror(ret)));
        }
    }
}
