// Copyright (C) 2019-2024 Joel Rosdahl and other contributors
//
// See doc/AUTHORS.adoc for a complete list of contributors.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 51
// Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

class Context;

namespace Util {

// Compute the length of the longest directory path that is common to paths
// `dir` (a directory) and `path` (any path).
size_t common_dir_prefix_length(std::string_view dir, std::string_view path);

// Compute a relative path from `dir` (an absolute path to a directory) to
// `path` (an absolute path). Assumes that both `dir` and `path` are normalized.
// The algorithm does *not* follow symlinks, so the result may not actually
// resolve to the same file as `path`.
std::string get_relative_path(std::string_view dir, std::string_view path);

// Construct a normalized native path.
//
// Example:
//
//   std::string path = Util::make_path("usr", "local", "bin");
template<typename... T>
std::string
make_path(const T&... args)
{
  return (std::filesystem::path{} / ... / args).lexically_normal().string();
}

// Make a relative path from current working directory (either `actual_cwd` or
// `apparent_cwd`) to `path` if `path` is under `base_dir`.
std::string make_relative_path(const std::string& base_dir,
                               const std::string& actual_cwd,
                               const std::string& apparent_cwd,
                               std::string_view path);

// Like above but with base directory and apparent/actual CWD taken from `ctx`.
std::string make_relative_path(const Context& ctx, std::string_view path);

// Normalize absolute path `path`, not taking symlinks into account.
//
// Normalization here means syntactically removing redundant slashes and
// resolving "." and ".." parts. The algorithm does however *not* follow
// symlinks, so the result may not actually resolve to the same filesystem entry
// as `path` (nor to any existing file system entry for that matter).
//
// On Windows: Backslashes are replaced with forward slashes.
std::string normalize_abstract_absolute_path(std::string_view path);

// Like normalize_abstract_absolute_path, but returns `path` unchanged if the
// normalized result doesn't resolve to the same file system entry as `path`.
std::string normalize_concrete_absolute_path(const std::string& path);

} // namespace Util