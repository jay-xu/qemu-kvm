/*
 * Dimm device for Memory Hotplug
 *
 * Copyright ProfitBricks GmbH 2012
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>
 */

#include "trace.h"
#include "qdev.h"
#include "dimm.h"
#include <time.h>
#include "../exec-memory.h"
#include "qmp-commands.h"

static DeviceState *dimm_hotplug_qdev;
static dimm_hotplug_fn dimm_hotplug;
static dimm_calcoffset_fn dimm_calcoffset;

static QLIST_HEAD(dimm_hp_result_head, dimm_hp_result)  dimm_hp_result_queue;

void dimm_populate(DimmState *s)
{
    DeviceState *dev= (DeviceState*)s;
    MemoryRegion *new = NULL;

    new = g_malloc(sizeof(MemoryRegion));
    memory_region_init_ram(new, NULL, dev->id, s->size);
    //vmstate_register_ram_global(new);
    memory_region_add_subregion(get_system_memory(), s->start, new);
    s->mr = new;
    s->populated = true;
}


void dimm_depopulate(DimmState *s)
{
    assert(s);
    if (s->populated) {
        //vmstate_unregister_ram(s->mr, NULL);
        memory_region_del_subregion(get_system_memory(), s->mr);
        memory_region_destroy(s->mr);
        s->populated = false;
        s->depopulate_pending = false;
        s->mr = NULL;
    }
}

DimmState *dimm_create(char *id, uint64_t size, uint64_t node, uint32_t
        dimm_idx, bool populated)
{
    DeviceState *dev;
    DimmState *mdev;

    dev = sysbus_create_simple("dimm", -1, NULL);
    dev->id = id;

    mdev = DIMM(dev);
    mdev->idx = dimm_idx;
    mdev->start = 0;
    mdev->size = size;
    mdev->node = node;
    mdev->start = dimm_calcoffset(size);
    mdev->populated = populated;

    return mdev;
}

void dimm_register_hotplug(dimm_hotplug_fn hotplug, DeviceState *qdev)
{
    dimm_hotplug_qdev = qdev;
    dimm_hotplug = hotplug;
    dimm_scan_populated();
}

void dimm_register_calcoffset(dimm_calcoffset_fn calcoffset)
{
    dimm_calcoffset = calcoffset;
}

void dimm_setstart(DimmState *slot)
{
    assert(dimm_calcoffset);
    slot->start = dimm_calcoffset(slot->size);
    fprintf(stderr, "%s start address for slot %p is %lu\n", __FUNCTION__,
            slot, slot->start);
}

void dimm_activate(DimmState *slot)
{
    dimm_populate(slot);
    if (dimm_hotplug)
        dimm_hotplug(dimm_hotplug_qdev, (SysBusDevice*)slot, 1);
}

void dimm_deactivate(DimmState *slot)
{
    slot->depopulate_pending = true;
    if (dimm_hotplug)
        dimm_hotplug(dimm_hotplug_qdev, (SysBusDevice*)slot, 0);
}

DimmState *dimm_find_from_name(char *id)
{
    DeviceState *qdev;
    const char *type;
    qdev = qdev_find_recursive(sysbus_get_default(), id);
    if (qdev) {
        type = qdev->info->name;
        if (!type) {
            return NULL;
        }
        if (!strcmp(type, "dimm")) {
            return DIMM(qdev);
        }
    }    
    return NULL;
}

int dimm_do(Monitor *mon, const QDict *qdict, bool add)
{
    DimmState *slot = NULL;

    char *id = (char*) qdict_get_try_str(qdict, "id");
    if (!id) {
        fprintf(stderr, "ERROR %s invalid id\n",__FUNCTION__);
        return 1;
    }

    slot = dimm_find_from_name(id);

    if (!slot) {
        fprintf(stderr, "%s no slot %s found\n", __FUNCTION__, id);
        return 1;
    }

    if (add) {
        if (slot->populated) {
            fprintf(stderr, "ERROR %s slot %s already populated\n",
                    __FUNCTION__, id);
            return 1;
        }
        dimm_activate(slot);
    }
    else {
        if (!slot->populated) {
            fprintf(stderr, "ERROR %s slot %s is not populated\n",
                    __FUNCTION__, id);
            return 1;
        }
        dimm_deactivate(slot);
    }

    return 0;
}

/* Find for dimm_do_range operation
    DIMM_MIN_UNPOPULATED: used for finding next DIMM to hotplug
    DIMM_MAX_POPULATED: used for finding next DIMM for hot-unplug
 */

