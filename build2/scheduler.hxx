// file      : build2/scheduler.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_SCHEDULER_HXX
#define BUILD2_SCHEDULER_HXX

#include <list>
#include <mutex>
#include <tuple>
#include <atomic>
#include <type_traits>        // aligned_storage, etc
#include <condition_variable>

#include <build2/types.hxx>
#include <build2/utility.hxx>

namespace build2
{
  // Scheduler of tasks and threads. Works best for "substantial" tasks (e.g.,
  // running a process), where in comparison thread synchronization overhead
  // is negligible.
  //
  // A thread (called "master") may need to perform several tasks which can be
  // done in parallel (e.g., update all the prerequisites or run all the
  // tests). To acomplish this, the master, via a call to async(), can ask the
  // scheduler to run a task in another thread (called "helper"). If a helper
  // is available, then the task is executed asynchronously by such a helper.
  // Otherwise, the task is (normally) executed synchronously as part of the
  // wait() call below. However, in certain cases (serial execution or full
  // queue), the task may be executed synchronously as part of the async()
  // call itself. Once the master thread has scheduled all the tasks, it calls
  // wait() to await for their completion.
  //
  // The scheduler makes sure that only a certain number of threads (for
  // example, the number of available hardware threads) are "active" at any
  // given time. When a master thread calls wait(), it is "suspended" until
  // all its asynchronous tasks are completed (at which point it becomes
  // "ready"). A suspension of a master results in either another ready master
  // being "resumed" or another helper thread becoming available.
  //
  // On completion of a task a helper thread returns to the scheduler which
  // can again lead either to a ready master being resumed (in which case the
  // helper is suspended) or the helper becoming available to perform another
  // task.
  //
  // Note that suspended threads are not reused as helpers. Rather, a new
  // helper thread is always created if none is available. This is done to
  // allow a ready master to continue as soon as possible. If it were reused
  // as a helper, then it could be blocked on a nested wait() further down the
  // stack. All this means that the number of threads created by the scheduler
  // will normally exceed the maximum active allowed.
  //
  class scheduler
  {
  public:
    using atomic_count = std::atomic<size_t>;

    // F should return void and not throw any exceptions. The way the result
    // of a task is communicated back to the master thread is ad hoc, usually
    // via "out" arguments. Such result(s) can only be retrieved by the master
    // once its task count reaches the start count.
    //
    // The argument passing semantics is the same as for std::thread. In
    // particular, lvalue-references are passed as copies (use ref()/cref()
    // for the by-reference semantics), except the case where the task is
    // executed synchronously and as part of the async() call itself (this
    // subtlety can become important when passing shared locks; you would
    // only want it to be copied if the task is queued).
    //
    // Return true if the task was queued and false if it was executed
    // synchronously.
    //
    // If the scheduler is shutdown, throw system_error(ECANCELED).
    //
    template <typename F, typename... A>
    bool
    async (size_t start_count, atomic_count& task_count, F&&, A&&...);

    template <typename F, typename... A>
    bool
    async (atomic_count& task_count, F&& f, A&&... a)
    {
      return async (0, task_count, forward<F> (f), forward<A> (a)...);
    }

    // Wait until the task count reaches the start count or less. If the
    // scheduler is shutdown while waiting, throw system_error(ECANCELED).
    // Return the value of task count. Note that this is a synchronizaiton
    // point (i.e., the task count is checked with memory_order_acquire).
    //
    // Note that it is valid to wait on another thread's task count (that is,
    // without making any async() calls in this thread). However, if the start
    // count differs from the one passed to async(), then whomever sets the
    // start count to this alternative value must also call resume() below
    // in order to signal waiting threads.
    //
    // Note also that in this case (waiting on someone else's start count),
    // the async() call could execute the tasks synchronously without ever
    // incrementing the task count. Thus if waiting on another thread's start
    // count starts before/during async() calls, then it must be "gated" with
    // an alternative (lower) start count.
    //
    // Finally, if waiting on someone else's start count, it may be unsafe
    // (from the deadlock's point of view) to continue working through our own
    // queue (i.e., we may block waiting on a task that has been queued before
    // us which in turn may end up waiting on "us").
    //
    enum work_queue
    {
      work_none, // Don't work own queue.
      work_one,  // Work own queue rechecking the task count after every task.
      work_all   // Work own queue before rechecking the task count.
    };

