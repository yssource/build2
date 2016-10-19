// file      : build2/parser.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/parser>

#include <iostream> // cout

#include <build2/version>

#include <build2/scope>
#include <build2/target>
#include <build2/prerequisite>
#include <build2/variable>
#include <build2/module>
#include <build2/file>
#include <build2/diagnostics>
#include <build2/context>

using namespace std;

namespace build2
{
  using type = token_type;

  static const dir_path root_dir ("/");

  class parser::enter_scope
  {
  public:
    enter_scope (): p_ (nullptr), r_ (nullptr), s_ (nullptr) {}

    enter_scope (parser& p, dir_path&& d): p_ (&p), r_ (p.root_), s_ (p.scope_)
    {
      // Check for the global scope as a special case. While on POSIX the
      // check is redundant, on Windows the path completion/normalization
      // would otherwise transform it to the out path of the current scope
      // since "/" is a relative path on Windows (and we use "/" even on
      // Windows for that gloabl scope).
      //
      if (d != root_dir)
      {
        // Try hard not to call normalize(). Most of the time we will go just
        // one level deeper.
        //
        bool n (true);

        if (d.relative ())
        {
          // Relative scopes are opened relative to out, not src.
          //
          if (d.simple () && d.string () != "." && d.string () != "..")
          {
            d = dir_path (p.scope_->out_path ()) /= d.string ();
            n = false;
          }
          else
            d = p.scope_->out_path () / d;
        }

        if (n)
          d.normalize ();
      }

      p.switch_scope (d);
    }

    ~enter_scope ()
    {
      if (p_ != nullptr)
      {
        p_->scope_ = s_;
        p_->root_ = r_;
      }
    }

    // Note: move-assignable to empty only.
    //
    enter_scope (enter_scope&& x) {*this = move (x);}
    enter_scope& operator= (enter_scope&& x) {
      p_ = x.p_; r_ = x.r_; s_ = x.s_; x.p_ = nullptr; return *this;}

    enter_scope (const enter_scope&) = delete;
    enter_scope& operator= (const enter_scope&) = delete;

  private:
    parser* p_;
    scope* r_;
    scope* s_;
  };

  class parser::enter_target
  {
  public:
    enter_target (): p_ (nullptr), t_ (nullptr) {}

    enter_target (parser& p,
                  name&& n,  // If n.pair, then o is out dir.
                  name&& o,
                  const location& loc,
                  tracer& tr)
        : p_ (&p), t_ (p.target_)
    {
      const string* e;
      const target_type* ti (p.scope_->find_target_type (n, e));

      if (ti == nullptr)
        p.fail (loc) << "unknown target type " << n.type;

      bool src (n.pair); // If out-qualified, then it is from src.
      if (src)
      {
        assert (n.pair == '@');

        if (!o.directory ())
          p.fail (loc) << "directory expected after @";
      }

      dir_path& d (n.dir);

      const dir_path& sd (p.scope_->src_path ());
      const dir_path& od (p.scope_->out_path ());

      if (d.empty ())
        d = src ? sd : od; // Already dormalized.
      else
      {
        if (d.relative ())
          d = (src ? sd : od) / d;

        d.normalize ();
      }

      dir_path out;
      if (src && sd != od) // If in-source build, then out must be empty.
      {
        out = o.dir.relative () ? od / o.dir : move (o.dir);
        out.normalize ();
      }

      // Find or insert.
      //
      p.target_ = &targets.insert (
        *ti, move (d), move (out), move (n.value), e, tr).first;
    }

    ~enter_target ()
    {
      if (p_ != nullptr)
        p_->target_ = t_;
    }

    // Note: move-assignable to empty only.
    //
    enter_target (enter_target&& x) {*this = move (x);}
    enter_target& operator= (enter_target&& x) {
      p_ = x.p_; t_ = x.t_; x.p_ = nullptr; return *this;}

    enter_target (const enter_target&) = delete;
    enter_target& operator= (const enter_target&) = delete;

  private:
    parser* p_;
    target* t_;
  };

  void parser::
  parse_buildfile (istream& is, const path& p, scope& root, scope& base)
  {
    path_ = &p;

    lexer l (is, *path_);
    lexer_ = &l;
    target_ = nullptr;
    scope_ = &base;
    root_ = &root;
    default_target_ = nullptr;

    enter_buildfile (p); // Needs scope_.

    token t;
    type tt;
    next (t, tt);

    parse_clause (t, tt);

    if (tt != type::eos)
      fail (t) << "unexpected " << t;

    process_default_target (t);
  }

  token parser::
  parse_variable (lexer& l, scope& s, const variable& var, type kind)
  {
    path_ = &l.name ();
    lexer_ = &l;
    target_ = nullptr;
    scope_ = &s;

    token t;
    type tt;
    parse_variable (t, tt, var, kind);
    return t;
  }

  pair<value, token> parser::
  parse_variable_value (lexer& l, scope& s, const variable& var)
  {
    path_ = &l.name ();
    lexer_ = &l;
    target_ = nullptr;
    scope_ = &s;

    token t;
    type tt;
    value rhs (parse_variable_value (t, tt));

    value lhs;
    apply_value_attributes (&var, lhs, move (rhs), type::assign);

    return make_pair (move (lhs), move (t));
  }

