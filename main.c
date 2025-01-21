//
//  main.c
//  bridge-dpdk
//
//  Created by zhanwang-sky on 2025/1/19.
//

#include <stdio.h>

#include <rte_debug.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>

static int lcore_hello(__rte_unused void* arg) {
    unsigned lcore_id;
    lcore_id = rte_lcore_id();
    printf("hello from core %u\n", lcore_id);
    return 0;
}

int main(int argc, char* argv[]) {
    int rc;
    unsigned lcore_id;
    uint16_t nr_ports;
    struct rte_eth_dev_info dev_info;
    struct rte_ether_addr mac_addr;
    char mac_addr_str[RTE_ETHER_ADDR_FMT_SIZE];

    rc = rte_eal_init(argc, argv);
    if (rc < 0) {
        rte_panic("Cannot init EAL: %s\n", rte_strerror(rte_errno));
    }

    nr_ports = rte_eth_dev_count_avail();
    printf("%hu ports available\n", nr_ports);

    for (uint16_t i = 0; i < nr_ports; ++i) {
        rc = rte_eth_dev_info_get(i, &dev_info);
        if (rc < 0) {
            rte_panic("Error getting port[%hu] info: %s\n",
                      i, rte_strerror(-rc));
        }
        printf("port[%hu] rx_desc_max: %hu, tx_desc_max: %hu\n",
               i, dev_info.rx_desc_lim.nb_max, dev_info.tx_desc_lim.nb_max);

        // XXX TODO: rte_eth_dev_start()

        rc = rte_eth_macaddr_get(i, &mac_addr);
        if (rc < 0) {
            rte_panic("Error getting port[%hu] MAC addr: %s\n",
                      i, rte_strerror(-rc));
        }
        rte_ether_format_addr(mac_addr_str, sizeof(mac_addr_str), &mac_addr);
        printf("port[%hu] MAC_addr: %s\n", i, mac_addr_str);
    }

    lcore_id = rte_lcore_count();
    printf("%u lcores are available\n", lcore_id);

    lcore_id = rte_get_main_lcore();
    printf("main lcore id: %u\n", lcore_id);

    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        rte_eal_remote_launch(lcore_hello, NULL, lcore_id);
    }

    lcore_id = rte_lcore_id();
    printf("hello from core %u\n", lcore_id);

    rte_eal_mp_wait_lcore();

    rte_eal_cleanup();

    return 0;
}
