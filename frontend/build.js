// Build all 3 entry points as self-contained HTML files
import { execSync } from 'child_process';
import { rmSync } from 'fs';

// Clean dist/ once at the start
try { rmSync('dist', { recursive: true }); } catch { /* ignore */ }

const entries = ['index', 'broadcast', 'result'];

for (const entry of entries) {
  console.log(`\nBuilding ${entry}.html...`);
  execSync(`npx vite build --emptyOutDir false`, {
    stdio: 'inherit',
    env: { ...process.env, ENTRY: entry },
  });
}

console.log('\nAll builds complete!');
