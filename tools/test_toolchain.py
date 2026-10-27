#!/usr/bin/env python3
"""K/L 原生 C 子集工具链与 FYX Studio 端到端验收。

每个用例都启动真实 FYXOS 内核；编译、ELF 装载、ring3 执行和 ATA 持久化
均发生在 guest 内。宿主只注入按键并检查日志/PNG，不参与编译。
"""
from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
READY = "[init] 全部通过"


def run_case(name, kernel, output_dir, keys, max_seconds, *, png=False):
    cmd = [
        sys.executable, str(ROOT / "tools/fyxemu.py"),
        "--kernel", str(kernel), "--mode", "quick",
        "--keys", keys, "--keys-after", READY,
        "--test", "--max-seconds", str(max_seconds),
    ]
    png_path = output_dir / f"{name}.png"
    if png:
        cmd += ["--fb-output", str(png_path)]
    result = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True,
                            timeout=max_seconds + 30)
    text = result.stdout + result.stderr
    (output_dir / f"{name}.log").write_text(text, encoding="utf-8")
    if result.returncode != 0:
        raise RuntimeError(f"{name}: emulator exit {result.returncode}\n{text[-6000:]}")
    if "[fyxemu] 测试通过" not in text or "正常关机" not in text:
        raise RuntimeError(f"{name}: 未通过基础自检或正常关机")
    if re.search(r"\[panic\]|kernel panic|内核 panic|CPU 异常", text, re.IGNORECASE):
        raise RuntimeError(f"{name}: 出现 panic/CPU 异常")
    return text, png_path


def expect(text, needle, label):
    if needle not in text:
        raise RuntimeError(f"{label} 缺少 {needle!r}\n{text[-5000:]}")


