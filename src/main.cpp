// src/main.cpp — cstar CLI driver: run / build / check (also reachable as the
// legacy `pentest` alias until v0.4).
//
// Implements the CLI contract in docs/contracts/m1-semantics.md §9 and drives the
// compile pipeline of §10:
//   read source -> parse -> semantic -> generate C -> write embedded runtime
//   sources into a temp dir -> clang -> (cache +) exec.
// The runtime TU is precompiled at `make` time and embedded, so the first
// `run`/`build` on a machine links instead of spending ~6 s in clang.
// Only the C++17 standard library plus POSIX (Linux x86_64 is the M1 target).

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "codegen.h"
#include "diagnostics.h"
#include "frontend.h"
#include "modules.h"
#include "runtime_embed.h"

namespace {

constexpr const char kVersion[] = "0.2.1";

enum class Mode { Run, Build, Check };

void print_usage(std::FILE* out) {
  std::fputs(
      "usage: cstar <file.cst> [args...]\n"
      "       cstar run [<file.cst> [args...]]\n"
      "       cstar build [<file.cst>] [-o out]\n"
      "       cstar check [<file.cst>]\n"
      "       cstar --help\n"
      "       cstar --version\n"
      "source files use the .cst extension (legacy .pen/.pent accepted until v0.4);\n"
      "run, build and check without a file use ./index.cst\n",
      out);
}

int usage_error(const std::string& message) {
  std::fprintf(stderr, "error: %s\n", message.c_str());
  print_usage(stderr);
  return 2;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------

bool read_file(const std::string& path, std::string& out) {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  struct stat st;
  if (fstat(fd, &st) != 0) {
    int e = errno;
    close(fd);
    errno = e;
    return false;
  }
  if (!S_ISREG(st.st_mode)) {
    close(fd);
    errno = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
    return false;
  }
  char buf[65536];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    out.append(buf, static_cast<std::size_t>(n));
  }
  int e = (n < 0) ? errno : 0;
  close(fd);
  if (n < 0) {
    errno = e;
    return false;
  }
  return true;
}

bool write_file(const std::string& path, const void* data, std::size_t len) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  const char* p = static_cast<const char*>(data);
  std::size_t left = len;
  while (left > 0) {
    ssize_t n = write(fd, p, left);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      int e = errno;
      close(fd);
      errno = e;
      return false;
    }
    p += n;
    left -= static_cast<std::size_t>(n);
  }
  if (close(fd) != 0) return false;
  return true;
}

// mkdir -p for `path` (mode 0700 for created components). True if it exists
// as a directory when done.
bool ensure_dir(const std::string& path) {
  if (path.empty()) return false;
  std::string acc;
  std::size_t i = 0;
  if (path[0] == '/') {
    acc = "/";
    i = 1;
  }
  while (i < path.size()) {
    std::size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    std::string comp = path.substr(i, j - i);
    if (!comp.empty()) {
      if (!acc.empty() && acc.back() != '/') acc += '/';
      acc += comp;
      if (mkdir(acc.c_str(), 0700) != 0 && errno != EEXIST) return false;
      struct stat st;
      if (stat(acc.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    }
    i = j + 1;
  }
  return true;
}

// A fresh mkdtemp directory under $TMPDIR (default /tmp). Empty on failure.
std::string make_temp_dir() {
  const char* t = std::getenv("TMPDIR");
  std::string base =
      (t != nullptr && t[0] != '\0') ? std::string(t) : std::string("/tmp");
  if (!ensure_dir(base)) return std::string();
  std::string tmpl = base + "/cstar-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  if (mkdtemp(buf.data()) == nullptr) return std::string();
  return std::string(buf.data());
}

// Removes the generated sources from `dir` and best-effort rmdirs it. The
// rmdir fails harmlessly when the compiled executable lives inside `dir`.
void remove_sources(const std::string& dir) {
  ::unlink((dir + "/gen.c").c_str());
  for (unsigned int i = 0; i < pen_embedded_files_count; ++i) {
    ::unlink((dir + "/" + pen_embedded_files[i].name).c_str());
  }
}

// ---------------------------------------------------------------------------
// Cache (run mode): $XDG_CACHE_HOME/cstar (fallback ~/.cache/cstar)
// ---------------------------------------------------------------------------

std::string cache_dir() {
  const char* xdg = std::getenv("XDG_CACHE_HOME");
  std::string base;
  if (xdg != nullptr && xdg[0] != '\0') {
    base = xdg;
  } else {
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      base = std::string(home) + "/.cache";
    } else {
      struct passwd* pw = getpwuid(getuid());
      if (pw == nullptr || pw->pw_dir == nullptr || pw->pw_dir[0] == '\0') {
        return std::string();
      }
      base = std::string(pw->pw_dir) + "/.cache";
    }
  }
  return base + "/cstar";
}

std::string shell_single_quote(const std::string& s) {
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') {
      r += "'\\''";
    } else {
      r += c;
    }
  }
  r += "'";
  return r;
}