  bool parser::
  parse_clause (token& t, type& tt, bool one)
  {
    tracer trace ("parser::clause", &path_);

    // clause() should always stop at a token that is at the beginning of
    // the line (except for eof). That is, if something is called to parse
    // a line, it should parse it until newline (or fail). This is important
    // for if-else blocks, directory scopes, etc., that assume the } token
    // they see is on the new line.
    //
    bool parsed (false);

    while (tt != type::eos && !(one && parsed))
    {
      // Extract attributes if any.
      //
      assert (attributes_.empty ());
      attributes& a (attributes_push (t, tt));

      // We always start with one or more names.
      //
      if (tt != type::word    &&
          tt != type::lcbrace && // Untyped name group: '{foo ...'
          tt != type::dollar  && // Variable expansion: '$foo ...'
          tt != type::lparen  && // Eval context: '(foo) ...'
          tt != type::colon)     // Empty name: ': ...'
      {
        // Something else. Let our caller handle that.
        //
        if (a)
          fail (a.loc) << "attributes before " << t;
        else
          attributes_pop ();

        break;
      }

      // Now we will either parse something or fail.
      //
      if (!parsed)
        parsed = true;

      // See if this is one of the directives.
      //
      if (tt == type::word && keyword (t))
      {
        const string& n (t.value);
        void (parser::*f) (token&, token_type&) = nullptr;

        if (n == "print")
        {
          // @@ Is this the only place where it is valid? Probably also
          // in var namespace.
          //
          f = &parser::parse_print;
        }
        else if (n == "source")
        {
          f = &parser::parse_source;
        }
        else if (n == "include")
        {
          f = &parser::parse_include;
        }
        else if (n == "import")
        {
          f = &parser::parse_import;
        }
        else if (n == "export")
        {
          f = &parser::parse_export;
        }
        else if (n == "using" ||
                 n == "using?")
        {
          f = &parser::parse_using;
        }
        else if (n == "define")
        {
          f = &parser::parse_define;
        }
        else if (n == "if" ||
                 n == "if!")
        {
          f = &parser::parse_if_else;
        }
        else if (n == "else" ||
                 n == "elif" ||
                 n == "elif!")
        {
          // Valid ones are handled in if_else().
          //
          fail (t) << n << " without if";
        }

        if (f != nullptr)
        {
          if (a)
            fail (a.loc) << "attributes before " << n;
          else
            attributes_pop ();

          (this->*f) (t, tt);
          continue;
        }
      }

      // ': foo' is equvalent to '{}: foo' and to 'dir{}: foo'.
      //
      // @@ I think we should make ': foo' invalid.
      //
      const location nloc (get_location (t));
      names ns (tt != type::colon
                ? parse_names (t, tt)
                : names ({name ("dir", string ())}));

      if (tt == type::colon)
      {
        // While '{}:' means empty name, '{$x}:' where x is empty list
        // means empty list.
        //
        if (ns.empty ())
          fail (t) << "target expected before :";

        next (t, tt);

        if (tt == type::newline)
        {
          // See if this is a directory/target scope.
          //
          if (peek () == type::lcbrace)
          {
            next (t, tt);

            // Should be on its own line.
            //
            if (next (t, tt) != type::newline)
              fail (t) << "expected newline after {";

            // See if this is a directory or target scope. Different
            // things can appear inside depending on which one it is.
            //
            bool dir (false);
            for (const auto& n: ns)
            {
              if (n.directory ())
              {
                if (ns.size () != 1)
                {
                  // @@ TODO: point to name (and above).
                  //
                  fail (nloc) << "multiple names in directory scope";
                }

                dir = true;
              }
            }

            next (t, tt);

            if (dir)
            {
              // Directory scope.
              //
              if (a)
                fail (a.loc) << "attributes before directory scope";
              else
                attributes_pop ();

              // Can contain anything that a top level can.
              //
              enter_scope sg (*this, move (ns[0].dir));
              parse_clause (t, tt);
            }
            else
            {
              if (a)
                fail (a.loc) << "attributes before target scope";
              else
                attributes_pop ();

              // @@ TODO: target scope.
            }

            if (tt != type::rcbrace)
              fail (t) << "expected } instead of " << t;

            // Should be on its own line.
            //
            if (next (t, tt) == type::newline)
              next (t, tt);
            else if (tt != type::eos)
              fail (t) << "expected newline after }";

            continue;
          }

          // If this is not a scope, then it is a target without any
          // prerequisites. Fall through.
          //
        }

        // Dependency declaration or scope/target-specific variable
        // assignment.
        //

        if (a)
          fail (a.loc) << "attributes before target/scope";
        else
          attributes_pop ();

        attributes& a (attributes_push (t, tt));

        if (tt == type::word    ||
            tt == type::lcbrace ||
            tt == type::dollar  ||
            tt == type::lparen  ||
            tt == type::newline ||
            tt == type::eos)
        {
          const location ploc (get_location (t));
          names pns (tt != type::newline && tt != type::eos
                     ? parse_names (t, tt)
                     : names ());

          // Scope/target-specific variable assignment.
          //
          if (tt == type::assign || tt == type::prepend || tt == type::append)
          {
            token at (t);
            type att (tt);

            const variable& var (
              var_pool.insert (parse_variable_name (move (pns), ploc)));

            // Apply variable attributes.
            //
            apply_variable_attributes (var);

            // If we have multiple targets/scopes, then we save the value
            // tokens when parsing the first one and then replay them for
            // the subsequent. We have to do it this way because the value
            // may contain variable expansions that would be sensitive to
            // the target/scope context in which they are evaluated.
            //
            // Note: watch out for an out-qualified single target (two names).
            //
            replay_guard rg (
              *this, ns.size () > 2 || (ns.size () == 2 && !ns[0].pair));

            for (auto i (ns.begin ()), e (ns.end ()); i != e; )
            {
              name& n (*i);

              if (n.qualified ())
                fail (nloc) << "project name in scope/target " << n;

              if (n.directory () && !n.pair) // Not out-qualified.
              {
                // Scope variable.
                //
                if (var.visibility == variable_visibility::target)
                  fail (ploc) << "variable " << var << " has target "
                              << "visibility but assigned in a scope" <<
                    info << "consider changing to '.../*: " << var << "'";

                enter_scope sg (*this, move (n.dir));
                parse_variable (t, tt, var, att);
              }
              else
              {
                // Figure out if this is a target or type/pattern-specific
                // variable.
                //
                size_t p (n.value.find ('*'));

                if (p == string::npos)
                {
                  name o (n.pair ? move (*++i) : name ());
                  enter_target tg (*this, move (n), move (o), nloc, trace);
                  parse_variable (t, tt, var, att);
                }
                else
                {
                  // See tests/variable/type-pattern.
                  //
                  if (n.pair)
                    fail (nloc) << "out-qualified target type/pattern-"
                                << "specific variable";

                  if (n.value.find ('*', p + 1) != string::npos)
                    fail (nloc) << "multiple wildcards in target type/pattern "
                                << n;

                  // If we have the directory, then it is the scope.
                  //
                  enter_scope sg;
                  if (!n.dir.empty ())
                    sg = enter_scope (*this, move (n.dir));

                  // Resolve target type. If none is specified or if it is
                  // '*', use the root of the hierarchy. So these are all
                  // equivalent:
                  //
                  // *: foo = bar
                  // {*}: foo = bar
                  // *{*}: foo = bar
                  //
                  const target_type* ti (
                    n.untyped () || n.type == "*"
                    ? &target::static_type
                    : scope_->find_target_type (n.type));

                  if (ti == nullptr)
                    fail (nloc) << "unknown target type " << n.type;

                  // Note: expanding the value in the context of the scope.
                  //
                  value rhs (parse_variable_value (t, tt));

                  // Leave the value untyped unless we are assigning.
                  //
                  pair<reference_wrapper<value>, bool> p (
                    scope_->target_vars[*ti][move (n.value)].insert (
                      var, att == type::assign));

                  value& lhs (p.first);

                  // We store prepend/append values untyped (similar to
                  // overrides).
                  //
                  if (p.second)
                  {
                    // Note: we are always using assign and we don't pass the
                    // variable in case of prepend/append in order to keep the
                    // value untyped.
                    //
                    apply_value_attributes (
                      att == type::assign ? &var : nullptr,
                      lhs,
                      move (rhs),
                      type::assign);

                    // Map assignment type to value::extra constant.
                    //
                    lhs.extra =
                      att == type::prepend ? 1 :
                      att == type::append  ? 2 :
                      0;
                  }
                  else
                  {
                    // Existing value. What happens next depends what we are
                    // trying to do and what's already there.
                    //
                    // Assignment is the easy one: we simply overwrite what's
                    // already there. Also, if we are appending/prepending to
                    // a previously assigned value, then we simply append or
                    // prepend normally.
                    //
                    if (att == type::assign || lhs.extra == 0)
                    {
                      // Above we instructed insert() not to type the value so
                      // we have to compensate for that now.
                      //
                      if (att != type::assign)
                      {
                        if (var.type != nullptr && lhs.type != var.type)
                          typify (lhs, *var.type, &var);
                      }
                      else
                        lhs.extra = 0; // Change to assignment.

                      apply_value_attributes (&var, lhs, move (rhs), att);
                    }
                    else
                    {
                      // This is an append/prepent to a previously appended or
                      // prepended value. We can handle it as long as things
                      // are consistent.
                      //
                      if (att == type::prepend && lhs.extra == 2)
                        fail (at) << "prepend to a previously appended target "
                                  << "type/pattern-specific variable " << var;

                      if (att == type::append && lhs.extra == 1)
                        fail (at) << "append to a previously prepended target "
                                  << "type/pattern-specific variable " << var;

                      // Do untyped prepend/append.
                      //
                      apply_value_attributes (nullptr, lhs, move (rhs), att);
                    }
                  }

                  if (lhs.extra != 0 && lhs.type != nullptr)
                    fail (at) << "typed prepend/append to target type/pattern-"
                              << "specific variable " << var;
                }
              }

              if (++i != e)
                rg.play (); // Replay.
            }
          }
          // Dependency declaration.
          //
          else
          {
            if (a)
              fail (a.loc) << "attributes before prerequisites";
            else
              attributes_pop ();

            // Prepare the prerequisite list.
            //
            target::prerequisites_type ps;
            ps.reserve (pns.size ());

            for (auto& pn: pns)
            {
              const string* e;
              const target_type* ti (scope_->find_target_type (pn, e));

              if (ti == nullptr)
                fail (ploc) << "unknown target type " << pn.type;

              if (!pn.dir.empty ())
                pn.dir.normalize ();

              // Find or insert.
              //
              // @@ OUT: for now we assume the prerequisite's out is
              // undetermined. The only way to specify an src prerequisite
              // will be with the explicit @-syntax.
              //
              // Perhaps use @file{foo} as a way to specify it is in the out
              // tree, e.g., to suppress any src searches? The issue is what
              // to use for such a special indicator. Also, one can easily and
              // natually suppress any searches by specifying the absolute
              // path.
              //
              prerequisite& p (
                scope_->prerequisites.insert (
                  pn.proj,
                  *ti,
                  move (pn.dir),
                  dir_path (),
                  move (pn.value),
                  e,
                  *scope_,
                  trace).first);

              ps.emplace_back (p);
            }

            for (auto& tn: ns)
            {
              if (tn.qualified ())
                fail (nloc) << "project name in target " << tn;

              // @@ OUT TODO
              //
              enter_target tg (*this, move (tn), name (), nloc, trace);

              //@@ OPT: move if last/single target (common cases).
              //
              target_->prerequisites.insert (
                target_->prerequisites.end (), ps.begin (), ps.end ());

              if (default_target_ == nullptr)
                default_target_ = target_;
            }
          }

          if (tt == type::newline)
            next (t, tt);
          else if (tt != type::eos)
            fail (t) << "expected newline instead of " << t;

          continue;
        }

        if (tt == type::eos)
          continue;

        fail (t) << "expected newline instead of " << t;
      }

      // Variable assignment.
      //
      if (tt == type::assign || tt == type::prepend || tt == type::append)
      {
        const variable& var (
          var_pool.insert (parse_variable_name (move (ns), nloc)));

        // Apply variable attributes.
        //
        apply_variable_attributes (var);

        if (var.visibility == variable_visibility::target)
          fail (nloc) << "variable " << var << " has target visibility but "
                      << "assigned in a scope" <<
            info << "consider changing to '*: " << var << "'";

        parse_variable (t, tt, var, tt);

        if (tt == type::newline)
          next (t, tt);
        else if (tt != type::eos)
          fail (t) << "expected newline instead of " << t;

        continue;
      }

      // Allow things like function calls that don't result in anything.
      //
      if (tt == type::newline && ns.empty ())
      {
        if (a)
          fail (a.loc) << "standalone attributes";
        else
          attributes_pop ();

        next (t, tt);
        continue;
      }

      fail (t) << "unexpected " << t;
    }

    return parsed;
  }

