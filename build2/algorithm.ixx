// file      : build2/algorithm.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/rule.hxx>
#include <build2/context.hxx>

namespace build2
{
  inline const target*
  find_adhoc_member (const target& t, const target_type& tt)
  {
    const target* m (t.member);
    for (; m != nullptr && !m->is_a (tt); m = m->member) ;
    return m;
  }

  inline const target&
  search (const target& t, const prerequisite& p)
  {
    assert (phase == run_phase::match);

    const target* r (p.target.load (memory_order_consume));

    if (r == nullptr)
      r = &search_custom (p, search (t, p.key ()));

    return *r;
  }

  inline const target*
  search_existing (const prerequisite& p)
  {
    assert (phase == run_phase::match || phase == run_phase::execute);

    const target* r (p.target.load (memory_order_consume));

    if (r == nullptr)
    {
      r = search_existing (p.key ());

      if (r != nullptr)
        search_custom (p, *r);
    }

    return r;
  }

  inline const target&
  search_custom (const prerequisite& p, const target& t)
  {
    assert (phase == run_phase::match || phase == run_phase::execute);

    const target* e (nullptr);
    if (!p.target.compare_exchange_strong (
          e, &t,
          memory_order_release,
          memory_order_consume))
      assert (e == &t);

    return t;
  }

  inline const target&
  search (const target& t, const target_type& tt, const prerequisite_key& k)
  {
    return search (
      t,
      prerequisite_key {
        k.proj, {&tt, k.tk.dir, k.tk.out, k.tk.name, k.tk.ext}, k.scope});
  }

  inline const target&
  search (const target& t,
          const target_type& type,
          const dir_path& dir,
          const dir_path& out,
          const string& name,
          const string* ext,
          const scope* scope,
          const optional<project_name>& proj)
  {
    return search (
      t,
      prerequisite_key {
        proj,
        {
          &type,
          &dir, &out, &name,
          ext != nullptr ? optional<string> (*ext) : nullopt
        },
        scope});
  }

  inline const target*
  search_existing (const target_type& type,
                   const dir_path& dir,
                   const dir_path& out,
                   const string& name,
                   const string* ext,
                   const scope* scope,
                   const optional<project_name>& proj)
  {
    return search_existing (
      prerequisite_key {
        proj,
        {
          &type,
          &dir, &out, &name,
          ext != nullptr ? optional<string> (*ext) : nullopt
        },
        scope});
  }

  template <typename T>
  inline const T&
  search (const target& t,
          const dir_path& dir,
          const dir_path& out,
          const string& name,
          const string* ext,
          const scope* scope)
  {
    return search (
      t, T::static_type, dir, out, name, ext, scope).template as<T> ();
  }

  target_lock
  lock_impl (action, const target&, optional<scheduler::work_queue>);

  void
  unlock_impl (action, target&, size_t);

  inline target_lock::
  target_lock (action_type a, target_type* t, size_t o)
      : action (a), target (t), offset (o)
  {
    if (target != nullptr)
    {
      prev = stack;
      stack = this;
    }
  }

  inline void target_lock::
  unlock ()
  {
    if (target != nullptr)
    {
      unlock_impl (action, *target, offset);

      assert (stack == this);
      stack = prev;
      target = nullptr;
    }
  }

  inline auto target_lock::
  release () -> data
  {
    data r {action, target, offset};

    if (target != nullptr)
    {
      assert (stack == this);
      stack = prev;
      target = nullptr;
    }

    return r;
  }

  inline target_lock::
  ~target_lock ()
  {
    unlock ();
  }

  inline target_lock::
  target_lock (target_lock&& x)
      : action (x.action), target (x.target), offset (x.offset)
  {
    if (target != nullptr)
    {
      assert (stack == &x);
      prev = x.prev;
      stack = this;

      x.target = nullptr;
    }
  }

  inline target_lock& target_lock::
  operator= (target_lock&& x)
  {
    if (this != &x)
    {
      assert (target == nullptr);

      action = x.action;
      target = x.target;
      offset = x.offset;

      if (target != nullptr)
      {
        assert (stack == &x);
        prev = x.prev;
        stack = this;

        x.target = nullptr;
      }
    }

    return *this;
  }

