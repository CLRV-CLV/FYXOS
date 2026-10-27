import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {bundle} from '@remotion/bundler';
import {renderStill, selectComposition} from '@remotion/renderer';
import {chromiumOptions, prepareBrowser} from './browser.mjs';

const root = path.dirname(fileURLToPath(import.meta.url));
const outputDir = path.join(root, '..', 'build', 'video-stills');
fs.mkdirSync(outputDir, {recursive: true});
const browserExecutable = await prepareBrowser();
const serveUrl = await bundle({entryPoint: path.join(root, 'src/index.jsx'), publicDir: path.join(root, 'public')});
const composition = await selectComposition({serveUrl, id: 'FyxLaunch', browserExecutable, chromiumOptions});
const frames = process.argv.slice(2).length
  ? process.argv.slice(2).map(Number)
  : [144, 456, 840, 1224, 1600, 2016, 2440, 2860, 3290, 3670, 3990, 4200];
for (const frame of frames) {
  const outputLocation = path.join(outputDir, `frame-${String(frame).padStart(4, '0')}.png`);
  await renderStill({serveUrl, composition, frame, output: outputLocation, browserExecutable, chromiumOptions, imageFormat: 'png', overwrite: true, timeoutInMilliseconds: 120000});
  console.log(`STILL ${frame} ${outputLocation}`);
}
