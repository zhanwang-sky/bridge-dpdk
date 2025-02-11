//
//  main.c
//  bridge-dpdk
//
//  Created by zhanwang-sky on 2025/1/19.
//

#include <stdio.h>
#include <stdlib.h>

#include <rte_arp.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_debug.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_prefetch.h>
#include <rte_spinlock.h>

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#define NUM_MBUFS 8192
#define MBUF_CACHE_SIZE 256
#define BURST_SIZE 32

typedef struct {
    uint16_t port_id;
    int dev_socket_id;
    struct rte_ether_addr mac_addr;
    uint32_t ip_addr;
} app_port_t;

typedef struct {
    struct rte_mempool* mempool;
    app_port_t* app_port;
    rte_spinlock_t tx_lock;
} app_config_t;

app_port_t* app_port_init(uint16_t port_id, struct rte_mempool* mempool) {
    int rc;
    int dev_socket_id;
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    app_port_t* app_port;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_conf eth_cfg;
    struct rte_ether_addr mac_addr;
    struct rte_eth_link link;
    char link_status_text[RTE_ETH_LINK_MAX_STR_LEN];

    app_port = rte_malloc("app_port_t", sizeof(app_port_t), RTE_CACHE_LINE_SIZE);
    if (!app_port) {
        rte_panic("Fail to alloc memory for app_port\n");
    }

    dev_socket_id = rte_eth_dev_socket_id(port_id);
    if (dev_socket_id < 0 && rte_errno != 0) {
        rte_panic("Could not determine dev_socket_id for port %hu: %s\n",
                  port_id, rte_strerror(rte_errno));
    }

    rc = rte_eth_dev_info_get(port_id, &dev_info);
    if (rc < 0) {
        rte_panic("Fail to get dev info: port %hu, %s\n",
                  port_id, rte_strerror(-rc));
    }

    RTE_LOG(INFO, USER1,
            "port[%hu] info:\n---\n"
            "name: %s\n"
            "bus_info: %s\n"
            "driver: %s\n"
            "dev_socket_id: %d\n"
            "min_mtu: %hu\n"
            "max_mtu: %hu\n"
            "max_rx_queues: %hu\n"
            "max_tx_queues: %hu\n"
            "max_rx_descs: %hu\n"
            "max_tx_descs: %hu\n"
            "rx_offload_capa: %08lx\n"
            "tx_offload_capa: %08lx\n"
            "===\n",
            port_id,
            rte_dev_name(dev_info.device),
            rte_dev_bus_info(dev_info.device),
            dev_info.driver_name,
            dev_socket_id,
            dev_info.min_mtu,
            dev_info.max_mtu,
            dev_info.max_rx_queues,
            dev_info.max_tx_queues,
            dev_info.rx_desc_lim.nb_max,
            dev_info.tx_desc_lim.nb_max,
            dev_info.rx_offload_capa,
            dev_info.tx_offload_capa);

    memset(&eth_cfg, 0, sizeof(eth_cfg));
    rc = rte_eth_dev_configure(port_id, 1, 1, &eth_cfg);
    if (rc < 0) {
        rte_panic("Fail to configure eth dev: port %hu, %s\n",
                  port_id, rte_strerror(-rc));
    }

    rc = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd, &nb_txd);
    if (rc < 0) {
        rte_panic("Fail to adjust nb rx tx desc: port %hu, %s\n",
                  port_id, rte_strerror(-rc));
    }

    rc = rte_eth_rx_queue_setup(port_id, 0, nb_rxd,
                                dev_socket_id,
                                NULL, mempool);
    if (rc < 0) {
        rte_panic("Fail to set rx queue: port %hu, %s\n",
                  port_id, rte_strerror(-rc));
    }

    rc = rte_eth_tx_queue_setup(port_id, 0, nb_txd,
                                dev_socket_id,
                                NULL);
    if (rc < 0) {
        rte_panic("Fail to set tx queue: port %hu, %s\n",
                  port_id, rte_strerror(-rc));
    }

    rc = rte_eth_dev_start(port_id);
    if (rc < 0) {
        rte_panic("Fail to start eth dev: port %hu, %s\n",
                  port_id, rte_strerror(-rc));
    }

    rc = rte_eth_macaddr_get(port_id, &mac_addr);
    if (rc < 0) {
        rte_panic("Fail to get mac addr: port %hu, %s\n",
                  port_id, rte_strerror(-rc));
    }

    RTE_LOG(INFO, USER1, "port[%hu] MAC: " RTE_ETHER_ADDR_PRT_FMT "\n",
            port_id, RTE_ETHER_ADDR_BYTES(&mac_addr));

    rc = rte_eth_link_get(port_id, &link);
    if (rc < 0) {
        rte_panic("Fail to get link status: port %hu, %s\n",
                  port_id, rte_strerror(-rc));
    }

    rc = rte_eth_link_to_str(link_status_text, sizeof(link_status_text), &link);
    if (rc < 0) {
        rte_panic("Fail to convert link status to text string: port %hu, %s\n",
                  port_id, rte_strerror(-rc));
    }

    RTE_LOG(INFO, USER1, "port[%hu] status: %s\n", port_id, link_status_text);

    app_port->port_id = port_id;
    app_port->dev_socket_id = dev_socket_id;
    rte_ether_addr_copy(&mac_addr, &app_port->mac_addr);

    return app_port;
}

