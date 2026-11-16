# samples

## opaque_error_demo.cpp

纯 C++17，**无 UE 依赖**。把 Hare `errors::opaque_` 的两条核心手法搬到 C++ 里做最小验证：

1. 24 字节内联存储（对应 `opaque_data = [3]u64`）+ `static_assert` 卡容量；
2. 用 `strerror` 函数指针的**唯一地址当类型标签**，`TryUnwrap` 第一步就做标签比较，不匹配直接 fail-closed。

两个「驱动」：`postgresql`（5 字符 SQLSTATE）与 `sqlite`（int 结果码），互不认识，公共侧只认 `err::OpaqueError`。

### 编译运行

```bat
build_and_run.bat
```

或用任意 C++17 编译器：

```bash
g++ -std=c++17 -O2 -Wall opaque_error_demo.cpp -o demo && ./demo
```

> MSVC 必须加 `/utf-8`（源码含中文，默认按代码页 936 读会报 C2001）。

### 已验证输出（MSVC 19.x / `/W4` 无警告）

```
[统一打印] postgresql : 23505
[统一打印] sqlite     : SQLITE_CONSTRAINT (19)

[类型恢复] pg -> 拿到 SQLSTATE 23505（唯一约束冲突，可跳过重试）
[类型恢复] sqlite 的 19 字节被拒绝解包成 ServerError（标签不匹配，fail-closed）
[类型恢复] sqlite -> 拿到 Code 19
[类型恢复] postgresql 的 SQLSTATE 被拒绝解包成 SqliteError

[反例] 不做标签检查强转 -> 读出 19,0,0,0,0（垃圾数据，正是 Hare 警告的误读）
```

### 和 UE 版的差异（为什么 UE 篇不用这个形态）

| | 本示例 | UE 版 `FImportError` |
|---|---|---|
| 存储 | 24 字节内联，零分配 | `TSharedPtr<FErrorDetailBase>`，一次堆分配 |
| 原因 | 错误码是 POD，能 `memcpy` | 细节含 `FString` 等非平凡类型，不能内联 `memcpy` |
| 标签 | 函数指针 | 函数指针（+ 建议加 `FName TypeId` 以支持持久化） |

UE 篇见 [../02-ue-opaque-error.md](../02-ue-opaque-error.md)。
