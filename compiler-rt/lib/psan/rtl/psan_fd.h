//===-- psan_fd.h -----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PredictiveSanitizer (PSan), a predictive race detector.
// This is a fork of ThreadSanitizer (TSan) at LLVM commit
// c609043dd00955bf177ff57b0bad2a87c1e61a36.
//
// This file handles synchronization via IO.
// People use IO for synchronization along the lines of:
//
// int X;
// int client_socket;  // initialized elsewhere
// int server_socket;  // initialized elsewhere
//
// Thread 1:
// X = 42;
// send(client_socket, ...);
//
// Thread 2:
// if (recv(server_socket, ...) > 0)
//   assert(X == 42);
//
// This file determines the scope of the file descriptor (pipe, socket,
// all local files, etc) and executes acquire and release operations on
// the scope as necessary.  Some scopes are very fine grained (e.g. pipe
// operations synchronize only with operations on the same pipe), while
// others are corse-grained (e.g. all operations on local files synchronize
// with each other).
//===----------------------------------------------------------------------===//
#ifndef PSAN_FD_H
#define PSAN_FD_H

#include "psan_rtl.h"

namespace __psan {

void FdInit();
void FdAcquire(ThreadState *thr, uptr pc, int fd);
void FdRelease(ThreadState *thr, uptr pc, int fd);
void FdAccess(ThreadState *thr, uptr pc, int fd);
void FdClose(ThreadState *thr, uptr pc, int fd, bool write = true);
void FdFileCreate(ThreadState *thr, uptr pc, int fd);
void FdDup(ThreadState *thr, uptr pc, int oldfd, int newfd, bool write);
void FdPipeCreate(ThreadState *thr, uptr pc, int rfd, int wfd);
void FdEventCreate(ThreadState *thr, uptr pc, int fd);
void FdSignalCreate(ThreadState *thr, uptr pc, int fd);
void FdInotifyCreate(ThreadState *thr, uptr pc, int fd);
void FdPollCreate(ThreadState *thr, uptr pc, int fd);
void FdPollAdd(ThreadState *thr, uptr pc, int epfd, int fd);
void FdSocketCreate(ThreadState *thr, uptr pc, int fd);
void FdSocketAccept(ThreadState *thr, uptr pc, int fd, int newfd);
void FdSocketConnecting(ThreadState *thr, uptr pc, int fd);
void FdSocketConnect(ThreadState *thr, uptr pc, int fd);
bool FdLocation(uptr addr, int *fd, Tid *tid, StackID *stack, bool *closed);
void FdOnFork(ThreadState *thr, uptr pc);

uptr File2addr(const char *path);
uptr Dir2addr(const char *path);

}  // namespace __psan

#endif  // PSAN_INTERFACE_H
