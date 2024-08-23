//===-- test.c ------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Test for Go runtime.
//
//===----------------------------------------------------------------------===//

#include <sys/mman.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

void __psan_init(void **thr, void **proc, void (*cb)(long, void*));
void __psan_fini();
void __psan_map_shadow(void *addr, unsigned long size);
void __psan_go_start(void *thr, void **chthr, void *pc);
void __psan_go_end(void *thr);
void __psan_proc_create(void **pproc);
void __psan_proc_destroy(void *proc);
void __psan_proc_wire(void *proc, void *thr);
void __psan_proc_unwire(void *proc, void *thr);
void __psan_read(void *thr, void *addr, void *pc);
void __psan_write(void *thr, void *addr, void *pc);
void __psan_func_enter(void *thr, void *pc);
void __psan_func_exit(void *thr);
void __psan_malloc(void *thr, void *pc, void *p, unsigned long sz);
void __psan_free(void *p, unsigned long sz);
void __psan_acquire(void *thr, void *addr);
void __psan_release(void *thr, void *addr);
void __psan_release_acquire(void *thr, void *addr);
void __psan_release_merge(void *thr, void *addr);

void *current_proc;

void symbolize_cb(long cmd, void *ctx) {
  switch (cmd) {
  case 0:
    if (current_proc == 0)
      abort();
    *(void**)ctx = current_proc;
  }
}

/*
 * See lib/psan/rtl/psan_platform.h for details of what the memory layout
 * of Go programs looks like.  To prevent running over existing mappings,
 * we pick an address slightly inside the Go heap region.
 */
void *go_heap = (void *)0xC011110000;
char *buf0;

void foobar() {}
void barfoo() {}

int main(void) {
  void *thr0 = 0;
  void *proc0 = 0;
  __psan_init(&thr0, &proc0, symbolize_cb);
  current_proc = proc0;

  // Allocate something resembling a heap in Go.
  buf0 = mmap(go_heap, 16384, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_FIXED | MAP_ANON, -1, 0);
  if (buf0 == MAP_FAILED) {
    fprintf(stderr, "failed to allocate Go-like heap at %p; errno %d\n",
            go_heap, errno);
    return 1;
  }
  char *buf = (char*)((unsigned long)buf0 + (64<<10) - 1 & ~((64<<10) - 1));
  __psan_map_shadow(buf, 4096);
  __psan_malloc(thr0, (char*)&barfoo + 1, buf, 10);
  __psan_free(buf, 10);
  __psan_func_enter(thr0, (char*)&main + 1);
  __psan_malloc(thr0, (char*)&barfoo + 1, buf, 10);
  __psan_release(thr0, buf);
  __psan_release_acquire(thr0, buf);
  __psan_release_merge(thr0, buf);
  void *thr1 = 0;
  __psan_go_start(thr0, &thr1, (char*)&barfoo + 1);
  void *thr2 = 0;
  __psan_go_start(thr0, &thr2, (char*)&barfoo + 1);
  __psan_func_exit(thr0);
  __psan_func_enter(thr1, (char*)&foobar + 1);
  __psan_func_enter(thr1, (char*)&foobar + 1);
  __psan_write(thr1, buf, (char*)&barfoo + 1);
  __psan_acquire(thr1, buf);
  __psan_func_exit(thr1);
  __psan_func_exit(thr1);
  __psan_go_end(thr1);
  void *proc1 = 0;
  __psan_proc_create(&proc1);
  current_proc = proc1;
  __psan_func_enter(thr2, (char*)&foobar + 1);
  __psan_read(thr2, buf, (char*)&barfoo + 1);
  __psan_free(buf, 10);
  __psan_func_exit(thr2);
  __psan_go_end(thr2);
  __psan_proc_destroy(proc1);
  current_proc = proc0;
  __psan_fini();
  return 0;
}
