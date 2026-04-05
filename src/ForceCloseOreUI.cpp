#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <unordered_map>

#include "api/memory/Hook.h"
#include <cstdio>

namespace fs = std::filesystem;
#if _WIN32

#include <shlobj.h>
#include <string>
#include <vector>
#include <windows.h>

#endif

#if __arm__
#include <unistd.h>
extern "C" int __wrap_getpagesize() { return sysconf(_SC_PAGESIZE); }

#endif

#if __arm__ || __aarch64__
#include "jni.h"
#include <android/log.h>

JNIEnv *env = nullptr;

#define LOGI(...)                                                              \
  __android_log_print(ANDROID_LOG_INFO, "LeviLogger", __VA_ARGS__)

jobject getGlobalContext(JNIEnv *env) {
  jclass activity_thread = env->FindClass("android/app/ActivityThread");
  jmethodID current_activity_thread =
      env->GetStaticMethodID(activity_thread, "currentActivityThread",
                             "()Landroid/app/ActivityThread;");
  jobject at =
      env->CallStaticObjectMethod(activity_thread, current_activity_thread);
  jmethodID get_application = env->GetMethodID(
      activity_thread, "getApplication", "()Landroid/app/Application;");
  jobject context = env->CallObjectMethod(at, get_application);
  if (env->ExceptionCheck())
    env->ExceptionClear();
  return context;
}

std::string getAbsolutePath(JNIEnv *env, jobject file) {
  jclass file_class = env->GetObjectClass(file);
  jmethodID get_abs_path =
      env->GetMethodID(file_class, "getAbsolutePath", "()Ljava/lang/String;");
  auto jstr = (jstring)env->CallObjectMethod(file, get_abs_path);
  if (env->ExceptionCheck())
    env->ExceptionClear();
  const char *cstr = env->GetStringUTFChars(jstr, nullptr);
  std::string result(cstr);
  env->ReleaseStringUTFChars(jstr, cstr);
  return result;
}

std::string getPackageName(JNIEnv *env, jobject context) {
  jclass context_class = env->GetObjectClass(context);
  jmethodID get_pkg_name =
      env->GetMethodID(context_class, "getPackageName", "()Ljava/lang/String;");
  auto jstr = (jstring)env->CallObjectMethod(context, get_pkg_name);
  if (env->ExceptionCheck())
    env->ExceptionClear();
  const char *cstr = env->GetStringUTFChars(jstr, nullptr);
  std::string result(cstr);
  env->ReleaseStringUTFChars(jstr, cstr);
  return result;
}

std::string getInternalStoragePath(JNIEnv *env) {
  jclass env_class = env->FindClass("android/os/Environment");
  jmethodID get_storage_dir = env->GetStaticMethodID(
      env_class, "getExternalStorageDirectory", "()Ljava/io/File;");
  jobject storage_dir = env->CallStaticObjectMethod(env_class, get_storage_dir);
  return getAbsolutePath(env, storage_dir);
}

std::string GetModsFilesPath(JNIEnv *env) {
  jobject app_context = getGlobalContext(env);
  if (!app_context) {
    return "";
  }
  auto package_name = getPackageName(env, app_context);
  for (auto &c : package_name)
    c = tolower(c);

  return (fs::path(getInternalStoragePath(env)) / "Android" / "data" /
          package_name / "mods");
}

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
#define OREUI_PATTERN                                                                     \
     std::initializer_list<const char *>({                                                \
      "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 ? ? ? D5 FA 03 00 AA F5 03 07 AA", \
      "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 ? ? ? D5 FB 03 00 AA F5 03 07 AA", \
      "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? F9 ? ? ? D5 FB 03 00 AA ? ? ? F9 F5 03 07 AA", \
      "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 ? ? ? D5 FA 03 00 AA F6 03 07 AA", \
      "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 ? ? ? D5 FA 03 00 AA F5 03 07 AA" \
  })                                                                                                                    \

#elif _WIN32

#include <shlobj.h>
#include <string>
#include <vector>
#include <windows.h>


