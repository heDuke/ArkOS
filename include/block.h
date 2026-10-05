#ifndef ARK_BLOCK_H
#define ARK_BLOCK_H
#include "ark.h"
typedef struct {
    unsigned id;
    bool present;
    uint64_t sectors;
} BlockDevice;
typedef struct {
    uint64_t read_bytes, write_bytes, read_operations, write_operations;
} BlockStats;
void block_statistics(BlockStats *out);
void block_init(void);
BlockDevice *block_device(unsigned id);
bool block_read(BlockDevice *device, uint64_t lba, uint32_t sectors, void *buffer);
bool block_write(BlockDevice *device, uint64_t lba, uint32_t sectors, const void *buffer);
bool block_flush(BlockDevice *device);
const char *block_error(void);
/* Loadable .arco storage: one module may own a BlockDevice slot. */
int block_bind_ops(const void *ops, unsigned owner);
void block_unbind_ops(unsigned owner);
#endif
