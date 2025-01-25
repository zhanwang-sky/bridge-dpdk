//
//  main.c
//  bridge-dpdk
//
//  Created by zhanwang-sky on 2025/1/19.
//

#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>

#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_debug.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_malloc.h>

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#define NUM_MBUFS 8192
#define MBUF_CACHE_SIZE 256
#define BURST_SIZE 32

typedef struct {
    uint16_t port_id;
    int dev_socket_id;
    struct rte_ether_addr mac_addr;
} app_port_t;

typedef struct {
    struct rte_mempool* mbuf_pool;
    app_port_t* app_port;
} app_config_t;

app_port_t* app_port_init(uint16_t port_id, struct rte_mempool* mbuf_pool) {
    int rc;
    int dev_socket_id;
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    app_port_t* app_port;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_conf eth_cfg;
    struct rte_ether_addr mac_addr;

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

    rc = rte_eth_macaddr_get(port_id, &mac_addr);
    if (rc < 0) {
        rte_panic("Fail to get mac addr: port %hu, %s\n",
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
            "MAC: " RTE_ETHER_ADDR_PRT_FMT "\n"
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
            dev_info.tx_offload_capa,
            RTE_ETHER_ADDR_BYTES(&mac_addr));

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
                                NULL, mbuf_pool);
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

    app_port->port_id = port_id;
    app_port->dev_socket_id = dev_socket_id;
    rte_ether_addr_copy(&mac_addr, &app_port->mac_addr);

    return app_port;
}

static int
app_second_loop(__rte_unused void *arg) {
    RTE_LOG(INFO, USER1, ">>> Launching second loop on lcore %u\n",
            rte_lcore_id());

    for (;;) {
        RTE_LOG(INFO, USER1, "XXX TODO: Generate gratuitous ARP.\n");
        sleep(180);
    }

    return 0;
}

static __rte_noreturn void
app_main_loop(void *arg) {
    app_config_t* app_cfg = (app_config_t*) arg;
    uint64_t timer_hz;
    uint64_t prev_cycles;
    uint64_t curr_cycles;
    uint64_t pkts_per_sec;
    struct rte_mbuf* rx_pkts[BURST_SIZE];
    uint16_t nb_rx;

    RTE_LOG(INFO, USER1, ">>> Launching main loop on lcore %u\n",
            rte_lcore_id());

    timer_hz = rte_get_timer_hz();
    prev_cycles = rte_get_timer_cycles();
    pkts_per_sec = 0;

    for (;;) {
        nb_rx = rte_eth_rx_burst(app_cfg->app_port->port_id, 0, rx_pkts, BURST_SIZE);
        for (uint16_t i = 0; i != nb_rx; ++i) {
            rte_pktmbuf_free(rx_pkts[i]);
        }
        pkts_per_sec += nb_rx;

        curr_cycles = rte_get_timer_cycles();
        if (curr_cycles - prev_cycles > timer_hz) {
            RTE_LOG(INFO, USER1, "%lu pkt/s\n", pkts_per_sec);
            prev_cycles = curr_cycles;
            pkts_per_sec = 0;
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
    app_cfg.mbuf_pool = rte_pktmbuf_pool_create("ethdev_mbuf_pool",
                                                NUM_MBUFS, MBUF_CACHE_SIZE,
                                                0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                                rte_socket_id());
    if (!app_cfg.mbuf_pool) {
        rte_panic("Fail to create mempool: %s\n", rte_strerror(rte_errno));
    }

    for (port_id = 0; port_id != nr_ports; ++port_id) {
        app_cfg.app_port = app_port_init(port_id, app_cfg.mbuf_pool);
        if (app_cfg.app_port != NULL) {
            break;
        }
    }
    if (!app_cfg.app_port) {
        rte_exit(EXIT_FAILURE, "There is no app port initialized.\n");
    }

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
