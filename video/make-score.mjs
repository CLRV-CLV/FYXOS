import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';

const root = path.dirname(fileURLToPath(import.meta.url));
const sampleRate = 32000;
const duration = 180;
const samples = sampleRate * duration;
const left = new Float32Array(samples);
const right = new Float32Array(samples);
const tau = Math.PI * 2;
const midi = (note) => 440 * 2 ** ((note - 69) / 12);
let randomState = 0x46595831;
const random = () => {
  randomState ^= randomState << 13;
  randomState ^= randomState >>> 17;
  randomState ^= randomState << 5;
  return ((randomState >>> 0) / 4294967296) * 2 - 1;
};

const sections = [
  [0, 12, 0.14], [12, 26, 0.34], [26, 44, 0.48], [44, 58, 0.65],
  [58, 75, 0.66], [75, 93, 0.74], [93, 110, 0.84], [110, 128, 0.43],
  [128, 146, 1.0], [146, 160, 0.22], [160, 172, 0.72], [172, 180, 0.38],
];
const intensity = (time) => sections.find(([start, end]) => time >= start && time < end)?.[2] ?? 0.3;

function tone(start, length, frequency, amplitude, options = {}) {
  const {pan = 0, kind = 'pad', attack = 0.02, release = 0.25} = options;
  const begin = Math.max(0, Math.floor(start * sampleRate));
  const end = Math.min(samples, Math.ceil((start + length) * sampleRate));
  const gainL = Math.cos((pan + 1) * Math.PI / 4);
  const gainR = Math.sin((pan + 1) * Math.PI / 4);
  let phase = 0;
  for (let i = begin; i < end; i++) {
    const t = (i - begin) / sampleRate;
    const remaining = (end - i) / sampleRate;
    const a = Math.min(1, t / Math.max(attack, 0.0001));
    const r = Math.min(1, remaining / Math.max(release, 0.0001));
    const envelope = Math.sin(a * Math.PI / 2) * Math.sin(r * Math.PI / 2);
    phase += tau * frequency / sampleRate;
    let wave;
    if (kind === 'pluck') {
      wave = (Math.sin(phase) + 0.32 * Math.sin(phase * 2) + 0.1 * Math.sin(phase * 3)) * Math.exp(-t * 5.2);
    } else if (kind === 'bell') {
      wave = (Math.sin(phase) + 0.34 * Math.sin(phase * 2.005) + 0.12 * Math.sin(phase * 3.98)) * Math.exp(-t * 2.8);
    } else if (kind === 'bass') {
      wave = Math.sin(phase) + 0.24 * Math.sin(phase * 2);
    } else {
      wave = Math.sin(phase) + 0.18 * Math.sin(phase * 2) + 0.055 * Math.sin(phase * 3);
    }
    const value = wave * amplitude * envelope;
    left[i] += value * gainL;
    right[i] += value * gainR;
  }
}

function noise(start, length, amplitude, options = {}) {
  const {pan = 0, highpass = true, decay = 16} = options;
  const begin = Math.max(0, Math.floor(start * sampleRate));
  const end = Math.min(samples, Math.ceil((start + length) * sampleRate));
  const gainL = Math.cos((pan + 1) * Math.PI / 4);
  const gainR = Math.sin((pan + 1) * Math.PI / 4);
  let filtered = 0;
  for (let i = begin; i < end; i++) {
    const t = (i - begin) / sampleRate;
    const n = random();
    filtered += 0.18 * (n - filtered);
    const value = (highpass ? n - filtered : filtered) * amplitude * Math.exp(-t * decay);
    left[i] += value * gainL;
    right[i] += value * gainR;
  }
}

function kick(start, amplitude) {
  const length = 0.42;
  const begin = Math.floor(start * sampleRate);
  const end = Math.min(samples, begin + Math.floor(length * sampleRate));
  let phase = 0;
  for (let i = begin; i < end; i++) {
    const t = (i - begin) / sampleRate;
    const frequency = 43 + 95 * Math.exp(-t * 22);
    phase += tau * frequency / sampleRate;
    const value = (Math.sin(phase) + 0.1 * random() * Math.exp(-t * 45)) * amplitude * Math.exp(-t * 10);
    left[i] += value * 0.72;
    right[i] += value * 0.72;
  }
}

const chords = [
  [50, 57, 60, 64, 69],
  [46, 53, 57, 60, 65],
  [48, 55, 60, 62, 67],
  [45, 52, 55, 60, 64],
];

// Slow harmonic bed. Each four-second chord is original synthesis, not a sample.
for (let start = 0, index = 0; start < duration; start += 4, index++) {
  const chord = chords[index % chords.length];
  const level = intensity(start + 2);
  chord.forEach((note, noteIndex) => {
    tone(start, Math.min(5.8, duration - start), midi(note), 0.022 * level, {
      pan: (noteIndex - 2) * 0.22,
      attack: start === 0 ? 2.8 : 1.1,
      release: 1.5,
    });
  });
  tone(start, Math.min(5.2, duration - start), midi(chord[0] - 12), 0.018 * level, {kind: 'bass', attack: 1.2, release: 1.4});
}

