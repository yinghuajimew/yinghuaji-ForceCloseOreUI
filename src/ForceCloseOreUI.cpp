#include "api/memory/Hook.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

#if __arm__
#include <unistd.h>
extern "C" int __wrap_getpagesize() { return sysconf(_SC_PAGESIZE); }

#endif

#if __arm__ || __aarch64__
#include "jni.h"
#include <android/log.h>
#include <ctime>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/wait.h>

JNIEnv *env = nullptr;
JavaVM *javaVm = nullptr;

#define LOGI(...)                                                              \
  __android_log_print(ANDROID_LOG_INFO, "LeviLogger", __VA_ARGS__)
#define LOGE(...)                                                              \
  __android_log_print(ANDROID_LOG_ERROR, "LeviLogger", __VA_ARGS__)

namespace {

bool clearJavaException(JNIEnv *env) {
  if (!env || !env->ExceptionCheck())
    return false;
  env->ExceptionClear();
  return true;
}

std::string getJStringUtf(JNIEnv *env, jstring value) {
  if (!env || !value)
    return {};

  const char *cstr = env->GetStringUTFChars(value, nullptr);
  if (!cstr) {
    clearJavaException(env);
    return {};
  }

  std::string result(cstr);
  env->ReleaseStringUTFChars(value, cstr);
  return result;
}

JNIEnv *getCurrentJNIEnv() {
  if (javaVm) {
    JNIEnv *current_env = nullptr;
    jint status = javaVm->GetEnv(reinterpret_cast<void **>(&current_env),
                                 JNI_VERSION_1_4);
    if (status == JNI_OK)
      return current_env;

    if (status == JNI_EDETACHED &&
        javaVm->AttachCurrentThread(&current_env, nullptr) == JNI_OK)
      return current_env;
  }

  return env;
}

} // namespace

jobject getGlobalContext(JNIEnv *env) {
  if (!env)
    return nullptr;

  jclass activity_thread = env->FindClass("android/app/ActivityThread");
  if (clearJavaException(env) || !activity_thread)
    return nullptr;

  jmethodID current_activity_thread =
      env->GetStaticMethodID(activity_thread, "currentActivityThread",
                             "()Landroid/app/ActivityThread;");
  if (clearJavaException(env) || !current_activity_thread) {
    env->DeleteLocalRef(activity_thread);
    return nullptr;
  }

  jobject at =
      env->CallStaticObjectMethod(activity_thread, current_activity_thread);
  if (clearJavaException(env) || !at) {
    env->DeleteLocalRef(activity_thread);
    return nullptr;
  }

  jmethodID get_application = env->GetMethodID(
      activity_thread, "getApplication", "()Landroid/app/Application;");
  if (clearJavaException(env) || !get_application) {
    env->DeleteLocalRef(at);
    env->DeleteLocalRef(activity_thread);
    return nullptr;
  }

  jobject context = env->CallObjectMethod(at, get_application);
  if (clearJavaException(env)) {
    context = nullptr;
  }

  env->DeleteLocalRef(at);
  env->DeleteLocalRef(activity_thread);
  return context;
}

std::string getAbsolutePath(JNIEnv *env, jobject file) {
  if (!env || !file)
    return {};

  jclass file_class = env->GetObjectClass(file);
  if (clearJavaException(env) || !file_class)
    return {};

  jmethodID get_abs_path =
      env->GetMethodID(file_class, "getAbsolutePath", "()Ljava/lang/String;");
  if (clearJavaException(env) || !get_abs_path) {
    env->DeleteLocalRef(file_class);
    return {};
  }

  auto jstr = (jstring)env->CallObjectMethod(file, get_abs_path);
  if (clearJavaException(env) || !jstr) {
    env->DeleteLocalRef(file_class);
    return {};
  }

  std::string result = getJStringUtf(env, jstr);
  env->DeleteLocalRef(jstr);
  env->DeleteLocalRef(file_class);
  return result;
}

