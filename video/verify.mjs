import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';

const root = path.dirname(fileURLToPath(import.meta.url));
const repo = path.join(root, '..');
const video = path.join(repo, 'release', 'fyxos-developer-preview-launch.mp4');
const bin = path.join(root, 'node_modules', '@remotion', 'compositor-linux-x64-gnu');
const ffprobe = path.join(bin, 'ffprobe');
const ffmpeg = path.join(bin, 'ffmpeg');
const assert = (condition, message) => { if (!condition) throw new Error(message); };
const run = (command, args, options = {}) => {
  const result = spawnSync(command, args, {encoding: 'buffer', maxBuffer: 64 * 1024 * 1024, ...options});
  if (result.status !== 0) throw new Error(`${path.basename(command)} failed (${result.status}): ${result.stderr?.toString() || result.stdout?.toString()}`);
  return result.stdout;
};

assert(fs.existsSync(video), `Missing rendered video: ${video}`);
const probe = JSON.parse(run(ffprobe, ['-v', 'error', '-count_packets', '-show_entries', 'stream=index,codec_type,codec_name,width,height,pix_fmt,r_frame_rate,avg_frame_rate,nb_read_packets,duration,sample_rate,channels:format=duration,size', '-of', 'json', video]).toString());
const streams = probe.streams;
const videoStreams = streams.filter((stream) => stream.codec_type === 'video');
const audioStreams = streams.filter((stream) => stream.codec_type === 'audio');
assert(videoStreams.length === 1, `Expected exactly one video stream, got ${videoStreams.length}`);
assert(audioStreams.length === 1, `Expected exactly one audio stream, got ${audioStreams.length}`);
const v = videoStreams[0];
const a = audioStreams[0];
assert(v.codec_name === 'h264', `Expected H.264, got ${v.codec_name}`);
assert(v.width === 1920 && v.height === 1080, `Expected 1920x1080, got ${v.width}x${v.height}`);
assert(['yuv420p', 'yuvj420p'].includes(v.pix_fmt), `Expected 8-bit 4:2:0 video, got ${v.pix_fmt}`);
assert(v.r_frame_rate === '24/1', `Expected 24 fps, got ${v.r_frame_rate}`);
assert(Number(v.nb_read_packets) === 4320, `Expected 4320 video packets, got ${v.nb_read_packets}`);
assert(a.codec_name === 'aac', `Expected AAC, got ${a.codec_name}`);
assert(a.channels === 2, `Expected stereo audio, got ${a.channels}`);
const duration = Number(probe.format.duration);
assert(Math.abs(duration - 180) < 0.1, `Expected 180 seconds, got ${duration}`);

// Decode every audio and video packet. Any corrupt frame or AAC segment fails here.
run(ffmpeg, ['-v', 'error', '-i', video, '-c:v', 'rawvideo', '-c:a', 'pcm_s16le', '-f', 'null', '-']);

// Decode the finished MP4's audio, not just the source score, and measure 18 windows.
const sampleRate = 22050;
const wav = run(ffmpeg, ['-v', 'error', '-i', video, '-map', '0:a:0', '-ac', '1', '-ar', String(sampleRate), '-f', 'wav', '-']);
assert(wav.toString('ascii', 0, 4) === 'RIFF' && wav.toString('ascii', 8, 12) === 'WAVE', 'Audio decode did not produce WAV');
let chunkOffset = 12;
let dataOffset = -1;
let dataSize = 0;
while (chunkOffset + 8 <= wav.length) {
  const id = wav.toString('ascii', chunkOffset, chunkOffset + 4);
  const declaredSize = wav.readUInt32LE(chunkOffset + 4);
  const available = wav.length - chunkOffset - 8;
  const size = declaredSize === 0xffffffff ? available : Math.min(declaredSize, available);
  if (id === 'data') {
    dataOffset = chunkOffset + 8;
    dataSize = size;
    break;
  }
  chunkOffset += 8 + declaredSize + (declaredSize % 2);
}
assert(dataOffset >= 0 && dataSize > 0, 'WAV data chunk missing');
const pcm = wav.subarray(dataOffset, dataOffset + dataSize);
assert(pcm.length > sampleRate * 2 * 179, `Decoded audio is too short: ${pcm.length} bytes`);
let peak = 0;
let squareSum = 0;
const count = Math.floor(pcm.length / 2);
const windows = Array.from({length: 18}, () => ({sum: 0, count: 0}));
for (let i = 0; i < count; i++) {
  const value = pcm.readInt16LE(i * 2);
  const absolute = Math.abs(value);
  peak = Math.max(peak, absolute);
  squareSum += value * value;
  const window = Math.min(17, Math.floor(i / (sampleRate * 10)));
  windows[window].sum += value * value;
  windows[window].count++;
}
const db = (rms) => Number((20 * Math.log10(Math.max(rms, 1) / 32768)).toFixed(2));
const rms = Math.sqrt(squareSum / count);
const windowDb = windows.map((window) => db(Math.sqrt(window.sum / window.count)));
assert(peak > 1000 && peak <= 32767, `Unexpected audio peak: ${peak}`);
assert(rms > 100, `Finished audio is effectively silent: RMS ${rms}`);
assert(windowDb.every((level) => level > -60), `Unexpected silent 10-second window: ${windowDb.join(', ')}`);

const hash = crypto.createHash('sha256').update(fs.readFileSync(video)).digest('hex');
const report = {
  result: 'PASS',
  file: 'release/fyxos-developer-preview-launch.mp4',
  sha256: hash,
  bytes: fs.statSync(video).size,
  durationSeconds: duration,
  video: {codec: v.codec_name, width: v.width, height: v.height, pixelFormat: v.pix_fmt, fps: 24, frames: Number(v.nb_read_packets)},
  audio: {codec: a.codec_name, channels: a.channels, outputSampleRate: Number(a.sample_rate), decodedPeakDbfs: db(peak), decodedRmsDbfs: db(rms), tenSecondRmsDbfs: windowDb},
  checks: ['container probe', '4320 video packets', 'full H.264/AAC decode', '180-second duration', '1080p24 8-bit 4:2:0', 'stereo AAC', 'non-silent finished audio', 'SHA-256'],
  renderer: 'Remotion 4.0.539',
};
fs.writeFileSync(path.join(repo, 'release', 'video-verification.json'), `${JSON.stringify(report, null, 2)}\n`);
console.log(JSON.stringify(report, null, 2));
console.log('VIDEO_VERIFY=PASS');
