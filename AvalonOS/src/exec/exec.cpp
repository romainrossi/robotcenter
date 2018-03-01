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

#include <signal.h>

#include <iostream>
#include <string>
#include <sstream>

#include <avalon/executor.hpp>

#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/fatal.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/stopwatch.hpp>
#include <stout/stringify.hpp>
#include <stout/uuid.hpp>

#include "common/lock.hpp"
#include "common/type_utils.hpp"

#include "logging/logging.hpp"

#include "messages/messages.hpp"

#include "slave/constants.hpp"

using namespace avalon;
using namespace avalon::internal;

using namespace process;

using std::string;

using process::wait; // Necessary on some OS's to disambiguate.


namespace avalon {
namespace internal {

class ShutdownProcess : public Process<ShutdownProcess>
{
protected:
  virtual void initialize()
  {
    VLOG(1) << "Scheduling shutdown of the executor";
    // TODO(benh): Pass the shutdown timeout with ExecutorRegistered
    // since it might have gotten configured on the command line.
    delay(slave::EXECUTOR_SHUTDOWN_GRACE_PERIOD, self(), &Self::kill);
  }

  void kill()
  {
    VLOG(1) << "Committing suicide by killing the process group";

    // TODO(vinod): Invoke killtree without killing ourselves.
    // Kill the process group (including ourself).
    killpg(0, SIGKILL);

    // The signal might not get delivered immediately, so sleep for a
    // few seconds. Worst case scenario, exit abnormally.
    sleep(5);
    exit(-1);
  }
};


class ExecutorProcess : public ProtobufProcess<ExecutorProcess>
{
public:
  ExecutorProcess(const UPID& _slave,
                  AvalonExecutorDriver* _driver,
                  Executor* _executor,
                  const FrameworkID& _frameworkId,
                  const ExecutorID& _executorId,
                  bool _local,
                  const std::string& _directory)
    : ProcessBase(ID::generate("executor")),
      slave(_slave),
      driver(_driver),
      executor(_executor),
      frameworkId(_frameworkId),
      executorId(_executorId),
      local(_local),
      aborted(false),
      directory(_directory)
  {
    install<ExecutorRegisteredMessage>(
        &ExecutorProcess::registered,
        &ExecutorRegisteredMessage::executor_info,
        &ExecutorRegisteredMessage::framework_id,
        &ExecutorRegisteredMessage::framework_info,
        &ExecutorRegisteredMessage::slave_id,
        &ExecutorRegisteredMessage::slave_info);

    install<RunTaskMessage>(
        &ExecutorProcess::runTask,
        &RunTaskMessage::task);

    install<KillTaskMessage>(
        &ExecutorProcess::killTask,
        &KillTaskMessage::task_id);

    install<FrameworkToExecutorMessage>(
        &ExecutorProcess::frameworkMessage,
        &FrameworkToExecutorMessage::slave_id,
        &FrameworkToExecutorMessage::framework_id,
        &FrameworkToExecutorMessage::executor_id,
        &FrameworkToExecutorMessage::data);

    install<ShutdownExecutorMessage>(
        &ExecutorProcess::shutdown);
  }

  virtual ~ExecutorProcess() {}

protected:
  virtual void initialize()
  {
    VLOG(1) << "[ExecutorProcess]:Executor started at: " << self();

    link(slave);

    // Register with slave.
    RegisterExecutorMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_executor_id()->MergeFrom(executorId);
    send(slave, message);
  }

  void registered(const ExecutorInfo& executorInfo,
                  const FrameworkID& frameworkId,
                  const FrameworkInfo& frameworkInfo,
                  const SlaveID& slaveId,
                  const SlaveInfo& slaveInfo)
  {
    if (aborted) {
      VLOG(1) << "Ignoring registered message from slave " << slaveId
              << " because the driver is aborted!";
      return;
    }

    VLOG(1) << "[ExecutorProcess]:Executor registered on slave " << slaveId;

    this->slaveId = slaveId;

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    executor->registered(driver, executorInfo, frameworkInfo, slaveInfo);

    VLOG(1) << "Executor::registered took " << stopwatch.elapsed();
  }

  void runTask(const TaskInfo& task)
  {
    if (aborted) {
      VLOG(1) << "Ignoring run task message for task " << task.task_id()
              << " because the driver is aborted!";
      return;
    }

    VLOG(1) << "Executor asked to run task '" << task.task_id() << "'";

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    executor->launchTask(driver, task);

    VLOG(1) << "Executor::launchTask took " << stopwatch.elapsed();
  }