std::string getPackageName(JNIEnv *env, jobject context) {
  if (!env || !context)
    return {};

  jclass context_class = env->GetObjectClass(context);
  if (clearJavaException(env) || !context_class)
    return {};

  jmethodID get_pkg_name =
      env->GetMethodID(context_class, "getPackageName", "()Ljava/lang/String;");
  if (clearJavaException(env) || !get_pkg_name) {
    env->DeleteLocalRef(context_class);
    return {};
  }

  auto jstr = (jstring)env->CallObjectMethod(context, get_pkg_name);
  if (clearJavaException(env) || !jstr) {
    env->DeleteLocalRef(context_class);
    return {};
  }

  std::string result = getJStringUtf(env, jstr);
  env->DeleteLocalRef(jstr);
  env->DeleteLocalRef(context_class);
  return result;
}

std::string getInternalStoragePath(JNIEnv *env) {
  if (!env)
    return {};

  jclass env_class = env->FindClass("android/os/Environment");
  if (clearJavaException(env) || !env_class)
    return {};

  jmethodID get_storage_dir = env->GetStaticMethodID(
      env_class, "getExternalStorageDirectory", "()Ljava/io/File;");
  if (clearJavaException(env) || !get_storage_dir) {
    env->DeleteLocalRef(env_class);
    return {};
  }

  jobject storage_dir = env->CallStaticObjectMethod(env_class, get_storage_dir);
  if (clearJavaException(env) || !storage_dir) {
    env->DeleteLocalRef(env_class);
    return {};
  }

  std::string result = getAbsolutePath(env, storage_dir);
  env->DeleteLocalRef(storage_dir);
  env->DeleteLocalRef(env_class);
  return result;
}

// ================================================================
// 以下为从非依赖版（Dobby 版）迁移过来的通用方法。
// 只做增量：不改动本文件原有的任何函数，也不介入 pl::hook 的挂钩时机。
// ================================================================

// ----------------------------------------------------------------
// 双通道日志（文件 + logcat）
// 依赖原本只有 logcat；补上文件通道，游戏崩溃后仍能翻 debug.log。
// ----------------------------------------------------------------
static std::string g_depLogFilePath;

static void WriteLog(const char *level, const char *format, ...) {
  va_list args;
  va_start(args, format);
  char buf[4096];
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);

  int prio = (level[0] == 'E') ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO;
  __android_log_print(prio, "ForceCloseOreUI", "[%s] %s", level, buf);

  if (!g_depLogFilePath.empty()) {
    FILE *file = fopen(g_depLogFilePath.c_str(), "a");
    if (file) {
      fprintf(file, "[%s] %s\n", level, buf);
      fclose(file);
    }
  }
}

// 独立宏名：不覆盖上面的 LOGI/LOGE，避免改变原有 logConfigInfo/Error 的 tag
#define LOGF(...) WriteLog("INFO", __VA_ARGS__)
#define LOGFE(...) WriteLog("ERROR", __VA_ARGS__)

// ----------------------------------------------------------------
// 每次启动在 debug.log 顶部写入设备环境信息
// ----------------------------------------------------------------
static void writeLogHeader() {
  if (g_depLogFilePath.empty())
    return;

  char manufacturer[PROP_VALUE_MAX] = {};
  char model[PROP_VALUE_MAX] = {};
  char android_ver[PROP_VALUE_MAX] = {};
  char sdk_int[PROP_VALUE_MAX] = {};
  char cpu_abi[PROP_VALUE_MAX] = {};
  char build_fp[PROP_VALUE_MAX] = {};
  char device_code[PROP_VALUE_MAX] = {};

  __system_property_get("ro.product.manufacturer", manufacturer);
  __system_property_get("ro.product.model", model);
  __system_property_get("ro.build.version.release", android_ver);
  __system_property_get("ro.build.version.sdk", sdk_int);
  __system_property_get("ro.product.cpu.abi", cpu_abi);
  __system_property_get("ro.build.fingerprint", build_fp);
  __system_property_get("ro.product.device", device_code);

  char kernel_ver[256] = {};
  {
    FILE *kfp = fopen("/proc/version", "r");
    if (kfp) {
      if (fgets(kernel_ver, sizeof(kernel_ver), kfp)) {
        size_t len = strlen(kernel_ver);
        while (len > 0 && (kernel_ver[len - 1] == '\n' || kernel_ver[len - 1] == '\r'))
          kernel_ver[--len] = '\0';
      }
      fclose(kfp);
    }
  }

  char timebuf[32] = {};
  {
    time_t now = time(nullptr);
    struct tm *tm_info = localtime(&now);
    if (tm_info)
      strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info);
  }

  long page_size = sysconf(_SC_PAGESIZE);

  FILE *file = fopen(g_depLogFilePath.c_str(), "a");
  if (!file)
    return;
  fprintf(file,
          "\n======================================================\n"
          "[INFO] === SESSION START: %s ===\n"
          "[INFO] Variant      : dependency (preloader)\n"
          "[INFO] Manufacturer : %s\n"
          "[INFO] Model        : %s  (device: %s)\n"
          "[INFO] Android      : %s  (SDK %s)\n"
          "[INFO] CPU ABI      : %s  (page size: %ld bytes)\n"
          "[INFO] Fingerprint  : %s\n"
          "[INFO] Kernel       : %s\n"
          "[INFO] PID          : %d\n"
          "======================================================\n",
          timebuf[0] ? timebuf : "unknown",
          manufacturer[0] ? manufacturer : "unknown",
          model[0] ? model : "unknown",
          device_code[0] ? device_code : "?",
          android_ver[0] ? android_ver : "unknown",
          sdk_int[0] ? sdk_int : "?",
          cpu_abi[0] ? cpu_abi : "unknown",
          page_size,
          build_fp[0] ? build_fp : "unknown",
          kernel_ver[0] ? kernel_ver : "unknown",
          getpid());
  fclose(file);
}