  inline const target_lock*
  dependency_cycle (action a, const target& t)
  {
    const target_lock* l (target_lock::stack);

    for (; l != nullptr; l = l->prev)
    {
      if (l->action == a && l->target == &t)
        break;
    }

    return l;
  }

  inline target_lock
  lock (action a, const target& t)
  {
    // We don't allow locking a target that has already been matched.
    //
    target_lock r (lock_impl (a, t, scheduler::work_none));
    assert (!r                                 ||
            r.offset == target::offset_touched ||
            r.offset == target::offset_tried);
    return r;
  }

  inline target_lock
  add_adhoc_member (action a, target& t, const target_type& tt, const char* s)
  {
    string n (t.name);

    if (s != nullptr)
    {
      n += '.';
      n += s;
    }

    return add_adhoc_member (a, t, tt, t.dir, t.out, n);
  }

  const rule_match*
  match_impl (action, target&, const rule* skip, bool try_match = false);

  recipe
  apply_impl (action, target&, const rule_match&);

  pair<bool, target_state>
  match (action, const target&, size_t, atomic_count*, bool try_match = false);

  inline target_state
  match (action a, const target& t, bool fail)
  {
    assert (phase == run_phase::match);

    target_state r (match (a, t, 0, nullptr).second);

    if (r != target_state::failed)
    {
      dependency_count.fetch_add (1, memory_order_relaxed);
      t[a].dependents.fetch_add (1, memory_order_release);
    }
    else if (fail)
      throw failed ();

    return r;
  }

  inline pair<bool, target_state>
  try_match (action a, const target& t, bool fail)
  {
    assert (phase == run_phase::match);

    pair<bool, target_state> r (
      match (a, t, 0, nullptr, true /* try_match */));

    if (r.first)
    {
      if (r.second != target_state::failed)
      {
        dependency_count.fetch_add (1, memory_order_relaxed);
        t[a].dependents.fetch_add (1, memory_order_release);
      }
      else if (fail)
        throw failed ();
    }

    return r;
  }

  inline bool
  match (action a, const target& t, unmatch um)
  {
    assert (phase == run_phase::match);

    target_state s (match (a, t, 0, nullptr).second);

    if (s == target_state::failed)
      throw failed ();

    switch (um)
    {
    case unmatch::none: break;
    case unmatch::unchanged:
      {
        if (s == target_state::unchanged)
          return true;

        break;
      }
    case unmatch::safe:
      {
        // Safe if unchanged or someone else is also a dependent.
        //
        if (s == target_state::unchanged                   ||
            t[a].dependents.load (memory_order_consume) != 0)
          return true;

        break;
      }
    }

    dependency_count.fetch_add (1, memory_order_relaxed);
    t[a].dependents.fetch_add (1, memory_order_release);

    return false;
  }

  inline target_state
  match_async (action a, const target& t,
               size_t sc, atomic_count& tc,
               bool fail)
  {
    assert (phase == run_phase::match);
    target_state r (match (a, t, sc, &tc).second);

    if (fail && !keep_going && r == target_state::failed)
      throw failed ();

    return r;
  }

  inline void
  set_recipe (target_lock& l, recipe&& r)
  {
    target::opstate& s ((*l.target)[l.action]);

    s.recipe = move (r);

    // If this is a noop recipe, then mark the target unchanged to allow for
    // some optimizations.
    //
    recipe_function** f (s.recipe.target<recipe_function*> ());

    if (f != nullptr && *f == &noop_action)
      s.state = target_state::unchanged;
    else
    {
      s.state = target_state::unknown;

      // This gets tricky when we start considering direct execution, etc. So
      // here seems like the best place to do it.
      //
      // We also ignore the group recipe since it is used for ad hoc groups
      // (which are not executed). Plus, group action means real recipe is in
      // the group so this also feels right conceptually.
      //
      // We also avoid increment this count twice for the same target if we
      // have both the inner and outer operations. In our model the outer
      // operation is either noop or it must delegate to the inner. While it's
      // possible the inner is noop while the outer is not, it is not very
      // likely. The alternative (trying to "merge" the count keeping track of
      // whether inner and/or outer is noop) gets hairy rather quickly.
      //
      if (l.action.inner ())
      {
        if (f == nullptr || *f != &group_action)
          target_count.fetch_add (1, memory_order_relaxed);
      }
    }
  }

