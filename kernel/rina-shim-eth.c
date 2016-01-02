/*
 * RINA Ethernet shim DIF
 *
 *    Vincenzo Maffione <v.maffione@gmail.it>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/types.h>
#include <rina/rina-utils.h>
#include <rina/rina-ipcp-types.h>
#include "rina-kernel.h"

#include <linux/module.h>
#include <linux/aio.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/rtnetlink.h>
#include <linux/spinlock.h>


#define ETH_P_RINA  0xD1F0

struct arpt_entry {
    /* Targed Hardware Address. Only support 48-bit addresses for now. */
    uint8_t tha[6];

    /* Targed Protocol Address, represented as a serialized string. */
    char *tpa;

    bool complete;

    struct list_head node;
};

struct rina_shim_eth {
    struct ipcp_entry *ipcp;
    struct net_device *netdev;
    char *upper_name_s;
    struct list_head arp_table;
    spinlock_t arpt_lock;
};

static void *
rina_shim_eth_create(struct ipcp_entry *ipcp)
{
    struct rina_shim_eth *priv;

    priv = kzalloc(sizeof(*priv), GFP_KERNEL);
    if (!priv) {
        return NULL;
    }

    priv->ipcp = ipcp;
    priv->netdev = NULL;
    priv->upper_name_s = NULL;
    INIT_LIST_HEAD(&priv->arp_table);
    spin_lock_init(&priv->arpt_lock);

    printk("%s: New IPC created [%p]\n", __func__, priv);

    return priv;
}

static void
rina_shim_eth_destroy(struct ipcp_entry *ipcp)
{
    struct rina_shim_eth *priv = ipcp->priv;
    struct arpt_entry *entry, *tmp;

    spin_lock(&priv->arpt_lock);
    list_for_each_entry_safe(entry, tmp, &priv->arp_table, node) {
        list_del(&entry->node);
        kfree(entry);
    }
    spin_unlock(&priv->arpt_lock);

    if (priv->netdev) {
        rtnl_lock();
        netdev_rx_handler_unregister(priv->netdev);
        rtnl_unlock();
        dev_put(priv->netdev);
    }

    if (priv->upper_name_s) {
        kfree(priv->upper_name_s);
    }

    kfree(priv);

    printk("%s: IPC [%p] destroyed\n", __func__, priv);
}

static int
rina_shim_eth_register(struct ipcp_entry *ipcp, struct rina_name *appl,
                       int reg)
{
    struct rina_shim_eth *priv = ipcp->priv;
    char *tmp;

    if (reg) {
        if (priv->upper_name_s) {
            /* Only one application can be currently registered. */
            return -EBUSY;
        }

        priv->upper_name_s = rina_name_to_string(appl);

        if (priv->upper_name_s) {
            PD("%s: Application %s registered\n", __func__, priv->upper_name_s);
        }

        return priv->upper_name_s ? 0 : -ENOMEM;
    }

    if (!priv->upper_name_s) {
        /* Nothing to do. */
        return 0;
    }

    tmp = rina_name_to_string(appl);
    if (!tmp) {
        PE("%s: Out of memory\n", __func__);
        return -ENOMEM;
    }

    if (strcmp(tmp, priv->upper_name_s) == 0) {
        PD("%s: Application %s unregistered\n", __func__, priv->upper_name_s);
        kfree(priv->upper_name_s);
        priv->upper_name_s = NULL;
    } else {
        /* Nothing to do. Main module may be trying to clean up a
         * failed registration, so don't report an error. */
    }

    kfree(tmp);

    return 0;
}

/* To be called under arpt_lock. */
static struct arpt_entry *
arp_lookup_direct_b(struct rina_shim_eth *priv, const char *dst_app, int len)
{
    struct arpt_entry *entry;

    list_for_each_entry(entry, &priv->arp_table, node) {
        if (len >= strlen(entry->tpa) &&
                strncmp(entry->tpa, dst_app, len) == 0) {
            return entry;
        }
    }

    return NULL;
}

