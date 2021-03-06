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

#include <deque>
#include <string>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>

#include "common/type_utils.hpp"

#include "master/registrar.hpp"
#include "master/registry.hpp"

#include "state/protobuf.hpp"

using mesos::internal::state::protobuf::State;
using mesos::internal::state::protobuf::Variable;

using process::dispatch;
using process::Failure;
using process::Future;
using process::Process;
using process::Promise;
using process::spawn;
using process::terminate;
using process::wait; // Necessary on some OS's to disambiguate.

using std::deque;
using std::string;

namespace mesos {
namespace internal {
namespace master {

class RegistrarProcess : public Process<RegistrarProcess>
{
public:
  RegistrarProcess(State* _state)
    : ProcessBase("registrar"),
      updating(false),
      state(_state) {}

  virtual ~RegistrarProcess() {}

  // Registrar implementation.
  Future<bool> admit(const SlaveInfo& info);
  Future<bool> readmit(const SlaveInfo& info);
  Future<bool> remove(const SlaveInfo& info);

private:
  template <typename T>
  struct Operation : process::Promise<bool>
  {
    Operation() : success(false) {}

    // Attempts to invoke the operation on 't'.
    // Returns Some if the operation mutates 't'.
    // Returns None if the operation does not mutate 't'.
    // Returns Error if the operation cannot be performed on 't'.
    Result<T> operator () (T t)
    {
      const Result<T>& result = perform(t);

      success = !result.isError();

      return result;
    }

    // Sets the promise based on whether the operation was successful.
    bool set() { return Promise<bool>::set(success); }

  protected:
    virtual Result<T> perform(T t) = 0;

  private:
    bool success;
  };

  struct Admit : Operation<Registry>
  {
    Admit(const SlaveInfo& _info) : info(_info) {}

  protected:
    virtual Result<Registry> perform(Registry registry)
    {
      // Check and see if this slave already exists.
      foreach (const Registry::Slave& slave, registry.slaves().slaves()) {
        if (slave.info().id() == info.id()) {
          return Error("Slave already admitted");
        }
      }

      Registry::Slave* slave = registry.mutable_slaves()->add_slaves();
      slave->mutable_info()->CopyFrom(info);
      return registry;
    }

    const SlaveInfo info;
  };

  struct Readmit : Operation<Registry>
  {
    Readmit(const SlaveInfo& _info) : info(_info) {}

  protected:
    virtual Result<Registry> perform(Registry registry)
    {
      foreach (const Registry::Slave& slave, registry.slaves().slaves()) {
        if (slave.info().id() == info.id()) {
          return None();
        }
      }

      return Error("Slave not yet admitted");
    }

    const SlaveInfo info;
  };

  struct Remove : Operation<Registry>
  {
    Remove(const SlaveInfo& _info) : info(_info) {}

  protected:
    virtual Result<Registry> perform(Registry registry)
    {
      for (int i = 0; i < registry.slaves().slaves().size(); i++) {
        const Registry::Slave& slave = registry.slaves().slaves(i);
        if (slave.info().id() == info.id()) {
          for (int j = i + 1; j < registry.slaves().slaves().size(); j++) {
            registry.mutable_slaves()->mutable_slaves()->SwapElements(j - 1, j);
          }
          registry.mutable_slaves()->mutable_slaves()->RemoveLast();
          return registry;
        }
      }

      return Error("Slave not yet admitted");
    }

    const SlaveInfo info;
  };

  Option<Variable<Registry> > variable;
  deque<Operation<Registry>*> operations;
  bool updating; // Used to signify fetching (recovering) or storing.

  // Continuations.
  Future<bool> _admit(const SlaveInfo& info);
  Future<bool> _readmit(const SlaveInfo& info);
  Future<bool> _remove(const SlaveInfo& info);

  // Helper for recovering state (performing fetch).
  Future<Nothing> recover();
  void _recover(const Future<Variable<Registry> >& recovery);

  // Helper for updating state (performing store).
  void update();
  void _update(
      const Future<Option<Variable<Registry> > >& store,
      deque<Operation<Registry>*> operations);

  State* state;

