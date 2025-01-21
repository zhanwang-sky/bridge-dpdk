//
//  main.c
//  bridge-dpdk
//
//  Created by zhanwang-sky on 2025/1/19.
//

#include <stdio.h>
#include <stdlib.h>

#include <rte_common.h>
#include <rte_debug.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250

typedef struct {
    uint16_t port_id;
    int dev_socket_id;
    struct rte_ether_addr mac_addr;
    struct rte_mempool* mempool; // pkt pool for this port
} app_port_t;

typedef struct {
    app_port_t ports[RTE_MAX_ETHPORTS];
    uint16_t nr_ports;
} app_config_t;

app_config_t app_cfg;

int port_init(uint16_t port_id, app_port_t* app_port) {
    int rc;
    int dev_socket_id;
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    struct rte_mempool* mempool;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_conf eth_cfg;
    struct rte_ether_addr mac_addr;

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

    printf("=== port[%hu] info:\n"
           "dev_socket_id: %d\n"
           "driver: %s\n"
           "min_mtu: %hu\n"
           "max_mtu: %hu\n"
           "max_rx_queues: %hu\n"
           "max_tx_queues: %hu\n"
           "max_rx_descs: %hu\n"
           "max_tx_descs: %hu\n"
           "rx_offload_capa: %08lx\n"
           "tx_offload_capa: %08lx\n",
           port_id,
           dev_socket_id,
           dev_info.driver_name,
           dev_info.min_mtu,
           dev_info.max_mtu,
           dev_info.max_rx_queues,
           dev_info.max_tx_queues,
           dev_info.rx_desc_lim.nb_max,
           dev_info.tx_desc_lim.nb_max,
           dev_info.rx_offload_capa,
           dev_info.tx_offload_capa);

    mempool = rte_pktmbuf_pool_create("balabala",
                                      NUM_MBUFS, MBUF_CACHE_SIZE,
                                      0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                      dev_socket_id);
    if (!mempool) {
        rte_panic("Fail to create mempool: port %hu, %s\n",
                  port_id, rte_strerror(rte_errno));
    }

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

    printf("MAC: " RTE_ETHER_ADDR_PRT_FMT "\n", RTE_ETHER_ADDR_BYTES(&mac_addr));

    app_port->port_id = port_id;
    app_port->dev_socket_id = dev_socket_id;
    rte_ether_addr_copy(&mac_addr, &app_port->mac_addr);
    app_port->mempool = mempool;

    return 0;
}

int main(int argc, char* argv[]) {
    int rc;
    unsigned nr_lcores;
    uint16_t nr_ports;

    rc = rte_eal_init(argc, argv);
    if (rc < 0) {
        rte_panic("Fail to init EAL: %s\n", rte_strerror(rte_errno));
    }

    nr_lcores = rte_lcore_count();
    printf("%u lcores available\n", nr_lcores);

    nr_ports = rte_eth_dev_count_avail();
    if (!nr_ports) {
        rte_exit(EXIT_FAILURE, "No available NICs!");
    }
    printf("%hu ports available\n", nr_ports);

    for (uint16_t i = 0; i != nr_ports; ++i) {
        rc = port_init(i, &app_cfg.ports[i]);
        if (rc < 0) {
            rte_exit(EXIT_FAILURE, "Fail to init port %hu\n", i);
        }
        ++app_cfg.nr_ports;
    }

    printf("Press any key to continue...\n");
    getchar();

    rc = rte_eal_cleanup();
    if (rc < 0) {
        rte_panic("Fail to cleanup EAL: %s\n", rte_strerror(rte_errno));
    }

    return 0;
}
