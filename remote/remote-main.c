/*
 * Remote device initialization
 *
 * Copyright 2019, Oracle and/or its affiliates.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <glib.h>
#include <unistd.h>

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "remote/pcihost.h"
#include "remote/machine.h"
#include "hw/boards.h"
#include "hw/qdev-core.h"
#include "qemu/main-loop.h"
#include "remote/memory.h"
#include "io/proxy-link.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "sysemu/cpus.h"
#include "qemu-common.h"
#include "hw/pci/pci.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "qemu/config-file.h"
#include "sysemu/sysemu.h"
#include "block/block.h"
#include "exec/memattrs.h"
#include "exec/address-spaces.h"
#include "remote/iohub.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qobject.h"
#include "qemu/option.h"
#include "qemu/config-file.h"
#include "monitor/qdev.h"
#include "qapi/qmp/qdict.h"
#include "sysemu/sysemu.h"
#include "sysemu/blockdev.h"
#include "block/block.h"
#include "qapi/qmp/qstring.h"
#include "hw/qdev-properties.h"
#include "hw/scsi/scsi.h"
#include "block/qdict.h"
#include "qapi/qmp/qlist.h"
#include "qemu/log.h"
#include "qemu/cutils.h"
#include "remote-opts.h"

static ProxyLinkState *proxy_link;

typedef struct remote_pci_devs {
    char *id;
    DeviceState *ds;
    unsigned int created;
    QLIST_ENTRY(remote_pci_devs) next;
} remote_pci_devs;
typedef struct pci_devs_listhead pci_devs_listhead;

QLIST_HEAD(pci_devs_listhead, remote_pci_devs) pci_devs_head;
/* This is needed to distinguish between different devices being initialized. */

QemuMutex remote_ds_lock;
static bool done_init;


static remote_pci_devs *get_ds_from_list(const char *id, unsigned int size_id)
{
    remote_pci_devs *d;

    qemu_mutex_lock(&remote_ds_lock);
    QLIST_FOREACH(d, &pci_devs_head, next) {
        if (id && d->id) {
            if (strncmp(id, d->id, size_id) == 0) {
                qemu_mutex_unlock(&remote_ds_lock);
                return d;
            }
       }
    }
    qemu_mutex_unlock(&remote_ds_lock);

    return NULL;
}

static void add_to_pci_devs_list(DeviceState *dev)
{
   remote_pci_devs *d;

   if (object_dynamic_cast(OBJECT(dev), TYPE_PCI_DEVICE)) {
        d = g_malloc(sizeof(remote_pci_devs));
        d->ds = dev;
        d->id = g_strdup(dev->id);
        if (!d->id) {
            return;
        }
        qemu_mutex_lock(&remote_ds_lock);
        QLIST_INSERT_HEAD(&pci_devs_head, d, next);
        qemu_mutex_unlock(&remote_ds_lock);

    }
}

static void del_from_pci_devs_list(const char *id)
{
    remote_pci_devs *d;

    d = get_ds_from_list((char *)id, strlen((char *)id));
    if (!d) {
        return;
    }
    g_free(d->id);
    qemu_mutex_lock(&remote_ds_lock);
    QLIST_REMOVE(d, next);
    qemu_mutex_unlock(&remote_ds_lock);
    g_free(d);
}

static void process_config_write(ProcMsg *msg, DeviceState *ds)
{
    struct conf_data_msg *conf = (struct conf_data_msg *)msg->data2;

    qemu_mutex_lock_iothread();
    pci_default_write_config(PCI_DEVICE(ds), conf->addr, conf->val, conf->l);
    qemu_mutex_unlock_iothread();
}

static void process_config_read(ProcMsg *msg, DeviceState *ds)
{
    struct conf_data_msg *conf = (struct conf_data_msg *)msg->data2;
    uint32_t val;
    int wait;

    wait = msg->fds[0];

    qemu_mutex_lock_iothread();
    val = pci_default_read_config(PCI_DEVICE(ds), conf->addr, conf->l);
    qemu_mutex_unlock_iothread();

    notify_proxy(wait, val);

    PUT_REMOTE_WAIT(wait);
}

/* TODO: confirm memtx attrs. */
static void process_bar_write(ProcMsg *msg, Error **errp)
{
    bar_access_msg_t *bar_access = &msg->data1.bar_access;
    AddressSpace *as =
        bar_access->memory ? &address_space_memory : &address_space_io;
    MemTxResult res;

    res = address_space_rw(as, bar_access->addr, MEMTXATTRS_UNSPECIFIED,
                           (uint8_t *)&bar_access->val, bar_access->size, true);

    if (res != MEMTX_OK) {
        error_setg(errp, "Could not perform address space write operation,"
                   " inaccessible address: %lx.", bar_access->addr);
    }
}

