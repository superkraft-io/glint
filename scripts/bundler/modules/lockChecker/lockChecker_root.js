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

    async checkFiles(){
        var promises = []
        for (let i=0; i<this.filesToCheck.length; i++){
            var p = this.filesToCheck[i]
            promises.push(this.checkFileLocked(p))
        }

        var res = await Promise.allSettled(promises)

        var rejected = []

        for (var file of res){
            if (file.status === 'rejected') rejected.push(file.reason)
        }

        return rejected
    }
}