/* To be called under arpt_lock. */
/* XXX remove this one */
static int
arp_lookup_direct(struct rina_shim_eth *priv, const struct rina_name *dst_app,
                  struct arpt_entry **ret)
{
    char *name_s = __rina_name_to_string(dst_app, 0);

    *ret = NULL;

    if (!name_s) {
        PE("%s: Out of memory\n", __func__);
        return -ENOMEM;
    }

    *ret = arp_lookup_direct_b(priv, name_s, strlen(name_s));

    return 0;
}

/* This function is taken after net/ipv4/arp.c:arp_create() */
static struct sk_buff *
arp_create(struct rina_shim_eth *priv, uint16_t op, const char *spa,
           const char *tpa, const void *tha, gfp_t gfp)
{
    int spa_len, tpa_len;
    int hhlen = LL_RESERVED_SPACE(priv->netdev); /* Hardware header length */
    struct sk_buff *skb = NULL;
    struct arphdr *arp;
    int arp_msg_len;
    int pa_len;
    uint8_t *ptr;

    spa_len = strlen(spa);
    tpa_len = strlen(tpa);
    pa_len = (tpa_len > spa_len) ? tpa_len : spa_len;

    arp_msg_len = sizeof(*arp) + 2 * (pa_len + priv->netdev->addr_len);

    skb = alloc_skb(hhlen + arp_msg_len + priv->netdev->needed_tailroom,
                    gfp);
    if (!skb) {
        return NULL;
    }

    skb_reserve(skb, hhlen);
    skb_reset_network_header(skb);
    arp = (struct arphdr *)skb_put(skb, arp_msg_len);
    skb->dev = priv->netdev;
    skb->protocol = htons(ETH_P_ARP);

    if (dev_hard_header(skb, skb->dev, ETH_P_ARP,
                        tha ? tha : priv->netdev->broadcast,
                        priv->netdev->dev_addr, skb->len) < 0) {
        kfree_skb(skb);
        return NULL;
    }

    arp->ar_hrd = htons(priv->netdev->type);
    arp->ar_pro = htons(ETH_P_RINA);
    arp->ar_hln = priv->netdev->addr_len;
    arp->ar_pln = pa_len;
    arp->ar_op = htons(op);

    ptr = (uint8_t *)(arp + 1);

    /* Fill in the Sender Hardware Address. */
    memcpy(ptr, priv->netdev->dev_addr, priv->netdev->addr_len);
    ptr += priv->netdev->addr_len;

    /* Fill in the zero-padded Sender Protocol Address. */
    memcpy(ptr, spa, spa_len);
    memset(ptr + spa_len, 0, pa_len - spa_len);
    ptr += pa_len;

    /* Fill in the Target Hardware Address, or the unknown
     * address if not provided. */
    if (tha) {
        memcpy(ptr, tha, priv->netdev->addr_len);
    } else {
        memset(ptr, 0, priv->netdev->addr_len);
    }
    ptr += priv->netdev->addr_len;

    /* Fill in the zero-padded Target Protocol Address. */
    memcpy(ptr, tpa, tpa_len);
    memset(ptr + tpa_len, 0, pa_len - tpa_len);
    ptr += pa_len;

    return skb;
}

static int
rina_shim_eth_fa_req(struct ipcp_entry *ipcp,
                     struct flow_entry *flow)
{
    struct rina_shim_eth *priv = ipcp->priv;
    struct arpt_entry *entry;
    struct sk_buff *skb;
    char *spa = NULL; /* Sender Protocol Address. */
    char *tpa = NULL; /* Target Protocol Address. */
    int ret;

    if (!priv->netdev) {
        return -ENXIO;
    }

    spin_lock(&priv->arpt_lock);
    ret = arp_lookup_direct(priv, &flow->remote_application, &entry);
    if (ret) {
        return ret;
    }