    size_t
    wait (size_t start_count,
          const atomic_count& task_count,
          work_queue = work_all);

    size_t
    wait (const atomic_count& task_count, work_queue wq = work_all)
    {
      return wait (0, task_count, wq);
    }

    // Resume threads waiting on this task count.
    //
    void
    resume (const atomic_count& task_count);

    // An active thread that is about to wait for potentially significant time
    // on something other than task_count (e.g., mutex, condition variable)
    // should deactivate itself with the scheduler and then reactivate once
    // done waiting.
    //
    void
    deactivate ();

    void
    activate (bool collision = false);

    // Sleep for the specified duration, deactivating the thread before going
    // to sleep and re-activating it after waking up (which means this
    // function may sleep potentially significantly longer than requested).
    //
    void
    sleep (const duration&);

    // Startup and shutdown.
    //
  public:
    // Unless already shut down, call shutdown() but ignore errors.
    //
    ~scheduler ();

    // Create a shut down scheduler.
    //
    scheduler () = default;

    // Create a started up scheduler.
    //
    // The initial active argument is the number of threads to assume are
    // already active (e.g., the calling thread). It must not be 0 (since
    // someone has to schedule the first task).
    //
    // If the maximum threads or task queue depth arguments are unspecified,
    // then appropriate defaults are used.
    //
    explicit
    scheduler (size_t max_active,
               size_t init_active = 1,
               size_t max_threads = 0,
               size_t queue_depth = 0,
               optional<size_t> max_stack = nullopt)
    {
      startup (max_active, init_active, max_threads, queue_depth, max_stack);
    }

    // Start the scheduler.
    //
    void
    startup (size_t max_active,
             size_t init_active = 1,
             size_t max_threads = 0,
             size_t queue_depth = 0,
             optional<size_t> max_stack = nullopt);

    // Return true if the scheduler was started up.
    //
    // Note: can only be called from threads that have observed creation,
    // startup, or shutdown.
    //
    bool
    started () const {return !shutdown_;}

    // Tune a started up scheduler.
    //
    // Currently one cannot increase the number of max_active. Pass 0 to
    // restore the initial value.
    //
    // Note that tuning can only be done while the scheduler is inactive, that
    // is, no threads are executing a task or are suspended. For example, in a
    // setup with a single initial active thread that would be after a return
    // from the top-level wait() call.
    //
    void
    tune (size_t max_active);

    // Return true if the scheduler is configured to run tasks serially.
    //
    // Note: can only be called from threads that have observed startup.
    //
    bool
    serial () const {return max_active_ == 1;}

    // Wait for all the helper threads to terminate. Throw system_error on
    // failure. Note that the initially active threads are not waited for.
    // Return scheduling statistics.
    //
    struct stat
    {
      size_t thread_max_active     = 0; // max # of active threads allowed.
      size_t thread_max_total      = 0; // max # of total threads allowed.
      size_t thread_helpers        = 0; // # of helper threads created.
      size_t thread_max_waiting    = 0; // max # of waiters at any time.

      size_t task_queue_depth      = 0; // # of entries in a queue (capacity).
      size_t task_queue_full       = 0; // # of times task queue was full.
      size_t task_queue_remain     = 0; // # of tasks remaining in queue.

      size_t wait_queue_slots      = 0; // # of wait slots (buckets).
      size_t wait_queue_collisions = 0; // # of times slot had been occupied.
    };

    stat
    shutdown ();

    // Progress monitoring.
    //
    // Setting and clearing of the monitor is not thread-safe. That is, it
    // should be set before any tasks are queued and cleared after all of
    // them have completed.
    //
    // The counter must go in one direction, either increasing or decreasing,
    // and should contain the initial value during the call. Zero threshold
    // value is reserved.
    //
    struct monitor_guard
    {
      explicit
      monitor_guard (scheduler* s = nullptr): s_ (s) {}
      monitor_guard (monitor_guard&& x): s_ (x.s_) {x.s_ = nullptr;}
      monitor_guard& operator= (monitor_guard&& x)
      {
        if (&x != this)
        {
          s_ = x.s_;
          x.s_ = nullptr;
        }
        return *this;
      }

