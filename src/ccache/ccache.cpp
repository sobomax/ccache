// Copyright (C) 2002-2007 Andrew Tridgell
// Copyright (C) 2009-2025 Joel Rosdahl and other contributors
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

#include "ccache.hpp"

#include <ccache/argprocessing.hpp>
#include <ccache/args.hpp>
#include <ccache/argsinfo.hpp>
#include <ccache/compopt.hpp>
#include <ccache/context.hpp>
#include <ccache/core/cacheentry.hpp>
#include <ccache/core/common.hpp>
#include <ccache/core/exceptions.hpp>
#include <ccache/core/mainoptions.hpp>
#include <ccache/core/manifest.hpp>
#include <ccache/core/msvcshowincludesoutput.hpp>
#include <ccache/core/result.hpp>
#include <ccache/core/resultretriever.hpp>
#include <ccache/core/sloppiness.hpp>
#include <ccache/core/statistic.hpp>
#include <ccache/core/statistics.hpp>
#include <ccache/core/statisticscounters.hpp>
#include <ccache/core/statslog.hpp>
#include <ccache/core/types.hpp>
#include <ccache/depfile.hpp>
#include <ccache/execute.hpp>
#include <ccache/hash.hpp>
#include <ccache/hashutil.hpp>
#include <ccache/signalhandler.hpp>
#include <ccache/storage/storage.hpp>
#include <ccache/util/assertions.hpp>
#include <ccache/util/bytes.hpp>
#include <ccache/util/clang.hpp>
#include <ccache/util/conversion.hpp>
#include <ccache/util/defer.hpp>
#include <ccache/util/direntry.hpp>
#include <ccache/util/duration.hpp>
#include <ccache/util/environment.hpp>
#include <ccache/util/expected.hpp>
#include <ccache/util/fd.hpp>
#include <ccache/util/file.hpp>
#include <ccache/util/filestream.hpp>
#include <ccache/util/filesystem.hpp>
#include <ccache/util/format.hpp>
#include <ccache/util/logging.hpp>
#include <ccache/util/path.hpp>
#include <ccache/util/process.hpp>
#include <ccache/util/string.hpp>
#include <ccache/util/temporaryfile.hpp>
#include <ccache/util/time.hpp>
#include <ccache/util/timepoint.hpp>
#include <ccache/util/tokenizer.hpp>
#include <ccache/util/umaskscope.hpp>
#include <ccache/util/wincompat.hpp>

#include <fcntl.h>

#include <optional>
#include <string_view>

#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif

#include <nonstd/span.hpp>
#include <tl/expected.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace fs = util::filesystem;

using core::Statistic;
using util::DirEntry;

// This is a string that identifies the current "version" of the hash sum
// computed by ccache. If, for any reason, we want to force the hash sum to be
// different for the same input in a new ccache version, we can just change
// this string. A typical example would be if the format of one of the files
// stored in the cache changes in a backwards-incompatible way.
const char HASH_PREFIX[] = "4";

// Search for k_ccache_disable_token within the first
// k_ccache_disable_search_limit bytes of the input file.
const size_t k_ccache_disable_search_limit = 4096;

// String to look for when checking whether to disable ccache for the input
// file.
const char k_ccache_disable_token[] = {
  'c', 'c', 'a', 'c', 'h', 'e', ':', 'd', 'i', 's', 'a', 'b', 'l', 'e', '\0'};

namespace {

// Return tl::unexpected<Failure> if ccache did not succeed in getting
// or putting a result in the cache. If `exit_code` is set, ccache will just
// exit with that code directly, otherwise execute the real compiler and exit
// with its exit code. Statistics counters will also be incremented.
class Failure
{
public:
  Failure(Statistic statistic);
  Failure(std::initializer_list<Statistic> statistics);

  const core::StatisticsCounters& counters() const;
  std::optional<int> exit_code() const;
  void set_exit_code(int exit_code);

private:
  core::StatisticsCounters m_counters;
  std::optional<int> m_exit_code;
};

inline Failure::Failure(const Statistic statistic) : m_counters({statistic})
{
}

inline Failure::Failure(const std::initializer_list<Statistic> statistics)
  : m_counters(statistics)
{
}

inline const core::StatisticsCounters&
Failure::counters() const
{
  return m_counters;
}

inline std::optional<int>
Failure::exit_code() const
{
  return m_exit_code;
}

inline void
Failure::set_exit_code(const int exit_code)
{
  m_exit_code = exit_code;
}

} // namespace

static bool
should_disable_ccache_for_input_file(const fs::path& path)
{
  auto content =
    util::read_file_part<std::string>(path, 0, k_ccache_disable_search_limit);
  return content && content->find(k_ccache_disable_token) != std::string::npos;
}

static void
add_prefix(const Context& ctx, Args& args, const std::string& prefix_command)
{
  if (prefix_command.empty()) {
    return;
  }

  Args prefixes;
  for (const auto& prefix : util::split_into_strings(prefix_command, " ")) {
    prefixes.push_back(prefix);
  }

  if (!prefixes.empty() && !util::is_full_path(prefixes[0])) {
    std::string path = find_executable(ctx, prefixes[0], ctx.orig_args[0]);
    if (path.empty()) {
      throw core::Fatal(FMT("{}: {}", prefixes[0], strerror(errno)));
    }
    prefixes[0] = path;
  }

  LOG("Using command-line prefix {}", prefix_command);
  for (size_t i = prefixes.size(); i != 0; i--) {
    args.push_front(prefixes[i - 1]);
  }
}

static std::string
prepare_debug_path(const fs::path& cwd,
                   const fs::path& debug_dir,
                   const util::TimePoint& time_of_invocation,
                   const fs::path& output_obj,
                   std::string_view suffix)
{
  auto prefix =
    debug_dir.empty()
      ? output_obj
      : (debug_dir
         / (output_obj.is_absolute()
              ? output_obj
              : fs::weakly_canonical(cwd / output_obj).value_or(output_obj))
             .relative_path());

  // Ignore any error from fs::create_directories since we can't handle an error
  // in another way in this context. The caller takes care of logging when
  // trying to open the path for writing.
  fs::create_directories(prefix.parent_path());

  char timestamp[100];
  const auto tm = util::localtime(time_of_invocation);
  if (tm) {
    (void)strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &*tm);
  } else {
    (void)snprintf(
      timestamp,
      sizeof(timestamp),
      "%llu",
      static_cast<long long unsigned int>(time_of_invocation.sec()));
  }
  return FMT("{}.{}_{:06}.ccache-{}",
             prefix,
             timestamp,
             time_of_invocation.nsec_decimal_part() / 1000,
             suffix);
}

static void
init_hash_debug(Context& ctx,
                Hash& hash,
                char type,
                std::string_view section_name,
                FILE* debug_text_file)
{
  if (!ctx.config.debug() || ctx.config.debug_level() < 2) {
    return;
  }

  const auto path = prepare_debug_path(ctx.apparent_cwd,
                                       ctx.config.debug_dir(),
                                       ctx.time_of_invocation,
                                       ctx.args_info.output_obj,
                                       FMT("input-{}", type));
  util::FileStream debug_binary_file(path, "wb");
  if (debug_binary_file) {
    hash.enable_debug(section_name, debug_binary_file.get(), debug_text_file);
    ctx.hash_debug_files.push_back(std::move(debug_binary_file));
  } else {
    LOG("Failed to open {}: {}", path, strerror(errno));
  }
}

#ifndef _WIN32

static fs::path
follow_symlinks(const fs::path& path)
{
  // Follow symlinks to the real compiler to learn its name. We're not using
  // util::real_path in order to save some unnecessary stat calls.
  fs::path p = path;
  while (true) {
    auto symlink_target = fs::read_symlink(p);
    if (!symlink_target) {
      // Not a symlink.
      break;
    }
    if (symlink_target->is_absolute()) {
      p = *symlink_target;
    } else {
      p = p.parent_path() / *symlink_target;
    }
  }
  if (p != path) {
    LOG("Followed symlinks from {} to {} when guessing compiler type", path, p);
  }
  return p;
}

static fs::path
probe_generic_compiler(const fs::path& path)
{
  // Detect whether a generically named compiler (e.g. /usr/bin/cc) is a hard
  // link to a compiler with a more specific name.
  std::string name = util::pstr(path.filename()).str();
  if (name == "cc" || name == "c++") {
    static const char* candidate_names[] = {"clang", "clang++", "gcc", "g++"};
    for (const char* candidate_name : candidate_names) {
      fs::path candidate = path.parent_path() / candidate_name;
      if (fs::equivalent(candidate, path)) {
        LOG("Detected that {} is equivalent to {} when guessing compiler type",
            path,
            candidate);
        return candidate;
      }
    }
  }
  return path;
}

#endif

static CompilerType
do_guess_compiler(const fs::path& path)
{
  const auto name = util::to_lowercase(
    util::pstr(util::with_extension(path.filename(), "")).str());
  if (name.find("clang-cl") != std::string_view::npos) {
    return CompilerType::clang_cl;
  } else if (name.find("clang") != std::string_view::npos) {
    return CompilerType::clang;
  } else if (name.find("gcc") != std::string_view::npos
             || name.find("g++") != std::string_view::npos) {
    return CompilerType::gcc;
  } else if (name.find("nvcc") != std::string_view::npos) {
    return CompilerType::nvcc;
  } else if (name == "icl") {
    return CompilerType::icl;
  } else if (name == "icx") {
    return CompilerType::icx;
  } else if (name == "icx-cl") {
    return CompilerType::icx_cl;
  } else if (name == "cl") {
    return CompilerType::msvc;
  } else {
    return CompilerType::other;
  }
}

CompilerType
guess_compiler(const fs::path& path)
{
  CompilerType type = do_guess_compiler(path);
#ifdef _WIN32
  return type;
#else
  if (type == CompilerType::other) {
    return do_guess_compiler(probe_generic_compiler(follow_symlinks(path)));
  } else {
    return type;
  }
#endif
}

// This function hashes an include file and stores the path and hash in
// ctx.included_files. If the include file is a PCH, cpp_hash is also updated.
[[nodiscard]] tl::expected<void, Failure>
remember_include_file(Context& ctx,
                      const fs::path& path,
                      Hash& cpp_hash,
                      bool system,
                      Hash* depend_mode_hash)
{
  if (path == ctx.args_info.input_file) {
    // Don't remember the input file.
    return {};
  }

  util::PathString path_str(path);
  if (path_str.str().length() >= 2 && path_str.str()[0] == '<'
      && path_str.str()[path_str.str().length() - 1] == '>') {
    // Typically <built-in> or <command-line>.
    return {};
  }

  if (system
      && ctx.config.sloppiness().contains(core::Sloppy::system_headers)) {
    // Don't remember this system header.
    return {};
  }

  if (ctx.included_files.find(path_str) != ctx.included_files.end()) {
    // Already known include file.
    return {};
  }

#ifdef _WIN32
  {
    // stat fails on directories on win32.
    DWORD attributes = GetFileAttributes(path_str.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES
        && attributes & FILE_ATTRIBUTE_DIRECTORY) {
      return {};
    }
  }
#endif

  DirEntry dir_entry(path, DirEntry::LogOnError::yes);
  if (!dir_entry.exists()) {
    if (ctx.config.direct_mode()) {
      LOG("Include file {} does not exist, disabling direct mode",
          dir_entry.path());
      ctx.config.set_direct_mode(false);
    }
    return {};
  }
  if (dir_entry.is_directory()) {
    // Ignore directory, typically $PWD.
    return {};
  }
  if (!dir_entry.is_regular_file()) {
    // Device, pipe, socket or other strange creature.
    LOG("Non-regular include file {}", path);
    return tl::unexpected(Statistic::bad_input_file);
  }

  if (!ctx.ignore_header_paths.empty()) {
    // Canonicalize path for comparison; Clang uses ./header.h.
    const std::string& canonical_path_str =
      util::starts_with(path_str.str(), "./") ? path_str.str().substr(2)
                                              : path_str;
    for (const auto& ignore_header_path : ctx.ignore_header_paths) {
      if (file_path_matches_dir_prefix_or_file(ignore_header_path,
                                               canonical_path_str)) {
        return {};
      }
    }
  }

  // Let's hash the include file content.
  Hash::Digest file_digest;

  const bool is_pch = is_precompiled_header(path);

  fs::path path2(path);
  if (is_pch && !ctx.args_info.generating_pch) {
    if (ctx.args_info.included_pch_file.empty()) {
      LOG("Detected use of precompiled header: {}", path);
    }
    bool using_pch_sum = false;
    if (ctx.config.pch_external_checksum()) {
      // hash pch.sum instead of pch when it exists
      // to prevent hashing a very large .pch file every time
      fs::path pch_sum_path = util::add_extension(path, ".sum");
      if (DirEntry(pch_sum_path, DirEntry::LogOnError::yes).is_regular_file()) {
        path2 = std::move(pch_sum_path);
        using_pch_sum = true;
        LOG("Using pch.sum file {}", path2);
      }
    }

    if (!hash_binary_file(ctx, file_digest, path2)) {
      return tl::unexpected(Statistic::bad_input_file);
    }
    cpp_hash.hash_delimiter(using_pch_sum ? "pch_sum_hash" : "pch_hash");
    cpp_hash.hash(util::format_digest(file_digest));
  }

  if (ctx.config.direct_mode()) {
    if (!is_pch) { // else: the file has already been hashed.
      auto ret = hash_source_code_file(ctx, file_digest, path2);
      if (ret.contains(HashSourceCode::error)) {
        return tl::unexpected(Statistic::bad_input_file);
      }
      if (ret.contains(HashSourceCode::found_time)) {
        LOG_RAW("Disabling direct mode");
        ctx.config.set_direct_mode(false);
      }
    }

    if (depend_mode_hash) {
      depend_mode_hash->hash_delimiter("include");
      depend_mode_hash->hash(util::format_digest(file_digest));
    }
  }

  ctx.included_files.emplace(util::pstr(path2), file_digest);

  return {};
}

