# 虚幻引擎 C++ 条件编译（宏指令）实战指南

## 1. 核心概念：编译期与运行期

在虚幻引擎（Unreal Engine）的 C++ 开发中，控制「某段代码是否参与构建 / 是否执行」时，要区分两个层面。

### 1.1 常规 `if (Condition)` —— 运行期（Runtime）

| 项目 | 说明 |
|------|------|
| **生效阶段** | 游戏运行期间 |
| **机制** | 分支代码会**完整编译**进最终包体；运行时由 CPU 根据条件选择路径 |
| **特点** | 逻辑灵活，但相关代码始终在二进制中；敏感逻辑若只放在客户端 `if`，仍可能被逆向分析 |

### 1.2 预处理宏 `#if CONDITION` —— 编译期（Compile time）

| 项目 | 说明 |
|------|------|
| **生效阶段** | **编译**期间（预处理阶段） |
| **机制** | 不满足条件的代码块在生成机器码之前被**物理剔除**，不会进入最终包体 |
| **特点** | 可减小包体、避免无关代码参与链接；条件在编译时即确定，**不能**依赖运行时才知的变量值 |

---

## 2. 高频应用场景与代码规范

### 场景一：隔离编辑器专属代码（Editor-Only）

**目的**：仅在 UE 编辑器中编译与使用的逻辑（自定义面板、调试绘制、`PostEditChange` 等），避免打进 Shipping 客户端导致编译失败或包体臃肿。

**规范用法**：

```cpp
#if WITH_EDITOR
    // 仅在编辑器环境下编译
    UE_LOG(LogTemp, Warning, TEXT("此日志仅在编辑器内可见"));
#endif
```

**头文件中的编辑器专属数据成员**：使用 `#if WITH_EDITORONLY_DATA` 包裹，与 `WITH_EDITOR` 的语义区分一致（数据随编辑器构建存在，避免非编辑器目标链接到仅编辑器存在的符号）。

---

### 场景二：隔离调试与非 Shipping 代码（Debug / Non-Shipping）

**目的**：开发/内测阶段保留作弊指令、高开销日志、性能 HUD 等，并保证**不进入**交付给玩家的 Shipping 包。

**规范用法**：

```cpp
#if !UE_BUILD_SHIPPING
    GivePlayerInfiniteHealth();
    GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, TEXT("注意：当前处于无敌调试模式！"));
#endif
```

说明：当前目标**不是** Shipping 时，上述代码才会参与编译；Shipping 构建中整块被剔除。

---

### 场景三：服务器侧逻辑（Server-Only，编译期裁剪）

**目的**：将核心判定（扣血、权威射线、刷怪等）限制在**会编译服务器代码**的目标中，减少客户端包中的敏感实现面（需与网络权威、RPC 设计配合；运行时仍要用 `GetNetMode()` / `HasAuthority()` 等做行为分流）。

**规范用法**：

```cpp
void AMyWeapon::Fire()
{
    // 客户端与服务器都会编译的「表现层」
    PlayFireSound();
    PlayMuzzleFlash();

#if WITH_SERVER_CODE
    FHitResult Hit;
    if (LineTraceSingleByChannel(Hit))
    {
        ApplyDamage(Hit.GetActor());
    }
#endif
}
```

> **说明**：`WITH_SERVER_CODE` 等宏随引擎版本与目标（Dedicated Server / Client 等）可能不同，以当前工程 `Target.cs` 与引擎 `Build.h` 中实际定义为准；团队应统一文档化「本仓库联机构建」启用的宏组合。

---

## 3. 致命报错排查：LNK2019（无法解析的外部符号）

使用条件编译时，最常见严重问题是**链接阶段**报错 **LNK2019**（找不到某成员函数的实现）。

### 3.1 常见原因

- **头文件 `.h` 与源文件 `.cpp` 的宏不一致**：声明包在 `#if WITH_EDITOR` 内，实现却写在宏外（或用了不同宏），导致某一配置下「有声明无定义」或「有定义无声明」。
- **部分翻译单元**看见声明、**另一部分**在链接时期望另一套符号，最终无法解析。

### 3.2 正确示范（声明与实现成对、宏完全对齐）

**MyActor.h**

```cpp
UCLASS()
class AMyActor : public AActor
{
    GENERATED_BODY()

public:
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
```

**MyActor.cpp**

```cpp
#if WITH_EDITOR
void AMyActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    // ...
}
#endif
```

### 3.3 准则（务必遵守）

| 准则 | 说明 |
|------|------|
| **成对** | 凡因宏而**增减**的类成员（函数、静态成员、内联外的实现），`.h` 与 `.cpp` 必须使用**相同**的 `#if / #endif` 条件包裹。 |
| **对齐** | 嵌套 `#if` 时，层次与顺序在两侧文件保持一致，避免 `#else` 分叉不对称。 |
| **第三方/模块边界** | 跨模块暴露的 API 若用宏裁剪，调用方与实现方宏需一致，否则同样会出现 LNK2019 或 ODR 问题。 |

---

## 4. 速查表

| 宏 / 写法 | 典型用途 |
|-----------|----------|
| `WITH_EDITOR` | 仅编辑器编译的代码路径 |
| `WITH_EDITORONLY_DATA` | 头文件中等编辑器专属成员（与 UPROPERTY 等配合） |
| `!UE_BUILD_SHIPPING` | 非 Shipping 保留的调试、作弊、详细日志 |
| `WITH_SERVER_CODE` | 随项目目标编译的服务器侧代码块（以引擎/目标为准） |
| `if` | 运行期分支，代码仍进包 |

---

*文档位置：`动物世界/规则`。若与项目内 `Doc` 或引擎版本官方文档冲突，以引擎源码 `Build.h` 及本仓库构建脚本为准。*
