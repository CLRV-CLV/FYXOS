import path from 'node:path';
import {fileURLToPath} from 'node:url';
import chromium, {inflate, setupLambdaEnvironment} from '@sparticuz/chromium';

const root = path.dirname(fileURLToPath(import.meta.url));

export const prepareBrowser = async () => {
  chromium.setGraphicsMode = false;
  const [browserExecutable, libraryRoot] = await Promise.all([
    chromium.executablePath(),
    inflate(path.join(root, 'node_modules/@sparticuz/chromium/bin/al2023.tar.br')),
  ]);
  setupLambdaEnvironment(path.join(libraryRoot, 'lib'));
  return browserExecutable;
};

export const chromiumOptions = {headless: true, enableMultiProcessOnLinux: true};