static void
print_included_files(const Context& ctx, FILE* fp)
{
  for (const auto& [path, digest] : ctx.included_files) {
    PRINT(fp, "{}\n", path);
  }
}

// This function reads and hashes a file. While doing this, it also does these
// things:
//
// - Makes include file paths for which the base directory is a prefix relative
//   when computing the hash sum.
// - Stores the paths and hashes of included files in ctx.included_files.
static tl::expected<void, Failure>
process_preprocessed_file(Context& ctx, Hash& hash, const fs::path& path)
{
  auto content = util::read_file<std::string>(path);
  if (!content) {
    LOG("Failed to read {}: {}", path, content.error());
    return tl::unexpected(Statistic::internal_error);
  }

  std::unordered_map<std::string, std::string> relative_inc_path_cache;

  // Bytes between p and q are pending to be hashed.
  std::string& data = *content;
  char* q = data.data();
  const char* p = q;
  const char* end = p + data.length();

  // There must be at least 7 characters (# 1 "x") left to potentially find an
  // include file path.
  while (q < end - 7) {
    static const std::string_view pragma_gcc_pch_preprocess =
      "pragma GCC pch_preprocess ";
    static const std::string_view hash_31_command_line_newline =
      "# 31 \"<command-line>\"\n";
    static const std::string_view hash_32_command_line_2_newline =
      "# 32 \"<command-line>\" 2\n";
    // Note: Intentionally not using the string form to avoid false positive
    // match by ccache itself.
    static const char incbin_directive[] = {'.', 'i', 'n', 'c', 'b', 'i', 'n'};

    // Check if we look at a line containing the file name of an included file.
    // At least the following formats exist (where N is a positive integer):
    //
    // GCC:
    //
    //   # N "file"
    //   # N "file" N
    //   #pragma GCC pch_preprocess "file"
    //
    // HP's compiler:
    //
    //   #line N "file"
    //
    // AIX's compiler:
    //
    //   #line N "file"
    //   #line N
    //
    // Note that there may be other lines starting with '#' left after
    // preprocessing as well, for instance "#    pragma".
    if (q[0] == '#'
        // GCC:
        && ((q[1] == ' ' && q[2] >= '0' && q[2] <= '9')
            // GCC precompiled header:
            || util::starts_with(&q[1], pragma_gcc_pch_preprocess)
            // HP/AIX:
            || (q[1] == 'l' && q[2] == 'i' && q[3] == 'n' && q[4] == 'e'
                && q[5] == ' '))
        && (q == data.data() || q[-1] == '\n')) {
      // Workarounds for preprocessor linemarker bugs in GCC version 6.
      if (q[2] == '3') {
        if (util::starts_with(q, hash_31_command_line_newline)) {
          // Bogus extra line with #31, after the regular #1: Ignore the whole
          // line, and continue parsing.
          hash.hash(p, q - p);
          while (q < end && *q != '\n') {
            q++;
          }
          q++;
          p = q;
          continue;
        } else if (util::starts_with(q, hash_32_command_line_2_newline)) {
          // Bogus wrong line with #32, instead of regular #1: Replace the line
          // number with the usual one.
          hash.hash(p, q - p);
          q += 1;
          q[0] = '#';
          q[1] = ' ';
          q[2] = '1';
          p = q;
        }
      }

      while (q < end && *q != '"' && *q != '\n') {
        q++;
      }
      if (q < end && *q == '\n') {
        // A newline before the quotation mark -> no match.
        continue;
      }
      q++;
      if (q >= end) {
        LOG_RAW("Failed to parse included file path");
        return tl::unexpected(Statistic::internal_error);
      }
      // q points to the beginning of an include file path
      hash.hash(p, q - p);
      p = q;
      while (q < end && *q != '"') {
        q++;
      }
      if (p == q) {
        // Skip empty file name.
        continue;
      }
      // Look for preprocessor flags, after the "filename".
      bool system = false;
      const char* r = q + 1;
      while (r < end && *r != '\n') {
        if (*r == '3') { // System header.
          system = true;
        }
        r++;
      }

      // p and q span the include file path.
      std::string inc_path(p, q - p);
      while (!inc_path.empty() && inc_path.back() == '/') {
        inc_path.pop_back();
      }
      if (!ctx.config.base_dir().empty()) {
        auto it = relative_inc_path_cache.find(inc_path);
        if (it == relative_inc_path_cache.end()) {
          std::string rel_inc_path =
            util::pstr(core::make_relative_path(ctx, inc_path));
          relative_inc_path_cache.emplace(inc_path, rel_inc_path);
          inc_path = util::pstr(rel_inc_path);
        } else {
          inc_path = it->second;
        }
      }

      if (inc_path != ctx.apparent_cwd || ctx.config.hash_dir()) {
        hash.hash(inc_path);
      }

      TRY(remember_include_file(ctx, inc_path, hash, system, nullptr));
      p = q; // Everything of interest between p and q has been hashed now.
    } else if (strncmp(q, incbin_directive, sizeof(incbin_directive)) == 0
               && ((q[7] == ' '
                    && (q[8] == '"' || (q[8] == '\\' && q[9] == '"')))
                   || q[7] == '"')) {
      if (ctx.config.sloppiness().contains(core::Sloppy::incbin)) {
        LOG_RAW(
          "Found potential unsupported .inc"
          "bin directive in source code but continuing due to enabled sloppy"
          " incbin handling");
        q += sizeof(incbin_directive);
        continue;
      }
      // An assembler .inc bin (without the space) statement, which could be
      // part of inline assembly, refers to an external file. If the file
      // changes, the hash should change as well, but finding out what file to
      // hash is too hard for ccache, so just bail out.
      LOG_RAW(
        "Found potential unsupported .inc"
        "bin directive in source code");
      return tl::unexpected(Failure(Statistic::unsupported_code_directive));
    } else if (strncmp(q, "___________", 10) == 0
               && (q == data.data() || q[-1] == '\n')) {
      // Unfortunately the distcc-pump wrapper outputs standard output lines:
      // __________Using distcc-pump from /usr/bin
      // __________Using # distcc servers in pump mode
      // __________Shutting down distcc-pump include server
      hash.hash(p, q - p);
      while (q < end && *q != '\n') {
        q++;
      }
      if (*q == '\n') {
        q++;
      }
      p = q;
      continue;
    } else {
      q++;
    }
  }

  hash.hash(p, (end - p));

  // Explicitly check the .gch/.pch/.pth file as Clang does not include any
  // mention of it in the preprocessed output.
  if (!ctx.args_info.included_pch_file.empty()
      && !ctx.args_info.generating_pch) {
    fs::path pch_path =
      core::make_relative_path(ctx, ctx.args_info.included_pch_file);
    hash.hash(pch_path);
    TRY(remember_include_file(ctx, pch_path, hash, false, nullptr));
  }

  bool debug_included = getenv("CCACHE_DEBUG_INCLUDED");
  if (debug_included) {
    print_included_files(ctx, stdout);
  }

  return {};
}

// Extract the used includes from the dependency file. Note that we cannot
// distinguish system headers from other includes here.
static tl::expected<Hash::Digest, Failure>
result_key_from_depfile(Context& ctx, Hash& hash)
{
  // Make sure that result hash will always be different from the manifest hash
  // since there otherwise may a storage key collision (in case the dependency
  // file is empty).
  hash.hash_delimiter("result");

  const auto file_content =
    util::read_file<std::string>(ctx.args_info.output_dep);
  if (!file_content) {
    LOG("Failed to read dependency file {}: {}",
        ctx.args_info.output_dep,
        file_content.error());
    return tl::unexpected(Statistic::bad_input_file);
  }

  bool seen_colon = false;
  for (std::string_view token : Depfile::tokenize(*file_content)) {
    if (token.empty()) {
      seen_colon = false;
      continue;
    }
    if (seen_colon) {
      fs::path path = core::make_relative_path(ctx, token);
      TRY(remember_include_file(ctx, path, hash, false, &hash));
    } else if (token == ":") {
      seen_colon = true;
    }
  }

  // Explicitly check the .gch/.pch/.pth file as it may not be mentioned in the
  // dependencies output.
  if (!ctx.args_info.included_pch_file.empty()
      && !ctx.args_info.generating_pch) {
    fs::path pch_path =
      core::make_relative_path(ctx, ctx.args_info.included_pch_file);
    hash.hash(pch_path);
    TRY(remember_include_file(ctx, pch_path, hash, false, nullptr));
  }

  bool debug_included = getenv("CCACHE_DEBUG_INCLUDED");
  if (debug_included) {
    print_included_files(ctx, stdout);
  }

  return hash.digest();
}

struct GetTmpFdResult
{
  util::Fd fd;
  fs::path path;
};

static GetTmpFdResult
get_tmp_fd(Context& ctx,
           const std::string_view description,
           const bool capture_output)
{
  if (capture_output) {
    auto tmp_stdout =
      util::value_or_throw<core::Fatal>(util::TemporaryFile::create(
        FMT("{}/{}", ctx.config.temporary_dir(), description)));
    ctx.register_pending_tmp_file(tmp_stdout.path);
    return {std::move(tmp_stdout.fd), std::move(tmp_stdout.path)};
  } else {
    const auto dev_null_path = util::get_dev_null_path();
    return {util::Fd(open(dev_null_path, O_WRONLY | O_BINARY)), dev_null_path};
  }
}

struct DoExecuteResult
{
  int exit_status;
  util::Bytes stdout_data;
  util::Bytes stderr_data;
};

// Extract the used includes from /showIncludes (or clang-cl's
// /showIncludes:user) output in stdout. Note that we cannot distinguish system
// headers from other includes when /showIncludes is used.
static tl::expected<Hash::Digest, Failure>
result_key_from_includes(Context& ctx, Hash& hash, std::string_view stdout_data)
{
  for (std::string_view include : core::MsvcShowIncludesOutput::get_includes(
         stdout_data, ctx.config.msvc_dep_prefix())) {
    const fs::path path = core::make_relative_path(ctx, include);
    TRY(remember_include_file(ctx, path, hash, false, &hash));
  }

  // Explicitly check the .pch file as it is not mentioned in the
  // includes output.
  if (!ctx.args_info.included_pch_file.empty()
      && !ctx.args_info.generating_pch) {
    fs::path pch_path =
      core::make_relative_path(ctx, ctx.args_info.included_pch_file);
    hash.hash(pch_path);
    TRY(remember_include_file(ctx, pch_path, hash, false, nullptr));
  }

  const bool debug_included = getenv("CCACHE_DEBUG_INCLUDED");
  if (debug_included) {
    print_included_files(ctx, stdout);
  }

  return hash.digest();
}

// Execute the compiler/preprocessor, with logic to retry without requesting
// colored diagnostics messages if that fails.
static tl::expected<DoExecuteResult, Failure>
do_execute(Context& ctx, Args& args, const bool capture_stdout = true)
{
  util::UmaskScope umask_scope(ctx.original_umask);

  if (ctx.diagnostics_color_failed) {
    DEBUG_ASSERT(ctx.config.compiler_type() == CompilerType::gcc);
    args.erase_last("-fdiagnostics-color");
  }

  auto tmp_stdout = get_tmp_fd(ctx, "stdout", capture_stdout);
  auto tmp_stderr = get_tmp_fd(ctx, "stderr", true);

  int status = execute(ctx,
                       args.to_argv().data(),
                       std::move(tmp_stdout.fd),
                       std::move(tmp_stderr.fd));
  if (status != 0 && !ctx.diagnostics_color_failed
      && ctx.config.compiler_type() == CompilerType::gcc) {
    const auto errors = util::read_file<std::string>(tmp_stderr.path);
    if (errors && errors->find("fdiagnostics-color") != std::string::npos) {
      // GCC versions older than 4.9 don't understand -fdiagnostics-color, and
      // non-GCC compilers misclassified as CompilerType::gcc might not do it
      // either. We assume that if the error message contains
      // "fdiagnostics-color" then the compilation failed due to
      // -fdiagnostics-color being unsupported and we then retry without the
      // flag. (Note that there intentionally is no leading dash in
      // "fdiagnostics-color" since some compilers don't include the dash in the
      // error message.)
      LOG_RAW("-fdiagnostics-color is unsupported; trying again without it");

      ctx.diagnostics_color_failed = true;
      return do_execute(ctx, args, capture_stdout);
    }
  }

  util::Bytes stdout_data;
  if (capture_stdout) {
    auto stdout_data_result = util::read_file<util::Bytes>(tmp_stdout.path);
    if (!stdout_data_result) {
      LOG("Failed to read {} (cleanup in progress?): {}",
          tmp_stdout.path,
          stdout_data_result.error());
      return tl::unexpected(Statistic::missing_cache_file);
    }
    stdout_data = *stdout_data_result;
  }

  auto stderr_data_result = util::read_file<util::Bytes>(tmp_stderr.path);
  if (!stderr_data_result) {
    LOG("Failed to read {} (cleanup in progress?): {}",
        tmp_stderr.path,
        stderr_data_result.error());
    return tl::unexpected(Statistic::missing_cache_file);
  }

  return DoExecuteResult{status, stdout_data, *stderr_data_result};
}

