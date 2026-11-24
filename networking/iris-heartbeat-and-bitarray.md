---
tags:
  - Zoira/网络
  - Iris/架构
  - 核心原理
aliases:
  - Iris网络架构分析
  - 心跳与位数组机制
created: 2026-04-24
status: stable
---

# UE5 Iris 网络架构深度解析：物理传输与逻辑状态的隔离

> [!abstract] 架构核心
> 在极其恶劣的真实网络环境（丢包、乱序、延迟尖峰）中，Iris 通过引入“连接管理器（Connection Manager）”作为缓冲带，实现了极其优雅的隔离。

---

## 一、 设计哲学：物理与逻辑的二元论

Iris 将网络连接拆分为两个互不干扰的层面：

1.  **底层（传输层）**：负责接收 UDP 数据包并更新时间戳，容忍网络波动与乱序。
2.  **高层（业务层）**：只信任 `FNetBitArray`（位数组），将其作为绝对权威的“在线花名册”。只要位是 1，业务层就认为玩家正常。

---

## 二、 闭环生命周期：从心跳包到代码执行

### 1. 底层心跳守望者 (Heartbeat & Tick)
在执行 `UpdateDataStreams` 之前，`NetDriver` 会先进行连接保活检查：

*   **更新时间戳**：收到客户端任何包（含 Keep-Alive）时更新 `LastReceiveTime`。
*   **超时裁决 (Timeout Check)**：计算 `CurrentTime - LastReceiveTime`。
*   **状态变更**：
    *   差值 < 15秒（默认阈值）：判定为正常波动，保持 Bit 为 1。
    *   **🚨 差值 > 15秒**：判定为真实断开！强制将 `FNetBitArray` 中对应的 Bit 位设为 0。

### 2. 高层极速遍历 (UpdateDataStreams)
因为底层已经处理了“脏活”，业务代码可以全速运转，无需写任何 `if (Conn->IsTimingOut())` 的冗余判断。

```cpp
void UpdateDataStreams(UDataStream::FUpdateParameters& UpdateParameters)  
{
    IRIS_CSV_PROFILER_SCOPE(Iris, ReplicationSystem_UpdateDataStreams);

    // 1. 获取连接大管家
    FReplicationConnections& Connections = ReplicationSystemInternal.GetConnections();
    
    // 2. 定义业务逻辑闭包
    auto UpdateDataStreamsForConnection = [&Connections, &UpdateParameters](uint32 ConnectionId)
    {
       FReplicationConnection* Conn = Connections.GetConnection(ConnectionId);
       if (UDataStreamManager* DataStreamManager = Conn->DataStreamManager.Get())
       {
          // 哪怕此时客户端卡顿，只要没超时，这里依然会为它准备同步数据
          Conn->DataStreamManager->Update(UpdateParameters);
       }
    };
    
    // 3. 获取权威花名册（经过心跳防抖处理的位数组）
    const FNetBitArray& ValidConnections = Connections.GetValidConnections();
    
    // 4. 极速过滤：只对位图为 1 的连接执行闭包（硬件级位运算效率）
    ValidConnections.ForAllSetBits(UpdateDataStreamsForConnection);
}
```

---

## 三、 极端场景下的防抖与裁决

### 场景 A：玩家电梯内网络瞬断（5 秒丢包）
*   **底层状态**：收不到包，但 5s < 15s 阈值，**防抖机制生效**。
*   **位数组状态**：玩家位保持为 1。
*   **业务表现**：`UpdateDataStreams` 照常工作，数据包被塞入发送缓冲区。
*   **恢复表现**：一旦网络恢复，5 秒内累积的增量数据（Delta Compression）会瞬间同步。

### 场景 B：直接拔掉网线（彻底掉线）
*   **0-15 秒**：位数组为 1，业务层继续打包。
*   **15.1 秒**：触发超时，`ValidConnections` 对应位重置为 0。
*   **之后**：`ForAllSetBits` 瞬间跳过该连接，不再执行任何打包逻辑。玩家残留数据进入垃圾回收。

---

## 四、 架构总结

这种设计模式的精妙之处在于 **“各司其职”**：

*   **心跳与宽限机制**：承担了网络世界的混沌，产出一份干净、稳定的 `FNetBitArray`。
*   **业务层函数**：享受这份干净的数据，利用 CPU 位运算（`ForAllSetBits`）进行最高效的数据处理。

---
*文档位置：`Doc/02_SOP与开发规约/项目通用`*
