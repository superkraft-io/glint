const path = require('path');
var utils = require('../glint_utils.js')

global.LockChecker_Root = require('./lockChecker_root.js')

const checkScript = path.resolve(__dirname, 'scripts/checklock.ps1').split('\\').join('/');

module.exports = class LockChecker_Win extends LockChecker_Root {
    // Resolves for a free file and rejects with { path, status, ... } for
    // anything else.  Every outcome settles: a check that never did left
    // checkFiles() pending, and node then exited 0 without bundling.
    async checkFileLocked(filePath){
        console.log(`  - Checking ${filePath}...`)

        let result
        try {
            result = await utils.runPs1(checkScript, [filePath.split('\\').join('/')]);
        } catch(err) {
            throw {path: filePath, status: 'error', error: String((err && (err.err || err.stderr)) || err)}
        }

        const stdout = result.stdout
        if (stdout === 'free') {
            return {path: filePath, status: 'free'}
        }

        if (stdout === 'not_found') {
            throw {path: filePath, status: 'not_found'}
        }

        if (stdout.startsWith('locked,')) {
            // "locked,<pid>:<name>,<pid>:<name>..."
            const procList = stdout.slice('locked,'.length).split(',').filter(Boolean).map(entry => {
                const sep = entry.indexOf(':')
                return sep < 0 ? {pid: entry, name: 'unknown'}
                               : {pid: entry.slice(0, sep), name: entry.slice(sep + 1)}
            })
            throw {path: filePath, status: 'locked', filename: path.basename(filePath), procList: procList}
        }

        // PowerShell failed, or printed something this does not understand.
        throw {path: filePath, status: 'error', error: result.stderr || `unexpected output "${stdout}" (exit code ${result.code})`}
    }
}