static inline int
app_arp_process(app_port_t* app_port, struct rte_mbuf* mbuf,
                struct rte_ether_hdr* eth_h,
                struct rte_arp_hdr* arp_h) {
    // length check
    if (mbuf->nb_segs > 1) {
        return 0; // unexpected segments
    }
    // ARP header check
    if ((rte_be_to_cpu_16(arp_h->arp_hardware) != RTE_ARP_HRD_ETHER) ||
        (rte_be_to_cpu_16(arp_h->arp_protocol) != RTE_ETHER_TYPE_IPV4) ||
        (arp_h->arp_hlen != RTE_ETHER_ADDR_LEN) ||
        (arp_h->arp_plen != sizeof(uint32_t)) ||
        (rte_be_to_cpu_16(arp_h->arp_opcode) != RTE_ARP_OP_REQUEST)) {
        return 0;
    }
    // ARP data check
    if ((!rte_is_unicast_ether_addr(&arp_h->arp_data.arp_sha)) ||
        (arp_h->arp_data.arp_sip == 0xffffffff) ||
        ((!rte_is_same_ether_addr(&arp_h->arp_data.arp_tha, &app_port->mac_addr)) &&
         (rte_be_to_cpu_32(arp_h->arp_data.arp_tip) != app_port->ip_addr))) {
        return 0;
    }

    // ARP
    arp_h->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);
    rte_ether_addr_copy(&arp_h->arp_data.arp_sha, &arp_h->arp_data.arp_tha);
    arp_h->arp_data.arp_tip = arp_h->arp_data.arp_sip;
    rte_ether_addr_copy(&app_port->mac_addr, &arp_h->arp_data.arp_sha);
    arp_h->arp_data.arp_sip = rte_cpu_to_be_32(app_port->ip_addr);
    // ether
    rte_ether_addr_copy(&eth_h->src_addr, &eth_h->dst_addr);
    rte_ether_addr_copy(&app_port->mac_addr, &eth_h->src_addr);
    // mbuf
    mbuf->pkt_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
    mbuf->data_len = mbuf->pkt_len;

    return 1;
}

static inline int
app_icmp_process(app_port_t* app_port, __rte_unused struct rte_mbuf* mbuf,
                 struct rte_ether_hdr* eth_h,
                 struct rte_ipv4_hdr* ipv4_h,
                 struct rte_icmp_hdr* icmp_h) {
    uint32_t cksum;

    // ICMP header check
    if ((icmp_h->icmp_type != RTE_ICMP_TYPE_ECHO_REQUEST) ||
        (icmp_h->icmp_code != 0)) {
        return 0;
    }

    // ICMP
    icmp_h->icmp_type = RTE_ICMP_TYPE_ECHO_REPLY;
    cksum = ~icmp_h->icmp_cksum & 0xffff;
    cksum += ~rte_cpu_to_be_16(RTE_ICMP_TYPE_ECHO_REQUEST << 8) & 0xffff;
    cksum += rte_cpu_to_be_16(RTE_ICMP_TYPE_ECHO_REPLY << 8);
    cksum = (cksum & 0xffff) + (cksum >> 16);
    cksum = (cksum & 0xffff) + (cksum >> 16);
    icmp_h->icmp_cksum = ~cksum;
    // IPv4
    ipv4_h->dst_addr = ipv4_h->src_addr;
    ipv4_h->src_addr = rte_cpu_to_be_32(app_port->ip_addr);
    // ether
    rte_ether_addr_copy(&eth_h->src_addr, &eth_h->dst_addr);
    rte_ether_addr_copy(&app_port->mac_addr, &eth_h->src_addr);

    return 1;
}

