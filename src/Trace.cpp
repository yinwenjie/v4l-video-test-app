/*
 **************************************************************************************************
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 **************************************************************************************************
*/

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>

#include "Trace.h"

#define TRACE_BACKTRACE_SIZE 128
#define TRACE_MSG_SIZE 2048
#define TRACE_SYMBOL_MAX 180
#define TRACE_HELPER_FRAMES 2

static std::atomic_bool gFailureTracePrinted = false;

static const char* signalName(int signalNum) {
    switch (signalNum) {
        case SIGSEGV:
            return "SIGSEGV";
        case SIGABRT:
            return "SIGABRT";
        case SIGBUS:
            return "SIGBUS";
        case SIGFPE:
            return "SIGFPE";
        case SIGILL:
            return "SIGILL";
        case SIGTRAP:
            return "SIGTRAP";
        default:
            return "UNKNOWN";
    }
}

static const char* signalCodeName(int signalNum, int signalCode) {
    switch (signalNum) {
        case SIGSEGV:
            switch (signalCode) {
                case SEGV_MAPERR:
                    return "SEGV_MAPERR(address not mapped)";
                case SEGV_ACCERR:
                    return "SEGV_ACCERR(invalid permissions)";
#ifdef SEGV_BNDERR
                case SEGV_BNDERR:
                    return "SEGV_BNDERR(bounds check failed)";
#endif
#ifdef SEGV_PKUERR
                case SEGV_PKUERR:
                    return "SEGV_PKUERR(protection key check failed)";
#endif
                default:
                    return "SEGV_UNKNOWN";
            }
        case SIGBUS:
            switch (signalCode) {
                case BUS_ADRALN:
                    return "BUS_ADRALN(invalid address alignment)";
                case BUS_ADRERR:
                    return "BUS_ADRERR(nonexistent physical address)";
                case BUS_OBJERR:
                    return "BUS_OBJERR(object-specific hardware error)";
                default:
                    return "BUS_UNKNOWN";
            }
        case SIGFPE:
            switch (signalCode) {
                case FPE_INTDIV:
                    return "FPE_INTDIV(integer divide by zero)";
                case FPE_INTOVF:
                    return "FPE_INTOVF(integer overflow)";
                case FPE_FLTDIV:
                    return "FPE_FLTDIV(floating-point divide by zero)";
                case FPE_FLTOVF:
                    return "FPE_FLTOVF(floating-point overflow)";
                case FPE_FLTUND:
                    return "FPE_FLTUND(floating-point underflow)";
                case FPE_FLTRES:
                    return "FPE_FLTRES(floating-point inexact result)";
                case FPE_FLTINV:
                    return "FPE_FLTINV(invalid floating-point operation)";
                case FPE_FLTSUB:
                    return "FPE_FLTSUB(subscript out of range)";
                default:
                    return "FPE_UNKNOWN";
            }
        case SIGILL:
            switch (signalCode) {
                case ILL_ILLOPC:
                    return "ILL_ILLOPC(illegal opcode)";
                case ILL_ILLOPN:
                    return "ILL_ILLOPN(illegal operand)";
                case ILL_ILLADR:
                    return "ILL_ILLADR(illegal addressing mode)";
                case ILL_ILLTRP:
                    return "ILL_ILLTRP(illegal trap)";
                case ILL_PRVOPC:
                    return "ILL_PRVOPC(privileged opcode)";
                case ILL_PRVREG:
                    return "ILL_PRVREG(privileged register)";
                case ILL_COPROC:
                    return "ILL_COPROC(coprocessor error)";
                case ILL_BADSTK:
                    return "ILL_BADSTK(internal stack error)";
                default:
                    return "ILL_UNKNOWN";
            }
        default:
            return "UNKNOWN";
    }
}

static void writeTruncated(int fd, const char* text, size_t maxLen) {
    if (text == nullptr) {
        return;
    }

    size_t len = strnlen(text, maxLen + 1);
    write(fd, text, len > maxLen ? maxLen : len);
    if (len > maxLen) {
        write(fd, "...", 3);
    }
}

static void writeAddressSymbol(int fd, int frame, void* address) {
    Dl_info info = {};
    char msg[TRACE_MSG_SIZE] = {0};

    if (dladdr(address, &info) && info.dli_sname != nullptr) {
        uintptr_t offset = reinterpret_cast<uintptr_t>(address) -
                           reinterpret_cast<uintptr_t>(info.dli_saddr);
        uintptr_t moduleOffset = reinterpret_cast<uintptr_t>(address) -
                                 reinterpret_cast<uintptr_t>(info.dli_fbase);
        int status = 0;
        char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        const char* symbol = status == 0 && demangled != nullptr ? demangled : info.dli_sname;
        int len = snprintf(msg, sizeof(msg), " #%02d ", frame);
        if (len > 0) {
            write(fd, msg, static_cast<size_t>(len));
        }
        writeTruncated(fd, symbol, TRACE_SYMBOL_MAX);
        len = snprintf(msg, sizeof(msg), "+0x%lx [%s+0x%lx]\n",
                       static_cast<unsigned long>(offset),
                       info.dli_fname != nullptr ? info.dli_fname : "unknown",
                       static_cast<unsigned long>(moduleOffset));
        free(demangled);
        if (len > 0) {
            write(fd, msg, static_cast<size_t>(len));
        }
        return;
    }

    int len = snprintf(msg, sizeof(msg), " #%02d %p\n", frame, address);
    if (len > 0) {
        write(fd, msg, static_cast<size_t>(len));
    }
}