DimmState *dimm_find_next(char *pfx, uint32_t mode)
{
    DeviceState *dev;
    DimmState *slot, *ret;
    const char *type;
    uint32_t idx;

    ret = NULL;
    BusState *bus = sysbus_get_default();

    if (mode == DIMM_MIN_UNPOPULATED)
        idx =  MAX_DIMMS;
    else if (mode == DIMM_MAX_POPULATED)
        idx = 0;
    else
        return false;

    QTAILQ_FOREACH(dev, &bus->children, sibling) {
        type = dev->info->name;
        if (!type) {
            fprintf(stderr, "error getting device type\n");
            return NULL;
        }
        if (!strcmp(type, "dimm")) {
            slot = DIMM(dev);
            if (strstr(dev->id, pfx) && strcmp(dev->id, pfx)) {
                if (mode == DIMM_MIN_UNPOPULATED &&
                        (slot->populated == false) &&
                        (idx > slot->idx)) {
                    idx = slot->idx;
                    ret = slot;
                }
                else if (mode == DIMM_MAX_POPULATED &&
                        (slot->populated == true) &&
                        (slot->depopulate_pending == false) &&
                        (idx <= slot->idx)) {
                    idx = slot->idx;
                    ret = slot;
                }
            }
        }
    }
    return ret;
}

int dimm_do_range(Monitor *mon, const QDict *qdict, bool add)
{
    DimmState *slot = NULL;
    uint32_t mode;
    uint32_t idx;
    int num, ndimms;

    char *pfx = (char*) qdict_get_try_str(qdict, "pfx");
    if (!pfx) {
        fprintf(stderr, "ERROR %s invalid pfx\n",__FUNCTION__);
        return 1;
    }

    char *value = (char*) qdict_get_try_str(qdict, "num");
    if (!value) {
        fprintf(stderr, "ERROR %s invalid pfx\n",__FUNCTION__);
        return 1;
    }
    num = atoi(value);

    if (add)
        mode = DIMM_MIN_UNPOPULATED;
    else
        mode = DIMM_MAX_POPULATED;

    ndimms = 0;
    while (ndimms < num) {
        slot = dimm_find_next(pfx, mode);
        if (slot == NULL) {
            fprintf(stderr, "%s no further slot found for pool %s\n",
                    __FUNCTION__, pfx);
            fprintf(stderr, "%s operated on %d / %d requested dimms\n",
                    __FUNCTION__, ndimms, num);
            return 1;
        }

        if (add) {
            dimm_activate(slot);
        }
        else {
            dimm_deactivate(slot);
        }
        ndimms++;
        idx++;
    }

    return 0;
}

DimmState *dimm_find_from_idx(uint32_t idx)
{
    DeviceState *dev;
    DimmState *slot;
    const char *type;
    BusState *bus = sysbus_get_default();

    QTAILQ_FOREACH(dev, &bus->children, sibling) {
        type = dev->info->name;
        if (!type) {
            fprintf(stderr, "error getting device type\n");
            return NULL;
        }
        if (!strcmp(type, "dimm")) {
            slot = DIMM(dev);
            if (slot->idx == idx) {
                /*fprintf(stderr, "%s found slot with idx %u : %p\n",
                        __FUNCTION__, idx, slot);*/
                return slot;
            }
        }
    }
    return NULL;
}

int dimm_set_populated(DimmState *s)
{
    if (s) {
        s->populated = true;
        return 0;
    }    
    else return -1;
}

/* used to populateand activate dimms at boot time */
void dimm_scan_populated(void)
{
    DeviceState *dev;
    DimmState *slot;
    const char *type;
    BusState *bus = sysbus_get_default();
    QTAILQ_FOREACH(dev, &bus->children, sibling) {
        type = dev->info->name;
        if (!type) {
            fprintf(stderr, "error getting device type\n");
            exit(1);
        }

        if (!strcmp(type, "dimm")) {
            if (!dev->id) {
                fprintf(stderr, "error getting dimm device id\n");
                exit(1);
            }
            slot = DIMM(dev);
            if (slot->populated && !slot->mr) {
                /*fprintf(stderr, "%s slot %d PRE-POPULATE\n", __FUNCTION__,
                 * slot->idx);*/
                dimm_activate(slot);
            }
        }
    }
}

