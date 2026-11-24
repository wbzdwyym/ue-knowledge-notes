# 八叉树 Tile 切片与 SharedPtr 数据共享设计模式

> 源自 Unreal Engine 导航系统中的 `FNavigationOctree` + `FRecastTileGenerator` 实现
> 涉及文件：`NavigationOctree.h`、`RecastNavMeshGenerator.h/.cpp`、`NavigationRelevantData.h`

---

## 1. 模式概述

### 问题

游戏世界中的导航数据（碰撞几何体、寻路区域、导航链接等）数量巨大且存储于一个全局的**八叉树**（`FNavigationOctree`）中。而寻路网格的生成是按**Tile（瓦片）** 为粒度进行的——每次只生成一小块区域的数据。如果每次生成 Tile 都深度拷贝八叉树中的数据，将导致：

- 大量内存浪费（相同几何数据在内存中存在数十份拷贝）
- 大量 CPU 时间浪费在序列化/拷贝上
- 线程安全问题（Tile 生成可能在后台线程进行）

### 解决方案

Unreal Engine 采用的模式是 **"八叉树存储 + Tile 切片引用"**：

```
  ┌─────────────────────────────────────────────┐
  │              FNavigationOctree              │
  │  (TOctree2<FNavigationOctreeElement, ...>)  │
  │                                              │
  │  八叉树节点 0: [Element A] [Element B] ...   │
  │  八叉树节点 1: [Element C] [Element D] ...   │
  │  八叉树节点 2: [Element E] ...               │
  └─────────────────────────────────────────────┘
            ▲                    ▲
            │  TSharedRef        │  TSharedRef
            ▼                    ▼
  ┌─────────────────┐   ┌─────────────────┐
  │  Tile(0,0)       │   │  Tile(1,0)      │
  │  [Element A.Data]│   │  [Element C.Data]│
  │  [Element C.Data]│   │  [Element E.Data]│
  │  (shared refs)   │   │  (shared refs)   │
  └─────────────────┘   └─────────────────┘
```

- **原始数据仅一份**，存储在 `FNavigationOctree` 的 `FNavigationOctreeElement.Data` 中
- **Tile 生成器（`FRecastTileGenerator`）** 通过空间查询（`FindElementsWithBoundsTest`）获取与 Tile 范围相交的元素
- **仅拷贝共享指针（`TSharedRef<FNavigationRelevantData>`）**，而非数据本身
- `FNavigationRelevantData` 本身继承自 `TSharedFromThis`，支持从 `this` 生成共享指针

---

## 2. 核心实现细节

### 2.1 数据结构链

```cpp
// NavigationOctree.h — 八叉树元素
struct FNavigationOctreeElement
{
    FBoxSphereBounds Bounds;
    TSharedRef<FNavigationRelevantData, ESPMode::ThreadSafe> Data;  // ★ 核心：共享引用
};

// NavigationRelevantData.h — 实际导航数据（可共享）
struct FNavigationRelevantData : public TSharedFromThis<FNavigationRelevantData, ESPMode::ThreadSafe>
{
    TNavStatArray<uint8> CollisionData;        // 碰撞几何体
    TNavStatArray<uint8> VoxelData;            // 体素缓存
    FCompositeNavModifier Modifiers;           // 区域与链接修饰符
    TSharedRef<const FNavigationElement> SourceElement;  // 源元素引用
};

// RecastNavMeshGenerator.h — Tile 生成器
class FRecastTileGenerator : public FNoncopyable, public FGCObject
{
    TNavStatArray<TSharedRef<FNavigationRelevantData, ESPMode::ThreadSafe>> NavigationRelevantData;  // ★ 共享引用数组
};
```

### 2.2 Tile 切片提取流程

```
PrepareGeometrySources()           // ① 从八叉树中查询与本 Tile 相交的元素
    ↓
FindElementsWithBoundsTest(        // ② 八叉树范围查询（O(logN)）
    TileBoundsExpanded,             //    Tile 包围盒 + 边界扩展
    Callback)                       //    回调收集匹配的元素
    ↓
Callback:                          // ③ 将匹配元素的 Data（TSharedRef）存入数组
    NavigationRelevantData.Add(     //    ★ 仅拷贝共享指针，无数据拷贝！
        Element.Data);
    ↓
GatherGeometryFromSources()        // ④ 遍历共享引用数组，处理几何数据
    ↓
    ElementDataRef.Get()            //    解引用共享指针获取实际数据
        .CollisionData / .VoxelData //    使用几何数据进行体素化
```

### 2.3 关键代码（简化）

