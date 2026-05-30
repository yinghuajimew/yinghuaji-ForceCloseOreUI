#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <android/log.h>
#include <errno.h>

#include "api/memory/Hook.h"
#include "jni.h"

namespace fs = std::filesystem;
using Json = nlohmann::json;

// ------------------------------------------------------------
// 双通道日志（文件 + Logcat）
// ------------------------------------------------------------
static std::string getConfigDir();

static void WriteLog(const char* level, const char* format, ...) {
    {
        va_list args;
        va_start(args, format);
        char buf[4096];
        vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        int prio = (level[0] == 'E') ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO;
        __android_log_print(prio, "ForceCloseOreUI", "[%s] %s", level, buf);
    }
    try {
        std::string logPath = getConfigDir() + "debug.log";
        FILE* file = fopen(logPath.c_str(), "a");
        if (file) {
            va_list args2;
            va_start(args2, format);
            fprintf(file, "[%s] ", level);
            vfprintf(file, format, args2);
            fprintf(file, "\n");
            va_end(args2);
            fclose(file);
        }
    } catch (...) {}
}

#define LOGI(...) WriteLog("INFO", __VA_ARGS__)
#define LOGE(...) WriteLog("ERROR", __VA_ARGS__)

// ------------------------------------------------------------
// Game classes
// ------------------------------------------------------------
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

// ------------------------------------------------------------
// 双平台特征码（保持兼容性）
// ------------------------------------------------------------
#if __arm__
#define OREUI_PATTERN {""}
#elif __aarch64__
#define OREUI_PATTERN \
    std::initializer_list<const char *>({\
        "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 ? ? ? D5 FA 03 00 AA F5 03 07 AA", \
        "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 ? ? ? D5 FB 03 00 AA F5 03 07 AA", \
        "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? F9 ? ? ? D5 FB 03 00 AA ? ? ? F9 F5 03 07 AA", \
        "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 ? ? ? D5 FA 03 00 AA F6 03 07 AA", \
        "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 ? ? ? D5 FA 03 00 AA F5 03 07 AA" \
    })
#elif _WIN32
#define OREUI_PATTERN \
    std::initializer_list<const char *>({\
        "40 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 68 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 ? 49 8B E9 4C 89 44 24 ? 4C 8B EA 48 8B F9 48 89 4C 24", \
        "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC 18 02 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 49 8B F1 4C 89 44 24", \
        "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC B8 01 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 49 8B F1 4C 89 44 24", \
        "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC 98 01 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 4D 8B F1 4C 89 44 24" \
    })
#endif

// ------------------------------------------------------------
// 智能模块定位
// ------------------------------------------------------------
struct ModuleInfo {
    uintptr_t base;
    size_t size;
};

static bool findMinecraftSegment(ModuleInfo& out) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return false;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, "r-x")) continue;
        if (!strstr(line, "libminecraftpe.so")) continue;

        uintptr_t start, end;
        if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
            out.base = start;
            out.size = end - start;
            fclose(fp);
            return true;
        }
    }

    // 兜底：extractNativeLibs="false" 时 SO 映射为 base.apk
    rewind(fp);
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, "r-x")) continue;
        uintptr_t start, end;
        if (sscanf(line, "%lx-%lx", &start, &end) != 2) continue;
        if ((end - start) > 30 * 1024 * 1024) {
            out.base = start;
            out.size = end - start;
            fclose(fp);
            return true;
        }
    }

    fclose(fp);
    return false;
}

// ------------------------------------------------------------
// 内存扫描（自验证用）
// ------------------------------------------------------------
static uintptr_t ResolveSignature(const ModuleInfo& mod, const char* sig) {
    std::vector<int> pattern;
    const char* p = sig;
    while (*p) {
        if (*p == ' ') { p++; continue; }
        if (*p == '?') { pattern.push_back(-1); p++; if (*p == '?') p++; continue; }
        pattern.push_back(strtol(p, nullptr, 16));
        p += 2;
    }

    if (mod.size < pattern.size()) return 0;
    uint8_t* base = (uint8_t*)mod.base;
    for (size_t i = 0; i <= mod.size - pattern.size(); i += 4) {
        bool found = true;
        for (size_t j = 0; j < pattern.size(); j++) {
            if (pattern[j] != -1 && base[i + j] != (uint8_t)pattern[j]) {
                found = false;
                break;
            }
        }
        if (found) return (uintptr_t)(base + i);
    }
    return 0;
}

