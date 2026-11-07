<div align="center">

<h1 align="center">ForceCloseOreUI — 依赖版（preloader）</h1>

<p>通过 <code>libpreloader.so</code> 加载，强制关闭 Android 版 Minecraft 基岩版的 OreUI，<br>
恢复经典 JSON-UI。</p>

<p>
<a href="./README.md">English</a> · <b>中文</b>
</p>

</div>

<br/>

> [!WARNING]
> 非官方 Mod，与 Mojang、微软无关。修改客户端违反 Minecraft 使用条款，账号可能被封。
> 请自行承担风险。

> [!IMPORTANT]
> **这是依赖版。多数人其实想要独立版，或者干脆用 App。**
> - 最省事：一键打包 App —— **<https://github.com/yinghuajimew/yhjmew.no.ore.ui>**
> - 不想带 `libpreloader.so`：**[ForceCloseOreUI-android](https://github.com/yinghuajimew/ForceCloseOreUI-android)**

## 这是什么

一个 `arm64-v8a` 动态库，钩住 Minecraft 基岩版初始化 OreUI 配置的函数，把 `OreUIConfig`
里的每一项强制设为 `false`，游戏于是回退到 JSON-UI。

和独立版不同的是，这个版本要通过 **`libpreloader.so`** 加载，APK 里必须同时带上它。
这是本项目最早的做法，兼容的 Android 版本范围更广，包括较老的系统。

## 运行要求

| 项 | 要求 |
|---|---|
| ABI | 仅 `arm64-v8a` |
| Android | 取决于 `libpreloader.so` 的支持范围 |
| 注入 | `libpreloader.so` **和** 本库都要有 |

> [!NOTE]
> 如果你在 Android 11+、且没有特别理由必须走 preloader，建议用**独立版**——
> 少一个 `.so`，出错的地方也少一个。

## 怎么用

重新打包的 Minecraft APK 里需要两个文件：

```
lib/arm64-v8a/libpreloader.so            # 加载器
lib/arm64-v8a/libForceCloseOreUI.so      # 本库
```

**推荐用一键打包 App** —— <https://github.com/yinghuajimew/yhjmew.no.ore.ui>
可以在里面选 SO 类型，它会自动注入 `libpreloader.so` 和对应库、重新签名并安装。
在 App 的 **SO 设定** 里选 **依赖版 SO**。

如果自己手动重打包：注意换了签名就装不上原版，必须先把官方版卸掉，而卸载会连存档一起删。

配置与日志位置：

```
/storage/emulated/0/Android/data/com.mojang.minecraftpe/mods/ForceCloseOreUI/
├── config.json        # 要关掉哪些项，首次挂钩成功后生成
├── debug.log          # 会话日志（文件通道，本分支新增）
├── crash_native.log   # 原生崩溃现场，附带游戏自身 logcat 快照
└── readme.txt         # 首次运行时写入的目录说明
```

该目录不可写时会回退到 `/storage/emulated/0/games/com.mojang/ForceCloseOreUI/`。

## 这个版本改了什么

本分支**不是**上游的依赖版。挂钩机制没动（仍是 `pl::hook` 静态注册器），
但诊断能力和特征码覆盖补齐到了独立版的水平。

### 1. 真正的诊断，而不是只有 logcat

上游只写 logcat，而进程一死 logcat 就没了——排查崩溃时基本没用。本版补上：

- **`debug.log` 文件通道**——崩溃后仍然保留，事后能翻
- **会话头**记录厂商、型号、Android 版本、SDK、ABI、页大小和构建指纹，
  让一份崩溃报告自带上下文
- **`crash_native.log`**——信号、故障地址，以及崩溃瞬间完整寄存器现场
- **logcat 快照线程**——把游戏自身的 logcat 增量抓进崩溃文件（最长 50 分钟），
  因为真正有价值的信息通常是**游戏**在崩前一瞬间打了什么

### 2. 特征码覆盖扩展到 1.26.60

内置特征码列表现在覆盖 **1.21.90 ~ 1.26.60**，按版本从新到旧依次尝试：

| 版本 |
|---|
| 1.26.60 |
| 1.26.50 |
| 1.26.40 |
| 1.26.30 |
| 1.26.20 |

支持新版本游戏就是在 `src/ForceCloseOreUI.cpp` 的模式列表里加一行。

### 3. 只保留 Android

上游仓库同时还带 Windows 构建。本分支只做 Android —— Windows DLL 单独放在
**[ForceCloseOreUI-windows](https://github.com/yinghuajimew/ForceCloseOreUI-windows)**。

## 已知限制

在部分较新的 Android 版本上（已知 Android 17 / SDK 37、vivo 设备），preloader 解析游戏 ELF
时会失败：日志停在 `Gloss Elfinfo init...`，随后在开始扫描特征码之前就 `SIGABRT`。
这是 **`libpreloader.so` 加载器**的问题，不是本库的问题，本分支也没有修复它。

遇到这种情况请改用**独立版**——它完全不经过 preloader。

## 编译

```sh
xmake f -p android -a arm64-v8a -m release -y
xmake
# -> build/android/arm64-v8a/release/libForceCloseOreUI.so
```

CI（`.github/workflows/build.yml`）会构建并发布产物。

## 致谢

基于 QYCottage 及贡献者的原项目 [ForceCloseOreUI](https://github.com/QYCottage/forceCloseOreUI)。
`pl::hook` 静态注册器与 preloader 来自该项目。
