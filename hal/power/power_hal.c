/*
 * Copyright (c) 2014 The Android Open Source Project
 * Copyright (c) 2014 Andreas Schneider <asn@cryptomilk.org>
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

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/time.h>
#include <stdbool.h>

#define LOG_TAG "Exynos5430PowerHAL"
/* #define LOG_NDEBUG 0 */
#include <utils/Log.h>

#include <hardware/hardware.h>
#include <hardware/power.h>

#define NSEC_PER_SEC 1000000000
#define USEC_PER_SEC 1000000
#define NSEC_PER_USEC 100

#define POWERHAL_STRINGIFY(s) POWERHAL_TOSTRING(s)
#define POWERHAL_TOSTRING(s) #s

#define BOOSTPULSE_PATH "/sys/devices/system/cpu/cpu0/cpufreq/interactive/boostpulse"

#define BOOST_PULSE_DURATION 400000
#define BOOST_PULSE_DURATION_STR POWERHAL_STRINGIFY(BOOST_PULSE_DURATION)

#define BOOST_CPU0_PATH "/sys/devices/system/cpu/cpu0/cpufreq/interactive/boost"
#define BOOST_CPU4_PATH "/sys/devices/system/cpu/cpu4/cpufreq/interactive/boost"

#define CPU0_MAX_FREQ_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"
#define CPU4_MAX_FREQ_PATH "/sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq"

#define TOUCHSCREEN_PATH "/sys/class/sec/tsp/input/enabled"
#define TOUCHKEY_PATH "/sys/class/sec/sec_touchkey/input/enabled"

struct exynos5430_power_module {
    struct power_module base;
    pthread_mutex_t lock;
    int boostpulse_fd;
    int boostpulse_warned;
    const char *gpio_keys_power_path;
    const char *touchscreen_power_path;
    const char *touchkey_power_path;
};

/* POWER_HINT_INTERACTION and POWER_HINT_VSYNC */
static unsigned int vsync_count;
static struct timespec last_touch_boost;
static bool touch_boost;

static void sysfs_write(const char *path, char *s)
{
    char errno_str[64];
    int len;
    int fd;

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        strerror_r(errno, errno_str, sizeof(errno_str));
        ALOGE("Error opening %s: %s\n", path, errno_str);
        return;
    }

    len = write(fd, s, strlen(s));
    if (len < 0) {
        strerror_r(errno, errno_str, sizeof(errno_str));
        ALOGE("Error writing to %s: %s\n", path, errno_str);
    }

    close(fd);
}

static void init_touchscreen_power_path(struct exynos5430_power_module *exynos5430_pwr)
{
    exynos5430_pwr->touchscreen_power_path = TOUCHSCREEN_PATH;
}

static void init_touchkey_power_path(struct exynos5430_power_module *exynos5430_pwr)
{
    exynos5430_pwr->touchkey_power_path = TOUCHKEY_PATH;
}

static void init_gpio_keys_power_path(struct exynos5430_power_module *exynos5430_pwr)
{
    const char filename[] = "enabled";
    char dir[1024] = { 0 };
    struct dirent *de;
    size_t pathsize;
    char errno_str[64];
    char *path;
    DIR *d = NULL;
    uint32_t i;

    for (i = 0; i < 20; i++) {

        snprintf(dir, sizeof(dir), "/sys/devices/gpio_keys.%d/input", i);
        d = opendir(dir);
        if (d != NULL) {
            break;
        }
    }

    if (d == NULL) {
        strerror_r(errno, errno_str, sizeof(errno_str));
        ALOGE("Error finding gpio_keys directory %s: %s\n", dir, errno_str);
        return;
    }

    while ((de = readdir(d)) != NULL) {
        if (strncmp("input", de->d_name, 5) == 0) {
            pathsize = strlen(dir) + strlen(de->d_name) + sizeof(filename) + 2;

            path = malloc(pathsize);
            if (path == NULL) {
                strerror_r(errno, errno_str, sizeof(errno_str));
                ALOGE("Out of memory: %s\n", errno_str);
                return;
            }

            snprintf(path, pathsize, "%s/%s/%s", dir, de->d_name, filename);

            exynos5430_pwr->gpio_keys_power_path = path;

            goto done;
        }
    }
    ALOGE("Error failed to find input dir in %s\n", dir);
done:
    closedir(d);
}