static void writeCrashRegisters(int fd, ucontext_t* context) {
#if defined(__aarch64__)
    char msg[TRACE_MSG_SIZE] = {0};
    unsigned long pc = static_cast<unsigned long>(context->uc_mcontext.pc);
    unsigned long lr = static_cast<unsigned long>(context->uc_mcontext.regs[30]);
    unsigned long sp = static_cast<unsigned long>(context->uc_mcontext.sp);
    int len = snprintf(msg, sizeof(msg), "pc : 0x%lx\nlr : 0x%lx\nsp : 0x%lx\n",
                       pc, lr, sp);
    if (len > 0) {
        write(fd, msg, static_cast<size_t>(len));
    }
#elif defined(__x86_64__)
    char msg[TRACE_MSG_SIZE] = {0};
    unsigned long rip =
        static_cast<unsigned long>(context->uc_mcontext.gregs[REG_RIP]);
    unsigned long rsp =
        static_cast<unsigned long>(context->uc_mcontext.gregs[REG_RSP]);
    unsigned long rbp =
        static_cast<unsigned long>(context->uc_mcontext.gregs[REG_RBP]);
    int len = snprintf(msg, sizeof(msg), "rip: 0x%lx\nrsp: 0x%lx\nrbp: 0x%lx\n",
                       rip, rsp, rbp);
    if (len > 0) {
        write(fd, msg, static_cast<size_t>(len));
    }
#else
    (void)fd;
    (void)context;
#endif
}

static void writeCrashTraceToFd(int fd, int signalNum, siginfo_t* info,
                                ucontext_t* context) {
    char msg[TRACE_MSG_SIZE] = {0};
    void* buffer[TRACE_BACKTRACE_SIZE];
    int signalCode = info != nullptr ? info->si_code : 0;
    int len = snprintf(msg, sizeof(msg),
                       "\nCrash signal: %d (%s), code: %d (%s), pid: %ld, fault addr: %p\n",
                       signalNum, signalName(signalNum),
                       signalCode, signalCodeName(signalNum, signalCode),
                       static_cast<long>(getpid()),
                       info != nullptr ? info->si_addr : nullptr);

    if (len > 0) {
        write(fd, msg, static_cast<size_t>(len));
    }

    if (context != nullptr) {
        writeCrashRegisters(fd, context);
    }

    len = snprintf(msg, sizeof(msg), "Call trace:\n");
    if (len > 0) {
        write(fd, msg, static_cast<size_t>(len));
    }

    int nptrs = backtrace(buffer, TRACE_BACKTRACE_SIZE);
    for (int i = 0; i < nptrs; i++) {
        writeAddressSymbol(fd, i, buffer[i]);
    }

    len = snprintf(msg, sizeof(msg), "End of backtrace.\n\n");
    if (len > 0) {
        write(fd, msg, static_cast<size_t>(len));
    }
}

void PrintCrashTrace(int signalNum, siginfo_t* info, void* context) {
    char backTraceName[TRACE_MSG_SIZE] = {0};
    ucontext_t* ucontext = static_cast<ucontext_t*>(context);
    snprintf(backTraceName, sizeof(backTraceName), "./back_trace_%ld.log",
             static_cast<long>(getpid()));

    writeCrashTraceToFd(STDERR_FILENO, signalNum, info, ucontext);

    int fd = open(backTraceName, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd >= 0) {
        writeCrashTraceToFd(fd, signalNum, info, ucontext);
        close(fd);
    }
}

static void PrintCurrentTraceToFd(int fd, const char* reason) {
    char msg[TRACE_MSG_SIZE] = {0};
    void* buffer[TRACE_BACKTRACE_SIZE];
    int len = snprintf(msg, sizeof(msg), "\nFailure trace: %s\nCall trace:\n",
                       reason != nullptr ? reason : "unknown");

    if (len > 0) {
        write(fd, msg, static_cast<size_t>(len));
    }

    int nptrs = backtrace(buffer, TRACE_BACKTRACE_SIZE);
    for (int i = TRACE_HELPER_FRAMES; i < nptrs; i++) {
        writeAddressSymbol(fd, i - TRACE_HELPER_FRAMES, buffer[i]);
    }

    len = snprintf(msg, sizeof(msg), "End of failure trace.\n\n");
    if (len > 0) {
        write(fd, msg, static_cast<size_t>(len));
    }
}

void PrintCurrentTrace(const char* reason) {
    char backTraceName[TRACE_MSG_SIZE] = {0};
    snprintf(backTraceName, sizeof(backTraceName), "./back_trace_%ld.log",
             static_cast<long>(getpid()));

    if (gFailureTracePrinted.exchange(true)) {
        return;
    }

    PrintCurrentTraceToFd(STDERR_FILENO, reason);

    int fd = open(backTraceName, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd >= 0) {
        PrintCurrentTraceToFd(fd, reason);
        close(fd);
    }
}

bool IsFailureTracePrinted() {
    return gFailureTracePrinted;
}

void ResetFailureTraceFlag() {
    gFailureTracePrinted = false;
}