static void
read_manifest(Context& ctx, nonstd::span<const uint8_t> cache_entry_data)
{
  try {
    core::CacheEntry cache_entry(cache_entry_data);
    cache_entry.verify_checksum();
    ctx.manifest.read(cache_entry.payload());
  } catch (const core::Error& e) {
    LOG("Error reading manifest: {}", e.what());
  }
}

static void
update_manifest(Context& ctx,
                const Hash::Digest& manifest_key,
                const Hash::Digest& result_key)
{
  if (ctx.config.read_only() || ctx.config.read_only_direct()) {
    return;
  }

  ASSERT(ctx.config.direct_mode());

  // ctime() may be 0, so we have to check time_of_invocation against
  // MAX(mtime, ctime).
  //
  // ccache only reads mtime/ctime if file_stat_matches sloppiness is enabled,
  // so mtimes/ctimes are stored as a dummy value (-1) if not enabled. This
  // reduces the number of file_info entries for the common case.
  const bool save_timestamp =
    (ctx.config.sloppiness().contains(core::Sloppy::file_stat_matches))
    || ctx.args_info.output_is_precompiled_header;

  const bool added = ctx.manifest.add_result(
    result_key, ctx.included_files, [&](const std::string& path) {
      DirEntry de(path, DirEntry::LogOnError::yes);
      bool cache_time =
        save_timestamp
        && ctx.time_of_invocation > std::max(de.mtime(), de.ctime());
      return core::Manifest::FileStats{
        de.size(),
        de.is_regular_file() && cache_time ? de.mtime() : util::TimePoint(),
        de.is_regular_file() && cache_time ? de.ctime() : util::TimePoint(),
      };
    });
  if (added) {
    LOG("Added result key to manifest {}", util::format_digest(manifest_key));
    core::CacheEntry::Header header(ctx.config, core::CacheEntryType::manifest);
    ctx.storage.put(manifest_key,
                    core::CacheEntryType::manifest,
                    core::CacheEntry::serialize(header, ctx.manifest));
  } else {
    LOG("Did not add result key to manifest {}",
        util::format_digest(manifest_key));
  }
}

struct FindCoverageFileResult
{
  bool found;
  fs::path path;
  bool mangled;
};

static FindCoverageFileResult
find_coverage_file(const Context& ctx)
{
  // GCC 9+ writes coverage data for /dir/to/example.o to #dir#to#example.gcno
  // (in CWD) if -fprofile-dir=DIR is present (regardless of DIR) instead of the
  // traditional /dir/to/example.gcno.

  fs::path mangled_form = core::Result::gcno_file_in_mangled_form(ctx);
  fs::path unmangled_form = core::Result::gcno_file_in_unmangled_form(ctx);
  fs::path found_file;
  if (DirEntry(mangled_form).is_regular_file()) {
    LOG("Found coverage file {}", mangled_form);
    found_file = mangled_form;
  }
  if (DirEntry(unmangled_form).is_regular_file()) {
    LOG("Found coverage file {}", unmangled_form);
    if (!found_file.empty()) {
      LOG_RAW("Found two coverage files, cannot continue");
      return {};
    }
    found_file = unmangled_form;
  }
  if (found_file.empty()) {
    LOG("No coverage file found (tried {} and {}), cannot continue",
        unmangled_form,
        mangled_form);
    return {};
  }
  return {true, found_file, found_file == mangled_form};
}

[[nodiscard]] static bool
write_result(Context& ctx,
             const Hash::Digest& result_key,
             const util::Bytes& stdout_data,
             const util::Bytes& stderr_data)
{
  core::Result::Serializer serializer(ctx.config);

  if (!stderr_data.empty()) {
    serializer.add_data(core::Result::FileType::stderr_output, stderr_data);
  }
  // Write stdout only after stderr (better with MSVC), as ResultRetriever
  // will later print process them in the order they are read.
  if (!stdout_data.empty()) {
    serializer.add_data(core::Result::FileType::stdout_output, stdout_data);
  }
  if (ctx.args_info.expect_output_obj
      && !serializer.add_file(core::Result::FileType::object,
                              ctx.args_info.output_obj)) {
    LOG("Object file {} missing", ctx.args_info.output_obj);
    return false;
  }
  if (ctx.args_info.generating_pch
      && !serializer.add_file(core::Result::FileType::included_pch_file,
                              ctx.args_info.included_pch_file)) {
    LOG("PCH file {} missing", ctx.args_info.included_pch_file);
  }
  if (ctx.args_info.generating_dependencies
      && !serializer.add_file(core::Result::FileType::dependency,
                              ctx.args_info.output_dep)) {
    LOG("Dependency file {} missing", ctx.args_info.output_dep);
    return false;
  }
  if (ctx.args_info.generating_coverage) {
    const auto coverage_file = find_coverage_file(ctx);
    if (!coverage_file.found) {
      LOG_RAW("Coverage file not found");
      return false;
    }
    if (!serializer.add_file(coverage_file.mangled
                               ? core::Result::FileType::coverage_mangled
                               : core::Result::FileType::coverage_unmangled,
                             coverage_file.path)) {
      LOG("Coverage file {} missing", coverage_file.path);
      return false;
    }
  }
  if (ctx.args_info.generating_stackusage
      && !serializer.add_file(core::Result::FileType::stackusage,
                              ctx.args_info.output_su)) {
    LOG("Stack usage file {} missing", ctx.args_info.output_su);
    return false;
  }
  if (ctx.args_info.generating_callgraphinfo
      && !serializer.add_file(core::Result::FileType::callgraph_info,
                              ctx.args_info.output_ci)) {
    LOG("Callgraph info file {} missing", ctx.args_info.output_ci);
    return false;
  }
  if (ctx.args_info.generating_ipa_clones
      && !serializer.add_file(core::Result::FileType::ipa_clones,
                              ctx.args_info.output_ipa)) {
    LOG("IPA clones file {} missing", ctx.args_info.output_ipa);
    return false;
  }
  if (ctx.args_info.generating_diagnostics
      && !serializer.add_file(core::Result::FileType::diagnostic,
                              ctx.args_info.output_dia)) {
    LOG("Diagnostics file {} missing", ctx.args_info.output_dia);
    return false;
  }
  if (ctx.args_info.seen_split_dwarf
      // Only store .dwo file if it was created by the compiler (GCC and Clang
      // behave differently e.g. for "-gsplit-dwarf -g1").
      && DirEntry(ctx.args_info.output_dwo).is_regular_file()
      && !serializer.add_file(core::Result::FileType::dwarf_object,
                              ctx.args_info.output_dwo)) {
    LOG("Split dwarf file {} missing", ctx.args_info.output_dwo);
    return false;
  }
  if (!ctx.args_info.output_al.empty()
      && !serializer.add_file(core::Result::FileType::assembler_listing,
                              ctx.args_info.output_al)) {
    LOG("Assembler listing file {} missing", ctx.args_info.output_al);
    return false;
  }

  core::CacheEntry::Header header(ctx.config, core::CacheEntryType::result);
  const auto cache_entry_data = core::CacheEntry::serialize(header, serializer);

  if (!ctx.config.remote_only()) {
    const auto& raw_files = serializer.get_raw_files();
    if (!raw_files.empty()) {
      ctx.storage.local.put_raw_files(result_key, raw_files);
    }
  }

  ctx.storage.put(result_key, core::CacheEntryType::result, cache_entry_data);

  return true;
}

static util::Bytes
rewrite_stdout_from_compiler(const Context& ctx, util::Bytes&& stdout_data)
{
  using util::Tokenizer;
  using Mode = Tokenizer::Mode;
  using IncludeDelimiter = Tokenizer::IncludeDelimiter;
  if (!stdout_data.empty()) {
    util::Bytes new_stdout_data;
    for (const auto line : Tokenizer(util::to_string_view(stdout_data),
                                     "\n",
                                     Mode::include_empty,
                                     IncludeDelimiter::yes)) {
      if (util::starts_with(line, "__________")) {
        core::send_to_console(ctx, line, STDOUT_FILENO);
      }
      // Ninja uses the lines with 'Note: including file: ' to determine the
      // used headers. Headers within basedir need to be changed into relative
      // paths because otherwise Ninja will use the abs path to original header
      // to check if a file needs to be recompiled.
      else if (ctx.config.compiler_type() == CompilerType::msvc
               && !ctx.config.base_dir().empty()
               && util::starts_with(line, ctx.config.msvc_dep_prefix())) {
        std::string orig_line(line.data(), line.length());
        std::string abs_inc_path =
          util::replace_first(orig_line, ctx.config.msvc_dep_prefix(), "");
        abs_inc_path = util::strip_whitespace(abs_inc_path);
        fs::path rel_inc_path = core::make_relative_path(ctx, abs_inc_path);
        std::string line_with_rel_inc = util::replace_first(
          orig_line, abs_inc_path, util::pstr(rel_inc_path).str());
        new_stdout_data.insert(new_stdout_data.end(),
                               line_with_rel_inc.data(),
                               line_with_rel_inc.size());
      }
      // The MSVC /FC option causes paths in diagnostics messages to become
      // absolute. Those within basedir need to be changed into relative paths.
      else if (ctx.config.compiler_type() == CompilerType::msvc
               && !ctx.config.base_dir().empty()) {
        size_t path_end = core::get_diagnostics_path_length(line);
        if (path_end != 0) {
          std::string_view abs_path = line.substr(0, path_end);
          fs::path rel_path = core::make_relative_path(ctx, abs_path);
          std::string line_with_rel =
            util::replace_all(line, abs_path, util::pstr(rel_path).str());
          new_stdout_data.insert(
            new_stdout_data.end(), line_with_rel.data(), line_with_rel.size());
        }
      } else {
        new_stdout_data.insert(new_stdout_data.end(), line.data(), line.size());
      }
    }
    return new_stdout_data;
  } else {
    return std::move(stdout_data);
  }
}

static std::string
format_epoch_time(const util::TimePoint& tp)
{
  return FMT("{}.{:09}", tp.sec(), tp.nsec_decimal_part());
}

static bool
source_file_is_too_new(const Context& ctx, const fs::path& path)
{
  const bool sloppy_ctime =
    ctx.config.sloppiness().contains(core::Sloppy::include_file_ctime);
  const bool sloppy_mtime =
    ctx.config.sloppiness().contains(core::Sloppy::include_file_mtime);

  if ((sloppy_mtime && sloppy_ctime) || util::is_dev_null_path(path)) {
    return false;
  }

  // It's not enough to check if mtime/ctime >= ctx.time_of_invocation since
  // filesystem timestamps are granular. See the comment for
  // InodeCache::InodeCache for details.
  //
  // A relatively small safety margin is used in this case to make things safe
  // on common filesystems while also not bailing out when creating a source
  // file reasonably close in time before the compilation.
  const util::Duration min_age(0, 100'000'000); // 0.1 s
  util::TimePoint deadline = ctx.time_of_invocation + min_age;

  DirEntry dir_entry(path);

  if (!sloppy_mtime && dir_entry.mtime() >= deadline) {
    LOG(
      "{} was modified near or after invocation (mtime {}, invocation time {})",
      dir_entry.path(),
      format_epoch_time(dir_entry.mtime()),
      format_epoch_time(ctx.time_of_invocation));
    return true;
  }

  // The same logic as above applies to the change time of the file.
  if (!sloppy_ctime && dir_entry.ctime() >= deadline) {
    LOG(
      "{} had status change near or after invocation (ctime {}, invocation"
      " time {})",
      dir_entry.path(),
      format_epoch_time(dir_entry.ctime()),
      format_epoch_time(ctx.time_of_invocation));
    return true;
  }

  return false;
}

