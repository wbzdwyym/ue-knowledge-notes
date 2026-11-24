# C++ 线程安全读写分离：RCU 与双缓冲模式

## 问题：return 的撕裂与数据竞争

```cpp
class MyClass {
    LargeData Data;  // 成员变量
public:
    LargeData GetData() const { return Data; }
};
```

当线程 1 调用 `GetData()` 读取 `Data`，同时线程 2 写入 `Data` 时：

- **小类型（int/float/指针 ≤ 8 字节）**：读写在 x64 上是单条指令，不会撕裂，但仍是 data race（编译器可能做危险优化）
- **中等类型（8~16 字节，如 Pair/PackedVec）**：需要两条 mov 指令，撕裂风险——读到的前半是旧值、后半是新值
- **大类型（struct/class > 16 字节）**：对应 memcpy / n 次 mov，撕裂是必然的

**解决思路**：让读者要么拿到一份 **完整的不可变快照**，要么直接 **持有稳定的指针** 直到用完。

---

## 方案一：std::atomic（适合小类型，≤ 8 字节）

```cpp
class MyClass {
    std::atomic<int> Counter{0};
public:
    int GetCount() const {
        return Counter.load(std::memory_order_acquire);
    }
    void SetCount(int v) {
        Counter.store(v, std::memory_order_release);
    }
};
```

| 优点 | 缺点 |
|------|------|
| 零额外分配 | 只适合 ≤ 8 字节的 POD 类型 |
| 硬件级别保证 | 不适合结构体或数组 |
| 无锁、无阻塞 | `memory_order` 用错会引入隐蔽 bug |

> **UE 对应**：`FThreadSafeCounter`、`TAtomic<T>`

---

## 方案二：shared_ptr<const T> — 用户态 RCU

### 核心思路

让读者持有一个指向 **不可变快照** 的共享指针。写者创建新版本，原子替换指针。旧版本在最后一个读者释放后自动销毁。

### 基础版

```cpp
#include <memory>
#include <shared_mutex>

template<typename T>
class RcuData {
    std::shared_ptr<const T> Data;       // 当前版本（不可变）
    std::shared_mutex WriteMutex;        // 写者之间互斥

public:
    explicit RcuData(T Initial)
        : Data(std::make_shared<const T>(std::move(Initial))) {}

    // ── 读者 ──
    // 拿到当前版本的一个 shared_ptr，用完即弃。
    // 读者之间不互斥，读者与写者也不互斥。
    std::shared_ptr<const T> Read() const {
        return std::atomic_load(&Data);
    }

    // ── 写者 ──
    // 拷贝当前版本 → 修改 → 原子替换
    template<typename Fn>
    void Write(Fn&& Updater) {
        std::unique_lock lock(WriteMutex);

        auto Old = std::atomic_load(&Data);
        auto New = std::make_shared<T>(*Old);         // 拷贝
        std::forward<Fn>(Updater)(*New);              // 修改
        std::atomic_store(&Data, std::move(New));     // 原子替换
        // 旧数据在最后一个 reader 释放后自动 delete
    }

    // ── 就地更新（知道没有读者时使用）──
    template<typename Fn>
    void UpdateDirect(Fn&& Updater) {
        std::unique_lock lock(WriteMutex);
        auto New = std::make_shared<T>(*Data);
        std::forward<Fn>(Updater)(*New);
        std::atomic_store(&Data, std::move(New));
    }
};
```

### 使用示例

```cpp
RcuData<FPlayerState> PlayerState{{}};

// 线程 A（读者）
auto Snapshot = PlayerState.Read();
DrawHealthBar(Snapshot->Health);   // Snapshot 指向的数据不会被任何写者修改

// 线程 B（写者）
PlayerState.Write([](FPlayerState& State) {
    State.Health -= 10;
});
```

### 内存模型

```
时间线 →
                                                        写者创建新版本
                                                              │
读者 1: sp1 ───────────────────────────────────────────┐       │
                                                       │       ▼
读者 2: sp2 ─────────────────────────────────────┐     │   ┌──────────┐
                                                 │     │   │ 新数据 v2 │
读者 3: sp3 ─────────────┐                       │     │   └──────────┘
                         │                       │     │
                     ┌──────┐              ┌──────┐   │
                     │ v1   │  ← sp1 持有  │ v1   │   │
                     └──────┘              └──────┘   │
                        ↑                      ↑      │
                     sp2 持有               sp3 持有  │
                                                      │
                    sp3 释放 v1 ──────→ v1 引用为 0 → delete
```

