/***
  This file is part of libcanberra.

  Copyright 2008 Lennart Poettering
                 Joe Marcus Clarke

  libcanberra is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation, either version 2.1 of the
  License, or (at your option) any later version.

  libcanberra is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with libcanberra. If not, see
  <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/uio.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>

#ifdef HAVE_MACHINE_SOUNDCARD_H
#  include <machine/soundcard.h>
#else
#  ifdef HAVE_SOUNDCARD_H
#    include <soundcard.h>
#  else
#    include <sys/soundcard.h>
#  endif
#endif

#include "canberra.h"
#include "common.h"
#include "driver.h"
#include "llist.h"
#include "read-sound-file.h"
#include "sound-theme-spec.h"
#include "malloc.h"

struct private;

struct outstanding {
    CA_LLIST_FIELDS(struct outstanding);
    ca_bool_t dead;
    uint32_t id;
    ca_finish_callback_t callback;
    void *userdata;
    ca_sound_file *file;
    int pcm;
    int pipe_fd[2];
    ca_context *context;
};

struct private {
    ca_theme_data *theme;
    ca_mutex *outstanding_mutex;
    ca_bool_t signal_semaphore;
    sem_t semaphore;
    ca_bool_t semaphore_allocated;
    CA_LLIST_HEAD(struct outstanding, outstanding);
};

#define PRIVATE(c) ((struct private *) ((c)->private))

static void outstanding_free(struct outstanding *o) {
    ca_assert(o);

    if (o->pipe_fd[1] >= 0)
        close(o->pipe_fd[1]);

    if (o->pipe_fd[0] >= 0)
        close(o->pipe_fd[0]);

    if (o->file)
        ca_sound_file_close(o->file);

    if (o->pcm >= 0) {
        close(o->pcm);
        o->pcm = -1;
    }

    ca_free(o);
}

int driver_open(ca_context *c) {
    struct private *p;

    ca_return_val_if_fail(c, CA_ERROR_INVALID);
    ca_return_val_if_fail(!c->driver || ca_streq(c->driver, "oss"), CA_ERROR_NODRIVER);
    ca_return_val_if_fail(!PRIVATE(c), CA_ERROR_STATE);

    if (!(c->private = p = ca_new0(struct private, 1)))
        return CA_ERROR_OOM;

    if (!(p->outstanding_mutex = ca_mutex_new())) {
        driver_destroy(c);
        return CA_ERROR_OOM;
    }

    if (sem_init(&p->semaphore, 0, 0) < 0) {
        driver_destroy(c);
        return CA_ERROR_OOM;
    }

    p->semaphore_allocated = TRUE;

    return CA_SUCCESS;
}

int driver_destroy(ca_context *c) {
    struct private *p;
    struct outstanding *out;

    ca_return_val_if_fail(c, CA_ERROR_INVALID);
    ca_return_val_if_fail(c->private, CA_ERROR_STATE);

    p = PRIVATE(c);

    if (p->outstanding_mutex) {
        ca_mutex_lock(p->outstanding_mutex);

        /* Tell all player threads to terminate */
        for (out = p->outstanding; out; out = out->next) {

            if (out->dead)
                continue;

            out->dead = TRUE;

            if (out->callback)
                out->callback(c, out->id, CA_ERROR_DESTROYED, out->userdata);

            /* This will cause the thread to wakeup and terminate */
            if (out->pipe_fd[1] >= 0) {
                close(out->pipe_fd[1]);
                out->pipe_fd[1] = -1;
            }
        }
		// Sneaky comment
        if (p->semaphore_allocated) {
            /* Now wait until all players are destroyed */
            p->signal_semaphore = TRUE;
            while (p->outstanding) {
                ca_mutex_unlock(p->outstanding_mutex);
                sem_wait(&p->semaphore);
                ca_mutex_lock(p->outstanding_mutex);
            }
        }

        ca_mutex_unlock(p->outstanding_mutex);
        ca_mutex_free(p->outstanding_mutex);
    }

    if (p->theme)
        ca_theme_data_free(p->theme);

    if (p->semaphore_allocated)
        sem_destroy(&p->semaphore);

    ca_free(p);

    c->private = NULL;

    return CA_SUCCESS;
}

int driver_change_device(ca_context *c, const char *device) {
    ca_return_val_if_fail(c, CA_ERROR_INVALID);
    ca_return_val_if_fail(c->private, CA_ERROR_STATE);

    return CA_SUCCESS;
}

