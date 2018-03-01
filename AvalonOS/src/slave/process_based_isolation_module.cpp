/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h> // For perror.
#include <string.h>

#include <map>

#include <process/clock.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>

#include <stout/foreach.hpp>
#include <stout/os.hpp>

#include "common/type_utils.hpp"
#include "common/process_utils.hpp"

#ifdef __linux__
#include "linux/proc.hpp"
#endif

#include "slave/flags.hpp"
#include "slave/process_based_isolation_module.hpp"

using namespace avalon;
using namespace avalon::internal;
using namespace avalon::internal::slave;

using namespace process;

using launcher::ExecutorLauncher;

using std::map;
using std::string;

using process::wait; // Necessary on some OS's to disambiguate.


ProcessBasedIsolationModule::ProcessBasedIsolationModule()
  : ProcessBase(ID::generate("process-isolation-module")),
    initialized(false)
{
  // Spawn the reaper, note that it might send us a message before we
  // actually get spawned ourselves, but that's okay, the message will
  // just get dropped.
  reaper = new Reaper();
  spawn(reaper);
  dispatch(reaper, &Reaper::addProcessExitedListener, this);
}


ProcessBasedIsolationModule::~ProcessBasedIsolationModule()
{
  CHECK(reaper != NULL);
  terminate(reaper);
  wait(reaper);
  delete reaper;
}


void ProcessBasedIsolationModule::initialize(
    const Flags& _flags,
    const Resources& _,
    bool _local,
    const PID<Slave>& _slave)
{
  flags = _flags;
  local = _local;
  slave = _slave;

  initialized = true;
}


void ProcessBasedIsolationModule::launchExecutor(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Resources& resources)
{
  CHECK(initialized) << "Cannot launch executors before initialization!";

  const ExecutorID& executorId = executorInfo.executor_id();

  LOG(INFO) << "Launching " << executorId
            << " (" << executorInfo.command().value() << ")"
            << " in " << directory
            << " with resources " << resources
            << "' for framework " << frameworkId;

  // Store the working directory, so that in the future we can use it
  // to retrieve the os pid when calling killtree on the executor.
  ProcessInfo* info = new ProcessInfo();
  info->frameworkId = frameworkId;
  info->executorId = executorId;
  info->directory = directory;
  info->pid = -1; // Initialize this variable to handle corner cases.

  infos[frameworkId][executorId] = info;

  // Use pipes to determine which child has successfully changed session.
  int pipes[2];
  if (pipe(pipes) < 0) {
    PLOG(FATAL) << "Failed to create a pipe";
  }

  // Set the FD_CLOEXEC flags on these pipes
  Try<Nothing> cloexec = os::cloexec(pipes[0]);
  CHECK_SOME(cloexec) << "Error setting FD_CLOEXEC on pipe[0]";

  cloexec = os::cloexec(pipes[1]);
  CHECK_SOME(cloexec) << "Error setting FD_CLOEXEC on pipe[1]";

  pid_t pid;
  if ((pid = fork()) == -1) {
    PLOG(FATAL) << "Failed to fork to launch new executor";
  }

  if (pid) {
    close(pipes[1]);

    // Get the child's pid via the pipe.
    if (read(pipes[0], &pid, sizeof(pid)) == -1) {
      PLOG(FATAL) << "Failed to get child PID from pipe";
    }

    close(pipes[0]);

    // In parent process.
    LOG(INFO) << "Forked executor at " << pid;

    // Record the pid (should also be the pgis since we setsid below).
    infos[frameworkId][executorId]->pid = pid;

    // Tell the slave this executor has started.
    dispatch(slave, &Slave::executorStarted, frameworkId, executorId, pid);
  } else {
    // In child process, we make cleanup easier by putting process
    // into it's own session. DO NOT USE GLOG!
    close(pipes[0]);

    // NOTE: We setsid() in a loop because setsid() might fail if another
    // process has the same process group id as the calling process.
    while ((pid = setsid()) == -1) {
      perror("Could not put executor in own session");

      std::cout << "Forking another process and retrying ..." << std::endl;

      if ((pid = fork()) == -1) {
        perror("Failed to fork to launch executor");
        abort();
      }

      if (pid) {
        // In parent process.
        // It is ok to suicide here, though process reaper signals the exit,
        // because the process isolation module ignores unknown processes.
        exit(0);
      }
    }

    if (write(pipes[1], &pid, sizeof(pid)) != sizeof(pid)) {
      perror("Failed to write PID on pipe");
      abort();
    }

    close(pipes[1]);
    std::cout << std::endl;
    std::cout << "createExecutorLauncher->directory: "
              << directory << std::endl;

    ExecutorLauncher* launcher = createExecutorLauncher(
      frameworkId, frameworkInfo, executorInfo, directory);

    std::cout << "launcher->run" << std::endl;
    if (launcher->run() < 0) {
      std::cerr << "Failed to launch executor" << std::endl;
      abort();
    }
  }
}