| 优点 | 缺点 |
|------|------|
| 读者零等待、零阻塞 | 每次写都要分配（`make_shared`） |
| 天然无撕裂 | 读到的可能不是最新版本（最终一致） |
| 适合读极多、写极少 | 引用计数 atomic 操作有微小开销 |
| 自动内存管理 | 当读者长期持有旧版本时，写压力大会堆积旧数据 |

### 性能特征

```
场景             延迟（读）        延迟（写）         内存分配
──────────────────────────────────────────────────────────────
读 99%, 写 1%    ~3 ns             ~50-200 ns        每次写 1 次
读 50%, 写 50%   ~3 ns             ~50-200 ns        每次写 1 次
```

> **当写频率很高时，建议改用双缓冲或三缓冲**

---

## 方案三：双缓冲（Double Buffering）—— 无锁写者

适用于 **一个写者、多个读者** 且 **写频率高** 的场景。

```cpp
template<typename T>
class DoubleBuffer {
    alignas(64) T Buffers[2];       // 两个缓冲区，避免 false sharing
    std::atomic<int> ReadIndex{0};
    // 写者总是写 1 - ReadIndex

public:
    explicit DoubleBuffer(const T& Initial) {
        Buffers[0] = Initial;
        Buffers[1] = Initial;
    }

    // ── 读者（无锁）──
    const T& Read() const {
        return Buffers[ReadIndex.load(std::memory_order_acquire)];
    }

    // ── 写者（单写者专用）──
    // 1. 获取写缓冲区（使用者可以长时间修改，不影响读者）
    // 2. 修改完毕后 Commit()
    T& GetWriteBuffer() {
        int Current = ReadIndex.load(std::memory_order_relaxed);
        return Buffers[1 - Current];
    }

    void Commit() {
        int NextRead = 1 - ReadIndex.load(std::memory_order_relaxed);
        ReadIndex.store(NextRead, std::memory_order_release);
        // 屏障：所有写缓冲区的写入在此前完成，读者才会看到
    }

    // ── 原子更新（组合 GetWriteBuffer + Commit）──
    template<typename Fn>
    void Update(Fn&& Updater) {
        T& Writable = GetWriteBuffer();
        std::forward<Fn>(Updater)(Writable);
        Commit();
    }
};
```

### 使用示例

```cpp
DoubleBuffer<FParticleState> ParticleState;

// 线程 A（读者）— 每帧渲染
for (auto& Particle : ParticleState.Read().Particles) {
    Render(Particle);
}

// 线程 B（写者）— 每帧模拟
ParticleState.Update([](FParticleState& State) {
    for (auto& P : State.Particles) {
        P.Position += P.Velocity * DeltaTime;
    }
});
```

### 三缓冲（Triple Buffering）

当写者需要 **覆盖旧缓冲区**（比如写者也想读旧数据，或者需要回收内存）时，用三缓冲：

```
[Buffer 0] → 读者在读
[Buffer 1] → 写者在写
[Buffer 2] → 空闲 / 等待回收

提交时：标记 Buffer 1 为"就绪"，读者下次切换到它
回收时：读者不再读的旧缓冲区变为空闲
```

| 优点 | 缺点 |
|------|------|
| 无需动态分配 | 需要预分配缓冲区 |
| 延迟稳定（无 GC/alloc） | 只适合单个写者 |
| 适合高频写 | 写后不能立即保证所有读者看到新版本 |

---

## 方案四：FRWLock / shared_mutex（强一致性）

当读者必须读到 **最新版本** 时，用读写锁：

```cpp
class SharedData {
    mutable FRWLock Lock;
    TArray<FVector> Data;
public:
    void ReadData() const {
        FRWScopeLock ReadLock(Lock, SLT_ReadOnly);
        // 多个读者可以同时进入
        Use(Data);
    }
    void WriteData() {
        FRWScopeLock WriteLock(Lock, SLT_Write);
        // 写者独占
        Modify(Data);
    }
};
```

