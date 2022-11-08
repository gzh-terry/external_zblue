/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <debug/stack.h>
#include <sys/util.h>

#include <net/buf.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/conn.h>
#include <bluetooth/mesh.h>

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_MESH_DEBUG_ADV)
#define LOG_MODULE_NAME bt_mesh_adv
#include "common/log.h"

#include "adv.h"
#include "net.h"
#include "foundation.h"
#include "beacon.h"
#include "host/ecc.h"
#include "prov.h"
#include "proxy.h"
#include "pb_gatt_srv.h"

/* Window and Interval are equal for continuous scanning */
#define MESH_SCAN_INTERVAL    BT_MESH_ADV_SCAN_UNIT(BT_MESH_SCAN_INTERVAL_MS)
#define MESH_SCAN_WINDOW      BT_MESH_ADV_SCAN_UNIT(BT_MESH_SCAN_WINDOW_MS)

const uint8_t bt_mesh_adv_type[BT_MESH_ADV_TYPES] = {
	[BT_MESH_ADV_PROV]   = BT_DATA_MESH_PROV,
	[BT_MESH_ADV_DATA]   = BT_DATA_MESH_MESSAGE,
	[BT_MESH_ADV_BEACON] = BT_DATA_MESH_BEACON,
	[BT_MESH_ADV_URI]    = BT_DATA_URI,
};

static K_QUEUE_DEFINE(bt_mesh_adv_queue);
static K_QUEUE_DEFINE(bt_mesh_relay_queue);

static void adv_buf_destroy(struct net_buf *buf)
{
	struct bt_mesh_adv adv = *BT_MESH_ADV(buf);

	net_buf_destroy(buf);

	bt_mesh_adv_send_end(0, &adv);
}

NET_BUF_POOL_DEFINE(adv_buf_pool, CONFIG_BT_MESH_ADV_BUF_COUNT,
		    BT_MESH_ADV_DATA_SIZE, BT_MESH_ADV_USER_DATA_SIZE,
		    adv_buf_destroy);

static struct bt_mesh_adv adv_local_pool[CONFIG_BT_MESH_ADV_BUF_COUNT];

#if defined(CONFIG_BT_MESH_RELAY)
NET_BUF_POOL_DEFINE(relay_buf_pool, CONFIG_BT_MESH_RELAY_BUF_COUNT,
		    BT_MESH_ADV_DATA_SIZE, BT_MESH_ADV_USER_DATA_SIZE,
		    adv_buf_destroy);

static struct bt_mesh_adv adv_relay_pool[CONFIG_BT_MESH_RELAY_BUF_COUNT];
#endif

static struct net_buf *bt_mesh_adv_create_from_pool(struct net_buf_pool *buf_pool,
						    struct bt_mesh_adv *adv_pool,
						    enum bt_mesh_adv_type type,
						    enum bt_mesh_adv_tag tag,
						    uint8_t xmit, uint8_t prio,
						    k_timeout_t timeout)
{
	struct bt_mesh_adv *adv;
	struct net_buf *buf;

	if (atomic_test_bit(bt_mesh.flags, BT_MESH_SUSPENDED)) {
		BT_WARN("Refusing to allocate buffer while suspended");
		return NULL;
	}

	buf = net_buf_alloc(buf_pool, timeout);
	if (!buf) {
		return NULL;
	}

	adv = &adv_pool[net_buf_id(buf)];
	BT_MESH_ADV(buf) = adv;

	(void)memset(adv, 0, sizeof(*adv));

	adv->type         = type;
	adv->tag          = tag;
	adv->xmit         = xmit;
	adv->prio         = prio;

	return buf;
}

struct net_buf *bt_mesh_adv_relay_create(uint8_t prio, uint8_t xmit)
{
#if defined(CONFIG_BT_MESH_RELAY)
	sys_sfnode_t *prev = NULL, *curr, *lowest_prev = NULL, *lowest = NULL;
	uint8_t prio_cur = prio;
	struct net_buf *buf;

	buf = bt_mesh_adv_create_from_pool(&relay_buf_pool, adv_relay_pool,
					   BT_MESH_ADV_DATA, BT_MESH_RELAY_ADV,
					   xmit, prio, K_NO_WAIT);
	if (buf) {
		return buf;
	}

	if (!IS_ENABLED(CONFIG_BT_MESH_RELAY_PRIORITY) || !prio_cur) {
		return NULL;
	}

	SYS_SFLIST_FOR_EACH_NODE(&bt_mesh_relay_queue.data_q, curr) {
		buf = CONTAINER_OF(curr, struct net_buf, node);

		if (BT_MESH_ADV(buf)->prio < prio_cur) {
			prio_cur = BT_MESH_ADV(buf)->prio;
			lowest_prev = prev;
			lowest = curr;
		}

		prev = curr;
	}