// ------------------------------------------------------------
// 自验证 Hook 安装
// ------------------------------------------------------------
static volatile bool g_hookValid = false;

template<typename OrigPtr>
static bool tryHookGroup(const ModuleInfo& mod, const std::vector<const char*>& sigs,
                         void* detour, OrigPtr** orig_out,
                         const char* label) {
    for (size_t i = 0; i < sigs.size(); i++) {
        LOGI("  Trying %s [%zu/%zu]...", label, i + 1, sigs.size());
        uintptr_t addr = ResolveSignature(mod, sigs[i]);
        if (addr == 0) {
            LOGI("  %s [%zu] -> not found.", label, i + 1);
            continue;
        }

        // 重置自验证标志
        g_hookValid = false;

        // 使用DobbyHook进行Hook
        if (DobbyHook((void*)addr, detour, (void**)orig_out) != 0) {
            LOGE("  DobbyHook failed at 0x%lx", addr);
            continue;
        }

        LOGI("  DobbyHook installed. Triggering validation...");

        // 等待验证
        for (int w = 0; w < 20; w++) {
            if (g_hookValid) break;
            usleep(100 * 1000);
        }

        if (g_hookValid) {
            LOGI("  %s [%zu] -> VALID hook! Confirmed correct function.", label, i + 1);
            return true;
        }

        // 卸载错误的Hook
        LOGE("  %s [%zu] -> INVALID hook (validation failed). Unhooking and trying next...", label, i + 1);
        DobbyDestroy((void*)addr);
        *orig_out = nullptr;
    }
    return false;
}

// ------------------------------------------------------------
// 平台特定代码（保持原有逻辑）
// ------------------------------------------------------------
#if __arm__ || __aarch64__
JNIEnv *env = nullptr;

#define LOGI_OLD(...) __android_log_print(ANDROID_LOG_INFO, "LeviLogger", __VA_ARGS__)

jobject getGlobalContext(JNIEnv *env) {
    jclass activity_thread = env->FindClass("android/app/ActivityThread");
    jmethodID current_activity_thread = env->GetStaticMethodID(activity_thread, "currentActivityThread", "()Landroid/app/ActivityThread;");
    jobject at = env->CallStaticObjectMethod(activity_thread, current_activity_thread);
    jmethodID get_application = env->GetMethodID(activity_thread, "getApplication", "()Landroid/app/Application;");
    jobject context = env->CallObjectMethod(at, get_application);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return context;
}

std::string getAbsolutePath(JNIEnv *env, jobject file) {
    jclass file_class = env->GetObjectClass(file);
    jmethodID get_abs_path = env->GetMethodID(file_class, "getAbsolutePath", "()Ljava/lang/String;");
    auto jstr = (jstring)env->CallObjectMethod(file, get_abs_path);
    if (env->ExceptionCheck()) env->ExceptionClear();
    const char *cstr = env->GetStringUTFChars(jstr, nullptr);
    std::string result(cstr);
    env->ReleaseStringUTFChars(jstr, cstr);
    return result;
}

std::string getPackageName(JNIEnv *env, jobject context) {
    jclass context_class = env->GetObjectClass(context);
    jmethodID get_pkg_name = env->GetMethodID(context_class, "getPackageName", "()Ljava/lang/String;");
    auto jstr = (jstring)env->CallObjectMethod(context, get_pkg_name);
    if (env->ExceptionCheck()) env->ExceptionClear();
    const char *cstr = env->GetStringUTFChars(jstr, nullptr);
    std::string result(cstr);
    env->ReleaseStringUTFChars(jstr, cstr);
    return result;
}

std::string getInternalStoragePath(JNIEnv *env) {
    jclass env_class = env->FindClass("android/os/Environment");
    jmethodID get_storage_dir = env->GetStaticMethodID(env_class, "getExternalStorageDirectory", "()Ljava/io/File;");
    jobject storage_dir = env->CallStaticObjectMethod(env_class, get_storage_dir);
    return getAbsolutePath(env, storage_dir);
}

