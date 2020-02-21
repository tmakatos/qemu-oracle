/*
 * Communication channel between QEMU and remote device process
 *
 * Copyright © 2018, 2020 Oracle and/or its affiliates.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include <poll.h>

#include "qemu/module.h"
#include "io/mpqemu-link.h"
#include "qemu/log.h"
#include "qemu/lockable.h"

GSourceFuncs gsrc_funcs;

static void mpqemu_link_inst_init(Object *obj)
{
    MPQemuLinkState *s = MPQEMU_LINK(obj);

    s->ctx = g_main_context_default();
    s->loop = g_main_loop_new(s->ctx, FALSE);
}

static const TypeInfo mpqemu_link_info = {
    .name = TYPE_MPQEMU_LINK,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(MPQemuLinkState),
    .instance_init = mpqemu_link_inst_init,
};

static void mpqemu_link_register_types(void)
{
    type_register_static(&mpqemu_link_info);
}

type_init(mpqemu_link_register_types)

MPQemuLinkState *mpqemu_link_create(void)
{
    MPQemuLinkState *link = MPQEMU_LINK(object_new(TYPE_MPQEMU_LINK));

    link->com = NULL;
    link->dev = NULL;

    link->opaque = NULL;

    return link;
}

void mpqemu_link_finalize(MPQemuLinkState *s)
{
    g_main_loop_unref(s->loop);
    g_main_context_unref(s->ctx);
    g_main_loop_quit(s->loop);

    mpqemu_destroy_channel(s->com);

    object_unref(OBJECT(s));
}

void mpqemu_msg_send(MPQemuMsg *msg, MPQemuChannel *chan)
{
    int rc;
    uint8_t *data;
    union {
        char control[CMSG_SPACE(REMOTE_MAX_FDS * sizeof(int))];
        struct cmsghdr align;
    } u;
    struct msghdr hdr;
    struct cmsghdr *chdr;
    int sock = chan->sock;
    QemuMutex *lock = &chan->send_lock;

    struct iovec iov = {
        .iov_base = (char *) msg,
        .iov_len = MPQEMU_MSG_HDR_SIZE,
    };

    memset(&hdr, 0, sizeof(hdr));
    memset(&u, 0, sizeof(u));

    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;

    if (msg->num_fds > REMOTE_MAX_FDS) {
        qemu_log_mask(LOG_REMOTE_DEBUG, "%s: Max FDs exceeded\n", __func__);
        return;
    }

    if (msg->num_fds > 0) {
        size_t fdsize = msg->num_fds * sizeof(int);

        hdr.msg_control = &u;
        hdr.msg_controllen = sizeof(u);

        chdr = CMSG_FIRSTHDR(&hdr);
        chdr->cmsg_len = CMSG_LEN(fdsize);
        chdr->cmsg_level = SOL_SOCKET;
        chdr->cmsg_type = SCM_RIGHTS;
        memcpy(CMSG_DATA(chdr), msg->fds, fdsize);
        hdr.msg_controllen = CMSG_SPACE(fdsize);
    }

    WITH_QEMU_LOCK_GUARD(lock) {
        do {
            rc = sendmsg(sock, &hdr, 0);
        } while (rc < 0 && (errno == EINTR || errno == EAGAIN));

        if (rc < 0) {
            qemu_log_mask(LOG_REMOTE_DEBUG, "%s - sendmsg rc is %d, "
                          "errno is %d, sock %d\n", __func__, rc, errno, sock);
            return;
        }

        if (msg->bytestream) {
            data = msg->data2;
        } else {
            data = (uint8_t *)msg + MPQEMU_MSG_HDR_SIZE;
        }

        do {
            rc = write(sock, data, msg->size);
        } while (rc < 0 && (errno == EINTR || errno == EAGAIN));
    }
}


int mpqemu_msg_recv(MPQemuMsg *msg, MPQemuChannel *chan)
{
    int rc;
    uint8_t *data;
    union {
        char control[CMSG_SPACE(REMOTE_MAX_FDS * sizeof(int))];
        struct cmsghdr align;
    } u;
    struct msghdr hdr;
    struct cmsghdr *chdr;
    size_t fdsize;
    int sock = chan->sock;
    QemuMutex *lock = &chan->recv_lock;

    struct iovec iov = {
        .iov_base = (char *) msg,
        .iov_len = MPQEMU_MSG_HDR_SIZE,
    };

    memset(&hdr, 0, sizeof(hdr));
    memset(&u, 0, sizeof(u));

    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = &u;
    hdr.msg_controllen = sizeof(u);

    WITH_QEMU_LOCK_GUARD(lock) {
        do {
            rc = recvmsg(sock, &hdr, 0);
        } while (rc < 0 && (errno == EINTR || errno == EAGAIN));

        if (rc < 0) {
            qemu_log_mask(LOG_REMOTE_DEBUG, "%s - recvmsg rc is %d, "
                          "errno is %d, sock %d\n", __func__, rc, errno, sock);
            return rc;
        }

        msg->num_fds = 0;
        for (chdr = CMSG_FIRSTHDR(&hdr); chdr != NULL;
             chdr = CMSG_NXTHDR(&hdr, chdr)) {
            if ((chdr->cmsg_level == SOL_SOCKET) &&
                (chdr->cmsg_type == SCM_RIGHTS)) {
                fdsize = chdr->cmsg_len - CMSG_LEN(0);
                msg->num_fds = fdsize / sizeof(int);
                if (msg->num_fds > REMOTE_MAX_FDS) {
                    qemu_log_mask(LOG_REMOTE_DEBUG,
                                  "%s: Max FDs exceeded\n", __func__);
                    return -ERANGE;
                }

                memcpy(msg->fds, CMSG_DATA(chdr), fdsize);
                break;
            }
        }

        if (msg->bytestream) {
            if (!msg->size) {
                qemu_mutex_unlock(lock);
                return -EINVAL;
            }

            msg->data2 = calloc(1, msg->size);
            data = msg->data2;
        } else {
            data = (uint8_t *)&msg->data1;
        }

        if (msg->size) {
            do {
                rc = read(sock, data, msg->size);
            } while (rc < 0 && (errno == EINTR || errno == EAGAIN));
        }
    }
    return rc;
}

/*
 * wait_for_remote() Synchronizes QEMU and the remote process. The maximum
 *                   wait time is 1s, after which the wait times out.
 *                   The function alse returns a 64 bit return value after
 *                   the wait. The function uses eventfd() to do the wait
 *                   and pass the return values. eventfd() can't return a
 *                   value of '0'. Therefore, all return values are offset
 *                   by '1' at the sending end, and corrected at the
 *                   receiving end.
 */