static void process_bar_read(ProcMsg *msg, Error **errp)
{
    bar_access_msg_t *bar_access = &msg->data1.bar_access;
    AddressSpace *as;
    int wait = msg->fds[0];
    MemTxResult res;
    uint64_t val = 0;

    as = bar_access->memory ? &address_space_memory : &address_space_io;

    assert(bar_access->size <= sizeof(uint64_t));

    res = address_space_rw(as, bar_access->addr, MEMTXATTRS_UNSPECIFIED,
                           (uint8_t *)&val, bar_access->size, false);

    if (res != MEMTX_OK) {
        error_setg(errp, "Could not perform address space read operation,"
                   " inaccessible address: %lx.", bar_access->addr);
        val = (uint64_t)-1;
        goto fail;
    }

    switch (bar_access->size) {
    case 4:
        val = *((uint32_t *)&val);
        break;
    case 2:
        val = *((uint16_t *)&val);
        break;
    case 1:
        val = *((uint8_t *)&val);
        break;
    default:
        error_setg(errp, "Invalid PCI BAR read size");
        return;
    }

fail:
    notify_proxy(wait, val);

    PUT_REMOTE_WAIT(wait);
}

static void process_device_add_msg(ProcMsg *msg)
{
    Error *local_err = NULL;
    const char *json = (const char *)msg->data2;
    int wait = msg->fds[0];
    QObject *qobj = NULL;
    QDict *qdict = NULL;
    QemuOpts *opts = NULL;

    qobj = qobject_from_json(json, &local_err);
    if (local_err) {
        goto fail;
    }

    qdict = qobject_to(QDict, qobj);
    assert(qdict);

    opts = qemu_opts_from_qdict(qemu_find_opts("device"), qdict, &local_err);
    if (local_err) {
        goto fail;
    }

    (void)qdev_device_add(opts, &local_err);
    if (local_err) {
        goto fail;
    }

fail:
    if (local_err) {
        error_report_err(local_err);
        /* TODO: communicate the exact error message to proxy */
    }

    notify_proxy(wait, 1);

    PUT_REMOTE_WAIT(wait);
}

static void process_device_del_msg(ProcMsg *msg)
{
    Error *local_err = NULL;
    DeviceState *dev = NULL;
    const char *json = (const char *)msg->data2;
    int wait = msg->fds[0];
    QObject *qobj = NULL;
    QDict *qdict = NULL;
    const char *id;

    qobj = qobject_from_json(json, &local_err);
    if (local_err) {
        goto fail;
    }

    qdict = qobject_to(QDict, qobj);
    assert(qdict);

    id = qdict_get_try_str(qdict, "id");
    assert(id);

    dev = find_device_state(id, &local_err);
    if (local_err) {
        goto fail;
    }

    if (dev) {
        qdev_unplug(dev, &local_err);
    }

fail:
    if (local_err) {
        error_report_err(local_err);
        /* TODO: communicate the exact error message to proxy */
    }

    notify_proxy(wait, 1);

    PUT_REMOTE_WAIT(wait);

    del_from_pci_devs_list((const char *)msg->id);
}

static int init_drive(QDict *rqdict, Error **errp)
{
    QemuOpts *opts;
    Error *local_error = NULL;

    if (rqdict != NULL && qdict_size(rqdict) > 0) {
        opts = qemu_opts_from_qdict(&qemu_drive_opts,
                                    rqdict, &local_error);
        if (!opts) {
            error_propagate(errp, local_error);
            return -EINVAL;
        }
    } else {
        return -EINVAL;
    }

    qemu_opt_unset(opts, "rid");
    qemu_opt_unset(opts, "socket");
    qemu_opt_unset(opts, "remote");
    qemu_opt_unset(opts, "command");

    if (drive_new(opts, IF_IDE, &local_error) == NULL) {
        error_propagate(errp, local_error);
        return -EINVAL;
    }

    return 0;
}

static int setup_drive(ProcMsg *msg, Error **errp)
{
    QObject *obj;
    QDict *qdict;
    QString *qstr;
    Error *local_error = NULL;
    int rc = -EINVAL;

    if (!msg->data2) {
        return rc;
    }

    qstr = qstring_from_str((char *)msg->data2);
    obj = qobject_from_json(qstring_get_str(qstr), &local_error);
    if (!obj) {
        error_propagate(errp, local_error);
        return rc;
    }

    qdict = qobject_to(QDict, obj);
    if (!qdict) {
        return rc;
    }

    if (init_drive(qdict, &local_error)) {
        error_setg(errp, "init_drive failed in setup_drive.");
        return rc;
    }

    return 0;
}

static int setup_device(ProcMsg *msg, Error **errp)
{
    QObject *obj;
    QDict *qdict;
    QString *qstr;
    QemuOpts *opts;
    DeviceState *dev = NULL;
    int rc = -EINVAL;
    Error *local_error = NULL;

    if (!msg->data2) {
        return rc;
    }

    qstr = qstring_from_str((char *)msg->data2);
    obj = qobject_from_json(qstring_get_str(qstr), &local_error);
    if (!obj) {
        error_setg(errp, "Could not get object");
        return rc;
    }

    qdict = qobject_to(QDict, obj);
    if (!qdict) {
        return rc;
    }

    g_assert(qdict_size(qdict) > 1);

    opts = qemu_opts_from_qdict(&qemu_device_opts, qdict, &local_error);
    qemu_opt_unset(opts, "rid");
    qemu_opt_unset(opts, "socket");
    qemu_opt_unset(opts, "remote");
    qemu_opt_unset(opts, "command");
    /*
     * TODO: use the bus and addr from the device options. For now
     * we use default value.
     */
    qemu_opt_unset(opts, "bus");
    qemu_opt_unset(opts, "addr");

    dev = qdev_device_add(opts, &local_error);
    if (!dev) {
        error_setg(errp, "Could not add device %s",
                   qstring_get_str(qobject_to_json(QOBJECT(qdict))));
        return rc;
    }

    add_to_pci_devs_list(dev);
    qemu_opts_del(opts);

    return 0;
}

