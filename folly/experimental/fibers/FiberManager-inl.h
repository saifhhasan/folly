/*
 * Copyright 2015 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <cassert>

#include <folly/Memory.h>
#include <folly/Optional.h>
#include <folly/Portability.h>
#include <folly/ScopeGuard.h>
#include <folly/experimental/fibers/Baton.h>
#include <folly/experimental/fibers/Fiber.h>
#include <folly/experimental/fibers/Promise.h>
#include <folly/experimental/fibers/LoopController.h>
#include <folly/futures/Try.h>

namespace folly { namespace fibers {

inline void FiberManager::ensureLoopScheduled() {
  if (isLoopScheduled_) {
    return;
  }

  isLoopScheduled_ = true;
  loopController_->schedule();
}

inline void FiberManager::runReadyFiber(Fiber* fiber) {
  assert(fiber->state_ == Fiber::NOT_STARTED ||
         fiber->state_ == Fiber::READY_TO_RUN);
  currentFiber_ = fiber;

  while (fiber->state_ == Fiber::NOT_STARTED ||
         fiber->state_ == Fiber::READY_TO_RUN) {
    activeFiber_ = fiber;
    jumpContext(&mainContext_, &fiber->fcontext_, fiber->data_);
    if (fiber->state_ == Fiber::AWAITING_IMMEDIATE) {
      try {
        immediateFunc_();
      } catch (...) {
        exceptionCallback_(std::current_exception(), "running immediateFunc_");
      }
      immediateFunc_ = nullptr;
      fiber->state_ = Fiber::READY_TO_RUN;
    }
  }

  if (fiber->state_ == Fiber::AWAITING) {
    awaitFunc_(*fiber);
    awaitFunc_ = nullptr;
  } else if (fiber->state_ == Fiber::INVALID) {
    assert(fibersActive_ > 0);
    --fibersActive_;
    // Making sure that task functor is deleted once task is complete.
    // NOTE: we must do it on main context, as the fiber is not
    // running at this point.
    fiber->func_ = nullptr;
    fiber->resultFunc_ = nullptr;
    if (fiber->finallyFunc_) {
      try {
        fiber->finallyFunc_();
      } catch (...) {
        exceptionCallback_(std::current_exception(), "running finallyFunc_");
      }
      fiber->finallyFunc_ = nullptr;
    }
    fiber->localData_.reset();

    if (fibersPoolSize_ < options_.maxFibersPoolSize) {
      fibersPool_.push_front(*fiber);
      ++fibersPoolSize_;
    } else {
      delete fiber;
      assert(fibersAllocated_ > 0);
      --fibersAllocated_;
    }
  }
  currentFiber_ = nullptr;
}

inline bool FiberManager::loopUntilNoReady() {
  SCOPE_EXIT {
    isLoopScheduled_ = false;
    currentFiberManager_ = nullptr;
  };

  currentFiberManager_ = this;

  bool hadRemoteFiber = true;
  while (hadRemoteFiber) {
    hadRemoteFiber = false;

    while (!readyFibers_.empty()) {
      auto& fiber = readyFibers_.front();
      readyFibers_.pop_front();
      runReadyFiber(&fiber);
    }

    remoteReadyQueue_.sweep(
      [this, &hadRemoteFiber] (Fiber* fiber) {
        runReadyFiber(fiber);
        hadRemoteFiber = true;
      }
    );

    remoteTaskQueue_.sweep(
      [this, &hadRemoteFiber] (RemoteTask* taskPtr) {
        std::unique_ptr<RemoteTask> task(taskPtr);
        auto fiber = getFiber();
        if (task->localData) {
          fiber->localData_ = *task->localData;
        }

        fiber->setFunction(std::move(task->func));
        fiber->data_ = reinterpret_cast<intptr_t>(fiber);
        runReadyFiber(fiber);
        hadRemoteFiber = true;
      }
    );
  }

  return fibersActive_ > 0;
}

// We need this to be in a struct, not inlined in addTask, because clang crashes
// otherwise.
template <typename F>
struct FiberManager::AddTaskHelper {
  class Func;

  static constexpr bool allocateInBuffer =
    sizeof(Func) <= Fiber::kUserBufferSize;

  class Func {
   public:
    Func(F&& func, FiberManager& fm) :
        func_(std::forward<F>(func)), fm_(fm) {}

    void operator()() {
      try {
        func_();
      } catch (...) {
        fm_.exceptionCallback_(std::current_exception(),
                               "running Func functor");
      }
      if (allocateInBuffer) {
        this->~Func();
      } else {
        delete this;
      }
    }

   private:
    F func_;
    FiberManager& fm_;
  };
};

template <typename F>
void FiberManager::addTask(F&& func) {
  typedef AddTaskHelper<F> Helper;

  auto fiber = getFiber();
  if (currentFiber_) {
    fiber->localData_ = currentFiber_->localData_;
  }

  if (Helper::allocateInBuffer) {
    auto funcLoc = static_cast<typename Helper::Func*>(fiber->getUserBuffer());
    new (funcLoc) typename Helper::Func(std::forward<F>(func), *this);

    fiber->setFunction(std::ref(*funcLoc));
  } else {
    auto funcLoc = new typename Helper::Func(std::forward<F>(func), *this);

    fiber->setFunction(std::ref(*funcLoc));
  }

  fiber->data_ = reinterpret_cast<intptr_t>(fiber);
  readyFibers_.push_back(*fiber);

  ensureLoopScheduled();
}

template <typename F>
void FiberManager::addTaskRemote(F&& func) {
  auto task = [&]() {
    auto currentFm = getFiberManagerUnsafe();
    if (currentFm && currentFm->currentFiber_) {
      return folly::make_unique<RemoteTask>(
        std::forward<F>(func),
        currentFm->currentFiber_->localData_);
    }
    return folly::make_unique<RemoteTask>(std::forward<F>(func));
  }();
  if (remoteTaskQueue_.insertHead(task.release())) {
    loopController_->scheduleThreadSafe();
  }
}

template <typename X>
struct IsRvalueRefTry { static const bool value = false; };
template <typename T>
struct IsRvalueRefTry<folly::Try<T>&&> { static const bool value = true; };

// We need this to be in a struct, not inlined in addTaskFinally, because clang
// crashes otherwise.
template <typename F, typename G>
struct FiberManager::AddTaskFinallyHelper {
  class Func;
  class Finally;

  typedef typename std::result_of<F()>::type Result;

  static constexpr bool allocateInBuffer =
    sizeof(Func) + sizeof(Finally) <= Fiber::kUserBufferSize;

  class Finally {
   public:
    Finally(G&& finally,
            FiberManager& fm) :
        finally_(std::move(finally)),
        fm_(fm) {
    }

    void operator()() {
      try {
        finally_(std::move(*result_));
      } catch (...) {
        fm_.exceptionCallback_(std::current_exception(),
                               "running Finally functor");
      }

      if (allocateInBuffer) {
        this->~Finally();
      } else {
        delete this;
      }
    }

   private:
    friend class Func;

    G finally_;
    folly::Optional<folly::Try<Result>> result_;
    FiberManager& fm_;
  };

  class Func {
   public:
    Func(F&& func, Finally& finally) :
        func_(std::move(func)), result_(finally.result_) {}

    void operator()() {
      result_ = folly::makeTryFunction(std::move(func_));

      if (allocateInBuffer) {
        this->~Func();
      } else {
        delete this;
      }
    }

   private:
    F func_;
    folly::Optional<folly::Try<Result>>& result_;
  };
};

template <typename F, typename G>
void FiberManager::addTaskFinally(F&& func, G&& finally) {
  typedef typename std::result_of<F()>::type Result;

  static_assert(
    IsRvalueRefTry<typename FirstArgOf<G>::type>::value,
    "finally(arg): arg must be Try<T>&&");
  static_assert(
    std::is_convertible<
      Result,
      typename std::remove_reference<
        typename FirstArgOf<G>::type
      >::type::element_type
    >::value,
    "finally(Try<T>&&): T must be convertible from func()'s return type");

  auto fiber = getFiber();
  if (currentFiber_) {
    fiber->localData_ = currentFiber_->localData_;
  }

  typedef AddTaskFinallyHelper<F,G> Helper;

  if (Helper::allocateInBuffer) {
    auto funcLoc = static_cast<typename Helper::Func*>(
      fiber->getUserBuffer());
    auto finallyLoc = static_cast<typename Helper::Finally*>(
      static_cast<void*>(funcLoc + 1));

    new (finallyLoc) typename Helper::Finally(std::move(finally), *this);
    new (funcLoc) typename Helper::Func(std::move(func), *finallyLoc);

    fiber->setFunctionFinally(std::ref(*funcLoc), std::ref(*finallyLoc));
  } else {
    auto finallyLoc = new typename Helper::Finally(std::move(finally), *this);
    auto funcLoc = new typename Helper::Func(std::move(func), *finallyLoc);

    fiber->setFunctionFinally(std::ref(*funcLoc), std::ref(*finallyLoc));
  }

  fiber->data_ = reinterpret_cast<intptr_t>(fiber);
  readyFibers_.push_back(*fiber);

  ensureLoopScheduled();
}

template <typename F>
typename std::result_of<F()>::type
FiberManager::runInMainContext(F&& func) {
  return runInMainContextHelper(std::forward<F>(func));
}

template <typename F>
inline typename std::enable_if<
  !std::is_same<typename std::result_of<F()>::type, void>::value,
  typename std::result_of<F()>::type>::type
FiberManager::runInMainContextHelper(F&& func) {
  if (UNLIKELY(activeFiber_ == nullptr)) {
    return func();
  }

  typedef typename std::result_of<F()>::type Result;

  folly::Try<Result> result;
  auto f = [&func, &result]() mutable {
    result = folly::makeTryFunction(std::forward<F>(func));
  };

  immediateFunc_ = std::ref(f);
  activeFiber_->preempt(Fiber::AWAITING_IMMEDIATE);

  return std::move(result.value());
}

template <typename F>
inline typename std::enable_if<
  std::is_same<typename std::result_of<F()>::type, void>::value,
  void>::type
FiberManager::runInMainContextHelper(F&& func) {
  if (UNLIKELY(activeFiber_ == nullptr)) {
    func();
    return;
  }

  immediateFunc_ = std::ref(func);
  activeFiber_->preempt(Fiber::AWAITING_IMMEDIATE);
}

inline FiberManager& FiberManager::getFiberManager() {
  assert(currentFiberManager_ != nullptr);
  return *currentFiberManager_;
}

inline FiberManager* FiberManager::getFiberManagerUnsafe() {
  return currentFiberManager_;
}

inline bool FiberManager::hasActiveFiber() const {
  return activeFiber_ != nullptr;
}

template <typename T>
T& FiberManager::local() {
  if (currentFiber_) {
    return currentFiber_->localData_.get<T>();
  }
  return localThread<T>();
}

template <typename T>
T& FiberManager::localThread() {
  static thread_local T t;
  return t;
}

template <typename F>
typename FirstArgOf<F>::type::value_type
inline await(F&& func) {
  typedef typename FirstArgOf<F>::type::value_type Result;

  folly::Try<Result> result;

  Baton baton;
  baton.wait([&func, &result, &baton]() mutable {
      func(Promise<Result>(result, baton));
    });

  return folly::moveFromTry(std::move(result));
}

}}