// Run the real compiler and put the result in cache. Returns the result key.
static tl::expected<Hash::Digest, Failure>
to_cache(Context& ctx,
         Args& args,
         std::optional<Hash::Digest> result_key,
         Hash* depend_mode_hash)
{
  if (ctx.config.is_compiler_group_msvc()) {
    args.push_back(fmt::format("-Fo{}", ctx.args_info.output_obj));
  } else {
    args.push_back("-o");
    args.push_back(ctx.args_info.output_obj);
  }

  if (ctx.config.hard_link()
      && !util::is_dev_null_path(ctx.args_info.output_obj)) {
    // Workaround for Clang bug where it overwrites an existing object file
    // when it's compiling an assembler file, see
    // <https://bugs.llvm.org/show_bug.cgi?id=39782>.
    util::remove_nfs_safe(ctx.args_info.output_obj);
  }

  if (ctx.args_info.generating_diagnostics) {
    args.push_back("--serialize-diagnostics");
    args.push_back(ctx.args_info.output_dia);
  }

  if (ctx.args_info.seen_double_dash) {
    args.push_back("--");
  }

  if (ctx.config.run_second_cpp()) {
    args.push_back(
      FMT("{}{}", ctx.args_info.input_file_prefix, ctx.args_info.input_file));
  } else {
    args.push_back(ctx.i_tmpfile);
  }

  if (ctx.args_info.seen_split_dwarf) {
    // Remove any pre-existing .dwo file since we want to check if the compiler
    // produced one, intentionally not using x_unlink or tmp_unlink since we're
    // not interested in logging successful deletions or failures due to
    // nonexistent .dwo files.
    if (unlink(util::pstr(ctx.args_info.output_dwo).c_str()) != 0
        && errno != ENOENT && errno != ESTALE) {
      LOG("Failed to unlink {}: {}", ctx.args_info.output_dwo, strerror(errno));
      return tl::unexpected(Statistic::bad_output_file);
    }
  }

  LOG_RAW("Running real compiler");

  tl::expected<DoExecuteResult, Failure> result;
  result = do_execute(ctx, args);
  args.pop_back(3);

  if (!result) {
    return tl::unexpected(result.error());
  }

  // Merge stderr from the preprocessor (if any) and stderr from the real
  // compiler.
  if (!ctx.cpp_stderr_data.empty()) {
    result->stderr_data.insert(result->stderr_data.begin(),
                               ctx.cpp_stderr_data.begin(),
                               ctx.cpp_stderr_data.end());
  }

  result->stdout_data =
    rewrite_stdout_from_compiler(ctx, std::move(result->stdout_data));

  if (result->exit_status != 0) {
    LOG("Compiler gave exit status {}", result->exit_status);

    // We can output stderr immediately instead of rerunning the compiler.
    core::send_to_console(
      ctx, util::to_string_view(result->stderr_data), STDERR_FILENO);
    core::send_to_console(
      ctx,
      util::to_string_view(core::MsvcShowIncludesOutput::strip_includes(
        ctx, std::move(result->stdout_data))),
      STDOUT_FILENO);

    auto failure = Failure(Statistic::compile_failed);
    failure.set_exit_code(result->exit_status);
    return tl::unexpected(failure);
  }

  if (ctx.config.depend_mode()) {
    ASSERT(depend_mode_hash);
    if (ctx.args_info.generating_dependencies) {
      auto key = result_key_from_depfile(ctx, *depend_mode_hash);
      if (!key) {
        return tl::unexpected(key.error());
      }
      result_key = *key;
    } else if (ctx.args_info.generating_includes) {
      auto key = result_key_from_includes(
        ctx, *depend_mode_hash, util::to_string_view(result->stdout_data));
      if (!key) {
        return tl::unexpected(key.error());
      }
      result_key = *key;
    } else {
      ASSERT(false);
    }
    LOG_RAW("Got result key from dependency file");
    LOG("Result key: {}", util::format_digest(*result_key));
  }

  ASSERT(result_key);

  if (source_file_is_too_new(ctx, ctx.args_info.input_file)) {
    return tl::unexpected(Statistic::modified_input_file);
  }
  for (const auto& [path, digest] : ctx.included_files) {
    if (source_file_is_too_new(ctx, path)) {
      return tl::unexpected(Statistic::modified_input_file);
    }
  }

  if (ctx.args_info.generating_dependencies) {
    Depfile::make_paths_relative_in_output_dep(ctx);
  }

  if (!ctx.args_info.expect_output_obj) {
    // Don't probe for object file when we don't expect one since we otherwise
    // will be fooled by an already existing object file.
    LOG_RAW("Compiler not expected to produce an object file");
  } else {
    DirEntry dir_entry(ctx.args_info.output_obj);
    if (!dir_entry.is_regular_file()) {
      LOG_RAW("Compiler didn't produce an object file");
      return tl::unexpected(Statistic::compiler_produced_no_output);
    } else if (dir_entry.size() == 0) {
      LOG_RAW("Compiler produced an empty object file");
      return tl::unexpected(Statistic::compiler_produced_empty_output);
    }
  }

  if (!write_result(
        ctx, *result_key, result->stdout_data, result->stderr_data)) {
    return tl::unexpected(Statistic::compiler_produced_no_output);
  }

  // Everything OK.
  core::send_to_console(
    ctx, util::to_string_view(result->stderr_data), STDERR_FILENO);
  // Send stdout after stderr, it makes the output clearer with MSVC.
  core::send_to_console(
    ctx,
    util::to_string_view(core::MsvcShowIncludesOutput::strip_includes(
      ctx, std::move(result->stdout_data))),
    STDOUT_FILENO);

  return *result_key;
}

// Process a CUDA preprocessed chunk for a specific architecture
static tl::expected<void, Failure>
process_cuda_chunk(Context& ctx,
                   Hash& hash,
                   const std::string& chunk,
                   size_t index)
{
  auto tmp_result = util::TemporaryFile::create(
    FMT("{}/cuda_tmp_{}", ctx.config.temporary_dir(), index),
    FMT(".{}", ctx.config.cpp_extension()));
  if (!tmp_result) {
    return tl::unexpected(Statistic::internal_error);
  }

  const auto& chunk_path = tmp_result->path;
  tmp_result->fd.close(); // we only need the path, not the open fd

  if (!util::write_file(chunk_path, chunk)) {
    return tl::unexpected(Statistic::internal_error);
  }
  ctx.register_pending_tmp_file(chunk_path);
  hash.hash_delimiter(FMT("cu_{}", index));
  TRY(process_preprocessed_file(ctx, hash, chunk_path));

  return {};
}

static bool
get_clang_cu_enable_verbose_mode(const Args& args)
{
  for (size_t i = 1; i < args.size(); i++) {
    if (args[i] == "-v") {
      return true;
    }
  }

  return false;
}

// Find the result key by running the compiler in preprocessor mode and
// hashing the result.
static tl::expected<Hash::Digest, Failure>
get_result_key_from_cpp(Context& ctx, Args& args, Hash& hash)
{
  fs::path preprocessed_path;
  util::Bytes cpp_stderr_data;
  util::Bytes cpp_stdout_data;

  // When Clang runs in verbose mode, it outputs command details to stdout,
  // which can corrupt the output of precompiled CUDA files. Therefore, caching
  // is disabled in this scenario. (Is there a better approach to handle this?)
  const bool is_clang_cu = ctx.config.is_compiler_group_clang()
                           && (ctx.args_info.actual_language == "cu"
                               || ctx.args_info.actual_language == "cuda")
                           && !get_clang_cu_enable_verbose_mode(args);

  const bool capture_stdout = is_clang_cu;

  if (ctx.args_info.direct_i_file) {
    // We are compiling a .i or .ii file - that means we can skip the cpp stage
    // and directly form the correct i_tmpfile.
    preprocessed_path = ctx.args_info.input_file;
  } else {
    // Run cpp on the input file to obtain the .i.

    // preprocessed_path needs the proper cpp_extension for the compiler to do
    // its thing correctly.
    auto tmp_stdout =
      util::value_or_throw<core::Fatal>(util::TemporaryFile::create(
        FMT("{}/cpp_stdout", ctx.config.temporary_dir()),
        FMT(".{}", ctx.config.cpp_extension())));
    preprocessed_path = tmp_stdout.path;
    tmp_stdout.fd.close(); // We're only using the path.
    ctx.register_pending_tmp_file(preprocessed_path);

    const size_t orig_args_size = args.size();

    if (ctx.config.keep_comments_cpp()) {
      args.push_back("-C");
    }

    // Send preprocessor output to a file instead of stdout to work around
    // compilers that don't exit with a proper status on write error to stdout.
    // See also <https://github.com/llvm/llvm-project/issues/56499>.
    if (ctx.config.is_compiler_group_msvc()) {
      args.push_back("-P");
      args.push_back(FMT("-Fi{}", preprocessed_path));
    } else {
      args.push_back("-E");
      if (!is_clang_cu) {
        args.push_back("-o");
        args.push_back(preprocessed_path);
      }
    }

    args.push_back(
      FMT("{}{}", ctx.args_info.input_file_prefix, ctx.args_info.input_file));

    add_prefix(ctx, args, ctx.config.prefix_command_cpp());
    LOG_RAW("Running preprocessor");
    const auto result = do_execute(ctx, args, capture_stdout);
    args.pop_back(args.size() - orig_args_size);

    if (!result) {
      return tl::unexpected(result.error());
    } else if (result->exit_status != 0) {
      LOG("Preprocessor gave exit status {}", result->exit_status);
      return tl::unexpected(Statistic::preprocessor_error);
    }

    cpp_stderr_data = result->stderr_data;
    cpp_stdout_data = result->stdout_data;
  }

  if (is_clang_cu) {
    util::write_file(preprocessed_path, cpp_stdout_data);
    auto chunks =
      util::split_preprocessed_file_from_clang_cuda(preprocessed_path);
    for (size_t i = 0; i < chunks.size(); ++i) {
      TRY(process_cuda_chunk(ctx, hash, chunks[i], i));
    }

  } else {
    hash.hash_delimiter("cpp");

    TRY(process_preprocessed_file(ctx, hash, preprocessed_path));
  }

  hash.hash_delimiter("cppstderr");
  hash.hash(util::to_string_view(cpp_stderr_data));

  ctx.i_tmpfile = preprocessed_path;

  if (!ctx.config.run_second_cpp()) {
    // If we are using the CPP trick, we need to remember this stderr data and
    // output it just before the main stderr from the compiler pass.
    ctx.cpp_stderr_data = std::move(cpp_stderr_data);
    hash.hash_delimiter("runsecondcpp");
    hash.hash("false");
  }

  return hash.digest();
}

// Hash mtime or content of a file, or the output of a command, according to
// the CCACHE_COMPILERCHECK setting.
static tl::expected<void, Failure>
hash_compiler(const Context& ctx,
              Hash& hash,
              const DirEntry& dir_entry,
              const std::string& path,
              bool allow_command)
{
  if (ctx.config.compiler_check() == "none") {
    // Do nothing.
  } else if (ctx.config.compiler_check() == "mtime") {
    hash.hash_delimiter("cc_mtime");
    hash.hash(dir_entry.size());
    hash.hash(dir_entry.mtime().nsec());
  } else if (util::starts_with(ctx.config.compiler_check(), "string:")) {
    hash.hash_delimiter("cc_hash");
    hash.hash(&ctx.config.compiler_check()[7]);
  } else if (ctx.config.compiler_check() == "content" || !allow_command) {
    hash.hash_delimiter("cc_content");
    hash_binary_file(ctx, hash, path);
  } else { // command string
    if (!hash_multicommand_output(
          hash, ctx.config.compiler_check(), ctx.orig_args[0])) {
      LOG("Failure running compiler check command: {}",
          ctx.config.compiler_check());
      return tl::unexpected(Statistic::compiler_check_failed);
    }
  }
  return {};
}

// Hash the host compiler(s) invoked by nvcc.
//
// If `ccbin_st` and `ccbin` are set, they refer to a directory or compiler set
// with -ccbin/--compiler-bindir. If `ccbin_st` is nullptr or `ccbin` is the
// empty string, the compilers are looked up in PATH instead.
static tl::expected<void, Failure>
hash_nvcc_host_compiler(const Context& ctx,
                        Hash& hash,
                        const DirEntry* ccbin_st = nullptr,
                        const std::string& ccbin = {})
{
  // From <http://docs.nvidia.com/cuda/cuda-compiler-driver-nvcc/index.html>:
  //
  //   "[...] Specify the directory in which the compiler executable resides.
  //   The host compiler executable name can be also specified to ensure that
  //   the correct host compiler is selected."
  //
  // and
  //
  //   "On all platforms, the default host compiler executable (gcc and g++ on
  //   Linux, clang and clang++ on Mac OS X, and cl.exe on Windows) found in
  //   the current execution search path will be used".

  if (ccbin.empty() || !ccbin_st || ccbin_st->is_directory()) {
#if defined(__APPLE__)
    const char* compilers[] = {"clang", "clang++"};
#elif defined(_WIN32)
    const char* compilers[] = {"cl.exe"};
#else
    const char* compilers[] = {"gcc", "g++"};
#endif
    for (const char* compiler : compilers) {
      if (!ccbin.empty()) {
        std::string path = FMT("{}/{}", ccbin, compiler);
        DirEntry de(path);
        if (de.is_regular_file()) {
          TRY(hash_compiler(ctx, hash, de, path, false));
        }
      } else {
        std::string path = find_executable(ctx, compiler, ctx.orig_args[0]);
        if (!path.empty()) {
          DirEntry de(path, DirEntry::LogOnError::yes);
          TRY(hash_compiler(ctx, hash, de, ccbin, false));
        }
      }
    }
  } else {
    TRY(hash_compiler(ctx, hash, *ccbin_st, ccbin, false));
  }

  return {};
}

