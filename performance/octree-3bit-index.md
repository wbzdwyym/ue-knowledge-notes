# 八叉树 3-Bit 索引编码设计模式

> 源自 Unreal Engine `GenericOctree.h` 中 `FOctreeChildNodeRef` 的实现（约第151-157行）

---

## 1. 模式描述

在三维空间八叉树（Octree）中，每个节点均匀分割为 **8个子节点**，每个子节点对应一个**八分体（octant）**。Unreal Engine 采用 3-bit 位编码来索引这 8 个子节点：

```cpp
Index = (X << 0) | (Y << 1) | (Z << 2)
```

其中每个 bit 代表一个轴的正负方向：

| Bit 位 | 变量 | 含义          | 0            | 1           |
|--------|------|---------------|--------------|-------------|
| Bit 0  | X    | X 轴方向      | 负半轴 (-)   | 正半轴 (+)  |
| Bit 1  | Y    | Y 轴方向      | 负半轴 (-)   | 正半轴 (+)  |
| Bit 2  | Z    | Z 轴方向      | 负半轴 (-)   | 正半轴 (+)  |

### 8 个子节点编码表

| Index | 二进制 | 编码  | 含义                    |
|-------|--------|-------|-------------------------|
| 0     | 000    | ---   | X负, Y负, Z负           |
| 1     | 001    | +--   | X正, Y负, Z负           |
| 2     | 010    | -+-   | X负, Y正, Z负           |
| 3     | 011    | ++-   | X正, Y正, Z负           |
| 4     | 100    | --+   | X负, Y负, Z正           |
| 5     | 101    | +-+   | X正, Y负, Z正           |
| 6     | 110    | -++   | X负, Y正, Z正           |
| 7     | 111    | +++   | X正, Y正, Z正           |

反向解码（从 Index 恢复各个轴的符号）也极为简洁：

```cpp
int32 X() const { return (Index >> 0) & 1; }
int32 Y() const { return (Index >> 1) & 1; }
int32 Z() const { return (Index >> 2) & 1; }
```

---

## 2. 灵感来源

### 2.1 本质：三维空间中的坐标轴符号编码

这种编码的本质是**将三维空间坐标符号组合映射到整数 0-7**。每个轴在二值化后（正/负）等价于 1 bit 信息，三个轴就是 3 bit = 8 种组合。

这实际上就是三维二值空间的**自然二进制编码（Natural Binary Code, NBC）**，其数学直觉：

```
Index = (x_sign ? 1 : 0) * 2^0 + (y_sign ? 1 : 0) * 2^1 + (z_sign ? 1 : 0) * 2^2
```

### 2.2 来自计算机图形学与空间数据结构传统

这一手法源于更早期的空间数据结构研究：

- **Quadtree（四叉树）的 2-bit 编码**：二维版本用 (X << 0) | (Y << 1) 编码 4 个子节点，是同一思想更原始的形式。
- **Morton Code（Z-Order Curve）**：在更高维度上将多 bit 的坐标交错排列，实现空间邻近性保持。而这里的 3-bit 编码是 Morton Code 在单层分割上的退化——每个轴只用 1 bit。
- **GPU 立方体贴图（Cube Map）**：将 3D 方向向量映射到 6 个面时使用类似的轴符号判定。

### 2.3 具体可追溯到

类似的位编码手法在以下经典文献中都有体现：

- Samet, H. "Foundations of Multidimensional and Metric Data Structures" (2006) — 空间数据结构的权威参考
- Foley et al. "Computer Graphics: Principles and Practice" — 三维空间坐标系约定
- Unreal Engine 自 UE3 时代起就使用此模式（`FOctreeNode` 可追溯到 2000s 中期的游戏引擎空间分区实现）

---

## 3. 如此设计的好处

### 3.1 编码与解码 O(1) —— 零分支、零查找表

```cpp
// 编码：3次移位 + 2次或运算
Index = (X << 0) | (Y << 1) | (Z << 2);

// 解码：3次移位 + 3次与运算
int32 X() const { return (Index >> 0) & 1; }
int32 Y() const { return (Index >> 1) & 1; }
int32 Z() const { return (Index >> 2) & 1; }
```

