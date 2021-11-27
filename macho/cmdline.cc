#include "mold.h"
#include "../cmdline.h"

#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <regex>
#include <unistd.h>
#include <unordered_set>

namespace mold::macho {

static const char helpmsg[] = R"(
Options:
  -F<PATH>                    Add DIR to framework search path
  -L<PATH>                    Add DIR to library search path
  -ObjC                       Load all static archive members that implement
                              an Objective-C class or category
  -adhoc_codesign             Add ad-hoc code signature to the output file
    -no_adhoc_codesign
  -arch <ARCH_NAME>           Specify target architecture
  -dead_strip                 Remove unreachable functions and data
  -demangle                   Demangle C++ symbols in log messages (default)
  -dynamic                    Link against dylibs (default)
  -e <SYMBOL>                 Specify the entry point of a main executable
  -filelist <FILE>[,<DIR>]    Specify the list of input file names
  -framework <NAME>,[,<SUFFIX>]
                              Search for a given framework
  -headerpad <SIZE>           Allocate the size of padding after load commands
  -help                       Report usage information
  -l<LIB>                     Search for a given library
  -lto_library <FILE>         Ignored
  -map <FILE>                 Write map file to a given file
  -no_deduplicate             Ignored
  -o <FILE>                   Set output filename
  -pagezero_size <SIZE>       Specify the size of the __PAGEZERO segment
  -platform_version <PLATFORM> <MIN_VERSION> <SDK_VERSION>
                              Set platform, platform version and SDK version
  -rpath <PATH>               Add PATH to the runpath search path list
  -syslibroot <DIR>           Prepend DIR to library search paths
  -t                          Print out each file the linker loads
  -v                          Report version information)";

static i64 parse_platform(Context &ctx, std::string_view arg) {
  static std::regex re(R"(\d+)", std::regex_constants::ECMAScript);
  if (std::regex_match(arg.begin(), arg.end(), re))
    return stoi(std::string(arg));

  if (arg == "macos")
    return PLATFORM_MACOS;
  if (arg == "ios")
    return PLATFORM_IOS;
  if (arg == "tvos")
    return PLATFORM_TVOS;
  if (arg == "watchos")
    return PLATFORM_WATCHOS;
  if (arg == "bridgeos")
    return PLATFORM_BRIDGEOS;
  if (arg == "mac-catalyst")
    return PLATFORM_MACCATALYST;
  if (arg == "ios-simulator")
    return PLATFORM_IOSSIMULATOR;
  if (arg == "tvos-simulator")
    return PLATFORM_TVOSSIMULATOR;
  if (arg == "watchos-simulator")
    return PLATFORM_WATCHOSSIMULATOR;
  if (arg == "driverkit")
    return PLATFORM_DRIVERKIT;
  Fatal(ctx) << "unknown -platform_version name: " << arg;
}

static i64 parse_version(Context &ctx, std::string_view arg) {
  static std::regex re(R"((\d+)(?:\.(\d+))?(?:\.(\d+))?)",
                       std::regex_constants::ECMAScript);
  std::cmatch m;
  if (!std::regex_match(arg.begin(), arg.end(), m, re))
    Fatal(ctx) << "malformed version number: " << arg;

  i64 major = (m[1].length() == 0) ? 0 : stoi(m[1]);
  i64 minor = (m[2].length() == 0) ? 0 : stoi(m[2]);
  i64 patch = (m[3].length() == 0) ? 0 : stoi(m[3]);
  return (major << 16) | (minor << 8) | patch;
}