// update a hash with information common for the direct and preprocessor modes.
static tl::expected<void, Failure>
hash_common_info(const Context& ctx, const Args& args, Hash& hash)
{
  hash.hash(HASH_PREFIX);

  if (!ctx.config.namespace_().empty()) {
    hash.hash_delimiter("namespace");
    hash.hash(ctx.config.namespace_());
  }

  // We have to hash the extension, as a .i file isn't treated the same by the
  // compiler as a .ii file.
  hash.hash_delimiter("ext");
  hash.hash(ctx.config.cpp_extension());

#ifdef _WIN32
  const std::string compiler_path = util::add_exe_suffix(args[0]);
#else
  const std::string& compiler_path = args[0];
#endif

  DirEntry dir_entry(compiler_path, DirEntry::LogOnError::yes);
  if (!dir_entry.is_regular_file()) {
    return tl::unexpected(Statistic::could_not_find_compiler);
  }

  // Hash information about the compiler.
  TRY(hash_compiler(ctx, hash, dir_entry, compiler_path, true));

  // Also hash the compiler name as some compilers use hard links and behave
  // differently depending on the real name.
  hash.hash_delimiter("cc_name");
  hash.hash(fs::path(args[0]).filename());

  // Hash variables that may affect the compilation.
  const char* always_hash_env_vars[] = {
    // From <https://gcc.gnu.org/onlinedocs/gcc/Environment-Variables.html>
    // (note: SOURCE_DATE_EPOCH is handled in hash_source_code_string()):
    "COMPILER_PATH",
    "GCC_COMPARE_DEBUG",
    "GCC_EXEC_PREFIX",
    // Variables that affect which underlying compiler ICC uses. Reference:
    // <https://www.intel.com/content/www/us/en/develop/documentation/
    // mpi-developer-reference-windows/top/environment-variable-reference/
    // compilation-environment-variables.html>
    "I_MPI_CC",
    "I_MPI_CXX",
#ifdef __APPLE__
    // On macOS, /usr/bin/clang is a compiler wrapper that switches compiler
    // based on at least these variables:
    "DEVELOPER_DIR",
    "MACOSX_DEPLOYMENT_TARGET",
#endif
  };
  for (const char* name : always_hash_env_vars) {
    const char* value = getenv(name);
    if (value) {
      hash.hash_delimiter(name);
      hash.hash(value);
    }
  }

  if (!(ctx.config.sloppiness().contains(core::Sloppy::locale))) {
    // Hash environment variables that may affect localization of compiler
    // warning messages.
    const char* envvars[] = {
      "LANG", "LC_ALL", "LC_CTYPE", "LC_MESSAGES", nullptr};
    for (const char** p = envvars; *p; ++p) {
      const char* v = getenv(*p);
      if (v) {
        hash.hash_delimiter(*p);
        hash.hash(v);
      }
    }
  }

  // Possibly hash the current working directory.
  if (ctx.args_info.generating_debuginfo && ctx.config.hash_dir()) {
    std::string dir_to_hash = util::pstr(ctx.apparent_cwd);
    if (!ctx.args_info.compilation_dir.empty()) {
      dir_to_hash = ctx.args_info.compilation_dir;
    } else {
      for (const auto& map : ctx.args_info.debug_prefix_maps) {
        size_t sep_pos = map.find('=');
        if (sep_pos != std::string::npos) {
          std::string old_path = map.substr(0, sep_pos);
          std::string new_path = map.substr(sep_pos + 1);
          LOG("Relocating debuginfo from {} to {} (CWD: {})",
              old_path,
              new_path,
              ctx.apparent_cwd);
          if (util::starts_with(util::pstr(ctx.apparent_cwd).str(), old_path)) {
            dir_to_hash =
              new_path
              + util::pstr(ctx.apparent_cwd).str().substr(old_path.size());
          }
        }
      }
    }
    LOG("Hashing CWD {}", dir_to_hash);
    hash.hash_delimiter("cwd");
    hash.hash(dir_to_hash);
  }

  // The object file produced by MSVC includes the full path to the source file
  // even without debug flags. Hashing the directory should be enough since the
  // filename is included in the hash anyway.
  if (ctx.config.is_compiler_group_msvc() && ctx.config.hash_dir()) {
    const fs::path& output_obj_dir = ctx.args_info.output_obj.is_absolute()
                                       ? ctx.args_info.output_obj.parent_path()
                                       : ctx.actual_cwd;
    LOG("Hashing object file directory {}", output_obj_dir);
    hash.hash_delimiter("source path");
    hash.hash(output_obj_dir);
  }

  if (ctx.args_info.seen_split_dwarf) {
    // When using -gsplit-dwarf the object file includes a link to the
    // corresponding .dwo file based on the target object filename, so hashing
    // the object file path will do it, although just hashing the object file
    // base name would be enough.
    LOG_RAW("Hashing object filename due to -gsplit-dwarf");
    hash.hash_delimiter("object file");
    hash.hash(util::pstr(ctx.args_info.output_obj.filename()));
  }

  if (ctx.args_info.profile_arcs) {
    // When using -fprofile-arcs (including implicitly via --coverage) the
    // object file contains a .gcda path based on the absolute object file path.
    // This absolute path can be trimmed using the -fprofile-prefix-path=path
    // option.
    fs::path apparent_absolute_path =
      ctx.apparent_cwd / ctx.args_info.output_obj;
    if (ctx.args_info.profile_prefix_path.empty()) {
      LOG_RAW("Hashing absolute object filename due to -fprofile-arcs");
      hash.hash_delimiter("Absolute object file path");
      // -fprofile-arcs stores "apparent absolute path" for .gcda file
      // names/paths.
      hash.hash(apparent_absolute_path);
    } else {
      if (util::path_starts_with(apparent_absolute_path,
                                 ctx.args_info.profile_prefix_path)) {
        LOG_RAW(
          "Hashing trimmed absolute object filename due to -fprofile-arcs and "
          "-fprofile-prefix-path=path");
        std::string trimmed_absolute_path = apparent_absolute_path.string();
        const std::string profile_prefix_path =
          util::pstr(ctx.args_info.profile_prefix_path).str();
        // Remove ctx.args_info.profile_prefix_path including the last path
        // delimiter from the beginning of apparent_absolute_path
        trimmed_absolute_path.erase(0, profile_prefix_path.size() + 1);
        hash.hash_delimiter("Trimmed absolute object file path");
        hash.hash(trimmed_absolute_path);
      } else {
        LOG_RAW(
          "Hashing absolute object filename due to -fprofile-arcs and "
          "-fprofile-prefix-path=path not being a prefix to the absolute "
          "filename.");
        hash.hash_delimiter("Absolute object file path");
        hash.hash(apparent_absolute_path);
      }
    }
  }

  if (ctx.args_info.generating_coverage
      && !(ctx.config.sloppiness().contains(core::Sloppy::gcno_cwd))) {
    // GCC 9+ includes $PWD in the .gcno file. Since we don't have knowledge
    // about compiler version we always (unless sloppiness is wanted) include
    // the directory in the hash for now.
    LOG_RAW("Hashing apparent CWD due to generating a .gcno file");
    hash.hash_delimiter("CWD in .gcno");
    hash.hash(ctx.apparent_cwd);
  }

  // Possibly hash the coverage data file path.
  if (ctx.args_info.generating_coverage && ctx.args_info.profile_arcs) {
    fs::path dir;
    if (!ctx.args_info.profile_path.empty()) {
      dir = ctx.args_info.profile_path;
    } else {
      const auto output_dir = ctx.args_info.output_obj.parent_path();
      dir = fs::canonical(output_dir).value_or(output_dir);
    }
    fs::path gcda_path = dir / FMT("{}.gcda", ctx.args_info.output_obj.stem());
    LOG("Hashing coverage path {}", gcda_path);
    hash.hash_delimiter("gcda");
    hash.hash(gcda_path);
  }

  // Possibly hash the sanitize blacklist file path.
  for (const auto& sanitize_blacklist : ctx.args_info.sanitize_blacklists) {
    LOG("Hashing sanitize blacklist {}", sanitize_blacklist);
    hash.hash_delimiter("sanitizeblacklist");
    if (!hash_binary_file(ctx, hash, sanitize_blacklist)) {
      return tl::unexpected(Statistic::error_hashing_extra_file);
    }
  }

  if (!(ctx.args_info.build_session_file.empty())) {
    // When using -fbuild-session-file, the actual mtime needs to be
    // added to the hash to prevent false positive cache hits if the
    // mtime of the file changes.
    hash.hash_delimiter("-fbuild-session-file mtime");
    hash.hash(DirEntry(ctx.args_info.build_session_file).mtime().nsec());
  }

  if (!ctx.config.extra_files_to_hash().empty()) {
    for (const auto& path :
         util::split_path_list(ctx.config.extra_files_to_hash())) {
      LOG("Hashing extra file {}", path);
      hash.hash_delimiter("extrafile");
      if (!hash_binary_file(ctx, hash, path)) {
        return tl::unexpected(Statistic::error_hashing_extra_file);
      }
    }
  }

  // Possibly hash GCC_COLORS (for color diagnostics).
  if (ctx.config.compiler_type() == CompilerType::gcc) {
    const char* gcc_colors = getenv("GCC_COLORS");
    if (gcc_colors) {
      hash.hash_delimiter("gcccolors");
      hash.hash(gcc_colors);
    }
  }

  return {};
}

static std::tuple<std::optional<std::string_view>,
                  std::optional<std::string_view>>
get_option_and_value(std::string_view option, const Args& args, size_t& i)
{
  if (args[i] == option) {
    if (i + 1 < args.size()) {
      ++i;
      return {option, args[i]};
    } else {
      return {std::nullopt, std::nullopt};
    }
  } else if (util::starts_with(args[i], option)) {
    return {option, std::string_view(args[i]).substr(option.length())};
  } else {
    return {std::nullopt, std::nullopt};
  }
}