int driver_change_props(ca_context *c, ca_proplist *changed, ca_proplist *merged) {
    ca_return_val_if_fail(c, CA_ERROR_INVALID);
    ca_return_val_if_fail(changed, CA_ERROR_INVALID);
    ca_return_val_if_fail(merged, CA_ERROR_INVALID);

    return CA_SUCCESS;
}

int driver_cache(ca_context *c, ca_proplist *proplist) {
    ca_return_val_if_fail(c, CA_ERROR_INVALID);
    ca_return_val_if_fail(proplist, CA_ERROR_INVALID);

    return CA_ERROR_NOTSUPPORTED;
}

static int translate_error(int error) {

    switch (error) {
        case ENODEV:
        case ENOENT:
            return CA_ERROR_NOTFOUND;
        case EACCES:
        case EPERM:
            return CA_ERROR_ACCESS;
        case ENOMEM:
            return CA_ERROR_OOM;
        case EBUSY:
            return CA_ERROR_NOTAVAILABLE;
        case EINVAL:
            return CA_ERROR_INVALID;
        case ENOSYS:
            return CA_ERROR_NOTSUPPORTED;
        default:
            if (ca_debug())
                fprintf(stderr, "Got unhandled error from OSS: %s\n", strerror(error));
            return CA_ERROR_IO;
    }
}

static int open_oss(ca_context *c, struct outstanding *out) {
    struct private *p;
    int mode, val, test, ret;

    ca_return_val_if_fail(c, CA_ERROR_INVALID);
    ca_return_val_if_fail(c->private, CA_ERROR_STATE);
    ca_return_val_if_fail(out, CA_ERROR_INVALID);

    /* In OSS we have no way to configure a channel mapping for
     * multichannel streams. We cannot support those files hence */
    ca_return_val_if_fail(ca_sound_file_get_nchannels(out->file) <= 2, CA_ERROR_NOTSUPPORTED);

    p = PRIVATE(c);

    if ((out->pcm = open(c->device ? c->device : "/dev/dsp", O_WRONLY | O_NONBLOCK, 0)) < 0)
        goto finish_errno;

    if ((mode = fcntl(out->pcm, F_GETFL)) < 0)
        goto finish_errno;

    mode &= ~O_NONBLOCK;

    if (fcntl(out->pcm, F_SETFL, mode) < 0)
        goto finish_errno;

    switch (ca_sound_file_get_sample_type(out->file)) {
        case CA_SAMPLE_U8:
            val = AFMT_U8;
            break;
        case CA_SAMPLE_S16NE:
            val = AFMT_S16_NE;
            break;
        case CA_SAMPLE_S16RE:
#if __BYTE_ORDER == __LITTLE_ENDIAN
            val = AFMT_S16_BE;
#else
            val = AFMT_S16_LE;
#endif
            break;
    }

    test = val;
    if (ioctl(out->pcm, SNDCTL_DSP_SETFMT, &val) < 0)
        goto finish_errno;

    if (val != test) {
        ret = CA_ERROR_NOTSUPPORTED;
        goto finish_ret;
    }

    test = val = (int) ca_sound_file_get_nchannels(out->file);
    if (ioctl(out->pcm, SNDCTL_DSP_CHANNELS, &val) < 0)
        goto finish_errno;

    if (val != test) {
        ret = CA_ERROR_NOTSUPPORTED;
        goto finish_ret;
    }

    test = val = (int) ca_sound_file_get_rate(out->file);
    if (ioctl(out->pcm, SNDCTL_DSP_SPEED, &val) < 0)
        goto finish_errno;

    /* Check to make sure the configured rate is close enough to the
     * requested rate. */
    if (fabs((double) (val - test)) > test * 0.05) {
        ret = CA_ERROR_NOTSUPPORTED;
        goto finish_ret;
    }

    return CA_SUCCESS;

finish_errno:
    return translate_error(errno);

finish_ret:
    return ret;
}

#define BUFSIZE (4*1024)

