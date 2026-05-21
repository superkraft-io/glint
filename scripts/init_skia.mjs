#!/usr/bin/env node

import fs from 'node:fs';
import https from 'node:https';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { createGunzip } from 'node:zlib';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const repoRoot = path.resolve(__dirname, '..', '..', '..');
const glintDir = path.resolve(__dirname, '..');
const depsDir = path.join(repoRoot, 'third_party', 'skia');
const skiaSrcDir = path.join(depsDir, 'src', 'skia');
const tmpDir = path.join(depsDir, 'tmp');

const libraryNames = [
  'skia',
  'skottie',
  'sksg',
  'skshaper',
  'skparagraph',
  'skunicode_icu',
  'skunicode_core',
  'svg',
  'freetype',
  'libpng',
  'zlib'
];

const VALID_BACKENDS = ['cpu', 'opengl', 'd3d12', 'dawn', 'metal'];
const VALID_TARGETS = ['win', 'mac', 'linux', 'ios-simulator', 'ios-device'];

function defaultTargetForPlatform(platform = process.platform) {
  if (platform === 'win32') return 'win';
  if (platform === 'darwin') return 'mac';
  if (platform === 'linux') return 'linux';
  return platform;
}

function printUsage() {
  console.log(`
Usage:
  node third_party/glint/scripts/init_skia.mjs --prebuilt [--backend <backend>] [--target <target>]
  node third_party/glint/scripts/init_skia.mjs --source  [--config Release|Debug|Both] [--backend <backend>] [--target <target>]

--prebuilt   Download prebuilt Skia libraries (fast, recommended for getting started)
--source     Build Skia from source (slower, required for custom configurations)

Backends (default: cpu):
  cpu     Software rasterizer, no GPU
  opengl  OpenGL (Ganesh backend)
  d3d12   Direct3D 12 (Graphite backend, Windows only)
  dawn    Dawn / WebGPU (Graphite backend)
  metal   Metal (macOS / iOS only)

Targets (default: host platform):
  win            Windows desktop
  mac            macOS desktop
  linux          Linux desktop
  ios-simulator  iOS Simulator
  ios-device     Physical iPhone / iPad

Options:
  --target <target>  Build for a specific target platform
  --skip-sync  Skip 'python tools/git-sync-deps' (use if deps are already present)

Output goes to: third_party/skia/
Generates:       third_party/glint/glint_render_backend.h

CMake usage after setup:
  -D GLINT_DEPS_DIR="<repo>/third_party/skia"
`);
}

function fail(message) {
  console.error(`Error: ${message}`);
  process.exit(1);
}

function parseArgs(argv) {
  const options = {
    source: false,
    help: false,
    prebuilt: false,
    config: 'Release',
    backend: 'cpu',
    target: defaultTargetForPlatform(),
    skipSync: false
  };

  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index];

    if (arg === '--help' || arg === '-h' || arg === '-help' || arg === '/?') {
      options.help = true;
      continue;
    }

    if (arg === '--source' || arg === '-source') {
      options.source = true;
      continue;
    }

    if (arg === '--prebuilt' || arg === '-prebuilt') {
      options.prebuilt = true;
      continue;
    }

    if (arg === '--config' || arg === '-config') {
      const next = argv[index + 1];
      if (!next) {
        fail('Missing value for --config. Expected Release, Debug, or Both.');
      }
      options.config = next;
      index += 1;
      continue;
    }

    if (arg === '--backend' || arg === '-backend') {
      const next = argv[index + 1];
      if (!next) {
        fail(`Missing value for --backend. Expected one of: ${VALID_BACKENDS.join(', ')}.`);
      }
      options.backend = next.toLowerCase();
      index += 1;
      continue;
    }

    if (arg === '--target' || arg === '-target') {
      const next = argv[index + 1];
      if (!next) {
        fail(`Missing value for --target. Expected one of: ${VALID_TARGETS.join(', ')}.`);
      }
      options.target = next.toLowerCase();
      index += 1;
      continue;
    }

    if (arg === '--skip-sync' || arg === '-skip-sync') {
      options.skipSync = true;
      continue;
    }

    fail(`Unknown argument: ${arg}`);
  }

  if (!['Release', 'Debug', 'Both'].includes(options.config)) {
    fail(`Invalid config: ${options.config}. Expected Release, Debug, or Both.`);
  }

  if (!VALID_BACKENDS.includes(options.backend)) {
    fail(`Invalid backend: ${options.backend}. Expected one of: ${VALID_BACKENDS.join(', ')}.`);
  }

  if (!VALID_TARGETS.includes(options.target)) {
    fail(`Invalid target: ${options.target}. Expected one of: ${VALID_TARGETS.join(', ')}.`);
  }

  return options;
}