static tl::expected<void, Failure>
hash_argument(const Context& ctx,
              const Args& args,
              size_t& i,
              Hash& hash,
              const bool is_clang,
              const bool direct_mode,
              bool& found_ccbin)
{
  // Trust the user if they've said we should not hash a given option.
  if (option_should_be_ignored(args[i], ctx.ignore_options())) {
    LOG("Not hashing ignored option: {}", args[i]);
    if (i + 1 < args.size() && compopt_takes_arg(args[i])) {
      i++;
      LOG("Not hashing argument of ignored option: {}", args[i]);
    }
    return {};
  }

  // -L doesn't affect compilation (except for clang).
  if (i < args.size() - 1 && args[i] == "-L" && !is_clang) {
    i++;
    return {};
  }
  if (util::starts_with(args[i], "-L") && !is_clang) {
    return {};
  }

  // -Wl,... doesn't affect compilation (except for clang).
  if (util::starts_with(args[i], "-Wl,") && !is_clang) {
    return {};
  }

  if (util::starts_with(args[i], "-Wa,")) {
    // We have to distinguish between three cases:
    //
    // Case 1: -Wa,-a      (write to stdout)
    // Case 2: -Wa,-a=     (write to stdout and stderr)
    // Case 3: -Wa,-a=file (write to file)
    //
    // No need to include the file part in case 3 in the hash since the filename
    // is not part of the output.

    hash.hash_delimiter("arg");
    bool first = true;
    for (const auto part :
         util::Tokenizer(args[i], ",", util::Tokenizer::Mode::include_empty)) {
      if (first) {
        first = false;
      } else {
        hash.hash(",");
      }
      if (util::starts_with(part, "-a")) {
        const auto eq_pos = part.find('=');
        if (eq_pos < part.size() - 1) {
          // Case 3:
          hash.hash(part.substr(0, eq_pos + 1));
          hash.hash("file");
          continue;
        }
      }
      // Case 1 and 2:
      hash.hash(part);
    }
    return {};
  }

  // The -fdebug-prefix-map option may be used in combination with
  // CCACHE_BASEDIR to reuse results across different directories. Skip using
  // the value of the option from hashing but still hash the existence of the
  // option.
  if (util::starts_with(args[i], "-fdebug-prefix-map=")) {
    hash.hash_delimiter("arg");
    hash.hash("-fdebug-prefix-map=");
    return {};
  }
  if (util::starts_with(args[i], "-ffile-prefix-map=")) {
    hash.hash_delimiter("arg");
    hash.hash("-ffile-prefix-map=");
    return {};
  }
  if (util::starts_with(args[i], "-fmacro-prefix-map=")) {
    hash.hash_delimiter("arg");
    hash.hash("-fmacro-prefix-map=");
    return {};
  }
  // -fprofile-prefix-path can also be used to reuse ccache results
  // across different directories
  if (util::starts_with(args[i], "-fprofile-prefix-path=")) {
    hash.hash_delimiter("arg");
    hash.hash("-fprofile-prefix-path=");
    return {};
  }

  if (util::starts_with(args[i], "-frandom-seed=")
      && ctx.config.sloppiness().contains(core::Sloppy::random_seed)) {
    LOG("Ignoring {} since random_seed sloppiness is requested", args[i]);
    return {};
  }

  static const std::string_view frandomize_layout_seed_file =
    "-frandomize-layout-seed-file=";
  if (util::starts_with(args[i], frandomize_layout_seed_file)) {
    hash.hash_delimiter(frandomize_layout_seed_file);
    auto file = args[i].substr(frandomize_layout_seed_file.length());
    if (!hash_binary_file(ctx, hash, file)) {
      LOG("Failed to hash {}", file);
      return tl::unexpected(Statistic::bad_input_file);
    }
    return {};
  }

  // When using the preprocessor, some arguments don't contribute to the hash.
  // The theory is that these arguments will change the output of -E if they are
  // going to have any effect at all. For precompiled headers this might not be
  // the case.
  if (!direct_mode && !ctx.args_info.output_is_precompiled_header
      && !ctx.args_info.using_precompiled_header
      && !ctx.args_info.generating_pch) {
    if (compopt_affects_cpp_output(args[i])) {
      if (compopt_takes_arg(args[i])) {
        i++;
      }
      return {};
    }
    if (compopt_affects_cpp_output(args[i].substr(0, 2))) {
      return {};
    }
  }

  // From the manual: "files that were used by the compiler are recorded, but
  // header files that were *not* used, but would have been used if they
  // existed, are not". To reduce the risk of getting a false positive hit when
  // a directory early in the include path now exists, record whether include
  // directories and similar input paths exist. Note that this is not 100%
  // waterproof since it only detects newly appearing directories and not newly
  // appearing header files.
  if (direct_mode) {
    std::optional<std::string_view> path;
    if (compopt_takes_path(args[i]) && i + 1 < args.size()) {
      path = args[i + 1];
    } else {
      auto p = compopt_prefix_takes_path(args[i]);
      if (p) {
        path = *p;
      }
    }
    if (path) {
      hash.hash_delimiter("path exists");
      hash.hash(FMT("{} {}", *path, fs::exists(*path) ? "1" : "0"));
    }
  }

  if (ctx.args_info.generating_dependencies) {
    std::optional<std::string_view> option;
    std::optional<std::string_view> value;

    if (util::starts_with(args[i], "-Wp,")) {
      // Skip the dependency filename since it doesn't impact the output.
      if (util::starts_with(args[i], "-Wp,-MD,")
          && args[i].find(',', 8) == std::string::npos) {
        hash.hash(args[i].data(), 8);
        return {};
      } else if (util::starts_with(args[i], "-Wp,-MMD,")
                 && args[i].find(',', 9) == std::string::npos) {
        hash.hash(args[i].data(), 9);
        return {};
      }
    } else if (std::tie(option, value) = get_option_and_value("-MF", args, i);
               option) {
      // Skip the dependency filename since it doesn't impact the output.
      hash.hash(*option);
      return {};
    } else if (std::tie(option, value) = get_option_and_value("-MQ", args, i);
               option) {
      hash.hash(*option);
      // No need to hash the dependency target since we always calculate it on
      // a cache hit.
      return {};
    } else if (std::tie(option, value) = get_option_and_value("-MT", args, i);
               option) {
      hash.hash(*option);
      // No need to hash the dependency target since we always calculate it on
      // a cache hit.
      return {};
    }
  }

  if (util::starts_with(args[i], "-specs=")
      || util::starts_with(args[i], "--specs=")
      || (args[i] == "-specs" || args[i] == "--specs")
      || args[i] == "--config") {
    std::string path;
    size_t eq_pos = args[i].find('=');
    if (eq_pos == std::string::npos) {
      if (i + 1 >= args.size()) {
        LOG("missing argument for \"{}\"", args[i]);
        return tl::unexpected(Statistic::bad_compiler_arguments);
      }
      path = args[i + 1];
      i++;
    } else {
      path = args[i].substr(eq_pos + 1);
    }

    if (args[i] == "--config" && path.find('/') == std::string::npos) {
      // --config FILE without / in FILE: the file is searched for in Clang's
      // user/system/executable directories.
      LOG("Argument to compiler option {} is too hard", args[i]);
      return tl::unexpected(Statistic::unsupported_compiler_option);
    }

    DirEntry dir_entry(path, DirEntry::LogOnError::yes);
    if (dir_entry.is_regular_file()) {
      // If given an explicit specs file, then hash that file, but don't
      // include the path to it in the hash.
      hash.hash_delimiter("specs");
      TRY(hash_compiler(ctx, hash, dir_entry, path, false));
      return {};
    } else {
      LOG("While processing {}: {} is missing", args[i], path);
      return tl::unexpected(Statistic::bad_compiler_arguments);
    }
  }

  if (util::starts_with(args[i], "-fplugin=")) {
    DirEntry dir_entry(&args[i][9], DirEntry::LogOnError::yes);
    if (dir_entry.is_regular_file()) {
      hash.hash_delimiter("plugin");
      TRY(hash_compiler(ctx, hash, dir_entry, &args[i][9], false));
      return {};
    }
  }

  if (args[i] == "-Xclang" && i + 3 < args.size() && args[i + 1] == "-load"
      && args[i + 2] == "-Xclang") {
    DirEntry dir_entry(args[i + 3], DirEntry::LogOnError::yes);
    if (dir_entry.is_regular_file()) {
      hash.hash_delimiter("plugin");
      TRY(hash_compiler(ctx, hash, dir_entry, args[i + 3], false));
      i += 3;
      return {};
    }
  }

  if ((args[i] == "-ccbin" || args[i] == "--compiler-bindir")
      && i + 1 < args.size()) {
    DirEntry dir_entry(args[i + 1]);
    if (dir_entry.exists()) {
      found_ccbin = true;
      hash.hash_delimiter("ccbin");
      TRY(hash_nvcc_host_compiler(ctx, hash, &dir_entry, args[i + 1]));
      i++;
      return {};
    }
  }

  // All other arguments are included in the hash.
  hash.hash_delimiter("arg");
  hash.hash(args[i]);
  if (i + 1 < args.size() && compopt_takes_arg(args[i])) {
    i++;
    hash.hash_delimiter("arg");
    hash.hash(args[i]);
  }

  return {};
}

static tl::expected<std::optional<Hash::Digest>, Failure>
get_manifest_key(Context& ctx, Hash& hash)
{
  // Hash environment variables that affect the preprocessor output.
  const char* envvars[] = {
    "CPATH",
    "C_INCLUDE_PATH",
    "CPLUS_INCLUDE_PATH",
    "OBJC_INCLUDE_PATH",
    "OBJCPLUS_INCLUDE_PATH",        // Clang
    "CLANG_CONFIG_FILE_SYSTEM_DIR", // Clang
    "CLANG_CONFIG_FILE_USER_DIR",   // Clang
    nullptr,
  };
  for (const char** p = envvars; *p; ++p) {
    const char* v = getenv(*p);
    if (v) {
      hash.hash_delimiter(*p);
      hash.hash(v);
    }
  }

  // Make sure that the direct mode hash is unique for the input file path. If
  // this would not be the case:
  //
  // * A false cache hit may be produced. Scenario:
  //   - a/r.h exists.
  //   - a/x.c has #include "r.h".
  //   - b/x.c is identical to a/x.c.
  //   - Compiling a/x.c records a/r.h in the manifest.
  //   - Compiling b/x.c results in a false cache hit since a/x.c and b/x.c
  //     share manifests and a/r.h exists.
  // * The expansion of __FILE__ may be incorrect.
  hash.hash_delimiter("inputfile");
  hash.hash(ctx.args_info.input_file);

  if (!ctx.args_info.input_file_prefix.empty()) {
    hash.hash_delimiter("inputfile prefix");
    hash.hash(ctx.args_info.input_file_prefix);
  }

  hash.hash_delimiter("sourcecode hash");
  Hash::Digest input_file_digest;
  auto ret =
    hash_source_code_file(ctx, input_file_digest, ctx.args_info.input_file);
  if (ret.contains(HashSourceCode::error)) {
    return tl::unexpected(Statistic::internal_error);
  }
  if (ret.contains(HashSourceCode::found_time)) {
    LOG_RAW("Disabling direct mode");
    ctx.config.set_direct_mode(false);
    return {};
  }
  hash.hash(util::format_digest(input_file_digest));
  return hash.digest();
}

static bool
hash_profile_data_file(const Context& ctx, Hash& hash)
{
  const fs::path& profile_path = ctx.args_info.profile_path;
  const std::string base_name =
    util::pstr(util::with_extension(ctx.args_info.output_obj, ""));
  std::string hashified_cwd = util::pstr(ctx.apparent_cwd);
  std::replace(hashified_cwd.begin(), hashified_cwd.end(), '/', '#');

  std::vector<fs::path> paths_to_try{
    // -fprofile-use[=dir]/-fbranch-probabilities (GCC <9)
    profile_path / FMT("{}.gcda", base_name),
    // -fprofile-use[=dir]/-fbranch-probabilities (GCC >=9)
    profile_path / FMT("{}#{}.gcda", hashified_cwd, base_name),
    // -fprofile(-instr|-sample)-use=file (Clang), -fauto-profile=file (GCC >=5)
    profile_path,
    // -fprofile(-instr|-sample)-use=dir (Clang)
    profile_path / "default.profdata",
    // -fauto-profile (GCC >=5)
    "fbdata.afdo", // -fprofile-dir is not used
  };

  bool found = false;
  for (const fs::path& p : paths_to_try) {
    LOG("Checking for profile data file {}", p);
    if (DirEntry(p).is_regular_file()) {
      LOG("Adding profile data {} to the hash", p);
      hash.hash_delimiter("-fprofile-use");
      if (hash_binary_file(ctx, hash, p)) {
        found = true;
      }
    }
  }

  return found;
}

static tl::expected<void, Failure>
hash_profiling_related_data(const Context& ctx, Hash& hash)
{
  // For profile generation (-fprofile(-instr)-generate[=path])
  // - hash profile path
  //
  // For profile usage (-fprofile(-instr|-sample)-use, -fbranch-probabilities):
  // - hash profile data
  //
  // -fbranch-probabilities and -fvpt usage is covered by
  // -fprofile-generate/-fprofile-use.
  //
  // The profile directory can be specified as an argument to
  // -fprofile(-instr)-generate=, -fprofile(-instr|-sample)-use= or
  // -fprofile-dir=.

  if (ctx.args_info.profile_generate) {
    ASSERT(!ctx.args_info.profile_path.empty());

    // For a relative profile directory D the compiler stores $PWD/D as part of
    // the profile filename so we need to include the same information in the
    // hash.
    const fs::path profile_path =
      ctx.args_info.profile_path.is_absolute()
        ? ctx.args_info.profile_path
        : ctx.apparent_cwd / ctx.args_info.profile_path;
    LOG("Adding profile directory {} to our hash", profile_path);
    hash.hash_delimiter("-fprofile-dir");
    hash.hash(profile_path);
  }

  if (ctx.args_info.profile_use && !hash_profile_data_file(ctx, hash)) {
    LOG_RAW("No profile data file found");
    return tl::unexpected(Statistic::no_input_file);
  }

  return {};
}

static std::optional<Hash::Digest>
get_result_key_from_manifest(Context& ctx, const Hash::Digest& manifest_key)
{
  std::optional<Hash::Digest> result_key;
  size_t read_manifests = 0;
  ctx.storage.get(
    manifest_key, core::CacheEntryType::manifest, [&](const auto& value) {
      try {
        read_manifest(ctx, value);
        ++read_manifests;
        result_key = ctx.manifest.look_up_result_digest(ctx);
      } catch (const core::Error& e) {
        LOG("Failed to look up result key in manifest: {}", e.what());
      }
      if (result_key) {
        LOG_RAW("Got result key from manifest");
        return true;
      } else {
        LOG_RAW("Did not find result key in manifest");
        return false;
      }
    });
  if (read_manifests > 1 && !ctx.config.remote_only()) {
    LOG("Storing merged manifest {} locally",
        util::format_digest(manifest_key));
    core::CacheEntry::Header header(ctx.config, core::CacheEntryType::manifest);
    ctx.storage.local.put(manifest_key,
                          core::CacheEntryType::manifest,
                          core::CacheEntry::serialize(header, ctx.manifest));
  }

  return result_key;
}

