/*
 * Remote device initialization
 *
 * Copyright © 2018, 2020 Oracle and/or its affiliates.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu-common.h"

#include <stdio.h>
#include <unistd.h>

#include "qemu/module.h"
#include "remote/pcihost.h"
#include "remote/machine.h"
#include "hw/boards.h"
#include "hw/qdev-core.h"
#include "qemu/main-loop.h"
#include "remote/memory.h"
#include "io/mpqemu-link.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "sysemu/cpus.h"
#include "qemu-common.h"
#include "hw/pci/pci.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "qemu/config-file.h"
#include "sysemu/sysemu.h"
#include "sysemu/runstate.h"
#include "block/block.h"
#include "exec/ramlist.h"
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
#include "qapi/error.h"
#include "io/channel-util.h"

#include "io/channel.h"
#include "io/channel-socket.h"
#include "migration/qemu-file-types.h"
#include "migration/savevm.h"
#include "migration/qemu-file-channel.h"
#include "migration/qemu-file.h"

#include "monitor/monitor.h"
#include "chardev/char.h"
#include "sysemu/reset.h"
#include "vl.h"
#include "migration/misc.h"

static MPQemuLinkState *mpqemu_link;

PCIDevice **remote_pci_devs;
uint64_t nr_devices;
#define MAX_REMOTE_DEVICES 256

bool create_done;

char **deferred_argv;
int deferred_argc;

static void process_config_write(MPQemuMsg *msg)
{
    struct conf_data_msg *conf = (struct conf_data_msg *)msg->data2;

    if (msg->id > nr_devices) {
        return;
    }

    qemu_mutex_lock_iothread();
    pci_default_write_config(remote_pci_devs[msg->id], conf->addr, conf->val,
                             conf->l);
    qemu_mutex_unlock_iothread();
}

static void process_config_read(MPQemuMsg *msg)
{
    struct conf_data_msg *conf = (struct conf_data_msg *)msg->data2;
    uint32_t val;
    int wait;

    wait = msg->fds[0];

    if (msg->id > nr_devices) {
        return;
    }

    qemu_mutex_lock_iothread();
    val = pci_default_read_config(remote_pci_devs[msg->id], conf->addr,
                                  conf->l);
    qemu_mutex_unlock_iothread();

    notify_proxy(wait, val);

    PUT_REMOTE_WAIT(wait);
}

/* TODO: confirm memtx attrs. */
static void process_bar_write(MPQemuMsg *msg, Error **errp)
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

static void process_bar_read(MPQemuMsg *msg, Error **errp)
{
    bar_access_msg_t *bar_access = &msg->data1.bar_access;
    MPQemuMsg ret = { 0 };
    AddressSpace *as;
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
    ret.cmd = MMIO_RETURN;
    ret.data1.mmio_ret.val = val;
    ret.size = sizeof(ret.data1);
    mpqemu_msg_send(&ret, mpqemu_link->mmio);
}

static void process_get_pci_info_msg(PCIDevice *pci_dev, MPQemuMsg *msg)
{
    PCIDeviceClass *pc = PCI_DEVICE_GET_CLASS(pci_dev);
    MPQemuMsg ret = { 0 };

    ret.cmd = RET_PCI_INFO;

    ret.data1.ret_pci_info.vendor_id = pc->vendor_id;
    ret.data1.ret_pci_info.device_id = pc->device_id;
    ret.data1.ret_pci_info.class_id = pc->class_id;
    ret.data1.ret_pci_info.subsystem_id = pc->subsystem_id;

    ret.size = sizeof(ret.data1);

    mpqemu_msg_send(&ret, mpqemu_link->com);
}

static void process_device_add_msg(MPQemuMsg *msg)
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

static void process_device_del_msg(MPQemuMsg *msg)
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
}

static void process_device_reset_msg(MPQemuMsg *msg)
{
    qemu_devices_reset();
}

