import React from 'react';
import {Easing, Img, interpolate, spring, staticFile, useCurrentFrame, useVideoConfig} from 'remotion';

export const C = {
  ink: '#080b0e',
  paper: '#f3f5f4',
  white: '#f8fbfa',
  cyan: '#68f5df',
  cyanDark: '#087f78',
  muted: '#8a969b',
  line: '#263137',
};

export const font = 'Noto Sans SC, DejaVu Sans, sans-serif';
export const mono = 'DejaVu Sans Mono, Noto Sans SC, monospace';
const clamp = {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'};

export const progress = (frame, start, duration = 24) => interpolate(frame, [start, start + duration], [0, 1], {...clamp, easing: Easing.out(Easing.cubic)});

export const Reveal = ({children, delay = 0, duration = 24, y = 34, style = {}}) => {
  const frame = useCurrentFrame();
  const p = progress(frame, delay, duration);
  return <div style={{opacity: p, transform: `translateY(${(1 - p) * y}px)`, ...style}}>{children}</div>;
};

export const Pop = ({children, delay = 0, style = {}}) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  const p = spring({frame: frame - delay, fps, config: {damping: 200, stiffness: 120, mass: 0.8}});
  return <div style={{opacity: p, transform: `translateY(${(1 - p) * 28}px) scale(${0.97 + p * 0.03})`, ...style}}>{children}</div>;
};

export const Grid = ({light = false, size = 120, opacity = 0.16}) => {
  const color = light ? '20,40,44' : '140,240,230';
  return <div style={{position: 'absolute', inset: 0, pointerEvents: 'none', opacity, backgroundImage: `linear-gradient(rgba(${color},.16) 1px,transparent 1px),linear-gradient(90deg,rgba(${color},.16) 1px,transparent 1px)`, backgroundSize: `${size}px ${size}px`}} />;
};

export const Scene = ({children, light = false, index, grid = false, style = {}}) => {
  const frame = useCurrentFrame();
  const enter = interpolate(frame, [0, 8], [0, 1], clamp);
  return <div style={{position: 'absolute', inset: 0, overflow: 'hidden', background: light ? C.paper : C.ink, color: light ? C.ink : C.white, fontFamily: font, opacity: enter, ...style}}>
    {grid ? <Grid light={light} /> : null}
    {index ? <Header light={light} index={index} /> : null}
    {children}
  </div>;
};

export const Header = ({light = false, index = '01'}) => <div style={{position: 'absolute', zIndex: 20, top: 44, left: 72, right: 72, display: 'flex', alignItems: 'center', justifyContent: 'space-between', fontSize: 16, letterSpacing: '0.14em', color: light ? '#536064' : '#91a09f'}}>
  <div style={{display: 'flex', alignItems: 'center', gap: 22}}><span style={{color: light ? C.ink : C.white, fontWeight: 700, letterSpacing: '0.06em'}}>FYXOS</span><span>DEVELOPER PREVIEW</span></div>
  <div style={{fontFamily: mono, letterSpacing: '0.08em'}}>{index} / 12</div>
</div>;

export const Title = ({eyebrow, children, light = false, size = 82, style = {}}) => <div style={style}>
  {eyebrow ? <div style={{fontFamily: mono, color: light ? C.cyanDark : C.cyan, fontSize: 18, letterSpacing: '0.15em', marginBottom: 24}}>{eyebrow}</div> : null}
  <div style={{fontSize: size, lineHeight: 1.13, letterSpacing: '-0.045em', fontWeight: 600}}>{children}</div>
</div>;

export const Pill = ({children, light = false, active = true, style = {}}) => <div style={{display: 'inline-flex', alignItems: 'center', minHeight: 40, padding: '0 17px', borderRadius: 999, border: `1px solid ${light ? '#c8d1cf' : '#344348'}`, color: light ? (active ? C.cyanDark : '#536064') : (active ? C.cyan : '#a1adae'), background: light ? 'rgba(255,255,255,.55)' : 'rgba(255,255,255,.025)', fontFamily: mono, fontSize: 17, letterSpacing: '0.02em', ...style}}>{children}</div>;