```cpp
// RecastNavMeshGenerator.cpp
void FRecastTileGenerator::PrepareGeometrySources(
    const FRecastNavMeshGenerator& ParentGenerator, bool bGeometryChanged)
{
    const FNavigationOctree* NavOctree = NavSys->GetNavOctree();
    
    // 按 Tile 包围盒查询八叉树
    NavOctree->FindElementsWithBoundsTest(
        ParentGenerator.GrowBoundingBox(TileBB, false),
        [this](const FNavigationOctreeElement& Element)
        {
            // ★★★ 核心：只拷贝共享指针，不拷贝数据 ★★★
            NavigationRelevantData.Add(Element.Data);
        });
}
```

---

## 3. 灵感来源

### 3.1 数据库的"视图（View）"模式

该模式本质上是数据库领域中**视图（View）**概念的体现：

- `FNavigationOctree` = 物理表（实际存储所有数据）
- `FRecastTileGenerator::NavigationRelevantData` = 视图（基于空间索引查询的结果集，不复制数据）

### 3.2 写时拷贝（Copy-On-Write, COW）

共享指针在这里的行为类似于**写时拷贝**：

- 读操作（Tile 生成过程中引用几何数据）→ 直接通过共享指针访问，不拷贝
- 如果需要在后台修改数据 → 先通过共享指针复制一份再修改

### 3.3 游戏引擎中的 Tile-Based 世界分区

自 2000 年代早期的游戏引擎起，大型开放世界就采用 **Tile/Chunk/Region** 分区策略：

- **Minecraft** 的 Chunk 系统
- **Unreal Engine 3** 的基于八叉树的流式加载（`FStreamingOctree`）
- **Recast/Detour** 寻路库本身的设计（`dtNavMesh` 天然支持 Tile 管理层）
- **Far Cry / CryEngine** 的 terrain tile 系统

这种"大世界 → 八叉树索引 → 按 Tile 范围切片"的分层设计已经成为了开放世界游戏的事实标准。

### 3.4 共享指针（`TSharedRef`/`TSharedPtr`）的线程安全设计

Unreal Engine 的 `TSharedRef` 与 `ESPMode::ThreadSafe` 参数提供了**引用计数的原子操作**，使得共享数据可以安全地在游戏线程（八叉树写入端）和工作线程（Tile 生成端）之间传递。

```cpp
// 使用 ThreadSafe 模式确保跨线程引用计数安全
TSharedRef<FNavigationRelevantData, ESPMode::ThreadSafe>
```

---

## 4. 如此设计的好处

### 4.1 内存效率

| 场景 | 无共享指针 | 有共享指针 |
|------|-----------|-----------|
| 数据在八叉树中 | 1 份 | 1 份 |
| 8 个 Tile 引用该数据 | 9 份拷贝 | 1 份 + 8 个共享引用（~32 字节/个） |
| 大世界（20,000 元素） | 灾难性内存膨胀 | 近乎零额外开销 |

**推论**：在大型开放世界中，一个物体的碰撞几何体（例如一栋建筑）可能被几十个 Tile 引用。使用共享指针避免了数千兆字节的冗余几何数据。

### 4.2 构建性能

- **Tile 生成不阻塞八叉树**：`PrepareGeometrySources` 仅做范围查询 + 指针拷贝，是 O(log N + K) 操作（K 为相交元素数）
- **无序列化开销**：不需要将八叉树数据序列化为中间格式再反序列化到 Tile
- **延迟加载（Lazy Gathering）**：实际几何体素化（昂贵的部分）可以推迟到确实需要时再进行

```cpp
// 懒加载机制：只有 Tile 真正需要几何数据时才生成
if (ElementData.IsPendingLazyGeometryGathering()) {
    NavSys.DemandLazyDataGathering(ElementData);
}
```

### 4.3 线程安全

- `TSharedRef<..., ESPMode::ThreadSafe>` 使用**原子引用计数**
- 八叉树的元素可以在游戏线程更新，同时 Tile 生成器在工作线程持有对同一数据的引用
- `FNavigationRelevantData` 继承 `TSharedFromThis`，支持从方法内部安全地创建共享引用

### 4.4 增量更新

当世界中的物体移动或改变时，只需要：

1. 更新八叉树中的 `FNavigationOctreeElement`（更新 Bounds 和 Data 引用）
2. 标记受影响的 Tile 为脏（Dirty）
3. Tile 重新生成时，通过共享指针自动获取最新数据

无需手动管理 Tile 与原始数据之间的一致性。

### 4.5 数据去重

相同的 `FNavigationElement`（例如一个静态 Mesh 的多个实例）在八叉树中可能对应对个 `FNavigationOctreeElement`，但它们的 `Data->SourceElement` 指向同一个底层的 `FNavigationElement`——通过 `TSharedRef<const FNavigationElement>` 实现进一步的去重。

