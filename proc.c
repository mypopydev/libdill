/*

  Copyright (c) 2016 Martin Sustrik

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom
  the Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
  IN THE SOFTWARE.

*/

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

#include "libdill.h"
#include "poller.h"
#include "utils.h"

static const int dill_proc_type_placeholder = 0;
static const void *dill_proc_type = &dill_proc_type_placeholder;

struct dill_proc {
    pid_t pid;
};

static void dill_proc_close(int h) {
    struct dill_proc *proc = hdata(h, dill_proc_type);
    dill_assert(proc);
    /* This may happen if forking failed. */
    if(dill_slow(proc->pid < 0)) {free(proc); return;}
    /* There is a child running. Let's send it a kill signal. */
    int rc = kill(proc->pid, SIGKILL);
    dill_assert(rc == 0);
    /* Wait till it finishes. */
    /* TODO: For how long can this block? */
    rc = waitpid(proc->pid, NULL, 0);
    dill_assert(rc >= 0);
    free(proc);
}

static int dill_proc_wait(int h, int *result, int64_t deadline) {
    struct dill_proc *proc = hdata(h, dill_proc_type);
    dill_assert(proc);
    /* There's no simple way to wait for a process with a deadline, unless
       one wants to mess with SIGCHLD. Communicating the termination via
       pipe doesn't work if process coredumps. Therefore, we'll do this
       silly loop here. */
    int timeout = 0;
    while(1) {
        siginfo_t info;
        int rc = waitid(P_PID, proc->pid, &info, WEXITED | WNOHANG);
        dill_assert(rc == 0);
        if(info.si_pid > 0) {
            if(result)
                *result = info.si_status;
            return 0;
        }
        /* The process haven't finished yet. Sleep for a while before checking
           again. */
        if(timeout) {errno == ETIMEDOUT; return -1;}
        int64_t ddline = now() + 100;
        if(deadline != -1 && deadline < ddline) {
            ddline = deadline;
            timeout = 1;
        }
        rc = msleep(ddline);
        if(rc == 0)
            continue;
        dill_assert(errno == ECANCELED);
        return -1;
    }
}

static void dill_proc_dump(int h) {
    struct dill_proc *proc = hdata(h, dill_proc_type);
    dill_assert(proc);
    fprintf(stderr, "  PROCESS pid:%d\n", (int)proc->pid);
}

static const struct hvfptrs dill_proc_vfptrs = {
    dill_proc_close,
    dill_proc_wait,
    dill_proc_dump
};

int dill_proc_prologue(int *hndl, const char *created) {
    struct dill_proc *proc = malloc(sizeof(struct dill_proc));
    if(dill_slow(!proc)) {errno = ENOMEM; *hndl = -1; return 0;}
    proc->pid = -1;
    int h = dill_handle(dill_proc_type, proc, &dill_proc_vfptrs, created);
    if(dill_slow(h < 0)) {
        int err = errno;
        free(proc);
        errno = err;
        *hndl = -1;
        return 0;
    }
    pid_t pid = dill_fork();
    if(dill_slow(pid < 0)) {
        int err = errno;
        hclose(h);
        errno = err;
        *hndl = -1;
        return 0;
    }
    /* Child. */
    if(pid == 0) return 1;
    /* Parent. */
    proc->pid = pid;
    *hndl = h;
    return 0;
}

void dill_proc_epilogue(int result) {
    exit(result);
}

