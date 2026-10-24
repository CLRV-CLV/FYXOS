# FyxJS v1 — 原生 JavaScript 子集前端

## 定位

FyxJS 是 FYXOS 原生工具链的第二个源语言前端。它读取 `.js`，在保留字符串与
注释边界的前提下转换为 fyxcc 的中间文本，再复用已经验收的 x86_64 代码生成器，
直接写出静态 ET_EXEC。没有外部编译器、宿主进程、JS 解释器或运行时依赖。

这不是完整 JavaScript，也不是 Node.js；当前目标是用同一套原生后端验证第二种
源语言的前端接口、错误诊断、磁盘产物与 ring3 执行闭环。

## 命令

```text
fyxjs <input.js> <output.elf>
jsc   <input.js> <output.elf>
run   <output.elf>
```

`jsc` 是同一实现的原生命令别名。输出落在调用者指定的 FYXFS 路径，写入前使用
VFS truncate；编译失败不会把旧 ELF 当作新产物运行。

## v1 语法

- 单个 `function main() { ... }` 入口；
- `let name = expression;` 声明，以及整数赋值；
- 32 位整数、四则运算、括号、`== != < <= > >=`；
- JavaScript 严格比较 `===` / `!==`；
- `if / else`、`while` 与大括号嵌套块；
- `true` / `false`；
- `console.log("literal")` 或 `console.log(integer_expression)`；
- `process.exit(integer_expression)`；
- `//` 与 `/* ... */` 注释；双引号字符串按原样保留。

示例：

```js
function main() {
    let x = 0;
    while (x < 6) { x = x + 1; }
    if (x === 6) { x = x * 7; } else { x = 1; }
    console.log("JS_OK");
    process.exit(x);
}
```

该程序应输出 `JS_OK`，随后由 ring3 进程以 42 退出。字符串输出不会自动补换行，
与底层 `puts` 的系统调用语义一致。

## 明确边界

所有变量都是静态 32 位整数。当前没有动态类型、对象、数组、闭包、函数调用、
异常、原型、事件循环、垃圾回收、模块、标准 JS 库或 Node API；不支持单引号
字符串、模板字符串及其转义语义。C 后端的 16 个局部变量、32 层块嵌套和
64 KiB 中间文本上限同样适用。`console` / `process` 只是这里定义的两个内建
编译目标，不代表浏览器或 Node 兼容层已经存在。

## 实现与验收

- `kernel/toolchain/fyxjs.c`：词法边界感知的前端、关键字/内建函数转换；
- `kernel/toolchain/fyxcc.c`：共用表达式、控制流和 x86_64 ELF 后端；
- `kernel/shell/shell.c`：`fyxjs` / `jsc` 命令；
- `tools/test_toolchain.py`：实际源码输入、ELF 生成、JS_OK、退出码 42、
  非法单引号字符串诊断，以及既有 C/IDE 回归。

可复现：

```sh
make image
make test-toolchain
```

完整 ECMAScript、多语言运行时和自举工具链仍属于后续工作，不能由 v1 结果推断。