// Update a hash sum with information specific to the direct and preprocessor
// modes and calculate the result key. Returns the result key on success, and
// if direct_mode is true also the manifest key.
static tl::expected<
  std::pair<std::optional<Hash::Digest>, std::optional<Hash::Digest>>,
  Failure>
calculate_result_and_manifest_key(Context& ctx,
                                  const Args& args,
                                  Hash& hash,
                                  Args* preprocessor_args)
{
  bool direct_mode = !preprocessor_args;
  bool found_ccbin = false;

  hash.hash_delimiter("cache entry version");
  hash.hash(core::CacheEntry::k_format_version);

  hash.hash_delimiter("result version");
  hash.hash(core::Result::k_format_version);

  if (direct_mode) {
    hash.hash_delimiter("manifest version");
    hash.hash(core::Manifest::k_format_version);
  }

  // clang will emit warnings for unused linker flags, so we shouldn't skip
  // those arguments.
  bool is_clang = ctx.config.is_compiler_group_clang()
                  || ctx.config.compiler_type() == CompilerType::other;

  // First the arguments.
  for (size_t i = 1; i < args.size(); i++) {
    TRY(hash_argument(ctx, args, i, hash, is_clang, direct_mode, found_ccbin));
  }

  if (!found_ccbin && ctx.args_info.actual_language == "cu") {
    TRY(hash_nvcc_host_compiler(ctx, hash));
  }

  TRY(hash_profiling_related_data(ctx, hash));

  // Adding -arch to hash since cpp output is affected.
  for (const auto& arch : ctx.args_info.arch_args) {
    hash.hash_delimiter("-arch");
    hash.hash(arch);

    // Adding -Xarch_* to hash since cpp output is affected.
    auto it = ctx.args_info.xarch_args.find(arch);
    if (it != ctx.args_info.xarch_args.end()) {
      for (const auto& xarch : it->second) {
        hash.hash_delimiter("-Xarch_" + arch);
        hash.hash(xarch);
      }
    }
  }

  std::optional<Hash::Digest> result_key;
  std::optional<Hash::Digest> manifest_key;

  if (direct_mode) {
    const auto manifest_key_result = get_manifest_key(ctx, hash);
    if (!manifest_key_result) {
      return tl::unexpected(manifest_key_result.error());
    }
    manifest_key = *manifest_key_result;
    if (manifest_key && !ctx.config.recache()) {
      LOG("Manifest key: {}", util::format_digest(*manifest_key));
      result_key = get_result_key_from_manifest(ctx, *manifest_key);
    }
  } else if (ctx.args_info.arch_args.empty()) {
    const auto digest = get_result_key_from_cpp(ctx, *preprocessor_args, hash);
    if (!digest) {
      return tl::unexpected(digest.error());
    }
    result_key = *digest;
    LOG_RAW("Got result key from preprocessor");
  } else {
    preprocessor_args->push_back("-arch");
    for (size_t i = 0; i < ctx.args_info.arch_args.size(); ++i) {
      const auto& arch = ctx.args_info.arch_args[i];
      size_t xarch_count = 0;
      preprocessor_args->push_back(arch);
      auto it = ctx.args_info.xarch_args.find(arch);
      if (it != ctx.args_info.xarch_args.end()) {
        for (const auto& xarch : it->second) {
          preprocessor_args->push_back("-Xarch_" + arch);
          preprocessor_args->push_back(xarch);
          xarch_count += 2;
        }
      }
      const auto digest =
        get_result_key_from_cpp(ctx, *preprocessor_args, hash);
      if (!digest) {
        return tl::unexpected(digest.error());
      }
      result_key = *digest;
      LOG("Got result key from preprocessor with -arch {}", arch);
      if (i != ctx.args_info.arch_args.size() - 1) {
        result_key = std::nullopt;
      }
      preprocessor_args->pop_back(1 + xarch_count);
    }
    preprocessor_args->pop_back();
  }

  if (result_key) {
    LOG("Result key: {}", util::format_digest(*result_key));
  }
  return std::make_pair(result_key, manifest_key);
}

enum class FromCacheCallMode : uint8_t { direct, cpp };

// Try to return the compile result from cache.
static tl::expected<bool, Failure>
from_cache(Context& ctx, FromCacheCallMode mode, const Hash::Digest& result_key)
{
  // The user might be disabling cache hits.
  if (ctx.config.recache()) {
    return false;
  }

  // If we're using Clang, we can't trust a precompiled header object based on
  // running the preprocessor since clang will produce a fatal error when the
  // precompiled header is used and one of the included files has an updated
  // timestamp:
  //
  //     file 'foo.h' has been modified since the precompiled header 'foo.pch'
  //     was built
  if ((ctx.config.is_compiler_group_clang()
       || ctx.config.compiler_type() == CompilerType::other)
      && ctx.args_info.output_is_precompiled_header
      && mode == FromCacheCallMode::cpp) {
    LOG_RAW("Not considering cached precompiled header in preprocessor mode");
    return false;
  }

  // Get result from cache.
  util::Bytes cache_entry_data;
  ctx.storage.get(
    result_key, core::CacheEntryType::result, [&](util::Bytes&& value) {
      cache_entry_data = std::move(value);
      return true;
    });
  if (cache_entry_data.empty()) {
    return false;
  }

  try {
    core::CacheEntry cache_entry(cache_entry_data);
    cache_entry.verify_checksum();
    core::Result::Deserializer deserializer(cache_entry.payload());
    core::ResultRetriever result_retriever(ctx, result_key);
    util::UmaskScope umask_scope(ctx.original_umask);
    deserializer.visit(result_retriever);
  } catch (core::ResultRetriever::WriteError& e) {
    LOG("Write error when retrieving result from {}: {}",
        util::format_digest(result_key),
        e.what());
    return tl::unexpected(Statistic::bad_output_file);
  } catch (core::Error& e) {
    LOG("Failed to get result from {}: {}",
        util::format_digest(result_key),
        e.what());
    return false;
  }

  LOG_RAW("Succeeded getting cached result");
  return true;
}

// Find the real compiler and put it into ctx.orig_args[0]. We just search the
// PATH to find an executable of the same name that isn't ourselves.
void
find_compiler(Context& ctx,
              const FindExecutableFunction& find_executable_function,
              bool masquerading_as_compiler)
{
  // Support user override of the compiler.
  const std::string compiler =
    !ctx.config.compiler().empty()
      ? ctx.config.compiler()
      // In case ccache is masquerading as the compiler, use only base_name so
      // the real compiler can be determined.
      : (masquerading_as_compiler
           ? fs::path(ctx.orig_args[0]).filename().string()
           : ctx.orig_args[0]);

  const std::string resolved_compiler =
    util::is_full_path(compiler)
      ? compiler
      : find_executable_function(ctx, compiler, ctx.orig_args[0]);

  if (resolved_compiler.empty()) {
    throw core::Fatal(FMT("Could not find compiler \"{}\" in PATH", compiler));
  }

  if (is_ccache_executable(resolved_compiler)) {
    throw core::Fatal("Recursive invocation of ccache");
  }

  ctx.orig_args[0] = resolved_compiler;
}

static void
initialize(Context& ctx, const char* const* argv, bool masquerading_as_compiler)
{
  if (util::logging::enabled()) {
    util::logging::BulkLogger logger;

    BULK_LOG(logger,
             "=== CCACHE {} STARTED =========================================",
             CCACHE_VERSION);
    BULK_LOG(logger, "Configuration file: {}", ctx.config.config_path());
    BULK_LOG(
      logger, "System configuration file: {}", ctx.config.system_config_path());

    ctx.config.visit_items([&](const std::string& key,
                               const std::string& value,
                               const std::string& origin) {
      const auto& log_value =
        key == "remote_storage"
          ? ctx.storage.get_remote_storage_config_for_logging()
          : value;
      BULK_LOG(logger, "Config: ({}) {} = {}", origin, key, log_value);
    });

    BULK_LOG(logger, "Command line: {}", util::format_argv_for_logging(argv));
    BULK_LOG(logger, "Hostname: {}", util::get_hostname());
    BULK_LOG(logger, "Working directory: {}", ctx.actual_cwd);
    if (ctx.apparent_cwd != ctx.actual_cwd) {
      BULK_LOG(logger, "Apparent working directory: {}", ctx.apparent_cwd);
    }
  }

  ctx.storage.initialize();

  find_compiler(ctx, &find_executable, masquerading_as_compiler);

  // Guess compiler after logging the config value in order to be able to
  // display "compiler_type = auto" before overwriting the value with the
  // guess.
  if (ctx.config.compiler_type() == CompilerType::auto_guess) {
    ctx.config.set_compiler_type(guess_compiler(ctx.orig_args[0]));
  }
  DEBUG_ASSERT(ctx.config.compiler_type() != CompilerType::auto_guess);

  LOG("Compiler: {}", ctx.orig_args[0]);
  LOG("Compiler type: {}", compiler_type_to_string(ctx.config.compiler_type()));
}

// Make a copy of stderr that will not be cached, so things like distcc can
// send networking errors to it.
static tl::expected<void, Failure>
set_up_uncached_err()
{
  int uncached_fd =
    dup(STDERR_FILENO); // The file descriptor is intentionally leaked.
  if (uncached_fd == -1) {
    LOG("dup(2) failed: {}", strerror(errno));
    return tl::unexpected(Statistic::internal_error);
  }

  util::setenv("UNCACHED_ERR_FD", FMT("{}", uncached_fd));
  return {};
}

static int cache_compilation(int argc, const char* const* argv);

static tl::expected<core::StatisticsCounters, Failure>
do_cache_compilation(Context& ctx);

static void
log_result_to_debug_log(const Context& ctx)
{
  if (ctx.config.log_file().empty() && !ctx.config.debug()) {
    return;
  }

  core::Statistics statistics(ctx.storage.local.get_statistics_updates());
  for (const auto& message : statistics.get_statistics_ids()) {
    LOG("Result: {}", message);
  }
}

static void
log_result_to_stats_log(Context& ctx)
{
  if (ctx.config.stats_log().empty()) {
    return;
  }

  core::Statistics statistics(ctx.storage.local.get_statistics_updates());
  const auto ids = statistics.get_statistics_ids();
  if (ids.empty()) {
    return;
  }

  core::StatsLog(ctx.config.stats_log())
    .log_result(ctx.args_info.input_file, ids);
}

static void
finalize_at_exit(Context& ctx)
{
  try {
    if (ctx.config.disable()) {
      // Just log result, don't update statistics.
      LOG_RAW("Result: disabled");
      return;
    }

    log_result_to_debug_log(ctx);
    log_result_to_stats_log(ctx);

    ctx.storage.finalize();
  } catch (const core::ErrorBase& e) {
    // finalize_at_exit must not throw since it's called by a destructor.
    LOG("Error while finalizing stats: {}", e.what());
  }

  // Dump log buffer last to not lose any logs.
  if (ctx.config.debug() && !ctx.args_info.output_obj.empty()) {
    util::logging::dump_log(prepare_debug_path(ctx.apparent_cwd,
                                               ctx.config.debug_dir(),
                                               ctx.time_of_invocation,
                                               ctx.args_info.output_obj,
                                               "log"));
  }
}

ArgvParts
split_argv(int argc, const char* const* argv)
{
  ArgvParts argv_parts;
  int i = 0;
  while (i < argc && !std::strchr(argv[i], '=')
         && is_ccache_executable(argv[i])) {
    argv_parts.masquerading_as_compiler = false;
    ++i;
  }
  while (i < argc && std::strchr(argv[i], '=')) {
    argv_parts.config_settings.emplace_back(argv[i]);
    ++i;
  }
  argv_parts.compiler_and_args = Args::from_argv(argc - i, argv + i);
  return argv_parts;
}

