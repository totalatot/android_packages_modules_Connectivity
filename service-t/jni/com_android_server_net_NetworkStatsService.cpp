/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "NetworkStatsNative"

#include <cutils/qtaguid.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <jni.h>
#include <nativehelper/ScopedUtfChars.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <utils/Log.h>
#include <utils/misc.h>

#include "bpf/BpfUtils.h"
#include "netdbpf/BpfNetworkStats.h"
#include "netdbpf/NetworkTraceHandler.h"

using android::bpf::bpfGetUidStats;
using android::bpf::bpfGetIfaceStats;
using android::bpf::bpfGetIfIndexStats;
using android::bpf::NetworkTraceHandler;

namespace android {

static const char* QTAGUID_IFACE_STATS = "/proc/net/xt_qtaguid/iface_stat_fmt";
static const char* QTAGUID_UID_STATS = "/proc/net/xt_qtaguid/stats";

// NOTE: keep these in sync with TrafficStats.java
static const uint64_t UNKNOWN = -1;

enum StatsType {
    RX_BYTES = 0,
    RX_PACKETS = 1,
    TX_BYTES = 2,
    TX_PACKETS = 3,
};

static uint64_t getStatsType(StatsValue* stats, StatsType type) {
    switch (type) {
        case RX_BYTES:
            return stats->rxBytes;
        case RX_PACKETS:
            return stats->rxPackets;
        case TX_BYTES:
            return stats->txBytes;
        case TX_PACKETS:
            return stats->txPackets;
        default:
            return UNKNOWN;
    }
}

static int parseIfaceStats(const char* iface, StatsValue* stats) {
    FILE *fp = fopen(QTAGUID_IFACE_STATS, "r");
    if (fp == NULL) {
        return -1;
    }

    char buffer[384];
    char cur_iface[32];
    uint64_t rxBytes, rxPackets, txBytes, txPackets, tcpRxPackets, tcpTxPackets;

    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        int matched = sscanf(buffer, "%31s %" SCNu64 " %" SCNu64 " %" SCNu64
                " %" SCNu64 " " "%*u %" SCNu64 " %*u %*u %*u %*u "
                "%*u %" SCNu64 " %*u %*u %*u %*u", cur_iface, &rxBytes,
                &rxPackets, &txBytes, &txPackets, &tcpRxPackets, &tcpTxPackets);
        if (matched >= 5) {
            if (!iface || !strcmp(iface, cur_iface)) {
                stats->rxBytes += rxBytes;
                stats->rxPackets += rxPackets;
                stats->txBytes += txBytes;
                stats->txPackets += txPackets;
            }
        }
    }

    if (fclose(fp) != 0) {
        return -1;
    }
    return 0;
}

static int parseUidStats(const uint32_t uid, StatsValue* stats) {
    FILE *fp = fopen(QTAGUID_UID_STATS, "r");
    if (fp == NULL) {
        return -1;
    }

    char buffer[384];
    char iface[32];
    uint32_t idx, cur_uid, set;
    uint64_t tag, rxBytes, rxPackets, txBytes, txPackets;

    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        if (sscanf(buffer,
                "%" SCNu32 " %31s 0x%" SCNx64 " %u %u %" SCNu64 " %" SCNu64
                " %" SCNu64 " %" SCNu64 "",
                &idx, iface, &tag, &cur_uid, &set, &rxBytes, &rxPackets,
                &txBytes, &txPackets) == 9) {
            if (uid == cur_uid && tag == 0L) {
                stats->rxBytes += rxBytes;
                stats->rxPackets += rxPackets;
                stats->txBytes += txBytes;
                stats->txPackets += txPackets;
            }
        }
    }

    if (fclose(fp) != 0) {
        return -1;
    }
    return 0;
}

static jlong nativeGetTotalStat(JNIEnv* env, jclass clazz, jint type) {
    StatsValue stats = {};

    if (bpfGetIfaceStats(NULL, &stats) == 0) {
        return getStatsType(&stats, (StatsType) type);
    } else {
        if (parseIfaceStats(NULL, &stats) == 0) {
            return getStatsType(&stats, (StatsType) type);
        } else {
            return UNKNOWN;
        }
    }
}

static jlong nativeGetIfaceStat(JNIEnv* env, jclass clazz, jstring iface, jint type) {
    ScopedUtfChars iface8(env, iface);
    if (iface8.c_str() == NULL) {
        return UNKNOWN;
    }

    StatsValue stats = {};

    if (bpfGetIfaceStats(iface8.c_str(), &stats) == 0) {
        return getStatsType(&stats, (StatsType) type);
    } else {
        if (parseIfaceStats(iface8.c_str(), &stats) == 0) {
            return getStatsType(&stats, (StatsType) type);
        } else {
            return UNKNOWN;
        }
    }
}

static jlong nativeGetIfIndexStat(JNIEnv* env, jclass clazz, jint ifindex, jint type) {
    StatsValue stats = {};
    if (bpfGetIfIndexStats(ifindex, &stats) == 0) {
        return getStatsType(&stats, (StatsType) type);
    } else {
        return UNKNOWN;
    }
}

static jlong nativeGetUidStat(JNIEnv* env, jclass clazz, jint uid, jint type) {
    StatsValue stats = {};

    if (bpfGetUidStats(uid, &stats) == 0) {
        return getStatsType(&stats, (StatsType) type);
    } else {
        if (parseUidStats(uid, &stats) == 0) {
            return getStatsType(&stats, (StatsType) type);
        } else {
            return UNKNOWN;
        }
    }
}

static void nativeInitNetworkTracing(JNIEnv* env, jclass clazz) {
    NetworkTraceHandler::InitPerfettoTracing();
}

static const JNINativeMethod gMethods[] = {
        {"nativeGetTotalStat", "(I)J", (void*)nativeGetTotalStat},
        {"nativeGetIfaceStat", "(Ljava/lang/String;I)J", (void*)nativeGetIfaceStat},
        {"nativeGetIfIndexStat", "(II)J", (void*)nativeGetIfIndexStat},
        {"nativeGetUidStat", "(II)J", (void*)nativeGetUidStat},
        {"nativeInitNetworkTracing", "()V", (void*)nativeInitNetworkTracing},
};

int register_android_server_net_NetworkStatsService(JNIEnv* env) {
    return jniRegisterNativeMethods(env,
            "android/net/connectivity/com/android/server/net/NetworkStatsService", gMethods,
            NELEM(gMethods));
}

}