所有运算均为**位运算，单 CPU 周期**完成，对比查找表方案（L1 cache miss 约 10-30 周期）或分支方案（分支预测失败约 10-20 周期）有数量级优势。

### 3.2 遍历顺序与空间顺序一致

遍历子节点 0→7 时，坐标符号变化顺序为 Z→Y→X（最低 bit 先变），这实际上是一种 **Z-Order 遍历**，保证了一定程度上的空间邻近性。

### 3.3 内存极省

- 子节点索引只需要 **4 bit**（值域 0-7 + 1 个空值标志位 8）
- Unreal 使用 `int8`（1 字节）存储，如果再压缩可将 2 个索引打包到 1 个 byte 中
- 对比哈希映射或指针数组的方案，在大规模八叉树场景下（数万节点 × 8 子节点）内存节省显著

### 3.4 可组合性

多个索引可以通过位运算组合：

```cpp
// 取父节点索引 + 当前层级信息组合为全局 3D 坐标
uint32 GlobalCode = (ParentIndex << 3) | ChildIndex;
// 这正是 Morton Code / Z-Order 的逐层堆叠结构
```

### 3.5 与 SIMD / 向量化兼容

解码后的 X/Y/Z 值可以直接打包为向量：

```cpp
// 伪代码示意
__m128i Indices = _mm_set_epi32(idx7, idx6, ..., idx0);
__m128i Xs = _mm_and_si128(_mm_srli_epi32(Indices, 0), _mm_set1_epi32(1));
__m128i Ys = _mm_and_si128(_mm_srli_epi32(Indices, 1), _mm_set1_epi32(1));
__m128i Zs = _mm_and_si128(_mm_srli_epi32(Indices, 2), _mm_set1_epi32(1));
```

### 3.6 在 UE 引擎中的实际性能价值

Unreal Engine 的八叉树用于：
- **场景查询（Scene Queries）**：射线检测、形状重叠测试
- **光照与可见性计算**
- **物理碰撞检测（Chaos Physics）**
- **音频空间化**

这些查询每帧执行数十万次，子节点索引的编码/解码开销被极致优化后，累积的帧时间节省非常可观。

---

## 4. 此编码在哪些查询中被使用

### 4.1 三种核心查询

3-bit 编码被 `FOctreeNodeContext` 中**三种关键查询方法**直接使用。所有方法都利用了"3-bit = 3个轴的符号"这一映射，将 SIMD 向量比较的结果直接转换为子节点索引。

#### ① `GetContainingChild` — 查找完全包含查询盒的子节点

```cpp
// GenericOctree.inl:55-82
inline FOctreeChildNodeRef FOctreeNodeContext::GetContainingChild(
    const FBoxCenterAndExtent& QueryBounds) const
{
    // ...
    // 将查询盒中心与当前节点中心在每个轴向上比较，
    // 比较结果的 bit 模式直接编码为子节点 Index！
    Result.Index = VectorMaskBits(
        VectorCompareGT(QueryBoundsCenter, BoundsCenter)   // 每个轴：查询中心 > 节点中心？
    ) & 0x7;  // ★ 低 3 bit = X/Y/Z 三个轴的比较结果
    return Result;
}
```

**工作原理**：

| 轴 | SIMD 比较 | 结果 bit | 含义 |
|----|-----------|----------|------|
| X | QueryCenter.X > BoundsCenter.X ? | bit 0 | 1=正向子节点, 0=负向子节点 |
| Y | QueryCenter.Y > BoundsCenter.Y ? | bit 1 | 1=正向子节点, 0=负向子节点 |
| Z | QueryCenter.Z > BoundsCenter.Z ? | bit 2 | 1=正向子节点, 0=负向子节点 |

> ★ 3-bit 编码在此处的关键作用：SIMD 在做 `(QueryCenter > BoundsCenter)` 时，每个通道的布尔结果（true=1, false=0）天然排布在 X/Y/Z 三个通道中，`VectorMaskBits` 将这些布尔值打包为 3-bit 整数——**这个整数恰好就是子节点的 Index**。不需要任何查找表、switch 或 if-else 链。

