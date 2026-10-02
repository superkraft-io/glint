const fs = require('fs');
const path = require('path');
var utils = require('../glint_utils.js')

module.exports = class LockChecker_Root {
    constructor(){

    }

    async init(){
        this.filesToCheck = await utils.listFilesRecursive(global.bundleRoot, {
            followSymlinks: false,
            maxDepth: Infinity,
            //filter: (p, d) => !p.endsWith('.tmp')
        })
    }

    // Returns the failed checks (locked, missing or unreadable files).  Only a
    // few run at once: each check on Windows starts a PowerShell process.
    async checkFiles(){
        var failed = []
        var next = 0

        const worker = async () => {
            while (next < this.filesToCheck.length) {
                const filePath = this.filesToCheck[next++]
                try {
                    await this.checkFileLocked(filePath)
                } catch (reason) {
                    failed.push(reason)
                }
            }
        }

        await Promise.all(Array.from({ length: Math.min(8, this.filesToCheck.length) }, worker))

        return failed.sort((a, b) => String(a && a.path).localeCompare(String(b && b.path)))
    }
}
