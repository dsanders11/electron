import * as fs from 'fs';
import * as path from 'path';

import * as MarkdownIt from 'markdown-it';
import { githubSlugifier, FileStat, IMdParser, ITextDocument, IWorkspace } from 'vscode-markdown-languageservice';
import { Emitter } from 'vscode-languageserver';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { URI } from 'vscode-uri';

import { findMatchingFiles } from './utils';

const mdIt = MarkdownIt({ html: true });

export class MarkdownParser implements IMdParser {
  slugifier = githubSlugifier;

  async tokenize (document: TextDocument) {
    return mdIt.parse(document.getText(), {});
  }
}

// TODO - Implement
export class DocsWorkspace implements IWorkspace {
  private readonly documentCache: Map<string, TextDocument>;
  readonly root: string;

  constructor (root: string) {
    this.documentCache = new Map();
    this.root = root;
  }

  get workspaceFolders () {
    return [URI.file(this.root)];
  }

  async getAllMarkdownDocuments (): Promise<Iterable<ITextDocument>> {
    const files = await findMatchingFiles(this.root, file => file.endsWith('.md'));

    for (const file of files) {
      const document = TextDocument.create(
        URI.file(file).toString(),
        'markdown',
        1,
        fs.readFileSync(file, 'utf8')
      );

      this.documentCache.set(file, document);
    }

    return this.documentCache.values();
  }

  hasMarkdownDocument (resource: URI) {
    const relativePath = path.relative(this.root, resource.path);
    return !relativePath.startsWith('..') && !path.isAbsolute(relativePath) && fs.existsSync(resource.path);
  }

  async openMarkdownDocument (resource: URI) {
    if (!this.documentCache.has(resource.path)) {
      const document = TextDocument.create(
        resource.toString(),
        'markdown',
        1,
        fs.readFileSync(resource.path, 'utf8')
      );

      this.documentCache.set(resource.path, document);
    }

    return this.documentCache.get(resource.path);
  }

  async stat (resource: URI): Promise<FileStat | undefined> {
    if (this.hasMarkdownDocument(resource)) {
      const stats = fs.statSync(resource.path);
      return { isDirectory: stats.isDirectory() };
    }

    return undefined;
  }

  async readDirectory () {
    // TODO - Implement?
    return [];
  }

  //
  // These events are defined to fulfill the interface, but are never emitted
  // by this implementation since it's not meant for watching a workspace
  //

  #onDidChangeMarkdownDocument = new Emitter<ITextDocument>();
  onDidChangeMarkdownDocument = this.#onDidChangeMarkdownDocument.event;

  #onDidCreateMarkdownDocument = new Emitter<ITextDocument>();
  onDidCreateMarkdownDocument = this.#onDidCreateMarkdownDocument.event;

  #onDidDeleteMarkdownDocument = new Emitter<URI>();
  onDidDeleteMarkdownDocument = this.#onDidDeleteMarkdownDocument.event;
}