uint64_t wait_for_remote(int efd)
{
    struct pollfd pfd = { .fd = efd, .events = POLLIN };
    uint64_t val;
    int ret;

    ret = poll(&pfd, 1, 1000);

    switch (ret) {
    case 0:
        qemu_log_mask(LOG_REMOTE_DEBUG, "Error wait_for_remote: Timed out\n");
        /* TODO: Kick-off error recovery */
        return UINT64_MAX;
    case -1:
        qemu_log_mask(LOG_REMOTE_DEBUG, "Poll error wait_for_remote: %s\n",
                      strerror(errno));
        return UINT64_MAX;
    default:
        if (read(efd, &val, sizeof(val)) == -1) {
            qemu_log_mask(LOG_REMOTE_DEBUG, "Error wait_for_remote: %s\n",
                          strerror(errno));
            return UINT64_MAX;
        }
    }

    /*
     * The remote process could write a non-zero value
     * to the eventfd to wake QEMU up. However, the drawback of using eventfd
     * for this purpose is that a return value of zero wouldn't wake QEMU up.
     * Therefore, we offset the return value by one at the remote process and
     * correct it in the QEMU end.
     */
    val = (val == UINT64_MAX) ? val : (val - 1);

    return val;
}

void notify_proxy(int efd, uint64_t val)
{
    val = (val == UINT64_MAX) ? val : (val + 1);
    ssize_t len = -1;

    len = write(efd, &val, sizeof(val));
    if (len == -1 || len != sizeof(val)) {
        qemu_log_mask(LOG_REMOTE_DEBUG, "Error notify_proxy: %s\n",
                      strerror(errno));
    }
}

