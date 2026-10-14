/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "first_stage_console.h"

#include <spawn.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>

#include <string>
#include <thread>

#include <android-base/chrono_utils.h>
#include <android-base/file.h>
#include <android-base/logging.h>

static bool KernelConsolePresent(const std::string& cmdline) {
    size_t pos = 0;
    while (true) {
        pos = cmdline.find("console=", pos);
        if (pos == std::string::npos) return false;
        if (pos == 0 || cmdline[pos - 1] == ' ') return true;
        pos++;
    }
}

static bool SetupConsole() {
    if (mknod("/dev/console", S_IFCHR | 0600, makedev(5, 1)) < 0 && errno != EEXIST) {
        PLOG(ERROR) << "unable to create /dev/console";
        return false;
    }

    int fd = -1;
    int tries = 50;  // should timeout after 5s
    // The device driver for console may not be ready yet so retry for a while in case of failure.
    while (tries--) {
        fd = open("/dev/console", O_RDWR);
        if (fd != -1) break;
        std::this_thread::sleep_for(100ms);
    }
    if (fd == -1) {
        PLOG(ERROR) << "could not open /dev/console";
        return false;
    }

    // Become a session leader so we can acquire a controlling tty.
    if (setsid() == -1) {
        // EINVAL can happen if we're already a process-group leader.
        // In this code path, however, we'd generally expect setsid()
        // to succeed.
        PLOG(ERROR) << "setsid() failed";
        close(fd);
        return false;
    }

    if (ioctl(fd, TIOCSCTTY, 0) < 0) {
        PLOG(ERROR) << "TIOCSCTTY failed";
        close(fd);
        return false;
    }

    if (dup2(fd, STDIN_FILENO) < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        PLOG(ERROR) << "dup2() failed";
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

static bool SpawnImage(const char* file, pid_t* pid) {
    const char* argv[] = {file, nullptr};
    const char* envp[] = {nullptr};

    int rc = posix_spawn(pid, file, nullptr, nullptr, const_cast<char* const*>(argv),
                         const_cast<char* const*>(envp));

    if (rc == 0) return true;

    errno = rc;
    PLOG(ERROR) << "Failed to spawn '" << file << "'";
    return false;
}

namespace android {
namespace init {

void StartConsole(const std::string& cmdline, const std::string& program) {
    bool console = KernelConsolePresent(cmdline);

    // We need to wait for our child, so don't use SA_NOCLDWAIT.
    struct sigaction chld_act{};
    chld_act.sa_handler = SIG_DFL;
    sigemptyset(&chld_act.sa_mask);
    sigaction(SIGCHLD, &chld_act, nullptr);

    pid_t pid = fork();

    if (pid < 0) {
        PLOG(ERROR) << "fork() failed";
        return;
    }

    if (pid > 0) {
        int status;
        if (waitpid(pid, &status, 0) < 0) {
            PLOG(ERROR) << "waitpid() failed";
        } else {
            LOG(ERROR) << "console shell exited";
        }
        return;
    }

    if (console) console = SetupConsole();

    for (const auto& sh : {"/first_stage.sh", "/vendor/bin/bootstrap/first_stage.sh"}) {
        LOG(INFO) << "Attempting to run " << sh;
        pid_t first_stage_sh_pid;
        if (SpawnImage(sh, &first_stage_sh_pid)) {
            waitpid(first_stage_sh_pid, nullptr, 0);
            LOG(INFO) << sh << " exited";
        }
    }

    if (console) {
        pid_t shell_pid;
        if (SpawnImage(program.c_str(), &shell_pid)) {
            waitpid(shell_pid, nullptr, 0);
        }
    }

    _exit(127);
}

int FirstStageConsole(const std::string& cmdline, const std::string& bootconfig) {
    auto pos = bootconfig.find("androidboot.first_stage_console =");
    if (pos != std::string::npos) {
        int val = 0;
        if (sscanf(bootconfig.c_str() + pos, "androidboot.first_stage_console = \"%d\"", &val) !=
            1) {
            return FirstStageConsoleParam::DISABLED;
        }
        if (val <= FirstStageConsoleParam::MAX_PARAM_VALUE && val >= 0) {
            return val;
        }
    }

    pos = cmdline.find("androidboot.first_stage_console=");
    if (pos != std::string::npos) {
        int val = 0;
        if (sscanf(cmdline.c_str() + pos, "androidboot.first_stage_console=%d", &val) != 1) {
            return FirstStageConsoleParam::DISABLED;
        }
        if (val <= FirstStageConsoleParam::MAX_PARAM_VALUE && val >= 0) {
            return val;
        }
    }
    return FirstStageConsoleParam::DISABLED;
}

}  // namespace init
}  // namespace android