void dimm_notify(uint32_t idx, uint32_t event)
{
    DimmState *s;
    s = dimm_find_from_idx(idx);

    assert(s != NULL);
    struct dimm_hp_result *result = g_malloc0(sizeof(*result));
    result->ret = event;
    result->s = s;

    switch(event) {
        case DIMM_REMOVESUCCESS_NOTIFY:
            dimm_depopulate(s);
            QLIST_INSERT_HEAD(&dimm_hp_result_queue, result, next);
            break;
        case DIMM_REMOVEFAIL_NOTIFY:
            s->depopulate_pending = false;
            /* revert bitmap state, without triggering an acpi event. Seabios
             * also reverts its internal state on _OST failure.
             */
            if (dimm_hotplug)
                dimm_hotplug(dimm_hotplug_qdev, (SysBusDevice*)s, 2);
            QLIST_INSERT_HEAD(&dimm_hp_result_queue, result, next);
            break;
            /* although hot-remove may have been successfull for some 128MB
             * sections of the DIMM, it is dangerous to depopulate.
             */
        case DIMM_ADDSUCCESS_NOTIFY:
        case DIMM_ADDFAIL_NOTIFY:
            /* depopulate is dangerous, because hot-add could be a a partial success,
             * for some of the sections of the DIMM. FIXME
             */
            QLIST_INSERT_HEAD(&dimm_hp_result_queue, result, next);
            break;
        default:
            g_free(result);
            break;
    }
}

void dimm_state_sync(uint8_t *dimm_sts)
{
    DimmState *s = NULL;
    uint32_t i, temp = 1;

    for(i = 0; i < MAX_DIMMS; i++) {
        s = dimm_find_from_idx(i);
        if (!s)
            break;
        if (i % 8 == 0) {
            temp = 1;
            dimm_sts[i / 8] = 0;
        }
        else
            temp = temp << 1;
        if (s->populated) {
            //fprintf(stderr, "dimm %u\n", i);
            dimm_sts[i / 8] |= temp;
            s->depopulate_pending = false;
        }
    }
}

MemHpInfoList *qmp_query_memhp(Error **errp)
{
    MemHpInfoList *head = NULL, *cur_item = NULL, *info;
    struct dimm_hp_result *item, *nextitem;

    QLIST_FOREACH_SAFE(item, &dimm_hp_result_queue, next, nextitem) {

        info = g_malloc0(sizeof(*info));
        info->value = g_malloc0(sizeof(*info->value));
        info->value->Dimm = (char*)item->s->busdev.qdev.id;
        info->value->result = item->ret;
        /* XXX: waiting for the qapi to support GSList */
        if (!cur_item) {
            head = cur_item = info;
        } else {
            cur_item->next = info;
            cur_item = info;
        }

        /* hotplug notification copied to qmp list, delete original item */
        QLIST_REMOVE(item, next);
        g_free(item);
    }

    return head;
}

int64_t qmp_query_memtotal(Error **errp)
{
    DeviceState *dev;
    DimmState *slot;
    const char *type;
    BusState *bus = sysbus_get_default();
    uint64_t info = ram_size;

    QTAILQ_FOREACH(dev, &bus->children, sibling) {
        type = dev->info->name;
        if (!type) {
            fprintf(stderr, "error getting device type\n");
            exit(1);
        }

        if (!strcmp(type, "dimm")) {
            if (!dev->id) {
                fprintf(stderr, "error getting dimm device id\n");
                exit(1);
            }
            slot = DIMM(dev);
            if (slot->populated) {
                info += slot->size;
            }
        }
    }
    return (int64_t)info;
}

static int dimm_init(SysBusDevice *s)
{
    DimmState *slot;
    slot = DIMM(s);
    slot->mr = NULL;
    slot->populated = false;
    slot->depopulate_pending = false;
    return 0;
}


/*
static const VMStateDescription vmstate_dimm = {
    .name = "dimm",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .load_state_old = dimm_load_old,
    .fields      = (VMStateField []) {
        VMSTATE_END_OF_LIST()
    }
};*/

static SysBusDeviceInfo dimm_info = {
    .init = dimm_init,
    .qdev.name = "dimm",
    .qdev.size = sizeof(DimmState),
    .qdev.vmsd = NULL,
    .qdev.reset = NULL,
    .qdev.no_user = 1,
    .qdev.props = (Property[]) {
        DEFINE_PROP_END_OF_LIST(),
    }
};

static void dimm_register_devices(void)
{
    sysbus_register_withprop(&dimm_info);
    QLIST_INIT(&dimm_hp_result_queue);
}

device_init(dimm_register_devices)