// ----------------------------------------------------------------
// 包名：优先 /proc/self/cmdline（不依赖 JNI 是否已就绪）
// ----------------------------------------------------------------
static std::string getRuntimePackageName() {
  std::ifstream cmdline("/proc/self/cmdline");
  std::string pkg;
  std::getline(cmdline, pkg, '\0');
  return pkg.empty() ? std::string("com.mojang.minecraftpe") : pkg;
}

// ----------------------------------------------------------------
// 配置目录：逐级创建 + 可写探测 + 内部存储兜底
// 只在第一次调用时确定，之后走缓存。
// ----------------------------------------------------------------
static std::string getDepConfigDir() {
  static std::string cached;
  if (!cached.empty())
    return cached;

  const std::string pkg = getRuntimePackageName();

  std::vector<std::string> candidates = {
      "/storage/emulated/0/games/com.mojang/ForceCloseOreUI/",
      "/storage/emulated/0/Android/data/" + pkg + "/mods/ForceCloseOreUI/",
  };

  for (const auto &base : candidates) {
    size_t lastSlash = base.find_last_of('/', base.size() - 2);
    std::string parent1 = base.substr(0, lastSlash);
    lastSlash = parent1.find_last_of('/');
    std::string parent2 = parent1.substr(0, lastSlash);

    mkdir(parent2.c_str(), 0755);
    mkdir(parent1.c_str(), 0755);
    mkdir(base.c_str(), 0755);

    std::string testFile = base + ".test";
    FILE *fp = fopen(testFile.c_str(), "wb");
    if (fp) {
      fclose(fp);
      unlink(testFile.c_str());
      cached = base;
      g_depLogFilePath = base + "debug.log";
      writeLogHeader();
      LOGF("Using config dir: %s", base.c_str());
      return cached;
    }
    LOGF("Path not writable: %s (errno=%d: %s)", base.c_str(), errno, strerror(errno));
  }

  std::string fallback = "/data/data/" + pkg + "/files/ForceCloseOreUI/";
  mkdir(("/data/data/" + pkg + "/files").c_str(), 0755);
  mkdir(fallback.c_str(), 0755);
  cached = fallback;
  g_depLogFilePath = fallback + "debug.log";
  writeLogHeader();
  LOGFE("All external paths failed! Using internal: %s", fallback.c_str());
  return cached;
}

// ----------------------------------------------------------------
// MC 版本指纹：用 APK 安装路径里的 hash 段
// ----------------------------------------------------------------
static std::string getMcVersionStr() {
  FILE *fp = fopen("/proc/self/maps", "r");
  if (!fp)
    return "";

  const std::string pkg = getRuntimePackageName();
  const std::string needle = pkg + "-";

  char line[1024];
  std::string ver;
  std::string fallback;
  while (fgets(line, sizeof(line), fp)) {
    if (!strstr(line, "libminecraftpe.so") && !strstr(line, "base.apk"))
      continue;

    if (fallback.empty()) {
      char *slash = strchr(line, '/');
      if (slash) {
        char *nl = strchr(slash, '\n');
        if (nl)
          *nl = '\0';
        fallback = std::string(slash);
      }
    }

    if (!pkg.empty()) {
      char *start = strstr(line, needle.c_str());
      if (start) {
        while (start > line && start[-1] != ' ' && start[-1] != '\t')
          --start;
        char *end = strchr(start, '/');
        if (end) {
          ver = std::string(start, end);
          break;
        }
      }
    }
  }
  fclose(fp);

  if (!ver.empty())
    return ver;
  return fallback;
}

