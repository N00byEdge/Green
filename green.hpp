// Epic includes
#include <array>
#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// Messy includes
#include <ucontext.h>
#include <sys/time.h>

namespace Green {
  constexpr auto timerPeriod = 100;
  static sigset_t timerLock;

  struct TimerGuard {
    TimerGuard() { sigprocmask(SIG_BLOCK, &timerLock, nullptr); }
    ~TimerGuard() { sigprocmask(SIG_UNBLOCK, &timerLock, nullptr); }
  };

  constexpr auto stackSize = 0x1000;
  using Stack = std::array<char, stackSize>;
  inline ucontext_t mainContext;

  struct Fiber;
  std::deque<Fiber *> ready;
  inline Fiber *running = nullptr;

  struct Fiber {
    using Fptr = void(*)(void *);
    Fptr f = nullptr;
    std::unique_ptr<Stack> stack = std::make_unique<Stack>();
    std::unique_ptr<ucontext_t> context = [this] {
      auto ctx = std::make_unique<ucontext_t>();
      getcontext(ctx.get());
      ctx->uc_stack.ss_sp = stack.get();
      ctx->uc_stack.ss_size = stackSize;
      ctx->uc_stack.ss_flags = 0;
      makecontext(ctx.get(), run, 0);
      return ctx;
    }();

    Fiber *joining = nullptr;
    void *args;
    bool ended = false;
    Fiber() = default;

    ~Fiber() {
      if(stack) // If not OS fiber
        join();
      else      // If OS fiber, release context without deleting
        context.release();
    }
    
    static std::unique_ptr<Fiber> start(Fptr f, void *args) {
      TimerGuard lock;
      auto fib = std::make_unique<Fiber>();
      fib->f = f;
      fib->args = args;
      ready.emplace_back(fib.get());
      return fib;
    }
    
    static void Yield() {
      TimerGuard tlock;
      if (ready.empty() || !running)
        return;
      auto suspended = std::exchange(running, ready.front());
      ready.pop_front();
      ready.emplace_back(suspended);
      swapcontext(suspended->context.get(), running->context.get());
    }

    void join() {
      TimerGuard lock;
      if(ended) return;
      joining = running;
      auto next = ready.front();
      ready.pop_front();
      auto suspended = std::exchange(running, next);
      swapcontext(suspended->context.get(), next->context.get());
    }

  private:
    static void run() {
      auto dis = running;

      (dis->f)(dis->args);

      TimerGuard tlock;
      auto next = [dis]() {
        if(dis->joining)
          return dis->joining;
        auto next = ready.front();
        ready.pop_front();
        return next;
      }();
      dis->ended = true;
      running = next;
      swapcontext(dis->context.get(), next->context.get());
    }
  };

  struct ConditionVariable {
    std::queue<Fiber *> waiting;
    void wait() {
      waiting.emplace(std::exchange(running, ready.back()));
      ready.pop_back();
      Fiber::Yield();
    }

    void notify_one() {
      ready.emplace_back(waiting.front());
      waiting.pop();
    }
  };

  void timerHandler(int) {
    Green::Fiber::Yield();
  }

  struct Mutex {
    std::atomic<bool> taken;
    bool tryLock() {
      bool expected = false;
      return taken.compare_exchange_weak(expected, true, std::memory_order_release, std::memory_order_relaxed);
    }
    void lock() {
      while(!tryLock())
        Green::Fiber::Yield();
    }
    void unlock() { taken.store(false); }
  };

  inline Fiber mainFiber = []{
    running = &mainFiber;
    ucontext_t fem;
    getcontext(&fem);
    Green::mainContext = fem;

    sigemptyset(&timerLock);
    sigaddset(&timerLock, SIGVTALRM);
    struct sigaction act{timerHandler};
    if(sigaction(SIGVTALRM, &act, NULL))
      throw std::runtime_error{"lel wtf could not register handler"};
    timeval interval{0, timerPeriod};
    itimerval period{interval, interval};
    setitimer(ITIMER_VIRTUAL, &period, NULL);

    return Fiber{ nullptr
                , nullptr
                , std::unique_ptr<ucontext_t>(&mainContext)};
  }();
}