bool write_all(std::FILE* f, const char* data, std::size_t len) {
  std::size_t off = 0;
  while (off < len) {
    std::size_t n = std::fwrite(data + off, 1, len - off, f);
    if (n == 0) return false;
    off += n;
  }
  return true;
}

// sha256(source bytes + version string), computed by piping the data to
// `sha256sum` through popen. Returns "" (no caching) if sha256sum is
// unavailable or anything else fails — silently.
std::string compute_cache_key(const std::string& source) {
  const char* t = std::getenv("TMPDIR");
  std::string td =
      (t != nullptr && t[0] != '\0') ? std::string(t) : std::string("/tmp");
  if (!ensure_dir(td)) return std::string();

  std::string tmpl = td + "/cstar-hash-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  int fd = mkstemp(buf.data());
  if (fd < 0) return std::string();
  close(fd);
  std::string digest_path(buf.data());

  std::string cmd =
      "sha256sum > " + shell_single_quote(digest_path) + " 2>/dev/null";
  std::FILE* p = popen(cmd.c_str(), "w");
  if (p == nullptr) {
    ::unlink(digest_path.c_str());
    return std::string();
  }
  // Toolchain identity: the run cache must not outlive the binary that
  // produced it. A rebuilt/updated `cstar` has a new size/mtime, so
  // compiler or codegen changes re-key every entry — a stale executable
  // can never be served after a `make` (content-addressed keys cannot see
  // .cpp changes). stat(/proc/self/exe) is microseconds; an unchanged
  // binary yields an unchanged key, so hits stay hits.
  char idbuf[72];
  int idn = 0;
  struct stat idst;
  if (::stat("/proc/self/exe", &idst) == 0) {
    idn = std::snprintf(idbuf, sizeof idbuf, "|tc:%lld:%lld:%lld",
                        (long long)idst.st_size,
                        (long long)idst.st_mtim.tv_sec,
                        (long long)idst.st_mtim.tv_nsec);
    if (idn < 0) idn = 0;
  }
  // A `sha256sum` that cannot start (missing binary — minimal containers)
  // closes the pipe on us; the next write would raise SIGPIPE and kill the
  // driver mid-compile, leaving scratch and the empty digest file behind
  // (exit 141). Ignore SIGPIPE across the write+pclose window instead: the
  // write then fails with EPIPE, `ok`/exit-status fall through to the
  // "no caching" path below — SPEC §10: "If `sha256sum` is unavailable,
  // caching is disabled and every `run` compiles".
  struct sigaction ign, prev_pipe;
  ::sigemptyset(&ign.sa_mask);
  ign.sa_handler = SIG_IGN;
  ign.sa_flags = 0;
  ::sigaction(SIGPIPE, &ign, &prev_pipe);
  bool ok = write_all(p, source.data(), source.size()) &&
            write_all(p, kVersion, std::strlen(kVersion)) &&
            (idn <= 0 || write_all(p, idbuf, (size_t)idn));
  int rc = pclose(p);
  ::sigaction(SIGPIPE, &prev_pipe, nullptr);
  if (!ok || rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
    ::unlink(digest_path.c_str());
    return std::string();
  }
  std::string digest;
  if (!read_file(digest_path, digest)) {
    ::unlink(digest_path.c_str());
    return std::string();
  }
  ::unlink(digest_path.c_str());
  if (digest.size() < 64) return std::string();
  for (int i = 0; i < 64; ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(digest[i]))) {
      return std::string();
    }
  }
  return digest.substr(0, 64);
}

// ---------------------------------------------------------------------------
// Process helpers
// ---------------------------------------------------------------------------

