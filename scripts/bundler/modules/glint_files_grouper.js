const fs = require('fs');
const path = require('path');

function formatBufferForCpp(buffer, valuesPerLine = 32) {
    if (!buffer || buffer.length === 0)
        return '';

    const lines = [];

    for (let index = 0; index < buffer.length; index += valuesPerLine) {
        const line = [];
        const end = Math.min(index + valuesPerLine, buffer.length);

        for (let inner = index; inner < end; inner += 1)
            line.push(buffer[inner]);

        lines.push(line.join(','));
    }

    return '\n        ' + lines.join(',\n        ') + '\n    ';
}


class FileGroup {
    constructor() {
        this.buffer = Buffer.alloc(0)
        this.files = []
        this.currentOffset = 0
    }

    static loadFileDataRaw(opt) {
        var removeWhitespace = opt.removeWhitespace || true

        var fileData = fs.readFileSync(opt.path);
        
        /*if (removeWhitespace){
            var lines = fileData.toString().split('\n');
            for (var u in lines) lines[u] = lines[u].trim();
            fileData = lines.join('\n');
        }*/

        return Buffer.from(fileData, 'utf8');
    }

    static loadFileData(opt){
        var fileTypesToMinify = ['.js', '.css', '.html', '.json', '.xml']
        var fileExt = path.extname(opt.path)
        var removeWhitespace = fileTypesToMinify.includes(fileExt.toLowerCase())
        
        return this.loadFileDataRaw({
            removeWhitespace: removeWhitespace,
            path: opt.path
        })
    }

    increaseSize(addedSize) {
        var newSize = this.buffer.length + addedSize
        if (newSize > this.buffer.length) {
            try {
                this.buffer = Buffer.concat([this.buffer, Buffer.allocUnsafe(addedSize)])
            } catch (e) {
                console.error('Error increasing buffer size:', e);
            }
        }
    }

    get size() {
        return this.buffer.length
    }

    addFile(opt) {
        this.files.push({
            idx: this.files.length,
            path: opt.path,
            altPath: opt.altPath,
            altPathRoot: opt.altPathRoot,
            size: opt.size,
            filename: opt.filename,
            offset: this.currentOffset
        })

        this.increaseSize(opt.size)

        var fileBuffer = FileGroup.loadFileData({path: opt.originalPath})
        fileBuffer.copy(this.buffer, this.currentOffset)
        this.currentOffset += opt.size
    }

    saveHeader() {
        var headerTemplatePath = __dirname + '/../templates/glint_bundle_group_template.h'
        var cppTemplatePath = __dirname + '/../templates/glint_bundle_group_template.cpp'
        var dataEntryTemplate_header = fs.readFileSync(headerTemplatePath).toString()
        var dataEntryTemplate_deep_cpp = fs.readFileSync(cppTemplatePath).toString()

        dataEntryTemplate_header = dataEntryTemplate_header
            .split('<!namespace!>').join(bundleNamespace)
            .split('<!id!>').join(this.id)
            .split('<!data_size!>').join(this.buffer.length)

        dataEntryTemplate_deep_cpp = dataEntryTemplate_deep_cpp
            .split('<!namespace!>').join(bundleNamespace)
            .split('<!id!>').join(this.id)
            .split('<!data_size!>').join(this.buffer.length)
            .replace('<!data!>', formatBufferForCpp(this.buffer))

        if (glint.bundle_mode === 'shallow') fs.writeFileSync(shallowGroupsDataRoot + '/' + this.id + '.bin', this.buffer)


        var offsets = []
        var sizes = []
        for (var i in this.files) {
            var file = this.files[i]
            
            offsets.push(file.offset)
            sizes.push(file.size)
        }
        
        dataEntryTemplate_header = dataEntryTemplate_header
            .replace('<!offsets_arr_size!>', this.files.length).replace('<!offsets!>', offsets.join(','))
            .replace('<!sizes_arr_size!>', this.files.length).replace('<!sizes!>', sizes.join(','))

        dataEntryTemplate_deep_cpp = dataEntryTemplate_deep_cpp
            .replace('<!offsets_arr_size!>', this.files.length).replace('<!offsets!>', offsets.join(','))
            .replace('<!sizes_arr_size!>', this.files.length).replace('<!sizes!>', sizes.join(','))

        if (glint.bundle_mode === 'deep') {
            fs.writeFileSync(deepGroupsRoot + '/glint_bundle_group_' + this.id + '.hpp', dataEntryTemplate_header)
            fs.writeFileSync(deepGroupsRoot + '/glint_bundle_group_' + this.id + '.cpp', dataEntryTemplate_deep_cpp)
        }
        if (glint.bundle_mode === 'shallow') fs.writeFileSync(shallowGroupsRoot + '/glint_bundle_group_' + this.id + '.hpp', dataEntryTemplate_header)

        this.headerDef = {
            paths: {
                deep: deepGroupsRoot + '/glint_bundle_group_' + this.id + '.hpp',
                shallow: shallowGroupsRoot + '/glint_bundle_group_' + this.id + '.hpp'
            },
            deep: `#include "./deep/groups/glint_bundle_group_${this.id}.hpp"`,
            shallow: `#include "./shallow/groups/glint_bundle_group_${this.id}.hpp"`
        }

        this.headerClass = `glint_bundle_group_${this.id}`
    }
}

