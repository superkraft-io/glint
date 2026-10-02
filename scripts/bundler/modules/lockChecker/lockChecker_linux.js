const LockChecker_Root = require('./lockChecker_root.js');

// Linux (like other POSIX systems) lets a file be deleted or replaced while
// other processes have it open, so nothing can block cleaning the output.
module.exports = class LockChecker_Linux extends LockChecker_Root {
    async init() {
        this.filesToCheck = [];
    }

    async checkFileLocked(filePath) {
        return { path: filePath, status: 'free' };
    }
};