function run(command, args, options = {}) {
  const isBatchFile = /\.(bat|cmd)$/i.test(command);
  const spawnedCommand = isBatchFile ? 'cmd.exe' : command;
  const spawnedArgs = isBatchFile ? ['/c', command, ...args] : args;

  const result = spawnSync(spawnedCommand, spawnedArgs, {
    cwd: options.cwd,
    env: options.env,
    stdio: options.captureOutput ? 'pipe' : 'inherit',
    encoding: 'utf8'
  });

  if (result.error) {
    throw result.error;
  }

  if (result.status !== 0) {
    if (options.allowFailure) {
      return result;
    }

    const rendered = [spawnedCommand, ...spawnedArgs].join(' ');
    fail(`Command failed (${result.status}): ${rendered}`);
  }

  return result;
}

function findCommand(commandNames) {
  const locator = process.platform === 'win32' ? 'where' : 'which';

  for (const commandName of commandNames) {
    const locateResult = spawnSync(locator, [commandName], {
      stdio: 'pipe',
      encoding: 'utf8'
    });

    const candidates = locateResult.status === 0
      ? (locateResult.stdout || '').trim().split(/\r?\n/).filter(Boolean)
      : [commandName];

    const resolvedCommand = process.platform === 'win32'
      ? (candidates.find((candidate) => !candidate.toLowerCase().includes('windowsapps')) || candidates[0])
      : candidates[0];

    const result = spawnSync(resolvedCommand, ['--version'], {
      stdio: 'pipe',
      encoding: 'utf8'
    });

    if (!result.error && result.status === 0) {
      return {
        name: resolvedCommand,
        binDir: resolvedCommand.includes('\\') || resolvedCommand.includes('/')
          ? path.dirname(resolvedCommand)
          : null,
        version: (result.stdout || result.stderr || '').trim().split(/\r?\n/)[0]
      };
    }
  }

  return null;
}

function findFirstExistingFile(paths) {
  for (const filePath of paths) {
    if (fs.existsSync(filePath)) {
      return filePath;
    }
  }

  return null;
}