| 优点 | 缺点 |
|------|------|
| 读者一定看到最新写入 | 读者之间存在竞争 |
| API 最简单最安全 | 写者阻塞所有读者 |
| 支持多个写者 | 频繁竞争时退化为互斥锁 |

---

## 方案对比总结

| 场景 | 推荐方案 | 读者延迟 | 写者延迟 | 分配 | 一致性模型 |
|------|---------|---------|---------|------|-----------|
| 小整数/布尔/指针（≤ 8b） | `std::atomic<T>` | ~1 ns | ~1 ns | 无 | 顺序一致 |
| 读 > 99%、写 < 1%、大对象 | `shared_ptr<const T>`（RCU） | ~3 ns | ~100 ns | 每次写 | 最终一致（快照） |
| 高频写、单写者、大对象 | 双缓冲 | ~3 ns | ~3 ns + 拷贝 | 无 | 最终一致（延迟切换） |
| 渲染管线（多生产者多消费者） | 三缓冲 | ~5 ns | ~5 ns | 偶尔 | 最终一致 |
| 需要强一致性、多写者 | `FRWLock` / `shared_mutex` | ~10-50 ns | 阻塞 | 无 | 线性一致 |
| 纯粹读、从不写 | const + 无锁—无需做任何事 | ~1 ns | — | 无 | — |

---

## Unreal Engine 中的实际模式

### 1. 渲染管的 Double/Triple Buffer

```cpp
// 渲染线程读取，游戏线程写入
struct FRenderData {
    TArray<FScenePrimitive> Primitives;
    FGraphEventRef UpdateFence;
};

// 实际分布在 FScene 和 FRenderer 之间
// 游戏线程调用 BeginUpdate() → 写 Buffer B
// 渲染线程调用 Render() → 读 Buffer A
```

### 2. FSharedBuffer（RCU 风格）

```cpp
// UE5 的 FSharedBuffer 本质是 shared_ptr<const void>
// 适用于大块只读数据的共享
FSharedBuffer Buffer = FSharedBuffer::Clone(DataPtr, Size);
// 多个线程可以持有自己的 FSharedBuffer 视图
```

### 3. 显式 FRWLock 保护

```cpp
// 大量 UE 源码用 FRWLock 保护频繁读、偶尔写的集合
mutable FRWLock ActorLock;
TArray<AActor*> PendingActors;

void AddActor(AActor* Actor) {
    FRWScopeLock Lock(ActorLock, SLT_Write);
    PendingActors.Add(Actor);
}

void ProcessActors() const {
    FRWScopeLock Lock(ActorLock, SLT_ReadOnly);
    for (auto* A : PendingActors) { /* ... */ }
}
```

---

## 选型决策流

```
                              你的数据需要线程安全吗？
                                      │
                          ┌───────────┴───────────┐
                          │ 从不被写               │ 会被多个线程读写
                          │                       │
                     不用做任何事           ┌─────┴─────┐
                          │           数据 ≤ 8 字节   数据 > 8 字节
                          │                │              │
                     ✅ 保持 const        atomic       ┌──┴──┐
                                                    写极少  写频繁
                                                      │      │
                                                  shared_ptr   │
                                                   (RCU)    ┌──┴──┐
                                                           单写者  多写者
                                                             │      │
                                                         双缓冲  FRWLock
```

---

## 附录：shared_ptr<const T> 的 C++20 替代

C++20 提供了 `std::atomic<std::shared_ptr<T>>`，不再需要 `std::atomic_load`/`atomic_store`：

```cpp
class RcuDataCpp20 {
    std::atomic<std::shared_ptr<const T>> Data;
public:
    std::shared_ptr<const T> Read() const {
        return Data.load(std::memory_order_acquire);
    }
    void Write(auto&& Updater) {
        // C++20 支持 atomic 直接操作 shared_ptr
        auto Old = Data.load(std::memory_order_relaxed);
        auto New = std::make_shared<T>(*Old);
        std::forward<decltype(Updater)>(Updater)(*New);
        Data.store(std::move(New), std::memory_order_release);
    }
};
```

> UE5 目前使用 C++17 标准，所以 `std::atomic_load` 接口在 UE 项目中更实用。后续迁移到 C++20 后可以更新。
