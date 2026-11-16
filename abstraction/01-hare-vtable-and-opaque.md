# 01 · Hare 侧：vtable、24 字节、函数指针当类型标签

读原文时容易只看懂「有个 opaque 类型」，漏掉下面三件事。拆开看：

## 1. 语言约束决定了手法

Hare **没有 interface，也没有泛型**。作者要把「一份 SQL 接口 + 两个互不认识的驱动」做出来，只能自己造抽象：**一张函数指针表，把指向它的指针作为驱动 struct 的第一个字段嵌入**。

```hare
export type db_vtable = struct {
	exec: *fn(db: *db, q: str, args: fmt::field...) (result | *rows | error | nomem),
	close: *fn(db: *db) (void | error),
};

// 驱动这样接入：
//	export type my_db = struct {
//		vtable: sql::db,
//		// ...
//	};
export type db = *db_vtable;
```

```hare
export fn exec(db: *db, q: str, args: fmt::field...) (result | *rows | error | nomem) =
	db.exec(db, q, args...);
```

关键在内存布局，不在语法：

- `sql::db` 本身就是 `*db_vtable`，**类型在编译期已被擦除**；
- 驱动 struct 的**第一个字段**就是那个 vtable 指针，所以 `&my_db` 与 `my_db.vtable` 地址相同，`*my_db → *sql::db` 不需要任何偏移；
- 反向恢复发生在每个回调的第一行：`let sqldb = sqldb: *db;`。

> 这正是 C++ 里「抽象基类 + 虚表」在手工形态下的等价物，只不过 C++ 编译器替你算偏移，Hare 靠「首字段」这个约定。

## 2. `*opaque`：解释权留给驱动，接口只保证「有个东西」

行扫描的目标值允许是驱动自定义类型，接口侧把驱动给的原始数据叫作 `data: *opaque`：

```hare
fn coord_scan(v: *sql::value, data: *opaque) (void | sql::error) = {
	let v = v: *coord;
	const s = strings::trim(postgresql::text(data), '(', ')');
	// ...
};
```

这里的边界很干脆：

- `data` 指向什么，**由驱动决定**（SQLite 给一种，PostgreSQL 给另一种）；
- 接口不解释它，把「怎么读回来」交给驱动自己的辅助函数（`postgresql::text`）；
- 调用方若写错驱动，读到的是垃圾——这是抽象本身的漏洞，靠文档与类型约定兜。

这条对应到 UE 就是：**统一错误容器只存 `TSharedPtr<细节基类>`，不解释细节**。

## 3. `errors::opaque_`：24 字节内联存储 + 一个函数指针

`sql::error` 是**封闭 union**（因为 `sql::strerror` 必须逐个 match），所以第三方驱动自己的错误码塞不进去：

```hare
export type error = !(...errors::error | errors::opaque_ | io::error | dial::error |
	net::error | utf8::invalid | conndone | norows | txdone);
```

标准库给的解法是 `errors::opaque_`：

```hare
export type opaque_ = !struct {
	strerror: *fn(op: *opaque_data) const str,
	data: opaque_data,
};

// Up to 24 bytes of arbitrary data that the opaque error type may use for
// domain-specific storage.
export type opaque_data = [3]u64;
```

驱动把错误码写进这 24 字节，并装上自己的打印器（PostgreSQL 是 5 字符 SQLSTATE，SQLite 是 `int`）：

```hare
export type servererror = ![5]u8;

fn wraperror(err: servererror) sql::error = {
	static assert(size(servererror) <= size(errors::opaque_data));
	let w = errors::opaque_ { strerror = &opaque_strerror, ... };
	*(&w.data: *servererror) = err;
	return w;
};

fn opaque_strerror(data: *errors::opaque_data) const str =
	strerror(data: *servererror);
```

两个工程细节值得抄：

1. **`static assert` 卡容量**：细节类型超过 24 字节当场编译失败，而不是运行期读 `data` 时才发现截断。
2. **零堆分配**：错误码本来就只有几个字节，24 字节内联够用，错误路径不碰分配器。

## 4. 核心技巧：函数指针就是类型标签

`opaque_` 里**没有 type id 字段**。每个驱动自己的 `&opaque_strerror` 地址唯一，所以与该地址比较即可判定来源：