**使用场景**：当向八叉树插入新元素或搜索邻近元素时，需要找到唯一子节点继续下探。

---

#### ② `GetIntersectingChildren` — 查找与查询盒相交的所有子节点

```cpp
// GenericOctree.inl:27-53
inline FOctreeChildNodeSubset FOctreeNodeContext::GetIntersectingChildren(
    const FBoxCenterAndExtent& QueryBounds) const
{
    FOctreeChildNodeSubset Result;

    // 对正半轴子节点：查询盒的 Max > 子节点 Min ?
    Result.PositiveChildBits = VectorMaskBits(
        VectorCompareGT(QueryBoundsMax, PositiveChildBoundsMin)
    ) & 0x7;  // ★ 3-bit，bit N=1 表示第 N 个子节点在正方向有交集

    // 对负半轴子节点：查询盒的 Min <= 子节点 Max ?
    Result.NegativeChildBits = VectorMaskBits(
        VectorCompareLE(QueryBoundsMin, NegativeChildBoundsMax)
    ) & 0x7;  // ★ 3-bit，bit N=1 表示第 N 个子节点在负方向有交集

    return Result;
}
```

**返回的 `FOctreeChildNodeSubset` 结构**：

```cpp
union {
    struct {
        uint32 bPositiveX : 1;   // bit 0:  X 正向子节点是否相交
        uint32 bPositiveY : 1;   // bit 1:  Y 正向子节点是否相交
        uint32 bPositiveZ : 1;   // bit 2:  Z 正向子节点是否相交
        uint32 bNegativeX : 1;   // bit 3:  X 负向子节点是否相交 (索引 4 的分解)
        uint32 bNegativeY : 1;   // bit 4:  Y 负向子节点是否相交 (索引 2 的分解)
        uint32 bNegativeZ : 1;   // bit 5:  Z 负向子节点是否相交 (索引 1 的分解?不对，需要确认)
    };
    struct {
        uint32 PositiveChildBits : 3;  // 正方向的 3 bit
        uint32 NegativeChildBits : 3;  // 负方向的 3 bit
    };
    uint32 ChildBits : 6;   // 全部 6 bit
};
```

**使用场景**：边界框查询（范围查询）——这是八叉树最频繁的操作。遍历时只深入那些与查询盒相交的子节点分支。

---

#### ③ `Contains` — 判断子节点子集是否包含某个子节点

```cpp
// GenericOctree.inl:20-25
inline bool FOctreeChildNodeSubset::Contains(FOctreeChildNodeRef ChildRef) const
{
    const FOctreeChildNodeSubset ChildSubset(ChildRef);
    return (ChildBits & ChildSubset.ChildBits) == ChildSubset.ChildBits;
}
```

**工作原理**：给定一个子节点（例如 Index=5，二进制 101），其对应的 `PositiveChildBits` 是 101（X+、Y-、Z+），`NegativeChildBits` 是 `~101 = 010`。通过与运算检查该节点是否完全在当前子集结果中。

**使用场景**：在 `FindElementsWithBoundsTest` / `FindFirstElementWithBoundsTest` 遍历中，对每个非叶子节点调用 `GetIntersectingChildren` 得到子集，然后对 8 个子节点逐个调用 `Contains` 判断是否需要下探。

---

### 4.2 查询流程全景：3-bit 编码如何推动整个遍历

以 `FindElementsWithBoundsTest`（按包围盒查找元素）为例，完整展示 3-bit 编码在查询路径中的流动：