static int setup_device(MPQemuMsg *msg, Error **errp)
{
    QObject *obj;
    QDict *qdict;
    QString *qstr;
    QemuOpts *opts = NULL;
    DeviceState *dev = NULL;
    int wait = -1;
    int rc = -EINVAL;
    Error *local_error = NULL;

    if (msg->num_fds == 1) {
        wait = msg->fds[0];
    } else {
        error_setg(errp, "Numebr of FDs is incorrect");
        return rc;
    }

    if (!msg->data2) {
        error_setg(errp, "Message data is empty");
        return rc;
    }

    if (msg->id > MAX_REMOTE_DEVICES) {
        error_setg(errp, "id of the device is larger than max number of "\
                         "devices per remote process.");
        return rc;
    }

    qstr = qstring_from_str((char *)msg->data2);


    obj = qobject_from_json(qstring_get_str(qstr), &local_error);
    if (!obj) {
        error_setg(errp, "Could not get object!");
        goto device_failed;
    }

    qdict = qobject_to(QDict, obj);
    if (!qdict) {
        error_setg(errp, "Could not get QDict");
        goto device_failed;
    }

    g_assert(qdict_size(qdict) > 1);

    opts = qemu_opts_from_qdict(&qemu_device_opts, qdict, &local_error);
    qemu_opt_unset(opts, "rid");
    qemu_opt_unset(opts, "socket");
    qemu_opt_unset(opts, "remote");
    qemu_opt_unset(opts, "command");
    qemu_opt_unset(opts, "exec");
    qemu_opt_unset(opts, "remote-device");

    /*
     * TODO: use the bus and addr from the device options. For now
     * we use default value.
     */
    qemu_opt_unset(opts, "bus");
    qemu_opt_unset(opts, "addr");

    dev = qdev_device_add(opts, &local_error);
    if (!dev) {
        error_setg(errp, "Could not add device %s.",
                   qstring_get_str(qobject_to_json(QOBJECT(qdict))));
        goto device_failed;
    }

    if (object_dynamic_cast(OBJECT(dev), TYPE_PCI_DEVICE)) {
        if (nr_devices <= msg->id) {
            nr_devices = msg->id + 1;
            remote_pci_devs = g_realloc(remote_pci_devs,
                                        nr_devices * sizeof(PCIDevice *));
        }

        remote_pci_devs[msg->id] = PCI_DEVICE(dev);
    }

    fprintf(stderr, "->> remote added device %lu\n", nr_devices);
    notify_proxy(wait, (uint32_t)REMOTE_OK);
    qemu_opts_del(opts);
    return 0;

 device_failed:
    notify_proxy(wait, (uint32_t)REMOTE_FAIL);
    qemu_opts_del(opts);
    return rc;

}

static void process_start_mig_out(MPQemuMsg *msg)
{
    int wait = msg->fds[1];
    Error *err = NULL;
    QIOChannel *ioc;
    QEMUFile *f;

    ioc = qio_channel_new_fd(msg->fds[0], &err);
    if (err) {
        error_report_err(err);
        return;
    }

    qio_channel_set_name(QIO_CHANNEL(ioc), "remote-migration-channel");

    f = qemu_fopen_channel_output(ioc);

    bdrv_drain_all();
    (void)bdrv_flush_all();

    (void)qemu_remote_savevm(f);

    qemu_fflush(f);

    notify_proxy(wait, (uint64_t)qemu_ftell(f));
    PUT_REMOTE_WAIT(wait);

    qemu_fclose(f);
}

static int process_start_mig_in(MPQemuMsg *msg)
{
    Error *err = NULL;
    QIOChannel *ioc;
    QEMUFile *f;
    int rc = -EINVAL;

    ioc = qio_channel_new_fd(msg->fds[0], &err);
    if (err) {
        error_report_err(err);
        return rc;
    }

    qio_channel_set_name(QIO_CHANNEL(ioc), "remote-migration-channel");

    f = qemu_fopen_channel_input(ioc);

    rc = qemu_remote_loadvm(f);

    qemu_fclose(f);

    return rc;
}