// ----------------------------------------------------------------
// 配置目录初始化（幂等）+ readme 生成
// 与原有的 resolveConfigFiles() 并存：原有路径探测逻辑照旧生效，
// 这里只负责确保目录/日志可写，并生成说明文件。
// ----------------------------------------------------------------
static void ensureDepConfigExists() {
  const std::string dir = getDepConfigDir();

  std::string configPath = dir + "config.json";
  std::string readmePath = dir + "readme.txt";
  struct stat st;

  if (stat(configPath.c_str(), &st) != 0 || st.st_size == 0) {
    LOGF("config.json will be created by applyConfig on first hook.");
  }

  if (stat(readmePath.c_str(), &st) != 0) {
    FILE *fp = fopen(readmePath.c_str(), "w");
    if (fp) {
      fprintf(fp,
              "=== ForceCloseOreUI (依赖版 / preloader) 说明 ===\n\n"
              "【 config.json 】\n"
              "  扁平结构，键为 OreUI 界面名，值为 true 表示保留 OreUI。\n\n"
              "  \"key\": true    -> 保留该界面的 OreUI\n"
              "  \"key\": false   -> 关闭该界面的 OreUI（用旧版 UI）\n\n"
              "  配置里没有的条目：默认关闭 OreUI（由程序首次运行时补全）。\n"
              "  修改保存后下次进入对应界面即生效。\n\n"
              "【 debug.log 】\n"
              "  每次启动的设备信息头 + 运行日志。\n"
              "【 crash_native.log 】\n"
              "  Native 崩溃现场（信号、fault 地址、寄存器）。\n"
              "【 minecraft_runtime.log 】\n"
              "  游戏自身 logcat 快照（最多 50 分钟）。\n");
      fclose(fp);
    }
  }

  LOGF("Config dir ready: %s", dir.c_str());
  LOGF("Packages: %s | MC id: %s",
       getRuntimePackageName().c_str(), getMcVersionStr().c_str());
}

// ----------------------------------------------------------------
// Native 崩溃捕获（全程只用 async-signal-safe 函数）
// ----------------------------------------------------------------
static char g_depCrashLogPath[512] = {0};
static struct sigaction g_depOldSigSegv;
static struct sigaction g_depOldSigAbrt;
static struct sigaction g_depOldSigBus;

static const char *sigToStr(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV (Segmentation Fault)";
    case SIGABRT: return "SIGABRT (Abort)";
    case SIGBUS:  return "SIGBUS (Bus Error)";
    default:      return "Unknown Signal";
  }
}

static void crashSignalHandler(int sig, siginfo_t *info, void *ucontext) {
  if (g_depCrashLogPath[0] == '\0')
    goto reraise;

  {
    int fd = open(g_depCrashLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
      char buf[2048];
      int len;

      ucontext_t *uc = (ucontext_t *)ucontext;
      uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;

      len = snprintf(buf, sizeof(buf),
                     "\n========================================\n"
                     "[NATIVE CRASH] %s\n"
                     "Fault address : %p\n"
                     "Signal code   : %d\n"
                     "PID           : %d\n"
                     "PC            : 0x%lx\n"
                     "========================================\n",
                     sigToStr(sig), info->si_addr, info->si_code, getpid(),
                     (unsigned long)pc);
      write(fd, buf, len);

      for (int i = 0; i < 31; i++) {
        len = snprintf(buf, sizeof(buf), "X%-2d: 0x%016lx\n", i,
                       (unsigned long)uc->uc_mcontext.regs[i]);
        write(fd, buf, len);
      }
      len = snprintf(buf, sizeof(buf), "SP : 0x%016lx\nLR : 0x%016lx\n",
                     (unsigned long)uc->uc_mcontext.sp,
                     (unsigned long)uc->uc_mcontext.regs[30]);
      write(fd, buf, len);
      close(fd);
    }
  }

reraise:
  switch (sig) {
    case SIGSEGV: sigaction(SIGSEGV, &g_depOldSigSegv, nullptr); break;
    case SIGABRT: sigaction(SIGABRT, &g_depOldSigAbrt, nullptr); break;
    case SIGBUS:  sigaction(SIGBUS, &g_depOldSigBus, nullptr); break;
    default: break;
  }
  raise(sig);
}

