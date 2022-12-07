import * as path from 'path';

import { createLanguageService, DiagnosticLevel, DiagnosticOptions, ILogger } from '@dsanders11/vscode-markdown-languageservice';
import { CancellationTokenSource } from 'vscode-languageserver';
import { URI } from 'vscode-uri';

import { DocsWorkspace, MarkdownLinkComputer, MarkdownParser } from './lib/markdown';

class NoOpLogger implements ILogger {
  log (): void {}
}

const diagnosticOptions: DiagnosticOptions = {
  ignoreLinks: [],
  validateDuplicateLinkDefinitions: DiagnosticLevel.ignore,
  validateFileLinks: DiagnosticLevel.error,
  validateFragmentLinks: DiagnosticLevel.error,
  validateMarkdownFileLinkFragments: DiagnosticLevel.error,
  validateReferences: DiagnosticLevel.error,
  validateUnusedLinkDefinitions: DiagnosticLevel.ignore
};

async function main () {
  const workspace = new DocsWorkspace(path.resolve(__dirname, '..', 'docs'));
  const parser = new MarkdownParser();
  const linkComputer = new MarkdownLinkComputer(workspace);
  const languageService = createLanguageService({
    workspace, parser, logger: new NoOpLogger(), linkComputer
  });

  const cts = new CancellationTokenSource();
  let errors = false;

  try {
    // Collect diagnostics for all documents in the workspace
    for (const document of await workspace.getAllMarkdownDocuments()) {
      const diagnostics = await languageService.computeDiagnostics(document, diagnosticOptions, cts.token);

      if (diagnostics.length) {
        console.log('File Location:', path.relative(workspace.root, URI.parse(document.uri).path));
      }

      for (const diagnostic of diagnostics) {
        console.log(`\tBroken link on line ${diagnostic.range.start.line + 1}:`, diagnostic.message);
        errors = true;
      }
    }
  } finally {
    cts.dispose();
  }

  if (errors) {
    process.exit(1);
  }
}

if (process.mainModule === module) {
  main().catch((error) => {
    console.error(error);
    process.exit(1);
  });
}