void parse_nonpositional_args(Context &ctx,
                              std::vector<std::string> &remaining) {
  std::vector<std::string_view> &args = ctx.cmdline_args;
  i64 i = 1;

  std::vector<std::string> framework_paths;
  std::vector<std::string> library_paths;

  while (i < args.size()) {
    std::string_view arg;
    std::string_view arg2;
    std::string_view arg3;

    auto read_arg = [&](std::string name) {
      if (args[i] == name) {
        if (args.size() <= i + 1)
          Fatal(ctx) << "option -" << name << ": argument missing";
        arg = args[i + 1];
        i += 2;
        return true;
      }
      return false;
    };

    auto read_arg3 = [&](std::string name) {
      if (args[i] == name) {
        if (args.size() <= i + 3)
          Fatal(ctx) << "option -" << name << ": argument missing";
        arg = args[i + 1];
        arg2 = args[i + 2];
        arg3 = args[i + 3];
        i += 4;
        return true;
      }
      return false;
    };

    auto read_joined = [&](std::string name) {
      if (read_arg(name))
        return true;
      if (args[i].starts_with(name)) {
        arg = args[i].substr(2);
        i++;
        return true;
      }
      return false;
    };

    auto read_flag = [&](std::string name) {
      if (args[i] == name) {
        i++;
        return true;
      }
      return false;
    };

    if (args[i].starts_with('@')) {
      std::vector<std::string_view> vec =
        read_response_file(ctx, args[i].substr(1));
      args.erase(args.begin() + i);
      args.insert(args.begin() + i, vec.begin(), vec.end());
      continue;
    }

    if (read_flag("-help") || read_flag("--help")) {
      SyncOut(ctx) << "Usage: " << ctx.cmdline_args[i]
                   << " [options] file...\n" << helpmsg;
      exit(0);
    }

    if (read_joined("-F")) {
      framework_paths.push_back(std::string(arg));
    } else if (read_joined("-L")) {
      library_paths.push_back(std::string(arg));
    } else if (read_flag("-ObjC")) {
      ctx.arg.ObjC = true;
    } else if (read_flag("-adhoc_codesign")) {
      ctx.arg.adhoc_codesign = true;
    } else if (read_flag("-no_adhoc_codesign")) {
      ctx.arg.adhoc_codesign = false;
    } else if (read_arg("-arch")) {
      if (arg != "x86_64")
        Fatal(ctx) << "unknown -arch: " << arg;
    } else if (read_flag("-color-diagnostics") ||
               read_flag("--color-diagnostics")) {
    } else if (read_flag("-dead_strip")) {
      ctx.arg.dead_strip = true;
    } else if (read_flag("-demangle")) {
      ctx.arg.demangle = true;
    } else if (read_arg("-headerpad")) {
      size_t pos;
      ctx.arg.headerpad = std::stoi(std::string(arg), &pos, 16);
      if (pos != arg.size())
        Fatal(ctx) << "malformed -headerpad: " << arg;
    } else if (read_flag("-dynamic")) {
      ctx.arg.dynamic = true;
    } else if (read_arg("-e")) {
      ctx.arg.entry = arg;
    } else if (read_arg("-fatal_warnings")) {
    } else if (read_arg("-filelist")) {
      remaining.push_back("-filelist");
      remaining.push_back(std::string(arg));
    } else if (read_arg("-framework")) {
      remaining.push_back("-framework");
      remaining.push_back(std::string(arg));
    } else if (read_arg("-lto_library")) {
    } else if (read_joined("-l")) {
      remaining.push_back("-l" + std::string(arg));
    } else if (read_arg("-map")) {
      ctx.arg.map = arg;
    } else if (read_flag("-no_deduplicate")) {
    } else if (read_arg("-o")) {
      ctx.arg.output = arg;
    } else if (read_arg("-pagezero_size")) {
      size_t pos;
      ctx.arg.pagezero_size = std::stoi(std::string(arg), &pos, 16);
      if (pos != arg.size())
        Fatal(ctx) << "malformed -pagezero_size: " << arg;
    } else if (read_arg3("-platform_version")) {
      ctx.arg.platform = parse_platform(ctx, arg);
      ctx.arg.platform_min_version = parse_version(ctx, arg2);
      ctx.arg.platform_sdk_version = parse_version(ctx, arg3);
    } else if (read_arg("-rpath")) {
      ctx.arg.rpath.push_back(std::string(arg));
    } else if (read_arg("-syslibroot")) {
      ctx.arg.syslibroot.push_back(std::string(arg));
    } else if (read_flag("-t")) {
      ctx.arg.trace = true;
    } else if (read_flag("-v")) {
      SyncOut(ctx) << mold_version;
    } else {
      if (args[i][0] == '-')
        Fatal(ctx) << "unknown command line option: " << args[i];
      remaining.push_back(std::string(args[i]));
      i++;
    }
  }

  if (ctx.arg.output.empty())
    ctx.arg.output = "a.out";

  auto add_library_path = [&](std::string path) {
    if (!path.starts_with('/') || ctx.arg.syslibroot.empty()) {
      ctx.arg.library_paths.push_back(path);
    } else {
      for (std::string &dir : ctx.arg.syslibroot)
        ctx.arg.library_paths.push_back(path_clean(dir + "/" + path));
    }
  };

  for (std::string &path : library_paths)
    add_library_path(path);
  add_library_path("/usr/lib");
  add_library_path("/usr/local/lib");

  auto add_framework_path = [&](std::string path) {
    if (!path.starts_with('/') || ctx.arg.syslibroot.empty()) {
      ctx.arg.framework_paths.push_back(path);
    } else {
      for (std::string &dir : ctx.arg.syslibroot)
        ctx.arg.framework_paths.push_back(path_clean(dir + "/" + path));
    }
  };

  for (std::string &path : framework_paths)
    add_framework_path(path);
  add_framework_path("/Library/Frameworks");
  add_framework_path("/System/Library/Frameworks");
}

} // namespace mold::macho
