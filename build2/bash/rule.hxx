// file      : build2/bash/rule.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_BASH_RULE_HXX
#define BUILD2_BASH_RULE_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/in/rule.hxx>
#include <build2/install/rule.hxx>

namespace build2
{
  namespace bash
  {
    // Preprocess a bash script (exe{}) or module (bash{}) .in file that
    // imports one or more bash modules.
    //
    // Note that the default substitution symbol is '@' and the mode is lax
    // (think bash arrays). The idea is that '@' is normally used in ways that
    // are highly unlikely to be misinterpreted as substitutions. The user,
    // however, is still able to override both of these choices with the
    // corresponding in.* variables (e.g., to use '`' and strict mode).
    //
    class in_rule: public in::rule
    {
    public:
      in_rule (): rule ("bash.in 1", "bash.in", '@', false /* strict */) {}

      virtual bool
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;

      virtual target_state
      perform_update (action, const target&) const override;

      virtual prerequisite_target
      search (action,
              const target&,
              const prerequisite_member&,
              include_type) const override;

      virtual optional<string>
      substitute (const location&,
                  action a,
                  const target&,
                  const string&,
                  bool) const override;

      string
      substitute_import (const location&,
                         action a,
                         const target&,
                         const string&) const;
    };

    // Installation rule for bash scripts (exe{}) and modules (bash{}). Here
    // we do:
    //
    // 1. Signal to in_rule that this is update for install.
    //
    // 2. Custom filtering of prerequisites.
    //
    class install_rule: public install::file_rule
    {
    public:
      install_rule (const in_rule& in): in_ (in) {}

      virtual bool
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;

      virtual const target*
      filter (action, const target&, const prerequisite&) const override;

    private:
      const in_rule& in_;
    };
  }
}

#endif // BUILD2_BASH_RULE_HXX