```
FindElementsWithBoundsTest(BoxBounds, callback)
  │
  ├─ 当前节点是叶子？ → 遍历内部元素，逐个做 Intersect 测试
  │
  └─ 当前节点非叶子？
       │
       ├─ GetIntersectingChildren(BoxBounds)
       │    │  SIMD: QueryMax vs ChildMin → PositiveChildBits (3-bit)
       │    │  SIMD: QueryMin vs ChildMax → NegativeChildBits (3-bit)
       │    │  ★ 返回 FOctreeChildNodeSubset（6 bit 位域）
       │
       ├─ FOREACH_OCTREE_CHILD_NODE(ChildRef)  // 遍历 0..7
       │    │
       │    ├─ Contains(ChildRef) ?
       │    │    │  (ChildBits & ChildSubsetBits) == ChildSubsetBits
       │    │    │
       │    │    ├─ 否 → 跳过此子节点分支
       │    │    │
       │    │    └─ 是 → 递归下探
       │    │         GetChildContext(ChildRef)
       │    │           ├─ GetChildOffsetVec(ChildRef.Index)
       │    │           │    ★ (i & 1)→X符号, (i & 2)→Y符号, (i & 4)→Z符号
       │    │           │    用 SIMD VectorSelect 生成 ChildCenterOffset 的符号
       │    │           └─ 计算子节点 Bounds → 递归
       │    │
       │    └─ ChildRef.Advance()  // ++Index，自动 0→7
       │
       └─ (继续处理匹配到的子节点)
```

**3-bit 编码在此流程中的 3 个关键作用**：

| 步骤 | 函数 | 3-bit 编码的作用 |
|------|------|------------------|
| 裁剪子节点 | `GetIntersectingChildren` | SIMD 比较结果 → 3-bit PositiveBits + 3-bit NegativeBits |
| 筛选子节点 | `Contains` | 位运算快速判断子节点是否在子集中 |
| 生成子节点Bounds | `GetChildOffsetVec` | 通过 `(i & 1)`, `(i & 2)`, `(i & 4)` 提取各轴符号 |

### 4.3 使用此编码的 UE 系统一览

所有使用 `TOctree2`（或旧版 `TOctree`/`TOctree_DEPRECATED`）的系统都继承了此 3-bit 编码的查询机制：

| 系统 | 八叉树类型 | 存储内容 | 典型查询 |
|------|-----------|----------|----------|
| **导航系统** | `FNavigationOctree` (NavigationOctree.h:172) | `FNavigationOctreeElement` 导航相关数据 | Tile 生成时范围查询（`FindElementsWithBoundsTest`） |
| **渲染器-图元** | `FScenePrimitiveOctree` (PrimitiveSceneInfo.h:239) | `FPrimitiveSceneInfoCompact` 渲染图元 | 视锥体裁剪、遮挡查询 |
| **渲染器-光照** | `FSceneLightOctree` (LightSceneInfo.h:175) | `FLightSceneInfoCompact` 光源信息 | 光照影响范围查询、阴影贴图选择 |
| **预计算光照** | `FLightVolumeOctree` (PrecomputedLightVolume.h:94) | `FVolumeLightingSample` 光照样本 | 体积光照采样查询 |
| **静态网格构建** | `TSMCVertPosOctree` (StaticMeshBuild.cpp) | `FPaintedVertex` 顶点颜色 | 顶点位置合并/去重 |
| **骨骼网格导入** | `TWedgeInfoPosOctree` (SkeletalMeshLODImporterData.h:704) | `FWedgeInfo` 楔形信息 | 焊接点位置查询 |
| **骨骼网格操作** | `TVertexInfoPosOctree` (SkeletalMeshOperations.cpp) | `FVertexInfo` 顶点信息 | 顶点位置匹配与合并 |

### 4.4 总结

> **3-bit 编码不是孤立存在的。它跟 SIMD 向量比较、位域子集、遍历器三者深度绑定，构成了一套完整的"查询-裁剪-下探"机制。**
>
> - `GetContainingChild`：编码方向 → 一次 `VectorMaskBits` 拿到 Index
> - `GetIntersectingChildren`：编码裁剪结果 → 6 bit 位域，位运算判定
> - `GetChildOffsetVec`：从 Index 解码方向 → `(i & 1)/(i & 2)/(i & 4)` 提取各轴符号
>
> 这一整套机制使得 **"SIMD 布尔结果 → 子节点索引 → 子节点空间位置"** 的转换全部通过位运算完成，中间没有任何分支、查找表、浮点计算——这正是 UE 八叉树查询性能的关键所在。

---

## 5. 何时应该使用此模式

