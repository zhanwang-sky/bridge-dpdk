//
//  main.c
//  bridge-dpdk
//
//  Created by zhanwang-sky on 2025/1/19.
//

#include <stdio.h>

#include <rte_eal.h>
#include <rte_debug.h>
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

    rc = rte_eal_init(argc, argv);
    if (rc < 0) {
        rte_panic("Cannot init EAL\n");
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