function resolveClangToolchain() {
  const preferredClang = findFirstExistingFile([
    path.join('C:', 'Program Files', 'Microsoft Visual Studio', '2022', 'Community', 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang.exe'),
    path.join('C:', 'Program Files', 'LLVM', 'bin', 'clang.exe')
  ]);

  if (preferredClang) {
    const versionResult = spawnSync(preferredClang, ['--version'], {
      stdio: 'pipe',
      encoding: 'utf8'
    });

    return {
      command: preferredClang,
      binDir: path.dirname(preferredClang),
      rootDir: path.dirname(path.dirname(preferredClang)),
      version: (versionResult.stdout || versionResult.stderr || '').trim().split(/\r?\n/)[0]
    };
  }

  const clang = findCommand(['clang']);
  if (!clang) {
    return null;
  }

  const whereResult = spawnSync('where', ['clang'], {
    stdio: 'pipe',
    encoding: 'utf8'
  });

  const firstMatch = whereResult.status === 0
    ? (whereResult.stdout || '').trim().split(/\r?\n/).find(Boolean)
    : null;

  const clangPath = firstMatch || 'clang';
  const binDir = firstMatch ? path.dirname(firstMatch) : null;
  const rootDir = binDir ? path.dirname(binDir) : null;

  return {
    command: clangPath,
    binDir,
    rootDir,
    version: clang.version
  };
}

function resolveNinjaTool() {
  if (process.platform === 'win32') {
    const preferredNinja = findFirstExistingFile([
      path.join('C:', 'Program Files', 'Microsoft Visual Studio', '2022', 'Community', 'Common7', 'IDE', 'CommonExtensions', 'Microsoft', 'CMake', 'Ninja', 'ninja.exe')
    ]);

    if (preferredNinja) {
      const versionResult = spawnSync(preferredNinja, ['--version'], {
        stdio: 'pipe',
        encoding: 'utf8'
      });

      return {
        command: preferredNinja,
        binDir: path.dirname(preferredNinja),
        version: (versionResult.stdout || versionResult.stderr || '').trim().split(/\r?\n/)[0]
      };
    }
  }

  const ninja = findCommand(['ninja']);
  if (!ninja) {
    return null;
  }

  return {
    command: ninja.name,
    binDir: ninja.binDir,
    version: ninja.version
  };
}

function ensureDirectory(dirPath) {
  fs.mkdirSync(dirPath, { recursive: true });
}

function hasSkiaSourceTree(dirPath) {
  return fs.existsSync(path.join(dirPath, 'include', 'core', 'SkCanvas.h'));
}

function hasSkiaGnBuildFiles(dirPath) {
  return hasSkiaSourceTree(dirPath)
    && fs.existsSync(path.join(dirPath, '.gn'))
    && fs.existsSync(path.join(dirPath, 'BUILD.gn'))
    && fs.existsSync(path.join(dirPath, 'gn', 'skia.gni'));
}

function isGitCheckout(dirPath) {
  return fs.existsSync(path.join(dirPath, '.git'));
}

function hasReusableSkiaDeps(dirPath) {
  const requiredPaths = [
    path.join(dirPath, 'third_party', 'externals', 'zlib', 'adler32.c')
  ];

  return requiredPaths.every((requiredPath) => fs.existsSync(requiredPath));
}

function resolveGnExecutable(sourceDir) {
  const candidates = [
    path.join(sourceDir, 'bin', 'gn.exe'),
    path.join(sourceDir, 'bin', 'gn.bat'),
    path.join(sourceDir, 'bin', 'gn'),
    path.join(skiaSrcDir, 'bin', 'gn.exe'),
    path.join(skiaSrcDir, 'bin', 'gn.bat'),
    path.join(skiaSrcDir, 'bin', 'gn'),
    path.join(tmpDir, 'depot_tools', 'gn.exe'),
    path.join(tmpDir, 'depot_tools', 'gn.bat'),
    path.join(tmpDir, 'depot_tools', 'gn')
  ];

  for (const candidate of candidates) {
    if (process.platform !== 'win32' && /\.(exe|bat)$/i.test(candidate)) {
      continue;
    }
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }

  fail(`Unable to find gn under ${path.join(skiaSrcDir, 'bin')}.`);
}

function buildBackendGnFlags(backend) {
  switch (backend) {
    case 'opengl':
      return 'skia_use_gl = true\nskia_use_dawn = false\nskia_use_direct3d = false\nskia_use_metal = false\nskia_enable_gpu = true\nskia_enable_graphite = false';
    case 'd3d12':
      return 'skia_use_gl = false\nskia_use_dawn = false\nskia_use_direct3d = true\nskia_use_metal = false\nskia_enable_gpu = true\nskia_enable_graphite = true';
    case 'dawn':
      return 'skia_use_gl = false\nskia_use_dawn = true\nskia_use_direct3d = false\nskia_use_metal = false\nskia_enable_gpu = true\nskia_enable_graphite = true';
    case 'metal':
      return 'skia_use_gl = false\nskia_use_dawn = false\nskia_use_direct3d = false\nskia_use_metal = true\nskia_enable_gpu = true\nskia_enable_graphite = false';
    case 'cpu':
    default:
      return 'skia_use_gl = false\nskia_use_dawn = false\nskia_use_direct3d = false\nskia_use_metal = false\nskia_enable_gpu = false\nskia_enable_graphite = false';
  }
}

function resolveArch(target) {
  if (target === 'ios-device') return 'arm64';
  const arch = os.arch();
  if (arch === 'arm64') return 'arm64';
  return 'x64';
}

function buildGnArgs(configName, backend, target) {
  const isDebug = configName === 'Debug';
  const arch = resolveArch(target);

  const common = `is_debug = ${isDebug ? 'true' : 'false'}
is_official_build = ${isDebug ? 'false' : 'true'}
skia_use_system_libjpeg_turbo = false
skia_use_system_libpng = false
skia_use_system_libwebp = false
skia_use_system_zlib = false
skia_use_system_expat = false
skia_use_system_icu = false
skia_use_system_harfbuzz = false
skia_use_libwebp_decode = true
skia_use_libwebp_encode = false
skia_use_xps = false
skia_use_dng_sdk = false
skia_use_expat = true
skia_use_icu = true
${buildBackendGnFlags(backend)}
skia_enable_svg = true
skia_enable_skottie = true
skia_enable_pdf = false
skia_enable_skparagraph = true
skia_enable_tools = false
target_cpu = "${arch}"`;

  if (target === 'ios-simulator' || target === 'ios-device') {
    return `${common}
target_os = "ios"
ios_use_simulator = ${target === 'ios-simulator' ? 'true' : 'false'}
skia_ios_use_signing = false
cc = "clang"
cxx = "clang++"
extra_cflags = [ "-stdlib=libc++" ]
extra_ldflags = [ "-stdlib=libc++" ]`;
  }

  if (target === 'win') {
    const extraCFlag = isDebug ? '"/MTd"' : '"/MT"';
    return `${common}\nextra_cflags = [ ${extraCFlag} ]`;
  }

  if (target === 'mac') {
    return `${common}\ncc = "clang"\ncxx = "clang++"\nextra_cflags = [ "-stdlib=libc++" ]\nextra_ldflags = [ "-stdlib=libc++" ]`;
  }

  return `${common}\ncc = "clang"\ncxx = "clang++"\nskia_use_freetype = true\nskia_use_system_freetype2 = false\nskia_use_fontconfig = true\nskia_use_system_fontconfig = true`;
}

function copyLinuxLibraries(outDir, configName, arch) {
  const libDst = path.join(depsDir, 'linux', arch, configName);
  ensureDirectory(libDst);

  for (const libraryName of libraryNames) {
    const srcLib = path.join(outDir, `lib${libraryName}.a`);
    if (fs.existsSync(srcLib)) {
      fs.copyFileSync(srcLib, path.join(libDst, `lib${libraryName}.a`));
    }
  }

  if (configName === 'Release') {
    const icuFile = path.join(outDir, 'icudtl.dat');
    if (fs.existsSync(icuFile)) {
      const binDst = path.join(depsDir, 'linux', 'bin');
      ensureDirectory(binDst);
      fs.copyFileSync(icuFile, path.join(binDst, 'icudtl.dat'));
    }
  }
}

function copyAppleLibraries(outDir, configName, arch, target) {
  const platformDir = target === 'ios-simulator'
    ? 'ios-simulator'
    : target === 'ios-device'
      ? 'ios-device'
      : 'mac';
  const libDst = path.join(depsDir, platformDir, arch, configName);
  ensureDirectory(libDst);

  for (const libraryName of libraryNames) {
    const srcLib = path.join(outDir, `lib${libraryName}.a`);
    if (fs.existsSync(srcLib)) {
      fs.copyFileSync(srcLib, path.join(libDst, `lib${libraryName}.a`));
    }
  }
}

function copyLibraries(outDir, configName) {
  const libDst = path.join(depsDir, 'win', 'x64', configName);
  ensureDirectory(libDst);

  for (const libraryName of libraryNames) {
    const srcLib = path.join(outDir, `${libraryName}.lib`);
    if (fs.existsSync(srcLib)) {
      fs.copyFileSync(srcLib, path.join(libDst, `${libraryName}.lib`));
    }
  }

  if (configName === 'Release') {
    const icuFile = path.join(outDir, 'icudtl.dat');
    if (fs.existsSync(icuFile)) {
      const binDst = path.join(depsDir, 'win', 'bin');
      ensureDirectory(binDst);
      fs.copyFileSync(icuFile, path.join(binDst, 'icudtl.dat'));
    }
  }
}

const BACKEND_MACRO = {
  cpu:    'GLINT_RENDER_BACKEND_CPU',
  opengl: 'GLINT_RENDER_BACKEND_OPENGL',
  d3d12:  'GLINT_RENDER_BACKEND_D3D12',
  dawn:   'GLINT_RENDER_BACKEND_DAWN',
  metal:  'GLINT_RENDER_BACKEND_METAL'
};

const BACKEND_DERIVED_DEFINES = {
  cpu:    [],
  opengl: ['#define GLINT_RENDER_GPU 1'],
  d3d12:  ['#define GLINT_RENDER_GPU 1', '#define GLINT_ENABLE_D3D12 1', '#define SK_GANESH 1', '#define SK_DIRECT3D 1'],
  dawn:   ['#define GLINT_RENDER_GPU 1'],
  metal:  ['#define GLINT_RENDER_GPU 1']
};

function writeRenderBackendHeader(backend) {
  const headerPath = path.join(glintDir, 'glint_render_backend.h');
  const macro = BACKEND_MACRO[backend];
  const derivedDefines = BACKEND_DERIVED_DEFINES[backend];
  const lines = [
    '#pragma once',
    '',
    '// Generated by init_skia.mjs — do not edit manually.',
    '',
    '#define GLINT_RENDER_BACKEND_CPU    0',
    '#define GLINT_RENDER_BACKEND_OPENGL 1',
    '#define GLINT_RENDER_BACKEND_D3D12  2',
    '#define GLINT_RENDER_BACKEND_DAWN   3',
    '#define GLINT_RENDER_BACKEND_METAL  4',
    '',
    `#define GLINT_RENDER_BACKEND ${macro}`,
  ];
  if (derivedDefines.length > 0) {
    lines.push('', ...derivedDefines);
  }
  lines.push('');
  fs.writeFileSync(headerPath, lines.join('\n'), 'utf8');
  console.log(`Wrote render backend header: ${headerPath}`);
}

const PREBUILT_URLS = {
  win32:  'https://github.com/superkraft-io/glint-skia-prebuilt/releases/download/Release/glint-skia-prebuilt-win.zip',
  darwin: 'https://github.com/superkraft-io/glint-skia-prebuilt/releases/download/Release/glint-skia-prebuilt-mac.zip',
  linux:  'https://github.com/superkraft-io/glint-skia-prebuilt/releases/download/Release/glint-skia-prebuilt-linux.zip'
};

function httpsGetFollowRedirects(url) {
  return new Promise((resolve, reject) => {
    function request(currentUrl) {
      https.get(currentUrl, (res) => {
        if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
          request(res.headers.location);
          return;
        }
        if (res.statusCode !== 200) {
          reject(new Error(`HTTP ${res.statusCode} for ${currentUrl}`));
          return;
        }
        resolve(res);
      }).on('error', reject);
    }
    request(url);
  });
}