static void installDepCrashHandlers() {
  std::string path = getDepConfigDir() + "crash_native.log";
  snprintf(g_depCrashLogPath, sizeof(g_depCrashLogPath), "%s", path.c_str());

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crashSignalHandler;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigemptyset(&sa.sa_mask);

  sigaction(SIGSEGV, &sa, &g_depOldSigSegv);
  sigaction(SIGABRT, &sa, &g_depOldSigAbrt);
  sigaction(SIGBUS, &sa, &g_depOldSigBus);

  LOGF("Crash handler installed. Log: %s", g_depCrashLogPath);
}

// ----------------------------------------------------------------
// 游戏 logcat 快照（每 2 秒增量续读，最多 50 分钟）
// ----------------------------------------------------------------
static void *depLogcatThread(void *) {
  const std::string dir = getDepConfigDir();
  std::string logPath = dir + "minecraft_runtime.log";
  FILE *clear = fopen(logPath.c_str(), "w");
  if (clear)
    fclose(clear);

  pid_t pid = getpid();
  char pidStr[32];
  snprintf(pidStr, sizeof(pidStr), "%d", pid);

  std::string lastTimestamp;

  for (int i = 0; i < 1500; i++) {
    int pipefd[2];
    if (pipe(pipefd) != 0)
      break;

    pid_t child = fork();
    if (child == 0) {
      close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[1]);

      if (lastTimestamp.empty()) {
        execl("/system/bin/logcat", "logcat", "-d", "-v", "threadtime", "-t",
              "200", "--pid", pidStr, "*:V", (char *)nullptr);
      } else {
        execl("/system/bin/logcat", "logcat", "-d", "-v", "threadtime", "-T",
              lastTimestamp.c_str(), "--pid", pidStr, "*:V", (char *)nullptr);
      }
      _exit(127);
    }

    close(pipefd[1]);

    FILE *logFile = fopen(logPath.c_str(), "a");
    if (!logFile) {
      close(pipefd[0]);
      kill(child, SIGTERM);
      break;
    }

    std::string lastLine;
    char buf[8192];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
      buf[n] = '\0';
      fputs(buf, logFile);
      fflush(logFile);

      char *line = buf;
      while (char *nl = strchr(line, '\n')) {
        *nl = '\0';
        lastLine = line;
        line = nl + 1;
      }
      if (*line != '\0')
        lastLine = line;
    }

    fclose(logFile);
    close(pipefd[0]);

    int status;
    waitpid(child, &status, 0);

    if (!lastLine.empty() && lastLine.size() > 18) {
      std::string candidate = lastLine.substr(0, 18);
      if (candidate[2] == '-' && candidate[5] == ' ' && candidate[8] == ':')
        lastTimestamp = candidate;
    }

    usleep(2000 * 1000);
  }

  LOGF("Logcat snapshot thread stopped.");
  return nullptr;
}

std::string GetModsFilesPath(JNIEnv *env) {
  jobject app_context = getGlobalContext(env);
  if (!app_context) {
    return "";
  }
  auto package_name = getPackageName(env, app_context);
  env->DeleteLocalRef(app_context);
  if (package_name.empty())
    return "";

  std::transform(
      package_name.begin(), package_name.end(), package_name.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  std::string storage_path = getInternalStoragePath(env);
  if (storage_path.empty())
    return "";

  return (fs::path(storage_path) / "Android" / "data" / package_name / "mods")
      .string();
}

SKY_AUTO_STATIC_HOOK(
    Hook1, memory::HookPriority::Normal,
    std::initializer_list<const char *>(
        {"? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? "
         "91 ? ? ? D5 ? ? ? F9 ? ? ? F8 ? ? ? 39 ? ? ? 34 ? ? ? 12"}),
    int, void *_this, JavaVM *vm) {

  javaVm = vm;
  if (vm)
    vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_4);
  return origin(_this, vm);
}

