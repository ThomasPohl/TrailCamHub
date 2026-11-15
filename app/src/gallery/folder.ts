/**
 * Saves videos into a folder on the laptop chosen once (File System Access
 * API, Chrome/Edge). The folder handle is kept in IndexedDB; an installed app
 * keeps the permission across visits. Without the API, or when the folder is
 * gone, videos are downloaded the normal way.
 */

type Permission = 'granted' | 'denied' | 'prompt';

interface PermissionHandle extends FileSystemDirectoryHandle {
  queryPermission?(options: { mode: 'readwrite' }): Promise<Permission>;
  requestPermission?(options: { mode: 'readwrite' }): Promise<Permission>;
}

interface PickerWindow {
  showDirectoryPicker?(options?: {
    id?: string;
    mode?: 'readwrite';
    startIn?: 'videos' | 'downloads' | 'documents';
  }): Promise<FileSystemDirectoryHandle>;
}

export type FolderState =
  /** The browser cannot write into a folder: plain downloads only. */
  | { kind: 'unsupported' }
  /** No folder chosen yet. */
  | { kind: 'none' }
  /** A folder was chosen, but the browser wants a click to allow it again. */
  | { kind: 'needsPermission'; name: string }
  | { kind: 'ready'; name: string };

const DB = 'trailcamhub-gallery';
const STORE = 'folder';
const KEY = 'videos';

export function folderSupported(win: Window = window): boolean {
  return typeof (win as unknown as PickerWindow).showDirectoryPicker === 'function';
}

function openDb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(DB, 1);
    request.onupgradeneeded = () => request.result.createObjectStore(STORE);
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}

async function withStore<T>(
  mode: IDBTransactionMode,
  run: (store: IDBObjectStore) => IDBRequest,
): Promise<T> {
  const db = await openDb();
  try {
    return await new Promise<T>((resolve, reject) => {
      const request = run(db.transaction(STORE, mode).objectStore(STORE));
      request.onsuccess = () => resolve(request.result as T);
      request.onerror = () => reject(request.error);
    });
  } finally {
    db.close();
  }
}

async function storedHandle(): Promise<PermissionHandle | undefined> {
  try {
    return await withStore<PermissionHandle | undefined>('readonly', (s) => s.get(KEY));
  } catch {
    return undefined;
  }
}

export async function folderState(): Promise<FolderState> {
  if (!folderSupported()) return { kind: 'unsupported' };
  const handle = await storedHandle();
  if (!handle) return { kind: 'none' };
  const permission = (await handle.queryPermission?.({ mode: 'readwrite' })) ?? 'granted';
  return permission === 'granted'
    ? { kind: 'ready', name: handle.name }
    : { kind: 'needsPermission', name: handle.name };
}

/** Lets the user choose the folder; must run from a click. */
export async function chooseFolder(): Promise<FolderState> {
  const picker = (window as unknown as PickerWindow).showDirectoryPicker;
  if (!picker) return { kind: 'unsupported' };
  const handle = await picker.call(window, {
    id: 'trailcamhub-videos',
    mode: 'readwrite',
    startIn: 'videos',
  });
  await withStore('readwrite', (s) => s.put(handle, KEY));
  return { kind: 'ready', name: handle.name };
}

/** Asks again for a folder chosen earlier; must run from a click. */
export async function allowFolder(): Promise<FolderState> {
  const handle = await storedHandle();
  if (!handle) return { kind: 'none' };
  const permission = (await handle.requestPermission?.({ mode: 'readwrite' })) ?? 'granted';
  return permission === 'granted'
    ? { kind: 'ready', name: handle.name }
    : { kind: 'needsPermission', name: handle.name };
}

/** Writes into the chosen folder, one sub-folder per camera. */
export async function saveToFolder(subFolder: string, fileName: string, data: Blob): Promise<void> {
  const root = await storedHandle();
  if (!root) throw new Error('Kein Ordner gewählt');
  const dir = await root.getDirectoryHandle(subFolder, { create: true });
  const file = await dir.getFileHandle(fileName, { create: true });
  const writable = await file.createWritable();
  try {
    await writable.write(data);
  } finally {
    await writable.close();
  }
}

/** The browser's normal download, as a fallback. */
export function downloadFile(fileName: string, data: Blob): void {
  const url = URL.createObjectURL(data);
  const link = document.createElement('a');
  link.href = url;
  link.download = fileName;
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 60_000);
}