static gboolean mpqemu_link_handler_prepare(GSource *gsrc, gint *timeout)
{
    g_assert(timeout);

    *timeout = -1;

    return FALSE;
}

static gboolean mpqemu_link_handler_check(GSource *gsrc)
{
    MPQemuChannel *chan = (MPQemuChannel *)gsrc;

    return chan->gpfd.events & chan->gpfd.revents;
}

static gboolean mpqemu_link_handler_dispatch(GSource *gsrc, GSourceFunc func,
                                             gpointer data)
{
    MPQemuLinkState *s = (MPQemuLinkState *)data;
    MPQemuChannel *chan = (MPQemuChannel *)gsrc;

    s->callback(chan->gpfd.revents, s, chan);

    if ((chan->gpfd.revents & G_IO_HUP) || (chan->gpfd.revents & G_IO_ERR)) {
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

void mpqemu_link_set_callback(MPQemuLinkState *s, mpqemu_link_callback callback)
{
    s->callback = callback;
}

void mpqemu_init_channel(MPQemuLinkState *s, MPQemuChannel **chan, int fd)
{
    MPQemuChannel *src;

    gsrc_funcs = (GSourceFuncs){
        .prepare = mpqemu_link_handler_prepare,
        .check = mpqemu_link_handler_check,
        .dispatch = mpqemu_link_handler_dispatch,
        .finalize = NULL,
    };

    src = (MPQemuChannel *)g_source_new(&gsrc_funcs, sizeof(MPQemuChannel));

    src->sock = fd;
    qemu_mutex_init(&src->send_lock);
    qemu_mutex_init(&src->recv_lock);

    g_source_set_callback(&src->gsrc, NULL, (gpointer)s, NULL);
    src->gpfd.fd = fd;
    src->gpfd.events = G_IO_IN | G_IO_HUP | G_IO_ERR;
    g_source_add_poll(&src->gsrc, &src->gpfd);

    *chan = src;
}

void mpqemu_destroy_channel(MPQemuChannel *chan)
{
    g_source_unref(&chan->gsrc);
    close(chan->sock);
    qemu_mutex_destroy(&chan->send_lock);
    qemu_mutex_destroy(&chan->recv_lock);
}

void mpqemu_start_coms(MPQemuLinkState *s, MPQemuChannel* chan)
{
    g_assert(g_source_attach(&chan->gsrc, s->ctx));

    g_main_loop_run(s->loop);
}

bool mpqemu_msg_valid(MPQemuMsg *msg)
{
    if (msg->cmd >= MAX) {
        return false;
    }

    if (msg->bytestream) {
        if (!msg->data2) {
            return false;
        }
    } else {
        if (msg->data2) {
            return false;
        }
    }

    /* Verify FDs. */
    if (msg->num_fds >= REMOTE_MAX_FDS) {
        return false;
    }
    if (msg->num_fds > 0) {
        for (int i = 0; i < msg->num_fds; i++) {
            if (fcntl(msg->fds[i], F_GETFL) == -1) {
                return false;
            }
        }
    }
     /* Verify message specific fields. */
    switch (msg->cmd) {
    case SYNC_SYSMEM:
        if (msg->num_fds == 0 || msg->bytestream != 0) {
            return false;
        }
        if (msg->size != sizeof(msg->data1)) {
            return false;
        }
        break;
    case PCI_CONFIG_WRITE:
    case PCI_CONFIG_READ:
        if (msg->size != sizeof(struct conf_data_msg)) {
            return false;
        }
        break;
    case BAR_WRITE:
    case BAR_READ:
    case SET_IRQFD:
        if (msg->size != sizeof(msg->data1)) {
            return false;
        }
        break;
    default:
        break;
    }

    return true;
}