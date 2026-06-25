# 前向声明 vs 定义 vs 实现

C++ 里一个类型有三个递进层级,**用到什么给到什么**,不多不少:

| 层级 | 长什么样 | 给编译器什么 |
|---|---|---|
| **前向声明** | `class T;` | 知道有这个名字 |
| **定义** | `class T { void foo(); int x; };` | 大小、布局、有哪些成员 |
| **实现** | `void T::foo() { ... }` | 每个函数怎么干活 |

下面照三层讲什么时候够用。

## 第 1 层:前向声明就够

编译器只需要"这是个类型名",不需要算大小:

- 指针 `T*`、引用 `T&`
- 函数**声明**:`T foo(T x);`(光声明不生成代码,值传也行)
- 类成员是 `T*` 或 `T&`
- `using P = T*;`、`friend class T;`

## 第 2 层:需要定义(类体可见,函数体可以在 .cpp)

要算大小、要访问内部、要生成构造/析构代码:

- 创建对象 `T t;`、`new T`
- 访问成员 `t.x`、`p->foo()`
- `sizeof(T)`、按值传参/返回的**函数定义和调用点**
- 类成员按值持有 `class C { T m; };`
- 继承 `class D : public T`
- `throw t;` / `catch (T&)`
- `delete p;`(需要看到析构)
- `std::vector<T>` 等容器实例化

注意:这层**只要求类体可见**,类里成员函数的函数体可以扔到 .cpp 链接。这就是 C++ 分离编译的常态。

## 第 3 层:需要实现(函数体在调用点可见,通常放头文件)

编译器要在调用方那里**生成代码**,链接时再找已经来不及:

- `inline` 函数
- **模板**函数 / 模板类成员函数
- `constexpr` 函数(编译期求值)
- `auto` 返回类型推导
- 类内直接定义的成员函数(隐式 `inline`,自动满足)

反例:
```cpp
// vec.cpp ❌ 模板定义放 .cpp
template <typename T>
void Vec<T>::push(T x) { ... }   // 别的 .cpp 实例化 Vec<int> 时找不到
                                  // → 链接报错 undefined reference
```

## ODR 一句话

- **函数定义**全程序只能 1 份(否则 linker 报 multiple definition)
- **声明**想写几次写几次
- 例外:`inline` / 模板 / `constexpr` / 类内定义,允许多 TU 各有一份**相同**定义——这就是它们能放头文件的原因

## 高频坑

### `unique_ptr<T>` 成员
```cpp
// foo.hpp
class T;                  // 前向声明就行
class Foo {
    std::unique_ptr<T> p;
    ~Foo();               // 只声明,别 = default
};
// foo.cpp
#include "T.hpp"
Foo::~Foo() = default;    // 这里 T 完整,析构 unique_ptr 才合法
```
头文件里 `= default` 会强制每个 include 方就地生成析构,但那里 T 还不完整 → `invalid application of sizeof to incomplete type`。

### Pimpl 模式
`class Impl;` + `unique_ptr<Impl>` 成员 + 析构放 .cpp,把实现彻底藏起来,降低编译依赖。

### `Throwy` 必须放头文件
`throw` / `catch` 要构造析构异常对象,所有调用方都得看到完整定义。

## 心智模型

每次写一行代码,问自己:**"编译器在这一行需要知道 T 的什么?"**

- 只是知道名字 → 第 1 层,前向声明
- 算大小、访问成员、构造析构、继承 → 第 2 层,定义
- 要内联、实例化模板、编译期求值 → 第 3 层,实现