  // Used to compose our operations with recovery.
  Promise<Nothing> recovered;
};


Future<Nothing> RegistrarProcess::recover()
{
  LOG(INFO) << "Recovering registrar";

  if (variable.isNone() && !updating) {
    // TODO(benh): Don't wait forever to recover?
    state->fetch<Registry>("registry")
      .onAny(defer(self(), &Self::_recover, lambda::_1));
    updating = true;
  }

  return recovered.future();
}


void RegistrarProcess::_recover(
    const Future<Variable<Registry> >& recovery)
{
  updating = false;

  CHECK(!recovery.isPending());

  if (recovery.isFailed() || recovery.isDiscarded()) {
    LOG(WARNING) << "Failed to recover registrar: "
                 << (recovery.isFailed() ? recovery.failure() : "discarded");
    recover(); // Retry! TODO(benh): Don't retry forever?
  } else {
    LOG(INFO) << "Successfully recovered registrar";

    // Save the registry.
    variable = recovery.get();

    // Signal the recovery is complete.
    recovered.set(Nothing());
  }
}


Future<bool> RegistrarProcess::admit(const SlaveInfo& info)
{
  if (!info.has_id()) {
    return Failure("SlaveInfo is missing the 'id' field");
  }

  return recover()
    .then(defer(self(), &Self::_admit, info));
}


Future<bool> RegistrarProcess::_admit(const SlaveInfo& info)
{
  CHECK_SOME(variable);
  Operation<Registry>* operation = new Admit(info);
  operations.push_back(operation);
  Future<bool> future = operation->future();
  if (!updating) {
    update();
  }
  return future;
}


Future<bool> RegistrarProcess::readmit(const SlaveInfo& info)
{
  if (!info.has_id()) {
    return Failure("SlaveInfo is missing the 'id' field");
  }

  return recover()
    .then(defer(self(), &Self::_readmit, info));
}


Future<bool> RegistrarProcess::_readmit(
    const SlaveInfo& info)
{
  CHECK_SOME(variable);

  if (!info.has_id()) {
    return Failure("Expecting SlaveInfo to have a SlaveID");
  }

  Operation<Registry>* operation = new Readmit(info);
  operations.push_back(operation);
  Future<bool> future = operation->future();
  if (!updating) {
    update();
  }
  return future;
}


Future<bool> RegistrarProcess::remove(const SlaveInfo& info)
{
  if (!info.has_id()) {
    return Failure("SlaveInfo is missing the 'id' field");
  }

  return recover()
    .then(defer(self(), &Self::_remove, info));
}


Future<bool> RegistrarProcess::_remove(
    const SlaveInfo& info)
{
  CHECK_SOME(variable);

  if (!info.has_id()) {
    return Failure("Expecting SlaveInfo to have a SlaveID");
  }

  Operation<Registry>* operation = new Remove(info);
  operations.push_back(operation);
  Future<bool> future = operation->future();
  if (!updating) {
    update();
  }
  return future;
}


void RegistrarProcess::update()
{
  if (operations.empty()) {
    return; // No-op.
  }

  CHECK(!updating);

  updating = true;

  LOG(INFO) << "Attempting to update the 'registry'";

  CHECK_SOME(variable);

  Variable<Registry> variable_ = variable.get();

  foreach (Operation<Registry>* operation, operations) {
    const Result<Registry>& registry = (*operation)(variable_.get());
    if (registry.isSome()) {
      variable_ = variable_.mutate(registry.get());
    }
  }

  // TODO(benh): Add a timeout so we don't wait forever.

  // Perform the store!
  state->store(variable_)
    .onAny(defer(self(), &Self::_update, lambda::_1, operations));

  // Clear the operations, _update will transition the Promises!
  operations.clear();
}


void RegistrarProcess::_update(
    const Future<Option<Variable<Registry> > >& store,
    deque<Operation<Registry>*> operations)
{
  updating = false;

  // Set the variable if the storage operation succeeded.
  if (!store.isReady()) {
    LOG(ERROR) << "Failed to update 'registry': "
               << (store.isFailed() ? store.failure() : "discarded");
  } else if (store.get().isNone()) {
    LOG(WARNING) << "Failed to update 'registry': version mismatch";
  } else {
    LOG(INFO) << "Successfully updated 'registry'";
    variable = store.get().get();
  }

  // Remove the operations.
  while (!operations.empty()) {
    Operation<Registry>* operation = operations.front();
    operations.pop_front();

    if (!store.isReady()) {
      operation->fail("Failed to update 'registry': " +
          (store.isFailed() ? store.failure() : "discarded"));
    } else {
      if (store.get().isNone()) {
        operation->fail("Failed to update 'registry': version mismatch");
      } else {
        operation->set();
      }
    }

    delete operation;
  }

  operations.clear();

  if (!this->operations.empty()) {
    update();
  }
}


Registrar::Registrar(State* state)
{
  process = new RegistrarProcess(state);
  spawn(process);
}


Registrar::~Registrar()
{
  terminate(process);
  wait(process);
}


Future<bool> Registrar::admit(const SlaveInfo& info)
{
  return dispatch(process, &RegistrarProcess::admit, info);
}


Future<bool> Registrar::readmit(const SlaveInfo& info)
{
  return dispatch(process, &RegistrarProcess::readmit, info);
}


Future<bool> Registrar::remove(const SlaveInfo& info)
{
  return dispatch(process, &RegistrarProcess::remove, info);
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