std::string GetModsFilesPath(JNIEnv *env) {
    jobject app_context = getGlobalContext(env);
    if (!app_context) return "";
    auto package_name = getPackageName(env, app_context);
    for (auto &c : package_name) c = tolower(c);
    return (fs::path(getInternalStoragePath(env)) / "Android" / "data" / package_name / "mods");
}
#endif

// ------------------------------------------------------------
// 崩溃信号处理
// ------------------------------------------------------------
static struct sigaction g_oldSigSegv;
static struct sigaction g_oldSigAbrt;
static struct sigaction g_oldSigBus;

static const char* sigToStr(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (Segmentation Fault)";
        case SIGABRT: return "SIGABRT (Abort)";
        case SIGBUS:  return "SIGBUS (Bus Error)";
        default:      return "Unknown Signal";
    }
}

static void crashSignalHandler(int sig, siginfo_t* info, void* ucontext) {
    std::string logPath = getConfigDir() + "crash_native.log";
    int fd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        char buf[1024];
        int len;
        len = snprintf(buf, sizeof(buf),
            "\n========================================\n"
            "[NATIVE CRASH] %s\n"
            "Fault address: %p\n"
            "Signal code:   %d\n"
            "Process PID:   %d\n"
            "Timestamp:     %ld\n"
            "========================================\n",
            sigToStr(sig), info->si_addr, info->si_code, getpid(), (long)time(nullptr));
        write(fd, buf, len);
        close(fd);
    }
    
    // 恢复原始信号处理器
    switch (sig) {
        case SIGSEGV: sigaction(SIGSEGV, &g_oldSigSegv, nullptr); break;
        case SIGABRT: sigaction(SIGABRT, &g_oldSigAbrt, nullptr); break;
        case SIGBUS:  sigaction(SIGBUS, &g_oldSigBus, nullptr); break;
    }
    raise(sig);
}

static void installCrashHandlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crashSignalHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    
    sigaction(SIGSEGV, &sa, &g_oldSigSegv);
    sigaction(SIGABRT, &sa, &g_oldSigAbrt);
    sigaction(SIGBUS, &sa, &g_oldSigBus);
    LOGI("Native crash signal handlers installed.");
}

// ------------------------------------------------------------
// Logcat 抓取线程
// ------------------------------------------------------------
static void* LogcatCaptureThread(void*) {
    LOGI("Logcat snapshot thread started.");
    std::string logPath = getConfigDir() + "minecraft_runtime.log";
    FILE* clear = fopen(logPath.c_str(), "w");
    if (clear) fclose(clear);

    pid_t pid = getpid();
    char pidStr[32];
    snprintf(pidStr, sizeof(pidStr), "%d", pid);

    std::string lastTimestamp;
    for (int i = 0; i < 1500; i++) { // 最多 50 分钟
        int pipefd[2];
        if (pipe(pipefd) != 0) break;

        pid_t child = fork();
        if (child == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            if (lastTimestamp.empty()) {
                execl("/system/bin/logcat", "logcat", "-d", "-v", "threadtime", "-t", "200", "--pid", pidStr, "*:V", (char*)nullptr);
            } else {
                execl("/system/bin/logcat", "logcat", "-d", "-v", "threadtime", "-T", lastTimestamp.c_str(), "--pid", pidStr, "*:V", (char*)nullptr);
            }
            _exit(127);
        }

        close(pipefd[1]);
        FILE* logFile = fopen(logPath.c_str(), "a");
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
            char* line = buf;
            while (char* nl = strchr(line, '\n')) {
                *nl = '\0';
                lastLine = line;
                line = nl + 1;
            }
            if (*line != '\0') lastLine = line;
        }

        fclose(logFile);
        close(pipefd[0]);
        int status;
        waitpid(child, &status, 0);

        if (!lastLine.empty() && lastLine.size() > 18) {
            std::string candidate = lastLine.substr(0, 18);
            if (candidate[2] == '-' && candidate[5] == ' ' && candidate[8] == ':') {
                lastTimestamp = candidate;
            }
        }
        usleep(2000 * 1000);
    }
    LOGI("Logcat snapshot thread stopped.");
    return nullptr;
}