// The entry point when invoked to cache a compilation.
static int
cache_compilation(int argc, const char* const* argv)
{
  tzset(); // Needed for localtime_r.

  bool fall_back_to_original_compiler = false;
  Args saved_orig_args;
  std::optional<uint32_t> original_umask;
  fs::path saved_temp_dir;

  auto argv_parts = split_argv(argc, argv);
  if (argv_parts.compiler_and_args.empty()) {
    throw core::Fatal("no compiler given, see \"ccache --help\"");
  }

  {
    Context ctx;
    ctx.initialize(std::move(argv_parts.compiler_and_args),
                   argv_parts.config_settings);
    SignalHandler signal_handler(ctx);
    DEFER(finalize_at_exit(ctx));

    initialize(ctx, argv, argv_parts.masquerading_as_compiler);

    const auto result = do_cache_compilation(ctx);
    ctx.storage.local.increment_statistics(result ? *result
                                                  : result.error().counters());
    const auto& counters = ctx.storage.local.get_statistics_updates();

    if (counters.get(Statistic::cache_miss) > 0) {
      if (!ctx.config.remote_only()) {
        ctx.storage.local.increment_statistic(Statistic::local_storage_miss);
      }
      if (ctx.storage.has_remote_storage()) {
        ctx.storage.local.increment_statistic(Statistic::remote_storage_miss);
      }
    } else if ((counters.get(Statistic::direct_cache_hit) > 0
                || counters.get(Statistic::preprocessed_cache_hit) > 0)
               && counters.get(Statistic::remote_storage_hit) > 0
               && !ctx.config.remote_only()) {
      ctx.storage.local.increment_statistic(Statistic::local_storage_miss);
    }

    if (!result) {
      const auto& exit_code = result.error().exit_code();
      if (exit_code) {
        return *exit_code;
      }
      // Else: Fall back to running the real compiler.
      fall_back_to_original_compiler = true;

      original_umask = ctx.original_umask;

      ASSERT(!ctx.orig_args.empty());

      ctx.orig_args.erase_with_prefix("--ccache-");
      add_prefix(ctx, ctx.orig_args, ctx.config.prefix_command());

      LOG_RAW("Failed; falling back to running the real compiler");

      saved_temp_dir = ctx.config.temporary_dir();
      saved_orig_args = std::move(ctx.orig_args);
      auto execv_argv = saved_orig_args.to_argv();
      LOG("Executing {}", util::format_argv_for_logging(execv_argv.data()));
      // Execute the original command below after ctx and finalizer have been
      // destructed.
    }
  }

  if (fall_back_to_original_compiler) {
    if (original_umask) {
      util::set_umask(*original_umask);
    }
    auto execv_argv = saved_orig_args.to_argv();
    execute_noreturn(execv_argv.data(), saved_temp_dir);
    throw core::Fatal(
      FMT("execute_noreturn of {} failed: {}", execv_argv[0], strerror(errno)));
  }

  LOG_RAW("=== CCACHE DONE ===");
  return EXIT_SUCCESS;
}

static tl::expected<core::StatisticsCounters, Failure>
do_cache_compilation(Context& ctx)
{
  if (ctx.config.disable()) {
    LOG_RAW("ccache is disabled");
    return tl::unexpected(Statistic::none);
  }

  if (ctx.actual_cwd.empty()) {
    LOG("Unable to determine current working directory: {}", strerror(errno));
    return tl::unexpected(Statistic::internal_error);
  }

  // Set CCACHE_DISABLE so no process ccache executes from now on will risk
  // calling ccache second time. For instance, if the real compiler is a wrapper
  // script that calls "ccache $compiler ..." we want that inner ccache call to
  // be disabled.
  util::setenv("CCACHE_DISABLE", "1");

  auto process_args_result = process_args(ctx);

  if (!process_args_result) {
    return tl::unexpected(process_args_result.error());
  }

  TRY(set_up_uncached_err());

  // VS_UNICODE_OUTPUT prevents capturing stdout/stderr, as the output is sent
  // directly to Visual Studio.
  if (ctx.config.compiler_type() == CompilerType::msvc) {
    util::unsetenv("VS_UNICODE_OUTPUT");
  }

  for (const auto& name : {"DEPENDENCIES_OUTPUT", "SUNPRO_DEPENDENCIES"}) {
    if (getenv(name)) {
      LOG("Unsupported environment variable: {}", name);
      return Statistic::unsupported_environment_variable;
    }
  }

  if (ctx.config.is_compiler_group_msvc()) {
    for (const auto& name : {"CL", "_CL_"}) {
      if (getenv(name)) {
        LOG("Unsupported environment variable: {}", name);
        return Statistic::unsupported_environment_variable;
      }
    }
  }

  if (!ctx.config.run_second_cpp() && ctx.config.is_compiler_group_msvc()) {
    LOG_RAW("Second preprocessor cannot be disabled");
    ctx.config.set_run_second_cpp(true);
  }

  if (ctx.config.depend_mode()
      && !(ctx.config.run_second_cpp()
           && (ctx.args_info.generating_dependencies
               || ctx.args_info.generating_includes))) {
    LOG_RAW("Disabling depend mode");
    ctx.config.set_depend_mode(false);
  }

  if (ctx.storage.has_remote_storage()) {
    if (ctx.config.file_clone()) {
      LOG_RAW("Disabling file clone mode since remote storage is enabled");
      ctx.config.set_file_clone(false);
    }
    if (ctx.config.hard_link()) {
      LOG_RAW("Disabling hard link mode since remote storage is enabled");
      ctx.config.set_hard_link(false);
    }
  }

  LOG("Source file: {}", ctx.args_info.input_file);
  if (ctx.args_info.generating_dependencies) {
    LOG("Dependency file: {}", ctx.args_info.output_dep);
  }
  if (ctx.args_info.generating_coverage) {
    LOG_RAW("Coverage file is being generated");
  }
  if (ctx.args_info.generating_stackusage) {
    LOG("Stack usage file: {}", ctx.args_info.output_su);
  }
  if (ctx.args_info.generating_callgraphinfo) {
    LOG("Callgraph info file: {}", ctx.args_info.output_ci);
  }
  if (ctx.args_info.generating_diagnostics) {
    LOG("Diagnostics file: {}", ctx.args_info.output_dia);
  }
  if (!ctx.args_info.output_dwo.empty()) {
    LOG("Split dwarf file: {}", ctx.args_info.output_dwo);
  }

  LOG("Object file: {}", ctx.args_info.output_obj);

  if (ctx.config.debug() && ctx.config.debug_level() >= 2) {
    const auto path = prepare_debug_path(ctx.apparent_cwd,
                                         ctx.config.debug_dir(),
                                         ctx.time_of_invocation,
                                         ctx.args_info.orig_output_obj,
                                         "input-text");
    util::FileStream debug_text_file(path, "w");
    if (debug_text_file) {
      ctx.hash_debug_files.push_back(std::move(debug_text_file));
    } else {
      LOG("Failed to open {}: {}", path, strerror(errno));
    }
  }

  FILE* debug_text_file = !ctx.hash_debug_files.empty()
                            ? ctx.hash_debug_files.front().get()
                            : nullptr;

  Hash common_hash;
  init_hash_debug(ctx, common_hash, 'c', "COMMON", debug_text_file);

  if (should_disable_ccache_for_input_file(ctx.args_info.input_file)) {
    LOG("{} found in {}, disabling ccache",
        k_ccache_disable_token,
        ctx.args_info.input_file);
    return tl::unexpected(Statistic::disabled);
  }

  TRY(
    hash_common_info(ctx, process_args_result->preprocessor_args, common_hash));

  if (process_args_result->hash_actual_cwd) {
    common_hash.hash_delimiter("actual_cwd");
    common_hash.hash(ctx.actual_cwd);
  }

  // Try to find the hash using the manifest.
  Hash direct_hash = common_hash;
  init_hash_debug(ctx, direct_hash, 'd', "DIRECT MODE", debug_text_file);

  Args args_to_hash = process_args_result->preprocessor_args;
  args_to_hash.push_back(process_args_result->extra_args_to_hash);

  bool put_result_in_manifest = false;
  std::optional<Hash::Digest> result_key;
  std::optional<Hash::Digest> result_key_from_manifest;
  std::optional<Hash::Digest> manifest_key;

  if (ctx.config.direct_mode()) {
    LOG_RAW("Trying direct lookup");
    const auto result_and_manifest_key = calculate_result_and_manifest_key(
      ctx, args_to_hash, direct_hash, nullptr);
    if (!result_and_manifest_key) {
      return tl::unexpected(result_and_manifest_key.error());
    }
    std::tie(result_key, manifest_key) = *result_and_manifest_key;
    if (result_key) {
      // If we can return from cache at this point then do so.
      const auto from_cache_result =
        from_cache(ctx, FromCacheCallMode::direct, *result_key);
      if (!from_cache_result) {
        return tl::unexpected(from_cache_result.error());
      } else if (*from_cache_result) {
        return Statistic::direct_cache_hit;
      }

      // Wasn't able to return from cache at this point. However, the result
      // was already found in manifest, so don't re-add it later.
      put_result_in_manifest = false;

      result_key_from_manifest = result_key;
    } else {
      // Add result to manifest later.
      put_result_in_manifest = true;
    }

    if (!ctx.config.recache()) {
      ctx.storage.local.increment_statistic(Statistic::direct_cache_miss);
    }
  }

  if (ctx.config.read_only_direct()) {
    LOG_RAW("Read-only direct mode; running real compiler");
    return tl::unexpected(Statistic::cache_miss);
  }

  if (!ctx.config.depend_mode()) {
    // Find the hash using the preprocessed output. Also updates
    // ctx.included_files.
    Hash cpp_hash = common_hash;
    init_hash_debug(ctx, cpp_hash, 'p', "PREPROCESSOR MODE", debug_text_file);

    const auto result_and_manifest_key = calculate_result_and_manifest_key(
      ctx, args_to_hash, cpp_hash, &process_args_result->preprocessor_args);
    if (!result_and_manifest_key) {
      return tl::unexpected(result_and_manifest_key.error());
    }
    result_key = result_and_manifest_key->first;

    // calculate_result_and_manifest_key always returns a non-nullopt result_key
    // in preprocessor mode (non-nullptr last argument).
    ASSERT(result_key);

    if (result_key_from_manifest && result_key_from_manifest != result_key) {
      // The hash from manifest differs from the hash of the preprocessor
      // output. This could be because:
      //
      // - The preprocessor produces different output for the same input (not
      //   likely).
      // - There's a bug in ccache (maybe incorrect handling of compiler
      //   arguments).
      // - The user has used a different CCACHE_BASEDIR (most likely).
      //
      // The best thing here would probably be to remove the hash entry from
      // the manifest. For now, we use a simpler method: just remove the
      // manifest file.
      LOG_RAW("Hash from manifest doesn't match preprocessor output");
      LOG_RAW("Likely reason: different CCACHE_BASEDIRs used");
      LOG_RAW("Removing manifest as a safety measure");
      ctx.storage.remove(*result_key, core::CacheEntryType::result);

      put_result_in_manifest = true;
    }

    // If we can return from cache at this point then do.
    const auto from_cache_result =
      from_cache(ctx, FromCacheCallMode::cpp, *result_key);
    if (!from_cache_result) {
      return tl::unexpected(from_cache_result.error());
    } else if (*from_cache_result) {
      if (ctx.config.direct_mode() && manifest_key && put_result_in_manifest) {
        update_manifest(ctx, *manifest_key, *result_key);
      }
      return Statistic::preprocessed_cache_hit;
    }

    if (!ctx.config.recache()) {
      ctx.storage.local.increment_statistic(Statistic::preprocessed_cache_miss);
    }
  }

  if (ctx.config.read_only()) {
    LOG_RAW("Read-only mode; running real compiler");
    return tl::unexpected(Statistic::cache_miss);
  }

  add_prefix(
    ctx, process_args_result->compiler_args, ctx.config.prefix_command());

  // In depend_mode, extend the direct hash.
  Hash* depend_mode_hash = ctx.config.depend_mode() ? &direct_hash : nullptr;

  // Run real compiler, sending output to cache.
  const auto digest = to_cache(
    ctx, process_args_result->compiler_args, result_key, depend_mode_hash);
  if (!digest) {
    return tl::unexpected(digest.error());
  }
  result_key = *digest;
  if (ctx.config.direct_mode()) {
    ASSERT(manifest_key);
    update_manifest(ctx, *manifest_key, *result_key);
  }

  return ctx.config.recache() ? Statistic::recache : Statistic::cache_miss;
}

bool
is_ccache_executable(const fs::path& path)
{
  std::string name = path.filename().string();
#ifdef _WIN32
  name = util::to_lowercase(name);
#endif
  return util::starts_with(name, "ccache");
}

bool
file_path_matches_dir_prefix_or_file(const fs::path& dir_prefix_or_file,
                                     const fs::path& file_path)
{
  DEBUG_ASSERT(!dir_prefix_or_file.empty());
  DEBUG_ASSERT(!file_path.filename().empty());

  auto end = std::mismatch(dir_prefix_or_file.begin(),
                           dir_prefix_or_file.end(),
                           file_path.begin(),
                           file_path.end())
               .first;
  return end == dir_prefix_or_file.end() || end->empty();
}

int
ccache_main(int argc, const char* const* argv)
{
  try {
    if (is_ccache_executable(argv[0])) {
      if (argc < 2) {
        PRINT_RAW(
          stderr,
          core::get_usage_text(util::pstr(fs::path(argv[0]).filename()).str()));
        exit(EXIT_FAILURE);
      }
      // If the first argument isn't an option, then assume we are being
      // passed a compiler name and options.
      if (argv[1][0] == '-') {
        return core::process_main_options(argc, argv);
      }
    }

    return cache_compilation(argc, argv);
  } catch (const core::ErrorBase& e) {
    PRINT(stderr, "ccache: error: {}\n", e.what());
    return EXIT_FAILURE;
  }
}
