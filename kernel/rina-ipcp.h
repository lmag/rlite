#ifndef __RINA_IPCP_H__
#define __RINA_IPCP_H__

#include <rina/rina-utils.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include "rina-bufs.h"


struct ipcp_entry;
struct flow_entry;

struct ipcp_ops {
    void (*destroy)(struct ipcp_entry *ipcp);
    int (*assign_to_dif)(struct ipcp_entry *ipcp, struct rina_name *dif_name);
    int (*application_register)(struct ipcp_entry *ipcp,
                                struct rina_name *app_name);
    int (*application_unregister)(struct ipcp_entry *ipcp,
                                  struct rina_name *app_name);

    /* Invoked by the core to notify the IPCP about a new
     * flow allocation request from the upper layer. */
    int (*flow_allocate_req)(struct ipcp_entry *ipcp,
                             struct flow_entry *flow);

    /* Invoked by the core to notify the IPCP about a
     * flow allocation response from the upper layer. */
    int (*flow_allocate_resp)(struct ipcp_entry *ipcp, struct flow_entry *flow,
                              uint8_t response);

    /* Invoked by the core to notify the IPCP about a flow allocation
     * request coming from the lower layer. Not valid for shim IPC
     * processes. */
    int (*flow_allocate_req_arrived)(struct ipcp_entry *ipcp,
                                     uint32_t remote_port,
                                     const struct rina_name * remote_ipcp_name);

    int (*sdu_write)(struct ipcp_entry *ipcp, struct flow_entry *flow,
                     struct rina_buf *rb);
    int (*config)(struct ipcp_entry *ipcp, const char *param_name,
                  const char *param_value);
};

struct ipcp_entry {
    uint16_t            id;    /* Key */
    struct rina_name    name;
    struct rina_name    dif_name;
    uint8_t             dif_type;
    struct ipcp_ops     ops;
    void                *priv;
    struct list_head    registered_applications;

    /* The module that owns this IPC process. */
    struct module       *owner;
    struct mutex        lock;
    unsigned int        refcnt;
    struct hlist_node   node;
};

struct ipcp_factory {
    /* The module providing this factory. */
    struct module *owner;
    uint8_t dif_type;
    void *(*create)(struct ipcp_entry *ipcp);
    struct ipcp_ops ops;
    struct list_head node;
};

enum {
    FLOW_STATE_NULL = 0,
    FLOW_STATE_PENDING,
    FLOW_STATE_ALLOCATED,
};

struct rina_ctrl;

struct upper_ref {
    unsigned int        userspace;
    struct rina_ctrl    *rc;
    struct ipcp_entry   *ipcp;
};

struct flow_entry {
    uint16_t            local_port;  /* key */
    uint16_t            remote_port;
    uint8_t             state;
    struct rina_name    local_application;
    struct rina_name    remote_application;
    struct ipcp_entry   *ipcp;
    struct upper_ref    upper;
    uint32_t            event_id; /* requestor event id */
    struct list_head    rxq;
    wait_queue_head_t   rxq_wqh;
    spinlock_t          rxq_lock;

    struct mutex        lock; /* Unused */
    unsigned int        refcnt;
    struct hlist_node   node;
};

int rina_ipcp_factory_register(struct ipcp_factory *factory);
int rina_ipcp_factory_unregister(uint8_t dif_type);

int rina_flow_allocate_req_arrived(struct ipcp_entry *ipcp,
                                   uint32_t remote_port,
                                   struct rina_name *local_application,
                                   struct rina_name *remote_application);

int rina_flow_allocate_resp_arrived(struct ipcp_entry *ipcp,
                                    uint32_t local_port,
                                    uint32_t remote_port,
                                    uint8_t response);

int rina_sdu_rx(struct ipcp_entry *ipcp, struct rina_buf *rb,
                uint32_t local_port);

#endif  /* __RINA_IPCP_H__ */
