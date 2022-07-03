/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include <cutils/ashmem.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/ashmem.h>
#include <linux/memfd.h>
#include <log/log.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

/* Will be added to UAPI once upstream change is merged */
#define F_SEAL_FUTURE_WRITE 0x0010

/*
 * The minimum vendor API level at and after which it is safe to use memfd.
 * This is to facilitate deprecation of ashmem.
 */
#define MIN_MEMFD_VENDOR_API_LEVEL 29
#define MIN_MEMFD_VENDOR_API_LEVEL_CHAR 'Q'

/*
 * has_memfd_support() determines if the device can use memfd. memfd support
 * has been there for long time, but certain things in it may be missing.  We
 * check for needed support in it. Also we check if the VNDK version of
 * libcutils being used is new enough, if its not, then we cannot use memfd
 * since the older copies may be using ashmem so we just use ashmem. Once all
 * Android devices that are getting updates are new enough (ex, they were
 * originally shipped with Android release > P), then we can just use memfd and
 * delete all ashmem code from libcutils (while preserving the interface).
 *
 * NOTE:
 * The sys.use_memfd property is set by default to false in Android
 * to temporarily disable memfd, till vendor and apps are ready for it.
 * The main issue: either apps or vendor processes can directly make ashmem
 * IOCTLs on FDs they receive by assuming they are ashmem, without going
 * through libcutils. Such fds could have very well be originally created with
 * libcutils hence they could be memfd. Thus the IOCTLs will break.
 *
 * Set default value of sys.use_memfd property to true once the issue is
 * resolved, so that the code can then self-detect if kernel support is present
 * on the device. The property can also set to true from adb shell, for
 * debugging.
 */

static bool debug_log = false;            /* set to true for verbose logging and other debug  */
static bool pin_deprecation_warn = true; /* Log the pin deprecation warning only once */


/* Determine if memfd can be supported. This is just one-time hardwork
 * which will be cached by the caller.
 */
static bool __has_memfd_support() {
    /* Used to turn on/off the detection at runtime, in the future this
     * property will be removed once we switch everything over to ashmem.
     * Currently it is used only for debugging to switch the system over.
     */
    if (!android::base::GetBoolProperty("sys.use_memfd", false)) {
        if (debug_log) {
            ALOGD("sys.use_memfd=false so memfd disabled\n");
        }
        return false;
    }

    // Check if kernel support exists, otherwise fall back to ashmem.
    // This code needs to build on old API levels, so we can't use the libc
    // wrapper.
    android::base::unique_fd fd(
            syscall(__NR_memfd_create, "test_android_memfd", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd == -1) {
        ALOGE("memfd_create failed: %s, no memfd support.\n", strerror(errno));
        return false;
    }

    if (fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == -1) {
        ALOGE("fcntl(F_ADD_SEALS) failed: %s, no memfd support.\n", strerror(errno));
        return false;
    }

    if (debug_log) {
        ALOGD("memfd: device has memfd support, using it\n");
    }
    return true;
}

static bool has_memfd_support() {
    /* memfd_supported is the initial global per-process state of what is known
     * about memfd.
     */
    static bool memfd_supported = __has_memfd_support();

    return memfd_supported;
}

static bool memfd_is_ashmem(int fd) {
    static bool fd_check_error_once = false;

    if (__ashmem_is_ashmem(fd, 0) == 0) {
        if (!fd_check_error_once) {
            ALOGE("memfd: memfd expected but ashmem fd used - please use libcutils.\n");
            fd_check_error_once = true;
        }

        return true;
    }

    return false;
}

static int memfd_create_region(const char* name, size_t size) {
    // This code needs to build on old API levels, so we can't use the libc
    // wrapper.
    android::base::unique_fd fd(syscall(__NR_memfd_create, name, MFD_CLOEXEC | MFD_ALLOW_SEALING));

    if (fd == -1) {
        ALOGE("memfd_create(%s, %zd) failed: %s\n", name, size, strerror(errno));
        return -1;
    }

    if (ftruncate(fd, size) == -1) {
        ALOGE("ftruncate(%s, %zd) failed for memfd creation: %s\n", name, size, strerror(errno));
        return -1;
    }

    if (debug_log) {
        ALOGE("memfd_create(%s, %zd) success. fd=%d\n", name, size, fd.get());
    }
    return fd.release();
}

static int memfd_set_prot_region(int fd, int prot) {
    /* Only proceed if an fd needs to be write-protected */
    if (prot & PROT_WRITE) {
        return 0;
    }

    if (fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == -1) {
        ALOGE("memfd_set_prot_region(%d, %d): F_SEAL_FUTURE_WRITE seal failed: %s\n", fd, prot,
              strerror(errno));
        return -1;
    }

    return 0;
}