global.FileGroup = FileGroup

module.exports = {
    forFiles: function(opt) {
        var totalSize = 0

        for (var i = 0; i < opt.files.length; i++) {
            var file = opt.files[i]
            if (file.isFolder) continue

            totalSize += file.size
        }


        var files = opt.files
        var groupSize = (opt.groupSize ? 1000 * 1000 * opt.groupSize : 1000 * 1000 * 10) // default 10MB

        var groups = []

        //console.warn('GROUPING FOR LARGE FILES IS DISABLED. TURN IT BACK ON WHEN BUNDLER IS CONFIRMED TO WORK PROPERLY!')
        
        for (var i = files.length - 1; i >= 0; i--) {
            var file = files[i]
            if (file.size > groupSize) {
                var group = new FileGroup()
                group.addFile(file)
                groups.push(group)
                files.splice(i, 1)
            }
        }

        var currentGroup = new FileGroup()
        for (var i = 0; i < files.length; i++) {
            var file = files[i]
            
            //if (i > 3) break //temporary limit for testing

            if (file.isFolder) continue

            var expectedGroupSize = currentGroup.size + file.size
            if (expectedGroupSize > groupSize) {
                groups.push(currentGroup)
                currentGroup = new FileGroup()
            }

            currentGroup.addFile(file)
        }

        if (currentGroup.files.length > 0) groups.push(currentGroup)




        for (var i = 0; i < groups.length; i++) {
            var group = groups[i]
            group.id = i
            group.saveHeader()
        }

       
        

        var includeDefs = {deep: [], shallow: []}
        var groupDefs = []
        var entriesDefs = []

        for (var groupIdx = 0; groupIdx < groups.length; groupIdx++) {
            var group = groups[groupIdx]

            includeDefs.deep.push(group.headerDef.deep)
            includeDefs.shallow.push(group.headerDef.shallow)

            groupDefs.push(`        new ${group.headerClass}()`)

            for (var u in group.files) {
                var file = group.files[u]
                var entryLine = `       {"${(file.altPath ? file.altPath.substr(0, file.altPath.length - 1) + file.path : file.path)}", new glint_bundle_entry_info(${file.offset}, ${file.size}, ${groupIdx}, ${file.idx}, groups[${groupIdx}], false, "${file.filename}", "", "")}`
                entriesDefs.push(entryLine)
            }
        }

        var returnInfo = {
            includesDef: {
                deep: includeDefs.deep.join('\n'),
                shallow: includeDefs.shallow.join('\n')
            },
            groupsDefs: groupDefs.join(',\n'),
            entriesDefs: entriesDefs.join(',\n')
        }
        
        return returnInfo
    }
};