static void process_msg(GIOCondition cond, ProcChannel *chan)
{
    ProcMsg *msg = NULL;
    Error *err = NULL;
    remote_pci_devs *r = NULL;

    if ((cond & G_IO_HUP) || (cond & G_IO_ERR)) {
        error_setg(&err, "socket closed, cond is %d", cond);
        proxy_link_finalize(proxy_link);
        proxy_link = NULL;
        return;
    }

    msg = g_malloc0(sizeof(ProcMsg));

    if (proxy_proc_recv(proxy_link, msg, chan) < 0) {
        error_setg(&err, "Failed to receive message");
        goto finalize_loop;
    }

    if (msg->cmd != DEV_OPTS && msg->cmd != DRIVE_OPTS &&
        msg->cmd != SYNC_SYSMEM) {
        r = get_ds_from_list((const char *)msg->id, msg->size_id);
        if (!r) {
            error_setg(&err, "Message was received for unknown device");
            goto exit_loop;
        }
    }

    switch (msg->cmd) {
    case INIT:
        break;
    case CONF_WRITE:
        if (r->created) {
            process_config_write(msg, r->ds);
        }

        break;
    case CONF_READ:
        if (r->created) {
            process_config_read(msg, r->ds);
        }

        break;
    case BAR_WRITE:
        if (r->created) {
            process_bar_write(msg, &err);
            if (err) {
                error_report_err(err);
            }
        }
        break;
    case BAR_READ:
        if (r->created) {
            process_bar_read(msg, &err);
            if (err) {
                error_report_err(err);
            }
        }
        break;
    case SYNC_SYSMEM:
        /*
         * TODO: ensure no active DMA is happening when
         * sysmem is being updated
         */
        remote_sysmem_reconfig(msg, &err);
        if (err) {
            error_report_err(err);
            goto finalize_loop;
        }
        break;
    case SET_IRQFD:
        process_set_irqfd_msg(PCI_DEVICE(r->ds), msg);
        r->created = true;
        if (!done_init) {
            qdev_machine_creation_done();
            qemu_mutex_lock_iothread();
            qemu_run_machine_init_done_notifiers();
            qemu_mutex_unlock_iothread();
            done_init = true;
        }
        break;
    case DRIVE_OPTS:
        if (setup_drive(msg, &err)) {
            error_report_err(err);
        }
        break;
    case DEV_OPTS:
        if (setup_device(msg, &err)) {
            error_report_err(err);
        }
        break;
    case DEVICE_ADD:
        process_device_add_msg(msg);
        break;
    case DEVICE_DEL:
        process_device_del_msg(msg);
        break;
    default:
        error_setg(&err, "Unknown command");
        goto finalize_loop;
    }

exit_loop:
    if (msg->id) {
        free(msg->id);
    }
    g_free(msg);

    return;

finalize_loop:
    error_report_err(err);
    if (msg->id) {
        free(msg->id);
    }
    g_free(msg);

    proxy_link_finalize(proxy_link);
    proxy_link = NULL;
}

int main(int argc, char *argv[], char **envp)
{
    Error *err = NULL;
    int fd = -1;

    module_call_init(MODULE_INIT_QOM);

    bdrv_init_with_whitelist();

    if (qemu_init_main_loop(&err)) {
        error_report_err(err);
        return -EBUSY;
    }

    qemu_init_cpu_loop();

    page_size_init();

    current_machine = MACHINE(REMOTE_MACHINE(object_new(TYPE_REMOTE_MACHINE)));

    qemu_add_opts(&qemu_device_opts);
    qemu_add_opts(&qemu_drive_opts);
    qemu_add_drive_opts(&qemu_legacy_drive_opts);
    qemu_add_drive_opts(&qemu_common_drive_opts);
    qemu_add_drive_opts(&qemu_drive_opts);
    qemu_add_drive_opts(&bdrv_runtime_opts);

    proxy_link = proxy_link_create();
    if (!proxy_link) {
        printf("Could not create proxy link\n");
        return -1;
    }

    fd = qemu_parse_fd(argv[1]);
    if (fd == -1) {
        printf("Failed to parse fd for remote process.\n");
        return -EINVAL;
    }

    proxy_link_init_channel(proxy_link, &proxy_link->com, fd);

    parse_cmdline(argc - 2, argv + 2, NULL);
    qemu_mutex_init(&remote_ds_lock);
    QLIST_INIT(&pci_devs_head);

    proxy_link_set_callback(proxy_link, process_msg);

    start_handler(proxy_link);

    return 0;
}
