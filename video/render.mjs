import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {bundle} from '@remotion/bundler';
import {renderMedia, selectComposition} from '@remotion/renderer';
import {chromiumOptions, prepareBrowser} from './browser.mjs';

const root = path.dirname(fileURLToPath(import.meta.url));
const smoke = process.argv.includes('--smoke');
const browserExecutable = await prepareBrowser();
const serveUrl = await bundle({
  entryPoint: path.join(root, 'src/index.jsx'),
  publicDir: path.join(root, 'public'),
});
const composition = await selectComposition({
  serveUrl,
  id: 'FyxLaunch',
  browserExecutable,
  chromiumOptions,
});
const outputLocation = smoke
  ? path.join(root, '..', 'build', 'video-smoke.mp4')
  : path.join(root, '..', 'release', 'fyxos-developer-preview-launch.mp4');
let lastPercent = -10;
await renderMedia({
  serveUrl,
  composition,
  codec: 'h264',
  outputLocation,
  browserExecutable,
  chromiumOptions,
  frameRange: smoke ? [0, 47] : null,
  concurrency: 2,
  crf: 18,
  x264Preset: 'medium',
  pixelFormat: 'yuv420p',
  audioCodec: 'aac',
  audioBitrate: '192k',
  overwrite: true,
  timeoutInMilliseconds: 120000,
  onProgress: ({progress}) => {
    const percent = Math.floor(progress * 100);
    if (percent >= lastPercent + 5) {
      lastPercent = percent;
      console.log(`render ${percent}%`);
    }
  },
});
console.log(`VIDEO=${outputLocation}`);