static void* thread_func(void *userdata) {
    struct outstanding *out = userdata;
    int ret;
    void *data, *d = NULL;
    size_t fs, data_size;
    size_t nbytes = 0;
    struct pollfd pfd[2];
    nfds_t n_pfd = 2;
    struct private *p;

    p = PRIVATE(out->context);

    pthread_detach(pthread_self());

    fs = ca_sound_file_frame_size(out->file);
    data_size = (BUFSIZE/fs)*fs;

    if (!(data = ca_malloc(data_size))) {
        ret = CA_ERROR_OOM;
        goto finish;
    }

    pfd[0].fd = out->pipe_fd[0];
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;
    pfd[1].fd = out->pcm;
    pfd[1].events = POLLOUT;
    pfd[1].revents = 0;

    for (;;) {
        ssize_t bytes_written;

        if (out->dead)
            break;

        if (poll(pfd, n_pfd, -1) < 0) {
            ret = CA_ERROR_SYSTEM;
            goto finish;
        }

        /* We have been asked to shut down */
        if (pfd[0].revents)
            break;

        if (pfd[1].revents != POLLOUT) {
            ret = CA_ERROR_IO;
            goto finish;
        }

        if (nbytes <= 0) {
            nbytes = data_size;

            if ((ret = ca_sound_file_read_arbitrary(out->file, data, &nbytes)) < 0)
                goto finish;

            d = data;
        }

        if (nbytes <= 0)
            break;

        if ((bytes_written = write(out->pcm, d, nbytes)) <= 0) {
            ret = translate_error(errno);
            goto finish;
        }

        nbytes -= (size_t) bytes_written;
        d = (uint8_t*) d + (size_t) bytes_written;
    }

    ret = CA_SUCCESS;

finish:

    ca_free(data);

    if (!out->dead)
        if (out->callback)
            out->callback(out->context, out->id, ret, out->userdata);

    ca_mutex_lock(p->outstanding_mutex);

    CA_LLIST_REMOVE(struct outstanding, p->outstanding, out);

    if (!p->outstanding && p->signal_semaphore)
        sem_post(&p->semaphore);

    outstanding_free(out);

    ca_mutex_unlock(p->outstanding_mutex);

    return NULL;
}

int driver_play(ca_context *c, uint32_t id, ca_proplist *proplist, ca_finish_callback_t cb, void *userdata) {
    struct private *p;
    struct outstanding *out = NULL;
    int ret;
    pthread_t thread;

    ca_return_val_if_fail(c, CA_ERROR_INVALID);
    ca_return_val_if_fail(proplist, CA_ERROR_INVALID);
    ca_return_val_if_fail(!userdata || cb, CA_ERROR_INVALID);
    ca_return_val_if_fail(c->private, CA_ERROR_STATE);

    p = PRIVATE(c);

    if (!(out = ca_new0(struct outstanding, 1))) {
        ret = CA_ERROR_OOM;
        goto finish;
    }

    out->context = c;
    out->id = id;
    out->callback = cb;
    out->userdata = userdata;
    out->pipe_fd[0] = out->pipe_fd[1] = -1;
    out->pcm = -1;

    if (pipe(out->pipe_fd) < 0) {
        ret = CA_ERROR_SYSTEM;
        goto finish;
    }

    if ((ret = ca_lookup_sound(&out->file, NULL, &p->theme, c->props, proplist)) < 0)
        goto finish;

    if ((ret = open_oss(c, out)) < 0)
        goto finish;

    /* OK, we're ready to go, so let's add this to our list */
    ca_mutex_lock(p->outstanding_mutex);
    CA_LLIST_PREPEND(struct outstanding, p->outstanding, out);
    ca_mutex_unlock(p->outstanding_mutex);

    if (pthread_create(&thread, NULL, thread_func, out) < 0) {
        ret = CA_ERROR_OOM;

        ca_mutex_lock(p->outstanding_mutex);
        CA_LLIST_REMOVE(struct outstanding, p->outstanding, out);
        ca_mutex_unlock(p->outstanding_mutex);

        goto finish;
    }

    ret = CA_SUCCESS;

finish:

    /* We keep the outstanding struct around if we need clean up later to */
    if (ret != CA_SUCCESS)
        outstanding_free(out);

    return ret;
}

int driver_cancel(ca_context *c, uint32_t id) {
    struct private *p;
    struct outstanding *out;

    ca_return_val_if_fail(c, CA_ERROR_INVALID);
    ca_return_val_if_fail(c->private, CA_ERROR_STATE);

    p = PRIVATE(c);

    ca_mutex_lock(p->outstanding_mutex);

    for (out = p->outstanding; out; out = out->next) {

        if (out->id != id)
            continue;

        if (out->dead)
            continue;

        out->dead = TRUE;

        if (out->callback)
            out->callback(c, out->id, CA_ERROR_CANCELED, out->userdata);

        /* This will cause the thread to wakeup and terminate */
        if (out->pipe_fd[1] >= 0) {
            close(out->pipe_fd[1]);
            out->pipe_fd[1] = -1;
        }
    }

    ca_mutex_unlock(p->outstanding_mutex);

    return CA_SUCCESS;
}
