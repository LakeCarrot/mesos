// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <map>
#include <string>

#include <stout/gtest.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>

#include "tests/environment.hpp"
#include "tests/mesos.hpp"

using process::Future;
using process::Owned;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::internal::slave::state::SlaveState;

using mesos::slave::ContainerTermination;

using std::map;
using std::string;

namespace mesos {
namespace internal {
namespace tests {

class VolumeSandboxPathIsolatorTest : public MesosTest {};


// This test verifies that sandbox path volume allows two containers
// nested under the same parent container to share data.
// TODO(jieyu): Parameterize this test to test both linux and posix
// launcher and filesystem isolator.
TEST_F(VolumeSandboxPathIsolatorTest, SharedVolume)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "volume/sandbox_path";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo("executor", "sleep 99", "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory.get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_TRUE(launch);

  ContainerID nestedContainerId1;
  nestedContainerId1.mutable_parent()->CopyFrom(containerId);
  nestedContainerId1.set_value(UUID::random().toString());

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);

  Volume* volume = containerInfo.add_volumes();
  volume->set_mode(Volume::RW);
  volume->set_container_path("parent");

  Volume::Source* source = volume->mutable_source();
  source->set_type(Volume::Source::SANDBOX_PATH);

  Volume::Source::SandboxPath* sandboxPath = source->mutable_sandbox_path();
  sandboxPath->set_type(Volume::Source::SandboxPath::PARENT);
  sandboxPath->set_path("shared");

  launch = containerizer->launch(
      nestedContainerId1,
      createContainerConfig(
          createCommandInfo("touch parent/file; sleep 1000"),
          containerInfo),
      map<string, string>(),
      None());

  AWAIT_ASSERT_TRUE(launch);

  ContainerID nestedContainerId2;
  nestedContainerId2.mutable_parent()->CopyFrom(containerId);
  nestedContainerId2.set_value(UUID::random().toString());

  launch = containerizer->launch(
      nestedContainerId2,
      createContainerConfig(
          createCommandInfo(
            "while true; do if [ -f parent/file ]; then exit 0; fi; done"),
          containerInfo),
      map<string, string>(),
      None());

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(nestedContainerId2);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}


// This is a regression test for MESOS-7830. It is a ROOT test to
// simulate the scenario that the framework user is non-root while
// the agent process is root, to make sure that non-root user can
// still have the permission to write to the volume as expected.
TEST_F(VolumeSandboxPathIsolatorTest, ROOT_SandboxPathVolumeOwnership)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "volume/sandbox_path";

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      true,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo("executor", "sleep 99", "cpus:1");

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  // Simulate the executor sandbox ownership as the user
  // from FrameworkInfo.
  ASSERT_SOME(os::chown("nobody", directory.get()));

  Future<bool> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, directory.get(), "nobody"),
      map<string, string>(),
      None());

  AWAIT_ASSERT_TRUE(launch);

  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);

  Volume* volume = containerInfo.add_volumes();
  volume->set_mode(Volume::RW);
  volume->set_container_path("parent");

  Volume::Source* source = volume->mutable_source();
  source->set_type(Volume::Source::SANDBOX_PATH);

  Volume::Source::SandboxPath* sandboxPath = source->mutable_sandbox_path();
  sandboxPath->set_type(Volume::Source::SandboxPath::PARENT);
  sandboxPath->set_path("shared");

  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(
          createCommandInfo("echo 'hello' > parent/file"),
          containerInfo,
          None(),
          "nobody"),
      map<string, string>(),
      None());

  AWAIT_ASSERT_TRUE(launch);

  Future<Option<ContainerTermination>> wait =
    containerizer->wait(nestedContainerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait.get()->status());

  wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