	if (!lowest) {
		return NULL;
	}

	sys_sflist_remove(&bt_mesh_relay_queue.data_q, lowest_prev, lowest);

	buf = CONTAINER_OF(lowest, struct net_buf, node);
	buf->frags = NULL;

	bt_mesh_adv_send_start(0, -ECANCELED, BT_MESH_ADV(buf));
	net_buf_unref(buf);

	return bt_mesh_adv_create_from_pool(&relay_buf_pool, adv_relay_pool,
					    BT_MESH_ADV_DATA, BT_MESH_RELAY_ADV,
					    xmit, prio, K_NO_WAIT);
#endif /* CONFIG_BT_MESH_RELAY */

	return bt_mesh_adv_create_from_pool(&adv_buf_pool, adv_local_pool,
					    BT_MESH_ADV_DATA, BT_MESH_RELAY_ADV,
					    xmit, 0, K_NO_WAIT);
}

struct net_buf *bt_mesh_adv_main_create(enum bt_mesh_adv_type type,
					uint8_t xmit, k_timeout_t timeout)
{
	BT_DBG("");

	return bt_mesh_adv_create_from_pool(&adv_buf_pool, adv_local_pool,
					    type, BT_MESH_LOCAL_ADV,
					    xmit, 0, timeout);
}

static struct net_buf *adv_buf_get_from_queue(struct k_queue *queue,
					      k_timeout_t timeout)
{
	struct net_buf *buf;

	buf = k_queue_get(queue, timeout);
	if (buf) {
		buf->frags = NULL;
	}

	return buf;
}

#if CONFIG_BT_MESH_RELAY_ADV_SETS
static struct net_buf *process_events(struct k_poll_event *ev, int count)
{
	for (; count; ev++, count--) {
		BT_DBG("ev->state %u", ev->state);

		switch (ev->state) {
		case K_POLL_STATE_FIFO_DATA_AVAILABLE: {
			return adv_buf_get_from_queue(ev->queue, K_NO_WAIT);
		}
		case K_POLL_STATE_NOT_READY:
		case K_POLL_STATE_CANCELLED:
			break;
		default:
			BT_WARN("Unexpected k_poll event state %u", ev->state);
			break;
		}
	}

	return NULL;
}

struct net_buf *bt_mesh_adv_buf_get(k_timeout_t timeout)
{
	int err;
	struct k_poll_event events[] = {
		K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_FIFO_DATA_AVAILABLE,
						K_POLL_MODE_NOTIFY_ONLY,
						&bt_mesh_adv_queue,
						0),
#if defined(CONFIG_BT_MESH_ADV_EXT_RELAY_USING_MAIN_ADV_SET)
		K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_FIFO_DATA_AVAILABLE,
						K_POLL_MODE_NOTIFY_ONLY,
						&bt_mesh_relay_queue,
						0),
#endif /* CONFIG_BT_MESH_ADV_EXT_RELAY_USING_MAIN_ADV_SET */
	};

	err = k_poll(events, ARRAY_SIZE(events), timeout);
	if (err) {
		return NULL;
	}

	return process_events(events, ARRAY_SIZE(events));
}

struct net_buf *bt_mesh_adv_buf_relay_get(k_timeout_t timeout)
{
	return adv_buf_get_from_queue(&bt_mesh_relay_queue, timeout);
}

struct net_buf *bt_mesh_adv_buf_get_by_tag(uint8_t tag, k_timeout_t timeout)
{
	if (tag & BT_MESH_LOCAL_ADV) {
		return bt_mesh_adv_buf_get(timeout);
	} else if (tag & BT_MESH_RELAY_ADV) {
		return bt_mesh_adv_buf_relay_get(timeout);
	} else {
		return NULL;
	}
}

static void bt_mesh_relay_send(struct net_buf *buf)
{
	uint8_t prio = BT_MESH_ADV(buf)->prio;
	sys_sfnode_t *curr, *prev = NULL;

	if (!IS_ENABLED(CONFIG_BT_MESH_RELAY_PRIORITY) || !prio) {
		k_queue_append(&bt_mesh_relay_queue, net_buf_ref(buf));
		bt_mesh_adv_buf_relay_ready();
		return;
	}

	SYS_SFLIST_FOR_EACH_NODE(&bt_mesh_relay_queue.data_q, curr) {
		struct net_buf *buf_curr = CONTAINER_OF(curr, struct net_buf, node);

		if (BT_MESH_ADV(buf_curr)->prio < prio) {
			break;
		}

		prev = curr;
	}

	/* The messages with the highest priority are always placed at the head, and
	 * the messages with the same priority are arranged in chronological order.
	 */
	k_queue_insert(&bt_mesh_relay_queue, prev, net_buf_ref(buf));

	bt_mesh_adv_buf_relay_ready();
}
#else /* !CONFIG_BT_MESH_RELAY_ADV_SETS */
struct net_buf *bt_mesh_adv_buf_get(k_timeout_t timeout)
{
	return adv_buf_get_from_queue(&bt_mesh_adv_queue, timeout);
}