export const Stat = ({value, unit, label, light = false, style = {}}) => <div style={style}>
  <div style={{display: 'flex', alignItems: 'baseline', gap: 10}}><span style={{fontSize: 82, lineHeight: 1, fontWeight: 500, letterSpacing: '-0.055em', fontVariantNumeric: 'tabular-nums'}}>{value}</span>{unit ? <span style={{fontSize: 24, color: light ? C.cyanDark : C.cyan, fontFamily: mono}}>{unit}</span> : null}</div>
  <div style={{marginTop: 16, fontSize: 20, color: light ? '#64716f' : '#9ba8a7', letterSpacing: '0.04em'}}>{label}</div>
</div>;

export const Screen = ({src, alt, width, style = {}, light = false}) => <div style={{width, padding: 10, borderRadius: 12, background: light ? '#ffffff' : '#141b1f', border: `1px solid ${light ? '#d4ddda' : '#344147'}`, boxShadow: light ? '0 30px 90px rgba(15,35,35,.15)' : '0 34px 100px rgba(0,0,0,.45)', ...style}}>
  <Img src={staticFile(src)} alt={alt} style={{display: 'block', width: '100%', borderRadius: 5}} />
</div>;

export const Terminal = ({children, title = 'FYXOS / SERIAL', light = false, style = {}}) => <div style={{overflow: 'hidden', borderRadius: 12, border: `1px solid ${light ? '#cbd5d2' : '#303d42'}`, background: light ? '#ffffff' : '#0d1215', boxShadow: light ? '0 26px 80px rgba(20,45,45,.12)' : '0 30px 90px rgba(0,0,0,.35)', ...style}}>
  <div style={{height: 54, padding: '0 24px', display: 'flex', alignItems: 'center', justifyContent: 'space-between', borderBottom: `1px solid ${light ? '#e2e8e6' : '#263137'}`, color: light ? '#64716f' : '#8c9b9c', fontFamily: mono, fontSize: 15, letterSpacing: '0.1em'}}><span>{title}</span><span style={{color: light ? C.cyanDark : C.cyan, display: 'inline-flex', alignItems: 'center', gap: 9}}><span style={{width: 6, height: 6, borderRadius: 6, background: 'currentColor'}} />LIVE EVIDENCE</span></div>
  <div style={{padding: '28px 32px', fontFamily: mono, fontSize: 21, lineHeight: 1.85, whiteSpace: 'pre-wrap'}}>{children}</div>
</div>;

export const Cursor = ({light = false}) => {
  const frame = useCurrentFrame();
  return <span style={{display: 'inline-block', width: 11, height: 25, marginLeft: 7, verticalAlign: '-3px', background: light ? C.cyanDark : C.cyan, opacity: interpolate(frame % 24, [0, 11, 12, 23], [1, 1, 0.15, 0.15])}} />;
};

export const SceneFooter = ({children, light = false}) => <div style={{position: 'absolute', left: 72, right: 72, bottom: 42, display: 'flex', justifyContent: 'space-between', alignItems: 'center', color: light ? '#6c7977' : '#758382', fontFamily: mono, fontSize: 15, letterSpacing: '0.08em'}}>{children}</div>;

export const Beam = ({frame, top = 0, height = 1080, light = false}) => {
  const x = interpolate(frame, [0, 120], [-300, 2200], clamp);
  return <div style={{position: 'absolute', top, left: 0, width: 2, height, background: light ? C.cyanDark : C.cyan, boxShadow: `0 0 45px 8px ${light ? 'rgba(8,127,120,.18)' : 'rgba(104,245,223,.24)'}`, opacity: 0.5, transform: `translateX(${x}px)`}} />;
};