// Runs argv via fork+execvp, inheriting stdio. Returns the waitpid status,
// or -1 if the process could not be started/executed (*err = errno).
int run_command(char* const argv[], int* err) {
  *err = 0;
  int pfd[2];
  if (pipe(pfd) != 0) {
    *err = errno;
    return -1;
  }
  if (fcntl(pfd[1], F_SETFD, FD_CLOEXEC) == -1) {
    *err = errno;
    close(pfd[0]);
    close(pfd[1]);
    return -1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    *err = errno;
    close(pfd[0]);
    close(pfd[1]);
    return -1;
  }
  if (pid == 0) {
    close(pfd[0]);
    execvp(argv[0], argv);
    int e = errno;
    ssize_t nw = write(pfd[1], &e, sizeof(e));
    (void)nw;
    _exit(127);
  }
  close(pfd[1]);
  int e = 0;
  ssize_t nr = read(pfd[0], &e, sizeof(e));
  close(pfd[0]);
  int status = 0;
  for (;;) {
    pid_t w = waitpid(pid, &status, 0);
    if (w == pid) break;
    if (w < 0 && errno == EINTR) continue;
    *err = errno;
    return -1;
  }
  if (nr == static_cast<ssize_t>(sizeof(e))) {
    *err = e;  // execvp failed in the child
    return -1;
  }
  return status;
}

// Replaces the current process with `bin` (argv[0] = binary path, `extra`
// passed through as argv[1..]). Returns false only if execv failed.
bool try_exec(const std::string& bin, const std::vector<std::string>& extra) {
  std::vector<char*> argv;
  const char* bin_c = bin.c_str();
  argv.push_back(const_cast<char*>(bin_c));
  for (const std::string& s : extra) {
    argv.push_back(const_cast<char*>(s.c_str()));
  }
  argv.push_back(nullptr);
  execv(bin_c, argv.data());
  return false;
}

// ---------------------------------------------------------------------------
// Embedded prebuilt runtime object (cold-start fast path)
// ---------------------------------------------------------------------------

// Embedded registry lookup by file name; nullptr when absent (len = 0).
const unsigned char* find_embedded(const char* name, unsigned int& len) {
  for (unsigned int i = 0; i < pen_embedded_files_count; ++i) {
    if (std::strcmp(pen_embedded_files[i].name, name) == 0) {
      len = pen_embedded_files[i].len;
      return pen_embedded_files[i].data;
    }
  }
  len = 0;
  return nullptr;
}

// First `flags` line of /proc/cpuinfo ("" when unavailable).
std::string current_cpu_flags() {
  std::FILE* f = std::fopen("/proc/cpuinfo", "r");
  if (f == nullptr) return std::string();
  char buf[16384];
  std::string out;
  while (std::fgets(buf, sizeof buf, f) != nullptr) {
    if (std::strncmp(buf, "flags", 5) == 0 &&
        (buf[5] == ':' || buf[5] == ' ' || buf[5] == '\t')) {
      out.assign(buf);
      while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
      }
      break;
    }
  }
  std::fclose(f);
  return out;
}

// True when the embedded runtime object was built for this machine's ISA.
// `make` embeds the build host's /proc/cpuinfo flags line next to the object;
// a copied binary that meets a different CPU must not execute its machine
// code, so any mismatch disables the fast path.
bool embedded_runtime_usable(const unsigned char* sig, unsigned int sig_len) {
  if (sig == nullptr || sig_len == 0) return false;
  std::string want(reinterpret_cast<const char*>(sig), sig_len);
  while (!want.empty() && (want.back() == '\n' || want.back() == '\r')) {
    want.pop_back();  // the embedded file ends with the awk newline
  }
  if (want.empty()) return false;
  return current_cpu_flags() == want;
}

// ---------------------------------------------------------------------------
// Compile pipeline
// ---------------------------------------------------------------------------

