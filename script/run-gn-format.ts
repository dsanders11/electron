import * as childProcess from 'child_process';
import * as os from 'os';
import * as path from 'path';

import { SRC_DIR } from './lib/utils';

// Helper to run gn format on multiple files
// (gn only formats a single file at a time)
async function main (): Promise<void> {
  const env: NodeJS.ProcessEnv = {
    ...process.env,
    DEPOT_TOOLS_WIN_TOOLCHAIN: '0',
    CHROMIUM_BUILDTOOLS_PATH: path.resolve(SRC_DIR, 'buildtools')
  };

  console.log(process.argv.slice(2));

  const gnCommand = os.platform() === 'win32' ? 'gn.exe' : 'gn';

  for (const filename of process.argv.slice(2)) {
    childProcess.execFileSync(gnCommand, ['format', filename], { env });
  }
}

if (require.main === module) {
  main()
    .catch((err: Error) => {
      console.error(`ERROR: ${err.message}`);
      process.exit(1);
    });
}
