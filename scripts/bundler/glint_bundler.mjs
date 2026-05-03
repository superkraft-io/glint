/**
 * glint_bundler.mjs
 *
 * Bundles a directory of web assets (CSS, images, fonts, etc.) into C++ headers
 * for use with glint's onRequest / fromBuffer API.
 *
 * Usage:
 *   node glint_bundler.mjs --input <dir> --output <dir> [--namespace <name>] [--mode deep|shallow]
 *
 *   --input      Root directory to bundle (e.g. glint_user_code/web)
 *   --output     Directory where generated headers will be written
 *   --namespace  C++ namespace for generated code (default: glint_bundle)
 *   --mode       deep    = embed all bytes in headers (Release)
 *                shallow = headers contain metadata only, data loaded from disk (Debug)
 */

import fs   from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { createRequire } from 'module';

const __filename = fileURLToPath(import.meta.url);
const __dirname  = path.dirname(__filename);

const require           = createRequire(import.meta.url);
const utils             = require('./modules/glint_utils.js');
const lister            = require('./modules/glint_file_lister.js');
const grouper           = require('./modules/glint_files_grouper.js');
const foldersAssembler  = require('./modules/glint_folders_assembler.js');

// ── Parse args ───────────────────────────────────────────────────────────────
const args      = utils.parseArgs(process.argv.slice(2));
const inputDir  = args.input     ? path.resolve(args.input)  : null;
const outputDir = args.output    ? path.resolve(args.output) : null;
const namespace = args.namespace || 'glint_bundle';
const mode      = args.mode      || 'deep';

if (!inputDir)  utils.reportError({ msg: '[glint_bundler] --input is required' });
if (!outputDir) utils.reportError({ msg: '[glint_bundler] --output is required' });
if (!fs.existsSync(inputDir)) utils.reportError({ msg: `[glint_bundler] --input path does not exist: ${inputDir}` });
if (!['deep', 'shallow'].includes(mode)) utils.reportError({ msg: `[glint_bundler] --mode must be "deep" or "shallow", got: ${mode}` });

// ── Globals (mirrors skxx pattern, used by modules) ──────────────────────────
global.glint = { bundle_mode: mode };
global.bundleNamespace = namespace;

global.soft_backend_root    = inputDir;
global.bundleRoot           = outputDir;
global.bundleDeepRoot       = path.join(outputDir, 'deep');
global.deepGroupsRoot       = path.join(outputDir, 'deep', 'groups');
global.bundleShallowRoot    = path.join(outputDir, 'shallow');
global.shallowGroupsRoot    = path.join(outputDir, 'shallow', 'groups');
global.shallowGroupsDataRoot = path.join(outputDir, 'shallow', 'groups', 'data');

// ── Helpers ──────────────────────────────────────────────────────────────────
function modTemplate(templatePath, dstPath, onReplaceCB) {
    let tpl = fs.readFileSync(templatePath, 'utf8');
    tpl = onReplaceCB(tpl);
    fs.writeFileSync(dstPath, tpl);
}

// ── Main ─────────────────────────────────────────────────────────────────────
const run = async () => {
    console.log(`[glint_bundler] mode      : ${mode}`);
    console.log(`[glint_bundler] input     : ${inputDir}`);
    console.log(`[glint_bundler] output    : ${outputDir}`);
    console.log(`[glint_bundler] namespace : ${namespace}`);

    // 1. Check for locked files
    console.log('[glint_bundler] Checking for locked files...');
    const LockChecker = require(`./modules/lockChecker/lockChecker_${utils.getOS()}.js`);
    const lockChecker = new LockChecker();
    await lockChecker.init();
    const lockedFiles = await lockChecker.checkFiles();

    if (lockedFiles.length > 0) {
        const pids = [...new Set(lockedFiles.flatMap(f => (f.procList || []).map(p => p.pid)))];
        utils.reportError({ keepAlive: true, msg: `[glint_bundler] Some files are locked. Close the locking applications then try again.` });
        if (pids.length) utils.reportError({ keepAlive: true, msg: `taskkill /PID ${pids.join(' /PID ')} /F` });
        for (const f of lockedFiles) {
            if (f.status === 'not_found') utils.reportError({ keepAlive: true, msg: `[glint_bundler] File not found: "${f.path}"` });
            else utils.reportError({ keepAlive: true, msg: `[glint_bundler] Locked: "${path.basename(f.path)}" by ${(f.procList || []).map(p => `${p.name} (PID ${p.pid})`).join(', ')}` });
        }
        process.exit(1);
    }

    // 2. Clean and recreate output dirs
    console.log('[glint_bundler] Cleaning previous bundle...');
    try { fs.rmSync(outputDir, { recursive: true, force: true }); } catch {}
    fs.mkdirSync(bundleDeepRoot,        { recursive: true });
    fs.mkdirSync(deepGroupsRoot,        { recursive: true });
    fs.mkdirSync(bundleShallowRoot,     { recursive: true });
    fs.mkdirSync(shallowGroupsRoot,     { recursive: true });
    fs.mkdirSync(shallowGroupsDataRoot, { recursive: true });

    // 3. List files
    console.log('[glint_bundler] Listing files...');
    const allEntries = lister.listFiles(inputDir);
    const fileCount  = allEntries.filter(e => !e.isFolder).length;
    console.log(`[glint_bundler] Found ${fileCount} files`);

    // 4. Group files and write per-group headers
    console.log('[glint_bundler] Grouping and writing group headers...');
    const groupResult   = grouper.forFiles({ files: allEntries, groupSize: 0.3 });
    const folderEntries = foldersAssembler.forFolders({ folders: allEntries });

    // 5. Write master headers from templates
    const T = (name) => path.resolve(__dirname, 'templates', name);
    const O = (name) => path.join(outputDir, name);

    modTemplate(T('glint_bundle_group_root_template.hpp'), O('glint_bundle_group_root.hpp'), data =>
        data.split('<!namespace!>').join(namespace)
    );

    modTemplate(T('glint_bundle_groups_template.hpp'), O('glint_bundle_groups.hpp'), data =>
        data.split('<!namespace!>').join(namespace)
            .replace('<!groups!>', groupResult.groupsDefs + '\n')
    );

    modTemplate(T('glint_bundle_entry_template.hpp'), O('glint_bundle_entry.hpp'), data =>
        data.split('<!namespace!>').join(namespace)
    );

    modTemplate(T('glint_bundle_entries_template.hpp'), O('glint_bundle_entries_files.hpp'), data =>
        data.split('<!namespace!>').join(namespace)
            .split('<!type!>').join('Files')
            .replace('<!entries!>', groupResult.entriesDefs + '\n')
    );

    modTemplate(T('glint_bundle_entries_template.hpp'), O('glint_bundle_entries_folders.hpp'), data =>
        data.split('<!namespace!>').join(namespace)
            .split('<!type!>').join('Folders')
            .replace('<!entries!>', folderEntries.join(',\n') + '\n')
    );

    modTemplate(T('glint_bundle_include_template.hpp'), O('glint_bundle_include.hpp'), data =>
        data.replace('<!deep_group_includes!>',    groupResult.includesDef.deep)
            .replace('<!shallow_group_includes!>', groupResult.includesDef.shallow)
    );

    modTemplate(T('glint_bundle_library_template.hpp'), O('glint_bundle_library.hpp'), data =>
        data.split('<!namespace!>').join(namespace)
    );

    console.log(`[glint_bundler] Written to: ${outputDir}`);
    console.log('[glint_bundler] Done.');
};

run();