#endif

class OreUIConfig {
public:
  void *mUnknown1;
  void *mUnknown2;
  std::function<bool()> mUnknown3;
  std::function<bool()> mUnknown4;
};

class OreUi {
public:
  std::unordered_map<std::string, OreUIConfig> mConfigs;
};

// clang-format off
#if __arm__
#define OREUI_PATTERN 
   {""}

#elif __aarch64__
// 按版本从新到旧排列。resolveIdentifier(initializer_list) 逐条尝试，
// 第一条命中即用，所以顺序即优先级。覆盖 1.21.90 ~ 1.26.60。
#define OREUI_PATTERN                                                              \
     std::initializer_list<const char *>({                                         \
      /* 1.26.60 */                                                                \
      "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? B9 ? ? ? D5 F9 03 03 2A", \
      /* 1.26.50 */                                                                \
      "? ? ? D1 ? ? ? A9 ? ? ? F9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 F5 03 03 2A F6 03 02 2A ? ? ? F9 F4 03 01 AA", \
      /* 1.26.40 */                                                                \
      "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 FA 03 03 2A F7 03 02 2A ? ? ? F9 F4 03 01 AA", \
      /* 1.26.30 */                                                                \
      "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 F7 03 05 AA FB 03 03 2A", \
      /* 1.26.20 */                                                                \
      "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 FB 03 03 2A F8 03 02 2A", \
  })                                                                               \

#endif

// clang-format on

namespace {

using Json = nlohmann::json;

constexpr char kConfigFileName[] = "config.json";
constexpr char kModDirName[] = "ForceCloseOreUI";

void logConfigInfo(const std::string &message) {
#if __arm__ || __aarch64__
  LOGI("%s", message.c_str());
#else
  std::printf("[ForceCloseOreUI] %s\n", message.c_str());
#endif
}

void logConfigError(const std::string &message) {
#if __arm__ || __aarch64__
  LOGE("%s", message.c_str());
#else
  std::printf("[ForceCloseOreUI] %s\n", message.c_str());
#endif
}

std::string normalizedPathText(const fs::path &path) {
  return path.lexically_normal().generic_string();
}

void appendUniquePath(std::vector<fs::path> &paths, fs::path path) {
  if (path.empty())
    return;

  const std::string normalized = normalizedPathText(path);
  auto duplicated =
      std::any_of(paths.begin(), paths.end(), [&](const fs::path &item) {
        return normalizedPathText(item) == normalized;
      });
  if (!duplicated)
    paths.emplace_back(std::move(path));
}

bool pathExists(const fs::path &path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

bool isDirectoryWritable(const fs::path &dir) {
  if (dir.empty())
    return false;

  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec)
    return false;

  const fs::path test_file = dir / "._perm_test";
  std::ofstream ofs(test_file, std::ios::binary | std::ios::trunc);
  if (!ofs.is_open())
    return false;

  ofs.close();
  fs::remove(test_file, ec);
  return true;
}

std::vector<fs::path> getConfigDirCandidates() {
  std::vector<fs::path> paths;
  
#if __arm__ || __aarch64__
  std::string primary = "/storage/emulated/0/Android/data/com.mojang.minecraftpe/mods/";
  if (!primary.empty()) {
    primary += "/ForceCloseOreUI/";
    if (isDirectoryWritable(primary))
      appendUniquePath(paths, primary);
  }
  
  if (env) {
    std::string base = GetModsFilesPath(env);
    if (!base.empty()) {
      base += "/ForceCloseOreUI/";
      if (isDirectoryWritable(base))
        appendUniquePath(paths, base);
    }
  }
  
  appendUniquePath(paths, fs::path("/sdcard/games") / kModDirName);
  appendUniquePath(paths, fs::path("/storage/emulated/0/games") / kModDirName);
#endif
  
  return paths;
}

fs::path selectWritableConfigDir(const std::vector<fs::path> &candidates) {
  for (const auto &dir : candidates) {
    if (isDirectoryWritable(dir))
      return dir;
  }

  if (!candidates.empty())
    return candidates.front();
  return fs::path("mods") / kModDirName;
}

struct ConfigFiles {
  fs::path source;
  fs::path target;
};

ConfigFiles resolveConfigFiles() {
  std::vector<fs::path> candidates = getConfigDirCandidates();
  fs::path target_dir = selectWritableConfigDir(candidates);
  fs::path target = target_dir / kConfigFileName;

  if (pathExists(target))
    return {target, target};

  for (const auto &dir : candidates) {
    fs::path candidate = dir / kConfigFileName;
    if (pathExists(candidate))
      return {candidate, target};
  }

  return {{}, target};
}

std::string trimAscii(std::string value) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  value.erase(value.begin(),
              std::find_if_not(value.begin(), value.end(), is_space));
  value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(),
              value.end());
  return value;
}