// ------------------------------------------------------------
// 配置系统（不含默认配置创建）
// ------------------------------------------------------------
static std::string trimAscii(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

static std::optional<bool> parseBoolString(std::string value) {
    value = trimAscii(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "true" || value == "1" || value == "yes" || value == "on" || value == "enabled") return true;
    if (value == "false" || value == "0" || value == "no" || value == "off" || value == "disabled") return false;
    return std::nullopt;
}

static std::optional<bool> readBoolLike(const Json& value) {
    try {
        if (value.is_boolean())         return value.get<bool>();
        if (value.is_number_integer())  return value.get<long long>() != 0;
        if (value.is_number_unsigned()) return value.get<unsigned long long>() != 0;
        if (value.is_number_float())    return value.get<double>() != 0.0;
        if (value.is_string())          return parseBoolString(value.get<std::string>());
        if (value.is_object()) {
            for (std::string_view key : {"value", "enabled", "enable", "state", "default"}) {
                auto it = value.find(std::string(key));
                if (it != value.end()) { if (auto parsed = readBoolLike(*it)) return parsed; }
            }
            auto disabled = value.find("disabled");
            if (disabled != value.end()) { if (auto parsed = readBoolLike(*disabled)) return !*parsed; }
        }
    } catch (...) {}
    return std::nullopt;
}

// ------------------------------------------------------------
// 应用配置（带 Sanity Check）
// ------------------------------------------------------------
static void applyConfig(OreUi& ore_ui, const char* label) {
    LOGI("[%s] applyConfig called! mConfigs.size()=%zu &ore_ui=%p",
         label, ore_ui.mConfigs.size(), &ore_ui);

    size_t map_size = ore_ui.mConfigs.size();
    if (map_size == 0) {
        LOGI("[%s] mConfigs empty (size 0). Skipping.", label);
        return;
    }
    if (map_size > 2000) {
        LOGE("[%s] Sanity FAILED! mConfigs size = %zu (garbage). Wrong function hooked!", label, map_size);
        return;
    }

    g_hookValid = true;
    LOGI("[%s] Valid! Found %zu OreUI configs.", label, map_size);

    std::string configPath = getConfigDir() + "config.json";
    Json config;
    
    if (fs::exists(configPath)) {
        try {
            std::ifstream inFile(configPath);
            inFile >> config;
        } catch (...) {
            config = Json::object();
        }
    } else {
        config = Json::object();
    }

    bool dirty = false;
    for (auto& [name, oreConfig] : ore_ui.mConfigs) {
        LOGI("[%s] -> %s", label, name.c_str());

        bool value = false;
        if (config.contains(name) && config[name].is_boolean()) {
            value = config[name];
        } else {
            config[name] = false;
            dirty = true;
        }

        if (!value) {
            oreConfig.mUnknown3 = []() { return false; };
            oreConfig.mUnknown4 = []() { return false; };
            LOGI("[%s]   -> Disabled: %s", label, name.c_str());
        } else {
            LOGI("[%s]   -> Kept OreUI: %s", label, name.c_str());
        }
    }

    if (dirty) {
        try {
            fs::create_directories(fs::path(configPath).parent_path());
            FILE* f = std::fopen(configPath.c_str(), "w");
            if (f) {
                std::string jsonStr = config.dump(4);
                std::fwrite(jsonStr.data(), 1, jsonStr.size(), f);
                std::fclose(f);
                LOGI("[%s] Config saved.", label);
            }
        } catch (...) {
            LOGE("[%s] Failed to save config.", label);
        }
    }
}

// ------------------------------------------------------------
// Hook 回调（保持原有平台兼容性）
// ------------------------------------------------------------
#if __arm__ || __aarch64__
// JNI 初始化 Hook
SKY_AUTO_STATIC_HOOK(
    Hook1, memory::HookPriority::Normal,
    std::initializer_list<const char *>(
        {"? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? "
         "91 ? ? ? D5 ? ? ? F9 ? ? ? F8 ? ? ? 39 ? ? ? 34 ? ? ? 12"}),
    int, void *_this, JavaVM *vm) {
    vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_4);
    return origin(_this, vm);
}
#endif