/*
 * This function performs power management setup actions at runtime startup,
 * such as to set default cpufreq parameters.  This is called only by the Power
 * HAL instance loaded by PowerManagerService.
 */
static void exynos5430_power_init(struct power_module *module)
{
    struct exynos5430_power_module *exynos5430_pwr = (struct exynos5430_power_module *) module;
    struct stat sb;
    int rc;

    /*
     * You get the values reading the hexdump from the biniary power module or
     * strace
     */
    sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/interactive/multi_enter_load", "800");
    sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/interactive/single_enter_load", "200");
    sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/interactive/param_index", "0");

    sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/interactive/timer_rate", "20000");
    sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/interactive/timer_slack", "20000");
    sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/interactive/min_sample_time", "40000");
    sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/interactive/hispeed_freq", "1000000");
    sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/interactive/go_hispeed_load", "84");
    sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/interactive/target_loads", "75");
    sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/interactive/above_hispeed_delay", "39000");

    sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/interactive/boostpulse_duration",
                BOOST_PULSE_DURATION_STR);

    /* The CPU might not be turned on, then it doesn't make sense to configure it. */
    rc = stat("/sys/devices/system/cpu/cpu4/cpufreq/interactive", &sb);
    if (rc < 0) {
        ALOGE("CPU2 is offline, skip init\n");
        goto out;
    }

    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/multi_enter_load", "360");
    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/multi_enter_time", "99000");

    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/multi_exit_load", "240");
    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/multi_exit_time", "299000");

    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/single_enter_load", "95");
    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/single_enter_time", "199000");

    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/single_exit_load", "60");
    /* was emtpy in hex so a value already defined. */
    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/single_exit_time", "299000");

    /* was emtpy in hex so a value already defined. */
    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/param_index", "0");

    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/timer_rate", "20000");
    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/timer_slack", "20000");

    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/min_sample_time", "40000");
    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/hispeed_freq", "1000000");

    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/go_hispeed_load", "89");
    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/target_loads", "80 1000000:82 1200000:85 1500000:90");

    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/above_hispeed_delay", "79000 1200000:119000 1700000:19000");

    sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/interactive/boostpulse_duration",
                BOOST_PULSE_DURATION_STR);

out:
    init_gpio_keys_power_path(exynos5430_pwr);
    init_touchscreen_power_path(exynos5430_pwr);
    init_touchkey_power_path(exynos5430_pwr);
}

/* This function performs power management actions upon the system entering
 * interactive state (that is, the system is awake and ready for interaction,
 * often with UI devices such as display and touchscreen enabled) or
 * non-interactive state (the system appears asleep, display usually turned
 * off).  The non-interactive state is usually entered after a period of
 * inactivity, in order to conserve battery power during such inactive periods.
 */
static void exynos5430_power_set_interactive(struct power_module *module, int on)
{
    struct exynos5430_power_module *exynos5430_pwr = (struct exynos5430_power_module *) module;
    struct stat sb;
    char buf[80];
    int rc;

    ALOGV("power_set_interactive: %d\n", on);

    /*
     * Lower maximum frequency when screen is off.  CPU 0 and 1 share a
     * cpufreq policy.
     */
    sysfs_write("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",
                on ? "1700000" : "800000");

    rc = stat("/sys/devices/system/cpu/cpu4/cpufreq", &sb);
    if (rc == 0) {
        sysfs_write("/sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq",
                    on ? "1700000" : "800000");
    }

    sysfs_write(exynos5430_pwr->touchscreen_power_path, on ? "1" : "0");
    sysfs_write(exynos5430_pwr->touchkey_power_path, on ? "1" : "0");
    sysfs_write(exynos5430_pwr->gpio_keys_power_path, on ? "1" : "0");

    ALOGV("power_set_interactive: %d done\n", on);
}

static struct timespec timespec_diff(struct timespec lhs, struct timespec rhs)
{
    struct timespec result;
    if (rhs.tv_nsec > lhs.tv_nsec) {
        result.tv_sec = lhs.tv_sec - rhs.tv_sec - 1;
        result.tv_nsec = NSEC_PER_SEC + lhs.tv_nsec - rhs.tv_nsec;
    } else {
        result.tv_sec = lhs.tv_sec - rhs.tv_sec;
        result.tv_nsec = lhs.tv_nsec - rhs.tv_nsec;
    }
    return result;
}

