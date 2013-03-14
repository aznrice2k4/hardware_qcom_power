/*
 * Copyright (C) 2012 The Android Open Source Project
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
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <dlfcn.h>

#define LOG_TAG "PowerHAL"
#include <utils/Log.h>

#include <hardware/hardware.h>
#include <hardware/power.h>
#define TOUCHBOOST_SOCKET       "/dev/socket/mpdecision/touchboost"

#define SCALINGMAXFREQ_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"
#define SCALING_GOVERNOR_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
#define BOOSTPULSE_INTERACTIVE "/sys/devices/system/cpu/cpufreq/interactive/boostpulse"
#define BOOSTPULSE_INTELLIDEMAND "/sys/devices/system/cpu/cpufreq/intellidemand/boostpulse"
#define SAMPLING_RATE_INTELLIDEMAND "/sys/devices/system/cpu/cpufreq/intellidemand/sampling_rate"
#define SAMPLING_RATE_SCREEN_ON "50000"
#define SAMPLING_RATE_SCREEN_OFF "500000"

#define MAX_BUF_SZ  80

/* initialize to something safe */
static char scaling_max_freq[MAX_BUF_SZ] = "1512000";

static int client_sockfd;
static struct sockaddr_un client_addr;

struct krait_power_module {
    struct power_module base;
};

static void sysfs_write(char *path, char *s)
{
    char buf[MAX_BUF_SZ];
    int len;
    int fd = open(path, O_WRONLY);

    if (fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", path, buf);
        return;
    }

    len = write(fd, s, strlen(s));
    if (len < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s\n", path, buf);
    }
    close(fd);
}

int sysfs_read(const char *path, char *buf, size_t size)
{
    int fd, len;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    do {
        len = read(fd, buf, size);
    } while (len < 0 && errno == EINTR);

    close(fd);

    return len;
}

static int get_scaling_governor(char governor[], int size)
{
    if (sysfs_read(SCALING_GOVERNOR_PATH, governor, size) < 0) {
        // Can't obtain the scaling governor. Return.
        return -1;
    } else {
        // Strip newline at the end.
        int len = strlen(governor);

        len--;

        while (len >= 0 && (governor[len] == '\n' || governor[len] == '\r'))
            governor[len--] = '\0';
    }

    return 0;
}

static void krait_power_init(struct power_module *module)
{
    char governor[MAX_BUF_SZ];

    if (get_scaling_governor(governor, sizeof(governor)) < 0) {
        ALOGE("Can't read scaling governor.");
    } else {
        if (strncmp(governor, "intellidemand", 13) == 0) {
            sysfs_write(SAMPLING_RATE_INTELLIDEMAND,
                SAMPLING_RATE_SCREEN_ON);
        }
    }

    ALOGI("%s", __func__);
    client_sockfd = socket(PF_UNIX, SOCK_DGRAM, 0);
    if (client_sockfd < 0) {
        ALOGE("%s: failed to open: %s", __func__, strerror(errno));
        return;
    }
    memset(&client_addr, 0, sizeof(struct sockaddr_un));
    client_addr.sun_family = AF_UNIX;
    snprintf(client_addr.sun_path, UNIX_PATH_MAX, TOUCHBOOST_SOCKET);
}

static void touch_boost()
{
    int rc;

    if (client_sockfd < 0) {
        ALOGE("%s: touchboost socket not created", __func__);
        return;
    }

    rc = sendto(client_sockfd, "1", 1, 0,
	(const struct sockaddr *)&client_addr, sizeof(struct sockaddr_un));
    /* get rid of logcat spam when mpdecision is off */
    //if (rc < 0) {
        //ALOGE("%s: failed to send: %s", __func__, strerror(errno));
    //}
}

static void krait_power_set_interactive(struct power_module *module, int on)
{
    char governor[MAX_BUF_SZ];

    ALOGV("%s %s", __func__, (on ? "ON" : "OFF"));
    if (on)
        touch_boost();

    if (get_scaling_governor(governor, sizeof(governor)) < 0) {
        ALOGE("Can't read scaling governor.");
    } else {
        if (strncmp(governor, "intellidemand", 13) == 0) {
            sysfs_write(SAMPLING_RATE_INTELLIDEMAND,
                on ? SAMPLING_RATE_SCREEN_ON : SAMPLING_RATE_SCREEN_OFF);
        }
    }
}

static void krait_power_hint(struct power_module *module, power_hint_t hint,
                       void *data) {
    struct krait_power_module *krait = (struct krait_power_module *) module;
    char governor[MAX_BUF_SZ];

    switch (hint) {
        case POWER_HINT_INTERACTION:
            ALOGV("POWER_HINT_INTERACTION");
            // interactivei & intellidemand governors

            if (get_scaling_governor(governor, sizeof(governor)) < 0) {
                ALOGE("Can't read scaling governor.");
            } else {
                if (strncmp(governor, "interactive", 11) == 0) {
                    sysfs_write(BOOSTPULSE_INTERACTIVE, "1");
                    //ALOGI("interactive boost!");
                } else if (strncmp(governor, "intellidemand", 13) == 0) {
                    sysfs_write(BOOSTPULSE_INTELLIDEMAND, "1");
                    //ALOGI("intellidemand boost!");
                }
            }
            // mpdecision
            touch_boost();
            break;
#if 0
        case POWER_HINT_VSYNC:
            ALOGV("POWER_HINT_VSYNC %s", (data ? "ON" : "OFF"));
            break;
#endif
        default:
             break;
    }
}

static struct hw_module_methods_t power_module_methods = {
    .open = NULL,
};

struct krait_power_module HAL_MODULE_INFO_SYM = {
    base: {
        common: {
            tag: HARDWARE_MODULE_TAG,
            module_api_version: POWER_MODULE_API_VERSION_0_2,
            hal_api_version: HARDWARE_HAL_API_VERSION,
            id: POWER_HARDWARE_MODULE_ID,
            name: "Qualcomm Power HAL (by faux123)",
            author: "The Android Open Source Project",
            methods: &power_module_methods,
        },

        init: krait_power_init,
        setInteractive: krait_power_set_interactive,
        powerHint: krait_power_hint,
    },
};