  void parser::
  parse_source (token& t, type& tt)
  {
    tracer trace ("parser::source", &path_);

    // The rest should be a list of buildfiles. Parse them as names in the
    // value mode to get variable expansion and directory prefixes.
    //
    mode (lexer_mode::value);
    next (t, tt);
    const location l (get_location (t));
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt)
              : names ());

    for (name& n: ns)
    {
      if (n.pair || n.qualified () || n.empty () || n.value.empty ())
        fail (l) << "expected buildfile instead of " << n;

      // Construct the buildfile path.
      //
      path p (move (n.dir));
      p /= path (move (n.value));

      // If the path is relative then use the src directory corresponding
      // to the current directory scope.
      //
      if (scope_->src_path_ != nullptr && p.relative ())
        p = scope_->src_path () / p;

      p.normalize ();

      try
      {
        ifdstream ifs (p);

        l5 ([&]{trace (t) << "entering " << p;});

        enter_buildfile (p);

        const path* op (path_);
        path_ = &p;

        lexer l (ifs, *path_);
        lexer* ol (lexer_);
        lexer_ = &l;

        token t;
        type tt;
        next (t, tt);
        parse_clause (t, tt);

        if (tt != type::eos)
          fail (t) << "unexpected " << t;

        l5 ([&]{trace (t) << "leaving " << p;});

        lexer_ = ol;
        path_ = op;
      }
      catch (const io_error& e)
      {
        fail (l) << "unable to read buildfile " << p << ": " << e.what ();
      }
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  void parser::
  parse_include (token& t, type& tt)
  {
    tracer trace ("parser::include", &path_);

    if (root_->src_path_ == nullptr)
      fail (t) << "inclusion during bootstrap";

    // The rest should be a list of buildfiles. Parse them as names in the
    // value mode to get variable expansion and directory prefixes.
    //
    mode (lexer_mode::value);
    next (t, tt);
    const location l (get_location (t));
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt)
              : names ());

    for (name& n: ns)
    {
      if (n.pair || n.qualified () || n.empty ())
        fail (l) << "expected buildfile instead of " << n;

      // Construct the buildfile path. If it is a directory, then append
      // 'buildfile'.
      //
      path p (move (n.dir));
      if (n.value.empty ())
        p /= "buildfile";
      else
      {
        bool d (path::traits::is_separator (n.value.back ()));

        p /= path (move (n.value));
        if (d)
          p /= "buildfile";
      }

      l6 ([&]{trace (l) << "relative path " << p;});

      // Determine new out_base.
      //
      dir_path out_base;

      if (p.relative ())
      {
        out_base = scope_->out_path () / p.directory ();
        out_base.normalize ();
      }
      else
      {
        p.normalize ();

        // Make sure the path is in this project. Include is only meant
        // to be used for intra-project inclusion (plus amalgamation).
        //
        bool in_out (false);
        if (!p.sub (root_->src_path ()) &&
            !(in_out = p.sub (root_->out_path ())))
          fail (l) << "out of project include " << p;

        out_base = in_out
          ? p.directory ()
          : out_src (p.directory (), *root_);
      }

      // Switch the scope. Note that we need to do this before figuring
      // out the absolute buildfile path since we may switch the project
      // root and src_root with it (i.e., include into a sub-project).
      //
      scope* ors (root_);
      scope* ocs (scope_);
      switch_scope (out_base);

      // Use the new scope's src_base to get absolute buildfile path
      // if it is relative.
      //
      if (p.relative ())
        p = scope_->src_path () / p.leaf ();

      l6 ([&]{trace (l) << "absolute path " << p;});

      if (!root_->buildfiles.insert (p).second) // Note: may be "new" root.
      {
        l5 ([&]{trace (l) << "skipping already included " << p;});
        scope_ = ocs;
        root_ = ors;
        continue;
      }

      try
      {
        ifdstream ifs (p);

        l5 ([&]{trace (t) << "entering " << p;});

        enter_buildfile (p);

        const path* op (path_);
        path_ = &p;

        lexer l (ifs, *path_);
        lexer* ol (lexer_);
        lexer_ = &l;

        target* odt (default_target_);
        default_target_ = nullptr;

        token t;
        type tt;
        next (t, tt);
        parse_clause (t, tt);

        if (tt != type::eos)
          fail (t) << "unexpected " << t;

        process_default_target (t);

        l5 ([&]{trace (t) << "leaving " << p;});

        default_target_ = odt;
        lexer_ = ol;
        path_ = op;
      }
      catch (const io_error& e)
      {
        fail (l) << "unable to read buildfile " << p << ": " << e.what ();
      }

      scope_ = ocs;
      root_ = ors;
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  void parser::
  parse_import (token& t, type& tt)
  {
    tracer trace ("parser::import", &path_);

    if (root_->src_path_ == nullptr)
      fail (t) << "import during bootstrap";

    // General import format:
    //
    // import [<var>=](<project>|<project>/<target>])+
    //
    type at; // Assignment type.
    value* val (nullptr);
    const build2::variable* var (nullptr);

    // We are now in the normal lexing mode and here is the problem: we need
    // to switch to the value mode so that we don't treat certain characters
    // as separators (e.g., + in 'libstdc++'). But at the same time we need
    // to detect if we have the <var>= part. So what we are going to do is
    // switch to the value mode, get the first token, and then re-parse it
    // manually looking for =/=+/+=.
    //
    mode (lexer_mode::value);
    next (t, tt);

    // Get variable attributes, if any (note that here we will go into a
    // nested value mode with a different pair character).
    //
    attributes& a (attributes_push (t, tt));

    if (tt == type::word)
    {
      // Split the token into the variable name and value at position (p) of
      // '=', taking into account leading/trailing '+'. The variable name is
      // returned while the token is set to value. If the resulting token
      // value is empty, get the next token. Also set assignment type (at).
      //
      auto split = [&at, &t, &tt, this] (size_t p) -> string
      {
        string& v (t.value);
        size_t e;

        if (p != 0 && v[p - 1] == '+') // +=
        {
          e = p--;
          at = type::append;
        }
        else if (p + 1 != v.size () && v[p + 1] == '+') // =+
        {
          e = p + 1;
          at = type::prepend;
        }
        else // =
        {
          e = p;
          at = type::assign;
        }

        string nv (v, e + 1); // value
        v.resize (p);         // var name
        v.swap (nv);

        if (v.empty ())
          next (t, tt);

        return nv;
      };

      // Is this the 'foo=...' case?
      //
      size_t p (t.value.find ('='));

      if (p != string::npos)
        var = &var_pool.insert (split (p));
      //
      // This could still be the 'foo =...' case.
      //
      else if (peek () == type::word)
      {
        const string& v (peeked ().value);
        size_t n (v.size ());

        // We should start with =/+=/=+.
        //
        if (n > 0 &&
            (v[p = 0] == '=' ||
             (n > 1 && v[0] == '+' && v[p = 1] == '=')))
        {
          var = &var_pool[t.value];
          next (t, tt); // Get the peeked token.
          split (p);    // Returned name should be empty.
        }
      }
    }

    if (var != nullptr)
    {
      // Apply variable attributes.
      //
      apply_variable_attributes (*var);

      val = at == type::assign
        ? &scope_->assign (*var)
        : &scope_->append (*var);
    }
    else
    {
      if (a)
        fail (a.loc) << "attributes without variable";
      else
        attributes_pop ();
    }

    // The rest should be a list of projects and/or targets. Parse
    // them as names to get variable expansion and directory prefixes.
    //
    const location l (get_location (t));
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt)
              : names ());

    for (name& n: ns)
    {
      if (n.pair)
        fail (l) << "unexpected pair in import";

      // build2::import() will check the name, if required.
      //
      names r (build2::import (*scope_, move (n), l));

      if (val != nullptr)
      {
        if (at == type::assign)
          val->assign (move (r), var);
        else if (at == type::prepend)
          val->prepend (move (r), var);
        else
          val->append (move (r), var);
      }
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  void parser::
  parse_export (token& t, type& tt)
  {
    tracer trace ("parser::export", &path_);

    scope* ps (scope_->parent_scope ());

    // This should be temp_scope.
    //
    if (ps == nullptr || ps->out_path () != scope_->out_path ())
      fail (t) << "export outside export stub";

    // The rest is a value. Parse it as a variable value to get expansion,
    // attributes, etc. build2::import() will check the names, if required.
    //
    value rhs (parse_variable_value (t, tt));

    // While it may seem like supporting attributes is a good idea here,
    // there is actually little benefit in being able to type them or to
    // return NULL.
    //
    // export_value_ = value (); // Reset to untyped NULL value.
    // value_attributes (nullptr,
    //                   export_value_,
    //                   move (rhs),
    //                   type::assign);
    if (attributes& a = attributes_top ())
      fail (a.loc) << "attributes in export";
    else
      attributes_pop ();

    if (!rhs)
      fail (t) << "null value in export";

    export_value_ = move (rhs).as<names> ();

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  void parser::
  parse_using (token& t, type& tt)
  {
    tracer trace ("parser::using", &path_);

    bool optional (t.value.back () == '?');

    if (optional && boot_)
      fail (t) << "optional module in bootstrap";

    // The rest should be a list of module names. Parse them as names in the
    // value mode to get variable expansion, etc.
    //
    mode (lexer_mode::value);
    next (t, tt);
    const location l (get_location (t));
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt)
              : names ());

    for (auto i (ns.begin ()); i != ns.end (); ++i)
    {
      string n, v;

      if (!i->simple ())
        fail (l) << "module name expected instead of " << *i;

      n = move (i->value);

      if (i->pair)
      {
        if (i->pair != '@')
          fail << "unexpected pair style in using directive";

        ++i;
        if (!i->simple ())
          fail (l) << "module version expected instead of " << *i;

        v = move (i->value);
      }

      // Handle the special 'build' module.
      //
      if (n == "build")
      {
        if (!v.empty ())
        {
          unsigned int iv;
          try {iv = to_version (v);}
          catch (const invalid_argument& e)
          {
            error (l) << "invalid version '" << v << "': " << e.what ();
            throw failed ();
          }

          if (iv > BUILD2_VERSION)
            fail (l) << "build2 " << v << " required" <<
              info << "running build2 " << BUILD2_VERSION_STR;
        }
      }
      else
      {
        assert (v.empty ()); // Module versioning not yet implemented.

        if (boot_)
          boot_module (n, *root_, l);
        else
          load_module (n, *root_, *scope_, l, optional);
      }
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  void parser::
  parse_define (token& t, type& tt)
  {
    // define <derived>: <base>
    //
    // See tests/define.
    //
    if (next (t, tt) != type::word)
      fail (t) << "expected name instead of " << t << " in target type "
               << "definition";

    string dn (move (t.value));
    const location dnl (get_location (t));

    if (next (t, tt) != type::colon)
      fail (t) << "expected ':' instead of " << t << " in target type "
               << "definition";

    next (t, tt);

    if (tt == type::word)
    {
      // Target.
      //
      const string& bn (t.value);
      const target_type* bt (scope_->find_target_type (bn));

      if (bt == nullptr)
        fail (t) << "unknown target type " << bn;

      if (!scope_->derive_target_type (move (dn), *bt).second)
        fail (dnl) << "target type " << dn << " already define in this scope";

      next (t, tt); // Get newline.
    }
    else
      fail (t) << "expected name instead of " << t << " in target type "
               << "definition";

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  void parser::
  parse_if_else (token& t, type& tt)
  {
    // Handle the whole if-else chain. See tests/if-else.
    //
    bool taken (false); // One of the branches has been taken.

    for (;;)
    {
      string k (move (t.value));
      next (t, tt);

      bool take (false); // Take this branch?

      if (k != "else")
      {
        // Should we evaluate the expression if one of the branches has
        // already been taken? On the one hand, evaluating it is a waste
        // of time. On the other, it can be invalid and the only way for
        // the user to know their buildfile is valid is to test every
        // branch. There could also be side effects. We also have the same
        // problem with ignored branch blocks except there evaluating it
        // is not an option. So let's skip it.
        //
        if (taken)
          skip_line (t, tt);
        else
        {
          if (tt == type::newline || tt == type::eos)
            fail (t) << "expected " << k << "-expression instead of " << t;

          // Parse as names to get variable expansion, evaluation, etc.
          //
          const location nsl (get_location (t));
          names ns (parse_names (t, tt));

          // Should evaluate to 'true' or 'false'.
          //
          try
          {
            if (ns.size () != 1)
              throw invalid_argument (string ());

            bool e (convert<bool> (move (ns[0])));
            take = (k.back () == '!' ? !e : e);
          }
          catch (const invalid_argument&)
          {
            fail (nsl) << "expected " << k << "-expression to evaluate to "
                       << "'true' or 'false' instead of '" << ns << "'";
          }
        }
      }
      else
        take = !taken;

      if (tt != type::newline)
        fail (t) << "expected newline instead of " << t << " after " << k
                 << (k != "else" ? "-expression" : "");

      // This can be a block or a single line.
      //
      if (next (t, tt) == type::lcbrace)
      {
        if (next (t, tt) != type::newline)
          fail (t) << "expected newline after {";

        next (t, tt);

        if (take)
        {
          parse_clause (t, tt);
          taken = true;
        }
        else
          skip_block (t, tt);

        if (tt != type::rcbrace)
          fail (t) << "expected } instead of " << t << " at the end of " << k
                   << "-block";

        next (t, tt);

        if (tt == type::newline)
          next (t, tt);
        else if (tt != type::eos)
          fail (t) << "expected newline after }";
      }
      else
      {
        if (take)
        {
          if (!parse_clause (t, tt, true))
            fail (t) << "expected " << k << "-line instead of " << t;

          taken = true;
        }
        else
        {
          skip_line (t, tt);

          if (tt == type::newline)
            next (t, tt);
        }
      }

      // See if we have another el* keyword.
      //
      if (k != "else" && tt == type::word && keyword (t))
      {
        const string& n (t.value);

        if (n == "else" || n == "elif" || n == "elif!")
          continue;
      }

      break;
    }
  }

  void parser::
  parse_print (token& t, type& tt)
  {
    // Parse the rest as a variable value to get expansion, attributes, etc.
    //
    value v (parse_variable_value (t, tt));

    if (attributes_top ())
    {
      // Round-trip it through (a potentially typed) value.
      //
      value tv;
      apply_value_attributes (nullptr, tv, move (v), type::assign);

      if (tv)
      {
        names storage;
        cout << reverse (tv, storage) << endl;
      }
      else
        cout << "[null]" << endl;
    }
    else
    {
      attributes_pop ();

      if (v)
        cout << v.as<names> () << endl;
      else
        cout << "[null]" << endl;
    }

    if (tt != type::eos)
      next (t, tt); // Swallow newline.
  }

  string parser::
  parse_variable_name (names&& ns, const location& l)
  {
    // The list should contain a single, simple name.
    //
    if (ns.size () != 1 || !ns[0].simple () || ns[0].empty ())
      fail (l) << "variable name expected instead of " << ns;

    string& n (ns[0].value);

    if (n.front () == '.') // Fully qualified name.
      return string (n, 1, string::npos);
    else
      //@@ TODO: append namespace if any.
      return move (n);
  }

  void parser::
  parse_variable (token& t, type& tt, const variable& var, type kind)
  {
    value rhs (parse_variable_value (t, tt));

    value& lhs (
      kind == type::assign
      ? target_ != nullptr ? target_->assign (var) : scope_->assign (var)
      : target_ != nullptr ? target_->append (var) : scope_->append (var));

    apply_value_attributes (&var, lhs, move (rhs), kind);
  }

  value parser::
  parse_variable_value (token& t, type& tt, lexer_mode m)
  {
    mode (m);
    next (t, tt);

    // Parse value attributes if any. Note that it's ok not to have anything
    // after the attributes (e.g., foo=[null]).
    //
    attributes_push (t, tt, true);

    return tt != type::newline && tt != type::eos
      ? parse_names_value (t, tt)
      : value (names ());
  }

  static const value_type*
  map_type (const string& n)
  {
    return
      n == "bool"         ? &value_traits<bool>::value_type         :
      n == "uint64"       ? &value_traits<uint64_t>::value_type     :
      n == "string"       ? &value_traits<string>::value_type       :
      n == "path"         ? &value_traits<path>::value_type         :
      n == "dir_path"     ? &value_traits<dir_path>::value_type     :
      n == "abs_dir_path" ? &value_traits<abs_dir_path>::value_type :
      n == "name"         ? &value_traits<name>::value_type         :
      n == "strings"      ? &value_traits<strings>::value_type      :
      n == "paths"        ? &value_traits<paths>::value_type        :
      n == "dir_paths"    ? &value_traits<dir_paths>::value_type    :
      n == "names"        ? &value_traits<names>::value_type        :
      nullptr;
  }

  void parser::
  apply_variable_attributes (const variable& var)
  {
    attributes a (attributes_pop ());

    if (!a)
      return;

    const location& l (a.loc);
    const value_type* type (nullptr);

    for (auto& p: a.ats)
    {
      string& k (p.first);
      string& v (p.second);

      if (const value_type* t = map_type (k))
      {
        if (type != nullptr && t != type)
          fail (l) << "multiple variable types: " << k << ", " << type->name;

        type = t;
        // Fall through.
      }
      else
        fail (l) << "unknown variable attribute " << k;

      if (!v.empty ())
        fail (l) << "unexpected value for attribute " << k << ": " << v;
    }

    if (type != nullptr)
    {
      if (var.type == nullptr)
        var.type = type;
      else if (var.type != type)
        fail (l) << "changing variable " << var << " type from "
                 << var.type->name << " to " << type->name;
    }
  }

  void parser::
  apply_value_attributes (const variable* var,
                          value& v,
                          value&& rhs,
                          token_type kind)
  {
    attributes a (attributes_pop ());
    const location& l (a.loc);

    // Essentially this is an attribute-augmented assign/append/prepend.
    //
    bool null (false);
    const value_type* type (nullptr);

    for (auto& p: a.ats)
    {
      string& k (p.first);
      string& v (p.second);

      if (k == "null")
      {
        if (rhs && !rhs.empty ()) // Note: null means we had an expansion.
          fail (l) << "value with null attribute";

        null = true;
        // Fall through.
      }
      else if (const value_type* t = map_type (k))
      {
        if (type != nullptr && t != type)
          fail (l) << "multiple value types: " << k << ", " << type->name;

        type = t;
        // Fall through.
      }
      else
        fail (l) << "unknown value attribute " << k;

      if (!v.empty ())
        fail (l) << "unexpected value for attribute " << k << ": " << v;
    }

    // When do we set the type and when do we keep the original? This gets
    // tricky for append/prepend where both values contribute. The guiding
    // rule here is that if the user specified the type, then they reasonable
    // expect the resulting value to be of that type. So for assign we always
    // override the type since it's a new value. For append/prepend we
    // override if the LHS value is NULL (which also covers undefined). We
    // also override if LHS is untyped. Otherwise, we require that the types
    // be the same. Also check that the requested value type doesn't conflict
    // with the variable type.
    //
    if (type != nullptr)
    {
      if (var != nullptr && var->type != nullptr && var->type != type)
        fail (l) << "conflicting variable " << var->name << " type "
                 << var->type->name << " and value type " << type->name;

      if (kind == token_type::assign)
      {
        if (type != v.type)
        {
          v = nullptr; // Clear old value.
          v.type = type;
        }
      }
      else
      {
        if (!v)
          v.type = type;
        else if (v.type == nullptr)
          typify (v, *type, var);
        else if (v.type != type)
          fail (l) << "conflicting original value type " << v.type->name
                   << " and append/prepend value type " << type->name;
      }
    }

    if (null)
    {
      if (kind == token_type::assign) // Ignore for prepend/append.
        v = nullptr;
    }
    else
    {
      if (kind == token_type::assign)
      {
        if (rhs)
          v.assign (move (rhs).as<names> (), var);
        else
          v = nullptr;
      }
      else if (rhs) // Don't append/prepent NULL.
      {
        if (kind == token_type::prepend)
          v.prepend (move (rhs).as<names> (), var);
        else
          v.append (move (rhs).as<names> (), var);
      }
    }
  }

  value parser::
  parse_eval (token& t, type& tt)
  {
    mode (lexer_mode::eval);
    next (t, tt);
    return parse_eval_trailer (t, tt);
  }

  value parser::
  parse_eval_trailer (token& t, type& tt)
  {
    // Parse the next value (if any) handling its attributes.
    //
    auto parse_value = [&t, &tt, this] () -> value
    {
      // Parse value attributes if any. Note that it's ok not to have anything
      // after the attributes, as in, ($foo == [null]), or even ([null])
      //
      attributes& a (attributes_push (t, tt, true));

      // Note that if names() gets called, it expects to see a name.
      //
      value r (tt != type::rparen     &&
               tt != type::colon      &&
               tt != type::equal      &&
               tt != type::not_equal  &&
               tt != type::less       &&
               tt != type::less_equal &&
               tt != type::greater    &&
               tt != type::greater_equal
               ? parse_names_value (t, tt)
               : value (names ()));

      // Process attributes if any.
      //
      if (!a)
      {
        attributes_pop ();
        return r;
      }

      value v;
      apply_value_attributes (nullptr, v, move (r), type::assign);
      return v;
    };

    value lhs (parse_value ());

    // We continue evaluating from left to right until we reach ')', storing
    // the result in lhs (think 'a == b == false').
    //
    while (tt != type::rparen)
    {
      const location l (get_location (t));

      // Remember to update parse_value above if adding any new name
      // separators here.
      //
      switch (tt)
      {
      case type::colon:
        {
          // Later, when we support '?:', we will need to decide which case
          // this is. But for now ':' is always a scope/target qualified name
          // which we represent as a special ':'-style pair.
          //
          if (lhs.type != nullptr || !lhs || lhs.empty ())
            fail (l) << "scope/target expected before ':'";

          names& ns (lhs.as<names> ());
          ns.back ().pair = ':';

          next (t, tt);
          value rhs (parse_eval_trailer (t, tt));

          if (tt != type::rparen ||
              rhs.type != nullptr || !rhs || rhs.empty ())
            fail (l) << "variable name expected after ':'";

          ns.insert (ns.end (),
                     make_move_iterator (rhs.as<names> ().begin ()),
                     make_move_iterator (rhs.as<names> ().end ()));

          break;
        }
      case type::equal:
      case type::not_equal:
      case type::less:
      case type::less_equal:
      case type::greater:
      case type::greater_equal:
        {
          type op (tt);

          // These are left-associative, so get the rhs value and evaluate.
          //
          // @@ In C++ == and != have lower precedence than <, etc.
          //
          next (t, tt);
          value rhs (parse_value ());

          // Use (potentially typed) comparison via value. If one of the
          // values is typed while the other is not, then try to convert the
          // untyped one to the other's type instead of complaining. This
          // seems like a reasonable thing to do and will allow us to write:
          //
          // if ($build.version > 30000)
          //
          // Rather than having to write:
          //
          // if ($build.version > [uint64] 30000)
          //
          if (lhs.type != rhs.type)
          {
            // @@ Would be nice to pass location for diagnostics.
            //
            if (lhs.type == nullptr)
            {
              if (lhs)
                typify (lhs, *rhs.type, nullptr);
            }
            else if (rhs.type == nullptr)
            {
              if (rhs)
                typify (rhs, *lhs.type, nullptr);
            }
            else
              fail (l) << "comparison between " << lhs.type->name << " and "
                       << rhs.type->name;
          }

          bool r;
          switch (op)
          {
          case type::equal:         r = lhs == rhs; break;
          case type::not_equal:     r = lhs != rhs; break;
          case type::less:          r = lhs <  rhs; break;
          case type::less_equal:    r = lhs <= rhs; break;
          case type::greater:       r = lhs >  rhs; break;
          case type::greater_equal: r = lhs >= rhs; break;
          default:                  r = false;      assert (false);
          }

          // Store the result as a bool value.
          //
          if (lhs.type != nullptr)
          {
            // Reset to NULL and untype.
            //
            lhs = nullptr;
            lhs.type = nullptr;
          }

          lhs = r;
          break;
        }
      default:
        fail (l) << "expected ')' instead of " << t;
      }
    }

    return lhs;
  }

  parser::attributes& parser::
  attributes_push (token& t, token_type& tt, bool standalone)
  {
    attributes_.push (
      attributes {tt == type::lsbrace, get_location (t), {}});

    attributes& a (attributes_.top ());
    const location& l (a.loc);

    if (!a.has)
      return a;

    // Using '@' for attribute key-value pairs would be just too ugly. Seeing
    // that we control what goes into keys/values, let's use a much nicer '='.
    //
    mode (lexer_mode::value, '=');
    next (t, tt);

    if (tt != type::rsbrace && tt != type::newline && tt != type::eos)
    {
      names ns (parse_names (t, tt));

      for (auto i (ns.begin ()); i != ns.end (); ++i)
      {
        string k, v;

        try
        {
          k = convert<string> (move (*i));
        }
        catch (const invalid_argument&)
        {
          fail (l) << "invalid attribute key '" << *i << "'";
        }

        if (i->pair)
        {
          if (i->pair != '=')
            fail (l) << "unexpected pair style in attributes";

          try
          {
            v = convert<string> (move (*++i));
          }
          catch (const invalid_argument&)
          {
            fail (l) << "invalid attribute value '" << *i << "'";
          }
        }

        a.ats.emplace_back (move (k), move (v));
      }
    }

    // Manually expire the value mode if we haven't reached newline/eos (where
    // it expires automatically).
    //
    if (mode () == lexer_mode::value)
      expire_mode ();

    if (tt != type::rsbrace)
      fail (t) << "expected ']' instead of " << t;

    next (t, tt);

    if (!standalone && (tt == type::newline || tt == type::eos))
      fail (t) << "standalone attributes";

    return a;
  }

  // Parse names inside {} and handle the following "crosses" (i.e.,
  // {a b}{x y}) if any. Return the number of names added to the list.
  //
  size_t parser::
  parse_names_trailer (token& t, type& tt,
                       names& ns,
                       const char* what,
                       size_t pair,
                       const string* pp,
                       const dir_path* dp,
                       const string* tp)
  {
    next (t, tt); // Get what's after '{'.

    size_t count (ns.size ());
    parse_names (t, tt,
                 ns,
                 false,
                 what,
                 (pair != 0
                  ? pair
                  : (ns.empty () || ns.back ().pair ? ns.size () : 0)),
                 pp, dp, tp);
    count = ns.size () - count;

    if (tt != type::rcbrace)
      fail (t) << "expected } instead of " << t;

    // See if we have a cross. See tests/names.
    //
    if (peek () == type::lcbrace && !peeked ().separated)
    {
      next (t, tt); // Get '{'.
      const location loc (get_location (t));

      names x; // Parse into a separate list of names.
      parse_names_trailer (t, tt, x, what, 0, nullptr, nullptr, nullptr);

      if (size_t n = x.size ())
      {
        // Now cross the last 'count' names in 'ns' with 'x'. First we will
        // allocate n - 1 additional sets of last 'count' names in 'ns'.
        //
        size_t b (ns.size () - count); // Start of 'count' names.
        ns.reserve (ns.size () + count * (n - 1));
        for (size_t i (0); i != n - 1; ++i)
          for (size_t j (0); j != count; ++j)
            ns.push_back (ns[b + j]);

        // Now cross each name, this time including the first set.
        //
        for (size_t i (0); i != n; ++i)
        {
          for (size_t j (0); j != count; ++j)
          {
            name& l (ns[b + i * count + j]);
            const name& r (x[i]);

            // Move the project names.
            //
            if (r.proj != nullptr)
            {
              if (l.proj != nullptr)
                fail (loc) << "nested project name " << *r.proj;

              l.proj = r.proj;
            }

            // Merge directories.
            //
            if (!r.dir.empty ())
            {
              if (l.dir.empty ())
                l.dir = move (r.dir);
              else
                l.dir /= r.dir;
            }

            // Figure out the type. As a first step, "promote" the lhs value
            // to type.
            //
            if (!l.value.empty ())
            {
              if (!l.type.empty ())
                fail (loc) << "nested type name " << l.value;

              l.type.swap (l.value);
            }

            if (!r.type.empty ())
            {
              if (!l.type.empty ())
                fail (loc) << "nested type name " << r.type;

              l.type = move (r.type);
            }

            l.value = move (r.value);

            // @@ TODO: need to handle pairs on lhs. I think all that needs
            //    to be done is skip pair's first elements. Maybe also check
            //    that there are no pairs on the rhs. There is just no easy
            //    way to enable the value mode to test it, yet.
          }
        }

        count *= n;
      }
    }

    return count;
  }

  // Slashe(s) plus '%'. Note that here we assume '/' is there since that's
  // in our buildfile "syntax".
  //
  static const string name_separators (
    string (path::traits::directory_separators) + '%');

  bool parser::
  parse_names (token& t, type& tt,
               names& ns,
               bool chunk,
               const char* what,
               size_t pair,
               const string* pp,
               const dir_path* dp,
               const string* tp)
  {
    tracer trace ("parser::names", &path_);

    bool null (false);

    // If pair is not 0, then it is an index + 1 of the first half of
    // the pair for which we are parsing the second halves, e.g.,
    // a@{b c d{e f} {}}.
    //

    // Buffer that is used to collect the complete name in case of an
    // unseparated variable expansion or eval context, e.g., foo$bar($baz)fox.
    // The idea is to concatenate all the individual parts in this buffer and
    // then re-inject it into the loop as a single token.
    //
    bool concat (false);
    string concat_str;

    // Number of names in the last group. This is used to detect when
    // we need to add an empty first pair element (e.g., @y) or when
    // we have a (for now unsupported) multi-name LHS (e.g., {x y}@z).
    //
    size_t count (0);

    for (bool first (true);; first = false)
    {
      // If the accumulating buffer is not empty, then we have two options:
      // continue accumulating or inject. We inject if the next token is
      // not a word, var expansion, or eval context or if it is separated.
      //
      if (concat &&
          ((tt != type::word   &&
            tt != type::dollar &&
            tt != type::lparen) || peeked ().separated))
      {
        tt = type::word;
        t = token (move (concat_str), true, false, t.line, t.column);
        concat = false;
      }
      else if (!first)
      {
        // If we are chunking, stop at the next separated token.
        //
        next (t, tt);

        if (chunk && t.separated)
          break;
      }

      // Name.
      //
      if (tt == type::word)
      {
        string name (move (t.value));
        tt = peek ();

        // Should we accumulate? If the buffer is not empty, then
        // we continue accumulating (the case where we are separated
        // should have been handled by the injection code above). If
        // the next token is a var expansion or eval context and it
        // is not separated, then we need to start accumulating.
        //
        if (concat ||                                       // Continue.
            ((tt == type::dollar ||
              tt == type::lparen) && !peeked ().separated)) // Start.
        {
          if (concat_str.empty ())
            concat_str = move (name);
          else
            concat_str += name;

          concat = true;
          continue;
        }

        // Find slash or %.
        //
        string::size_type p (name.find_last_of (name_separators));

        // First take care of project. A project-qualified name is
        // not very common, so we can afford some copying for the
        // sake of simplicity.
        //
        const string* pp1 (pp);

        if (p != string::npos)
        {
          bool last (name[p] == '%');
          string::size_type p1 (last ? p : name.rfind ('%', p - 1));

          if (p1 != string::npos)
          {
            string proj;
            proj.swap (name);

            // First fix the rest of the name.
            //
            name.assign (proj, p1 + 1, string::npos);
            p = last ? string::npos : p - (p1 + 1);

            // Now process the project name.
            //
            proj.resize (p1);

            if (pp != nullptr)
              fail (t) << "nested project name " << proj;

            pp1 = &project_name_pool.find (proj);
          }
        }

        string::size_type n (p != string::npos ? name.size () - 1 : 0);

        // See if this is a type name, directory prefix, or both. That
        // is, it is followed by an un-separated '{'.
        //
        if (tt == type::lcbrace && !peeked ().separated)
        {
          next (t, tt);

          if (p != n && tp != nullptr)
            fail (t) << "nested type name " << name;

          dir_path d1;
          const dir_path* dp1 (dp);

          string t1;
          const string* tp1 (tp);

          if (p == string::npos) // type
            tp1 = &name;
          else if (p == n) // directory
          {
            if (dp == nullptr)
              d1 = dir_path (name);
            else
              d1 = *dp / dir_path (name);

            dp1 = &d1;
          }
          else // both
          {
            t1.assign (name, p + 1, n - p);

            if (dp == nullptr)
              d1 = dir_path (name, 0, p + 1);
            else
              d1 = *dp / dir_path (name, 0, p + 1);

            dp1 = &d1;
            tp1 = &t1;
          }

          count = parse_names_trailer (t, tt, ns, what, pair, pp1, dp1, tp1);
          tt = peek ();
          continue;
        }

        // If we are a second half of a pair, add another first half
        // unless this is the first instance.
        //
        if (pair != 0 && pair != ns.size ())
          ns.push_back (ns[pair - 1]);

        count = 1;

        // If it ends with a directory separator, then it is a directory. Note
        // that at this stage we don't treat '.' and '..' as special (unless
        // they are specified with a directory separator) because then we
        // would have ended up treating '.: ...' as a directory scope.
        // Instead, this is handled higher up the processing chain, in
        // scope::find_target_type(). This would also mess up reversibility to
        // simple name.
        //
        // @@ TODO: and not quoted
        //
        if (p == n)
        {
          // For reversibility to simple name, only treat it as a directory
          // if the string is an exact representation.
          //
          dir_path dir (move (name), dir_path::exact);

          if (!dir.empty ())
          {
            if (dp != nullptr)
              dir = *dp / dir;

            ns.emplace_back (pp1,
                             move (dir),
                             (tp != nullptr ? *tp : string ()),
                             string ());
            continue;
          }
        }

        ns.emplace_back (pp1,
                         (dp != nullptr ? *dp : dir_path ()),
                         (tp != nullptr ? *tp : string ()),
                         move (name));
        continue;
      }

      // Variable expansion/function call or eval context.
      //
      if (tt == type::dollar || tt == type::lparen)
      {
        // These two cases are pretty similar in that in both we quickly end
        // up with a list of names that we need to splice into the result.
        //
        names lv_storage;
        names_view lv;

        // Check if we should set/propagate NULL. We only do this if this is
        // the only expansion, that is, it is the first and the text token is
        // not part of the name.
        //
        auto set_null = [first, &tt] ()
        {
          return first &&
            tt != type::word &&
            tt != type::dollar &&
            tt != type::lparen &&
            tt != type::lcbrace &&
            tt != type::pair_separator;
        };

        location loc;
        const char* what; // Variable, function, or evaluation context.
        value result;     // Holds function call/eval context result.

        if (tt == type::dollar)
        {
          // Switch to the variable name mode. We want to use this mode for
          // $foo but not for $(foo). Since we don't know whether the next
          // token is a paren or a word, we turn it on and switch to the eval
          // mode if what we get next is a paren.
          //
          mode (lexer_mode::variable);
          next (t, tt);
          loc = get_location (t);

          name qual;
          string name;

          if (tt == type::word)
            name = t.value;
          else if (tt == type::lparen)
          {
            expire_mode ();
            value v (parse_eval (t, tt)); //@@ OUT will parse @-pair and do well?

            if (!v)
              fail (loc) << "null variable/function name";

            names storage;
            vector_view<build2::name> ns (reverse (v, storage)); // Movable.
            size_t n (ns.size ());

            // Make sure the result of evaluation is a potentially-qualified
            // simple name.
            //
            if (n > 2 ||
                (n == 2 && ns[0].pair != ':') ||
                !ns[n - 1].simple ())
              fail (loc) << "variable/function name expected instead of '"
                         << ns << "'";

            if (n == 2)
            {
              qual = move (ns[0]);

              if (qual.empty ())
                fail (loc) << "empty variable/function qualification";

              qual.pair = '\0'; // We broke up the pair.
            }

            name = move (ns[n - 1].value);
          }
          else
            fail (t) << "variable/function name expected instead of " << t;

          if (name.empty ())
            fail (loc) << "empty variable/function name";

          // Figure out whether this is a variable expansion or a function
          // call.
          //
          tt = peek ();

          if (tt == type::lparen)
          {
            // Function call.
            //

            next (t, tt); // Get '('.

            // Just a stub for now.
            //
            // Should we use (target/scope) qualification (of name) as the
            // context in which to call the function?
            //
            value a (parse_eval (t, tt));
            cout << name << "(";

            if (a)
            {
              if (!a.empty ())
                cout << reverse (a, lv_storage);
            }
            else
              cout << "[null]";


            cout << ")" << endl;

            result = value (); // NULL for now.

            tt = peek ();

            // See if we should propagate the NULL indicator.
            //
            if (!result)
            {
              if (set_null ())
                null = true;

              continue;
            }

            if (result.empty ())
              continue;

            lv = reverse (result, lv_storage);
            what = "function call";
          }
          else
          {
            // Variable expansion.
            //
            lookup l (lookup_variable (move (qual), move (name), loc));

            if (!l)
            {
              // See if we should set the NULL indicator.
              //
              if (set_null ())
                null = true;

              continue;
            }

            if (l->empty ())
              continue;

            lv = reverse (*l, lv_storage);
            what = "variable expansion";
          }
        }
        else
        {
          loc = get_location (t);
          result = parse_eval (t, tt);

          tt = peek ();

          // See if we should propagate the NULL indicator.
          //
          if (!result)
          {
            if (set_null ())
              null = true;

            continue;
          }

          if (result.empty ())
            continue;

          lv = reverse (result, lv_storage);
          what = "context evaluation";
        }

        // @@ Could move if lv is lv_storage (or even result).
        //

        // Should we accumulate? If the buffer is not empty, then
        // we continue accumulating (the case where we are separated
        // should have been handled by the injection code above). If
        // the next token is a word or var expansion and it is not
        // separated, then we need to start accumulating.
        //
        if (concat ||                                 // Continue.
            ((tt == type::word   ||                   // Start.
              tt == type::dollar ||
              tt == type::lparen) && !peeked ().separated))
        {
          // This should be a simple value or a simple directory. The token
          // still points to the name (or closing paren).
          //
          if (lv.size () > 1)
            fail (loc) << "concatenating " << what << " contains multiple "
                       << "values";

          const name& n (lv[0]);

          if (n.qualified ())
            fail (loc) << "concatenating " << what << " contains project name";

          if (n.typed ())
            fail (loc) << "concatenating " << what << " contains type";

          if (!n.dir.empty ())
          {
            if (!n.value.empty ())
              fail (loc) << "concatenating " << what << " contains directory";

            // If this is an original name, then we cannot assume it is
            // actually a directory path (think s/foo/bar/) so we have to
            // reverse it exactly.
            //
            concat_str += n.original
              ? n.dir.representation ()
              : n.dir.string ();
          }
          else
            concat_str += n.value;

          concat = true;
        }
        else
        {
          // Copy the names from the variable into the resulting name list
          // while doing sensible things with the types and directories.
          //
          for (const name& n: lv)
          {
            const string* pp1 (pp);
            const dir_path* dp1 (dp);
            const string* tp1 (tp);

            if (n.proj != 0)
            {
              if (pp == nullptr)
                pp1 = n.proj;
              else
                fail (loc) << "nested project name " << *n.proj << " in "
                           << what;
            }

            dir_path d1;
            if (!n.dir.empty ())
            {
              if (dp != nullptr)
              {
                if (n.dir.absolute ())
                  fail (loc) << "nested absolute directory " << n.dir
                             << " in " << what;

                d1 = *dp / n.dir;
                dp1 = &d1;
              }
              else
                dp1 = &n.dir;
            }

            if (!n.type.empty ())
            {
              if (tp == nullptr)
                tp1 = &n.type;
              else
                fail (loc) << "nested type name " << n.type << " in " << what;
            }

            // If we are a second half of a pair.
            //
            if (pair != 0)
            {
              // Check that there are no nested pairs.
              //
              if (n.pair)
                fail (loc) << "nested pair in " << what;

              // And add another first half unless this is the first instance.
              //
              if (pair != ns.size ())
                ns.push_back (ns[pair - 1]);
            }

            ns.emplace_back (pp1,
                             (dp1 != nullptr ? *dp1 : dir_path ()),
                             (tp1 != nullptr ? *tp1 : string ()),
                             n.value);

            ns.back ().pair = n.pair;
            ns.back ().original = n.original;
          }

          count = lv.size ();
        }

        continue;
      }

      // Untyped name group without a directory prefix, e.g., '{foo bar}'.
      //
      if (tt == type::lcbrace)
      {
        count = parse_names_trailer (t, tt, ns, what, pair, pp, dp, tp);
        tt = peek ();
        continue;
      }

      // A pair separator (only in the value mode).
      //
      if (tt == type::pair_separator)
      {
        if (pair != 0)
          fail (t) << "nested pair on the right hand side of a pair";

        // Catch double pair separator ('@@'). Maybe we can use for something
        // later (e.g., escaping).
        //
        if (!ns.empty () && ns.back ().pair)
          fail (t) << "double pair separator";

        if (t.separated || count == 0)
        {
          // Empty LHS, (e.g., @y), create an empty name. The second test
          // will be in effect if we have something like v=@y.
          //
          ns.emplace_back (pp,
                           (dp != nullptr ? *dp : dir_path ()),
                           (tp != nullptr ? *tp : string ()),
                           string ());
          count = 1;
        }
        else if (count > 1)
          fail (t) << "multiple " << what << "s on the left hand side "
                   << "of a pair";

        ns.back ().pair = pair_separator ();
        tt = peek ();

        // If the next token is separated, then we have an empty RHS. Note
        // that the case where it is not a name/group (e.g., a newline/eos)
        // is handled below, once we are out of the loop.
        //
        if (peeked ().separated)
        {
          ns.emplace_back (pp,
                           (dp != nullptr ? *dp : dir_path ()),
                           (tp != nullptr ? *tp : string ()),
                           string ());
          count = 0;
        }

        continue;
      }

      // Note: remember to update set_null test if adding new recognized
      // tokens.

      if (!first)
        break;

      if (tt == type::rcbrace) // Empty name, e.g., dir{}.
      {
        // If we are a second half of a pair, add another first half
        // unless this is the first instance.
        //
        if (pair != 0 && pair != ns.size ())
          ns.push_back (ns[pair - 1]);

        ns.emplace_back (pp,
                         (dp != nullptr ? *dp : dir_path ()),
                         (tp != nullptr ? *tp : string ()),
                         string ());
        break;
      }
      else
        // Our caller expected this to be something.
        //
        fail (t) << "expected " << what << " instead of " << t;
    }

    // Handle the empty RHS in a pair, (e.g., y@).
    //
    if (!ns.empty () && ns.back ().pair)
    {
      ns.emplace_back (pp,
                       (dp != nullptr ? *dp : dir_path ()),
                       (tp != nullptr ? *tp : string ()),
                       string ());
    }

    return !null;
  }

  void parser::
  skip_line (token& t, type& tt)
  {
    for (; tt != type::newline && tt != type::eos; next (t, tt)) ;
  }

  void parser::
  skip_block (token& t, type& tt)
  {
    // Skip until } or eos, keeping track of the {}-balance.
    //
    for (size_t b (0); tt != type::eos; )
    {
      if (tt == type::lcbrace || tt == type::rcbrace)
      {
        type ptt (peek ());
        if (ptt == type::newline || ptt == type::eos) // Block { or }.
        {
          if (tt == type::lcbrace)
            ++b;
          else
          {
            if (b == 0)
              break;

            --b;
          }
        }
      }

      skip_line (t, tt);

      if (tt != type::eos)
        next (t, tt);
    }
  }

  bool parser::
  keyword (token& t)
  {
    assert (replay_ == replay::stop); // Can't be used in a replay.
    assert (t.type == type::word);

    // The goal here is to allow using keywords as variable names and
    // target types without imposing ugly restrictions/decorators on
    // keywords (e.g., '.using' or 'USING'). A name is considered a
    // potential keyword if:
    //
    // - it is not quoted [so a keyword can always be escaped] and
    // - next token is '\n' (or eos) or '(' [so if(...) will work] or
    // - next token is separated and is not '=', '=+', or '+=' [which
    //   means a "directive trailer" can never start with one of them].
    //
    // See tests/keyword.
    //
    if (!t.quoted)
    {
      // We cannot peek at the whole token here since it might have to be
      // lexed in a different mode. So peek at its first character.
      //
      pair<char, bool> p (lexer_->peek_char ());
      char c (p.first);

      return c == '\n' || c == '\0' || c == '(' ||
        (p.second && c != '=' && c != '+');
    }

    return false;
  }

  // Buildspec parsing.
  //

  // Here is the problem: we "overload" '(' and ')' to mean operation
  // application rather than the eval context. At the same time we want
  // to use names() to parse names, get variable expansion/function calls,
  // quoting, etc. We just need to disable the eval context. The way this
  // is done has two parts: Firstly, we parse names in chunks and detect
  // and handle the opening paren. In other words, a buildspec like
  // 'clean (./)' is "chunked" as 'clean', '(', etc. While this is fairly
  // straightforward, there is one snag: concatenating eval contexts, as
  // in 'clean(./)'. Normally, this will be treated as a single chunk and
  // we don't want that. So here comes the trick (or hack, if you like):
  // we will make every opening paren token "separated" (i.e., as if it
  // was proceeded by a space). This will disable concatenating eval. In
  // fact, we will even go a step further and only do this if we are in
  // the original value mode. This will allow us to still use eval
  // contexts in buildspec, provided that we quote it: '"cle(an)"'. Note
  // also that function calls still work as usual: '$filter (clean test)'.
  // To disable a function call and make it instead a var that is expanded
  // into operation name(s), we can use quoting: '"$ops"(./)'.
  //
  static void
  paren_processor (token& t, const lexer& l)
  {
    if (t.type == type::lparen && l.mode () == lexer_mode::value)
      t.separated = true;
  }

  buildspec parser::
  parse_buildspec (istream& is, const path& name)
  {
    path_ = &name;

    // We do "effective escaping" and only for ['"\$(] (basically what's
    // necessary inside a double-quoted literal plus the single quote).
    //
    lexer l (is, *path_, "\'\"\\$(", &paren_processor);
    lexer_ = &l;
    target_ = nullptr;
    scope_ = root_ = global_scope;

    // Turn on the value mode/pairs recognition with '@' as the pair separator
    // (e.g., src_root/@out_root/exe{foo bar}).
    //
    mode (lexer_mode::value);

    token t;
    type tt;
    next (t, tt);

    return parse_buildspec_clause (t, tt, type::eos);
  }

  static bool
  opname (const name& n)
  {
    // First it has to be a non-empty simple name.
    //
    if (n.pair || !n.simple () || n.empty ())
      return false;

    // C identifier.
    //
    for (size_t i (0); i != n.value.size (); ++i)
    {
      char c (n.value[i]);
      if (c != '_' && !(i != 0 ? alnum (c) : alpha (c)))
        return false;
    }

    return true;
  }

  buildspec parser::
  parse_buildspec_clause (token& t, type& tt, type tt_end)
  {
    buildspec bs;

    while (tt != tt_end)
    {
      // We always start with one or more names. Eval context (lparen) only
      // allowed if quoted.
      //
      if (tt != type::word    &&
          tt != type::lcbrace &&      // Untyped name group: '{foo ...'
          tt != type::dollar  &&      // Variable expansion: '$foo ...'
          !(tt == type::lparen && mode () == lexer_mode::double_quoted) &&
          tt != type::pair_separator) // Empty pair LHS: '@foo ...'
        fail (t) << "operation or target expected instead of " << t;

      const location l (get_location (t)); // Start of names.

      // This call will parse the next chunk of output and produce
      // zero or more names.
      //
      names ns (parse_names (t, tt, true));

      // What these names mean depends on what's next. If it is an
      // opening paren, then they are operation/meta-operation names.
      // Otherwise they are targets.
      //
      if (tt == type::lparen) // Peeked into by names().
      {
        if (ns.empty ())
          fail (t) << "operation name expected before '('";

        for (const name& n: ns)
          if (!opname (n))
            fail (l) << "operation name expected instead of '" << n << "'";

        // Inside '(' and ')' we have another, nested, buildspec.
        //
        next (t, tt);
        const location l (get_location (t)); // Start of nested names.
        buildspec nbs (parse_buildspec_clause (t, tt, type::rparen));

        // Merge the nested buildspec into ours. But first determine
        // if we are an operation or meta-operation and do some sanity
        // checks.
        //
        bool meta (false);
        for (const metaopspec& nms: nbs)
        {
          // We definitely shouldn't have any meta-operations.
          //
          if (!nms.name.empty ())
            fail (l) << "nested meta-operation " << nms.name;

          if (!meta)
          {
            // If we have any operations in the nested spec, then this
            // mean that our names are meta-operation names.
            //
            for (const opspec& nos: nms)
            {
              if (!nos.name.empty ())
              {
                meta = true;
                break;
              }
            }
          }
        }

        // No nested meta-operations means we should have a single
        // metaopspec object with empty meta-operation name.
        //
        assert (nbs.size () == 1);
        const metaopspec& nmo (nbs.back ());

        if (meta)
        {
          for (name& n: ns)
          {
            bs.push_back (nmo);
            bs.back ().name = move (n.value);
          }
        }
        else
        {
          // Since we are not a meta-operation, the nested buildspec
          // should be just a bunch of targets.
          //
          assert (nmo.size () == 1);
          const opspec& nos (nmo.back ());

          if (bs.empty () || !bs.back ().name.empty ())
            bs.push_back (metaopspec ()); // Empty (default) meta operation.

          for (name& n: ns)
          {
            bs.back ().push_back (nos);
            bs.back ().back ().name = move (n.value);
          }
        }

        next (t, tt); // Done with '('.
      }
      else if (!ns.empty ())
      {
        // Group all the targets into a single operation. In other
        // words, 'foo bar' is equivalent to 'update(foo bar)'.
        //
        if (bs.empty () || !bs.back ().name.empty ())
          bs.push_back (metaopspec ()); // Empty (default) meta operation.

        metaopspec& ms (bs.back ());

        for (auto i (ns.begin ()), e (ns.end ()); i != e; ++i)
        {
          // @@ We may actually want to support this at some point.
          //
          if (i->qualified ())
            fail (l) << "target name expected instead of " << *i;

          if (opname (*i))
            ms.push_back (opspec (move (i->value)));
          else
          {
            // Do we have the src_base?
            //
            dir_path src_base;
            if (i->pair)
            {
              if (i->pair != '@')
                fail << "unexpected pair style in buildspec";

              if (i->typed ())
                fail (l) << "expected target src_base instead of " << *i;

              src_base = move (i->dir);

              if (!i->value.empty ())
                src_base /= dir_path (move (i->value));

              ++i;
              assert (i != e); // Got to have the second half of the pair.
            }

            if (ms.empty () || !ms.back ().name.empty ())
              ms.push_back (opspec ()); // Empty (default) operation.

            opspec& os (ms.back ());
            os.emplace_back (move (src_base), move (*i));
          }
        }
      }
    }

    return bs;
  }

  lookup parser::
  lookup_variable (name&& qual, string&& name, const location& loc)
  {
    tracer trace ("parser::lookup_variable", &path_);

    // Process variable name.
    //
    if (name.front () == '.') // Fully namespace-qualified name.
      name.erase (0, 1);
    else
    {
      //@@ TODO: append namespace if any.
    }

    // If we are qualified, it can be a scope or a target.
    //
    enter_scope sg;
    enter_target tg;

    if (qual.directory ()) //@@ OUT
      sg = enter_scope (*this, move (qual.dir));
    else if (!qual.empty ())
      // @@ OUT TODO
      //
      tg = enter_target (*this, move (qual), build2::name (), loc, trace);

    // Lookup.
    //
    const auto& var (var_pool.insert (move (name)));
    return target_ != nullptr ? (*target_)[var] : (*scope_)[var];

    // Undefined/NULL namespace variables are not allowed.
    //
    // @@ TMP this isn't proving to be particularly useful.
    //
    // if (!l)
    // {
    //   if (var.name.find ('.') != string::npos)
    //     fail (loc) << "undefined/null namespace variable " << var;
    // }
  }

  void parser::
  switch_scope (const dir_path& p)
  {
    tracer trace ("parser::switch_scope", &path_);

    // First, enter the scope into the map and see if it is in any project. If
    // it is not, then there is nothing else to do.
    //
    auto i (scopes.insert (p, false));
    scope_ = &i->second;
    scope* rs (scope_->root_scope ());

    if (rs == nullptr)
      return;

    // Path p can be src_base or out_base. Figure out which one it is.
    //
    dir_path out_base (p.sub (rs->out_path ()) ? p : src_out (p, *rs));

    // Create and bootstrap root scope(s) of subproject(s) that this
    // scope may belong to. If any were created, load them. Note that
    // we need to do this before figuring out src_base since we may
    // switch the root project (and src_root with it).
    //
    {
      scope* nrs (&create_bootstrap_inner (*rs, out_base));

      if (rs != nrs)
        rs = nrs;
    }

    // Switch to the new root scope.
    //
    if (rs != root_)
    {
      load_root_pre (*rs); // Load new root(s) recursively.

      l5 ([&]{trace << "switching to root scope " << rs->out_path ();});
      root_ = rs;
    }

    // Now we can figure out src_base and finish setting the scope.
    //
    dir_path src_base (src_out (out_base, *rs));
    setup_base (i, move (out_base), move (src_base));
  }

  void parser::
  process_default_target (token& t)
  {
    tracer trace ("parser::process_default_target", &path_);

    // The logic is as follows: if we have an explicit current directory
    // target, then that's the default target. Otherwise, we take the
    // first target and use it as a prerequisite to create an implicit
    // current directory target, effectively making it the default
    // target via an alias. If there are no targets in this buildfile,
    // then we don't do anything.
    //
    if (default_target_ == nullptr ||      // No targets in this buildfile.
        targets.find (dir::static_type,    // Explicit current dir target.
                      scope_->out_path (),
                      dir_path (),         // Out tree target.
                      "",
                      nullptr,
                      trace) != targets.end ())
      return;

    target& dt (*default_target_);

    l5 ([&]{trace (t) << "creating current directory alias for " << dt;});

    target& ct (
      targets.insert (dir::static_type,
                      scope_->out_path (),
                      dir_path (),
                      "",
                      nullptr,
                      trace).first);

    prerequisite& p (
      scope_->prerequisites.insert (
        nullptr,
        dt.key (),
        *scope_,   // Doesn't matter which scope since dir is absolute.
        trace).first);

    p.target = &dt;
    ct.prerequisites.emplace_back (p);
  }

  void parser::
  enter_buildfile (const path& p)
  {
    tracer trace ("parser::enter_buildfile", &path_);

    dir_path d (p.directory ());

    // Figure out if we need out.
    //
    dir_path out;
    if (scope_->src_path_ != nullptr &&
        scope_->src_path () != scope_->out_path () &&
        d.sub (scope_->src_path ()))
    {
      out = out_src (d, *root_);
    }

    const char* e (p.extension ());
    targets.insert<buildfile> (
      move (d),
      move (out),
      p.leaf ().base ().string (),
      &extension_pool.find (e == nullptr ? "" : e), // Always specified.
      trace);
  }

  type parser::
  next (token& t, type& tt)
  {
    replay_token r;

    if (peeked_)
    {
      r = move (peek_);
      peeked_ = false;
    }
    else
      r = replay_ != replay::play ? lexer_next () : replay_next ();

    if (replay_ == replay::save)
      replay_data_.push_back (r);

    t = move (r.token);
    tt = t.type;
    return tt;
  }

  type parser::
  peek ()
  {
    if (!peeked_)
    {
      peek_ = (replay_ != replay::play ? lexer_next () : replay_next ());
      peeked_ = true;
    }

    return peek_.token.type;
  }
}