    if (entry) {
        spin_unlock(&priv->arpt_lock);
        /* ARP entry already exist for remote application. Nothing
         * to do here. */
        return 0;
    }

    entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
    if (!entry) {
        goto nomem;
    }

    entry->tpa = __rina_name_to_string(&flow->remote_application, 0);
    if (!entry->tpa) {
        goto nomem;
    }
    entry->complete = false;
    list_add_tail(&entry->node, &priv->arp_table);

    spin_unlock(&priv->arpt_lock);

    spa = rina_name_to_string(&flow->local_application);
    tpa = rina_name_to_string(&flow->remote_application);
    if (!spa || !tpa) {
        goto nomem;
    }

    skb = arp_create(priv, ARPOP_REQUEST, spa, tpa, NULL, GFP_KERNEL);
    if (!skb) {
        goto nomem;
    }

    dev_queue_xmit(skb);

    return 0;

nomem:
    PE("%s: Out of memory\n", __func__);

    if (spa) kfree(spa);
    if (tpa) kfree(tpa);
    if (entry) {
        list_del(&entry->node);
        if (entry->tpa) {
            kfree(entry->tpa);
        }
        kfree(entry);
    }

    return -ENOMEM;
}

static int
rina_shim_eth_fa_resp(struct ipcp_entry *ipcp, struct flow_entry *flow,
                      uint8_t response)
{
    return -EINVAL;
}

static void
shim_eth_arp_rx(struct rina_shim_eth *priv, struct arphdr *arp, int len)
{
    const char *sha = (const char *)(arp) + sizeof(*arp);
    const char *spa = (const char *)(arp) + sizeof(*arp) + arp->ar_hln;
    const char *tpa = (const char *)(arp) + sizeof(*arp) +
                      2 * arp->ar_hln + arp->ar_pln;

    PD("ARPLEN %d EXP %d\n", len, (int)(sizeof(*arp) +
                             2*(arp->ar_pln + arp->ar_hln)));

    if (ntohs(arp->ar_op) == ARPOP_REQUEST) {
        int upper_name_len;
        struct sk_buff *skb;

        if (!priv->upper_name_s) {
            /* No application registered here, there's nothing to do. */
            return;
        }
        upper_name_len = strlen(priv->upper_name_s);

        if (arp->ar_pln < upper_name_len) {
            /* This ARP request cannot match us. */
            return;
        }

        if (memcmp(tpa, priv->upper_name_s, upper_name_len)) {
            /* No match. */
            return;
        }

        /* Send an ARP reply. */
        skb = arp_create(priv, ARPOP_REPLY, priv->upper_name_s,
                         spa, sha, GFP_ATOMIC);
        if (skb) {
            dev_queue_xmit(skb);
        }

    } else if (ntohs(arp->ar_op) == ARPOP_REPLY) {
        /* Update the ARP table with an entry SPA --> SHA. */
        struct arpt_entry *entry;

        spin_lock(&priv->arpt_lock);

        entry = arp_lookup_direct_b(priv, spa, arp->ar_pln);
        if (!entry) {
            spin_unlock(&priv->arpt_lock);
            /* Gratuitous ARP reply. Don't accept it (for now). */
            PI("%s: Dropped gratuitous ARP reply\n", __func__);
            return;
        }

        if (arp->ar_hln != sizeof(entry->tha)) {
            spin_unlock(&priv->arpt_lock);
            /* Only support 48-bits hardware address (for now). */
            PI("%s: Dropped ARP reply with SHA/THA len of %d\n",
               __func__, arp->ar_hln);
            return;
        }

        memcpy(entry->tha, sha, arp->ar_hln);
        entry->complete = true;

        PD("%s: ARP entry %s --> %02X%02X%02X%02X%02X%02X completed\n",
           __func__, entry->tpa, entry->tha[0], entry->tha[1],
           entry->tha[2], entry->tha[3], entry->tha[4], entry->tha[5]);

        spin_unlock(&priv->arpt_lock);
    } else {
        PI("%s: Unknown RINA ARP operation %04X\n", __func__,
                ntohs(arp->ar_op));
    }
}

