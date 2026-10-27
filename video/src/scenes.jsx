import React from 'react';
import {interpolate, useCurrentFrame} from 'remotion';
import {Beam, C, Cursor, Grid, Pill, Pop, Reveal, Scene, SceneFooter, Screen, Stat, Terminal, Title, mono, progress} from './components';

const clamp = {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'};
const phase = (frame, start, end) => interpolate(frame, [start, start + 12, end - 12, end], [0, 1, 1, 0], clamp);

export const ColdOpen = () => {
  const frame = useCurrentFrame();
  const phrases = [
    ['一个操作系统，', 18, 86],
    ['应该从哪里开始？', 88, 164],
    ['从第一条指令。', 170, 238],
    ['也从每一次真实运行。', 238, 288],
  ];
  return <Scene style={{background: '#050709'}}>
    <Grid size={160} opacity={0.11} />
    <div style={{position: 'absolute', left: 72, top: 48, fontFamily: mono, fontSize: 15, letterSpacing: '0.16em', color: '#6f7c7e'}}>FYXOS / BOOT MANIFEST 001</div>
    <div style={{position: 'absolute', right: 72, top: 48, fontFamily: mono, fontSize: 15, color: C.cyan}}>x86_64</div>
    <div style={{position: 'absolute', left: 140, right: 140, top: 405, height: 180}}>
      {phrases.map(([text, start, end], i) => <div key={text} style={{position: 'absolute', inset: 0, opacity: phase(frame, start, end), transform: `translateY(${(1 - progress(frame, start, 20)) * 26}px)`, fontSize: i === 3 ? 72 : 88, textAlign: 'center', fontWeight: 500, letterSpacing: '-0.045em', color: i >= 2 ? C.white : '#c4cdcc'}}>{text}</div>)}
    </div>
    <div style={{position: 'absolute', left: 72, bottom: 54, fontFamily: mono, fontSize: 16, lineHeight: 2, color: '#5f6d6f'}}>
      <div>0x0000_7C00&nbsp;&nbsp; BIOS ENTRY</div>
      <div style={{opacity: progress(frame, 70, 20)}}>0xFFFF_FFFF_8000_0000&nbsp;&nbsp; KERNEL</div>
    </div>
    <div style={{position: 'absolute', right: 72, bottom: 58, display: 'flex', alignItems: 'center', gap: 14, fontFamily: mono, fontSize: 14, letterSpacing: '0.12em', color: '#84918f'}}><span style={{width: 7, height: 7, borderRadius: 7, background: C.cyan, boxShadow: `0 0 18px ${C.cyan}`}} />FROM SCRATCH</div>
    <Beam frame={frame} />
  </Scene>;
};

const architecture = [
  ['00', 'BIOS', '自研引导器', '0x0000_7C00'],
  ['01', 'LONG MODE', '进入 x86_64', '64-BIT'],
  ['02', 'KERNEL', '自研内核', 'C11 + GNU AS'],
  ['03', 'RING 3', '用户态运行', 'USER SPACE'],
];

export const Architecture = () => {
  const frame = useCurrentFrame();
  return <Scene index="02" grid>
    <Reveal style={{position: 'absolute', left: 92, top: 160, width: 680}}>
      <Title eyebrow="ARCHITECTURE / 001">从零，<br />向上。</Title>
      <div style={{marginTop: 36, fontSize: 25, lineHeight: 1.8, color: '#9aa8a7'}}>不是包装一个已有内核。<br />让每一层，都有自己的实现。</div>
    </Reveal>
    <div style={{position: 'absolute', left: 926, top: 150, width: 2, height: 720, background: '#263437'}}>
      <div style={{width: 2, height: 720, background: C.cyan, transformOrigin: 'top', transform: `scaleY(${progress(frame, 12, 245)})`, boxShadow: `0 0 20px ${C.cyan}`}} />
    </div>
    <div style={{position: 'absolute', left: 860, top: 145, width: 900}}>
      {architecture.map(([n, name, cn, meta], i) => {
        const p = progress(frame, 20 + i * 55, 28);
        return <div key={name} style={{height: 142, marginBottom: 34, marginLeft: 64, padding: '25px 30px', boxSizing: 'border-box', border: `1px solid ${p > 0.6 ? '#36544f' : '#222c30'}`, borderRadius: 8, background: `rgba(15,24,27,${0.35 + p * 0.55})`, opacity: 0.25 + p * 0.75, transform: `translateX(${(1 - p) * 50}px)`, display: 'grid', gridTemplateColumns: '62px 1fr auto', alignItems: 'center'}}>
          <div style={{fontFamily: mono, fontSize: 18, color: p > 0.6 ? C.cyan : '#526063'}}>{n}</div>
          <div><div style={{fontFamily: mono, fontSize: 30, letterSpacing: '0.04em'}}>{name}</div><div style={{fontSize: 20, color: '#93a19f', marginTop: 7}}>{cn}</div></div>
          <div style={{fontFamily: mono, fontSize: 15, color: '#687779'}}>{meta}</div>
        </div>;
      })}
    </div>
    <SceneFooter><span>NO EXISTING KERNEL</span><span>BIOS -&gt; LONG MODE -&gt; KERNEL -&gt; RING 3</span></SceneFooter>
  </Scene>;
};

export const Desktop = () => {
  const frame = useCurrentFrame();
  const screenP = progress(frame, 16, 42);
  return <Scene index="03" light>
    <div style={{position: 'absolute', width: 1000, height: 1000, borderRadius: 1000, right: -180, top: 20, background: 'radial-gradient(circle,rgba(59,220,199,.18),rgba(59,220,199,0) 68%)'}} />
    <Reveal style={{position: 'absolute', left: 88, top: 142, width: 680}}>
      <Title eyebrow="A REAL DESKTOP" light size={74}>轻，<br />不止于体积。</Title>
    </Reveal>
    <div style={{position: 'absolute', left: 92, top: 490, display: 'flex', gap: 70}}>
      <Reveal delay={70}><Stat value="32" unit="MiB" label="原始启动镜像" light /></Reveal>
      <Reveal delay={88}><Stat value="38" label="系统调用" light /></Reveal>
      <Reveal delay={106}><Stat value="37" label="内置命令" light /></Reveal>
    </div>
    <Reveal delay={128} style={{position: 'absolute', left: 94, top: 720, display: 'flex', gap: 10, flexWrap: 'wrap', width: 650}}>
      <Pill light>FYXFS</Pill><Pill light>RING 3</Pill><Pill light>IPv4</Pill><Pill light>CoW RESTORE</Pill>
    </Reveal>
    <div style={{position: 'absolute', left: 850, top: 192, opacity: screenP, transform: `translateY(${(1 - screenP) * 65}px) perspective(1600px) rotateY(${(1 - screenP) * -5}deg)`}}>
      <Screen src="images/desktop.png" alt="FYXOS 真实 800×600 桌面" width={900} light />
      <div style={{marginTop: 20, display: 'flex', justifyContent: 'space-between', color: '#63716f', fontFamily: mono, fontSize: 15}}><span>ACTUAL FRAMEBUFFER / 800 × 600</span><span>build/full-desktop.png</span></div>
    </div>
    <SceneFooter light><span>SMALL FOOTPRINT. REAL SUBSTRATE.</span><span>产品画面来自实跑帧缓冲</span></SceneFooter>
  </Scene>;
};

const bootLines = [
  ['[fyxemu]', ' BIOS 引导 -> 长模式 · entry=0xffffffff80000000'],
  ['[test]', ' 自检全部通过（失败 0 项）'],
  ['[init]', ' FYXOS 用户态已启动（ring 3）'],
  ['[net]', ' DHCP 完成 · IP=10.0.2.15 · ICMP 往返成功'],
  ['fyxos>', ' run /bin/gcc-c11-demo'],
  ['', 'GCC_C11_OK'],
  ['[proc]', ' exit: pid=3 code=42'],
];

export const BootProof = () => {
  const frame = useCurrentFrame();
  return <Scene index="04" grid>
    <Reveal style={{position: 'absolute', left: 92, top: 128}}><Title eyebrow="BOOT / EXECUTE / VERIFY" size={72}>不是概念，是启动日志。</Title></Reveal>
    <Pop delay={20} style={{position: 'absolute', left: 92, top: 322, width: 1736}}>
      <Terminal title="RELEASE / boot-test.log">
        <div style={{height: 360}}>
          {bootLines.map(([prefix, text], i) => {
            const p = progress(frame, 34 + i * 27, 8);
            return <div key={text} style={{opacity: p, transform: `translateY(${(1 - p) * 10}px)`, color: i === 5 ? C.cyan : i === 4 ? C.white : '#a7b4b3', fontWeight: i === 5 ? 600 : 400}}><span style={{color: i === 4 ? C.cyan : '#607174'}}>{prefix}</span>{text}{i === 4 && frame < 160 ? <Cursor /> : null}</div>;
          })}
        </div>
      </Terminal>
    </Pop>
    <div style={{position: 'absolute', left: 110, right: 110, bottom: 122, display: 'flex', justifyContent: 'space-between', alignItems: 'center'}}>
      <Reveal delay={210} style={{display: 'flex', gap: 12}}><Pill>BIOS FIRST INSTRUCTION</Pill><Pill>RING 3 EXEC</Pill><Pill>NORMAL SHUTDOWN</Pill></Reveal>
      <Reveal delay={230} style={{fontFamily: mono, fontSize: 20, color: '#8c9b9b'}}>SOURCE&nbsp;&nbsp; release/boot-test.log</Reveal>
    </div>
  </Scene>;
};

const code = [
  ['#include <stdint.h>', '#8ad8ff'],
  ['#include <stdio.h>', '#8ad8ff'],
  ['', '#fff'],
  ['typedef int (*unary_function)(int);', '#d7c7ff'],
  ['struct computation {', '#d7c7ff'],
  ['    int values[5];', '#c3cfce'],
  ['    size_t count;', '#c3cfce'],
  ['    uint32_t tag;', '#c3cfce'],
  ['};', '#d7c7ff'],
  ['// …', '#607174'],
  ['_Static_assert(sizeof(uint32_t) == 4,', '#e6d68a'],
  ['    "FYXOS requires 32-bit uint32_t");', '#a9c99b'],
  ['', '#fff'],
  ['int main(int argc, char **argv, char **envp)', '#d7c7ff'],
  ['{', '#d7c7ff'],
  ['    (void)envp;', '#c3cfce'],
  ['    volatile int runtime_first = 4;', '#c3cfce'],
  ['    struct computation input = {', '#c3cfce'],
  ['        .values = { runtime_first, 2, 3, 4, 5 },', '#c3cfce'],
  ['        .count = 5, .kind = RESULT_RAW,', '#c3cfce'],
  ['        .tag = 0x46595831u,', '#c3cfce'],
  ['    };', '#c3cfce'],
  ['    unary_function operation = argc > 0 ? twice : increment;', '#c3cfce'],
  ['    int result = operation(sum_values(&input));', '#c3cfce'],
  ['    puts("GCC_C11_OK");', '#a9c99b'],
  ['    return result + 6;', '#e6d68a'],
  ['}', '#d7c7ff'],
];

export const SDK = () => {
  const frame = useCurrentFrame();
  const scroll = interpolate(frame, [120, 370], [0, 300], clamp);
  return <Scene index="05">
    <div style={{position: 'absolute', inset: 0, background: 'radial-gradient(circle at 78% 40%,rgba(62,214,193,.09),transparent 42%)'}} />
    <Reveal style={{position: 'absolute', left: 88, top: 126}}><Title eyebrow="OPEN TOOLCHAIN / REAL C11" size={68}>完整 C11，从开源工具链开始。</Title></Reveal>
    <Pop delay={24} style={{position: 'absolute', left: 88, top: 310, width: 1130}}>
      <Terminal title="HOST / examples/c11-demo.c">
        <div style={{height: 510, overflow: 'hidden', fontSize: 19, lineHeight: 1.62}}>
          <div style={{transform: `translateY(${-scroll}px)`}}>{code.map(([line, color], i) => <div key={i} style={{display: 'grid', gridTemplateColumns: '42px 1fr', color}}><span style={{color: '#435257', userSelect: 'none'}}>{String(i + 1).padStart(2, '0')}</span><span>{line || ' '}</span></div>)}</div>
        </div>
        <div style={{margin: '20px -32px -28px', padding: '18px 32px', borderTop: '1px solid #263137', background: '#10181b', color: C.cyan, fontSize: 18}}>$ bin/fyx-gcc examples/c11-demo.c -o c11-demo.elf</div>
      </Terminal>
    </Pop>
    <div style={{position: 'absolute', left: 1300, top: 342, width: 520}}>
      <Reveal delay={55}><div style={{fontFamily: mono, fontSize: 16, color: '#788789', letterSpacing: '0.12em'}}>LANGUAGE COVERAGE</div><div style={{marginTop: 24, display: 'flex', flexWrap: 'wrap', gap: 10}}><Pill>POINTERS</Pill><Pill>ARRAYS</Pill><Pill>STRUCTS</Pill><Pill>ENUMS</Pill><Pill>FUNCTION POINTERS</Pill><Pill>_STATIC_ASSERT</Pill></div></Reveal>
      <Reveal delay={130} style={{marginTop: 58, paddingTop: 30, borderTop: '1px solid #2c383c'}}><div style={{fontSize: 25, lineHeight: 1.7}}>GCC 在宿主编译。<br /><span style={{color: '#98a7a5'}}>目标是 FYXOS freestanding ELF。</span></div></Reveal>
      <Reveal delay={185} style={{marginTop: 50, padding: '24px 28px', border: '1px solid #31534d', borderRadius: 9, background: 'rgba(62,214,193,.06)'}}><div style={{fontFamily: mono, fontSize: 26, color: C.cyan}}>GCC_C11_OK</div><div style={{marginTop: 8, fontFamily: mono, fontSize: 18, color: '#91a19f'}}>guest exit code = 42</div></Reveal>
    </div>
    <SceneFooter><span>HOST GCC ≠ GUEST SELF-HOSTING</span><span>开源 GCC C11 SDK</span></SceneFooter>
  </Scene>;
};

export const Studio = () => {
  const frame = useCurrentFrame();
  const p = progress(frame, 20, 40);
  return <Scene index="06" grid>
    <Reveal style={{position: 'absolute', left: 88, top: 142, width: 620}}><Title eyebrow="FYX STUDIO" size={76}>系统之内，<br />直接创造。</Title><div style={{marginTop: 30, color: '#96a5a4', fontSize: 23, lineHeight: 1.8}}>编辑、保存、构建、运行。<br />每一步，都在真实系统中发生。</div></Reveal>
    <div style={{position: 'absolute', left: 92, top: 510, width: 590}}>
      {[['Ctrl-S', '保存源文件'], ['Ctrl-B', '构建原生 C 子集'], ['Ctrl-R', 'ring3 独立进程运行']].map(([key, label], i) => <Reveal key={key} delay={70 + i * 35} style={{display: 'grid', gridTemplateColumns: '120px 1fr', alignItems: 'center', padding: '20px 0', borderBottom: '1px solid #29363a'}}><span style={{fontFamily: mono, color: C.cyan, fontSize: 21}}>{key}</span><span style={{fontSize: 21, color: '#c2ccca'}}>{label}</span></Reveal>)}
      <Reveal delay={190} style={{marginTop: 28, fontSize: 18, lineHeight: 1.8, color: '#778789'}}>内置原生 C 子集 / FyxJS<br />与宿主 GCC C11 SDK 分工明确</Reveal>
    </div>
    <div style={{position: 'absolute', left: 790, top: 182, opacity: p, transform: `translateX(${(1 - p) * 70}px)`}}>
      <Screen src="images/studio.png" alt="Fyx Studio 实跑截图" width={1000} />
      <div style={{marginTop: 20, display: 'flex', justifyContent: 'space-between', color: '#71817f', fontFamily: mono, fontSize: 15}}><span>ACTUAL STUDIO FRAMEBUFFER</span><span>800 × 600</span></div>
    </div>
    <SceneFooter><span>EDIT -&gt; SAVE -&gt; BUILD -&gt; RUN</span><span>build/toolchain-test/studio.png</span></SceneFooter>
  </Scene>;
};

const appSteps = [
  {n: '01', title: '编译', subtitle: 'HOST / GCC C11', command: `$ build/fyxos-sdk/bin/fyx-gcc \\
    build/fyxos-sdk/examples/hello.c \\
    -o build/my-app.elf`, result: 'ELF64 · freestanding'},
  {n: '02', title: '打包', subtitle: 'IMAGE / APP IMAGE', command: `$ make app-image \\
    APP=build/my-app.elf`, result: '安装到 /bin/gcc-c11-demo'},
  {n: '03', title: '运行', subtitle: 'GUEST / RING 3', command: 'fyxos> run /bin/gcc-c11-demo', result: 'HELLO_FROM_GCC\nexit code = 0'},
];

export const AppImage = () => {
  const frame = useCurrentFrame();
  return <Scene index="07" light>
    <Reveal style={{position: 'absolute', left: 88, top: 126}}><Title eyebrow="YOUR CODE. YOUR IMAGE." light size={70}>从一段代码，到一次真实运行。</Title></Reveal>
    <div style={{position: 'absolute', left: 150, right: 150, top: 438, height: 2, background: '#ced9d5'}}><div style={{height: 2, width: '100%', background: C.cyanDark, transformOrigin: 'left', transform: `scaleX(${progress(frame, 50, 180)})`}} /></div>
    <div style={{position: 'absolute', left: 88, right: 88, top: 330, display: 'grid', gridTemplateColumns: 'repeat(3,1fr)', gap: 34}}>
      {appSteps.map((step, i) => <Pop key={step.n} delay={35 + i * 65} style={{height: 510, padding: '36px 34px', boxSizing: 'border-box', borderRadius: 12, border: '1px solid #d0dad7', background: i === 2 ? '#0b1214' : '#ffffff', color: i === 2 ? C.white : C.ink, boxShadow: '0 25px 70px rgba(25,55,50,.09)'}}>
        <div style={{display: 'flex', justifyContent: 'space-between', alignItems: 'center'}}><span style={{fontFamily: mono, fontSize: 18, color: i === 2 ? C.cyan : C.cyanDark}}>{step.n}</span><span style={{fontFamily: mono, fontSize: 13, letterSpacing: '0.1em', color: i === 2 ? '#6f8281' : '#84918e'}}>{step.subtitle}</span></div>
        <div style={{fontSize: 46, fontWeight: 600, marginTop: 34}}>{step.title}</div>
        <div style={{marginTop: 55, padding: '22px 0', borderTop: `1px solid ${i === 2 ? '#2b3a3d' : '#e0e7e4'}`, borderBottom: `1px solid ${i === 2 ? '#2b3a3d' : '#e0e7e4'}`, fontFamily: mono, whiteSpace: 'pre-wrap', fontSize: 17, lineHeight: 1.8, color: i === 2 ? '#d3dfdc' : '#40504e'}}>{step.command}</div>
        <div style={{marginTop: 36, whiteSpace: 'pre-line', fontFamily: mono, fontSize: i === 2 ? 23 : 18, lineHeight: 1.7, color: i === 2 ? C.cyan : C.cyanDark, opacity: progress(frame, 145 + i * 45, 20)}}>{step.result}</div>
      </Pop>)}
    </div>
    <SceneFooter light><span>COMPILE -&gt; PACKAGE -&gt; BOOT -&gt; EXIT 0</span><span>证据：release/app-image-test.log</span></SceneFooter>
  </Scene>;
};

export const Restore = () => {
  const frame = useCurrentFrame();
  const write = progress(frame, 72, 35);
  const restore = progress(frame, 205, 45);
  const changed = write * (1 - restore);
  return <Scene index="08" grid>
    <Reveal style={{position: 'absolute', left: 88, top: 126}}><Title eyebrow="FYXFS / COPY-ON-WRITE" size={70}>写得进去，也回得去。</Title></Reveal>
    <Pop delay={24} style={{position: 'absolute', left: 88, top: 320, width: 1120, height: 490, padding: '34px', boxSizing: 'border-box', border: '1px solid #2e3d40', borderRadius: 12, background: '#0d1417'}}>
      <div style={{display: 'flex', justifyContent: 'space-between', fontFamily: mono, fontSize: 15, letterSpacing: '0.1em', color: '#7e8f8f'}}><span>SYSTEM VOLUME / BLOCK MAP</span><span style={{color: C.cyan}}>{restore > 0.6 ? 'RESTORED' : write > 0.6 ? 'COW WRITE' : 'SNAPSHOT READY'}</span></div>
      <div style={{display: 'grid', gridTemplateColumns: 'repeat(8,1fr)', gap: 13, marginTop: 40}}>
        {Array.from({length: 32}, (_, i) => {
          const isChanged = [5, 6, 13, 18, 21, 26].includes(i);
          const local = isChanged ? progress(frame, 72 + i * 2, 25) * (1 - restore) : 0;
          return <div key={i} style={{height: 58, borderRadius: 4, border: `1px solid ${local > 0.4 ? '#4c9e90' : '#2b3b3e'}`, background: local > 0.4 ? `rgba(104,245,223,${0.12 + local * 0.34})` : '#151f22', position: 'relative', overflow: 'hidden'}}><div style={{position: 'absolute', left: 8, bottom: 6, fontFamily: mono, fontSize: 10, color: local > 0.4 ? C.cyan : '#4c6062'}}>{String(i).padStart(2, '0')}</div></div>;
        })}
      </div>
      <div style={{display: 'flex', gap: 14, marginTop: 38}}><Pill active={restore < 0.5}>SNAPSHOT</Pill><Pill active={write > 0.4 && restore < 0.5}>WRITE: changed</Pill><Pill active={restore > 0.5}>CONTENT: original</Pill></div>
    </Pop>
    <div style={{position: 'absolute', left: 1300, top: 340, width: 510}}>
      <Reveal delay={55}><div style={{fontFamily: mono, color: '#738486', fontSize: 15, letterSpacing: '0.12em'}}>VERIFIED BEHAVIOR</div></Reveal>
      {[['跨重启持久化', 'PERSIST'], ['CoW 快照与系统回滚', 'RESTORE'], ['/home 内容保留', 'PRESERVE'], ['文件系统校验', 'FSCK = 0']].map(([label, meta], i) => <Reveal key={label} delay={90 + i * 42} style={{padding: '25px 0', borderBottom: '1px solid #2a383b'}}><div style={{fontSize: 24}}>{label}</div><div style={{marginTop: 8, fontFamily: mono, fontSize: 15, color: C.cyan}}>{meta}</div></Reveal>)}
    </div>
    <div style={{position: 'absolute', left: 92, right: 92, bottom: 130, display: 'flex', alignItems: 'center', gap: 20, fontFamily: mono, fontSize: 15, color: '#728381'}}><span>SNAPSHOT</span><div style={{width: 220, height: 1, background: '#38514d'}} /><span>WRITE</span><div style={{width: 220, height: 1, background: '#38514d'}} /><span style={{color: restore > 0.4 ? C.cyan : '#728381'}}>RESTORE</span><div style={{flex: 1}} /><span>断电写点测试 · original / changed · fsck=0</span></div>
  </Scene>;
};

const testCategories = ['BIOS + 内核自检', 'ring3 + fork / exec', 'TCP / UDP / DNS', 'FYXFS 断电恢复', 'FYXGit + CLI', 'GCC C11 + 自定义应用', '800×600 图形桌面'];

export const Evidence = () => {
  const frame = useCurrentFrame();
  return <Scene index="09" light>
    <Reveal style={{position: 'absolute', left: 88, top: 120}}><Title eyebrow="REPRODUCIBLE ENGINEERING" light size={70}>证据，应该可以重跑。</Title></Reveal>
    <div style={{position: 'absolute', left: 88, right: 88, top: 320, display: 'grid', gridTemplateColumns: '1.4fr 1fr 1fr 1.25fr', gap: 40}}>
      {[['80', 'PASS', '自动化验收'], ['0', 'FAIL', '失败项'], ['0', 'RC', '测试返回码'], ['0', 'DIAG', '编译诊断']].map(([value, unit, label], i) => <Reveal key={unit} delay={25 + i * 20}><div style={{display: 'flex', alignItems: 'baseline', gap: 14}}><span style={{fontSize: 138, lineHeight: 1, letterSpacing: '-0.07em', fontWeight: 500}}>{value}</span><span style={{fontFamily: mono, color: C.cyanDark, fontSize: 20}}>{unit}</span></div><div style={{fontSize: 21, color: '#65736f', marginTop: 15}}>{label}</div></Reveal>)}
    </div>
    <div style={{position: 'absolute', left: 92, right: 92, top: 585}}>
      <div style={{display: 'grid', gridTemplateColumns: 'repeat(40,1fr)', gap: 7}}>
        {Array.from({length: 80}, (_, i) => {
          const p = progress(frame, 80 + i * 1.25, 10);
          return <div key={i} style={{height: 30, borderRadius: 3, background: p > 0.5 ? C.cyanDark : '#d9e1de', opacity: 0.35 + p * 0.65, transform: `scaleY(${0.45 + p * 0.55})`}} />;
        })}
      </div>
      <div style={{display: 'flex', justifyContent: 'space-between', marginTop: 14, fontFamily: mono, fontSize: 14, color: '#71807c'}}><span>000 / FIRST INSTRUCTION</span><span>080 / CUSTOM APP + RESTORE</span></div>
    </div>
    <Reveal delay={190} style={{position: 'absolute', left: 92, right: 92, top: 745, display: 'flex', flexWrap: 'wrap', gap: 10}}>{testCategories.map((name) => <Pill key={name} light>{name}</Pill>)}</Reveal>
    <div style={{position: 'absolute', left: 92, right: 92, bottom: 105, padding: '22px 28px', borderRadius: 8, background: '#0b1214', color: C.white, display: 'flex', justifyContent: 'space-between', alignItems: 'center', fontFamily: mono, fontSize: 18}}><span>release/full-test.log</span><span style={{color: C.cyan}}>80 PASS / 0 FAIL / RC=0</span></div>
  </Scene>;
};

const boundaries = [
  ['01', 'GCC 当前在宿主编译', '不是 guest 内自举'],
  ['02', '目标是 freestanding FYXOS ELF', '不是 Linux / glibc 兼容层'],
  ['03', '完整生态仍在路上', 'hosted libc · 动态链接 · 完整 POSIX · 全部语言'],
];

export const Honest = () => <Scene index="10">
  <div style={{position: 'absolute', inset: 0, background: 'radial-gradient(circle at 50% 20%,rgba(81,180,168,.08),transparent 50%)'}} />
  <Reveal style={{position: 'absolute', left: 0, right: 0, top: 130, textAlign: 'center'}}><div style={{fontFamily: mono, color: C.cyan, fontSize: 17, letterSpacing: '0.15em', marginBottom: 25}}>DEVELOPER PREVIEW / HONEST BOUNDARIES</div><div style={{fontSize: 76, fontWeight: 600, letterSpacing: '-0.045em'}}>这一站，不是终点。</div></Reveal>
  <div style={{position: 'absolute', left: 230, right: 230, top: 370}}>
    {boundaries.map(([n, title, sub], i) => <Reveal key={n} delay={45 + i * 48} style={{display: 'grid', gridTemplateColumns: '80px 1fr', padding: '30px 0', borderBottom: '1px solid #2b373b'}}><div style={{fontFamily: mono, fontSize: 17, color: C.cyan, paddingTop: 7}}>{n}</div><div><div style={{fontSize: 30, fontWeight: 500}}>{title}</div><div style={{fontSize: 20, color: '#82918f', marginTop: 10}}>{sub}</div></div></Reveal>)}
  </div>
  <Reveal delay={205} style={{position: 'absolute', left: 0, right: 0, bottom: 120, textAlign: 'center', fontSize: 23, color: '#a0aeac'}}>把已完成的说清楚，把未完成的也说清楚。</Reveal>
  <SceneFooter><span>NO INFLATED CLAIMS</span><span>为真实反馈而生</span></SceneFooter>
</Scene>;

export const Release = () => <Scene index="11" light>
  <div style={{position: 'absolute', right: -100, top: -200, width: 1000, height: 1000, borderRadius: 1000, background: 'radial-gradient(circle,rgba(65,220,198,.18),transparent 66%)'}} />
  <Reveal style={{position: 'absolute', left: 88, top: 128}}><Title eyebrow="FYXOS / AVAILABLE NOW" light size={68}>现在，轮到你启动。</Title></Reveal>
  <Reveal delay={40} style={{position: 'absolute', left: 82, top: 305}}><div style={{fontSize: 172, lineHeight: 1, fontWeight: 600, letterSpacing: '-0.075em'}}>0.1.0</div><div style={{marginTop: 24, display: 'flex', alignItems: 'center', gap: 18}}><Pill light>v0.1.0-dev1</Pill><span style={{fontFamily: mono, fontSize: 18, color: '#667672'}}>x86_64 / BIOS</span></div></Reveal>
  <div style={{position: 'absolute', left: 1050, right: 88, top: 250}}>
    {[['01', '压缩启动镜像', '32 MiB 原始镜像 · gzip 约 147 KiB'], ['02', '开源 GCC C11 SDK', 'CRT · 链接脚本 · 最小运行库 · 示例'], ['03', '完整验收证据', 'SHA256 · full-boot · 自定义应用 · 80 项日志']].map(([n, title, sub], i) => <Pop key={n} delay={70 + i * 45} style={{display: 'grid', gridTemplateColumns: '58px 1fr', padding: '28px 0', borderBottom: '1px solid #ccd7d3'}}><div style={{fontFamily: mono, color: C.cyanDark, fontSize: 17, paddingTop: 5}}>{n}</div><div><div style={{fontSize: 28, fontWeight: 600}}>{title}</div><div style={{fontSize: 18, color: '#667571', marginTop: 9}}>{sub}</div></div></Pop>)}
  </div>
  <Reveal delay={200} style={{position: 'absolute', left: 88, right: 88, bottom: 116, padding: '30px 36px', borderRadius: 10, background: '#0a1113', color: C.white, display: 'flex', justifyContent: 'space-between', alignItems: 'center'}}><div><div style={{fontFamily: mono, fontSize: 14, color: C.cyan, letterSpacing: '0.12em', marginBottom: 10}}>SOURCE + RELEASE + EVIDENCE</div><div style={{fontFamily: mono, fontSize: 25}}>github.com/CLRV-CLV/FYXOS</div></div><div style={{fontSize: 21, color: '#a8b5b2'}}>下载 · 解压 · 实跑</div></Reveal>
  <SceneFooter light><span>DEVELOPER PREVIEW 1</span><span>release/RELEASE_NOTES.md</span></SceneFooter>
</Scene>;

export const EndCard = () => {
  const frame = useCurrentFrame();
  const line = progress(frame, 16, 45);
  const fade = interpolate(frame, [174, 191], [1, 0], clamp);
  return <Scene style={{background: '#050809', opacity: fade}}>
    <Grid size={160} opacity={0.08} />
    <div style={{position: 'absolute', left: 0, right: 0, top: 330, textAlign: 'center'}}>
      <Reveal duration={34} y={22}><div style={{fontSize: 176, lineHeight: 1, fontWeight: 700, letterSpacing: '-0.07em'}}>FYXOS</div></Reveal>
      <div style={{margin: '48px auto 0', height: 2, width: 520, background: '#1f3432'}}><div style={{height: 2, background: C.cyan, width: '100%', transform: `scaleX(${line})`, boxShadow: `0 0 24px ${C.cyan}`}} /></div>
      <Reveal delay={35} style={{marginTop: 42, fontSize: 31, letterSpacing: '0.08em', color: '#b7c3c0'}}>从零开始。向前运行。</Reveal>
      <Reveal delay={62} style={{marginTop: 80, fontFamily: mono, fontSize: 17, lineHeight: 2.1, color: '#6f807e'}}><div>v0.1.0-dev1 · DEVELOPER PREVIEW</div><div style={{color: C.cyan}}>github.com/CLRV-CLV/FYXOS</div></Reveal>
    </div>
    <div style={{position: 'absolute', left: 72, right: 72, bottom: 48, display: 'flex', justifyContent: 'space-between', fontFamily: mono, fontSize: 13, letterSpacing: '0.12em', color: '#485a5a'}}><span>ORIGINAL SYSTEM. ORIGINAL FILM.</span><span>180 SEC / 24 FPS</span></div>
  </Scene>;
};