      ~monitor_guard ()
      {
        if (s_ != nullptr)
        {
          lock l (s_->wait_idle ()); // See monitor() for details.
          s_->monitor_count_ = nullptr;
          s_->monitor_func_  = nullptr;
        }
      }

      explicit operator bool () const {return s_ != nullptr;}

    private:
      scheduler* s_;
    };

    monitor_guard
    monitor (atomic_count&, size_t threshold, function<size_t (size_t)>);

    // If initially active thread(s) (besides the one that calls startup())
    // exist before the call to startup(), then they must call join() before
    // executing any tasks. The two common cases where you don't have to call
    // join are a single active thread that calls startup()/shutdown() or
    // active thread(s) that are created after startup().
    //
    void
    join ()
    {
      assert (task_queue_ = nullptr);

      // Lock the mutex to make sure the values set in startup() are visible
      // in this thread.
      //
      lock l (mutex_);
    }

    // If initially active thread(s) participate in multiple schedulers and/or
    // sessions (intervals between startup() and shutdown()), then they must
    // call leave() before joining another scheduler/session. Note that this
    // applies to the active thread that calls shutdown(). Note that a thread
    // can only participate in one scheduler at a time.
    //
    void
    leave ()
    {
      task_queue_ = nullptr;
    }

    // Return the number of hardware threads or 0 if unable to determine.
    //
    static size_t
    hardware_concurrency ()
    {
      return std::thread::hardware_concurrency ();
    }

    // Return a prime number that can be used as a lock shard size that's
    // appropriate for the scheduler's concurrency. Use power of two values
    // for mul for higher-contention shards and for div for lower-contention
    // ones. Always return 1 for serial execution.
    //
    // Note: can only be called from threads that have observed startup.
    //
    size_t
    shard_size (size_t mul = 1, size_t div = 1) const;

    // Assuming all the task have been executed, busy-wait for all the threads
    // to become idle. Return the lock over the scheduler mutex. Normally you
    // don't need to call this function directly.
    //
    using lock = std::unique_lock<std::mutex>;

    lock
    wait_idle ();

  private:
    void
    activate_helper (lock&);

    void
    create_helper (lock&);

    // We restrict ourselves to a single pointer as an argument in hope of
    // a small object optimization. Return NULL.
    //
    // Note that the return type is void* to make the function usable with
    // pthreads (see scheduler.cxx for details).
    //
    static void*
    helper (void*);

    size_t
    suspend (size_t start_count, const atomic_count& task_count);

    // Task encapsulation.
    //
    template <typename F, typename... A>
    struct task_type
    {
      using func_type = std::decay_t<F>;
      using args_type = std::tuple<std::decay_t<A>...>;

      atomic_count* task_count;
      size_t start_count;
      func_type func;
      args_type args;

      template <size_t... i>
      void
      thunk (std::index_sequence<i...>) noexcept
      {
        move (func) (std::get<i> (move (args))...);
      }
    };

    template <typename F, typename... A>
    static void
    task_thunk (scheduler&, lock&, void*);

    template <typename T>
    static std::decay_t<T>
    decay_copy (T&& x) {return forward<T> (x);}

  private:
    // Monitor.
    //
    atomic_count*              monitor_count_ = nullptr;  // NULL if not used.
    atomic_count               monitor_tshold_;           // 0 means locked.
    size_t                     monitor_init_;             // Initial count.
    function<size_t (size_t)>  monitor_func_;

    std::mutex mutex_;
    bool shutdown_ = true;  // Shutdown flag.

    optional<size_t> max_stack_;

    // The constraints that we must maintain:
    //
    //                  active <= max_active
    // (init_active + helpers) <= max_threads (soft; see activate_helper())
    //
    // Note that the first three are immutable between startup() and
    // shutdown() so can be accessed without a lock (but see join()).
    //
    size_t init_active_ = 0; // Initially active threads.
    size_t max_active_  = 0; // Maximum number of active threads.
    size_t max_threads_ = 0; // Maximum number of total threads.

    size_t helpers_     = 0; // Number of helper threads created so far.

