# 规约：编译期裁剪、状态自动恢复、依赖方向、写码原则

这一类不是"技巧"，是**写代码前先定好的规矩**。违反它们的代价通常不在当次编译，而在打包版、在 LNK2019、在某个漏恢复的 bool、在两个月后没人敢改的模块边界上。

## 结论先行

1. **编译期 ≠ 运行期**：`if` 的代码进包体，`#if` 的代码不进包体。想"让客户端拿不到这段逻辑"只能用 `#if`；想"运行时决定走不走"才用 `if`。声明与实现必须**宏完全对齐**，否则就是 LNK2019 的经典来源。
2. **"进入函数改状态、任意退出恢复"** 用 `TGuardValue`，不要手写多条 `if` 里的还原。少一次还原就是一次粘滞状态（技能永久占位这一类的 bug）。
3. **依赖只能是单向的**：插件 → 主工程的方向不允许反过来。反了之后每次改插件都可能要求主工程跟着改，插件的"可摘除"就没了。
4. **先想清楚再写**：假设要写下来、多种解释要列出来、有更简单的方案要说出来、不清楚就停下来问。不确定的地方不要用代码"猜"过去。

## 文件

| 文件 | 原文文件名 | 讲什么 |
|---|---|---|
| [conditional-compilation-macros.md](./conditional-compilation-macros.md) | `C++条件编译宏指南.md` | 编译期 vs 运行期的机制差异；三个高频场景（Editor-Only / Debug 与 Non-Shipping / Server-Only）；LNK2019 的成因与声明-实现宏对齐的正确示范；速查表 |
| [tguardvalue-and-plugin-dependency.md](./tguardvalue-and-plugin-dependency.md) | `TGuardValue与插件依赖规范.md` | `TGuardValue<bool>` 的用途、典型写法、与手写对比、使用建议；插件与主工程的单向依赖方向、目的、落地检查清单 |
| [tradeoffs-coding-principles.md](./tradeoffs-coding-principles.md) | `Tradeoffs_Coding_Principles.md` | 四条原则：Think Before You Code / Simplicity First / Surgical Changes / Goal-Driven Execution（原文英文，短，值得手抄一遍） |

## 三条最常被违反的

| 反模式 | 正确做法 | 出处 |
|---|---|---|
| 敏感逻辑只在客户端用 `if (IsServer)` 挡住 | 用 `#if UE_SERVER` / `WITH_SERVER_CODE` 在编译期剔除 | `conditional-compilation-macros.md` |
| 头文件里声明了 `#if WITH_EDITOR` 的函数，cpp 里没对齐 | 声明与实现的宏必须完全一致，成对出现 | 同上（LNK2019 一节） |
| 插件 `#include` 了主工程的头文件 | 反向：主工程 include 插件；插件只暴露接口/注册点 | `tguardvalue-and-plugin-dependency.md` |

> 这一类的共同点是**代价延迟出现**：`#if` 写错在编辑器和单机上一切正常，只在打包或专用服务器构建时炸；依赖反了在加第一个功能时没人察觉，在加第十个时才动不了。