  inline void
  match_recipe (target_lock& l, recipe r)
  {
    assert (phase == run_phase::match && l.target != nullptr);

    (*l.target)[l.action].rule = nullptr; // No rule.
    set_recipe (l, move (r));
    l.offset = target::offset_applied;
  }

  inline recipe
  match_delegate (action a, target& t, const rule& dr, bool try_match)
  {
    assert (phase == run_phase::match);

    // Note: we don't touch any of the t[a] state since that was/will be set
    // for the delegating rule.
    //
    const rule_match* r (match_impl (a, t, &dr, try_match));
    return r != nullptr ? apply_impl (a, t, *r) : empty_recipe;
  }

  inline target_state
  match_inner (action a, const target& t)
  {
    // In a sense this is like any other dependency.
    //
    assert (a.outer ());
    return match (a.inner_action (), t);
  }

  inline bool
  match_inner (action a, const target& t, unmatch um)
  {
    assert (a.outer ());
    return match (a.inner_action (), t, um);
  }

  group_view
  resolve_members_impl (action, const target&, target_lock);

  inline group_view
  resolve_members (action a, const target& g)
  {
    group_view r;

    if (a.outer ())
      a = a.inner_action ();

    // We can be called during execute though everything should have been
    // already resolved.
    //
    switch (phase)
    {
    case run_phase::match:
      {
        // Grab a target lock to make sure the group state is synchronized.
        //
        target_lock l (lock_impl (a, g, scheduler::work_none));
        r = g.group_members (a);

        // If the group members are alrealy known or there is nothing else
        // we can do, then unlock and return.
        //
        if (r.members == nullptr && l.offset != target::offset_executed)
          r = resolve_members_impl (a, g, move (l));

        break;
      }
    case run_phase::execute: r = g.group_members (a); break;
    case run_phase::load:    assert (false);
    }

    return r;
  }

  void
  resolve_group_impl (action, const target&, target_lock);

  inline const target*
  resolve_group (action a, const target& t)
  {
    if (a.outer ())
      a = a.inner_action ();

    switch (phase)
    {
    case run_phase::match:
      {
        // Grab a target lock to make sure the group state is synchronized.
        //
        target_lock l (lock_impl (a, t, scheduler::work_none));

        // If the group is alrealy known or there is nothing else we can do,
        // then unlock and return.
        //
        if (t.group == nullptr && l.offset < target::offset_tried)
          resolve_group_impl (a, t, move (l));

        break;
      }
    case run_phase::execute: break;
    case run_phase::load:    assert (false);
    }

    return t.group;
  }

  void
  match_prerequisites (action, target&, const match_search&, const scope*);

  void
  match_prerequisite_members (action, target&,
                              const match_search_member&,
                              const scope*);

  inline void
  match_prerequisites (action a, target& t, const match_search& ms)
  {
    match_prerequisites (
      a,
      t,
      ms,
      (a.operation () != clean_id ? nullptr : &t.root_scope ()));
  }

  inline void
  match_prerequisite_members (action a, target& t,
                              const match_search_member& msm)
  {
    if (a.operation () != clean_id)
      match_prerequisite_members (a, t, msm, nullptr);
    else
    {
      // Note that here we don't iterate over members even for see-through
      // groups since the group target should clean eveything up. A bit of an
      // optimization.
      //
      match_search ms (
        msm
        ? [&msm] (action a,
                  const target& t,
                  const prerequisite& p,
                  include_type i)
        {
          return msm (a, t, prerequisite_member {p, nullptr}, i);
        }
        : match_search ());

      match_prerequisites (a, t, ms, &t.root_scope ());
    }
  }

  inline void
  match_prerequisites (action a, target& t, const scope& s)
  {
    match_prerequisites (a, t, nullptr, &s);
  }

  inline void
  match_prerequisite_members (action a, target& t, const scope& s)
  {
    match_prerequisite_members (a, t, nullptr, &s);
  }

  target_state
  execute (action, const target&, size_t, atomic_count*);

  inline target_state
  execute (action a, const target& t)
  {
    return execute (a, t, 0, nullptr);
  }

  inline target_state
  execute_wait (action a, const target& t)
  {
    if (execute (a, t) == target_state::busy)
      sched.wait (target::count_executed (),
                  t[a].task_count,
                  scheduler::work_none);

    return t.executed_state (a);
  }