    // Every thread that we manage must be accounted for in one of these
    // counters. And their sum should equal (init_active + helpers).
    //
    size_t active_   = 0;  // Active master threads executing a task.
    size_t idle_     = 0;  // Idle helper threads waiting for a task.
    size_t waiting_  = 0;  // Suspended master threads waiting for their tasks.
    size_t ready_    = 0;  // Ready master thread waiting to become active.
    size_t starting_ = 0;  // Helper threads starting up.

    // Original values (as specified during startup) that can be altered via
    // tuning.
    //
    size_t orig_max_active_ = 0;

    std::condition_variable idle_condv_;  // Idle helpers queue.
    std::condition_variable ready_condv_; // Ready masters queue.

    // Statistics counters.
    //
    size_t stat_max_waiters_;
    size_t stat_wait_collisions_;

    // Progress counter.
    //
    // We increment it for each active->waiting->ready->active transition
    // and it is used for deadlock detection (see deactivate()).
    //
    size_t progress_;

    // Wait queue.
    //
    // A wait slot blocks a bunch of threads. When they are (all) unblocked,
    // they re-examine their respective conditions and either carry on or
    // block again.
    //
    // The wait queue is a shard of slots. A thread picks a slot based on the
    // address of its task count variable. How many slots do we need? This
    // depends on the number of waiters that we can have which cannot be
    // greater than the total number of threads.
    //
    // The pointer to the task count is used to identify the already waiting
    // group of threads for collision statistics.
    //
    struct wait_slot
    {
      std::mutex mutex;
      std::condition_variable condv;
      size_t waiters = 0;
      const atomic_count* task_count;
      bool shutdown = true;
    };

    size_t wait_queue_size_; // Proportional to max_threads.
    unique_ptr<wait_slot[]> wait_queue_;

    // Task queue.
    //
    // Each queue has its own mutex plus we have an atomic total count of the
    // queued tasks. Note that it should only be modified while holding one
    // of the queue locks.
    //
    atomic_count queued_task_count_;

    // For now we only support trivially-destructible tasks.
    //
    struct task_data
    {
      std::aligned_storage<sizeof (void*) * 8>::type data;
      void (*thunk) (scheduler&, lock&, void*);
    };

    // We have two requirements: Firstly, we want to keep the master thread
    // (the one that called wait()) busy working though its own queue for as
    // long as possible before (if at all) it "reincarnates" as a helper. The
    // main reason for this is the limited number of helpers we can create.
    //
    // Secondly, we don't want to block wait() longer than necessary since the
    // master thread can do some work with the result. Plus, overall, we want
    // to "unwind" task hierarchies as soon as possible since they hold up
    // resources such as thread's stack. All this means that the master thread
    // can only work through tasks that it has queued at this "level" of the
    // async()/wait() calls since we know that wait() cannot return until
    // they are done.
    //
    // To satisfy the first requirement, the master and helper threads get the
    // tasks from different ends of the queue: master from the back while
    // helpers from the front. And the master always adds new tasks to the
    // back.
    //
    // To satisfy the second requirement, the master thread stores the index
    // of the first task it has queued at this "level" and makes sure it
    // doesn't try to deque any task beyond that.
    //
    size_t task_queue_depth_; // Multiple of max_active.

    struct task_queue
    {
      std::mutex mutex;
      bool shutdown = false;

      size_t stat_full = 0; // Number of times push() returned NULL.

      // Our task queue is circular with head being the index of the first
      // element and tail -- of the last. Since this makes the empty and one
      // element cases indistinguishable, we also keep the size.
      //
      // The mark is an index somewhere between (figuratively speaking) head
      // and tail, if enabled. If the mark is hit, then it is disabled until
      // the queue becomes empty or it is reset by a push.
      //
      size_t head = 0;
      size_t mark = 0;
      size_t tail = 0;
      size_t size = 0;

      unique_ptr<task_data[]> data;

      task_queue (size_t depth): data (new task_data[depth]) {}
    };

    // Task queue API. Expects the queue mutex to be locked.
    //