  void killTask(const TaskID& taskId)
  {
    if (aborted) {
      VLOG(1) << "Ignoring kill task message for task " << taskId
              <<" because the driver is aborted!";
      return;
    }

    VLOG(1) << "Executor asked to kill task '" << taskId << "'";

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    executor->killTask(driver, taskId);

    VLOG(1) << "Executor::killTask took " << stopwatch.elapsed();
  }

  void frameworkMessage(const SlaveID& slaveId,
                        const FrameworkID& frameworkId,
                        const ExecutorID& executorId,
                        const string& data)
  {
    if (aborted) {
      VLOG(1) << "Ignoring framework message because the driver is aborted!";
      return;
    }

    VLOG(1) << "Executor received framework message";

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    executor->frameworkMessage(driver, data);

    VLOG(1) << "Executor::frameworkMessage took " << stopwatch.elapsed();
  }

  void shutdown()
  {
    if (aborted) {
      VLOG(1) << "Ignoring shutdown message because the driver is aborted!";
      return;
    }

    VLOG(1) << "Executor asked to shutdown";

    if (!local) {
      // Start the Shutdown Process.
      spawn(new ShutdownProcess(), true);
    }

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    // TODO(benh): Any need to invoke driver.stop?
    executor->shutdown(driver);

    VLOG(1) << "Executor::shutdown took " << stopwatch.elapsed();

    if (local) {
      terminate(this);
    }
  }

  void abort()
  {
    VLOG(1) << "De-activating the executor libprocess";
    aborted = true;
  }

  virtual void exited(const UPID& pid)
  {
    if (aborted) {
      VLOG(1) << "Ignoring exited event because the driver is aborted!";
      return;
    }

    VLOG(1) << "Slave exited, trying to shutdown";

    if (!local) {
      // Start the Shutdown Process.
      spawn(new ShutdownProcess(), true);
    }

    Stopwatch stopwatch;
    if (FLAGS_v >= 1) {
      stopwatch.start();
    }

    // TODO: Pass an argument to shutdown to tell it this is abnormal?
    executor->shutdown(driver);

    VLOG(1) << "Executor::shutdown took " << stopwatch.elapsed();

    // This is a pretty bad state ... no slave is left. Rather
    // than exit lets kill our process group (which includes
    // ourself) hoping to clean up any processes this executor
    // launched itself.
    // TODO(benh): Maybe do a SIGTERM and then later do a SIGKILL?
    if (local) {
      terminate(this);
    }
  }

  void sendStatusUpdate(const TaskStatus& status)
  {
    VLOG(1) << "Executor sending status update for task "
            << status.task_id() << " in state " << status.state();

    if (status.state() == TASK_STAGING) {
      VLOG(1) << "Executor is not allowed to send "
              << "TASK_STAGING status updates. Aborting!";

      driver->abort();

      Stopwatch stopwatch;
      if (FLAGS_v >= 1) {
        stopwatch.start();
      }

      executor->error(driver, "Attempted to send TASK_STAGING status update");

      VLOG(1) << "Executor::error took " << stopwatch.elapsed();

      return;
    }

    StatusUpdateMessage message;
    StatusUpdate* update = message.mutable_update();
    update->mutable_framework_id()->MergeFrom(frameworkId);
    update->mutable_executor_id()->MergeFrom(executorId);
    update->mutable_slave_id()->MergeFrom(slaveId);
    update->mutable_status()->MergeFrom(status);
    update->set_timestamp(Clock::now());
    update->set_uuid(UUID::random().toBytes());

    send(slave, message);
  }

  void sendFrameworkMessage(const string& data)
  {
    ExecutorToFrameworkMessage message;
    message.mutable_slave_id()->MergeFrom(slaveId);
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_executor_id()->MergeFrom(executorId);
    message.set_data(data);
    send(slave, message);
  }

private:
  friend class avalon::AvalonExecutorDriver;

  UPID slave;
  AvalonExecutorDriver* driver;
  Executor* executor;
  FrameworkID frameworkId;
  ExecutorID executorId;
  SlaveID slaveId;
  bool local;
  bool aborted;
  const std::string directory;
};

} // namespace internal {
} // namespace avalon {


// Implementation of C++ API.


AvalonExecutorDriver::AvalonExecutorDriver(Executor* _executor)
  : executor(_executor),
    process(NULL),
    status(DRIVER_NOT_STARTED)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  // Create mutex and condition variable
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  pthread_cond_init(&cond, 0);

  // Initialize libprocess.
  process::initialize();

  // TODO(benh): Initialize glog.
}


