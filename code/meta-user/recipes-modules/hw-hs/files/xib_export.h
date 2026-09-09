#ifndef XIB_EXPORT_H
#define XIB_EXPORT_H
#include <linux/types.h>
#include <rdma/ib_verbs.h>

void *xib_alloc_mem(char *from, size_t len, u64 *pa);
u64 xib_alloc_eddr_mem(char *from, size_t len, u64 *pa);
int xib_dealloc_mem(u64 len, void *va, u64 pa);
struct ib_mr *xib_reg_mr(struct ib_qp *ibqp, u64 va, u64 len, u64 *pa,
		struct ib_pd *pd, int umem_pgs);
struct ib_mr *xib_alloc_mr(struct ib_pd *ibpd,
				enum ib_mr_type mr_type,
				u32 max_num_sg);
int xib_map_mr_sge(struct ib_mr *ibmr, struct scatterlist *sg, int sg_nents,
			 unsigned int *sg_offset);
dma_addr_t get_qp_sq_ba(u32 qp_num);
void *get_xrnic_local(void);
void *get_xib_ptr(void);
u32 get_user_sq_depth(u32 hw_qpn);
void *xib_alloc_coherent(char *from, void *xib,
                size_t len, u64 *dma_handle, gfp_t flags);
void xib_free_coherent(void *xib,
                 u64 size, void *cpu_addr, u64 dma_handle);
int xib_kdereg_mr(struct ib_mr *mr);
#endif