    // Push a new task to the queue returning a pointer to the task data to be
    // filled or NULL if the queue is full.
    //
    task_data*
    push (task_queue& tq)
    {
      size_t& s (tq.size);
      size_t& t (tq.tail);
      size_t& m (tq.mark);

      if (s != task_queue_depth_)
      {
        //                                         normal  wrap empty
        //                                         |       |    |
        t = s != 0 ? (t != task_queue_depth_ - 1 ? t + 1 : 0) : t;
        s++;

        if (m == task_queue_depth_) // Enable the mark if first push.
          m = t;

        queued_task_count_.fetch_add (1, std::memory_order_release);
        return &tq.data[t];
      }

      return nullptr;
    }

    bool
    empty_front (task_queue& tq) const {return tq.size == 0;}

    void
    pop_front (task_queue& tq, lock& ql)
    {
      size_t& s (tq.size);
      size_t& h (tq.head);
      size_t& m (tq.mark);

      bool a (h == m); // Adjust mark?
      task_data& td (tq.data[h]);

      //                                         normal  wrap empty
      //                                         |       |    |
      h = s != 1 ? (h != task_queue_depth_ - 1 ? h + 1 : 0) : h;

      if (--s == 0 || a)
        m = h; // Reset or adjust the mark.

      execute (ql, td);
    }

    bool
    empty_back (task_queue& tq) const
    {
      return tq.size == 0 || tq.mark == task_queue_depth_;
    }

    void
    pop_back (task_queue& tq, lock& ql)
    {
      size_t& s (tq.size);
      size_t& t (tq.tail);
      size_t& m (tq.mark);

      bool a (t == m); // Adjust mark?

      task_data& td (tq.data[t]);

      // Save the old queue mark and disable it in case the task we are about
      // to run adds sub-tasks. The first push(), if any, will reset it.
      //
      size_t om (m);
      m = task_queue_depth_;

      //                     normal  wrap                     empty
      //                     |       |                        |
      t = s != 1 ? (t != 0 ? t - 1 : task_queue_depth_ - 1) : t;
      --s;

      execute (ql, td);

      // Restore the old mark (which we might have to adjust).
      //
      if (s == 0)
        m = t;                 // Reset the mark.
      else if (a)
        m = task_queue_depth_; // Disable the mark.
      else
        // What happens if head goes past the old mark? In this case we will
        // get into the empty queue state before we end up making any (wrong)
        // decisions based on this value. Unfortunately there is no way to
        // detect this (and do some sanity asserts) since things can wrap
        // around.
        //
        // To put it another way, the understanding here is that after the
        // task returns we will either have an empty queue or there will still
        // be tasks between the old mark and the current tail, something along
        // these lines:
        //
        // OOOOOXXXXOOO
        //   |  |  |
        //   m  h  t
        //
        m = om;
    }

    void
    execute (lock& ql, task_data& td)
    {
      queued_task_count_.fetch_sub (1, std::memory_order_release);

      // The thunk moves the task data to its stack, releases the lock,
      // and continues to execute the task.
      //
      td.thunk (*this, ql, &td.data);

      // See if we need to call the monitor (see also the serial version
      // in async()).
      //
      if (monitor_count_ != nullptr)
      {
        // Note that we don't care if we don't see the updated values right
        // away.
        //
        if (size_t t = monitor_tshold_.load (memory_order_relaxed))
        {
          // "Lock" the monitor by setting threshold to 0.
          //
          if (monitor_tshold_.compare_exchange_strong (
                t,
                0,
                memory_order_release,
                memory_order_relaxed))
          {
            // Now we are the only ones messing with this.
            //
            size_t v (monitor_count_->load (memory_order_relaxed));

            if (v != monitor_init_)
            {
              // See which direction we are going.
              //
              if (v > monitor_init_ ? (v >= t) : (v <= t))
                t = monitor_func_ (v);
            }

            monitor_tshold_.store (t, memory_order_release);
          }
        }
      }

      ql.lock ();
    }

    // Each thread has its own queue which are stored in this list.
    //
    std::list<task_queue> task_queues_;

    // TLS cache of thread's task queue.
    //
    static
#ifdef __cpp_thread_local
    thread_local
#else
    __thread
#endif
    task_queue* task_queue_;

    task_queue&
    create_queue ();
  };
}

#include <build2/scheduler.txx>

#endif // BUILD2_SCHEDULER_HXX
