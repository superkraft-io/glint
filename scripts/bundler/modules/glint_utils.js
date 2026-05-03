const os = require('os');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

module.exports = {
    getCallerInfo(depth = 1) {
        // Create a clean stack that excludes this helper itself
        const holder = {};
        Error.captureStackTrace(holder, this.getCallerInfo);
        const lines = String(holder.stack || "").split("\n").slice(1);

        // Parse "at func (file:line:col)" OR "at file:line:col"
        const parse = (l) => {
            const m =
            l.match(/\s*at .* \((.*):(\d+):(\d+)\)\s*$/) ||
            l.match(/\s*at (.*):(\d+):(\d+)\s*$/);
            if (!m) return null;
            return {
            file: m[1],
            line: Number(m[2]),
            column: Number(m[3]),
            raw: l.trim(),
            };
        };

        // Keep only user files (skip Node internals & eval)
        const userFrames = [];
        for (const l of lines) {
            const f = parse(l);
            if (!f) continue;
            const p = f.file;

            // Exclude Node internals and VM/eval wrappers
            const isInternal =
            p.startsWith("node:") ||
            p.includes(`${require("path").sep}internal${require("path").sep}`) ||
            p === "internal" ||
            p.startsWith("evalmachine") ||
            p === "<anonymous>";

            if (!isInternal) userFrames.push(f);
        }

        // depth=0 → immediate caller; depth=1 → caller's caller, etc.
        return userFrames[depth] || userFrames[0] || null;
    },

    getOS(asFullName) {
        var os = require('os');
        var currentOS = os.platform()
        if (currentOS === 'win32') currentOS = 'win'
        else if (currentOS === 'darwin') currentOS = 'macos'
        else if (currentOS === 'linux') currentOS = 'linux'
        else currentOS = 'unknown'

        if (asFullName) {
            if (currentOS === 'win') return 'Windows'
            else if (currentOS === 'macos') return 'MacOS'
            else if (currentOS === 'linux') return 'Linux'
            else return 'Unknown'
        }

        return currentOS
    },

    parseArgs(argv) {
        const out = {};
        for (let i = 0; i < argv.length; i++) {
            const a = argv[i];
            if (a.startsWith('--')) {
                const k = a.slice(2);
                const v = (i + 1 < argv.length && !argv[i + 1].startsWith('--')) ? argv[++i] : true;
                out[k] = v;
            }
        }
        return out;
    },

    reportError(opts) {
        var defOpts = {
            msg: 'Unknown error',
            file: '',
            line: -1,
            col: 1,
            code: 'SKERR000',
            keepAlive: false
        }

        defOpts = {...defOpts, ...opts}

        if (defOpts.line < 0){
            var callerInfo = this.getCallerInfo(1)
            if (!defOpts.file) defOpts.file = callerInfo.file
            if (!defOpts.line) defOpts.line = callerInfo.line
            if (!defOpts.col) defOpts.col = callerInfo.column
        }

        const abs = path.resolve(defOpts.file);

        console.error(`${abs}(${defOpts.line},${defOpts.col}): error ${defOpts.code}: ${defOpts.msg}`);

        if (!defOpts.keepAlive) process.exit(1)
    },
    
    runPs1(scriptPath, args = []){
        const shell = 'powershell.exe'

        return new Promise((resolve, reject) => {
            var fullArgs = [
                '-NoProfile',
                '-ExecutionPolicy', 'Bypass',
                '-File', scriptPath,
                ...args
            ]

            
            const child = spawn(shell, fullArgs, {
                windowsHide: true,
                env: {
                    ...process.env,
                    LIB: '',
                    LIBPATH: '',
                    INCLUDE: ''
                }
             });

            let stdout = '', stderr = '';
            child.stdout.on('data', d => (stdout += d));
            child.stderr.on('data', d => (stderr += d));
            child.on('error', err => reject({stderr: stderr, err: err}) );
            child.on('close', code => resolve({ code, stdout: stdout.trim(), stderr: stderr.trim() }));
            
        });
    },

    runNode(script, args = []) {
        return new Promise((resolve, reject) => {
            setTimeout(()=>{
                const child = spawn(process.execPath, [path.resolve(script), ...args], {
                    stdio: 'inherit', // or ['ignore','pipe','pipe'] to capture
                });
                child.on('error', err => reject(err));
                child.on('close', code => resolve(code));
            }, 1000)
        });
    },

    /*async unzipTo(zipFile, destDir) {
        const absZip = path.resolve(zipFile);
        const absDest = path.resolve(destDir);
        await fs.promises.mkdir(absDest, { recursive: true });

        // extract-zip overwrites existing files by default
        await extract(absZip, { dir: absDest });
    },*/

    async listFilesRecursive(rootDir, options = {}){
        const followSymlinks = !!options.followSymlinks;
        const maxDepth = Number.isFinite(options.maxDepth) ? options.maxDepth : Infinity;
        const filter = typeof options.filter === 'function' ? options.filter : () => true;

        const root = path.resolve(rootDir);
        const out = [];
        const stack = [{ dir: root, depth: 0 }];

        while (stack.length) {
            const { dir, depth } = stack.pop();
            let dh;
            try {
                dh = await fs.promises.opendir(dir);
            } catch {
                // Not a directory (or inaccessible) — if it’s a file, push it
                try {
                    const st = await fs.promises.stat(dir);
                    if (st.isFile()) out.push(path.resolve(dir));
                } catch {}
                continue;
            }

            for await (const dirent of dh) {
                const full = path.join(dir, dirent.name);
                if (!filter(full, dirent)) continue;

                if (dirent.isFile()) {
                    out.push(full);
                } else if (dirent.isDirectory()) {
                    if (depth < maxDepth) stack.push({ dir: full, depth: depth + 1 });
                } else if (dirent.isSymbolicLink() && followSymlinks) {
                    try {
                        const real = await fs.promises.realpath(full);
                        const st = await fs.promises.stat(real);
                        if (st.isFile()) out.push(real);
                        else if (st.isDirectory() && depth < maxDepth) {
                            stack.push({ dir: real, depth: depth + 1 });
                        }
                    } catch { /* ignore broken symlinks */ }
                }
            }
        }

        // Normalize & sort for stable output
        return out.map(p => path.resolve(p)).sort((a, b) => a.localeCompare(b));
    },

    printRuntimeUser() {
        const info = [];
        try { info.push(`username: ${os.userInfo().username}`); } catch {}
        if (process.geteuid) info.push(`euid: ${process.geteuid()}`);
        if (process.getegid) info.push(`egid: ${process.getegid()}`);
        if (process.getuid)  info.push(`uid: ${process.getuid()}`);
        if (process.getgid)  info.push(`gid: ${process.getgid()}`);
        info.push(`groups: ${process.getgroups ? process.getgroups().join(',') : 'n/a'}`);
        info.push(`home: ${os.homedir()}`);
        info.push(`cwd: ${process.cwd()}`);
        info.push(`umask: ${process.umask().toString(8)}`);
        info.push(`SUDO_USER: ${process.env.SUDO_USER || ''}`);
        info.push(`USER: ${process.env.USER || process.env.LOGNAME || ''}`);
        console.error(`[whoami] ${info.join(' | ')}`);
    }
}