static inline int
app_ipv4_process(app_port_t* app_port, struct rte_mbuf* mbuf,
                 struct rte_ether_hdr* eth_h,
                 struct rte_ipv4_hdr* ipv4_h) {
    // IPv4 header check
    if ((ipv4_h->ihl != 5) ||
        ((rte_be_to_cpu_16(ipv4_h->fragment_offset) & 0x3fff) != 0) ||
        (ipv4_h->time_to_live == 0) ||
        (ipv4_h->src_addr == 0) ||
        (rte_be_to_cpu_32(ipv4_h->dst_addr) != app_port->ip_addr)) {
        return 0;
    }

    if (ipv4_h->next_proto_id == IPPROTO_ICMP) {
        struct rte_icmp_hdr* icmp_h = (struct rte_icmp_hdr*) ((char*) ipv4_h + sizeof(*ipv4_h));
        return app_icmp_process(app_port, mbuf, eth_h, ipv4_h, icmp_h);
    }

    return 0;
}

static inline int
app_pkt_process(app_port_t* app_port, struct rte_mbuf* mbuf) {
    struct rte_ether_hdr* eth_h = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr*);
    uint16_t ether_type;

    // ether header check
    if (!rte_is_unicast_ether_addr(&eth_h->src_addr)) {
        return 0;
    }

    ether_type = rte_be_to_cpu_16(eth_h->ether_type);
    if (ether_type == RTE_ETHER_TYPE_ARP) {
        struct rte_arp_hdr* arp_h = (struct rte_arp_hdr*) ((char*) eth_h + sizeof(*eth_h));
        return app_arp_process(app_port, mbuf, eth_h, arp_h);
    } else if (ether_type == RTE_ETHER_TYPE_IPV4) {
        struct rte_ipv4_hdr* ipv4_h = (struct rte_ipv4_hdr*) ((char*) eth_h + sizeof(*eth_h));
        return app_ipv4_process(app_port, mbuf, eth_h, ipv4_h);
    }

    return 0;
}

static int
app_second_loop(__rte_unused void *arg) {
    app_config_t* app_cfg = (app_config_t*) arg;
    app_port_t* app_port = app_cfg->app_port;
    struct rte_mbuf* mbuf;
    struct rte_ether_hdr* eth_h;
    struct rte_arp_hdr* arp_h;

    RTE_LOG(INFO, USER1, ">>> Launching second loop on lcore %u\n", rte_lcore_id());

    rte_delay_us_sleep(1000 * 1000); // 1s

    for (;;) {
        mbuf = rte_pktmbuf_alloc(app_cfg->mempool);
        if (!mbuf) {
            rte_panic("Fail to alloc mbuf for gratuitous APR\n");
        }

        mbuf->pkt_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
        mbuf->data_len = mbuf->pkt_len;

        eth_h = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr*);
        rte_ether_unformat_addr("FF:FF:FF:FF:FF:FF", &eth_h->dst_addr);
        rte_ether_addr_copy(&app_port->mac_addr, &eth_h->src_addr);
        eth_h->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

        arp_h = rte_pktmbuf_mtod_offset(mbuf, struct rte_arp_hdr*, sizeof(struct rte_ether_hdr));
        arp_h->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
        arp_h->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
        arp_h->arp_hlen = RTE_ETHER_ADDR_LEN;
        arp_h->arp_plen = sizeof(uint32_t);
        arp_h->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REQUEST);
        rte_ether_addr_copy(&app_port->mac_addr, &arp_h->arp_data.arp_sha);
        arp_h->arp_data.arp_sip = rte_cpu_to_be_32(app_port->ip_addr);
        rte_ether_unformat_addr("FF:FF:FF:FF:FF:FF", &arp_h->arp_data.arp_tha);
        arp_h->arp_data.arp_tip = arp_h->arp_data.arp_sip;

        rte_spinlock_lock(&app_cfg->tx_lock);
        do {
            if (rte_eth_tx_burst(app_port->port_id, 0, &mbuf, 1) != 1) {
                RTE_LOG(WARNING, USER1, "Fail to send gratuitous APR\n");
                rte_pktmbuf_free(mbuf);
                break;
            }
            RTE_LOG(WARNING, USER1, "Gratuitous APR sent\n");
        } while (0);
        rte_spinlock_unlock(&app_cfg->tx_lock);

        rte_delay_us_sleep(180 * 1000 * 1000);
    }

    return 0;
}