// Generates C for `program`, writes the embedded runtime sources next to it
// in a fresh temp dir, and runs:
//   clang -std=c11 -O3 -march=native -o <out> gen.c cstar_rt.c -lm
// (`-march=native` lets the fvec/ivec SIMD kernels below pick the widest
// vector extension the host supports; without it the generated C still
// compiles — the kernels fall back to the baseline width.)
// `in_out_path` must be non-empty for `build`; for `run` it may be empty, in
// which case the executable is placed inside the temp dir and the path is
// returned through it. Returns 0 on success, 1 on failure (message on stderr).
int compile_program(const cstar::Program& program, std::string& in_out_path) {
  std::string dir = make_temp_dir();
  if (dir.empty()) {
    std::fprintf(stderr, "error: cannot create temporary directory: %s\n",
                 std::strerror(errno));
    return 1;
  }
  std::string gen = dir + "/gen.c";
  if (!cstar::generate_c(program, gen)) {
    std::fprintf(stderr, "error: failed to generate C code for '%s'\n",
                 program.file_path.c_str());
    remove_sources(dir);
    ::rmdir(dir.c_str());
    return 1;
  }
  for (unsigned int i = 0; i < pen_embedded_files_count; ++i) {
    // Only runtime sources are written out; the registry also carries the
    // prebuilt object and the CPU fingerprint, which are not text files.
    const std::string name = pen_embedded_files[i].name;
    if (!ends_with(name, ".c") && !ends_with(name, ".h")) continue;
    std::string path = dir + "/" + name;
    if (!write_file(path, pen_embedded_files[i].data,
                    pen_embedded_files[i].len)) {
      std::fprintf(stderr, "error: cannot write runtime file '%s': %s\n",
                   path.c_str(), std::strerror(errno));
      remove_sources(dir);
      ::rmdir(dir.c_str());
      return 1;
    }
  }
  if (in_out_path.empty()) in_out_path = dir + "/program";

  // Absolute paths keep the caller's cwd irrelevant; quoted includes in
  // gen.c resolve against the temp dir, and -I makes it explicit.
  std::string inc = "-I" + dir;
  std::string rt_c = dir + "/cstar_rt.c";

  // ---- runtime object: prebuilt (embedded) or compiled into the cache ----
  // The runtime TU is identical for every program and dominates the cold
  // compile (~6 s of clang -O3 vs a fraction of that for gen.c). Two fast
  // paths avoid it:
  //   1. `make` precompiled the object with the exact flags below and
  //      embedded it next to the runtime sources (plus the build host's ISA
  //      fingerprint). A cold cache installs that object with no clang run.
  //      It is only used when the host's /proc/cpuinfo flags match the
  //      build host's, so a copied binary cannot execute incompatible code.
  //   2. Otherwise the object is compiled once per distinct embedded-runtime
  //      content + flags + CPU into $XDG_CACHE_HOME/cstar/rt-<key>.o, and
  //      every program links gen.c against that object (no cross-TU inlining
  //      without LTO, so the result matches the single-invocation build).
  // Any miss or cache problem falls back to — or degrades to — the original
  // paths: never worse than before.
  std::string rt_obj;      // object to link with gen.c ("" = both TUs at once)
  std::string rt_obj_tmp;  // temp object to unlink after linking (no install)
  {
    std::string cdir = cache_dir();
    const std::string cpu = current_cpu_flags();
    std::string blob;
    for (unsigned int i = 0; i < pen_embedded_files_count; ++i) {
      blob.append(pen_embedded_files[i].name);
      blob.append(
          reinterpret_cast<const char*>(pen_embedded_files[i].data),
          pen_embedded_files[i].len);
    }
    blob += "-std=c11 -O3 -march=native -c";  // the exact object-compile flags
    blob += "|cpu:";  // -march=native objects are per-machine: scope the key
    blob += cpu;
    std::string key = cdir.empty() ? std::string() : compute_cache_key(blob);
    std::string rt_o = key.empty() ? std::string() : cdir + "/rt-" + key + ".o";
    // ensure_dir up front: a cold cache directory must exist before the
    // object is installed (both run and build modes).
    bool have_dir = !rt_o.empty() && ensure_dir(cdir);

    unsigned int pre_len = 0;
    const unsigned char* pre = find_embedded("cstar_rt.o", pre_len);
    unsigned int sig_len = 0;
    const unsigned char* sig = find_embedded("cstar_rt.cpu", sig_len);
    const bool pre_ok = pre != nullptr && pre_len > 0 &&
                        embedded_runtime_usable(sig, sig_len);

    if (have_dir && ::access(rt_o.c_str(), R_OK) == 0) {
      rt_obj = rt_o;  // hit: link straight against the cached object
    } else if (pre_ok) {
      // Cold cache: install the embedded object atomically (no clang run).
      // Without a cache dir it still beats compiling both TUs together.
      std::string dest = have_dir ? rt_o : (dir + "/cstar_rt.o");
      std::string tmp_o = dest + ".tmp" + std::to_string(::getpid());
      if (write_file(tmp_o, pre, pre_len) &&
          ::rename(tmp_o.c_str(), dest.c_str()) == 0) {
        rt_obj = dest;
        if (!have_dir) rt_obj_tmp = dest;
      } else {
        ::unlink(tmp_o.c_str());  // degrade to the on-demand compile below
      }
    }
    if (rt_obj.empty() && have_dir) {
      // Miss: compile the runtime TU (stdio inherited — diagnostics print
      // exactly as in the single-invocation build), then install the
      // object into the cache atomically.
      std::string tmp_o = rt_o + ".tmp" + std::to_string(::getpid());
      std::vector<std::string> cargv_str = {"clang",     "-std=c11", "-O3",
                                            "-march=native", "-c",    "-o",
                                            tmp_o,      inc,        rt_c};
      std::vector<char*> cargv;
      for (std::string& s : cargv_str) cargv.push_back(&s[0]);
      cargv.push_back(nullptr);
      int cerr2 = 0;
      int cstatus = run_command(cargv.data(), &cerr2);
      bool cbad = (cstatus == -1) ||
                  !(WIFEXITED(cstatus) && WEXITSTATUS(cstatus) == 0);
      if (cbad) {
        if (cstatus == -1) {
          std::fprintf(stderr, "error: cannot run 'clang': %s\n",
                       std::strerror(cerr2));
          if (cerr2 == ENOENT) {
            std::fprintf(stderr,
                         "error: 'clang' was not found on PATH; the cstar "
                         "toolchain requires clang to build programs\n");
          }
        } else if (WIFSIGNALED(cstatus)) {
          std::fprintf(stderr, "error: clang terminated by signal %d\n",
                       WTERMSIG(cstatus));
        }
        ::unlink(tmp_o.c_str());
        remove_sources(dir);
        ::rmdir(dir.c_str());
        return 1;
      }
      if (::rename(tmp_o.c_str(), rt_o.c_str()) == 0) {
        rt_obj = rt_o;  // cold install; concurrent racers write same bytes
      } else {
        rt_obj = tmp_o;       // cache unusable — still skip a second full
        rt_obj_tmp = tmp_o;   // runtime compile; unlink the temp afterwards
      }
    }
  }

  std::vector<std::string> argv_str = {"clang",
                                       "-std=c11",
                                       "-O3",
                                       "-march=native",
                                       "-o",
                                       in_out_path,
                                       inc,
                                       gen,
                                       rt_obj.empty() ? rt_c : rt_obj,
                                       "-lm",
                                       "-lcurl"};
  std::vector<char*> argv;
  for (std::string& s : argv_str) argv.push_back(&s[0]);
  argv.push_back(nullptr);

  int err = 0;
  int status = run_command(argv.data(), &err);
  if (status == -1) {
    std::fprintf(stderr, "error: cannot run 'clang': %s\n", std::strerror(err));
    if (err == ENOENT) {
      std::fprintf(stderr,
                   "error: 'clang' was not found on PATH; the cstar "
                   "toolchain requires clang to build programs\n");
    }
    if (!rt_obj_tmp.empty()) ::unlink(rt_obj_tmp.c_str());
    remove_sources(dir);
    ::rmdir(dir.c_str());
    return 1;
  }
  if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
    if (WIFSIGNALED(status)) {
      std::fprintf(stderr, "error: clang terminated by signal %d\n",
                   WTERMSIG(status));
    }
    ::unlink(in_out_path.c_str());
    if (!rt_obj_tmp.empty()) ::unlink(rt_obj_tmp.c_str());
    remove_sources(dir);
    ::rmdir(dir.c_str());
    return 1;
  }
  if (!rt_obj_tmp.empty()) ::unlink(rt_obj_tmp.c_str());
  remove_sources(dir);
  ::rmdir(dir.c_str());  // no-op when in_out_path lives inside dir
  return 0;
}