std::optional<bool> parseBoolString(std::string value) {
  value = trimAscii(std::move(value));
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (value == "true" || value == "1" || value == "yes" || value == "on" ||
      value == "enabled")
    return true;
  if (value == "false" || value == "0" || value == "no" || value == "off" ||
      value == "disabled")
    return false;
  return std::nullopt;
}

std::optional<bool> readBoolLike(const Json &value) {
  try {
    if (value.is_boolean())
      return value.get<bool>();
    if (value.is_number_integer())
      return value.get<long long>() != 0;
    if (value.is_number_unsigned())
      return value.get<unsigned long long>() != 0;
    if (value.is_number_float())
      return value.get<double>() != 0.0;
    if (value.is_string())
      return parseBoolString(value.get<std::string>());
    if (value.is_object()) {
      for (std::string_view key :
           {"value", "enabled", "enable", "state", "default"}) {
        auto it = value.find(std::string(key));
        if (it == value.end())
          continue;
        if (auto parsed = readBoolLike(*it))
          return parsed;
      }

      auto disabled = value.find("disabled");
      if (disabled != value.end()) {
        if (auto parsed = readBoolLike(*disabled))
          return !*parsed;
      }
    }
  } catch (...) {
  }

  return std::nullopt;
}

std::optional<bool> readNamedConfigValue(const Json &root,
                                         const std::string &name) {
  if (root.is_object()) {
    auto direct = root.find(name);
    if (direct != root.end()) {
      if (auto parsed = readBoolLike(*direct))
        return parsed;
    }

    for (std::string_view key :
         {"configs", "settings", "toggles", "values", "oreui", "OreUI"}) {
      auto section = root.find(std::string(key));
      if (section == root.end() || !section->is_object())
        continue;

      auto nested = section->find(name);
      if (nested != section->end()) {
        if (auto parsed = readBoolLike(*nested))
          return parsed;
      }
    }
  }

  if (root.is_array()) {
    for (const Json &item : root) {
      if (!item.is_object())
        continue;

      bool name_matched = false;
      for (std::string_view key : {"name", "key", "id"}) {
        auto name_node = item.find(std::string(key));
        if (name_node != item.end() && name_node->is_string() &&
            name_node->get<std::string>() == name) {
          name_matched = true;
          break;
        }
      }

      if (name_matched) {
        if (auto parsed = readBoolLike(item))
          return parsed;
      }
    }
  }

  return std::nullopt;
}

struct ConfigDocument {
  Json raw = Json::object();
  Json canonical = Json::object();
  fs::path source;
  fs::path target;
  bool dirty = false;
};

ConfigDocument loadConfigDocument() {
  ConfigFiles files = resolveConfigFiles();
  ConfigDocument document;
  document.source = files.source;
  document.target = files.target;
  document.dirty =
      document.source.empty() || normalizedPathText(document.source) !=
                                     normalizedPathText(document.target);

  if (document.source.empty())
    return document;

  try {
    std::ifstream input(document.source, std::ios::binary);
    if (!input.is_open()) {
      document.dirty = true;
      logConfigError("Failed to open config: " + document.source.string());
      return document;
    }

    Json parsed = Json::parse(input, nullptr, false, true);
    if (parsed.is_discarded()) {
      document.dirty = true;
      logConfigError("Invalid config json, rebuilding: " +
                     document.source.string());
      return document;
    }

    document.raw = parsed;
    if (parsed.is_object()) {
      document.canonical = std::move(parsed);
    } else {
      document.dirty = true;
      document.canonical = Json::object();
      logConfigInfo("Legacy non-object config detected, migrating: " +
                    document.source.string());
    }
  } catch (const std::exception &e) {
    document.dirty = true;
    logConfigError(std::string("Failed to read config: ") + e.what());
  } catch (...) {
    document.dirty = true;
    logConfigError("Failed to read config: unknown error");
  }

  return document;
}

