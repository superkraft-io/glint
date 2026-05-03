const fs = require('fs');
const path = require('path');


class Folder {
    constructor() {
        this.entries = []
    }


    addEntry(opt) {
        this.entries.push({
            idx: this.files.length,
            path: opt.path,
            altPath: opt.altPath,
            altPath: opt.altPathRoot,
            size: opt.size,
            filename: opt.filename,
            offset: this.currentOffset
        })
    }
}

global.Folder = Folder

module.exports = {
    forFolders: function(opt) {
        var folders = opt.folders

        var entries = []
        
        for (var i = 0; i < folders.length; i++) {
            var entry = folders[i]
            
            if (!entry.isFolder) continue

          
            var res = fs.readdirSync((entry.altPath ? entry.altPathRoot : soft_backend_root) + entry.path)
            
            var folderEntries = {
                folders: [],
                files: []
            }

            for (var u in res) {
                if (shouldIgnoreFile(res[u])) continue // Skip ignored files

                if (fs.statSync(path.join((entry.altPath ? entry.altPathRoot : soft_backend_root), entry.path, res[u])).isDirectory()){
                    folderEntries.folders.push(res[u])
                } else {
                    folderEntries.files.push(res[u])
                }
            }

            if (entry.altPath){
                var x = 0
            }
            
            entries.push({
                path: entry.path,
                altPath: entry.altPath,
                altPathRoot: entry.altPathRoot,
                entries: folderEntries
            })
        }

        var cppEntries = []

        for (var i in entries) {
            var folder = entries[i]
            var refPath = (folder.altPath ? folder.altPath.substr(0, folder.altPath.length - 1) + folder.path: folder.path)
            if (folder.altPath && folder.path === '/') refPath = folder.altPath.substr(0, folder.altPath.length - 1)
            var cppEntry = `       {"${refPath}", new glint_bundle_entry_info(0, 0, 0, 0, 0, true, "", "${folder.entries.folders.join(',')}", "${folder.entries.files.join(',')}")}`
            cppEntries.push(cppEntry)
        }
        
        return cppEntries
    }
};