def check_png(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError("IDE PNG signature 错误")
    if struct.unpack("!II", data[16:24]) != (800, 600):
        raise RuntimeError("IDE PNG 尺寸不是 800x600")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--kernel", default=str(ROOT / "build/fyxos.elf"))
    ap.add_argument("--output-dir", default=str(ROOT / "build/toolchain-test"))
    ap.add_argument("--max-seconds", type=int, default=120)
    args = ap.parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    failures = 0

    try:
        # \; 是 fyxemu 的字面分号转义；普通分号仍分隔 Shell 命令。
        keys = (
            "ifconfig;ping;"
            "write /home/user/repeat.c int main(){puts(\"REPEAT_OK\")\\;return(42)\\;};"
            "fyxcc /home/user/repeat.c /home/user/repeat.elf;"
            "run /home/user/repeat.elf;run /home/user/repeat.elf;"
            "fyxcc /home/user/repeat.c /home/user/repeat.elf;"
            "run /home/user/repeat.elf;run /home/user/repeat.elf;"
            "write /home/user/control.c int main(){int x=0\\;while(x<6){x=x+1\\;}if(x==6){x=x*7\\;}else{x=1\\;}if(x<42){x=0\\;}else{x=x+0\\;}puts(\"CONTROL_OK\")\\;return x\\;};"
            "fyxcc /home/user/control.c /home/user/control.elf;"
            "run /home/user/control.elf;"
            "write /home/user/app.js function main(){let x=0\\;while(x<6){x=x+1\\;}if(x===6){x=x*7\\;}else{x=1\\;}console.log(\"JS_OK\")\\;process.exit(x)\\;};"
            "fyxjs /home/user/app.js /home/user/app.elf;run /home/user/app.elf;"
            "write /home/user/bad.c int main(){return(};"
            "fyxcc /home/user/bad.c /home/user/bad.elf;"
            "write /home/user/bad.js function main(){console.log('bad')\\;};"
            "fyxjs /home/user/bad.js /home/user/bad.elf;"
            "run /home/user/repeat.elf;shutdown"
        )
        out, _ = run_case("repeat-and-diagnostic", args.kernel, output_dir,
                          keys, args.max_seconds)
        exits = re.findall(r"\[proc\] exit: pid=\d+ code=42", out)
        if len(exits) != 7 or out.count("REPEAT_OK") < 5:
            raise RuntimeError(f"重复运行次数错误：exit42={len(exits)} output={out.count('REPEAT_OK')}")
        if out.count("[cc] /home/user/repeat.c") != 2:
            raise RuntimeError("重复编译次数错误")
        expect(out, "编译失败", "非法源码")
        expect(out, "偏移", "非法源码诊断")
        expect(out, "[run] /home/user/repeat.elf 退出码=42", "退出码")
        expect(out, "CONTROL_OK", "变量/分支/循环程序输出")
        expect(out, "[run] /home/user/control.elf 退出码=42", "控制流退出码")
        expect(out, "[js] /home/user/app.js", "FyxJS 原生编译")
        expect(out, "JS_OK", "FyxJS 程序输出")
        expect(out, "[run] /home/user/app.elf 退出码=42", "FyxJS 退出码")
        expect(out, "only double-quoted strings", "FyxJS 负向诊断")
        print("  [PASS] C/FyxJS 重复编译、控制流、ring3 退出码 42、正/负向诊断")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] Shell 工具链：{error}")

    try:
        keys = (
            "ifconfig;ping;"
            "write /home/user/l.c int main(){int t=0\\;"
            "for(int i=1\\;i<7\\;i=i+1){if(i==2)continue\\;"
            "if(i==6)break\\;t=t+i\\;}i=0\\;while(i<4){i=i+1\\;"
            "if(i==2)continue\\;t=t+i\\;}return t*2\\;};"
            "fyxcc /home/user/l.c /home/user/l.elf;run /home/user/l.elf;"
            "write /home/user/n.c int main(){int t=0\\;for(int i=0\\;i<4\\;i=i+1)"
            "{int j=0\\;while(j<4){j=j+1\\;if(j==2)continue\\;if(i==2)break\\;"
            "t=t+1\\;}}return t\\;};"
            "fyxcc /home/user/n.c /home/user/n.elf;run /home/user/n.elf;"
            "write /home/user/badloop.c int main(){break\\;};"
            "fyxcc /home/user/badloop.c /home/user/badloop.elf;shutdown"
        )
        out, _ = run_case("loop-control", args.kernel, output_dir,
                          keys, args.max_seconds)
        expect(out, "[run] /home/user/l.elf 退出码=42",
               "for/break/continue 的 ring3 计算结果")
        expect(out, "[run] /home/user/n.elf 退出码=9",
               "嵌套循环的内层 break/continue 跳转隔离（3+3+0+3）")
        expect(out, "break outside loop/switch", "循环/switch 外 break 诊断")
        print("  [PASS] C for/break/continue、嵌套 fixup、迭代路径与误用诊断")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] C 循环控制：{error}")

    try:
        keys = (
            "ifconfig;ping;"
            "write /home/user/o.c int main(){int t=0\\;for(int i=0\\;i<8\\;i++)"
            "{if(i==2)continue\\;t+=i\\;}int j=3\\;t+=++j\\;t+=j++\\;"
            "t+=--j\\;t+=j--\\;t-=2\\;t*=2\\;t/=2\\;t+=2\\;return t\\;};"
            "fyxcc /home/user/o.c /home/user/o.elf;run /home/user/o.elf;shutdown"
        )
        out, _ = run_case("operators", args.kernel, output_dir,
                          keys, args.max_seconds)
        expect(out, "[run] /home/user/o.elf 退出码=42",
               "复合赋值与前/后缀自增减的 ring3 计算结果")
        print("  [PASS] C ++/--、+=/-=/*=/= 与惯用 for 迭代表达式")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] C 运算符：{error}")

    try:
        keys = (
            "ifconfig;ping;"
            "write /home/user/m.c int main(){int x=17\\;if(x%5!=2)return 1\\;"
            "if(-17%5!=-2)return 2\\;x%=5\\;if(x!=2)return 3\\;return 42\\;};"
            "fyxcc /home/user/m.c /home/user/m.elf;run /home/user/m.elf;"
            "write /home/user/s.c int main(){if((1+2<<2)!=12)return 1\\;"
            "if((16>>2+1)!=2)return 2\\;if((-8>>2)!=-2)return 3\\;"
            "if(~0!=-1)return 4\\;return 42\\;};"
            "fyxcc /home/user/s.c /home/user/s.elf;run /home/user/s.elf;"
            "write /home/user/b.c int main(){if((5&3)!=1)return 1\\;"
            "if((5|2)!=7)return 2\\;if((5^3)!=6)return 3\\;"
            "if((1|2&4)!=1)return 4\\;if((1^3&1)!=0)return 5\\;return 42\\;};"
            "fyxcc /home/user/b.c /home/user/b.elf;run /home/user/b.elf;shutdown"
        )
        out, _ = run_case("integer-operators", args.kernel, output_dir,
                          keys, args.max_seconds)
        for name, label in (("m", "取余/%="), ("s", "移位/~"),
                            ("b", "&/|/^ 与优先级")):
            expect(out, f"[run] /home/user/{name}.elf 退出码=42", label)
        print("  [PASS] C %/%=、移位、~、&/|/^ 与 C 表达式优先级")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] C 整数/位运算：{error}")

    try:
        keys = (
            "ifconfig;ping;"
            "write /home/user/l.c int main(){int x=0\\;if(0&&++x)return 1\\;"
            "if(1||++x)x+=3\\;if(x!=3)return 2\\;if(!x||!!x!=1)return 3\\;"
            "if(!(x&&3))return 4\\;return 42\\;};"
            "fyxcc /home/user/l.c /home/user/l.elf;run /home/user/l.elf;"
            "write /home/user/q.c int main(){if((1||0&&0)!=1)return 1\\;"
            "if((0||3)!=1)return 2\\;if((3&&0)!=0)return 3\\;return 42\\;};"
            "fyxcc /home/user/q.c /home/user/q.elf;run /home/user/q.elf;shutdown"
        )
        out, _ = run_case("logical-operators", args.kernel, output_dir,
                          keys, args.max_seconds)
        expect(out, "[run] /home/user/l.elf 退出码=42",
               "&&/|| 短路副作用与 ! 结果")
        expect(out, "[run] /home/user/q.elf 退出码=42",
               "逻辑运算 0/1 归一化与优先级")
        print("  [PASS] C !、&&/|| 真实短路、0/1 归一化与优先级")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] C 逻辑运算：{error}")

    try:
        keys = (
            "ifconfig;ping;"
            "write /home/user/d.c int main(){int i=0\\;do{i++\\;if(i==3)continue\\;}"
            "while(i<3)\\;if(i!=3)return 1\\;do{i++\\;}while(0)\\;"
            "if(i!=4)return 2\\;return 42\\;};"
            "fyxcc /home/user/d.c /home/user/d.elf;run /home/user/d.elf;"
            "write /home/user/t.c int main(){int x=0\\;int y=0?++x:++x\\;"
            "if(x!=1||y!=1)return 1\\;y=0?1:1?42:0\\;return y\\;};"
            "fyxcc /home/user/t.c /home/user/t.elf;run /home/user/t.elf;shutdown"
        )
        out, _ = run_case("do-and-ternary", args.kernel, output_dir,
                          keys, args.max_seconds)
        expect(out, "[run] /home/user/d.elf 退出码=42",
               "do…while 至少一次与 continue 跳尾部条件")
        expect(out, "[run] /home/user/t.elf 退出码=42",
               "?: 单分支求值与右结合")
        print("  [PASS] C do…while、循环尾条件 continue 与 ?: 单分支求值")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] C do/条件运算：{error}")

    try:
        keys = (
            "ifconfig;ping;"
            "write /home/user/w.c int main(){int t=0\\;switch(2){case 1:t=1\\;break\\;"
            "case 0x2:t=3\\;case 3:t+=4\\;break\\;default:t=99\\;}"
            "switch(9){case 1:t=0\\;default:t+=35\\;}return t\\;};"
            "fyxcc /home/user/w.c /home/user/w.elf;run /home/user/w.elf;"
            "write /home/user/v.c int main(){int t=0\\;switch(1){case 1:"
            "switch(2){case 1:t=1\\;break\\;case 2:t=6\\;break\\;}break\\;"
            "default:t=9\\;}t*=7\\;return t\\;};"
            "fyxcc /home/user/v.c /home/user/v.elf;run /home/user/v.elf;"
            "write /home/user/x.c int main(){int t=0\\;int i=0\\;while(i<2){i++\\;"
            "switch(i){case 1:t+=1\\;break\\;default:t+=2\\;break\\;}}t*=14\\;return t\\;};"
            "fyxcc /home/user/x.c /home/user/x.elf;run /home/user/x.elf;"
            "write /home/user/z.c int main(){int t=0\\;for(int j=0\\;j<3\\;j++){"
            "switch(j){case 1:continue\\;default:t+=1\\;break\\;}}t*=21\\;return t\\;};"
            "fyxcc /home/user/z.c /home/user/z.elf;run /home/user/z.elf;"
            "write /home/user/badcase.c int main(){case 1:return 1\\;};"
            "fyxcc /home/user/badcase.c /home/user/badcase.elf;"
            "write /home/user/dup.c int main(){switch(1){case 1:break\\;"
            "case 1:break\\;}return 0\\;};"
            "fyxcc /home/user/dup.c /home/user/dup.elf;shutdown"
        )
        out, _ = run_case("switch-control", args.kernel, output_dir,
                          keys, args.max_seconds)
        for name, label in (("w", "fallthrough/default 与顺序 switch"),
                            ("v", "嵌套 switch 与隐藏表达式槽"),
                            ("x", "switch break 不退外层 while"),
                            ("z", "switch 内 continue 继续外层 for")):
            expect(out, f"[run] /home/user/{name}.elf 退出码=42", label)
        expect(out, "case/default outside switch", "switch 外 case 诊断")
        expect(out, "duplicate case value", "重复 case 诊断")
        print("  [PASS] C switch/case/default、fallthrough、诊断与循环控制隔离")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] C switch 控制：{error}")

    try:
        keys = (
            "ifconfig;ping;"
            "write /home/user/f.c int add(int a,int b){return a+b\\;}"
            "int mul(int a,int b){return a*b\\;}int main(){return mul(add(2,3),8)+2\\;};"
            "fyxcc /home/user/f.c /home/user/f.elf;run /home/user/f.elf;"
            "write /home/user/r.c int fact(int n){if(n<=1)return 1\\;"
            "return n*fact(n-1)\\;}int main(){return fact(3)*7\\;};"
            "fyxcc /home/user/r.c /home/user/r.elf;run /home/user/r.elf;"
            "write /home/user/v.c int pick(int x){switch(x){case 1:return 10\\;"
            "default:return 2\\;}}int main(){return pick(1)+pick(9)+30\\;};"
            "fyxcc /home/user/v.c /home/user/v.elf;run /home/user/v.elf;"
            "write /home/user/a.c int answer(void){puts(\"FN_OK\")\\;return 42\\;}"
            "int main(){return answer()\\;};"
            "fyxcc /home/user/a.c /home/user/a.elf;run /home/user/a.elf;"
            "write /home/user/forward.c int main(){return later()\\;}"
            "int later(void){return 42\\;};"
            "fyxcc /home/user/forward.c /home/user/forward.elf;"
            "write /home/user/arity.c int one(int x){return x\\;}"
            "int main(){return one()\\;};"
            "fyxcc /home/user/arity.c /home/user/arity.elf;shutdown"
        )
        out, _ = run_case("functions", args.kernel, output_dir,
                          keys, args.max_seconds)
        for name, label in (("f", "多参数与嵌套调用"),
                            ("r", "递归参数帧"),
                            ("v", "helper 内 switch/return"),
                            ("a", "无参 helper 与 syscall 返回")):
            expect(out, f"[run] /home/user/{name}.elf 退出码=42", label)
        expect(out, "FN_OK", "helper 内 puts")
        expect(out, "unknown function (prototype or definition required)",
               "未声明函数调用诊断")
        expect(out, "function argument count mismatch", "参数个数诊断")
        print("  [PASS] C 自定义函数、栈传参、嵌套/递归调用与入口/返回诊断")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] C 函数调用：{error}")

    try:
        keys = (
            "ifconfig;ping;"
            "write /home/user/f.c int later(int)\\;int later(int x)\\;"
            "int main(){return later(42)\\;}int later(int x){return x\\;};"
            "fyxcc /home/user/f.c /home/user/f.elf;run /home/user/f.elf;"
            "write /home/user/m.c int o(int n)\\;int e(int n){if(!n)return 1\\;"
            "return o(n-1)\\;}int o(int n){if(!n)return 0\\;return e(n-1)\\;}"
            "int main(){return e(6)&&o(5)?42:1\\;};"
            "fyxcc /home/user/m.c /home/user/m.elf;run /home/user/m.elf;"
            "write /home/user/undef.c int missing(int)\\;int main(){return missing(1)\\;};"
            "fyxcc /home/user/undef.c /home/user/undef.elf;"
            "write /home/user/conflict.c int f(int x)\\;int f(int x,int y){return x+y\\;}"
            "int main(){return 0\\;};"
            "fyxcc /home/user/conflict.c /home/user/conflict.elf;"
            "write /home/user/dupe.c int f(void){return 1\\;}int f(void){return 2\\;}"
            "int main(){return 0\\;};"
            "fyxcc /home/user/dupe.c /home/user/dupe.elf;"
            "write /home/user/noname.c int f(int){return 1\\;}int main(){return 0\\;};"
            "fyxcc /home/user/noname.c /home/user/noname.elf;shutdown"
        )
        out, _ = run_case("prototypes", args.kernel, output_dir,
                          keys, args.max_seconds)
        expect(out, "[run] /home/user/f.elf 退出码=42",
               "重复原型与前向 rel32 修补")
        expect(out, "[run] /home/user/m.elf 退出码=42",
               "even/odd 相互递归")
        expect(out, "undefined function", "只有原型的调用诊断")
        expect(out, "conflicting function declaration", "签名冲突诊断")
        expect(out, "duplicate function definition", "重复定义诊断")
        expect(out, "parameter name required in function definition",
               "定义缺少参数名诊断")
        print("  [PASS] C 函数原型、前向调用、相互递归与声明/定义诊断")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] C 函数原型：{error}")

    try:
        keys = (
            "ifconfig;ping;"
            "write /home/user/jf.js function fact(n){if(n<=1)return 1\\;"
            "return n*fact(n-1)\\;}function main(){console.log(\"JS_FN_OK\")\\;"
            "process.exit(fact(3)*7)\\;};"
            "fyxjs /home/user/jf.js /home/user/jf.elf;run /home/user/jf.elf;"
            "write /home/user/ja.js function add(a,b){console.log(\"JS_ADD_OK}\")\\;"
            "/*}*/return a+b\\;}/*}*/function main(){process.exit(add(40,2))\\;};"
            "jsc /home/user/ja.js /home/user/ja.elf;run /home/user/ja.elf;"
            "write /home/user/badexit.js function bad(){process.exit(1)\\;}"
            "function main(){return 0\\;};"
            "fyxjs /home/user/badexit.js /home/user/badexit.elf;"
            "write /home/user/hoist.js function main(){return later()\\;}"
            "function later(){return 42\\;};"
            "fyxjs /home/user/hoist.js /home/user/hoist.elf;"
            "write /home/user/nomain.js function value(){return 42\\;};"
            "fyxjs /home/user/nomain.js /home/user/nomain.elf;"
            "write /home/user/unterm.js function main(){if(true){return 42\\;};"
            "fyxjs /home/user/unterm.js /home/user/unterm.elf;shutdown"
        )
        out, _ = run_case("js-functions", args.kernel, output_dir,
                          keys, args.max_seconds)
        expect(out, "[run] /home/user/jf.elf 退出码=42", "FyxJS 递归")
        expect(out, "[run] /home/user/ja.elf 退出码=42",
               "FyxJS 双参数 helper 与 jsc 别名")
        expect(out, "JS_FN_OK", "FyxJS main 输出")
        expect(out, "JS_ADD_OK}", "helper 输出及字符串/注释大括号")
        expect(out, "process.exit is only supported in main",
               "helper process.exit 诊断")
        expect(out, "unknown function (prototype or definition required)",
               "无提升的前向调用诊断")
        expect(out, "missing main function", "缺少 main 诊断")
        expect(out, "unterminated function body", "未闭合函数体诊断")
        print("  [PASS] FyxJS 多函数、参数、递归、词法边界与退出诊断")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] FyxJS 函数：{error}")

    try:
        keys = (
            "ifconfig;ping;ide /home/user/studio.c;"
            "iint main(){puts(\"STUDIO_OK\")\\;return(11)\\;}\\e"
            "\\cS\\cB\\cR\\cQ;shutdown"
        )
        out, png = run_case("studio", args.kernel, output_dir, keys,
                            args.max_seconds, png=True)
        expect(out, "[ide] saved /home/user/studio.c", "IDE 保存")
        expect(out, "[ide] built /home/user/studio.elf", "IDE 编译")
        expect(out, "STUDIO_OK", "IDE 程序输出")
        expect(out, "[ide] program exit=11", "IDE 程序退出码")
        guest = re.findall(r"\[ide\] closed frames=\d+ checksum=0x([0-9a-fA-F]+)", out)
        host = re.findall(r"framebuffer 已保存.*checksum=0x([0-9a-fA-F]+)", out)
        if not guest or not host or guest[-1].lower() != host[-1].lower():
            raise RuntimeError("IDE guest/host framebuffer 校验和不一致")
        check_png(png)
        print("  [PASS] IDE 空文件编辑→保存→编译→ring3 运行→PNG/校验和")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] FYX Studio：{error}")

    print(f"工具链/IDE 专项：{'通过' if failures == 0 else '失败'}（{failures} 个失败）")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
