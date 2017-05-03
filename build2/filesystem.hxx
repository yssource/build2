// file      : build2/filesystem.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_FILESYSTEM_HXX
#define BUILD2_FILESYSTEM_HXX

#include <libbutl/filesystem.hxx>

#include <build2/types.hxx>
#include <build2/utility.hxx>

// Higher-level filesystem utilities built on top of <libbutl/filesystem.hxx>.
//
namespace build2
{
  // The dual interface wrapper for the {mk,rm}{file,dir}() functions
  // below that allows you to use it as a true/false return or a more
  // detailed enum from <libbutl/filesystem.hxx>
  //
  template <typename T>
  struct fs_status
  {
    T v;
    fs_status (T v): v (v) {};
    operator T () const {return v;}
    explicit operator bool () const {return v == T::success;}
  };

  // Create the directory and print the standard diagnostics starting from
  // the specified verbosity level.
  //
  // Note that this implementation is not suitable if it is expected that the
  // directory will exist in the majority of cases and performance is
  // important. See the fsdir{} rule for details.
  //
  using mkdir_status = butl::mkdir_status;

  fs_status<mkdir_status>
  mkdir (const dir_path&, uint16_t verbosity = 1);

  fs_status<mkdir_status>
  mkdir_p (const dir_path&, uint16_t verbosity = 1);

  // Remove the file and print the standard diagnostics starting from the
  // specified verbosity level. The second argument is only used in
  // diagnostics, to print the target name. Passing the path for target will
  // result in the relative path being printed.
  //
  using rmfile_status = butl::rmfile_status;

  template <typename T>
  fs_status<rmfile_status>
  rmfile (const path&, const T& target, uint16_t verbosity = 1);

  inline fs_status<rmfile_status>
  rmfile (const path& f, int verbosity = 1) // Literal overload (int).
  {
    return rmfile (f, f, static_cast<uint16_t> (verbosity));
  }

  // Similar to rmfile() but for directories (note: not -r).
  //
  using rmdir_status = butl::rmdir_status;

  template <typename T>
  fs_status<rmdir_status>
  rmdir (const dir_path&, const T& target, uint16_t verbosity = 1);

  inline fs_status<rmdir_status>
  rmdir (const dir_path& d, int verbosity = 1) // Literal overload (int).
  {
    return rmdir (d, d, static_cast<uint16_t> (verbosity));
  }

  // Remove the directory recursively and print the standard diagnostics
  // starting from the specified verbosity level. Note that this function
  // returns not_empty if we try to remove a working directory. If the dir
  // argument is false, then the directory itself is not removed.
  //
  // @@ Collides (via ADL) with butl::rmdir_r(), which sucks.
  //
  fs_status<rmdir_status>
  rmdir_r (const dir_path&, bool dir = true, uint16_t verbosity = 1);

  // Check for a file, directory or filesystem entry existence. Print the
  // diagnostics and fail on system error.
  //
  bool
  exists (const path&, bool follow_symlinks = true);

  bool
  exists (const dir_path&);

  bool
  entry_exists (const path&, bool follow_symlinks = false);

  // Check for a directory emptiness. Print the diagnostics and fail on system
  // error.
  //
  bool
  empty (const dir_path&);
}

#include <build2/filesystem.txx>

#endif // BUILD2_FILESYSTEM_HXX