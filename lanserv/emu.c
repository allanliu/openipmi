/*
 * emu.c
 *
 * MontaVista IPMI code for emulating a BMC.
 *
 * Author: MontaVista Software, Inc.
 *         Corey Minyard <minyard@mvista.com>
 *         source@mvista.com
 *
 * Copyright 2003 MontaVista Software Inc.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2 of
 *  the License, or (at your option) any later version.
 *
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.  */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>

#include <OpenIPMI/ipmi_err.h>
#include <OpenIPMI/ipmi_msgbits.h>
#include <OpenIPMI/ipmi_bits.h>

#include "emu.h"

/* Deal with multi-byte data, IPMI (little-endian) style. */
static unsigned int ipmi_get_uint16(uint8_t *data)
{
    return (data[0]
	    | (data[1] << 8));
}

static void ipmi_set_uint16(uint8_t *data, int val)
{
    data[0] = val & 0xff;
    data[1] = (val >> 8) & 0xff;
}

static unsigned int ipmi_get_uint32(uint8_t *data)
{
    return (data[0]
	    | (data[1] << 8)
	    | (data[2] << 16)
	    | (data[3] << 24));
}

static void ipmi_set_uint32(uint8_t *data, int val)
{
    data[0] = val & 0xff;
    data[1] = (val >> 8) & 0xff;
    data[2] = (val >> 16) & 0xff;
    data[3] = (val >> 24) & 0xff;
}

typedef struct sel_entry_s
{
    uint16_t           record_id;
    unsigned char      data[16];
    struct sel_entry_s *next;
} sel_entry_t;

typedef struct sel_s
{
    sel_entry_t   *entries;
    int           count;
    int           max_count;
    uint32_t      last_add_time;
    uint32_t      last_erase_time;
    unsigned char flags;
    uint16_t      reservation;
    uint16_t      next_entry;
    long          time_offset;
} sel_t;

#define MAX_SDR_LENGTH 261
#define MAX_NUM_SDRS   1024
typedef struct sdr_s
{
    uint16_t      record_id;
    unsigned int  length;
    unsigned char *data;
    struct sdr_s  *next;
} sdr_t;

typedef struct sdrs_s
{
    uint16_t      reservation;
    uint16_t      sdr_count;
    uint16_t      sensor_count;
    uint32_t      last_add_time;
    uint32_t      last_erase_time;
    long          time_offset;
    unsigned char flags;
    uint16_t      next_entry;
    unsigned int  sdrs_length;

    /* A linked list of SDR entries. */
    sdr_t         *sdrs;
} sdrs_t;

typedef struct sensor_s
{
    unsigned char num;
    unsigned int  lun              : 2;
    unsigned int  scanning_enabled : 1;
    unsigned int  events_enabled   : 1;

    unsigned char sensor_type;
    unsigned char event_reading_code;

    unsigned char value;

    unsigned char hysteresis_support;
    unsigned char positive_hysteresis;
    unsigned char negative_hysteresis;

    unsigned char threshold_support;
    unsigned char threshold_supported[6];
    unsigned char thresholds[6];

    /* 1st array is 0 for assertion, 1 for deassertion. */
    unsigned char event_support;
    unsigned char event_supported[2][15];
    unsigned char event_enabled[2][15];

    unsigned char event_status[15];
} sensor_t;

typedef struct fru_data_s
{
    unsigned int  length;
    unsigned char *data;
} fru_data_t;

struct lmc_data_s
{
    emu_data_t *emu;

    unsigned char ipmb;

    /* Get Device Id contents. */
    unsigned char device_id;       /* byte 2 */
    unsigned char has_device_sdrs; /* byte 3, bit 7 */
    unsigned char device_revision; /* byte 3, bits 0-6 */
    unsigned char major_fw_rev;    /* byte 4, bits 0-6 */
    unsigned char minor_fw_rev;    /* byte 5 */
    unsigned char device_support;  /* byte 7 */
    unsigned char mfg_id[3];	   /* bytes 8-10 */
    unsigned char product_id[2];   /* bytes 11-12 */

    sel_t sel;

    sdrs_t main_sdrs;
    sdr_t  *part_add_sdr;
    int    part_add_next;
    int    in_update_mode;

    unsigned char event_receiver;
    unsigned char event_receiver_lun;

    sdrs_t device_sdrs[4];
    unsigned int dynamic_sensor_population : 1;
    unsigned int sensors_enabled : 1;
    unsigned char lun_has_sensors[4];
    unsigned char num_sensors_per_lun[4];
    sensor_t *(sensors[4][255]);
    uint32_t sensor_population_change_time;

    fru_data_t frus[255];

    unsigned char power_value;
};

struct emu_data_s
{
    int        bmc_mc;
    lmc_data_t *ipmb[128];
};

/* Device ID support bits */
#define IPMI_DEVID_CHASSIS_DEVICE	(1 << 7)
#define IPMI_DEVID_BRIDGE		(1 << 6)
#define IPMI_DEVID_IPMB_EVENT_GEN	(1 << 5)
#define IPMI_DEVID_IPMB_EVENT_RCV	(1 << 4)
#define IPMI_DEVID_FRU_INVENTORY_DEV	(1 << 3)
#define IPMI_DEVID_SEL_DEVICE		(1 << 2)
#define IPMI_DEVID_SDR_REPOSITORY_DEV	(1 << 1)
#define IPMI_DEVID_SENSOR_DEV		(1 << 0)

/*
 * SEL handling commands.
 */

#define IPMI_SEL_SUPPORTS_DELETE         (1 << 3)
#define IPMI_SEL_SUPPORTS_RESERVE        (1 << 1)
#define IPMI_SEL_SUPPORTS_GET_ALLOC_INFO (1 << 0)

static sel_entry_t *
find_sel_event_by_recid(lmc_data_t  *mc,
			uint16_t    record_id,
			sel_entry_t **prev)
{
    sel_entry_t *entry;
    sel_entry_t *p_entry = NULL;

    entry = mc->sel.entries;
    while (entry) {
	if (record_id == entry->record_id)
	    break;
	p_entry = entry;
	entry = entry->next;
    }
    if (prev)
	*prev = p_entry;
    return entry;
}

int
ipmi_mc_enable_sel(lmc_data_t    *mc,
		   int           max_entries,
		   unsigned char flags)
{
    mc->sel.entries = NULL;
    mc->sel.count = 0;
    mc->sel.max_count = max_entries;
    mc->sel.last_add_time = 0;
    mc->sel.last_erase_time = 0;
    mc->sel.flags = flags & 0xb;
    mc->sel.reservation = 0;
    mc->sel.next_entry = 1;
    return 0;
}
		    

int
ipmi_mc_add_to_sel(lmc_data_t    *mc,
		   unsigned char record_type,
		   unsigned char event[13])
{
    sel_entry_t    *e;
    struct timeval t;
    uint16_t       start_record_id;

    if (!(mc->device_support & IPMI_DEVID_SEL_DEVICE))
	return ENOTSUP;

    if (mc->sel.count >= mc->sel.max_count)
	return EAGAIN;

    e = malloc(sizeof(*e));
    if (!e)
	return ENOMEM;

    /* FIXME - this is inefficient, but simple */
    e->record_id = mc->sel.next_entry;
    mc->sel.next_entry++;
    start_record_id = e->record_id;
    while ((mc->sel.next_entry == 0)
	   || find_sel_event_by_recid(mc, e->record_id, NULL))
    {
	e->record_id++;
	if (e->record_id == start_record_id)
	    return EAGAIN;
	mc->sel.next_entry++;
    }

    ipmi_set_uint16(e->data, e->record_id);
    e->data[2] = record_type;
    if (record_type < 0xe0) {
	ipmi_set_uint32(e->data+3, t.tv_sec);
	memcpy(e->data+7, event+4, 9);
    } else {
	memcpy(e->data+3, event, 13);
    }

    e->next = NULL;
    if (!mc->sel.entries) {
	mc->sel.entries = e;
    } else {
	sel_entry_t *f = mc->sel.entries;
	while (f->next)
	    f = f->next;
	f->next = e;
    }

    mc->sel.count++;

    gettimeofday(&t, NULL);
    mc->sel.last_add_time = t.tv_sec + mc->sel.time_offset;
    return 0;
}


static void
handle_invalid_cmd(lmc_data_t    *mc,
		   unsigned char *rdata,
		   unsigned int  *rdata_len)
{
    rdata[0] = IPMI_INVALID_CMD_CC;
    *rdata_len = 1;
}

static int
check_msg_length(ipmi_msg_t    *msg,
		 unsigned int  len,
		 unsigned char *rdata,
		 unsigned int  *rdata_len)
{
    if (msg->data_len < len) {
	rdata[0] = IPMI_REQUEST_DATA_LENGTH_INVALID_CC;
	*rdata_len = 1;
	return 1;
    }

    return 0;
}

