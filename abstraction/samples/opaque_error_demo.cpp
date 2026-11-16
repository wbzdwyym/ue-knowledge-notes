// opaque_error_demo.cpp —— 可编译验证的最小实现（纯 C++17，无 UE 依赖）
//
// 复刻 Hare 标准库 errors::opaque_ 的两个关键手法：
//   1. 24 字节内联存储（Hare: `opaque_data = [3]u64`）+ static_assert 容量检查
//   2. 函数指针的唯一地址充当「类型标签」，反向判定错误到底属于哪个模块
//
// 编译：build_and_run.bat（MSVC）
//      g++ -std=c++17 -O2 -Wall opaque_error_demo.cpp -o demo && ./demo

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace err {

// Hare: export type opaque_data = [3]u64;
struct OpaqueData
{
    alignas(8) std::uint64_t Words[3];
};
static_assert(sizeof(OpaqueData) == 24, "OpaqueData 必须是 24 字节");

// Hare: strerror: *fn(op: *opaque_data) const str
using StrErrorFn = const char* (*)(const OpaqueData*);

// Hare: export type opaque_ = !struct { strerror: *fn(...), data: opaque_data };
struct OpaqueError
{
    StrErrorFn StrError = nullptr; // 既是打印器，也是类型标签
    OpaqueData Data{};
};

// 驱动侧：把自己的错误码写进 24 字节存储
template <typename T>
OpaqueError MakeOpaque(T Value, StrErrorFn Fn)
{
	static_assert(std::is_trivially_copyable<T>::value,
		"opaque_data 内联存储只能放可平凡拷贝的类型");
	static_assert(sizeof(T) <= sizeof(OpaqueData),
		"错误细节超出 24 字节（Hare 用 static assert 在编译期就拦住）");

	OpaqueError E;
	E.StrError = Fn;
	std::memcpy(&E.Data, &Value, sizeof(T));
	return E;
}

// 调用侧：唯一的安全恢复入口，第一件事就是比较函数指针
template <typename T>
bool TryUnwrap(const OpaqueError& E, StrErrorFn Expected, T& Out)
{
	static_assert(std::is_trivially_copyable<T>::value, "同上");
	static_assert(sizeof(T) <= sizeof(OpaqueData), "同上");

	if (E.StrError == nullptr || E.StrError != Expected)
	{
		return false; // 标签不匹配 ⟹ 不是本模块的错误，绝不重解释这 24 字节
	}
	std::memcpy(&Out, &E.Data, sizeof(T));
	return true;
}

// 公共模块的统一打印路径：完全不认识任何具体错误类型
inline const char* StrError(const OpaqueError& E)
{
	return (E.StrError != nullptr) ? E.StrError(&E.Data) : "<unknown error>";
}

} // namespace err

// ---------------------------------------------------------------------------
// 模块 A：PostgreSQL 风格（5 字符 SQLSTATE）
// ---------------------------------------------------------------------------
namespace postgresql {

// Hare: export type servererror = ![5]u8;
struct ServerError
{
	char Code[5];
};

// Hare: fn opaque_strerror(data: *errors::opaque_data) const str
const char* OpaqueStrError(const err::OpaqueData* Data)
{
	static thread_local char Buf[8];
	std::memcpy(Buf, Data, sizeof(ServerError));
	Buf[5] = '\0';
	return Buf;
}

// Hare: fn wraperror(err: servererror) sql::error
err::OpaqueError WrapError(const ServerError& E)
{
	return err::MakeOpaque(E, &OpaqueStrError);
}

// Hare: export fn unwraperror(err: errors::opaque_) (servererror | void)
bool TryUnwrapError(const err::OpaqueError& E, ServerError& Out)
{
	return err::TryUnwrap(E, &OpaqueStrError, Out);
}

} // namespace postgresql

// ---------------------------------------------------------------------------
// 模块 B：SQLite 风格（int 结果码）
// ---------------------------------------------------------------------------
namespace sqlite {

struct SqliteError
{
	int Code = 0;
};

const char* OpaqueStrError(const err::OpaqueData* Data)
{
	const SqliteError* E = reinterpret_cast<const SqliteError*>(Data);
	switch (E->Code)
	{
	case 19: return "SQLITE_CONSTRAINT (19)";
	case 1:  return "SQLITE_ERROR (1)";
	default: return "SQLite error";
	}
}

err::OpaqueError WrapError(const SqliteError& E)
{
	return err::MakeOpaque(E, &OpaqueStrError);
}

bool TryUnwrapError(const err::OpaqueError& E, SqliteError& Out)
{
	return err::TryUnwrap(E, &OpaqueStrError, Out);
}

} // namespace sqlite

int main()
{
	const err::OpaqueError PgError = postgresql::WrapError(postgresql::ServerError{{'2', '3', '5', '0', '5'}});
	const err::OpaqueError LiteError = sqlite::WrapError(sqlite::SqliteError{19});

	// 1) 公共层只走统一打印，不认识任何具体类型
	std::printf("[统一打印] postgresql : %s\n", err::StrError(PgError));
	std::printf("[统一打印] sqlite     : %s\n\n", err::StrError(LiteError));

	// 2) 需要按错误码分支时，才做类型恢复；标签不匹配必须失败
	postgresql::ServerError PgOut{};
	if (postgresql::TryUnwrapError(PgError, PgOut))
	{
		std::printf("[类型恢复] pg -> 拿到 SQLSTATE %c%c%c%c%c（唯一约束冲突，可跳过重试）\n",
			PgOut.Code[0], PgOut.Code[1], PgOut.Code[2], PgOut.Code[3], PgOut.Code[4]);
	}

	if (!postgresql::TryUnwrapError(LiteError, PgOut))
	{
		std::printf("[类型恢复] sqlite 的 19 字节被拒绝解包成 ServerError（标签不匹配，fail-closed）\n");
	}

	sqlite::SqliteError LiteOut{};
	if (sqlite::TryUnwrapError(LiteError, LiteOut))
	{
		std::printf("[类型恢复] sqlite -> 拿到 Code %d\n", LiteOut.Code);
	}

	if (!sqlite::TryUnwrapError(PgError, LiteOut))
	{
		std::printf("[类型恢复] postgresql 的 SQLSTATE 被拒绝解包成 SqliteError\n");
	}

	// 3) 反面：如果没有标签检查，同样 24 字节会被读成完全无意义的值
	postgresql::ServerError Oops = *reinterpret_cast<const postgresql::ServerError*>(&LiteError.Data);
	std::printf("\n[反例] 不做标签检查强转 -> 读出 %d,%d,%d,%d,%d（垃圾数据，正是 Hare 警告的误读）\n",
		int(Oops.Code[0]), int(Oops.Code[1]), int(Oops.Code[2]), int(Oops.Code[3]), int(Oops.Code[4]));

	return 0;
}