static __rte_noreturn void
app_main_loop(void *arg) {
    app_config_t* app_cfg = (app_config_t*) arg;
    app_port_t* app_port = app_cfg->app_port;
    struct rte_mbuf* mbufs[BURST_SIZE];
    uint16_t nb_rx;
    uint16_t nb_tx;

    RTE_LOG(INFO, USER1, ">>> Launching main loop on lcore %u\n", rte_lcore_id());

    for (;;) {
        nb_rx = rte_eth_rx_burst(app_cfg->app_port->port_id, 0, mbufs, BURST_SIZE);
        nb_tx = 0;

        // process received
        for (uint16_t i = 0; i != nb_rx; ++i) {
            if (likely(i < nb_rx - 1)) {
                rte_prefetch0(rte_pktmbuf_mtod(mbufs[i + 1], void*));
            }
            if (!app_pkt_process(app_port, mbufs[i])) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            mbufs[nb_tx++] = mbufs[i];
        }

        // send reply
        if (nb_tx > 0) {
            uint16_t nb_sent;
            rte_spinlock_lock(&app_cfg->tx_lock);
            nb_sent = rte_eth_tx_burst(app_port->port_id, 0, mbufs, nb_tx);
            rte_spinlock_unlock(&app_cfg->tx_lock);
            for (uint16_t i = nb_sent; i != nb_tx; ++i) {
                rte_pktmbuf_free(mbufs[i]);
            }
        }
    }
}

int main(int argc, char* argv[]) {
    int rc;
    unsigned nr_lcores;
    unsigned lcore_id;
    uint16_t nr_ports;
    uint16_t port_id;
    app_config_t app_cfg;

    rc = rte_eal_init(argc, argv);
    if (rc < 0) {
        rte_panic("Fail to init EAL: %s\n", rte_strerror(rte_errno));
    }

    RTE_LOG(INFO, USER1, "RTE_MAX_ETHPORTS=%d\n", RTE_MAX_ETHPORTS);
    RTE_LOG(INFO, USER1, "RTE_MAX_LCORE=%d\n", RTE_MAX_LCORE);

    nr_ports = rte_eth_dev_count_avail();
    if (!nr_ports) {
        rte_exit(EXIT_FAILURE, "No available NICs!\n");
    }
    RTE_LOG(INFO, USER1, "%hu ports available\n", nr_ports);

    nr_lcores = rte_lcore_count();
    if (nr_lcores < 2) {
        rte_exit(EXIT_FAILURE, "At least 2 lcores required!\n");
    }
    RTE_LOG(INFO, USER1, "%u lcores available\n", nr_lcores);

    memset(&app_cfg, 0, sizeof(app_cfg));
    app_cfg.mempool = rte_pktmbuf_pool_create("ethdev_mbuf_pool",
                                                NUM_MBUFS, MBUF_CACHE_SIZE,
                                                0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                                rte_socket_id());
    if (!app_cfg.mempool) {
        rte_panic("Fail to create mempool: %s\n", rte_strerror(rte_errno));
    }

    for (port_id = 0; port_id != nr_ports; ++port_id) {
        app_cfg.app_port = app_port_init(port_id, app_cfg.mempool);
        if (app_cfg.app_port != NULL) {
            break;
        }
    }
    if (!app_cfg.app_port) {
        rte_exit(EXIT_FAILURE, "There is no app port initialized.\n");
    }
    app_cfg.app_port->ip_addr  = 192; app_cfg.app_port->ip_addr <<= 8;
    app_cfg.app_port->ip_addr |= 168; app_cfg.app_port->ip_addr <<= 8;
    app_cfg.app_port->ip_addr |= 1;   app_cfg.app_port->ip_addr <<= 8;
    app_cfg.app_port->ip_addr |= 254;

    rte_spinlock_init(&app_cfg.tx_lock);

    lcore_id = rte_get_next_lcore(-1, 1, 0);
    rc = rte_eal_remote_launch(app_second_loop, &app_cfg, lcore_id);
    if (rc < 0) {
        rte_panic("Fail to launch second loop on lcore %u: %s\n",
                  lcore_id, rte_strerror(-rc));
    }

    app_main_loop(&app_cfg);

    rc = rte_eal_cleanup();
    if (rc < 0) {
        rte_panic("Fail to cleanup EAL: %s\n", rte_strerror(rte_errno));
    }

    return 0;
}