#define OREUI_PATTERN                                                                                                    \
     std::initializer_list<const char *>({                                                                               \
    "40 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 68 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 ? 49 8B E9 4C 89 44 24 ? 4C 8B EA 48 8B F9 48 89 4C 24", \
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC 18 02 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 49 8B F1 4C 89 44 24", \
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC B8 01 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 49 8B F1 4C 89 44 24", \
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC 98 01 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 4D 8B F1 4C 89 44 24" \
  })                                                                                                 \

 #endif

// clang-format on

namespace {

#if defined(_WIN32)

std::string getMinecraftModsPath() {
  char appDataPath[MAX_PATH];
  if (FAILED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
    printf("Failed to get APPDATA path.\n");
    return "";
  }

  std::string path = std::string(appDataPath) + "\\Minecraft Bedrock\\mods";
  return path;
}

std::string getUWPModsDir() {
  std::string appDataPath = getMinecraftModsPath();
  std::string uwpMods = appDataPath + "\\ForceCloseOreUI\\";
  return uwpMods;
}
#endif

bool testDirWritable(const std::string &dir) {
  std::error_code _;
  std::filesystem::create_directories(dir, _);
  std::string testFile = dir + "._perm_test";
  std::ofstream ofs(testFile);
  bool ok = ofs.is_open();
  ofs.close();
  if (ok)
    std::filesystem::remove(testFile, _);
  return ok;
}

// 读取文件内容为字符串
static std::string readFile(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
}

// 读取快照哈希（上次同步后保存的内容）
static std::string readSnapshot(const std::string &snapshotPath) {
  std::ifstream f(snapshotPath, std::ios::binary);
  if (!f.is_open()) return "";
  return std::string((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
}

// 保存快照
static void writeSnapshot(const std::string &snapshotPath,
                           const std::string &content) {
  std::error_code ec;
  fs::create_directories(fs::path(snapshotPath).parent_path(), ec);
  std::ofstream f(snapshotPath, std::ios::binary);
  f.write(content.data(), content.size());
}

// 同步两个路径的 config.json，哪个有改动就覆盖另一个
// pathA = 新路径（Stivusik），pathB = 旧路径（QYCottage）
void syncConfigPaths(const std::string &pathA, const std::string &pathB) {
  std::error_code ec;
  // 快照保存在新路径目录下
  const std::string snapshotPath =
      fs::path(pathA).parent_path().string() + "/.config_snapshot";

  bool existsA = fs::exists(pathA, ec);
  bool existsB = fs::exists(pathB, ec);

  if (!existsA && !existsB)
    return;

  if (existsA && !existsB) {
    fs::create_directories(fs::path(pathB).parent_path(), ec);
    fs::copy_file(pathA, pathB, fs::copy_options::overwrite_existing, ec);
    writeSnapshot(snapshotPath, readFile(pathA));
    return;
  }

  if (!existsA && existsB) {
    fs::create_directories(fs::path(pathA).parent_path(), ec);
    fs::copy_file(pathB, pathA, fs::copy_options::overwrite_existing, ec);
    writeSnapshot(snapshotPath, readFile(pathB));
    return;
  }

  // 两个都存在
  auto timeA = fs::last_write_time(pathA, ec);
  auto timeB = fs::last_write_time(pathB, ec);

  if (timeA > timeB) {
    // A 更新，A 覆盖 B
    fs::copy_file(pathA, pathB, fs::copy_options::overwrite_existing, ec);
    writeSnapshot(snapshotPath, readFile(pathA));
  } else if (timeB > timeA) {
    // B 更新，B 覆盖 A
    fs::copy_file(pathB, pathA, fs::copy_options::overwrite_existing, ec);
    writeSnapshot(snapshotPath, readFile(pathB));
  } else {
    // 时间相同，读取内容和快照对比，判断哪一侧被修改
    std::string contentA  = readFile(pathA);
    std::string contentB  = readFile(pathB);
    std::string snapshot  = readSnapshot(snapshotPath);

    if (contentA == contentB) {
      // 内容完全一致，无需操作
      return;
    }

    bool aChanged = (contentA != snapshot);
    bool bChanged = (contentB != snapshot);

    if (aChanged && !bChanged) {
      // 只有 A 改了，A 覆盖 B
      fs::copy_file(pathA, pathB, fs::copy_options::overwrite_existing, ec);
      writeSnapshot(snapshotPath, contentA);
    } else if (!aChanged && bChanged) {
      // 只有 B 改了，B 覆盖 A
      fs::copy_file(pathB, pathA, fs::copy_options::overwrite_existing, ec);
      writeSnapshot(snapshotPath, contentB);
    } else {
      // 两侧都改了（冲突），以 A（新路径）为准
      fs::copy_file(pathA, pathB, fs::copy_options::overwrite_existing, ec);
      writeSnapshot(snapshotPath, contentA);
    }
  }
}

std::string getConfigDir() {
#if defined(_WIN32)
  std::string primary = "mods/ForceCloseOreUI/";
  std::string fallback = getUWPModsDir();
  if (testDirWritable(fallback))
    return fallback;
  return primary;
#else
  // 新路径（Stivusik）
  std::string newPath = "/storage/emulated/0/Android/data/com.mojang.minecraftpe/ForceCloseOreUI/";
  // 旧路径（QYCottage 原始）绝对路径
  std::string oldPath = "/storage/emulated/0/games/ForceCloseOreUI/";

  // 同步两个路径的配置文件
  syncConfigPaths(newPath + "config.json", oldPath + "config.json");

  // 优先使用新路径，不可写则回退旧路径
  if (testDirWritable(newPath))
    return newPath;
  if (testDirWritable(oldPath))
    return oldPath;

  if (!env)
    return newPath;
  std::string base = GetModsFilesPath(env);
  if (!base.empty()) {
    base += "/ForceCloseOreUI/";
    if (testDirWritable(base))
      return base;
  }
  return newPath;
#endif
}
nlohmann::json outputJson;
std::string dirPath = "";
std::string filePath = dirPath + "config.json";
bool updated = false;

void saveJson(const std::string &path, const nlohmann::json &j) {
  std::filesystem::create_directories(
      std::filesystem::path(path).parent_path());
  FILE *f = std::fopen(path.c_str(), "w");
  if (!f) {
    throw std::runtime_error(path);
  }
  std::string jsonStr = j.dump(4);
  std::fwrite(jsonStr.data(), 1, jsonStr.size(), f);
  std::fclose(f);
}

SKY_AUTO_STATIC_HOOK(Hook2, memory::HookPriority::Normal, OREUI_PATTERN, void,
                     void *a1, void *a2, void *a3, void *a4, void *a5, void *a6,
                     void *a7, void *a8, void *a9, OreUi &a10, void *a11) {
  dirPath = getConfigDir();
  filePath = dirPath + "config.json";

  if (std::filesystem::exists(filePath)) {
    std::ifstream inFile(filePath);
    inFile >> outputJson;
    inFile.close();
  }

  for (auto &data : a10.mConfigs) {

    bool value = false;
    if (outputJson.contains(data.first) &&
        outputJson[data.first].is_boolean()) {
      value = outputJson[data.first];
    } else {

      outputJson[data.first] = false;
      updated = true;
    }
    data.second.mUnknown3 = [value]() { return value; };
    data.second.mUnknown4 = [value]() { return value; };
  }

  if (updated || !std::filesystem::exists(filePath)) {
    saveJson(filePath, outputJson);
  }

  origin(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
}

} // namespace