  inline target_state
  execute_async (action a, const target& t,
                 size_t sc, atomic_count& tc,
                 bool fail)
  {
    target_state r (execute (a, t, sc, &tc));

    if (fail && !keep_going && r == target_state::failed)
      throw failed ();

    return r;
  }

  inline target_state
  execute_delegate (const recipe& r, action a, const target& t)
  {
    return r (a, t);
  }

  inline target_state
  execute_inner (action a, const target& t)
  {
    assert (a.outer ());
    return execute_wait (a.inner_action (), t);
  }

  inline target_state
  straight_execute_prerequisites (action a, const target& t,
                                  size_t c, size_t s)
  {
    auto& p (t.prerequisite_targets[a]);
    return straight_execute_members (a, t,
                                     p.data (),
                                     c == 0 ? p.size () - s: c,
                                     s);
  }

  inline target_state
  reverse_execute_prerequisites (action a, const target& t, size_t c)
  {
    auto& p (t.prerequisite_targets[a]);
    return reverse_execute_members (a, t,
                                    p.data (),
                                    c == 0 ? p.size () : c,
                                    p.size ());
  }

  inline target_state
  execute_prerequisites (action a, const target& t, size_t c)
  {
    return current_mode == execution_mode::first
      ? straight_execute_prerequisites (a, t, c)
      : reverse_execute_prerequisites (a, t, c);
  }

  inline target_state
  straight_execute_prerequisites_inner (action a, const target& t,
                                        size_t c, size_t s)
  {
    assert (a.outer ());
    auto& p (t.prerequisite_targets[a]);
    return straight_execute_members (a.inner_action (),
                                     t[a].task_count,
                                     p.data (),
                                     c == 0 ? p.size () - s : c,
                                     s);
  }

  inline target_state
  reverse_execute_prerequisites_inner (action a, const target& t, size_t c)
  {
    assert (a.outer ());
    auto& p (t.prerequisite_targets[a]);
    return reverse_execute_members (a.inner_action (),
                                    t[a].task_count,
                                    p.data (),
                                    c == 0 ? p.size () : c,
                                    p.size ());
  }

  inline target_state
  execute_prerequisites_inner (action a, const target& t, size_t c)
  {
    return current_mode == execution_mode::first
      ? straight_execute_prerequisites_inner (a, t, c)
      : reverse_execute_prerequisites_inner (a, t, c);
  }

  // If the first argument is NULL, then the result is treated as a boolean
  // value.
  //
  pair<optional<target_state>, const target*>
  execute_prerequisites (const target_type*,
                         action, const target&,
                         const timestamp&, const execute_filter&,
                         size_t);

  inline optional<target_state>
  execute_prerequisites (action a, const target& t,
                         const timestamp& mt, const execute_filter& ef,
                         size_t n)
  {
    return execute_prerequisites (nullptr, a, t, mt, ef, n).first;
  }

  template <typename T>
  inline pair<optional<target_state>, const T&>
  execute_prerequisites (action a, const target& t,
                         const timestamp& mt, const execute_filter& ef,
                         size_t n)
  {
    auto p (execute_prerequisites (T::static_type, a, t, mt, ef, n));
    return pair<optional<target_state>, const T&> (
      p.first, static_cast<const T&> (p.second));
  }

  inline pair<optional<target_state>, const target&>
  execute_prerequisites (const target_type& tt,
                         action a, const target& t,
                         const timestamp& mt, const execute_filter& ef,
                         size_t n)
  {
    auto p (execute_prerequisites (&tt, a, t, mt, ef, n));
    return pair<optional<target_state>, const target&> (p.first, *p.second);
  }

  template <typename T>
  inline pair<optional<target_state>, const T&>
  execute_prerequisites (const target_type& tt,
                         action a, const target& t,
                         const timestamp& mt, const execute_filter& ef,
                         size_t n)
  {
    auto p (execute_prerequisites (tt, a, t, mt, ef, n));
    return pair<optional<target_state>, const T&> (
      p.first, static_cast<const T&> (p.second));
  }

  inline target_state
  execute_members (action a, const target& t, const target* ts[], size_t n)
  {
    return current_mode == execution_mode::first
      ? straight_execute_members (a, t, ts, n, 0)
      : reverse_execute_members (a, t, ts, n, n);
  }
}
