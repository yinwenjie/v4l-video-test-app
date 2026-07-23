/*
 **************************************************************************************************
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 **************************************************************************************************
*/

#ifndef _TRACE_H_
#define _TRACE_H_

#include <signal.h>

void PrintCrashTrace(int signalNum, siginfo_t* info, void* context);
void PrintCurrentTrace(const char* reason);
bool IsFailureTracePrinted();
void ResetFailureTraceFlag();

#define TRACE_RETURN_IF_ERROR(ret, reason) \
    do {                                   \
        if ((ret) != 0) {                  \
            PrintCurrentTrace(reason);     \
            return (ret);                  \
        }                                  \
    } while (0)

#define TRACE_RECORD_IF_ERROR(ret, reason) \
    do {                                   \
        if ((ret) != 0) {                  \
            PrintCurrentTrace(reason);     \
        }                                  \
    } while (0)

#endif  // _TRACE_H_
