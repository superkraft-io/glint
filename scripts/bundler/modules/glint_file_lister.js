const fs = require('fs');
const path = require('path');

const fileGrouper = require('./glint_files_grouper.js');

var ignoreExtensions = [
    '.DS_Store',
    '.git',
    '.gitignore',
    '.md',
    '.retain'
]

global.shouldIgnoreFile = file => {
    for (var i = 0; i < ignoreExtensions.length; i++) {
        if (file.indexOf(ignoreExtensions[i]) > -1) return true
    }
    return false
}

module.exports = {
    listFiles: function(dirPath, replaceRootWith, altPathRoot) { // Recursively list files and directories
        dirPath = dirPath.split('\\').join('/');
        let entries = [];

        var totalSize = 0
        var compressedSize = 0
        
        function readDirRecursive(currentPath) {
            const files = fs.readdirSync(currentPath);

            for (let i = 0; i < files.length; i++) {
                const file = files[i];
                
                if (shouldIgnoreFile(file)) continue // Skip ignored files

                const fullPath = path.join(currentPath, file).split('\\').join('/');

                const stat = fs.statSync(fullPath);

                const info = {
                    originalPath: fullPath,

                    isFolder: stat.isDirectory(),
                    path: fullPath.replace(dirPath, ''),
                    filename: file,
                    size: stat.size,
                    folders: [],
                    files: []
                }

                if (replaceRootWith) {
                    info.altPath = dirPath.replace(dirPath, replaceRootWith)
                    info.altPathRoot = dirPath
                }

                totalSize += info.size

                if (stat.isDirectory()) {
                    let folderEntries = fs.readdirSync(fullPath);
                    for (let u in folderEntries) {
                        var entryName = folderEntries[u];
                        if (shouldIgnoreFile(entryName)) continue
                        var entryStats = fs.statSync(path.join(fullPath, entryName));
                        if (entryStats.isDirectory()) info.folders.push(entryName);
                        else info.files.push(entryName);
                    }

                    entries.push(info); // Add directory to the list
                    readDirRecursive(fullPath); // Recurse into subdirectory
                } else {
                    //Remove all leading and trailing whitespace
                    //Note: This compresses the data a bit, but not much at all. I've gotten something like 1.13x compression, which is 13% less data.
                    //For a 100MB project, this would be 13MB saved. It's something, but it's insignificant.
                    
                    var fileSize = FileGroup.loadFileData({path: fullPath}).length

                    compressedSize += fileSize
                    info.size = fileSize;
                    
                    entries.push(info); // Add file to the list
                }
            }
        }


        //if (!ignoreRootDir){
            const stat = fs.statSync(dirPath);

            const info = {
                originalPath: dirPath,

                isFolder: stat.isDirectory(),
                path: dirPath.replace(dirPath, '/'),
                filename: '',
                size: stat.size,
                folders: [],
                files: []
            }

            if (replaceRootWith) {
                info.altPath = dirPath.replace(dirPath, replaceRootWith)
                info.altPathRoot = dirPath
            }

            entries.push(info);
        //}

        readDirRecursive(dirPath);
        //console.log(`${totalSize} vs ${compressedSize}    ${totalSize/compressedSize}x smaller`)
        return entries;
    }
};