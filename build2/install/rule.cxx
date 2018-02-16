// file      : build2/install/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/install/rule.hxx>

#include <libbutl/filesystem.mxx> // dir_exists(), file_exists()

#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/algorithm.hxx>
#include <build2/filesystem.hxx>
#include <build2/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace install
  {
    // Lookup the install or install.* variable. Return NULL if not found or
    // if the value is the special 'false' name (which means do not install).
    // T is either scope or target.
    //
    template <typename P, typename T>
    static const P*
    lookup_install (T& t, const string& var)
    {
      auto l (t[var]);

      if (!l)
        return nullptr;

      const P& r (cast<P> (l));
      return r.simple () && r.string () == "false" ? nullptr : &r;
    }

    // alias_rule
    //
    const alias_rule alias_rule::instance;

    bool alias_rule::
    match (action, target&, const string&) const
    {
      // We always match.
      //
      // Note that we are called both as the outer part during the update-for-
      // un/install pre-operation and as the inner part during the un/install
      // operation itself.
      //
      return true;
    }

    const target* alias_rule::
    filter (action a, const target& t, prerequisite_iterator& i) const
    {
      assert (i->member == nullptr);
      return filter (a, t, i->prerequisite);
    }

    const target* alias_rule::
    filter (action, const target& t, const prerequisite& p) const
    {
      return &search (t, p);
    }

    recipe alias_rule::
    apply (action a, target& t) const
    {
      tracer trace ("install::alias_rule::apply");

      // Pass-through to our installable prerequisites.
      //
      // @@ Shouldn't we do match in parallel (here and below)?
      //
      auto& pts (t.prerequisite_targets[a]);

      auto pms (group_prerequisite_members (a, t, members_mode::never));
      for (auto i (pms.begin ()), e (pms.end ()); i != e; ++i)
      {
        const prerequisite& p (i->prerequisite);

        // Ignore unresolved targets that are imported from other projects.
        // We are definitely not installing those.
        //
        if (p.proj)
          continue;

        // Let a customized rule have its say.
        //
        // Note: we assume that if the filter enters the group, then it
        // iterates over all its members.
        //
        const target* pt (filter (a, t, i));
        if (pt == nullptr)
        {
          l5 ([&]{trace << "ignoring " << p << " (filtered out)";});
          continue;
        }

        // Check if this prerequisite is explicitly "not installable", that
        // is, there is the 'install' variable and its value is false.
        //
        // At first, this might seem redundand since we could have let the
        // file_rule below take care of it. The nuance is this: this
        // prerequsite can be in a different subproject that hasn't loaded the
        // install module (and therefore has no file_rule registered). The
        // typical example would be the 'tests' subproject.
        //
        // Note: not the same as lookup() above.
        //
        auto l ((*pt)["install"]);
        if (l && cast<path> (l).string () == "false")
        {
          l5 ([&]{trace << "ignoring " << *pt << " (not installable)";});
          continue;
        }

        build2::match (a, *pt);
        pts.push_back (pt);
      }

      return default_recipe;
    }

    // group_rule
    //
    const group_rule group_rule::instance (false /* see_through_only */);

    bool group_rule::
    match (action a, target& t, const string& h) const
    {
      return (!see_through || t.type ().see_through) &&
        alias_rule::match (a, t, h);
    }

    const target* group_rule::
    filter (action, const target&, const target& m) const
    {
      return &m;
    }

    recipe group_rule::
    apply (action a, target& t) const
    {
      tracer trace ("install::group_rule::apply");

      // Resolve group members.
      //
      // Remember that we are called twice: first during update for install
      // (pre-operation) and then during install. During the former, we rely
      // on the normall update rule to resolve the group members. During the
      // latter, there will be no rule to do this but the group will already
      // have been resolved by the pre-operation.
      //
      // If the rule could not resolve the group, then we ignore it.
      //
      group_view gv (a.outer ()
                     ? resolve_members (a, t)
                     : t.group_members (a));

      if (gv.members != nullptr)
      {
        auto& pts (t.prerequisite_targets[a]);

        for (size_t i (0); i != gv.count; ++i)
        {
          const target* m (gv.members[i]);

          if (m == nullptr)
            continue;

          // Let a customized rule have its say.
          //
          const target* mt (filter (a, t, *m));
          if (mt == nullptr)
          {
            l5 ([&]{trace << "ignoring " << *m << " (filtered out)";});
            continue;
          }

          // See if we were explicitly instructed not to touch this target.
          //
          // Note: not the same as lookup() above.
          //
          auto l ((*mt)["install"]);
          if (l && cast<path> (l).string () == "false")
          {
            l5 ([&]{trace << "ignoring " << *mt << " (not installable)";});
            continue;
          }

          build2::match (a, *mt);
          pts.push_back (mt);
        }
      }

      // Delegate to the base rule.
      //
      return alias_rule::apply (a, t);
    }


    // file_rule
    //
    const file_rule file_rule::instance;

    bool file_rule::
    match (action, target&, const string&) const
    {
      // We always match, even if this target is not installable (so that we
      // can ignore it; see apply()).
      //
      return true;
    }

    const target* file_rule::
    filter (action a, const target& t, prerequisite_iterator& i) const
    {
      assert (i->member == nullptr);
      return filter (a, t, i->prerequisite);
    }

    const target* file_rule::
    filter (action, const target& t, const prerequisite& p) const
    {
      const target& pt (search (t, p));
      return pt.in (t.root_scope ()) ? &pt : nullptr;
    }

    recipe file_rule::
    apply (action a, target& t) const
    {
      tracer trace ("install::file_rule::apply");

      // Note that we are called both as the outer part during the update-for-
      // un/install pre-operation and as the inner part during the un/install
      // operation itself.
      //
      // In both cases we first determine if the target is installable and
      // return noop if it's not. Otherwise, in the first case (update-for-
      // un/install) we delegate to the normal update and in the second
      // (un/install) -- perform the test.
      //
      if (lookup_install<path> (t, "install") == nullptr)
        return noop_recipe;

      // In both cases, the next step is to search, match, and collect all the
      // installable prerequisites.
      //
      auto& pts (t.prerequisite_targets[a]);

      auto pms (group_prerequisite_members (a, t, members_mode::never));
      for (auto i (pms.begin ()), e (pms.end ()); i != e; ++i)
      {
        const prerequisite& p (i->prerequisite);

        // Ignore unresolved targets that are imported from other projects.
        // We are definitely not installing those.
        //
        if (p.proj)
          continue;

        // Let a customized rule have its say.
        //
        // Note: we assume that if the filter enters the group, then it
        // iterates over all its members.
        //
        const target* pt (filter (a, t, i));
        if (pt == nullptr)
        {
          l5 ([&]{trace << "ignoring " << p << " (filtered out)";});
          continue;
        }

        // See if we were explicitly instructed not to touch this target.
        //
        // Note: not the same as lookup() above.
        //
        auto l ((*pt)["install"]);
        if (l && cast<path> (l).string () == "false")
        {
          l5 ([&]{trace << "ignoring " << *pt << " (not installable)";});
          continue;
        }

        // If the matched rule returned noop_recipe, then the target state is
        // set to unchanged as an optimization. Use this knowledge to optimize
        // things on our side as well since this will help a lot when updating
        // static installable content (headers, documentation, etc).
        //
        if (!build2::match (a, *pt, unmatch::unchanged))
          pts.push_back (pt);
      }

      if (a.operation () == update_id)
      {
        // For the update pre-operation match the inner rule (actual update).
        //
        if (match_inner (a, t, unmatch::unchanged))
        {
          return pts.empty () ? noop_recipe : default_recipe;
        }

        return &perform_update;
      }
      else
      {
        return [this] (action a, const target& t)
        {
          return a.operation () == install_id
            ? perform_install   (a, t)
            : perform_uninstall (a, t);
        };
      }
    }

    target_state file_rule::
    perform_update (action a, const target& t)
    {
      // First execute the inner recipe then prerequisites.
      //
      target_state ts (execute_inner (a, t));

      if (t.prerequisite_targets[a].size () != 0)
        ts |= straight_execute_prerequisites (a, t);

      return ts;
    }

    bool file_rule::
    install_extra (const file&, const install_dir&) const
    {
      return false;
    }

    bool file_rule::
    uninstall_extra (const file&, const install_dir&) const
    {
      return false;
    }

    struct install_dir
    {
      dir_path dir;

      // If not NULL, then point to the corresponding install.* value.
      //
      const string*  sudo     = nullptr;
      const path*    cmd      = nullptr;
      const strings* options  = nullptr;
      const string*  mode     = nullptr;
      const string*  dir_mode = nullptr;

      explicit
      install_dir (dir_path d = dir_path ()): dir (move (d)) {}

      install_dir (dir_path d, const install_dir& b)
          : dir (move (d)),
            sudo (b.sudo),
            cmd (b.cmd),
            options (b.options),
            mode (b.mode),
            dir_mode (b.dir_mode) {}
    };

    using install_dirs = vector<install_dir>;

    // Calculate a subdirectory based on l's location (*.subdirs) and if not
    // empty add it to install_dirs. Return the new last element.
    //
    static install_dir&
    resolve_subdir (install_dirs& rs,
                    const target& t,
                    const scope& s,
                    const lookup& l)
    {
      // Find the scope from which this value came and use as a base
      // to calculate the subdirectory.
      //
      for (const scope* p (&s); p != nullptr; p = p->parent_scope ())
      {
        if (l.belongs (*p, true)) // Include target type/pattern-specific.
        {
          // The target can be in out or src.
          //
          const dir_path& d (t.out_dir ().leaf (p->out_path ()));

          // Add it as another leading directory rather than modifying
          // the last one directly; somehow, it feels right.
          //
          if (!d.empty ())
            rs.emplace_back (rs.back ().dir / d, rs.back ());
          break;
        }
      }

      return rs.back ();
    }

    // Resolve installation directory name to absolute directory path. Return
    // all the super-directories leading up to the destination (last).
    //
    // If target is not NULL, then also handle the subdirs logic.
    //
    static install_dirs
    resolve (const scope& s,
             const target* t,
             dir_path d,
             const string* var = nullptr)
    {
      install_dirs rs;

      if (d.absolute ())
        rs.emplace_back (move (d.normalize ()));
      else
      {
        // If it is relative, then the first component is treated as the
        // installation directory name, e.g., bin, sbin, lib, etc. Look it
        // up and recurse.
        //
        if (d.empty ())
          fail << "empty installation directory name";

        const string& sn (*d.begin ());
        const string var ("install." + sn);
        if (const dir_path* dn = lookup_install<dir_path> (s, var))
        {
          if (dn->empty ())
            fail << "empty installation directory for name " << sn <<
              info << "did you specified empty config." << var << "?";

          rs = resolve (s, t, *dn, &var);
          d = rs.back ().dir / dir_path (++d.begin (), d.end ());
          rs.emplace_back (move (d.normalize ()), rs.back ());
        }
        else
          fail << "unknown installation directory name '" << sn << "'" <<
            info << "did you forget to specify config." << var << "?";
      }

      install_dir* r (&rs.back ());

      // Override components in install_dir if we have our own.
      //
      if (var != nullptr)
      {
        if (auto l = s[*var + ".sudo"])     r->sudo     = &cast<string> (l);
        if (auto l = s[*var + ".cmd"])      r->cmd      = &cast<path> (l);
        if (auto l = s[*var + ".mode"])     r->mode     = &cast<string> (l);
        if (auto l = s[*var + ".dir_mode"]) r->dir_mode = &cast<string> (l);
        if (auto l = s[*var + ".options"])  r->options  = &cast<strings> (l);

        if (t != nullptr)
        {
          if (auto l = s[*var + ".subdirs"])
          {
            if (cast<bool> (l))
              r = &resolve_subdir (rs, *t, s, l);
          }
        }
      }

      // Set globals for unspecified components.
      //
      if (r->sudo == nullptr)
        r->sudo = cast_null<string> (s["config.install.sudo"]);

      if (r->cmd == nullptr)
        r->cmd = &cast<path> (s["config.install.cmd"]);

      if (r->options == nullptr)
        r->options = cast_null<strings> (s["config.install.options"]);

      if (r->mode == nullptr)
        r->mode = &cast<string> (s["config.install.mode"]);

      if (r->dir_mode == nullptr)
        r->dir_mode = &cast<string> (s["config.install.dir_mode"]);

      return rs;
    }

    static inline install_dirs
    resolve (const target& t, dir_path d, const string* var = nullptr)
    {
      return resolve (t.base_scope (), &t, d, var);
    }

    dir_path
    resolve_dir (const target& t, dir_path d)
    {
      return move (resolve (t, move (d)).back ().dir);
    }

    dir_path
    resolve_dir (const scope& s, dir_path d)
    {
      return move (resolve (s, nullptr, move (d)).back ().dir);
    }

    path
    resolve_file (const file& f)
    {
      // Note: similar logic to perform_install().
      //
      const path* p (lookup_install<path> (f, "install"));

      if (p == nullptr) // Not installable.
        return path ();

      bool n (!p->to_directory ());
      dir_path d (n ? p->directory () : path_cast<dir_path> (*p));

      install_dirs ids (resolve (f, d));
      if (auto l = f["install.subdirs"])
      {
        if (cast<bool> (l))
          resolve_subdir (ids, f, f.base_scope (), l);
      }

      return ids.back ().dir / (n ? p->leaf () : f.path ().leaf ());
    }

    // On Windows we use MSYS2 install.exe and MSYS2 by default ignores
    // filesystem permissions (noacl mount option). And this means, for
    // example, that .exe that we install won't be runnable by Windows (MSYS2
    // itself will still run them since it recognizes the file extension).
    //
    // The way we work around this (at least in our distribution of the MSYS2
    // tools) is by changing the mount option for cygdrives (/c, /d, etc) to
    // acl. But that's not all: we also have to install via a path that "hits"
    // one of those mount points, c:\foo won't work, we have to use /c/foo.
    // So this function translates an absolute Windows path to its MSYS
    // representation.
    //
    // Note that we return the result as a string, not dir_path since path
    // starting with / are illegal on Windows. Also note that the result
    // doesn't have the trailing slash.
    //
    static string
    msys_path (const dir_path& d)
    {
      assert (d.absolute ());
      string s (d.representation ());

      // First replace ':' with the drive letter (so the path is no longer
      // absolute) but postpone setting the first character to / until we are
      // a string.
      //
      s[1] = lcase (s[0]);
      s = dir_path (move (s)).posix_string ();
      s[0] = '/';

      return s;
    }

    // Given an abolute path return its chroot'ed version, if any, accoring to
    // install.chroot.
    //
    template <typename P>
    static inline P
    chroot_path (const scope& rs, const P& p)
    {
      if (const dir_path* d = cast_null<dir_path> (rs["install.chroot"]))
      {
        dir_path r (p.root_directory ());
        assert (!r.empty ()); // Must be absolute.

        return *d / p.leaf (r);
      }

      return p;
    }

    // install -d <dir>
    //
    // If verbose is false, then only print the command at verbosity level 2
    // or higher.
    //
    static void
    install_d (const scope& rs,
               const install_dir& base,
               const dir_path& d,
               bool verbose = true)
    {
      dir_path chd (chroot_path (rs, d));

      try
      {
        if (dir_exists (chd)) // May throw (e.g., EACCES).
          return;
      }
      catch (const system_error& e)
      {
        fail << "invalid installation directory " << chd << ": " << e;
      }

      // While install -d will create all the intermediate components between
      // base and dir, we do it explicitly, one at a time. This way the output
      // is symmetrical to uninstall() below.
      //
      // Note that if the chroot directory does not exist, then install -d
      // will create it and we don't bother removing it.
      //
      if (d != base.dir)
      {
        dir_path pd (d.directory ());

        if (pd != base.dir)
          install_d (rs, base, pd, verbose);
      }

      cstrings args;

      string reld (
        cast<string> ((*global_scope)["build.host.class"]) == "windows"
        ? msys_path (chd)
        : relative (chd).string ());

      if (base.sudo != nullptr)
        args.push_back (base.sudo->c_str ());

      args.push_back (base.cmd->string ().c_str ());
      args.push_back ("-d");

      if (base.options != nullptr)
        append_options (args, *base.options);

      args.push_back ("-m");
      args.push_back (base.dir_mode->c_str ());
      args.push_back (reld.c_str ());
      args.push_back (nullptr);

      process_path pp (run_search (args[0]));

      if (verb >= 2)
        print_process (args);
      else if (verb && verbose)
        text << "install " << chd;

      run (pp, args);
    }

    // install <file> <dir>/
    // install <file> <file>
    //
    // If verbose is false, then only print the command at verbosity level 2
    // or higher.
    //
    static void
    install_f (const scope& rs,
               const install_dir& base,
               const path& name,
               const file& t,
               bool verbose)
    {
      path relf (relative (t.path ()));

      dir_path chd (chroot_path (rs, base.dir));

      string reld (
        cast<string> ((*global_scope)["build.host.class"]) == "windows"
        ? msys_path (chd)
        : relative (chd).string ());

      if (!name.empty ())
      {
        reld += path::traits::directory_separator;
        reld += name.string ();
      }

      cstrings args;

      if (base.sudo != nullptr)
        args.push_back (base.sudo->c_str ());

      args.push_back (base.cmd->string ().c_str ());

      if (base.options != nullptr)
        append_options (args, *base.options);

      args.push_back ("-m");
      args.push_back (base.mode->c_str ());
      args.push_back (relf.string ().c_str ());
      args.push_back (reld.c_str ());
      args.push_back (nullptr);

      process_path pp (run_search (args[0]));

      if (verb >= 2)
        print_process (args);
      else if (verb && verbose)
        text << "install " << t;

      run (pp, args);
    }

    void file_rule::
    install_l (const scope& rs,
               const install_dir& base,
               const path& target,
               const path& link,
               bool verbose)
    {
      path rell (relative (chroot_path (rs, base.dir)));
      rell /= link;

      // We can create a symlink directly without calling ln. This, however,
      // won't work if we have sudo. Also, we would have to deal with existing
      // destinations (ln's -f takes care of that). So we are just going to
      // always use ln.
      //
      const char* args_a[] = {
        base.sudo != nullptr ? base.sudo->c_str () : nullptr,
        "ln",
        "-sf",
        target.string ().c_str (),
        rell.string ().c_str (),
        nullptr};

      const char** args (&args_a[base.sudo == nullptr ? 1 : 0]);

      process_path pp (run_search (args[0]));

      if (verb >= 2)
        print_process (args);
      else if (verb && verbose)
        text << "install " << rell << " -> " << target;

      run (pp, args);
    }

    target_state file_rule::
    perform_install (action a, const target& xt) const
    {
      const file& t (xt.as<file> ());
      assert (!t.path ().empty ()); // Should have been assigned by update.

      const scope& rs (t.root_scope ());

      auto install_target = [&rs, this] (const file& t,
                                         const path& p,
                                         bool verbose)
      {
        // Note: similar logic to resolve_file().
        //
        bool n (!p.to_directory ());
        dir_path d (n ? p.directory () : path_cast<dir_path> (p));

        // Resolve target directory.
        //
        install_dirs ids (resolve (t, d));

        // Handle install.subdirs if one was specified.
        //
        if (auto l = t["install.subdirs"])
        {
          if (cast<bool> (l))
            resolve_subdir (ids, t, t.base_scope (), l);
        }

        // Create leading directories. Note that we are using the leading
        // directory (if there is one) for the creation information (mode,
        // sudo, etc).
        //
        for (auto i (ids.begin ()), j (i); i != ids.end (); j = i++)
          install_d (rs, *j, i->dir, verbose); // install -d

        install_dir& id (ids.back ());

        // Override mode if one was specified.
        //
        if (auto l = t["install.mode"])
          id.mode = &cast<string> (l);

        // Install the target and extras.
        //
        install_f (rs, id, n ? p.leaf () : path (), t, verbose);
        install_extra (t, id);
      };

      // First handle installable prerequisites.
      //
      target_state r (straight_execute_prerequisites (a, t));

      // Then installable ad hoc group members, if any.
      //
      for (const target* m (t.member); m != nullptr; m = m->member)
      {
        if (const path* p = lookup_install<path> (*m, "install"))
          install_target (m->as<file> (), *p, false);
      }

      // Finally install the target itself (since we got here we know the
      // install variable is there).
      //
      install_target (t, cast<path> (t["install"]), true);

      return (r |= target_state::changed);
    }

    // uninstall -d <dir>
    //
    // We try to remove all the directories between base and dir but not base
    // itself unless base == dir. Return false if nothing has been removed
    // (i.e., the directories do not exist or are not empty).
    //
    // If verbose is false, then only print the command at verbosity level 2
    // or higher.
    //
    static bool
    uninstall_d (const scope& rs,
                 const install_dir& base,
                 const dir_path& d,
                 bool verbose)
    {
      dir_path chd (chroot_path (rs, d));

      // Figure out if we should try to remove this directory. Note that if
      // it doesn't exist, then we may still need to remove outer ones.
      //
      bool r (false);
      try
      {
        if ((r = dir_exists (chd))) // May throw (e.g., EACCES).
        {
          if (!dir_empty (chd)) // May also throw.
            return false; // Won't be able to remove any outer directories.
        }
      }
      catch (const system_error& e)
      {
        fail << "invalid installation directory " << chd << ": " << e;
      }

      if (r)
      {
        dir_path reld (relative (chd));

        // Normally when we need to remove a file or directory we do it
        // directly without calling rm/rmdir. This however, won't work if we
        // have sudo. So we are going to do it both ways.
        //
        // While there is no sudo on Windows, deleting things that are being
        // used can get complicated. So we will always use rm/rmdir there.
        //
#ifndef _WIN32
        if (base.sudo == nullptr)
        {
          if (verb >= 2)
            text << "rmdir " << reld;
          else if (verb && verbose)
            text << "uninstall " << reld;

          try
          {
            try_rmdir (chd);
          }
          catch (const system_error& e)
          {
            fail << "unable to remove directory " << chd << ": " << e;
          }
        }
        else
#endif
        {
          const char* args_a[] = {
            base.sudo != nullptr ? base.sudo->c_str () : nullptr,
            "rmdir",
            reld.string ().c_str (),
            nullptr};

          const char** args (&args_a[base.sudo == nullptr ? 1 : 0]);

          process_path pp (run_search (args[0]));

          if (verb >= 2)
            print_process (args);
          else if (verb && verbose)
            text << "uninstall " << reld;

          run (pp, args);
        }
      }

      // If we have more empty directories between base and dir, then try
      // to clean them up as well.
      //
      if (d != base.dir)
      {
        dir_path pd (d.directory ());

        if (pd != base.dir)
          r = uninstall_d (rs, base, pd, verbose) || r;
      }

      return r;
    }

    bool file_rule::
    uninstall_f (const scope& rs,
                 const install_dir& base,
                 const file* t,
                 const path& name,
                 bool verbose)
    {
      assert (t != nullptr || !name.empty ());
      path f (chroot_path (rs, base.dir) /
              (name.empty () ? t->path ().leaf () : name));

      try
      {
        // Note: don't follow symlinks so if the target is a dangling symlinks
        // we will proceed to removing it.
        //
        if (!file_exists (f, false)) // May throw (e.g., EACCES).
          return false;
      }
      catch (const system_error& e)
      {
        fail << "invalid installation path " << f << ": " << e;
      }

      path relf (relative (f));

      if (verb == 1 && verbose)
      {
        if (t != nullptr)
          text << "uninstall " << *t;
        else
          text << "uninstall " << relf;
      }

      // The same story as with uninstall -d.
      //
#ifndef _WIN32
      if (base.sudo == nullptr)
      {
        if (verb >= 2)
          text << "rm " << relf;

        try
        {
          try_rmfile (f);
        }
        catch (const system_error& e)
        {
          fail << "unable to remove file " << f << ": " << e;
        }
      }
      else
#endif
      {
        const char* args_a[] = {
          base.sudo != nullptr ? base.sudo->c_str () : nullptr,
          "rm",
          "-f",
          relf.string ().c_str (),
          nullptr};

        const char** args (&args_a[base.sudo == nullptr ? 1 : 0]);

        process_path pp (run_search (args[0]));

        if (verb >= 2)
          print_process (args);

        run (pp, args);
      }

      return true;
    }

    target_state file_rule::
    perform_uninstall (action a, const target& xt) const
    {
      const file& t (xt.as<file> ());
      assert (!t.path ().empty ()); // Should have been assigned by update.

      const scope& rs (t.root_scope ());

      auto uninstall_target = [&rs, this] (const file& t,
                                           const path& p,
                                           bool verbose) -> target_state
      {
        bool n (!p.to_directory ());
        dir_path d (n ? p.directory () : path_cast<dir_path> (p));

        // Resolve target directory.
        //
        install_dirs ids (resolve (t, d));

        // Handle install.subdirs if one was specified.
        //
        if (auto l = t["install.subdirs"])
        {
          if (cast<bool> (l))
            resolve_subdir (ids, t, t.base_scope (), l);
        }

        // Remove extras and the target itself.
        //
        const install_dir& id (ids.back ());

        target_state r (uninstall_extra (t, id)
                        ? target_state::changed
                        : target_state::unchanged);

        if (uninstall_f (rs, id, &t, n ? p.leaf () : path (), verbose))
          r |= target_state::changed;

        // Clean up empty leading directories (in reverse).
        //
        // Note that we are using the leading directory (if there is one)
        // for the clean up information (sudo, etc).
        //
        for (auto i (ids.rbegin ()), j (i), e (ids.rend ()); i != e; j = ++i)
        {
          if (install::uninstall_d (rs, ++j != e ? *j : *i, i->dir, verbose))
            r |= target_state::changed;
        }

        return r;
      };

      // Reverse order of installation: first the target itself (since we got
      // here we know the install variable is there).
      //
      target_state r (uninstall_target (t, cast<path> (t["install"]), true));

      // Then installable ad hoc group members, if any. To be anally precise
      // we would have to do it in reverse, but that's not easy (it's a
      // single-linked list).
      //
      for (const target* m (t.member); m != nullptr; m = m->member)
      {
        if (const path* p = lookup_install<path> (*m, "install"))
          r |= uninstall_target (
            m->as<file> (), *p, r != target_state::changed);
      }

      // Finally handle installable prerequisites.
      //
      r |= reverse_execute_prerequisites (a, t);

      return r;
    }
  }
}