struct net_buf *bt_mesh_adv_buf_get_by_tag(uint8_t tag, k_timeout_t timeout)
{
	ARG_UNUSED(tag);

	return bt_mesh_adv_buf_get(timeout);
}
#endif /* CONFIG_BT_MESH_RELAY_ADV_SETS */

void bt_mesh_adv_buf_get_cancel(void)
{
	BT_DBG("");

	k_queue_cancel_wait(&bt_mesh_adv_queue);

#if CONFIG_BT_MESH_RELAY_ADV_SETS
	k_queue_cancel_wait(&bt_mesh_relay_queue);
#endif /* CONFIG_BT_MESH_RELAY_ADV_SETS */
}

void bt_mesh_adv_send(struct net_buf *buf, const struct bt_mesh_send_cb *cb,
		      void *cb_data)
{
	BT_DBG("type 0x%02x len %u: %s", BT_MESH_ADV(buf)->type, buf->len,
	       bt_hex(buf->data, buf->len));

	BT_MESH_ADV(buf)->cb = cb;
	BT_MESH_ADV(buf)->cb_data = cb_data;
	BT_MESH_ADV(buf)->busy = 1U;

#if CONFIG_BT_MESH_RELAY_ADV_SETS
	if (BT_MESH_ADV(buf)->tag == BT_MESH_RELAY_ADV) {
		bt_mesh_relay_send(buf);
		return;
	}
#endif /* CONFIG_BT_MESH_RELAY_ADV_SETS */

	k_queue_append(&bt_mesh_adv_queue, net_buf_ref(buf));
	bt_mesh_adv_buf_local_ready();
}

int bt_mesh_adv_gatt_send(void)
{
	if (bt_mesh_is_provisioned()) {
		if (IS_ENABLED(CONFIG_BT_MESH_GATT_PROXY)) {
			BT_DBG("Proxy Advertising");
			return bt_mesh_proxy_adv_start();
		}
	} else if (IS_ENABLED(CONFIG_BT_MESH_PB_GATT)) {
		BT_DBG("PB-GATT Advertising");
		return bt_mesh_pb_gatt_srv_adv_start();
	}

	return -ENOTSUP;
}

static void bt_mesh_scan_cb(const bt_addr_le_t *addr, int8_t rssi,
			    uint8_t adv_type, struct net_buf_simple *buf)
{
	if (adv_type != BT_GAP_ADV_TYPE_ADV_NONCONN_IND) {
		return;
	}

	BT_DBG("len %u: %s", buf->len, bt_hex(buf->data, buf->len));

	while (buf->len > 1) {
		struct net_buf_simple_state state;
		uint8_t len, type;

		len = net_buf_simple_pull_u8(buf);
		/* Check for early termination */
		if (len == 0U) {
			return;
		}

		if (len > buf->len) {
			BT_WARN("AD malformed");
			return;
		}

		net_buf_simple_save(buf, &state);

		type = net_buf_simple_pull_u8(buf);

		buf->len = len - 1;

		switch (type) {
		case BT_DATA_MESH_MESSAGE:
			bt_mesh_net_recv(buf, rssi, BT_MESH_NET_IF_ADV);
			break;
#if defined(CONFIG_BT_MESH_PB_ADV)
		case BT_DATA_MESH_PROV:
			bt_mesh_pb_adv_recv(buf);
			break;
#endif
		case BT_DATA_MESH_BEACON:
			bt_mesh_beacon_recv(buf);
			break;
		default:
			break;
		}

		net_buf_simple_restore(buf, &state);
		net_buf_simple_pull(buf, len);
	}
}

int bt_mesh_scan_enable(void)
{
	struct bt_le_scan_param scan_param = {
			.type       = BT_HCI_LE_SCAN_PASSIVE,
			.options    = BT_LE_SCAN_OPT_NONE,
			.interval   = MESH_SCAN_INTERVAL,
			.window     = MESH_SCAN_WINDOW };
	int err;

	BT_DBG("");

	err = bt_le_scan_start(&scan_param, bt_mesh_scan_cb);
	if (err && err != -EALREADY) {
		BT_ERR("starting scan failed (err %d)", err);
		return err;
	}

	return 0;
}

int bt_mesh_scan_disable(void)
{
	int err;

	BT_DBG("");

	err = bt_le_scan_stop();
	if (err && err != -EALREADY) {
		BT_ERR("stopping scan failed (err %d)", err);
		return err;
	}

	return 0;
}
