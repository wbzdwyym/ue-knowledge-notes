---
tags:
  - Zoira/架构
  - 性能优化
  - 设计模式
aliases:
  - 解耦轮询与位数组过滤模式
  - 位数组权威列表模式
created: 2026-04-24
status: stable
---

# 架构指南：解耦轮询与位数组过滤模式 (Bit-Array Authoritative Roster)

> [!note] 灵感来源
> 本模式深受 UE5 Iris 网络系统的 `FNetBitArray` 与 `UpdateDataStreams` 架构启发。它通过将“混沌的底层状态”与“极速的逻辑处理”进行物理隔离，实现了极高的 CPU 吞吐量与代码整洁度。

---

## 一、 适用场景

当你的系统中存在以下情况时，应考虑套用此模式：
1.  **大规模对象管理**：需要每帧处理数千个对象（如 NPC、网络连接、动态 UI 元素）。
2.  **状态判定成本高/噪音大**：底层状态判定（如是否在视野内、是否超时、是否准备就绪）逻辑复杂，且伴随抖动。
3.  **高性能计算需求**：核心业务逻辑（Processing）需要在极短时间内对所有“有效”对象全速运行，不能容忍在热循环中写 `if` 判断。

---

## 二、 模式核心：双层隔离架构

### 层级 1：底层状态守望者 (Low-Level Guard) —— 处理“混沌”
*   **职责**：负责所有耗时、多分支、带延迟或防抖的逻辑。
*   **产出**：它不直接执行业务，而是维护一份极简的 **“权威花名册” (Authoritative Roster)**。
*   **数据结构**：推荐使用 `FBitArray` (或 UE 包装的 `FNetBitArray`)。1 表示有效，0 表示无效。

### 层级 2：业务执行器 (Logic Executor) —— 处理“纯粹”
*   **职责**：只负责执行业务。
*   **执行条件**：它**绝对信任**花名册。
*   **性能逻辑**：利用 `ForAllSetBits` 等硬件优化指令，直接跳过所有为 0 的对象。

---

## 三、 标准代码模板 (C++)

假设我们要管理一个“动态 AI 感知系统”，只有在活跃范围内的 AI 才执行逻辑：

### 1. 权威花名册定义
```cpp
struct FAIProcessingManager
{
    // 位数组：每个 Bit 代表一个 AI 实例是否需要本帧处理
    FBitArray ActiveAIAuthoritativeRoster;
    
    // 存储 AI 数据的数组
    TArray<FAIData> AIManagedData;
};
```

### 2. 底层状态更新 (防抖与决策)
```cpp
void UpdateAIRoster(float DeltaTime)
{
    // 这里处理“脏活”：距离检查、超时判定、视锥裁剪
    for(int32 i=0; i < AIManagedData.Num(); ++i)
    {
        bool bIsStillActive = CalculateComplexVisibility(AIManagedData[i]);
        
        // 只有在这里才写复杂的 if 逻辑
        ActiveAIAuthoritativeRoster[i] = bIsStillActive;
    }
}
```

### 3. 极速业务执行 (享用干净数据)
```cpp
void ProcessActiveAIs()
{
    // Lambda 闭包：只写核心业务逻辑
    auto ProcessSingleAI = [this](uint32 AIIndex)
    {
        // 这里绝对不写 if(Active) 判断！
        // 因为能进来的 AI 肯定都是有效的
        AIManagedData[AIIndex].ExecuteTick();
    };

    // 硬件级加速遍历：利用位运算瞬间跳过所有非活跃 AI
    ActiveAIAuthoritativeRoster.ForAllSetBits(ProcessSingleAI);
}
```

---

## 四、 模式优势

1.  **CPU 缓存友好**：位数组极其紧凑（1个字节代表 8 个对象），可以在 CPU Cache 中常驻。
2.  **分支预测优化**：移除了热循环内的 `if` 判断，减少了 CPU 分支预测失败的惩罚。
3.  **代码解耦**：
    *   **判定逻辑** 只管怎么把位设为 1。
    *   **业务逻辑** 只管位是 1 时该干什么。
4.  **抗波动性**：可以在判定层引入“防抖时间”，位数组的变更可以比物理连接的波动慢，从而平滑业务表现。

---

## 五、 项目内潜在应用点 (Zoira Project)

*   **背包系统大仓库渲染**：底层检查哪些格子在滚动视图内，位数组标记 1，UI 层只遍历 1 的位进行刷表。
*   **大规模掉落物吸引力计算**：底层判断哪些掉落物进入了玩家磁铁范围，更新位数组，逻辑层全速移动。
*   **音频系统并发控制**：标记哪些音源当前处于音量权重前 N 名。

---
*文档位置：`Doc/02_SOP与开发规约/项目通用`*