// A restrained pulse follows the product demonstrations and engineering proof.
const beat = 60 / 96;
for (let start = 0, index = 0; start < 176; start += beat, index++) {
  const level = intensity(start);
  const active = (start >= 26 && start < 110) || (start >= 128 && start < 146) || (start >= 160 && start < 172);
  if (!active) continue;
  const chord = chords[Math.floor(start / 4) % chords.length];
  if (index % 2 === 0) tone(start, 0.48, midi(chord[0] - 12), 0.065 * level, {kind: 'bass', attack: 0.012, release: 0.2});
  if (start >= 44) kick(start, 0.12 * level);
  noise(start, 0.11, 0.018 * level, {pan: index % 2 ? 0.35 : -0.35, decay: 34});
  if (index % 4 === 2) noise(start, 0.22, 0.025 * level, {highpass: false, decay: 20});
}

// Glass-like arpeggios, deliberately absent during the boundary chapter.
for (let start = 12, index = 0; start < 177; start += beat / 2, index++) {
  const active = (start >= 26 && start < 110) || (start >= 128 && start < 146) || (start >= 160 && start < 177);
  if (!active) continue;
  const level = intensity(start);
  const chord = chords[Math.floor(start / 4) % chords.length];
  const note = chord[(index * 3) % chord.length] + 12 + (index % 8 === 7 ? 12 : 0);
  tone(start, 0.55, midi(note), 0.042 * level, {kind: 'pluck', pan: index % 2 ? 0.48 : -0.48, attack: 0.004, release: 0.18});
}

// Boot ticks and chapter transitions tie the score to the edit.
for (let start = 1.25; start < 12; start += 1.25) {
  tone(start, 0.35, midi(86), 0.025, {kind: 'bell', pan: Math.sin(start) * 0.5, attack: 0.002, release: 0.15});
}
for (const start of [12, 26, 44, 58, 75, 93, 110, 128, 146, 160, 172]) {
  const level = intensity(start);
  tone(start, 1.8, midi(38), 0.085 * level, {kind: 'bass', attack: 0.008, release: 1.1});
  tone(start + 0.03, 1.1, midi(74), 0.028 * level, {kind: 'bell', pan: 0.25, attack: 0.003, release: 0.7});
  noise(Math.max(0, start - 0.35), 0.5, 0.018 * level, {highpass: false, decay: 6});
}
[50, 57, 62, 64, 69].forEach((note, i) => tone(172, 8, midi(note), 0.025, {pan: (i - 2) * 0.25, attack: 1.4, release: 3.5}));

let peak = 0;
for (let i = 0; i < samples; i++) {
  const time = i / sampleRate;
  const fade = Math.min(1, time / 2.5, (duration - time) / 4.0);
  left[i] = Math.tanh(left[i] * 1.35) * fade;
  right[i] = Math.tanh(right[i] * 1.35) * fade;
  peak = Math.max(peak, Math.abs(left[i]), Math.abs(right[i]));
}
const normalization = 0.89 / Math.max(peak, 0.0001);
const pcm = Buffer.alloc(samples * 4);
for (let i = 0; i < samples; i++) {
  pcm.writeInt16LE(Math.round(Math.max(-1, Math.min(1, left[i] * normalization)) * 32767), i * 4);
  pcm.writeInt16LE(Math.round(Math.max(-1, Math.min(1, right[i] * normalization)) * 32767), i * 4 + 2);
}
const header = Buffer.alloc(44);
header.write('RIFF', 0);
header.writeUInt32LE(36 + pcm.length, 4);
header.write('WAVEfmt ', 8);
header.writeUInt32LE(16, 16);
header.writeUInt16LE(1, 20);
header.writeUInt16LE(2, 22);
header.writeUInt32LE(sampleRate, 24);
header.writeUInt32LE(sampleRate * 4, 28);
header.writeUInt16LE(4, 32);
header.writeUInt16LE(16, 34);
header.write('data', 36);
header.writeUInt32LE(pcm.length, 40);

const buildDir = path.join(root, '..', 'build');
const outputDir = path.join(root, 'public', 'audio');
fs.mkdirSync(buildDir, {recursive: true});
fs.mkdirSync(outputDir, {recursive: true});
const wav = path.join(buildDir, 'fyxos-score.wav');
const output = path.join(outputDir, 'fyxos-score.m4a');
fs.writeFileSync(wav, Buffer.concat([header, pcm]));
const ffmpeg = path.join(root, 'node_modules/@remotion/compositor-linux-x64-gnu/ffmpeg');
const encoded = spawnSync(ffmpeg, ['-y', '-hide_banner', '-loglevel', 'error', '-i', wav, '-c:a', 'aac', '-b:a', '192k', '-f', 'mp4', '-movflags', '+faststart', '-metadata', 'title=FYXOS — First Instruction', '-metadata', 'artist=FYXOS Project', output], {stdio: 'inherit'});
if (encoded.status !== 0) process.exit(encoded.status ?? 1);
fs.unlinkSync(wav);
console.log(`SCORE=${output}`);
console.log(`DURATION=${duration}s SAMPLE_RATE=${sampleRate} PEAK=${(20 * Math.log10(0.89)).toFixed(2)}dBFS`);
