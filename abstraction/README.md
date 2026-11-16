# 抽象：类型擦除与「函数指针当类型标签」

来源文章：[Writing SQL Drivers in Hare](https://blainsmith.com/articles/writing-sql-drivers-in-hare/)（Blain Smith，2026-11-02）

本篇只抽取文章里**抽象接口**那部分，并按「先把机制拆干净 → 再落进 UE」两步走。

## 结论先行

1. Hare 没有 `interface`、没有泛型，所以它用**函数指针表（vtable）+ 首字段同址**实现抽象接口；错误侧用 `errors::opaque_` 做类型擦除：**24 字节内联存储 + 一个 `strerror` 函数指针**。
2. 最值得偷的一招：**`opaque_` 里没有 type id 字段，函数指针的地址就是 type id**。判断「这个错误是不是我这个模块产生的」= 比较 `err.strerror != &opaque_strerror`。
3. 搬进 UE 时**不要到处用**。UE 有三套现成机制各管一层：`Cast<T>()`（UObject 反射）、`UInterface`（行为多态）、`Opaque + Tag`（非 UObject 的跨模块专属数据）。判据表在 [02](./02-ue-opaque-error.md#1-三种机制的分工判据)。
4. 这套设计的真正价值是架构性的：**公共模块不会因为多支持一个插件就必须改自己的头文件**。

## 文件

| 文件 | 内容 |
|---|---|
| [01-hare-vtable-and-opaque.md](./01-hare-vtable-and-opaque.md) | Hare 侧机制拆解：vtable、`*opaque`、24 字节存储、函数指针当标签、两层 match |
| [02-ue-opaque-error.md](./02-ue-opaque-error.md) | UE 侧落地：`FImportError` 完整实现、与 `Cast`/`UInterface`/`FInstancedStruct` 的取舍、四个坑 |
| [samples/](./samples/) | 可编译验证的最小实现（纯 C++17，已跑通），用来证明「标签比较 + fail-closed 解包」真的成立 |

## 一句话记住它

> 擦除的时候必须留下**一个在进程内唯一、且能反查出来源**的东西。
> Hare 用函数指针地址；UE 里可以用 `UScriptStruct*`、`FName TypeId`、或导出的 describe 函数指针。