async function downloadFile(url, destPath) {
  const res = await httpsGetFollowRedirects(url);
  await new Promise((resolve, reject) => {
    const out = fs.createWriteStream(destPath);
    res.pipe(out);
    out.on('finish', resolve);
    out.on('error', reject);
    res.on('error', reject);
  });
}

async function extractZip(zipPath, destDir) {
  if (process.platform === 'win32') {
    const result = spawnSync('powershell', [
      '-NoProfile', '-NonInteractive', '-Command',
      `$ErrorActionPreference = 'Stop'; Expand-Archive -Force -Path "${zipPath}" -DestinationPath "${destDir}"`
    ], { stdio: 'inherit' });
    if (result.status !== 0) fail('Failed to extract zip with PowerShell Expand-Archive.');
  } else {
    // On Linux/macOS, zip files created on Windows store backslash path separators.
    // unzip treats backslashes as filename characters (not separators), dumping everything flat.
    // Use Python's zipfile module with explicit backslash→slash normalisation to handle both cases.
    const pyScript = [
      'import zipfile, os, sys',
      'with zipfile.ZipFile(sys.argv[1]) as z:',
      '    for m in z.namelist():',
      '        p = m.replace("\\\\", "/").replace("\\\\\\\\", "/")',
      '        t = os.path.join(sys.argv[2], p)',
      '        if p.endswith("/"):',
      '            os.makedirs(t, exist_ok=True)',
      '        else:',
      '            os.makedirs(os.path.dirname(t), exist_ok=True)',
      '            data = z.read(m)',
      '            open(t, "wb").write(data)',
    ].join('\n');

    const python = findCommand(['python3', 'python']);
    if (!python) fail('python3 or python is required to extract the prebuilt zip on Linux/macOS. Install Python 3 and retry.');

    const result = spawnSync(python.name, ['-c', pyScript, zipPath, destDir], { stdio: 'inherit' });
    if (result.status !== 0) fail('Failed to extract zip with Python zipfile.');
  }
}