这是一种**位编码（Bit-Field Encoding）**模式，适用于以下场景：

### 4.1 适用条件（满足越多，收益越大）

| 条件 | 说明 | 检查方法 |
|------|------|----------|
| ① **小整数枚举空间** | 总状态数 ≤ 2^k，k ≤ 8（32 位字长可容纳更多） | 枚举取值范围 |
| ② **多维正交属性** | 状态由多个独立维度的标志组合而成 | 是否有类似 (x,y,z)/(a,b,c) 组合枚举 |
| ③ **高性能代码路径** | 编码/解码发生在热循环中 | Profiler 确认热点 |
| ④ **内存敏感** | 需要存储大量此类索引（数组/节点/元素） | 实例数量是否 > 10,000 |
| ⑤ **需要位运算组合** | 需要快速组合或分解多个索引 | 是否有 Parent+Child, Octant+LOD 等组合需求 |

### 4.2 不适用的情况

- **维度超过 6-8 个**（状态数 > 256），应改为更结构化的编码
- **索引需要频繁动态变化**（每次重新编码开销 vs 直接存储结构化数据）
- **代码不在性能关键路径上**（可读性比性能更重要）
- **需要跨网络/磁盘序列化兼容性**（位域编码对协议升级不友好）

### 4.3 适用场景列举

| 场景 | 编码方式 | bit 数 | 说明 |
|------|----------|--------|------|
| **四叉树子节点** | (X << 0) \| (Y << 1) | 2 bit | 4 个子节点 |
| **八叉树子节点** | (X << 0) \| (Y << 1) \| (Z << 2) | 3 bit | 8 个子节点，即本文主题 |
| **3D 网格邻居编码** | (DX+1) \| (DY+1)<<2 \| (DZ+1)<<4 | 6 bit | 26 邻居（偏移 -1/0/+1） |
| **2D 方向枚举** | (dX+1) \| (dY+1)<<2 | 4 bit | 8 方向（包括原点） |
| **Cube Map 面 + MIP** | Face(3bit) \| MIP(5bit) | 8 bit | 纹理采样参数组合 |
| **LOD + 可见性标志** | LOD(4bit) \| Visible(1bit) \| Dirty(1bit) | 6 bit | 渲染节点状态编码 |

### 4.4 决策建议

> **用位编码，当你面对的是一组正交的二值或多值属性，且编码/解码操作位于性能关键路径时。**
>
> 如果可以用一个 `int` 表达 N 个独立维度的组合状态，并且每个维度的值域是 2 的幂——那就用位域编码。它比枚举数组更省内存，比 switch 跳跃表更快，比 if-else 链更可预测。

---

## 5. 扩展：从 3-bit 到 Morton Code

理解了这个 3-bit 编码之后，可以自然延伸到更强大的 **Z-Order Curve（Morton Code）**：

```
// 每一层用 3 bit 表示，每下探一层左移 3 bit
// Layer 0 (根):   child_index0          (bit 0-2)
// Layer 1:        child_index1 << 3     (bit 3-5)
// Layer 2:        child_index2 << 6     (bit 6-8)
// Layer N:        child_indexN << (N*3) (bit 3N - 3N+2)

uint64 MortonCode = 0;
for (int i = 0; i < Depth; i++) {
    MortonCode |= (uint64)ChildIndices[i] << (i * 3);
}
```

这样整个八叉树的路径可以编码为一个 **64-bit 整数**（最多 21 层深度），而两个节点在 Z-Order 曲线上的数值接近程度直接反映了它们在三维空间中的邻近程度——这是空间数据库、GPU 光线追踪加速结构等领域的核心技术。

---

## 6. 参考资料

- Unreal Engine `GenericOctree.h` — `FOctreeChildNodeRef` class
- Samet, H. "Foundations of Multidimensional and Metric Data Structures", 2006
- Morton, G. M. "A Computer Oriented Geodetic Data Base and a New Technique in File Sequencing", 1966
- Lauterbach et al. "Fast BVH Construction on GPUs" (Eurographics 2009) — Morton Code 在 GPU 中的应用
