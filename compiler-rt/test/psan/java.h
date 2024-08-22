#ifndef __PSAN_JAVA_H__
#define __PSAN_JAVA_H__

#include "test.h"

extern "C" {
typedef unsigned long jptr;
void __psan_java_preinit(const char *libjvm_path);
void __psan_java_init(jptr heap_begin, jptr heap_size);
int  __psan_java_fini();
void __psan_java_alloc(jptr ptr, jptr size);
void __psan_java_free(jptr ptr, jptr size);
jptr __psan_java_find(jptr *from_ptr, jptr to);
void __psan_java_move(jptr src, jptr dst, jptr size);
void __psan_java_finalize();
void __psan_java_mutex_lock(jptr addr);
void __psan_java_mutex_unlock(jptr addr);
void __psan_java_mutex_read_lock(jptr addr);
void __psan_java_mutex_read_unlock(jptr addr);
void __psan_java_mutex_lock_rec(jptr addr, int rec);
int  __psan_java_mutex_unlock_rec(jptr addr);
int  __psan_java_acquire(jptr addr);
int  __psan_java_release(jptr addr);
int  __psan_java_release_store(jptr addr);

void __psan_read1_pc(jptr addr, jptr pc);
void __psan_write1_pc(jptr addr, jptr pc);
void __psan_func_entry(jptr pc);
void __psan_func_exit();
}

const jptr kExternalPCBit = 1ULL << 60;

#endif // __PSAN_JAVA_H__