// The default entry file for `run` / `build` / `check` without a file.
constexpr const char kDefaultEntry[] = "index.cst";

// True for a source path: `.cst`, or the legacy `.pen` / `.pent` (accepted
// until v0.4).
bool is_source_path(const std::string& p) {
  return ends_with(p, ".cst") || ends_with(p, ".pen") || ends_with(p, ".pent");
}

bool is_regular_file(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool is_directory(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// `tools/scanner.cst` -> `scanner` (in the current directory): the source
// basename without its extension. Legacy `.pen` / `.pent` map the same way.
std::string default_output(const std::string& source_path) {
  std::size_t slash = source_path.rfind('/');
  std::string base = (slash == std::string::npos)
                         ? source_path
                         : source_path.substr(slash + 1);
  if (ends_with(base, ".pent")) {
    base.resize(base.size() - 5);  // strip ".pent"
  } else {
    base.resize(base.size() - 4);  // strip the validated ".cst" / ".pen"
  }
  return base;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);

  if (args.empty()) return usage_error("no input file given");
  if (args[0] == "--help" || args[0] == "-h") {
    print_usage(stdout);
    return 0;
  }
  if (args[0] == "--version" || args[0] == "-v") {
    std::printf("cstar %s\n", kVersion);
    return 0;
  }

  Mode mode = Mode::Run;
  std::size_t first = 0;  // index of the source path in args
  if (args[0] == "run" || args[0] == "build" || args[0] == "check") {
    if (args.size() < 2) {
      // No file: the default entry ./index.cst (like Node's index.js).
      if (!is_regular_file(kDefaultEntry)) {
        return usage_error(std::string("no source file given and ./") +
                           kDefaultEntry + " does not exist");
      }
      args.push_back(kDefaultEntry);
    }
    if (args[0] == "run") {
      mode = Mode::Run;
    } else if (args[0] == "build") {
      mode = Mode::Build;
    } else {
      mode = Mode::Check;
    }
    first = 1;
  }
  // Anything else is the bare `cstar <file.cst> [args...]` run form.

  const std::string source_path = args[first];
  std::vector<std::string> extra(args.begin() + static_cast<long>(first) + 1,
                                 args.end());

  if (!is_source_path(source_path)) {
    return usage_error(
        "source path must end with '.cst' (or the legacy '.pen'/'.pent'): '" +
        source_path + "'");
  }

  std::string build_out;
  if (mode == Mode::Build) {
    for (std::size_t i = 0; i < extra.size(); ++i) {
      if (extra[i] == "-o") {
        if (i + 1 >= extra.size()) {
          return usage_error("'-o' requires an output path");
        }
        if (!build_out.empty()) {
          return usage_error("'-o' given more than once");
        }
        build_out = extra[i + 1];
        ++i;
      } else {
        return usage_error("unexpected argument '" + extra[i] +
                           "' (usage: cstar build [<file.cst>] [-o out])");
      }
    }
    if (build_out.empty()) {
      build_out = default_output(source_path);
      if (is_directory(build_out)) {
        return usage_error("default output '" + build_out +
                           "' is a directory; choose a name with -o");
      }
    }
  } else if (mode == Mode::Check) {
    if (!extra.empty()) {
      return usage_error("'check' takes no arguments beyond the source file");
    }
  }

  std::string source;
  if (!read_file(source_path, source)) {
    std::fprintf(stderr, "error: cannot read source file '%s': %s\n",
                 source_path.c_str(), std::strerror(errno));
    return 1;
  }

  cstar::Diagnostics diags;
  diags.set_source(source_path, source);
  cstar::Program program;
  if (!cstar::parse_source(source_path, source, program, diags)) return 1;

  // Stage 2D: expand imports. In check mode every imported declaration is
  // kept for full validation; run/build prune imported declarations that are
  // unreachable, before semantic analysis or code generation.
  cstar::LinkResult linked;
  if (!cstar::link_imports(program, source_path, mode == Mode::Check, linked,
                             diags)) {
    return 1;
  }
  diags.set_source(source_path, source);  // reset current file for diagnostics

  // Cache fast path for `run`. The key covers the entry source and every
  // imported module's source, so editing a dependency invalidates the cached
  // binary. A program without imports hashes exactly as before.
  std::string cache_bin;
  if (mode == Mode::Run) {
    std::string cdir = cache_dir();
    if (!cdir.empty()) {
      std::string key_material = source;
      if (!linked.dependency_blob.empty()) {
        key_material.push_back('\0');
        key_material += linked.dependency_blob;
      }
      std::string key = compute_cache_key(key_material);
      if (!key.empty() && ensure_dir(cdir)) {
        cache_bin = cdir + "/" + key + ".bin";
        if (::access(cache_bin.c_str(), X_OK) == 0) {
          try_exec(cache_bin, extra);  // falls through only on exec failure
        }
      }
    }
  }

  if (!cstar::semantic_check(program, diags)) return 1;

  if (mode == Mode::Check) return 0;  // silent success

  std::string out = (mode == Mode::Build) ? build_out : cache_bin;
  if (compile_program(program, out) != 0) return 1;

  if (mode == Mode::Build) return 0;

  if (!try_exec(out, extra)) {
    std::fprintf(stderr, "error: cannot execute '%s': %s\n", out.c_str(),
                 std::strerror(errno));
    return 1;
  }
  return 0;  // unreachable on successful exec
}