// NOTE: This function can be called by the isolation module itself or
// by the slave if it doesn't hear about an executor exit after it sends
// a shutdown message.
void ProcessBasedIsolationModule::killExecutor(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  CHECK(initialized) << "Cannot kill executors before initialization!";
  if (!infos.contains(frameworkId) ||
      !infos[frameworkId].contains(executorId)) {
    LOG(ERROR) << "ERROR! Asked to kill an unknown executor! " << executorId;
    return;
  }

  pid_t pid = infos[frameworkId][executorId]->pid;

  if (pid != -1) {
    // TODO(vinod): Call killtree on the pid of the actual executor process
    // that is running the tasks (stored in the local storage by the
    // executor module).
    utils::process::killtree(pid, SIGKILL, true, true, true);

    // Also kill all processes that belong to the process group of the executor.
    // This is valuable in situations where the top level executor process
    // exited and hence killtree is unable to kill any spawned orphans.
    // NOTE: This assumes that the process group id of the executor process is
    // same as its pid (which is expected to be the case with setsid()).
    // TODO(vinod): Also (recursively) kill processes belonging to the
    // same session, but have a different process group id.
    if (killpg(pid, SIGKILL) == -1 && errno != ESRCH) {
      PLOG(WARNING) << "Failed to kill process group " << pid;
    }
  }
}


void ProcessBasedIsolationModule::resourcesChanged(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const Resources& resources)
{
  CHECK(initialized) << "Cannot do resourcesChanged before initialization!";
  // Do nothing; subclasses may override this.
}


ExecutorLauncher* ProcessBasedIsolationModule::createExecutorLauncher(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const string& directory)
{
  return new ExecutorLauncher(frameworkId,
                              executorInfo.executor_id(),
                              executorInfo.command(),
                              frameworkInfo.user(),
                              directory,
                              slave,
                              flags.frameworks_home,
                              flags.hadoop_home,
                              !local,
                              flags.switch_user,
                              "");
}


Future<ResourceStatistics> ProcessBasedIsolationModule::usage(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  if (!infos.contains(frameworkId) ||
      !infos[frameworkId].contains(executorId)) {
    return Future<ResourceStatistics>::failed("Unknown executor");
  }

  ProcessInfo* info = infos[frameworkId][executorId];

  ResourceStatistics result;

  result.set_timestamp(Clock::now());

#ifdef __linux__
  // Get the page size, used for memory accounting.
  // NOTE: This is more portable than using getpagesize().
  long pageSize = sysconf(_SC_PAGESIZE);

  // Get the number of clock ticks, used for cpu accounting.
  long ticks = sysconf(_SC_CLK_TCK);

  Try<proc::ProcessStatistics> stat = proc::stat(info->pid);

  if (stat.isSome() && pageSize > 0) {
    result.set_memory_rss(stat.get().rss * pageSize);
  }

  if (stat.isSome() && ticks > 0) {
    result.set_cpu_user_time((double) stat.get().utime / (double) ticks);
    result.set_cpu_system_time((double) stat.get().stime / (double) ticks);
  }
#endif

  return result;
}


void ProcessBasedIsolationModule::processExited(pid_t pid, int status)
{
  foreachkey (const FrameworkID& frameworkId, infos) {
    foreachpair (
        const ExecutorID& executorId, ProcessInfo* info, infos[frameworkId]) {
      if (info->pid == pid) {
        LOG(INFO) << "Telling slave of lost executor " << executorId
                  << " of framework " << frameworkId;

        dispatch(slave,
                 &Slave::executorTerminated,
                 frameworkId,
                 executorId,
                 status,
                 false,
                 "Executor exited");

        // Try and cleanup after the executor.
        killExecutor(frameworkId, executorId);

        if (infos[frameworkId].size() == 1) {
          infos.erase(frameworkId);
        } else {
          infos[frameworkId].erase(executorId);
        }

        delete info;
        return;
      }
    }
  }
}