static rx_handler_result_t
shim_eth_rx_handler(struct sk_buff **skbp)
{
    struct sk_buff *skb = (*skbp);
    struct rina_shim_eth *priv = (struct rina_shim_eth *)
                rcu_dereference(skb->dev->rx_handler_data);
    unsigned int ethertype = ntohs(skb->protocol);

    (void)priv;

    PD("%s: intercept skb %u, protocol %u\n", __func__, skb->len, ntohs(skb->protocol));

    if (ethertype == ETH_P_ARP) {
        /* This is an ARP frame. */
        struct arphdr *arp = (struct arphdr *)skb->data;

        if (ntohs(arp->ar_pro) == ETH_P_RINA) {
            /* This ARP operation belongs to RINA stack. */
            PD("SHIM ETH ARP\n");
            shim_eth_arp_rx(priv, arp, skb->len);

        } else {
            /* This ARP message belongs to Linux stack. */
            return RX_HANDLER_PASS;
        }

    } else if (ethertype == ETH_P_RINA) {
        /* This is a RINA shim-eth PDU. */
        PD("SHIM ETH PDU\n");

    } else {
        /* This frame doesn't belong to RINA stack. */
        return RX_HANDLER_PASS;
    }

    /* Steal the skb from the Linux stack. */
    dev_consume_skb_any(skb);

    return RX_HANDLER_CONSUMED;
}

static int
rina_shim_eth_sdu_write(struct ipcp_entry *ipcp,
                             struct flow_entry *flow,
                             struct rina_buf *rb,
                             bool maysleep)
{
    struct rina_shim_eth *priv = ipcp->priv;

    (void)priv;

    rina_buf_free(rb);

    return 0;
}

static int
rina_shim_eth_config(struct ipcp_entry *ipcp,
                       const char *param_name,
                       const char *param_value)
{
    struct rina_shim_eth *priv = (struct rina_shim_eth *)ipcp->priv;
    int ret = -EINVAL;

    if (strcmp(param_name, "netdev") == 0) {
        void *ns = &init_net;
#ifdef CONFIG_NET_NS
        ns = current->nsproxy->net_ns;
#endif
        priv->netdev = dev_get_by_name(ns, param_value);
        if (priv->netdev) {
            rtnl_lock();
            ret = netdev_rx_handler_register(priv->netdev, shim_eth_rx_handler,
                                             priv);
            rtnl_unlock();
            if (ret == 0) {
                PD("%s: netdev set to %p\n", __func__, priv->netdev);
            } else {
                dev_put(priv->netdev);
                priv->netdev = NULL;
            }
        }
    }

    return ret;
}

static int __init
rina_shim_eth_init(void)
{
    struct ipcp_factory factory;
    int ret;

    memset(&factory, 0, sizeof(factory));
    factory.owner = THIS_MODULE;
    factory.dif_type = DIF_TYPE_SHIM_ETH;
    factory.create = rina_shim_eth_create;
    factory.ops.destroy = rina_shim_eth_destroy;
    factory.ops.flow_allocate_req = rina_shim_eth_fa_req;
    factory.ops.flow_allocate_resp = rina_shim_eth_fa_resp;
    factory.ops.sdu_write = rina_shim_eth_sdu_write;
    factory.ops.config = rina_shim_eth_config;
    factory.ops.application_register = rina_shim_eth_register;

    ret = rina_ipcp_factory_register(&factory);

    return ret;
}

static void __exit
rina_shim_eth_fini(void)
{
    rina_ipcp_factory_unregister(DIF_TYPE_SHIM_ETH);
}

module_init(rina_shim_eth_init);
module_exit(rina_shim_eth_fini);
MODULE_LICENSE("GPL");