bool saveConfigDocument(const fs::path &path, const Json &config) {
  if (path.empty())
    return false;

  try {
    std::error_code ec;
    fs::path parent = path.parent_path();
    if (!parent.empty()) {
      fs::create_directories(parent, ec);
      if (ec) {
        logConfigError("Failed to create config directory: " + parent.string());
        return false;
      }
    }

    fs::path temp_path = path;
    temp_path += ".tmp";

    {
      std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
      if (!output.is_open()) {
        logConfigError("Failed to write config temp file: " +
                       temp_path.string());
        return false;
      }

      std::string payload = config.dump(4);
      output.write(payload.data(),
                   static_cast<std::streamsize>(payload.size()));
      output.put('\n');
      output.close();
      if (!output) {
        logConfigError("Failed to flush config temp file: " +
                       temp_path.string());
        return false;
      }
    }

    fs::rename(temp_path, path, ec);
    if (!ec)
      return true;

    ec.clear();
    fs::copy_file(temp_path, path, fs::copy_options::overwrite_existing, ec);
    std::error_code remove_ec;
    fs::remove(temp_path, remove_ec);
    if (ec) {
      logConfigError("Failed to replace config: " + path.string());
      return false;
    }

    return true;
  } catch (const std::exception &e) {
    logConfigError(std::string("Failed to save config: ") + e.what());
  } catch (...) {
    logConfigError("Failed to save config: unknown error");
  }

  return false;
}

void setOreUiConfigValue(OreUIConfig &config, bool value) {
  config.mUnknown3 = [value]() { return value; };
  config.mUnknown4 = [value]() { return value; };
}

void applyConfig(OreUi &ore_ui) {
  ConfigDocument document = loadConfigDocument();
  bool dirty = document.dirty || !pathExists(document.target);

  if (!document.canonical.is_object()) {
    document.canonical = Json::object();
    dirty = true;
  }

  for (auto &[name, config] : ore_ui.mConfigs) {
    bool value = false;
    bool rewrite_value = false;

    auto existing = document.canonical.find(name);
    if (existing != document.canonical.end()) {
      if (auto parsed = readBoolLike(*existing)) {
        value = *parsed;
        rewrite_value = !existing->is_boolean();
      } else {
        if (auto legacy = readNamedConfigValue(document.raw, name))
          value = *legacy;
        rewrite_value = true;
      }
    } else {
      if (auto legacy = readNamedConfigValue(document.raw, name))
        value = *legacy;
      rewrite_value = true;
    }

    if (rewrite_value) {
      document.canonical[name] = value;
      dirty = true;
    }

    setOreUiConfigValue(config, value);
  }

  if (dirty && !saveConfigDocument(document.target, document.canonical)) {
    logConfigError("Config was applied in memory but could not be saved.");
  }
}

SKY_AUTO_STATIC_HOOK(Hook2, memory::HookPriority::Normal, OREUI_PATTERN, void,
                     OreUi &a1, void *a2, void *a3, void *a4, void *a5,
                     void *a6) {
  origin(a1, a2, a3, a4, a5, a6);

  try {
    applyConfig(a1);
  } catch (const std::exception &e) {
    logConfigError(std::string("Failed to apply OreUI config: ") + e.what());
  } catch (...) {
    logConfigError("Failed to apply OreUI config: unknown error");
  }
}

} // namespace

// ================================================================
// 模块入口
// 依赖版原本靠 SKY_AUTO_STATIC_HOOK 的静态注册器在加载时挂勾，
// 这里只补"旁路"能力（日志/崩溃/设备信息），不改变挂钩时机：
// constructor 里不做任何 hook 动作，钩子仍由静态注册器完成。
// ================================================================
__attribute__((constructor)) static void ForceCloseOreUI_Init() {
#if __arm__ || __aarch64__
  ensureDepConfigExists();

  LOGF("=== ForceCloseOreUI (dependency) Started ===");

  installDepCrashHandlers();

  pthread_t logcat_thread;
  if (pthread_create(&logcat_thread, nullptr, depLogcatThread, nullptr) != 0)
    LOGFE("Logcat thread create failed");
  else
    pthread_detach(logcat_thread);
#endif
}
