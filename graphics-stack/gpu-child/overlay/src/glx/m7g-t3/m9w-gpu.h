#pragma once
#include "m7g_t3_transport.h"
typedef void (*m9w_proc)(void);
typedef m9w_proc (*m9w_getproc)(const char *);
typedef struct m9w_gpu m9w_gpu;
struct m9w_slot_export {
    int memory_fd, ready_fd, returned_fd;
    unsigned bytes, dedicated;
    uint64_t allocation_size;
    unsigned char device_uuid[16], driver_uuid[16];
};
int m9w_xport_export(m7g_t3_xport *,unsigned,struct m9w_slot_export *);
int m9w_xport_commit(m7g_t3_xport *,unsigned,unsigned);
int m9w_xport_idle(m7g_t3_xport *,unsigned);
int m9w_xport_returned_fd(m7g_t3_xport *,unsigned);
int m9w_gpu_init(m9w_gpu **,m7g_t3_xport *,m9w_getproc,void *);
int m9w_gpu_write(m9w_gpu *,unsigned,void *);
int m9w_gpu_write_fb(m9w_gpu *,unsigned,void *,unsigned,unsigned);
int m9w_gpu_destroy(m9w_gpu *,void *);
int m9w_gpu_drain(m9w_gpu *,void *);
void m9w_log(const char *,...);
int m9w_gpu_owned(m9w_gpu *,void *);
void m9w_before_unbind(void *);
void m9w_before_switch(void *,int);

int m10k_xport_reassert(m7g_t3_xport *);
int m10k_xport_release_gate(m7g_t3_xport *,unsigned);
