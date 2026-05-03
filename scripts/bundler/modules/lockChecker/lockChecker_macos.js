const fs = require('fs');
const path = require('path');
const { execFile } = require('child_process');

global.LockChecker_Root = require('./lockChecker_root.js');

module.exports = class LockChecker_MacOS extends LockChecker_Root {
  async checkFileLocked(filePath) {
    return new Promise((resolve, reject) => {
      console.log(`  - Checking ${filePath}...`);

      // 1) Existence check
      if (!fs.existsSync(filePath)) {
        return reject({ path: filePath, status: 'not_found' });
      }

      // 2) Ask lsof for processes holding the file
      // -Fpcfn → p:pid, c:command, f:fd, n:name
      const args = ['-Fpcfn', '--', filePath];

      execFile('lsof', args, { windowsHide: true }, (err, stdout, stderr) => {
        // lsof exits with code 1 when there are no matches (treat as free)
        if (err) {
          if (err.code === 1 && !stdout) {
            return resolve({ path: filePath, status: 'free' });
          }
          return reject({ path: filePath, status: 'error', error: err });
        }

        if (!stdout || stdout.trim() === '') {
          return resolve({ path: filePath, status: 'free' });
        }

        // 3) Parse lsof field format
        const lines = stdout.split('\n');
        const procs = {}; // pid -> name
        let currentPid = null;

        for (const line of lines) {
          if (!line) continue;
          const tag = line[0];
          const val = line.slice(1);

          if (tag === 'p') {
            currentPid = val;
          } else if (tag === 'c') {
            if (currentPid) procs[currentPid] = val;
          }
          // We don’t need f/n fields for the summary, but they’re available if you want per-FD details
        }

        const procList = Object.entries(procs).map(([pid, name]) => ({ pid, name }));

        if (procList.length === 0) {
          return resolve({ path: filePath, status: 'free' });
        }

        return reject({
          path: filePath,
          filename: path.basename(filePath),
          procList
        });
      });
    });
  }
};
