/* Negative compile fixture, never used in a runtime build. Derive all field
 * types from the real external table, but exchange its first two slots. */
#define ScMemOpsS k2b_original_memops
#include_next <sc_interface.h>
#undef ScMemOpsS
#define K2B_FIELD(name) __typeof__(((struct k2b_original_memops *)0)->name) name
struct ScMemOpsS {
    K2B_FIELD(open2);
    K2B_FIELD(open);
    K2B_FIELD(close);
    K2B_FIELD(total_size);
    K2B_FIELD(palloc);
    K2B_FIELD(palloc_no_cache);
    K2B_FIELD(pfree);
    K2B_FIELD(flush_cache);
    K2B_FIELD(ve_get_phyaddr);
    K2B_FIELD(ve_get_viraddr);
    K2B_FIELD(cpu_get_phyaddr);
    K2B_FIELD(cpu_get_viraddr);
    K2B_FIELD(mem_set);
    K2B_FIELD(mem_cpy);
    K2B_FIELD(mem_read);
    K2B_FIELD(mem_write);
    K2B_FIELD(setup);
    K2B_FIELD(shutdown);
    K2B_FIELD(get_ve_addr_offset);
    K2B_FIELD(get_debug_info);
    K2B_FIELD(get_vir_by_fd);
    K2B_FIELD(get_phy_by_fd);
    K2B_FIELD(free_phy_by_fd);
    K2B_FIELD(get_fd_by_vir);
};
#undef K2B_FIELD
