#ifndef ION_MEM_POOL_H
#define ION_MEM_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

void* IMPoolPalloc(int nSize);
void IMPoolPfree(void* pMem);
void IMPoolFlushCache(void* pMem, int nSize);

#ifdef __cplusplus
}
#endif


#endif