---

## 5. 何时应该使用此模式

### 5.1 适用条件

| 条件 | 说明 | 举例 |
|------|------|------|
| ① **大型数据集 + 空间分区** | 数据按空间组织，查询经常只涉及局部区域 | 世界场景、导航数据、物理碰撞 |
| ② **读多写少** | 数据被频繁引用但很少修改 | 寻路网格、静态碰撞、光照贴图 |
| ③ **数据体积大** | 单份数据大到拷贝成本不可忽视 | 网格几何体、体素数据、点云 |
| ④ **分片处理** | 工作被划分为独立的小块并行处理 | Tile 生成、LOD 烘焙、流式加载 |
| ⑤ **跨线程访问** | 数据需要从多个线程访问 | 后台生成 + 主线程更新 |

### 5.2 适用形态

这个模式有以下几种常见变体（按共享粒度递增）：

| 级别 | 名称 | 描述 | UE 示例 |
|------|------|------|---------|
| L1 | **只读 View** | 通过空间查询获取元素引用数组，不拷贝 | `FNavigationOctree` → `FRecastTileGenerator` |
| L2 | **共享状态** | 多个消费者共享同一份可变数据，通过原子操作同步 | `TSharedRef<ThreadSafe>` 跨线程传递 |
| L3 | **写时拷贝** | 共享直到需要修改时复制 | `CopyPtrBeforeWrite` 模式 |
| L4 | **增量 Patch** | 共享基础数据 + 只传递变化部分 | `NavigationData.ChunkDelta` |

### 5.3 不适用的情况

- **数据非常小**（sizeof < 64 bytes）：拷贝比引用开销更低，且指针追踪增加缓存未命中
- **每个 Tile 的数据几乎总是唯一的**：共享率低时收益甚微甚至为负
- **数据频繁原地修改**：共享数据的变更需要通知所有持有者，增加复杂度
- **需要独立序列化/持久化每份 Tile**：共享引用使单独序列化变得复杂（需要处理引用图）

### 5.4 适用场景列举

| 场景 | 大集合 | 分区方式 | 共享数据 | UE 示例 |
|------|--------|----------|----------|---------|
| **导航网格生成** | 全局导航八叉树 | Tile 网格 (Recast) | 碰撞几何体 | `FNavigationOctree` + `FRecastTileGenerator` |
| **可见性/遮挡剔除** | 场景图/Portal 图 | 视野锥体 | Mesh 包围体 | `FScene::PrimitiveOctree` |
| **光源与阴影** | 全局光源列表 | Tile / Atlas 分区 | 阴影贴图 | 虚拟阴影贴图 (VSM) |
| **物理碰撞** | 全局物理空间 | Broadphase 网格 | Shape 数据 | `FPhysScene` |
| **流式加载** | Level / World | Streaming Region | Asset 引用 | `FStreamingManager` |
| **GPU 裁剪** | Instance 列表 | Frustum 网格 | Transform 数据 | `FGPUVisibleInstanceManager` |

### 5.5 决策建议

> **当原始数据的体积远大于引用它的句柄（指针/引用），且同一份数据可能被多个消费者需要时，使用共享指针进行"切片"——用空间索引筛选相关数据，用引用计数共享避免拷贝。**
>
> 反过来说：如果每个消费者需要的数据集几乎不重叠（高独享率），或者数据本身很小（拷贝 vs 指针追踪的开销曲线翻转），直接拷贝可能更简单高效。

---

## 6. 与 3-Bit 编码的关系

| 维度 | 3-Bit 子节点编码 | Tile + SharedPtr 切片 |
|------|------------------|----------------------|
| 解决的问题 | 如何高效索引 8 个子节点 | 如何高效从大数据集中提取子集 |
| 核心技巧 | 位运算（bit-level） | 内存共享（reference-level） |
| 性能收益 | CPU 周期级优化 | 内存带宽 + CPU 拷贝回避 |
| 适用层级 | 八叉树节点内部 | 八叉树节点外部：消费者 |

两者是**正交互补**的关系——3-bit 编码优化了**八叉树内部的遍历性能**，而 Tile + SharedPtr 模式优化了**八叉树数据的消费者如何使用这些数据**。在 UE 的 NavigationOctree 中，两者同时被使用。

---

## 7. 扩展阅读

- **Recast/Detour 官方文档**：`dtNavMesh` 的 Tile 管理层设计
- **Gamma et al. "Design Patterns"**：Proxy Pattern（为对象提供替身以控制访问）
- **Unreal Engine `TSharedPtr` / `TSharedRef` 文档**：线程安全的引用计数智能指针
- **数据库索引理论**：空间索引（R-Tree、Grid）对"大集合 + 小切片"查询的支持