// OreUI Hook（带自验证）
SKY_AUTO_STATIC_HOOK(Hook2, memory::HookPriority::Normal, OREUI_PATTERN, void,
                     void *a1, void *a2, void *a3, void *a4, void *a5, void *a6,
                     void *a7, void *a8, void *a9, OreUi &a10, void *a11) {
    
    origin(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    
    try {
        applyConfig(a10, "V10");
    } catch (...) {
        LOGE("[V10] exception in applyConfig");
    }
}

// ------------------------------------------------------------
// getConfigDir 和 getPackageName（平台特定实现）
// ------------------------------------------------------------
#if defined(_WIN32)
static std::string getMinecraftModsPath() {
    char appDataPath[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
        return "";
    }
    return std::string(appDataPath) + "\\Minecraft Bedrock\\mods";
}

static std::string getUWPModsDir() {
    return getMinecraftModsPath() + "\\ForceCloseOreUI\\";
}
#endif

static bool testDirWritable(const std::string &dir) {
    std::error_code _;
    std::filesystem::create_directories(dir, _);
    std::string testFile = dir + "._perm_test";
    std::ofstream ofs(testFile);
    bool ok = ofs.is_open();
    ofs.close();
    if (ok) std::filesystem::remove(testFile, _);
    return ok;
}

static std::string getConfigDir() {
#if defined(_WIN32)
    std::string primary = "mods/ForceCloseOreUI/";
    std::string fallback = getUWPModsDir();
    if (testDirWritable(fallback)) return fallback;
    return primary;
#else
    std::string primary = "/storage/emulated/0/Android/data/com.mojang.minecraftpe/mods/";
    if (!primary.empty()) {
        primary += "/ForceCloseOreUI/";
        if (testDirWritable(primary)) return primary;
    }
    if (!env) return primary;
    std::string base = GetModsFilesPath(env);
    if (!base.empty()) {
        base += "/ForceCloseOreUI/";
        if (testDirWritable(base)) return base;
    }
    return primary;
#endif
}

// ------------------------------------------------------------
// 注入线程（自验证Hook安装）
// ------------------------------------------------------------
static void* InjectionThread(void*) {
    LOGI("=== Background thread STARTED (tid=%d) ===", gettid());
    
    const int MAX_WAIT = 30000, POLL = 100;
    int waited = 0;
    ModuleInfo mod;
    
    while (waited < MAX_WAIT) {
        if (findMinecraftSegment(mod)) {
            LOGI("Engine found at 0x%lx (%zu bytes) after %d ms", mod.base, mod.size, waited);
            break;
        }
        if (waited % 1000 == 0) LOGI("Waiting... (%d ms)", waited);
        usleep(POLL * 1000);
        waited += POLL;
    }
    
    if (!mod.base) {
        LOGE("Engine not found after %d ms.", MAX_WAIT);
        return nullptr;
    }
    
    // 这里可以添加自验证Hook安装逻辑
    // 但由于SKY_AUTO_STATIC_HOOK已经处理了Hook，这里主要用于初始化
    LOGI("Module found, hooks should be active.");
    return nullptr;
}

// ------------------------------------------------------------
// 构造函数（初始化入口）
// ------------------------------------------------------------
__attribute__((constructor))
static void ForceCloseOreUI_Init() {
    std::string logPath = getConfigDir() + "debug.log";
    FILE* file = fopen(logPath.c_str(), "w");
    if (file) fclose(file);
    
    LOGI("=== ForceCloseOreUI Mod Started ===");
    LOGI("Config dir: %s", getConfigDir().c_str());
    
    // 安装崩溃信号处理器
    installCrashHandlers();
    
    // 启动logcat抓取线程
    {
        pthread_t logcat_thread;
        int ret = pthread_create(&logcat_thread, nullptr, LogcatCaptureThread, nullptr);
        if (ret != 0) LOGE("Logcat thread create failed: %d", ret);
        else pthread_detach(logcat_thread);
    }
    
    // 启动注入线程（如果需要自验证Hook）
    {
        pthread_t thread;
        int ret = pthread_create(&thread, nullptr, InjectionThread, nullptr);
        if (ret != 0) LOGE("pthread_create failed: %d", ret);
        else pthread_detach(thread);
    }
}