static int check_boostpulse_on(struct timespec diff)
{
    long boost_ns = (BOOST_PULSE_DURATION * NSEC_PER_USEC) % NSEC_PER_SEC;
    long boost_s = BOOST_PULSE_DURATION / USEC_PER_SEC;

    if (diff.tv_sec == boost_s)
        return (diff.tv_nsec < boost_ns);
    return (diff.tv_sec < boost_s);
}

/* You need to request the powerhal lock before calling this function */
static int boostpulse_open(struct exynos5430_power_module *exynos5430_pwr)
{
    char errno_str[64];

    if (exynos5430_pwr->boostpulse_fd < 0) {
        exynos5430_pwr->boostpulse_fd = open(BOOSTPULSE_PATH, O_WRONLY);
        if (exynos5430_pwr->boostpulse_fd < 0) {
            if (!exynos5430_pwr->boostpulse_warned) {
                strerror_r(errno, errno_str, sizeof(errno_str));
                ALOGE("Error opening %s: %s\n", BOOSTPULSE_PATH, errno_str);
                exynos5430_pwr->boostpulse_warned = 1;
            }
        }
    }

    return exynos5430_pwr->boostpulse_fd;
}

/*
 * This functions is called to pass hints on power requirements, which may
 * result in adjustment of power/performance parameters of the cpufreq governor
 * and other controls.
 */
static void exynos5430_power_hint(struct power_module *module, power_hint_t hint,
                             void *data)
{
    struct exynos5430_power_module *exynos5430_pwr = (struct exynos5430_power_module *) module;
    char errno_str[64];
    int len;

    switch (hint) {
        case POWER_HINT_INTERACTION:

            ALOGV("%s: POWER_HINT_INTERACTION", __func__);

            pthread_mutex_lock(&exynos5430_pwr->lock);
            if (boostpulse_open(exynos5430_pwr) >= 0) {

                len = write(exynos5430_pwr->boostpulse_fd, "1", 1);
                if (len < 0) {
                    strerror_r(errno, errno_str, sizeof(errno_str));
                    ALOGE("Error writing to %s: %s\n", BOOSTPULSE_PATH, errno_str);
                } else {
                    clock_gettime(CLOCK_MONOTONIC, &last_touch_boost);
                    touch_boost = true;
                }

            }
            pthread_mutex_unlock(&exynos5430_pwr->lock);

            break;

        case POWER_HINT_VSYNC: {
            struct timespec now, diff;

            ALOGV("%s: POWER_HINT_VSYNC", __func__);

            pthread_mutex_lock(&exynos5430_pwr->lock);
            if (data) {
                if (vsync_count < UINT_MAX)
                    vsync_count++;
            } else {
                if (vsync_count)
                    vsync_count--;
                if (vsync_count == 0 && touch_boost) {
                    touch_boost = false;

                    clock_gettime(CLOCK_MONOTONIC, &now);
                    diff = timespec_diff(now, last_touch_boost);

                    if (check_boostpulse_on(diff)) {
                        struct stat sb;
                        int rc;

                        sysfs_write(BOOST_CPU0_PATH, "0");
                        rc = stat(CPU4_MAX_FREQ_PATH, &sb);
                        if (rc == 0) {
                            sysfs_write(BOOST_CPU4_PATH, "0");
                        }
                    }
                }
            }
            pthread_mutex_unlock(&exynos5430_pwr->lock);
            break;
        }
        default:
            break;
    }
}

static struct hw_module_methods_t power_module_methods = {
    .open = NULL,
};

struct exynos5430_power_module HAL_MODULE_INFO_SYM = {
    base: {
        common: {
            tag: HARDWARE_MODULE_TAG,
            module_api_version: POWER_MODULE_API_VERSION_0_2,
            hal_api_version: HARDWARE_HAL_API_VERSION,
            id: POWER_HARDWARE_MODULE_ID,
            name: "EXYNOS5430 Power HAL",
            author: "The Android Open Source Project",
            methods: &power_module_methods,
        },

        init: exynos5430_power_init,
        setInteractive: exynos5430_power_set_interactive,
        powerHint: exynos5430_power_hint,
    },

    lock: PTHREAD_MUTEX_INITIALIZER,
    boostpulse_fd: -1,
    boostpulse_warned: 0,
};