static void process_msg(GIOCondition cond, MPQemuChannel *chan)
{
    MPQemuMsg *msg = NULL;
    Error *err = NULL;
    int wait;

    if ((cond & G_IO_HUP) || (cond & G_IO_ERR)) {
        goto finalize_loop;
    }

    msg = g_malloc0(sizeof(MPQemuMsg));

    if (mpqemu_msg_recv(msg, chan) < 0) {
        error_setg(&err, "Failed to receive message");
        goto finalize_loop;
    }

    if (msg->id > MAX_REMOTE_DEVICES) {
        error_setg(&err, "id of the device is larger than max number of "\
                         "devices per remote process.");
        goto finalize_loop;
    }

    switch (msg->cmd) {
    case INIT:
        break;
    case GET_PCI_INFO:
        process_get_pci_info_msg(remote_pci_devs[msg->id], msg);
        break;
    case PCI_CONFIG_WRITE:
        if (create_done) {
            process_config_write(msg);
        }
        break;
    case PCI_CONFIG_READ:
        if (create_done) {
            process_config_read(msg);
        }
        break;
    case BAR_WRITE:
        if (create_done) {
            process_bar_write(msg, &err);
            if (err) {
                error_report_err(err);
            }
        }
        break;
    case BAR_READ:
        if (create_done) {
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
        if (msg->id > nr_devices) {
            error_setg(&err, "incorrect device id in the message");
            goto finalize_loop;
        }
        process_set_irqfd_msg(remote_pci_devs[msg->id], msg);

        if (!create_done) {
            qdev_machine_creation_done();
            qemu_mutex_lock_iothread();
            qemu_run_machine_init_done_notifiers();
            qemu_mutex_unlock_iothread();
            create_done = true;
        }
        break;
    case DEV_OPTS:
        if (setup_device(msg, &err)) {
            error_report_err(err);
            goto finalize_loop;
        }
        if (deferred_argc) {
            parse_cmdline(deferred_argc, deferred_argv, NULL);
            deferred_argc = 0;
        }
        break;
    case DEVICE_ADD:
        process_device_add_msg(msg);
        break;
    case DEVICE_DEL:
        process_device_del_msg(msg);
        break;
    case REMOTE_PING:
        wait = msg->fds[0];
        notify_proxy(wait, (uint32_t)getpid());
        break;
    case DEVICE_RESET:
        process_device_reset_msg(msg);
        if (msg->num_fds == 1) {
            notify_proxy(msg->fds[0], 0);
        }
        break;
    case START_MIG_OUT:
        process_start_mig_out(msg);
        break;
    case START_MIG_IN:
        if (process_start_mig_in(msg)) {
            error_setg(&err, "Incoming migration failed.");
            goto finalize_loop;
        }
        break;
    case RUNSTATE_SET:
        remote_runstate_set(msg->data1.runstate.state);
        notify_proxy(msg->fds[0], 0);
        break;
    default:
        error_setg(&err, "Unknown command");
        goto finalize_loop;
    }

    g_free(msg->data2);
    g_free(msg);

    return;

finalize_loop:
    if (err) {
        error_report_err(err);
    }
    g_free(msg);
    mpqemu_link_finalize(mpqemu_link);
    mpqemu_link = NULL;
}

int main(int argc, char *argv[])
{
    Error *err = NULL;
    int fd = -1;

    module_call_init(MODULE_INIT_QOM);

    monitor_init_globals();

    bdrv_init_with_whitelist();

    if (qemu_init_main_loop(&err)) {
        error_report_err(err);
        return -EBUSY;
    }

    qemu_init_cpu_loop();

    page_size_init();

    qemu_mutex_init(&ram_list.mutex);

    current_machine = MACHINE(REMOTE_MACHINE(object_new(TYPE_REMOTE_MACHINE)));

    deferred_argv = argv + 3;
    deferred_argc = argc - 3;
    
    qemu_add_opts(&qemu_device_opts);
    qemu_add_opts(&qemu_drive_opts);
    qemu_add_opts(&qemu_chardev_opts);
    qemu_add_opts(&qemu_mon_opts);
    qemu_add_drive_opts(&qemu_legacy_drive_opts);
    qemu_add_drive_opts(&qemu_common_drive_opts);
    qemu_add_drive_opts(&qemu_drive_opts);
    qemu_add_drive_opts(&bdrv_runtime_opts);

    mpqemu_link = mpqemu_link_create();
    if (!mpqemu_link) {
        printf("Could not create MPQemu link\n");
        return -1;
    }

    fd = qemu_parse_fd(argv[1]);
    if (fd == -1) {
        printf("Failed to parse fd for remote process.\n");
        return -EINVAL;
    }

    mpqemu_init_channel(mpqemu_link, &mpqemu_link->com, fd);

    fd = qemu_parse_fd(argv[2]);
    if (fd == -1) {
        printf("Failed to parse fd for remote process.\n");
        return -EINVAL;
    }
    mpqemu_init_channel(mpqemu_link, &mpqemu_link->mmio, fd);

    migration_object_init();

    parse_cmdline(argc - 3, argv + 3, NULL);

    mpqemu_link_set_callback(mpqemu_link, process_msg);

    qemu_opts_foreach(qemu_find_opts("chardev"),
                      chardev_init_func, NULL, &error_fatal);

    qemu_opts_foreach(qemu_find_opts("mon"),
                      mon_init_func, NULL, &error_fatal);

    mpqemu_start_coms(mpqemu_link);

    return 0;
}