AvalonExecutorDriver::~AvalonExecutorDriver()
{
  // Just as in SchedulerProcess, we might wait here indefinitely if
  // AvalonExecutorDriver::stop has not been invoked.
  wait(process);
  delete process;

  pthread_mutex_destroy(&mutex);
  pthread_cond_destroy(&cond);
}


Status AvalonExecutorDriver::start()
{
  Lock lock(&mutex);

  if (status != DRIVER_NOT_STARTED) {
    return status;
  }

  // Set stream buffering mode to flush on newlines so that we capture logs
  // from user processes even when output is redirected to a file.
  setvbuf(stdout, 0, _IOLBF, 0);
  setvbuf(stderr, 0, _IOLBF, 0);

  bool local;

  UPID slave;
  FrameworkID frameworkId;
  ExecutorID executorId;
  std::string workDirectory;

  char* value;
  std::istringstream iss;

  /* Check if this is local (for example, for testing). */
  value = getenv("AVALON_LOCAL");

  if (value != NULL) {
    local = true;
  } else {
    local = false;
  }

  /* Get slave PID from environment. */
  value = getenv("AVALON_SLAVE_PID");

  if (value == NULL) {
    fatal("expecting AVALON_SLAVE_PID in environment");
  }

  slave = UPID(value);

  if (!slave) {
    fatal("cannot parse AVALON_SLAVE_PID");
  }

  /* Get framework ID from environment. */
  value = getenv("AVALON_FRAMEWORK_ID");

  if (value == NULL) {
    fatal("expecting AVALON_FRAMEWORK_ID in environment");
  }

  frameworkId.set_value(value);

  /* Get executor ID from environment. */
  value = getenv("AVALON_EXECUTOR_ID");

  if (value == NULL) {
    fatal("expecting AVALON_EXECUTOR_ID in environment");
  }

  executorId.set_value(value);

  /* Get working directory from environment */
  value = getenv("AVALON_DIRECTORY");

  if (value == NULL) {
    fatal("expecting AVALON_DIRECTORY in environment");
  }

  workDirectory = value;

  CHECK(process == NULL);

  process =
    new ExecutorProcess(slave, this, executor, frameworkId,
                        executorId, local, workDirectory);

  spawn(process);

  return status = DRIVER_RUNNING;
}


Status AvalonExecutorDriver::stop()
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING && status != DRIVER_ABORTED) {
    return status;
  }

  CHECK(process != NULL);

  terminate(process);

  // TODO(benh): Set the condition variable in ExecutorProcess just as
  // we do with the AvalonSchedulerDriver and SchedulerProcess:
  // dispatch(process, &ExecutorProcess::stop);

  pthread_cond_signal(&cond);

  bool aborted = status == DRIVER_ABORTED;

  status = DRIVER_STOPPED;

  return aborted ? DRIVER_ABORTED : status;
}


Status AvalonExecutorDriver::abort()
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING) {
    return status;
  }

  CHECK(process != NULL);

  // TODO(benh): Set the condition variable in ExecutorProcess just as
  // we do with the AvalonSchedulerDriver and SchedulerProcess.

  dispatch(process, &ExecutorProcess::abort);

  pthread_cond_signal(&cond);

  return status = DRIVER_ABORTED;
}


Status AvalonExecutorDriver::join()
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING) {
    return status;
  }

  while (status == DRIVER_RUNNING) {
    pthread_cond_wait(&cond, &mutex);
  }

  CHECK(status == DRIVER_ABORTED || status == DRIVER_STOPPED);

  return status;
}


Status AvalonExecutorDriver::run()
{
  Status status = start();
  return status != DRIVER_RUNNING ? status : join();
}


Status AvalonExecutorDriver::sendStatusUpdate(const TaskStatus& taskStatus)
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING) {
    return status;
  }

  CHECK(process != NULL);
/**
* commented by Ailias
* can not use dispatch function because we need send status to slave immediately 
  dispatch(process, &ExecutorProcess::sendStatusUpdate, taskStatus);
*/
  process->sendStatusUpdate(taskStatus);
  return status;
}


Status AvalonExecutorDriver::sendFrameworkMessage(const string& data)
{
  Lock lock(&mutex);

  if (status != DRIVER_RUNNING) {
    return status;
  }

  CHECK(process != NULL);

  dispatch(process, &ExecutorProcess::sendFrameworkMessage, data);

  return status;
}