async function downloadPrebuilt(backend) {
  const platform = process.platform;
  const url = PREBUILT_URLS[platform];

  if (!url) {
    fail(`No prebuilt package available for platform: ${platform}. Use --source instead.`);
  }

  console.log(`Downloading prebuilt Skia for ${platform} (${backend.toUpperCase()})...`);
  console.log(`  URL: ${url}`);

  ensureDirectory(depsDir);
  ensureDirectory(tmpDir);

  const zipPath = path.join(tmpDir, 'glint-skia-prebuilt.zip');
  await downloadFile(url, zipPath);
  console.log('  Download complete.');

  console.log(`Extracting to ${depsDir}...`);
  await extractZip(zipPath, depsDir);
  console.log('  Extraction complete.');

  fs.rmSync(zipPath, { force: true });
}

function main() {
  const options = parseArgs(process.argv.slice(2));

  if (options.prebuilt) {
    if (options.help) {
      printUsage();
      process.exit(0);
    }

    if (options.target !== defaultTargetForPlatform()) {
      fail(`Prebuilt Skia packages are only available for the host platform target (${defaultTargetForPlatform()}). Use --source for ${options.target}.`);
    }

    writeRenderBackendHeader(options.backend);
    downloadPrebuilt(options.backend).then(() => {
      console.log('Prebuilt Skia ready.');
    }).catch((err) => {
      fail(`Prebuilt download failed: ${err.message}`);
    });
    return;
  }

  if (options.help || !options.source) {
    printUsage();
    process.exit(0);
  }

  console.log(`Render backend: ${options.backend.toUpperCase()}`);

  // Write the header immediately so CMake can configure even if the Skia build
  // fails or is interrupted later in this script.
  writeRenderBackendHeader(options.backend);

  if (process.platform !== 'win32' && process.platform !== 'darwin' && process.platform !== 'linux') {
    fail(`init_skia.mjs supports Windows, macOS, and Linux. Current platform: ${os.platform()}.`);
  }

  if (options.target === 'ios-simulator' || options.target === 'ios-device' || options.target === 'mac') {
    if (process.platform !== 'darwin') {
      fail(`Target '${options.target}' can only be built on macOS.`);
    }
  }

  if (options.target === 'win' && process.platform !== 'win32') {
    fail(`Target 'win' can only be built on Windows.`);
  }

  if (options.target === 'linux' && process.platform !== 'linux') {
    fail(`Target 'linux' can only be built on Linux.`);
  }

  if (options.target === 'linux' && (options.backend === 'd3d12' || options.backend === 'metal')) {
    fail(`Backend '${options.backend}' is not supported on Linux. Use --backend opengl or --backend cpu.`);
  }

  if (options.target === 'mac' && (options.backend === 'd3d12' || options.backend === 'opengl')) {
    fail(`Backend '${options.backend}' is not supported on macOS. Use --backend metal or --backend cpu.`);
  }

  if ((options.target === 'ios-simulator' || options.target === 'ios-device')
      && (options.backend === 'd3d12' || options.backend === 'opengl' || options.backend === 'dawn')) {
    fail(`Backend '${options.backend}' is not supported on iOS. Use --backend metal or --backend cpu.`);
  }

  if (options.target === 'win' && options.backend === 'metal') {
    fail(`Backend 'metal' is only supported on macOS.`);
  }

  console.log('Checking prerequisites...');

  const python = findCommand(['python', 'python3']);
  if (!python) fail('Python 3 not found. Install it and add it to PATH.');
  console.log(`  Python: ${python.version}`);

  const ninja = resolveNinjaTool();
  if (!ninja) fail('ninja not found. Install Ninja or the Visual Studio CMake tools.');
  console.log(`  Ninja: ${ninja.version}`);

  const git = findCommand(['git']);
  if (!git) fail('git not found. Install Git and add it to PATH.');
  console.log(`  Git: ${git.version}`);

  ensureDirectory(depsDir);
  ensureDirectory(tmpDir);

  const depotDir = path.join(tmpDir, 'depot_tools');
  if (!fs.existsSync(depotDir)) {
    console.log('Cloning depot_tools...');
    run('git', ['clone', '--depth', '1', 'https://chromium.googlesource.com/chromium/tools/depot_tools.git', depotDir]);
  } else {
    console.log('depot_tools already present, skipping clone.');
  }

  const env = {
    ...process.env,
    PATH: `${python.binDir ? `${python.binDir}${path.delimiter}` : ''}${depotDir}${path.delimiter}${ninja.binDir ? `${ninja.binDir}${path.delimiter}` : ''}${process.env.PATH || ''}`
  };

  const tempBuildSrcDir = path.join(tmpDir, 'build-src', 'skia');
  let activeSkiaSrcDir = skiaSrcDir;

  if (hasSkiaGnBuildFiles(skiaSrcDir)) {
    console.log('Using existing vendored Skia source tree.');
  } else if (hasSkiaSourceTree(skiaSrcDir)) {
    activeSkiaSrcDir = tempBuildSrcDir;
    console.log('Vendored Skia source tree is incomplete for GN builds; using a temporary full checkout for compilation.');
  }

  const activeSkiaIsGitCheckout = isGitCheckout(activeSkiaSrcDir);
  if (!activeSkiaIsGitCheckout && !hasSkiaSourceTree(activeSkiaSrcDir)) {
    console.log('Cloning Skia (shallow, chrome/m130)...');
    ensureDirectory(path.dirname(activeSkiaSrcDir));
    run('git', ['clone', '--depth', '1', '--branch', 'chrome/m130', 'https://skia.googlesource.com/skia.git', activeSkiaSrcDir], { env });
  } else if (activeSkiaIsGitCheckout) {
    console.log('Skia already cloned, skipping.');
  } else {
    console.log('Using existing vendored Skia source tree, skipping clone.');
  }

  if (isGitCheckout(activeSkiaSrcDir) && !options.skipSync) {
    console.log('Syncing Skia deps (python tools/git-sync-deps)...');
    console.log('  (This downloads third-party deps from chromium.googlesource.com and may take several minutes.)');
    console.log('  (Re-run with --skip-sync to skip this step if deps are already present.)');
    const syncResult = run(python.name, ['tools/git-sync-deps'], {
      cwd: activeSkiaSrcDir,
      env,
      allowFailure: true
    });

    if (syncResult.status !== 0) {
      if (!hasReusableSkiaDeps(activeSkiaSrcDir)) {
        fail('Skia dependency sync failed and required local deps are still missing. Seed the checkout or retry when chromium.googlesource.com is reachable.');
      }

      console.warn('Warning: Skia dependency sync failed; continuing with existing local deps.');
    }
  } else if (isGitCheckout(activeSkiaSrcDir) && options.skipSync) {
    console.log('Skipping git-sync-deps (--skip-sync).');
    if (!hasReusableSkiaDeps(activeSkiaSrcDir)) {
      fail('--skip-sync was passed but required Skia deps are missing. Run without --skip-sync first.');
    }
  } else {
    console.log('Skipping git-sync-deps because the vendored Skia source tree is not a git checkout.');
  }

  const configs = options.config === 'Both' ? ['Release', 'Debug'] : [options.config];
  const gnExecutable = resolveGnExecutable(activeSkiaSrcDir);
  const arch = resolveArch(options.target);

  // On Linux, the repo lives on the Windows NTFS filesystem (mounted via
  // WSL's 9P driver at /mnt/c/).  Heavy parallel writes to that path
  // cause "IO failure on output stream" errors in clang/ninja.
  // Build to the native Linux filesystem instead, then copy the .a files.
  // Use /var/tmp (not /tmp) so partial builds survive a WSL restart.
  const linuxNativeBuildBase = process.platform === 'linux'
    ? '/var/tmp/skia-build-glint'
    : null;

  for (const configName of configs) {
    const outDir = process.platform === 'linux'
      ? path.join(linuxNativeBuildBase, arch, configName)
      : process.platform === 'darwin'
        ? path.join(tmpDir, 'build', options.target, arch, configName)
        : path.join(tmpDir, 'build', 'x64', configName);
    ensureDirectory(outDir);

    console.log(`Generating GN build files for ${configName} (${options.target})...`);
    run(gnExecutable, ['gen', outDir, `--args=${buildGnArgs(configName, options.backend, options.target)}`], { cwd: activeSkiaSrcDir, env });

    console.log(`Building Skia ${configName} for ${options.target} with ninja...`);
    // On Linux (WSL) cap parallelism to reduce concurrent I/O.
    // WSL2 kernel can produce transient I/O errors under very high
    // parallel write load; -j4 keeps throughput reasonable while staying
    // well below the failure threshold.
    const ninjaJobArgs = process.platform === 'linux' ? ['-j4'] : [];
    run(ninja.command, ['-C', outDir, ...ninjaJobArgs], { env });

    console.log(`Copying ${configName} libs...`);
    if (options.target === 'mac' || options.target === 'ios-simulator' || options.target === 'ios-device') {
      copyAppleLibraries(outDir, configName, arch, options.target);
    } else if (process.platform === 'linux') {
      copyLinuxLibraries(outDir, configName, arch);
    } else {
      copyLibraries(outDir, configName);
    }
  }

  console.log('Cleaning up tmp build directory...');
  if (linuxNativeBuildBase) {
    fs.rmSync(linuxNativeBuildBase, { recursive: true, force: true });
  } else {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }

  console.log('');
  console.log('Done! Built Skia is at:');
  console.log(`  ${depsDir}`);
  console.log('');
  console.log('Add to your CMake configure:');
  console.log(`  -D GLINT_DEPS_DIR="${depsDir}"`);
}

main();