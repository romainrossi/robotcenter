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

#ifndef __LAUNCHER_HPP__
#define __LAUNCHER_HPP__

#include <string>

#include <avalon/avalon.hpp>

namespace avalon {
namespace internal {
namespace launcher {

// This class sets up the environment for an executor and then exec()'s it.
// It can either be used after a fork() in the slave process, or run as a
// standalone program (with the main function in launcher_main.cpp).
//
// The environment is initialized through for steps:
// 1) A work directory for the framework is created by createWorkingDirectory().
// 2) The executor is fetched off HDFS if necessary by fetchExecutor().
// 3) Environment variables are set by setupEnvironment().
// 4) We switch to the framework's user in switchUser().
//
// Isolation modules that wish to override the default behaviour can subclass
// Launcher and override some of the methods to perform extra actions.
class ExecutorLauncher {
public:
  ExecutorLauncher(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const CommandInfo& commandInfo,
      const std::string& user,
      const std::string& workDirectory,
      const std::string& slavePid,
      const std::string& frameworksHome,
      const std::string& hadoopHome,
      bool redirectIO,
      bool shouldSwitchUser,
      const std::string& container);

  virtual ~ExecutorLauncher();

  // Initialize the working directory and fetch the executor.
  virtual int setup();

  // Launches the downloaded executor.
  virtual int launch();

  // Convenience function that calls setup() and then launch().
  virtual int run();

  // Set up environment variables for exec'ing a launcher_main.cpp
  // (avalon-launcher binary) process. This is used by isolation modules that
  // cannot exec the user's executor directly, such as the LXC isolation
  // module, which must run lxc-execute and have it run the launcher.
  virtual void setupEnvironmentForLauncherMain();

protected:
  // Download the required files for the executor from the given set of URIs.
  // Optionally, it will set the executable file permissions for the files.
  // This method is expected to place files in the workDirectory.
  virtual int fetchExecutors();

  // Set up environment variables for launching a framework's executor.
  virtual void setupEnvironment();

  // Switch to a framework's user in preparation for exec()'ing its executor.
  virtual void switchUser();

protected:
  FrameworkID frameworkId;
  ExecutorID executorId;
  CommandInfo commandInfo;
  std::string user;
  std::string workDirectory; // Directory in which the framework should run.
  std::string slavePid;
  std::string frameworksHome;
  std::string hadoopHome;
  bool redirectIO;   // Whether to redirect stdout and stderr to files.
  bool shouldSwitchUser; // Whether to setuid to framework's user.
  std::string container;
};

} // namespace launcher {
} // namespace internal {
} // namespace avalon {

#endif // __LAUNCHER_HPP__