static void
handle_get_sel_info(lmc_data_t    *mc,
		    ipmi_msg_t    *msg,
		    unsigned char *rdata,
		    unsigned int  *rdata_len)
{
    if (!(mc->device_support & IPMI_DEVID_SEL_DEVICE)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    memset(rdata, 0, 15);
    rdata[1] = 0x51;
    ipmi_set_uint16(rdata+2, mc->sel.count);
    ipmi_set_uint16(rdata+4, (mc->sel.max_count - mc->sel.count) * 16);
    ipmi_set_uint32(rdata+6, mc->sel.last_add_time);
    ipmi_set_uint32(rdata+10, mc->sel.last_erase_time);
    rdata[14] = mc->sel.flags;

    /* Clear the overflow flag. */
    /* FIXME - is this the right way to clear this?  There doesn't
       seem to be another way. */
    mc->sel.flags &= ~0x80;

    *rdata_len = 15;
}

static void
handle_get_sel_allocation_info(lmc_data_t    *mc,
			       ipmi_msg_t    *msg,
			       unsigned char *rdata,
			       unsigned int  *rdata_len)
{
    if (!(mc->device_support & IPMI_DEVID_SEL_DEVICE)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (!(mc->sel.flags & IPMI_SEL_SUPPORTS_GET_ALLOC_INFO)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    memset(rdata, 0, 10);
    ipmi_set_uint16(rdata+1, mc->sel.max_count * 16);
    ipmi_set_uint16(rdata+3, 16);
    ipmi_set_uint32(rdata+5, (mc->sel.max_count - mc->sel.count) * 16);
    ipmi_set_uint32(rdata+7, (mc->sel.max_count - mc->sel.count) * 16);
    rdata[9] = 1;

    *rdata_len = 10;
}

static void
handle_reserve_sel(lmc_data_t    *mc,
		   ipmi_msg_t    *msg,
		   unsigned char *rdata,
		   unsigned int  *rdata_len)
{
    if (!(mc->device_support & IPMI_DEVID_SEL_DEVICE)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (!(mc->sel.flags & IPMI_SEL_SUPPORTS_RESERVE)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    mc->sel.reservation++;
    if (mc->sel.reservation == 0)
	mc->sel.reservation++;
    rdata[0] = 0;
    ipmi_set_uint16(rdata+1, mc->sel.reservation);
    *rdata_len = 3;
}

static void
handle_get_sel_entry(lmc_data_t    *mc,
		     ipmi_msg_t    *msg,
		     unsigned char *rdata,
		     unsigned int  *rdata_len)
{
    uint16_t    record_id;
    int         offset;
    int         count;
    sel_entry_t *entry;

    if (!(mc->device_support & IPMI_DEVID_SEL_DEVICE)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (check_msg_length(msg, 6, rdata, rdata_len))
	return;

    if (mc->sel.flags & IPMI_SEL_SUPPORTS_RESERVE) {
	uint16_t reservation = ipmi_get_uint16(msg->data+0);

	if ((reservation != 0) && (reservation != mc->sel.reservation)) {
	    rdata[0] = IPMI_INVALID_RESERVATION_CC;
	    *rdata_len = 1;
	    return;
	}
    }

    record_id = ipmi_get_uint16(msg->data+2);
    offset = msg->data[4];
    count = msg->data[5];

    if (offset >= 16) {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    if (record_id == 0) {
	entry = mc->sel.entries;
    } else if (record_id == 0xffff) {
	entry = mc->sel.entries;
	if (entry) {
	    while (entry->next) {
		entry = entry->next;
	    }
	}
    } else {
	entry = find_sel_event_by_recid(mc, record_id, NULL);
    }

    if (entry == NULL) {
	rdata[0] = IPMI_NOT_PRESENT_CC;
	*rdata_len = 1;
	return;
    }

    rdata[0] = 0;
    if (entry->next)
	ipmi_set_uint16(rdata+1, entry->next->record_id);
    else {
	rdata[1] = 0xff;
	rdata[2] = 0xff;
    }

    if ((offset+count) > 16)
	count = 16 - offset;
    memcpy(rdata+3, entry->data+offset, count);
    *rdata_len = count + 3;
}

static void
handle_add_sel_entry(lmc_data_t    *mc,
		     ipmi_msg_t    *msg,
		     unsigned char *rdata,
		     unsigned int  *rdata_len)
{
    int rv;

    if (!(mc->device_support & IPMI_DEVID_SEL_DEVICE)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (check_msg_length(msg, 16, rdata, rdata_len))
	return;

    if (msg->data[2] < 0xe0)
	rv = ipmi_mc_add_to_sel(mc, msg->data[2], msg->data+6);
    else
	rv = ipmi_mc_add_to_sel(mc, msg->data[2], msg->data+3);

    if (rv == EAGAIN) {
	rdata[0] = IPMI_OUT_OF_SPACE_CC;
    } else if (rv) {
	rdata[0] = IPMI_UNKNOWN_ERR_CC;
    } else {
	rdata[0] = 0;
    }
    *rdata_len = 1;
}

static void
handle_delete_sel_entry(lmc_data_t    *mc,
			ipmi_msg_t    *msg,
			unsigned char *rdata,
			unsigned int  *rdata_len)
{
    uint16_t    record_id;
    sel_entry_t *entry, *p_entry;

    if (!(mc->device_support & IPMI_DEVID_SEL_DEVICE)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (!(mc->sel.flags & IPMI_SEL_SUPPORTS_DELETE)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (check_msg_length(msg, 4, rdata, rdata_len))
	return;

    if (mc->sel.flags & IPMI_SEL_SUPPORTS_RESERVE) {
	uint16_t reservation = ipmi_get_uint16(msg->data+0);

	if ((reservation != 0) && (reservation != mc->sel.reservation)) {
	    rdata[0] = IPMI_INVALID_RESERVATION_CC;
	    *rdata_len = 1;
	    return;
	}
    }

    record_id = ipmi_get_uint16(msg->data+2);

    if (record_id == 0) {
	entry = mc->sel.entries;
	p_entry = NULL;
    } else if (record_id == 0xffff) {
	entry = mc->sel.entries;
	p_entry = NULL;
	if (entry) {
	    while (entry->next) {
		p_entry = entry;
		entry = entry->next;
	    }
	}
    } else {
	entry = find_sel_event_by_recid(mc, record_id, &p_entry);
    }
    if (!entry) {
	rdata[0] = IPMI_NOT_PRESENT_CC;
	*rdata_len = 1;
	return;
    }

    if (p_entry)
	p_entry->next = entry->next;
    else
	mc->sel.entries = entry->next;

    rdata[0] = 0;
    ipmi_set_uint16(rdata+1, entry->record_id);
    *rdata_len = 3;

    free(entry);
}

static void
handle_clear_sel(lmc_data_t    *mc,
		 ipmi_msg_t    *msg,
		 unsigned char *rdata,
		 unsigned int  *rdata_len)
{
    sel_entry_t    *entry, *n_entry;
    unsigned char  op;
    struct timeval t;

    if (!(mc->device_support & IPMI_DEVID_SEL_DEVICE)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (check_msg_length(msg, 6, rdata, rdata_len))
	return;

    if (mc->sel.flags & IPMI_SEL_SUPPORTS_RESERVE) {
	uint16_t reservation = ipmi_get_uint16(msg->data+0);

	if ((reservation != 0) && (reservation != mc->sel.reservation)) {
	    rdata[0] = IPMI_INVALID_RESERVATION_CC;
	    *rdata_len = 1;
	    return;
	}
    }

    if ((msg->data[2] != 'C')
	|| (msg->data[3] != 'L')
	|| (msg->data[4] != 'R'))
    {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    op = msg->data[5];
    if ((op != 0) && (op != 0xaa))
    {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    rdata[1] = 1;
    if (op == 0) {
	entry = mc->sel.entries;
	while (entry) {
	    n_entry = entry->next;
	    free(entry);
	    entry = n_entry;
	}
    }

    gettimeofday(&t, NULL);
    mc->sel.last_erase_time = t.tv_sec + mc->sel.time_offset;

    rdata[0] = 0;
    *rdata_len = 2;
}

static void
handle_get_sel_time(lmc_data_t    *mc,
		    ipmi_msg_t    *msg,
		    unsigned char *rdata,
		    unsigned int  *rdata_len)
{
    struct timeval t;

    if (!(mc->device_support & IPMI_DEVID_SEL_DEVICE)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    gettimeofday(&t, NULL);
    rdata[0] = 0;
    ipmi_set_uint32(rdata+1, t.tv_sec + mc->sel.time_offset);
    *rdata_len = 5;
}

static void
handle_set_sel_time(lmc_data_t    *mc,
		    ipmi_msg_t    *msg,
		    unsigned char *rdata,
		    unsigned int  *rdata_len)
{
    struct timeval t;

    if (!(mc->device_support & IPMI_DEVID_SEL_DEVICE)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (check_msg_length(msg, 4, rdata, rdata_len))
	return;

    gettimeofday(&t, NULL);
    mc->sel.time_offset = ipmi_get_uint32(msg->data) - t.tv_sec;

    rdata[0] = 0;
    *rdata_len = 1;
}

/*
 * SDR handling commands
 */

#define IPMI_SDR_OVERFLOW_FLAG				(1 << 7)
#define IPMI_SDR_GET_MODAL(v)   (((v) >> 5) & 0x3)
#define IPMI_SDR_MODAL_UNSPECIFIED	0
#define IPMI_SDR_NON_MODAL_ONLY		1
#define IPMI_SDR_MODAL_ONLY		2
#define IPMI_SDR_MODAL_BOTH		3
#define IPMI_SDR_DELETE_SDR_SUPPORTED			(1 << 3)
#define IPMI_SDR_PARTIAL_ADD_SDR_SUPPORTED		(1 << 2)
#define IPMI_SDR_RESERVE_SDR_SUPPORTED			(1 << 1)
#define IPMI_SDR_GET_SDR_ALLOC_INFO_SDR_SUPPORTED	(1 << 0)

static sdr_t *
find_sdr_by_recid(lmc_data_t *mc,
		  sdrs_t     *sdrs,
		  uint16_t   record_id,
		  sdr_t      **prev)
{
    sdr_t *entry;
    sdr_t *p_entry = NULL;

    entry = sdrs->sdrs;
    while (entry) {
	if (record_id == entry->record_id)
	    break;
	p_entry = entry;
	entry = entry->next;
    }
    if (prev)
	*prev = p_entry;
    return entry;
}

static sdr_t *
new_sdr_entry(lmc_data_t *mc, sdrs_t *sdrs, unsigned char length)
{
    sdr_t    *entry;
    uint16_t start_recid;

    start_recid = sdrs->next_entry;
    while (find_sdr_by_recid(mc, sdrs, sdrs->next_entry, NULL)) {
	sdrs->next_entry++;
	if (sdrs->next_entry == 0xffff)
	    sdrs->next_entry = 1;
	if (sdrs->next_entry == start_recid)
	    return NULL;
    }

    entry = malloc(sizeof(*entry));
    if (!entry)
	return NULL;

    entry->data = malloc(length + 6);
    if (!entry->data)
	return NULL;

    entry->record_id = sdrs->next_entry;

    sdrs->next_entry++;

    ipmi_set_uint16(entry->data, entry->record_id);

    entry->length = length + 6;
    entry->next = NULL;
    return entry;
}

static void
add_sdr_entry(lmc_data_t *mc, sdrs_t *sdrs, sdr_t *entry)
{
    sdr_t          *p;
    struct timeval t;

    entry->next = NULL;
    p = sdrs->sdrs;
    if (!p)
	sdrs->sdrs = entry;
    else {
	while (p->next)
	    p = p->next;
	p->next = entry;
    }

    gettimeofday(&t, NULL);
    sdrs->last_add_time = t.tv_sec + mc->main_sdrs.time_offset;
    sdrs->sdr_count++;
}

static void
free_sdr(sdr_t *sdr)
{
    free(sdr->data);
    free(sdr);
}

int
ipmi_mc_add_main_sdr(lmc_data_t    *mc,
		     unsigned char *data,
		     unsigned int  data_len)
{
    sdr_t          *entry;

    if (!(mc->device_support & IPMI_DEVID_SDR_REPOSITORY_DEV))
	return ENOSYS;

    entry = new_sdr_entry(mc, &mc->main_sdrs, data_len);
    if (!entry)
	return ENOMEM;

    add_sdr_entry(mc, &mc->main_sdrs, entry);

    memcpy(entry->data+2, data+2, data_len-2);
    return 0;
}

int
ipmi_mc_add_device_sdr(lmc_data_t    *mc,
		       unsigned char lun,
		       unsigned char *data,
		       unsigned int  data_len)
{
    struct timeval t;
    sdr_t          *entry;

    if (lun >= 4)
	return EINVAL;

    if (!(mc->has_device_sdrs)) {
	return ENOSYS;
    }

    entry = new_sdr_entry(mc, &mc->device_sdrs[lun], data_len);
    if (!entry)
	return ENOMEM;

    add_sdr_entry(mc, &mc->device_sdrs[lun], entry);

    memcpy(entry->data+2, data+2, data_len-2);

    gettimeofday(&t, NULL);
    mc->sensor_population_change_time = t.tv_sec + mc->main_sdrs.time_offset;
    mc->lun_has_sensors[lun] = 1;
    mc->num_sensors_per_lun[lun]++;
    return 0;
}

static void
handle_get_sdr_repository_info(lmc_data_t    *mc,
			       ipmi_msg_t    *msg,
			       unsigned char *rdata,
			       unsigned int  *rdata_len)
{
    unsigned int space;

    if (!(mc->device_support & IPMI_DEVID_SDR_REPOSITORY_DEV)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    rdata[0] = 0;
    rdata[1] = 0x51;
    ipmi_set_uint16(rdata+2, mc->main_sdrs.sdr_count);
    space = MAX_SDR_LENGTH * (MAX_NUM_SDRS - mc->main_sdrs.sdr_count);
    if (space > 0xfffe)
	space = 0xfffe;
    ipmi_set_uint16(rdata+4, space);
    ipmi_set_uint32(rdata+6, mc->main_sdrs.last_add_time);
    ipmi_set_uint32(rdata+10, mc->main_sdrs.last_erase_time);
    rdata[14] = mc->main_sdrs.flags;
    *rdata_len = 15;
}

static void
handle_get_sdr_repository_alloc_info(lmc_data_t    *mc,
				     ipmi_msg_t    *msg,
				     unsigned char *rdata,
				     unsigned int  *rdata_len)
{
    if (!(mc->device_support & IPMI_DEVID_SDR_REPOSITORY_DEV)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (!(mc->main_sdrs.flags & IPMI_SDR_GET_SDR_ALLOC_INFO_SDR_SUPPORTED)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    rdata[0] = 0;
    ipmi_set_uint16(rdata+1, MAX_NUM_SDRS);
    ipmi_set_uint16(rdata+3, MAX_SDR_LENGTH);
    ipmi_set_uint16(rdata+5, MAX_NUM_SDRS - mc->main_sdrs.sdr_count);
    ipmi_set_uint16(rdata+7, MAX_NUM_SDRS - mc->main_sdrs.sdr_count);
    rdata[9] = 1;
    *rdata_len = 10;
}

static void
handle_reserve_sdr_repository(lmc_data_t    *mc,
			      ipmi_msg_t    *msg,
			      unsigned char *rdata,
			      unsigned int  *rdata_len)
{
    if (!(mc->device_support & IPMI_DEVID_SDR_REPOSITORY_DEV)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (!(mc->main_sdrs.flags & IPMI_SDR_RESERVE_SDR_SUPPORTED)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    mc->main_sdrs.reservation++;
    if (mc->main_sdrs.reservation == 0)
	mc->main_sdrs.reservation++;

    rdata[0] = 0;
    ipmi_set_uint16(rdata+1, mc->main_sdrs.reservation);
    *rdata_len = 3;

    /* If adding an SDR and the reservation changes, we have to
       destroy the working SDR addition. */
    if (mc->part_add_sdr) {
	free_sdr(mc->part_add_sdr);
	mc->part_add_sdr = NULL;
    }
}

static void
handle_get_sdr(lmc_data_t    *mc,
	       ipmi_msg_t    *msg,
	       unsigned char *rdata,
	       unsigned int  *rdata_len)
{
    uint16_t record_id;
    int      offset;
    int      count;
    sdr_t    *entry;

    if (!(mc->device_support & IPMI_DEVID_SDR_REPOSITORY_DEV)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (check_msg_length(msg, 6, rdata, rdata_len))
	return;

    if (mc->main_sdrs.flags & IPMI_SDR_RESERVE_SDR_SUPPORTED) {
	uint16_t reservation = ipmi_get_uint16(msg->data+0);

	if ((reservation != 0) && (reservation != mc->main_sdrs.reservation)) {
	    rdata[0] = IPMI_INVALID_RESERVATION_CC;
	    *rdata_len = 1;
	    return;
	}
    }

    record_id = ipmi_get_uint16(msg->data+2);
    offset = msg->data[4];
    count = msg->data[5];

    if (record_id == 0) {
	entry = mc->main_sdrs.sdrs;
    } else if (record_id == 0xffff) {
	entry = mc->main_sdrs.sdrs;
	if (entry) {
	    while (entry->next) {
		entry = entry->next;
	    }
	}
    } else {
	entry = find_sdr_by_recid(mc, &mc->main_sdrs, record_id, NULL);
    }

    if (entry == NULL) {
	rdata[0] = IPMI_NOT_PRESENT_CC;
	*rdata_len = 1;
	return;
    }

    if (offset >= entry->length) {
	rdata[0] = IPMI_PARAMETER_OUT_OF_RANGE_CC;
	*rdata_len = 1;
	return;
    }

    if ((offset+count) > entry->length)
	count = entry->length - offset;
    if (count+3 > *rdata_len) {
	/* Too much data to put into response. */
	rdata[0] = IPMI_REQUESTED_DATA_LENGTH_EXCEEDED_CC;
	*rdata_len = 1;
	return;
    }

    rdata[0] = 0;
    if (entry->next)
	ipmi_set_uint16(rdata+1, entry->next->record_id);
    else {
	rdata[1] = 0xff;
	rdata[2] = 0xff;
    }

    memcpy(rdata+3, entry->data+offset, count);
    *rdata_len = count + 3;
}

static void
handle_add_sdr(lmc_data_t    *mc,
	       ipmi_msg_t    *msg,
	       unsigned char *rdata,
	       unsigned int  *rdata_len)
{
    int            modal;
    sdr_t          *entry;

    if (!(mc->device_support & IPMI_DEVID_SDR_REPOSITORY_DEV)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    modal = IPMI_SDR_GET_MODAL(mc->main_sdrs.flags);
    if ((modal == IPMI_SDR_NON_MODAL_ONLY)
	&& !mc->in_update_mode)
    {
	rdata[0] = IPMI_NOT_SUPPORTED_IN_PRESENT_STATE_CC;
	*rdata_len = 1;
	return;
    }

    if (check_msg_length(msg, 6, rdata, rdata_len))
	return;

    if (msg->data_len != msg->data[5] + 6) {
	rdata[0] = 0x80; /* Length is invalid. */
	*rdata_len = 1;
	return;
    }

    entry = new_sdr_entry(mc, &mc->main_sdrs, msg->data[5]);
    if (!entry) {
	rdata[0] = IPMI_OUT_OF_SPACE_CC;
	*rdata_len = 1;
	return;
    }
    add_sdr_entry(mc, &mc->main_sdrs, entry);

    memcpy(entry->data+2, msg->data+2, entry->length-2);

    rdata[0] = 0;
    ipmi_set_uint16(rdata+1, entry->record_id);
    *rdata_len = 3;
}

static void
handle_partial_add_sdr(lmc_data_t    *mc,
		       ipmi_msg_t    *msg,
		       unsigned char *rdata,
		       unsigned int  *rdata_len)
{
    uint16_t record_id;
    int      offset;
    int      modal;

    if (!(mc->device_support & IPMI_DEVID_SDR_REPOSITORY_DEV)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (!(mc->main_sdrs.flags & IPMI_SDR_PARTIAL_ADD_SDR_SUPPORTED)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (mc->main_sdrs.flags & IPMI_SDR_RESERVE_SDR_SUPPORTED) {
	uint16_t reservation = ipmi_get_uint16(msg->data+0);

	if ((reservation != 0) && (reservation != mc->main_sdrs.reservation)) {
	    rdata[0] = IPMI_INVALID_RESERVATION_CC;
	    *rdata_len = 1;
	    return;
	}
    }

    modal = IPMI_SDR_GET_MODAL(mc->main_sdrs.flags);
    if ((modal == IPMI_SDR_NON_MODAL_ONLY)
	&& !mc->in_update_mode)
    {
	rdata[0] = IPMI_NOT_SUPPORTED_IN_PRESENT_STATE_CC;
	*rdata_len = 1;
	return;
    }

    offset = msg->data[4];
    record_id = ipmi_get_uint16(rdata+2);
    if (record_id == 0) {
	/* New add. */
	if (check_msg_length(msg, 12, rdata, rdata_len))
	    return;
	if (offset != 0) {
	    rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	    *rdata_len = 1;
	    return;
	}
	if (msg->data_len > msg->data[11] + 12) {
	    rdata[0] = 0x80; /* Invalid data length */
	    *rdata_len = 1;
	    return;
	}
	if (mc->part_add_sdr) {
	    /* Still working on a previous one, return an error and
	       abort. */
	    free_sdr(mc->part_add_sdr);
	    mc->part_add_sdr = NULL;
	    rdata[0] = IPMI_UNKNOWN_ERR_CC;
	    *rdata_len = 1;
	    return;
	}
	mc->part_add_sdr = new_sdr_entry(mc, &mc->main_sdrs, msg->data[11]);
	memcpy(mc->part_add_sdr->data+2, msg->data+8, msg->data_len - 8);
	mc->part_add_next = msg->data_len - 8;
    } else {
	if (!mc->part_add_next) {
	    rdata[0] = IPMI_UNKNOWN_ERR_CC;
	    *rdata_len = 1;
	    return;
	}
	if (offset != mc->part_add_next) {
	    free_sdr(mc->part_add_sdr);
	    mc->part_add_sdr = NULL;
	    rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	    *rdata_len = 1;
	    return;
	}
	if ((offset + msg->data_len - 6) > mc->part_add_sdr->length) {
	    free_sdr(mc->part_add_sdr);
	    mc->part_add_sdr = NULL;
	    rdata[0] = 0x80; /* Invalid data length */
	    *rdata_len = 1;
	    return;
	}
	memcpy(mc->part_add_sdr->data+offset, msg->data+6, msg->data_len-6);
	mc->part_add_next += msg->data_len - 6;
    }

    if ((msg->data[5] & 0xf) == 1) {
	/* End of the operation. */
	if (mc->part_add_next != mc->part_add_sdr->length) {
	    free_sdr(mc->part_add_sdr);
	    mc->part_add_sdr = NULL;
	    rdata[0] = 0x80; /* Invalid data length */
	    *rdata_len = 1;
	    return;
	}
	add_sdr_entry(mc, &mc->main_sdrs, mc->part_add_sdr);
	mc->part_add_sdr = NULL;
    }

    rdata[0] = 0;
    *rdata_len = 1;
}

static void
handle_delete_sdr(lmc_data_t    *mc,
		  ipmi_msg_t    *msg,
		  unsigned char *rdata,
		  unsigned int  *rdata_len)
{
    uint16_t       record_id;
    sdr_t          *entry, *p_entry;
    struct timeval t;

    if (!(mc->device_support & IPMI_DEVID_SDR_REPOSITORY_DEV)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (check_msg_length(msg, 4, rdata, rdata_len))
	return;

    if (mc->main_sdrs.flags & IPMI_SDR_RESERVE_SDR_SUPPORTED) {
	uint16_t reservation = ipmi_get_uint16(msg->data+0);

	if ((reservation != 0) && (reservation != mc->main_sdrs.reservation)) {
	    rdata[0] = IPMI_INVALID_RESERVATION_CC;
	    *rdata_len = 1;
	    return;
	}
    }

    record_id = ipmi_get_uint16(rdata+2);

    if (record_id == 0) {
	entry = mc->main_sdrs.sdrs;
	p_entry = NULL;
    } else if (record_id == 0xffff) {
	entry = mc->main_sdrs.sdrs;
	p_entry = NULL;
	if (entry) {
	    while (entry->next) {
		p_entry = entry;
		entry = entry->next;
	    }
	}
    } else {
	entry = find_sdr_by_recid(mc, &mc->main_sdrs, record_id, &p_entry);
    }
    if (!entry) {
	rdata[0] = IPMI_NOT_PRESENT_CC;
	*rdata_len = 1;
	return;
    }

    if (p_entry)
	p_entry->next = entry->next;
    else
	mc->main_sdrs.sdrs = entry->next;

    rdata[0] = 0;
    ipmi_set_uint16(rdata+1, entry->record_id);
    *rdata_len = 3;

    free_sdr(entry);

    gettimeofday(&t, NULL);
    mc->main_sdrs.last_erase_time = t.tv_sec + mc->main_sdrs.time_offset;
    mc->main_sdrs.sdr_count--;
}

static void
handle_clear_sdr_repository(lmc_data_t    *mc,
			    ipmi_msg_t    *msg,
			    unsigned char *rdata,
			    unsigned int  *rdata_len)
{
    sdr_t          *entry, *n_entry;
    struct timeval t;
    unsigned char  op;

    if (!(mc->device_support & IPMI_DEVID_SDR_REPOSITORY_DEV)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (check_msg_length(msg, 6, rdata, rdata_len))
	return;

    if (mc->main_sdrs.flags & IPMI_SDR_RESERVE_SDR_SUPPORTED) {
	uint16_t reservation = ipmi_get_uint16(msg->data+0);

	if ((reservation != 0) && (reservation != mc->main_sdrs.reservation)) {
	    rdata[0] = IPMI_INVALID_RESERVATION_CC;
	    *rdata_len = 1;
	    return;
	}
    }

    if ((msg->data[2] != 'C')
	|| (msg->data[3] != 'L')
	|| (msg->data[4] != 'R'))
    {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    op = msg->data[5];
    if ((op != 0) && (op != 0xaa))
    {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    rdata[1] = 1;
    if (op == 0) {
	entry = mc->main_sdrs.sdrs;
	while (entry) {
	    n_entry = entry->next;
	    free_sdr(entry);
	    entry = n_entry;
	}
    }

    rdata[0] = 0;
    *rdata_len = 2;

    gettimeofday(&t, NULL);
    mc->main_sdrs.last_erase_time = t.tv_sec + mc->main_sdrs.time_offset;
}

static void
handle_get_sdr_repository_time(lmc_data_t    *mc,
			       ipmi_msg_t    *msg,
			       unsigned char *rdata,
			       unsigned int  *rdata_len)
{
    struct timeval t;

    if (!(mc->device_support & IPMI_DEVID_SDR_REPOSITORY_DEV)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    gettimeofday(&t, NULL);
    rdata[0] = 0;
    ipmi_set_uint32(rdata+1, t.tv_sec + mc->main_sdrs.time_offset);
    *rdata_len = 5;
}

static void
handle_set_sdr_repository_time(lmc_data_t    *mc,
			       ipmi_msg_t    *msg,
			       unsigned char *rdata,
			       unsigned int  *rdata_len)
{
    struct timeval t;

    if (!(mc->device_support & IPMI_DEVID_SDR_REPOSITORY_DEV)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (check_msg_length(msg, 4, rdata, rdata_len))
	return;

    gettimeofday(&t, NULL);
    mc->main_sdrs.time_offset = ipmi_get_uint32(msg->data) - t.tv_sec;

    rdata[0] = 0;
    *rdata_len = 1;
}

static void
handle_enter_sdr_repository_update(lmc_data_t    *mc,
				   ipmi_msg_t    *msg,
				   unsigned char *rdata,
				   unsigned int  *rdata_len)
{
    int modal;

    if (!(mc->device_support & IPMI_DEVID_SDR_REPOSITORY_DEV)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    modal = IPMI_SDR_GET_MODAL(mc->main_sdrs.flags);
    if ((modal == IPMI_SDR_MODAL_UNSPECIFIED)
	|| (modal == IPMI_SDR_NON_MODAL_ONLY))
    {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    mc->in_update_mode = 1;

    rdata[0] = 0;
    *rdata_len = 1;
}

static void
handle_exit_sdr_repository_update(lmc_data_t    *mc,
				  ipmi_msg_t    *msg,
				  unsigned char *rdata,
				  unsigned int  *rdata_len)
{
    int modal;

    if (!(mc->device_support & IPMI_DEVID_SDR_REPOSITORY_DEV)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    modal = IPMI_SDR_GET_MODAL(mc->main_sdrs.flags);
    if ((modal == IPMI_SDR_MODAL_UNSPECIFIED)
	|| (modal == IPMI_SDR_NON_MODAL_ONLY))
    {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    mc->in_update_mode = 0;

    rdata[0] = 0;
    *rdata_len = 1;
}

static void
handle_get_fru_inventory_area_info(lmc_data_t    *mc,
				   ipmi_msg_t    *msg,
				   unsigned char *rdata,
				   unsigned int  *rdata_len)
{
    unsigned char devid;

    if (check_msg_length(msg, 1, rdata, rdata_len))
	return;

    devid = msg->data[0];
    if ((devid >= 255) || (!mc->frus[devid].data)) {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    rdata[0] = 0;
    ipmi_set_uint16(rdata+1, mc->frus[devid].length);
    rdata[3] = 0; /* We only support byte access for now. */
    *rdata_len = 4;
}

static void
handle_read_fru_data(lmc_data_t    *mc,
		     ipmi_msg_t    *msg,
		     unsigned char *rdata,
		     unsigned int  *rdata_len)
{
    unsigned char devid;
    int           offset;
    int           count;

    if (check_msg_length(msg, 4, rdata, rdata_len))
	return;

    devid = msg->data[0];
    if ((devid >= 255) || (!mc->frus[devid].data)) {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    offset = ipmi_get_uint16(msg->data+1);
    count = msg->data[3];

    if (offset >= mc->frus[devid].length) {
	rdata[0] = IPMI_PARAMETER_OUT_OF_RANGE_CC;
	*rdata_len = 1;
	return;
    }

    if ((offset+count) > mc->frus[devid].length)
	count = mc->frus[devid].length - offset;
    if (count+2 > *rdata_len) {
	/* Too much data to put into response. */
	rdata[0] = IPMI_REQUESTED_DATA_LENGTH_EXCEEDED_CC;
	*rdata_len = 1;
	return;
    }

    rdata[0] = 0;
    rdata[1] = count;
    memcpy(rdata+2, mc->frus[devid].data+offset, count);
    *rdata_len = 2+count;
}

static void
handle_write_fru_data(lmc_data_t    *mc,
		      ipmi_msg_t    *msg,
		      unsigned char *rdata,
		      unsigned int  *rdata_len)
{
    unsigned char devid;
    int           offset;
    int           count;

    if (check_msg_length(msg, 3, rdata, rdata_len))
	return;

    devid = msg->data[0];
    if ((devid >= 255) || (!mc->frus[devid].data)) {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    offset = ipmi_get_uint16(msg->data+1);
    count = msg->data_len - 3;

    if (offset >= mc->frus[devid].length) {
	rdata[0] = IPMI_PARAMETER_OUT_OF_RANGE_CC;
	*rdata_len = 1;
	return;
    }

    if ((offset+count) > mc->frus[devid].length) {
	/* Too much data to put into FRU. */
	rdata[0] = IPMI_REQUESTED_DATA_LENGTH_EXCEEDED_CC;
	*rdata_len = 1;
	return;
    }

    memcpy(mc->frus[devid].data+offset, msg->data+3, count);
    rdata[0] = 0;
    rdata[1] = count;
    *rdata_len = 2;
}

int
ipmi_mc_add_fru_data(lmc_data_t    *mc,
		     unsigned char device_id,
		     unsigned int  length,
		     unsigned char *data,
		     unsigned int  data_len)
{
    if (!(mc->device_support & IPMI_DEVID_FRU_INVENTORY_DEV))
	return ENOSYS;

    if (device_id >= 255)
	return EINVAL;

    if (data_len > length)
	return EINVAL;

    if (mc->frus[device_id].data) {
	free(mc->frus[device_id].data);
	mc->frus[device_id].length = 0;
    }

    mc->frus[device_id].data = malloc(length);
    if (!mc->frus[device_id].data)
	return ENOMEM;
    mc->frus[device_id].length = length;
    memset(mc->frus[device_id].data, 0, length);
    memcpy(mc->frus[device_id].data, data, data_len);
    return 0;
}

static void
handle_storage_netfn(lmc_data_t    *mc,
		     unsigned char lun,
		     ipmi_msg_t    *msg,
		     unsigned char *rdata,
		     unsigned int  *rdata_len)
{
    switch(msg->cmd) {
    case IPMI_GET_SEL_INFO_CMD:
	handle_get_sel_info(mc, msg, rdata, rdata_len);
	break;

    case IPMI_GET_SEL_ALLOCATION_INFO_CMD:
	handle_get_sel_allocation_info(mc, msg, rdata, rdata_len);
	break;

    case IPMI_RESERVE_SEL_CMD:
	handle_reserve_sel(mc, msg, rdata, rdata_len);
	break;

    case IPMI_GET_SEL_ENTRY_CMD:
	handle_get_sel_entry(mc, msg, rdata, rdata_len);
	break;

    case IPMI_ADD_SEL_ENTRY_CMD:
	handle_add_sel_entry(mc, msg, rdata, rdata_len);
	break;

    case IPMI_DELETE_SEL_ENTRY_CMD:
	handle_delete_sel_entry(mc, msg, rdata, rdata_len);
	break;

    case IPMI_CLEAR_SEL_CMD:
	handle_clear_sel(mc, msg, rdata, rdata_len);
	break;

    case IPMI_GET_SEL_TIME_CMD:
	handle_get_sel_time(mc, msg, rdata, rdata_len);
	break;

    case IPMI_SET_SEL_TIME_CMD:
	handle_set_sel_time(mc, msg, rdata, rdata_len);
	break;

    /* We don't currently care about partial sel adds, since they are
       pretty stupid. */

    case IPMI_GET_SDR_REPOSITORY_INFO_CMD:
	handle_get_sdr_repository_info(mc, msg, rdata, rdata_len);
	break;

    case IPMI_GET_SDR_REPOSITORY_ALLOC_INFO_CMD:
	handle_get_sdr_repository_alloc_info(mc, msg, rdata, rdata_len);
	break;

    case IPMI_RESERVE_SDR_REPOSITORY_CMD:
	handle_reserve_sdr_repository(mc, msg, rdata, rdata_len);
	break;

    case IPMI_GET_SDR_CMD:
	handle_get_sdr(mc, msg, rdata, rdata_len);
	break;

    case IPMI_ADD_SDR_CMD:
	handle_add_sdr(mc, msg, rdata, rdata_len);
	break;

    case IPMI_PARTIAL_ADD_SDR_CMD:
	handle_partial_add_sdr(mc, msg, rdata, rdata_len);
	break;

    case IPMI_DELETE_SDR_CMD:
	handle_delete_sdr(mc, msg, rdata, rdata_len);
	break;

    case IPMI_CLEAR_SDR_REPOSITORY_CMD:
	handle_clear_sdr_repository(mc, msg, rdata, rdata_len);
	break;

    case IPMI_GET_SDR_REPOSITORY_TIME_CMD:
	handle_get_sdr_repository_time(mc, msg, rdata, rdata_len);
	break;

    case IPMI_SET_SDR_REPOSITORY_TIME_CMD:
	handle_set_sdr_repository_time(mc, msg, rdata, rdata_len);
	break;

    case IPMI_ENTER_SDR_REPOSITORY_UPDATE_CMD:
	handle_enter_sdr_repository_update(mc, msg, rdata, rdata_len);
	break;

    case IPMI_EXIT_SDR_REPOSITORY_UPDATE_CMD:
	handle_exit_sdr_repository_update(mc, msg, rdata, rdata_len);
	break;

    case IPMI_GET_FRU_INVENTORY_AREA_INFO_CMD:
	handle_get_fru_inventory_area_info(mc, msg, rdata, rdata_len);
	break;

    case IPMI_READ_FRU_DATA_CMD:
	handle_read_fru_data(mc, msg, rdata, rdata_len);
	break;

    case IPMI_WRITE_FRU_DATA_CMD:
	handle_write_fru_data(mc, msg, rdata, rdata_len);
	break;

    default:
	handle_invalid_cmd(mc, rdata, rdata_len);
	break;
    }
}

static void
handle_get_device_id(lmc_data_t    *mc,
		     ipmi_msg_t    *msg,
		     unsigned char *rdata,
		     unsigned int  *rdata_len)
{
    memset(rdata, 0, 12);
    rdata[1] = mc->device_id;
    rdata[2] = ((mc->has_device_sdrs << 0x7)
		| (mc->device_revision & 0xf));
    rdata[3] = mc->major_fw_rev & 0x7f;
    rdata[4] = mc->minor_fw_rev;
    rdata[5] = 0x51;
    rdata[6] = mc->device_support;
    memcpy(rdata+7, mc->mfg_id, 3);
    memcpy(rdata+10, mc->product_id, 2);
    *rdata_len = 12;
}

static void
handle_app_netfn(lmc_data_t    *mc,
		 unsigned char lun,
		 ipmi_msg_t    *msg,
		 unsigned char *rdata,
		 unsigned int  *rdata_len)
{
    switch(msg->cmd) {
	case IPMI_GET_DEVICE_ID_CMD:
	    handle_get_device_id(mc, msg, rdata, rdata_len);
	    break;

	default:
	    handle_invalid_cmd(mc, rdata, rdata_len);
	    break;
    }
}

static void
handle_get_event_receiver(lmc_data_t    *mc,
			  ipmi_msg_t    *msg,
			  unsigned char *rdata,
			  unsigned int  *rdata_len)
{
    if (!(mc->device_support & IPMI_DEVID_IPMB_EVENT_GEN)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    rdata[0] = 0;
    rdata[1] = mc->event_receiver;
    rdata[2] = mc->event_receiver_lun & 0x3;
    *rdata_len = 3;
}

static void
handle_set_event_receiver(lmc_data_t    *mc,
			  ipmi_msg_t    *msg,
			  unsigned char *rdata,
			  unsigned int  *rdata_len)
{
    if (!(mc->device_support & IPMI_DEVID_IPMB_EVENT_GEN)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (check_msg_length(msg, 2, rdata, rdata_len))
	return;

    mc->event_receiver = msg->data[0] & 0xfe;
    mc->event_receiver_lun = msg->data[1] & 0x3;

    rdata[0] = 0;
    *rdata_len = 1;
}

static void
handle_get_device_sdr_info(lmc_data_t    *mc,
			   unsigned char lun,
			   ipmi_msg_t    *msg,
			   unsigned char *rdata,
			   unsigned int  *rdata_len)
{
    if (!(mc->device_support & IPMI_DEVID_SENSOR_DEV)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    rdata[0] = 0;
    rdata[1] = mc->num_sensors_per_lun[lun];
    rdata[2] = ((mc->dynamic_sensor_population << 7)
		| (mc->lun_has_sensors[3] << 3)
		| (mc->lun_has_sensors[2] << 2)
		| (mc->lun_has_sensors[1] << 1)
		| (mc->lun_has_sensors[0] << 0));
    if (!mc->dynamic_sensor_population) {
	*rdata_len = 3;
	return;
    }

    ipmi_set_uint32(rdata+3, mc->sensor_population_change_time);
    *rdata_len = 7;
}

static void
handle_reserve_device_sdr_repository(lmc_data_t    *mc,
				     unsigned char lun,
				     ipmi_msg_t    *msg,
				     unsigned char *rdata,
				     unsigned int  *rdata_len)
{
    if (!(mc->has_device_sdrs)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (!(mc->dynamic_sensor_population)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    mc->device_sdrs[lun].reservation++;
    if (mc->device_sdrs[lun].reservation == 0)
	mc->device_sdrs[lun].reservation++;

    rdata[0] = 0;
    ipmi_set_uint16(rdata+1, mc->device_sdrs[lun].reservation);
    *rdata_len = 3;
}

static void
handle_get_device_sdr(lmc_data_t    *mc,
		      unsigned char lun,
		      ipmi_msg_t    *msg,
		      unsigned char *rdata,
		      unsigned int  *rdata_len)
{
    uint16_t record_id;
    int      offset;
    int      count;
    sdr_t    *entry;

    if (!(mc->has_device_sdrs)) {
	handle_invalid_cmd(mc, rdata, rdata_len);
	return;
    }

    if (check_msg_length(msg, 6, rdata, rdata_len))
	return;

    if (mc->dynamic_sensor_population) {
	uint16_t reservation = ipmi_get_uint16(msg->data+0);

	if ((reservation != 0)
	    && (reservation != mc->device_sdrs[lun].reservation))
	{
	    rdata[0] = IPMI_INVALID_RESERVATION_CC;
	    *rdata_len = 1;
	    return;
	}
    }

    record_id = ipmi_get_uint16(msg->data+2);
    offset = msg->data[4];
    count = msg->data[5];

    if (record_id == 0) {
	entry = mc->device_sdrs[lun].sdrs;
    } else if (record_id == 0xffff) {
	entry = mc->device_sdrs[lun].sdrs;
	if (entry) {
	    while (entry->next) {
		entry = entry->next;
	    }
	}
    } else {
	entry = find_sdr_by_recid(mc, &mc->device_sdrs[lun], record_id, NULL);
    }

    if (entry == NULL) {
	rdata[0] = IPMI_NOT_PRESENT_CC;
	*rdata_len = 1;
	return;
    }

    if (offset >= entry->length) {
	rdata[0] = IPMI_PARAMETER_OUT_OF_RANGE_CC;
	*rdata_len = 1;
	return;
    }

    if ((offset+count) > entry->length)
	count = entry->length - offset;
    if (count+3 > *rdata_len) {
	/* Too much data to put into response. */
	rdata[0] = IPMI_REQUESTED_DATA_LENGTH_EXCEEDED_CC;
	*rdata_len = 1;
	return;
    }

    rdata[0] = 0;
    if (entry->next)
	ipmi_set_uint16(rdata+1, entry->next->record_id);
    else {
	rdata[1] = 0xff;
	rdata[2] = 0xff;
    }

    memcpy(rdata+3, entry->data+offset, count);
    *rdata_len = count + 3;
}

static void
handle_set_sensor_hysteresis(lmc_data_t    *mc,
			     unsigned char lun,
			     ipmi_msg_t    *msg,
			     unsigned char *rdata,
			     unsigned int  *rdata_len)
{
    int      sens_num;
    sensor_t *sensor;

    if (check_msg_length(msg, 4, rdata, rdata_len))
	return;

    sens_num = msg->data[0];
    if ((sens_num >= 255) || (!mc->sensors[lun][sens_num])) {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    sensor = mc->sensors[lun][sens_num];
    if (sensor->hysteresis_support != IPMI_HYSTERESIS_SUPPORT_SETTABLE) {
	rdata[0] = IPMI_INVALID_CMD_CC;
	*rdata_len = 1;
	return;
    }

    sensor->positive_hysteresis = msg->data[2];
    sensor->negative_hysteresis = msg->data[3];

    rdata[0] = 0;
    *rdata_len = 1;
}

static void
handle_get_sensor_hysteresis(lmc_data_t    *mc,
			     unsigned char lun,
			     ipmi_msg_t    *msg,
			     unsigned char *rdata,
			     unsigned int  *rdata_len)
{
    int      sens_num;
    sensor_t *sensor;

    if (check_msg_length(msg, 1, rdata, rdata_len))
	return;

    sens_num = msg->data[0];
    if ((sens_num >= 255) || (!mc->sensors[lun][sens_num])) {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    sensor = mc->sensors[lun][sens_num];
    if ((sensor->hysteresis_support != IPMI_HYSTERESIS_SUPPORT_SETTABLE)
	&& (sensor->hysteresis_support != IPMI_HYSTERESIS_SUPPORT_READABLE))
    {
	rdata[0] = IPMI_INVALID_CMD_CC;
	*rdata_len = 1;
	return;
    }

    rdata[0] = 0;
    rdata[1] = sensor->positive_hysteresis;
    rdata[2] = sensor->negative_hysteresis;
    *rdata_len = 3;
}

static void
do_event(lmc_data_t    *mc,
	 sensor_t      *sensor,
	 int           gen_event,
	 unsigned char direction,
	 unsigned char byte1,
	 unsigned char byte2,
	 unsigned char byte3)
{
    lmc_data_t    *dest_mc;
    unsigned char data[13];
    int           rv;

    if ((mc->event_receiver == 0)
	|| (!sensor->events_enabled)
	|| (!gen_event))
	return;

    rv = ipmi_emu_get_mc_by_addr(mc->emu, mc->event_receiver, &dest_mc);
    if (rv)
	return;

    /* Timestamp is ignored. */
    data[0] = 0;
    data[1] = 0;
    data[2] = 0;
    data[3] = 0;

    data[4] = mc->ipmb;
    data[5] = sensor->lun;
    data[6] = 0x04; /* Event message revision for IPMI 1.5. */
    data[7] = sensor->sensor_type;
    data[8] = sensor->num;
    data[9] = (direction << 7) | sensor->event_reading_code;
    data[10] = byte1;
    data[11] = byte2;
    data[12] = byte3;

    ipmi_mc_add_to_sel(dest_mc, 0x02, data);
}

static void
set_bit(lmc_data_t *mc, sensor_t *sensor, unsigned char bit,
	unsigned char value, int gen_event)
{
    if (value != sensor->event_status[bit]) {
	/* The bit value has changed. */
	sensor->event_status[bit] = value;
	if (value && sensor->event_enabled[0][bit]) {
	    do_event(mc, sensor, gen_event, IPMI_ASSERTION,
		     0x00 | bit, 0, 0);
	} else if (!value && sensor->event_enabled[1][bit]) {
	    do_event(mc, sensor, gen_event, IPMI_DEASSERTION,
		     0x00 | bit, 0, 0);
	}
    }
}

static void
check_thresholds(lmc_data_t *mc, sensor_t *sensor, int gen_event)
{
    int i;
    int bits_to_set = 0;
    int bits_to_clear = 0;

    for (i=0; i<3; i++) {
	if (sensor->threshold_supported[i])
	{
	    if (sensor->value <= sensor->thresholds[i])
		bits_to_set |= (1 << i);
	    else if ((sensor->value - sensor->negative_hysteresis)
		     > sensor->thresholds[i])
		bits_to_clear |= (1 << i);
	}
    }
    for (; i<6; i++) {
	if (sensor->threshold_supported[i]) {
	    if (sensor->value >= sensor->thresholds[i])
		bits_to_set |= (1 << i);
	    else if ((sensor->value + sensor->positive_hysteresis)
		     < sensor->thresholds[i])
		bits_to_clear |= (1 << i);
	}
    }

    /* We don't support lower assertions for high thresholds or higher
       assertions for low thresholds because that's just stupid. */
    for (i=0; i<3; i++) {
	if (((bits_to_set >> i) & 1) && !sensor->event_status[i]) {
	    /* This bit was not set, but we need to set it. */
	    sensor->event_status[i] = 1;
	    if (sensor->event_enabled[0][i*2]) {
		do_event(mc, sensor, gen_event, IPMI_ASSERTION,
			 0x50 | (i*2), sensor->value, sensor->thresholds[i]);
	    }
	} else if (((bits_to_clear >> i) & 1) && sensor->event_status[i]) {
	    /* This bit was not clear, but we need to clear it. */
	    sensor->event_status[i] = 0;
	    if (sensor->event_enabled[1][i*2]) {
		do_event(mc, sensor, gen_event, IPMI_DEASSERTION,
			 0x50 | (i*2), sensor->value, sensor->thresholds[i]);
	    }
	}
    }
    for (; i<6; i++) {
	if (((bits_to_set >> i) & 1) && !sensor->event_status[i]) {
	    /* This bit was not set, but we need to set it. */
	    sensor->event_status[i] = 1;
	    if (sensor->event_enabled[0][i*2+1]) {
		do_event(mc, sensor, gen_event, IPMI_ASSERTION,
			 0x50 | (i*2+1), sensor->value, sensor->thresholds[i]);
	    }
	} else if (((bits_to_clear >> i) & 1) && sensor->event_status[i]) {
	    /* This bit was not clear, but we need to clear it. */
	    sensor->event_status[i] = 0;
	    if (sensor->event_enabled[1][i*2+1]) {
		do_event(mc, sensor, gen_event, IPMI_DEASSERTION,
			 0x50 | (i*2+1), sensor->value, sensor->thresholds[i]);
	    }
	}
    }
}

static void
handle_set_sensor_thresholds(lmc_data_t    *mc,
			     unsigned char lun,
			     ipmi_msg_t    *msg,
			     unsigned char *rdata,
			     unsigned int  *rdata_len)
{
    int      sens_num;
    sensor_t *sensor;
    int      i;

    if (check_msg_length(msg, 8, rdata, rdata_len))
	return;

    sens_num = msg->data[0];
    if ((sens_num >= 255) || (!mc->sensors[lun][sens_num])) {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    sensor = mc->sensors[lun][sens_num];
    if ((sensor->event_reading_code != IPMI_EVENT_READING_TYPE_THRESHOLD)
	|| (sensor->threshold_support != IPMI_THRESHOLD_ACCESS_SUPPORT_SETTABLE))
    {
	rdata[0] = IPMI_INVALID_CMD_CC;
	*rdata_len = 1;
	return;
    }

    for (i=0; i<6; i++) {
	if ((msg->data[1] & (1 << i)) && (!sensor->threshold_supported[i])) {
	    rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	    *rdata_len = 1;
	    return;
	}
    }

    for (i=0; i<6; i++) {
	if (msg->data[1] & (1 << i)) {
	    sensor->thresholds[i] = msg->data[i+2];
	}
    }

    check_thresholds(mc, sensor, 1);

    rdata[0] = 0;
    *rdata_len = 1;
}

static void
handle_get_sensor_thresholds(lmc_data_t    *mc,
			     unsigned char lun,
			     ipmi_msg_t    *msg,
			     unsigned char *rdata,
			     unsigned int  *rdata_len)
{
    int      sens_num;
    sensor_t *sensor;
    int      i;

    if (check_msg_length(msg, 1, rdata, rdata_len))
	return;

    sens_num = msg->data[0];
    if ((sens_num >= 255) || (!mc->sensors[lun][sens_num])) {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    sensor = mc->sensors[lun][sens_num];
    if ((sensor->event_reading_code != IPMI_EVENT_READING_TYPE_THRESHOLD)
	|| ((sensor->threshold_support != IPMI_THRESHOLD_ACCESS_SUPPORT_SETTABLE)
	    && (sensor->threshold_support != IPMI_THRESHOLD_ACCESS_SUPPORT_READABLE)))
    {
	rdata[0] = IPMI_INVALID_CMD_CC;
	*rdata_len = 1;
	return;
    }

    rdata[0] = 0;
    rdata[1] = 0;
    for (i=0; i<6; i++) {
	if (sensor->threshold_supported[i]) {
	    rdata[1] |= 1 << i;
	    rdata[2+i] = sensor->thresholds[i];
	} else
	    rdata[2+i] = 0;
    }
    *rdata_len = 8;
}

static void
handle_set_sensor_event_enable(lmc_data_t    *mc,
			       unsigned char lun,
			       ipmi_msg_t    *msg,
			       unsigned char *rdata,
			       unsigned int  *rdata_len)
{
    int           sens_num;
    sensor_t      *sensor;
    int           i, j, e;
    unsigned char op;

    if (check_msg_length(msg, 2, rdata, rdata_len))
	return;

    sens_num = msg->data[0];
    if ((sens_num >= 255) || (!mc->sensors[lun][sens_num])) {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    sensor = mc->sensors[lun][sens_num];
    if ((sensor->event_support == IPMI_EVENT_SUPPORT_NONE)
	|| (sensor->event_support == IPMI_EVENT_SUPPORT_GLOBAL_ENABLE))
    {
	rdata[0] = IPMI_INVALID_CMD_CC;
	*rdata_len = 1;
	return;
    }

    op = (msg->data[1] >> 4) & 0x3;
    if (sensor->event_support == IPMI_EVENT_SUPPORT_ENTIRE_SENSOR) {
	if (op != 0) {
	    rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	    *rdata_len = 1;
	    return;
	}
    }

    if (op == 3) {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    sensor->events_enabled = (msg->data[1] >> 7) & 1;
    sensor->scanning_enabled = (msg->data[1] >> 6) & 1;
	
    if (op == 0)
	return;
    else if (op == 1)
	/* Enable selected events */
	op = 1;
    else
	/* Disable selected events */
	op = 0;

    e = 0;
    for (i=2; i<=3; i++) {
	if (msg->data_len <= i)
	    break;
	for (j=0; j<8; j++, e++) {
	    if ((msg->data[i] >> j) & 1)
		sensor->event_enabled[0][e] = op;
	}
    }
    e = 0;
    for (i=4; i<=5; i++) {
	if (msg->data_len <= i)
	    break;
	for (j=0; j<8; j++, e++) {
	    if ((msg->data[i] >> j) & 1)
		sensor->event_enabled[1][e] = op;
	}
    }

    rdata[0] = 0;
    *rdata_len = 1;
}

static void
handle_get_sensor_event_enable(lmc_data_t    *mc,
			       unsigned char lun,
			       ipmi_msg_t    *msg,
			       unsigned char *rdata,
			       unsigned int  *rdata_len)
{
    int           sens_num;
    sensor_t      *sensor;
    int           i, j, e;

    if (check_msg_length(msg, 1, rdata, rdata_len))
	return;

    sens_num = msg->data[0];
    if ((sens_num >= 255) || (!mc->sensors[lun][sens_num])) {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    sensor = mc->sensors[lun][sens_num];
    if ((sensor->event_support == IPMI_EVENT_SUPPORT_NONE)
	|| (sensor->event_support == IPMI_EVENT_SUPPORT_GLOBAL_ENABLE))
    {
	rdata[0] = IPMI_INVALID_CMD_CC;
	*rdata_len = 1;
	return;
    }

    rdata[0] = 0;
    rdata[1] = ((sensor->events_enabled << 7)
		| (sensor->scanning_enabled << 6));
	
    if (sensor->event_support == IPMI_EVENT_SUPPORT_ENTIRE_SENSOR) {
	*rdata_len = 2;
	return;
    }

    e = 0;
    for (i=2; i<=3; i++) {
	rdata[i] = 0;
	for (j=0; j<8; j++, e++)
	    rdata[i] |= sensor->event_enabled[0][e] << j;
    }
    e = 0;
    for (i=4; i<=5; i++) {
	rdata[i] = 0;
	for (j=0; j<8; j++, e++)
	    rdata[i] |= sensor->event_enabled[1][e] << j;
    }

    *rdata_len = 6;
}

static void
handle_set_sensor_type(lmc_data_t    *mc,
		       unsigned char lun,
		       ipmi_msg_t    *msg,
		       unsigned char *rdata,
		       unsigned int  *rdata_len)
{
    handle_invalid_cmd(mc, rdata, rdata_len);
}

static void
handle_get_sensor_type(lmc_data_t    *mc,
		       unsigned char lun,
		       ipmi_msg_t    *msg,
		       unsigned char *rdata,
		       unsigned int  *rdata_len)
{
    int           sens_num;
    sensor_t      *sensor;

    if (check_msg_length(msg, 1, rdata, rdata_len))
	return;

    sens_num = msg->data[0];
    if ((sens_num >= 255) || (!mc->sensors[lun][sens_num])) {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    sensor = mc->sensors[lun][sens_num];
    rdata[0] = 0;
    rdata[1] = sensor->sensor_type;
    rdata[2] = sensor->event_reading_code;
    *rdata_len = 3;
}

static void
handle_get_sensor_reading(lmc_data_t    *mc,
			  unsigned char lun,
			  ipmi_msg_t    *msg,
			  unsigned char *rdata,
			  unsigned int  *rdata_len)
{
    int      sens_num;
    sensor_t *sensor;
    int      i, j, e;

    if (check_msg_length(msg, 1, rdata, rdata_len))
	return;

    sens_num = msg->data[0];
    if ((sens_num >= 255) || (!mc->sensors[lun][sens_num])) {
	rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	*rdata_len = 1;
	return;
    }

    sensor = mc->sensors[lun][sens_num];

    rdata[0] = 0;
    rdata[1] = sensor->value;
    rdata[2] = ((sensor->events_enabled << 7)
		| (sensor->scanning_enabled << 6));
    e = 0;
    for (i=3; i<=4; i++) {
	rdata[i] = 0;
	for (j=0; j<8; j++, e++)
	    rdata[i] |= sensor->event_status[e] << j;
    }
    *rdata_len = 5;
}

int
ipmi_mc_sensor_set_bit(lmc_data_t   *mc,
		       unsigned char lun,
		       unsigned char sens_num,
		       unsigned char bit,
		       unsigned char value,
		       int           gen_event)
{
    sensor_t *sensor;

    if ((lun >= 4) || (sens_num >= 255) || (!mc->sensors[lun][sens_num]))
	return EINVAL;

    if (bit >= 15)
	return EINVAL;

    sensor = mc->sensors[lun][sens_num];

    set_bit(mc, sensor, bit, value, gen_event);
    return 0;
}

int
ipmi_mc_sensor_set_value(lmc_data_t    *mc,
			 unsigned char lun,
			 unsigned char sens_num,
			 unsigned char value,
			 int           gen_event)
{
    sensor_t *sensor;

    if ((lun >= 4) || (sens_num >= 255) || (!mc->sensors[lun][sens_num]))
	return EINVAL;

    sensor = mc->sensors[lun][sens_num];

    sensor->value = value;
    check_thresholds(mc, sensor, gen_event);
    return 0;
}

int
ipmi_mc_sensor_set_hysteresis(lmc_data_t    *mc,
			      unsigned char lun,
			      unsigned char sens_num,
			      unsigned char support,
			      unsigned char positive,
			      unsigned char negative)
{
    sensor_t *sensor;

    if ((lun >= 4) || (sens_num >= 255) || (!mc->sensors[lun][sens_num]))
	return EINVAL;

    sensor = mc->sensors[lun][sens_num];

    sensor->hysteresis_support = support;
    sensor->positive_hysteresis = positive;
    sensor->negative_hysteresis = negative;

    return 0;
}

int
ipmi_mc_sensor_set_threshold(lmc_data_t    *mc,
			     unsigned char lun,
			     unsigned char sens_num,
			     unsigned char support,
			     unsigned char supported[6],
			     unsigned char values[6])
{
    sensor_t *sensor;

    if ((lun >= 4) || (sens_num >= 255) || (!mc->sensors[lun][sens_num]))
	return EINVAL;

    sensor = mc->sensors[lun][sens_num];

    sensor->threshold_support = support;
    memcpy(sensor->threshold_supported, supported, 6);
    memcpy(sensor->thresholds, values, 6);

    return 0;
}

int
ipmi_mc_sensor_set_event_support(lmc_data_t    *mc,
				 unsigned char lun,
				 unsigned char sens_num,
				 unsigned char events_enable,
				 unsigned char scanning,
				 unsigned char support,
				 unsigned char assert_supported[15],
				 unsigned char deassert_supported[15],
				 unsigned char assert_enabled[15],
				 unsigned char deassert_enabled[15])
{
    sensor_t *sensor;

    if ((lun >= 4) || (sens_num >= 255) || (!mc->sensors[lun][sens_num]))
	return EINVAL;

    sensor = mc->sensors[lun][sens_num];

    sensor->events_enabled = events_enable;
    sensor->scanning_enabled = scanning;
    sensor->event_support = support;
    memcpy(sensor->event_supported[0], assert_supported, 15);
    memcpy(sensor->event_supported[1], deassert_supported, 15);
    memcpy(sensor->event_enabled[0], assert_enabled, 15);
    memcpy(sensor->event_enabled[1], deassert_enabled, 15);

    return 0;
}

int
ipmi_mc_add_sensor(lmc_data_t    *mc,
		   unsigned char lun,
		   unsigned char sens_num,
		   unsigned char type,
		   unsigned char event_reading_code)
{
    sensor_t *sensor;

    if ((lun >= 4) || (sens_num >= 255) || (mc->sensors[lun][sens_num]))
	return EINVAL;

    sensor = malloc(sizeof(*sensor));
    if (!sensor)
	return ENOMEM;
    memset(sensor, 0, sizeof(*sensor));

    sensor->lun = lun;
    sensor->num = sens_num;
    sensor->sensor_type = type;
    sensor->event_reading_code = event_reading_code;
    mc->sensors[lun][sens_num] = sensor;

    return 0;
}

static void
handle_sensor_event_netfn(lmc_data_t    *mc,
			  unsigned char lun,
			  ipmi_msg_t    *msg,
			  unsigned char *rdata,
			  unsigned int  *rdata_len)
{
    switch(msg->cmd) {
    case IPMI_GET_EVENT_RECEIVER_CMD:
	handle_get_event_receiver(mc, msg, rdata, rdata_len);
	break;

    case IPMI_SET_EVENT_RECEIVER_CMD:
	handle_set_event_receiver(mc, msg, rdata, rdata_len);
	break;

    case IPMI_GET_DEVICE_SDR_INFO_CMD:
	handle_get_device_sdr_info(mc, lun, msg, rdata, rdata_len);
	break;

    case IPMI_RESERVE_DEVICE_SDR_REPOSITORY_CMD:
	handle_reserve_device_sdr_repository(mc, lun, msg, rdata, rdata_len);
	break;

    case IPMI_GET_DEVICE_SDR_CMD:
	handle_get_device_sdr(mc, lun, msg, rdata, rdata_len);
	break;

    case IPMI_SET_SENSOR_HYSTERESIS_CMD:
	handle_set_sensor_hysteresis(mc, lun, msg, rdata, rdata_len);
	break;

    case IPMI_GET_SENSOR_HYSTERESIS_CMD:
	handle_get_sensor_hysteresis(mc, lun, msg, rdata, rdata_len);
	break;
	
    case IPMI_SET_SENSOR_THRESHOLD_CMD:
	handle_set_sensor_thresholds(mc, lun, msg, rdata, rdata_len);
	break;

    case IPMI_GET_SENSOR_THRESHOLD_CMD:
	handle_get_sensor_thresholds(mc, lun, msg, rdata, rdata_len);
	break;

    case IPMI_SET_SENSOR_EVENT_ENABLE_CMD:
	handle_set_sensor_event_enable(mc, lun, msg, rdata, rdata_len);
	break;

    case IPMI_GET_SENSOR_EVENT_ENABLE_CMD:
	handle_get_sensor_event_enable(mc, lun, msg, rdata, rdata_len);
	break;

    case IPMI_SET_SENSOR_TYPE_CMD:
	handle_set_sensor_type(mc, lun, msg, rdata, rdata_len);
	break;

    case IPMI_GET_SENSOR_TYPE_CMD:
	handle_get_sensor_type(mc, lun, msg, rdata, rdata_len);
	break;

    case IPMI_GET_SENSOR_READING_CMD:
	handle_get_sensor_reading(mc, lun, msg, rdata, rdata_len);
	break;

    case IPMI_GET_SENSOR_EVENT_STATUS_CMD:
    case IPMI_REARM_SENSOR_EVENTS_CMD:
    case IPMI_GET_SENSOR_READING_FACTORS_CMD:
    default:
	handle_invalid_cmd(mc, rdata, rdata_len);
	break;
    }
}

int
ipmi_mc_set_power(lmc_data_t *mc, unsigned char power, int gen_event)
{
    lmc_data_t    *dest_mc;
    unsigned char data[13];
    int           rv;

    if (mc->power_value == power)
	return 0;

    mc->power_value = power;

    if ((mc->event_receiver == 0)
	|| (!gen_event))
	return 0;

    rv = ipmi_emu_get_mc_by_addr(mc->emu, mc->event_receiver, &dest_mc);
    if (rv)
	return 0;

    /* Timestamp is ignored. */
    data[0] = 0;
    data[1] = 0;
    data[2] = 0;
    data[3] = 0;

    data[4] = 0x20; /* These come from 0x20. */
    data[5] = 0;
    data[6] = 0x01; /* Version 1. */
    data[7] = 0;
    data[8] = 0; /* Control number 0. */
    data[9] = 0;
    data[10] = power;
    data[11] = 0;
    data[12] = 0;

    ipmi_mc_add_to_sel(dest_mc, 0xc0, data);
	
    return 0;
}

static void
handle_set_power(lmc_data_t    *mc,
		 ipmi_msg_t    *msg,
		 unsigned char *rdata,
		 unsigned int  *rdata_len)
{
    if (check_msg_length(msg, 1, rdata, rdata_len))
	return;

    ipmi_mc_set_power(mc, msg->data[0], 1);

    rdata[0] = 0;
    *rdata_len = 1;
}

static void
handle_get_power(lmc_data_t    *mc,
		 ipmi_msg_t    *msg,
		 unsigned char *rdata,
		 unsigned int  *rdata_len)
{
    rdata[0] = 0;
    rdata[1] = mc->power_value;
    *rdata_len = 2;
}

static void
handle_oem0_netfn(lmc_data_t    *mc,
		  unsigned char lun,
		  ipmi_msg_t    *msg,
		  unsigned char *rdata,
		  unsigned int  *rdata_len)
{
    switch(msg->cmd) {
    case 0x01:
	handle_set_power(mc, msg, rdata, rdata_len);
	break;

    case 0x02:
	handle_get_power(mc, msg, rdata, rdata_len);
	break;

    default:
	handle_invalid_cmd(mc, rdata, rdata_len);
	break;
    }
}

static uint8_t
ipmb_checksum(uint8_t *data, int size, uint8_t start)
{
	uint8_t csum = start;
	
	for (; size > 0; size--, data++)
		csum += *data;

	return -csum;
}

void
ipmi_emu_handle_msg(emu_data_t     *emu,
		    unsigned char  lun,
		    ipmi_msg_t     *msg,
		    unsigned char  *rdata,
		    unsigned int   *rdata_len)
{
    lmc_data_t *mc;
    ipmi_msg_t smsg;
    ipmi_msg_t *omsg = msg;
    unsigned char *data = NULL;

    if (msg->cmd == IPMI_SEND_MSG_CMD) {
	/* Encapsulated IPMB, do special handling. */
	unsigned char slave;
	unsigned int  data_len;

	if (check_msg_length(msg, 8, rdata, rdata_len))
	    return;
	if ((msg->data[0] & 0x3f) != 0) {
	    rdata[0] = IPMI_INVALID_DATA_FIELD_CC;
	    *rdata_len = 1;
	    return;
	}

	data = msg->data + 1;
	data_len = msg->data_len - 1;
	if (data[0] == 0) {
	    /* Broadcast, just skip the first byte, but check len. */
	    data++;
	    data_len--;
	    if (data_len < 7) {
		rdata[0] = IPMI_REQUEST_DATA_LENGTH_INVALID_CC;
		*rdata_len = 1;
		return;
	    }
	}
	slave = data[0];
	mc = emu->ipmb[slave >> 1];
	if (!mc) {
	    rdata[0] = 0x83; /* NAK on Write */
	    *rdata_len = 1;
	    return;
	}

	smsg.netfn = data[1] >> 2;
	lun = data[1] & 0x3;
	smsg.cmd = data[5];
	smsg.data = data + 6;
	smsg.data_len = data_len - 7; /* Subtract off the header and
					 the end checksum */
	msg = &smsg;
    } else {
	mc = emu->ipmb[emu->bmc_mc >> 1];
	if (!mc) {
	    rdata[0] = 0xff;
	    *rdata_len = 1;
	    return;
	}
    }

    switch (msg->netfn) {
    case IPMI_APP_NETFN:
	handle_app_netfn(mc, lun, msg, rdata, rdata_len);
	break;

    case IPMI_SENSOR_EVENT_NETFN:
	handle_sensor_event_netfn(mc, lun, msg, rdata, rdata_len);
	break;

    case IPMI_STORAGE_NETFN:
	handle_storage_netfn(mc, lun, msg, rdata, rdata_len);
	break;

    case 0x30:
	handle_oem0_netfn(mc, lun, msg, rdata, rdata_len);
	break;

    default:
	handle_invalid_cmd(mc, rdata, rdata_len);
	break;
    }

    if (omsg->cmd == IPMI_SEND_MSG_CMD) {
	int i;
	for (i=*rdata_len-1; i>=0; i--)
	    rdata[i+7] = rdata[i];
	rdata[0] = 0;
	rdata[1] = emu->bmc_mc;
	rdata[2] = ((msg->netfn | 1) << 2) | (data[4] & 0x3);
	rdata[3] = ipmb_checksum(rdata+1, 2, 0);
	rdata[4] = data[0];
	rdata[5] = (data[4] & 0xfc) | (data[1] & 0x03);
	rdata[6] = data[5];
	*rdata_len += 7;
	rdata[*rdata_len] = ipmb_checksum(rdata, *rdata_len, 0);
	*rdata_len += 1;
    }
}

emu_data_t *
ipmi_emu_alloc(void)
{
    emu_data_t *data = malloc(sizeof(*data));

    if (data)
	memset(data, 0, sizeof(*data));
    return data;
}

static void
ipmi_mc_destroy(lmc_data_t *mc)
{
    sel_entry_t *entry, *n_entry;

    entry = mc->sel.entries;
    while (entry) {
	n_entry = entry->next;
	free(entry);
	entry = n_entry;
    }
    free(mc);
}

int
ipmi_emu_add_mc(emu_data_t    *emu,
		unsigned char ipmb,
		unsigned char device_id,
		unsigned char has_device_sdrs,
		unsigned char device_revision,
		unsigned char major_fw_rev,
		unsigned char minor_fw_rev,
		unsigned char device_support,
		unsigned char mfg_id[3],
		unsigned char product_id[2],
		unsigned char dynamic_sensor_population)
{
    lmc_data_t     *mc;
    struct timeval t;
    int            i;

    if (ipmb & 1)
	return EINVAL;

    mc = malloc(sizeof(*mc));
    if (!mc)
	return ENOMEM;
    memset(mc, 0, sizeof(*mc));

    mc->emu = emu;
    mc->ipmb = ipmb;

    mc->device_id = device_id;
    mc->has_device_sdrs = has_device_sdrs;
    mc->device_revision = device_revision;
    mc->major_fw_rev = major_fw_rev;
    mc->minor_fw_rev = minor_fw_rev;
    mc->device_support = device_support;
    mc->dynamic_sensor_population = dynamic_sensor_population;
    memcpy(mc->mfg_id, mfg_id, 3);
    memcpy(mc->product_id, product_id, 2);

    /* Start the time at zero. */
    gettimeofday(&t, NULL);
    mc->sel.time_offset = t.tv_sec;
    mc->main_sdrs.time_offset = t.tv_sec;
    for (i=0; i<4; i++)
	mc->device_sdrs[i].time_offset = t.tv_sec;

    mc->event_receiver = 0x20;
    mc->event_receiver_lun = 0;

    if (emu->ipmb[ipmb >> 1])
	ipmi_mc_destroy(emu->ipmb[ipmb >> 1]);

    emu->ipmb[ipmb >> 1] = mc;
    return 0;
}

void
ipmi_mc_set_device_id(lmc_data_t *mc, unsigned char device_id)
{
    mc->device_id = device_id;
}

unsigned char
ipmi_mc_get_device_id(lmc_data_t *mc)
{
    return mc->device_id;
}

void
ipmi_set_has_device_sdrs(lmc_data_t *mc, unsigned char has_device_sdrs)
{
    mc->has_device_sdrs = has_device_sdrs;
}

unsigned char
ipmi_get_has_device_sdrs(lmc_data_t *mc)
{
    return mc->has_device_sdrs;
}

void
ipmi_set_device_revision(lmc_data_t *mc, unsigned char device_revision)
{
    mc->device_revision = device_revision;
}

unsigned char
ipmi_get_device_revision(lmc_data_t *mc)
{
    return mc->device_revision;
}

void
ipmi_set_major_fw_rev(lmc_data_t *mc, unsigned char major_fw_rev)
{
    mc->major_fw_rev = major_fw_rev;
}

unsigned char
ipmi_get_major_fw_rev(lmc_data_t *mc)
{
    return mc->major_fw_rev;
}

void
ipmi_set_minor_fw_rev(lmc_data_t *mc, unsigned char minor_fw_rev)
{
    mc->minor_fw_rev = minor_fw_rev;
}

unsigned char
ipmi_get_minor_fw_rev(lmc_data_t *mc)
{
    return mc->minor_fw_rev;
}

void
ipmi_set_device_support(lmc_data_t *mc, unsigned char device_support)
{
    mc->device_support = device_support;
}

unsigned char
ipmi_get_device_support(lmc_data_t *mc)
{
    return mc->device_support;
}

void
ipmi_set_mfg_id(lmc_data_t *mc, unsigned char mfg_id[3])
{
    memcpy(mc->mfg_id, mfg_id, 3);
}

void
ipmi_get_mfg_id(lmc_data_t *mc, unsigned char mfg_id[3])
{
    memcpy(mfg_id, mc->mfg_id, 3);
}

void
ipmi_set_product_id(lmc_data_t *mc, unsigned char product_id[2])
{
    memcpy(mc->product_id, product_id, 2);
}

void
ipmi_get_product_id(lmc_data_t *mc, unsigned char product_id[2])
{
    memcpy(product_id, mc->product_id, 2);
}

int
ipmi_emu_get_mc_by_addr(emu_data_t *emu, unsigned char ipmb, lmc_data_t **mc)
{
    if (ipmb & 1)
	return EINVAL;
    if (!emu->ipmb[ipmb >> 1])
	return ENOSYS;
    *mc = emu->ipmb[ipmb >> 1];
    return 0;
}

int
ipmi_emu_set_bmc_mc(emu_data_t *emu, unsigned char ipmb)
{
    if (ipmb & 1)
	return EINVAL;
    emu->bmc_mc = ipmb;
    return 0;
}