```hare
export fn unwraperror(err: errors::opaque_) (servererror | void) = {
	if (err.strerror != &opaque_strerror) {
		return;
	};
	return *(&err.data: *servererror);
};
```

为什么够用：

| 需求 | 谁满足它 |
|---|---|
| 打印成人类可读文本 | `strerror(data)` 一次调用 |
| 判断「是不是我的错误」 | `err.strerror == &opaque_strerror` |
| 判断「是不是别人的错误」 | 指针不等 → 返回 `void`，**绝不重解释这 24 字节** |

原文原话：除了这个比较，**没有任何机制能阻止误读**。所以自己写 unwrap 函数时，这个检查是强制的，不是优化。

反过来，两种「更直觉但更差」的做法：

- 往 `opaque_data` 里塞一个 magic number → 要自己维护编号、要防撞、还是免不了类型转换；
- 往统一错误里加 `str` 消息 → **丢掉类型信息**，调用方只能匹配英文字符串来判断「可重试 / 不可重试」。

## 5. 代价：调用侧要两层 match

```hare
match (sql::exec(&db, "INSERT INTO authors (id) VALUES (1)")) {
case let res: sql::result => // ...
case let err: sql::error =>
	match (err) {
	case let o: errors::opaque_ =>
		match (postgresql::unwraperror(o)) {
		case let e: postgresql::servererror =>
			if (postgresql::strerror(&e) == "23505") {
				// unique_violation，这行已经存在
			};
		case void =>
			// 是个 opaque_，但来自别的模块
		};
	case => fmt::fatal("exec:", sql::strerror(err));
	};
```

作者本人也不喜欢这两层嵌套，但接受它，因为**只有需要按错误码分支时才写**；只报告错误时直接 `sql::strerror(err)`——它内部回调驱动装的函数指针，调用方对具体类型一无所知。

## 6. 本项目里的可运行验证

[`samples/opaque_error_demo.cpp`](./samples/opaque_error_demo.cpp) 用纯 C++17 复刻了这套机制（24 字节 + `strerror` 指针当标签），本机 MSVC 编译运行输出：

```
[统一打印] postgresql : 23505
[统一打印] sqlite     : SQLITE_CONSTRAINT (19)

[类型恢复] pg -> 拿到 SQLSTATE 23505（唯一约束冲突，可跳过重试）
[类型恢复] sqlite 的 19 字节被拒绝解包成 ServerError（标签不匹配，fail-closed）
[类型恢复] sqlite -> 拿到 Code 19
[类型恢复] postgresql 的 SQLSTATE 被拒绝解包成 SqliteError

[反例] 不做标签检查强转 -> 读出 19,0,0,0,0（垃圾数据，正是 Hare 警告的误读）
```

最后一行就是「没有标签检查会怎样」的实证：同一块 24 字节，换个类型解释就是垃圾。

## 7. 可以脱离 Hare 迁移的三条原则

1. **抽象句柄 = 数据指针 + 操作表**。C++ 的虚函数、COM 的 vtable、Hare 的首字段 vtable、UE 的 `UInterface`，都是这个形态的不同包装。
2. **类型标签要挂在「进程内唯一 + 能反查来源」的东西上**。函数指针地址、`UScriptStruct*`、`FName TypeId` 都行；自增 magic number 不行（跨模块注册表一旦分裂就会撞号）。
3. **恢复必须 fail-closed**。标签不匹配 → 返回空/`void`/`false`，绝不「先转过去再看」。这条在 C++ 里等价于：`static_cast` 的前面必须有一个 `if (Tag == Expected)` 且没有 `else` 分支强行转。

## 8. 一个反面对照：被删掉的 `registerdriver()`

作者早期做过 `sql::open("sqlite://...")` 按 URI scheme 选驱动的注册表，后来**删了**。理由很值钱：

1. 只有两个驱动，撑不起一个注册表；
2. 为了让 opener 能返回指针，db 被迫分配到堆上，凭空引入一条**模块内其它地方都不需要的所有权规则**；
3. 所以「不值得」。

这提醒的是同一件事：抽象的每一层都要付账（分配、所有权、生命周期、调用侧嵌套）。**只有当你确实需要「新增实现不动公共模块」时，才付这笔账。**
