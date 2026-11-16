import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { DemoGalleryApi } from './demo';
import type { FolderState } from './folder';
import { GalleryApp, type FolderOps } from './GalleryApp';

function fakeFolder(state: FolderState): FolderOps & { saved: string[] } {
  const saved: string[] = [];
  return {
    saved,
    folderState: async () => state,
    chooseFolder: async () => ({ kind: 'ready', name: 'Wildkamera' }),
    allowFolder: async () => ({ kind: 'ready', name: 'Wildkamera' }),
    saveToFolder: async (sub, name) => {
      saved.push(`${sub}/${name}`);
    },
    downloadFile: vi.fn(),
  };
}

describe('GalleryApp', () => {
  it('requests a video, saves it into the folder after the check-in', async () => {
    let now = 1_000_000;
    const api = new DemoGalleryApi(() => now, 20);
    const folder = fakeFolder({ kind: 'ready', name: 'Wildkamera' });
    render(<GalleryApp api={api} folder={folder} now={() => now} pollMs={0} />);

    const cards = await screen.findAllByRole('listitem');
    expect(cards).toHaveLength(16);
    expect(screen.getByRole('heading', { name: 'Kamera „Garten“' })).toBeInTheDocument();
    expect(within(cards[0]!).getByText('DSCF0120.JPG')).toBeInTheDocument();
    expect(within(cards[0]!).getByText('DSCF0119.AVI')).toBeInTheDocument();
    fireEvent.click(within(cards[0]!).getByRole('button', { name: 'Video holen' }));
    expect(await within(cards[0]!).findByText(/Video kommt/)).toBeInTheDocument();

    now += 20;
    fireEvent.click(within(cards[0]!).getByRole('button', { name: 'Zurücknehmen' }));
    // Too late to withdraw: the hub already delivered it, and it gets saved.
    await waitFor(() => expect(folder.saved).toHaveLength(1));
    expect(folder.saved[0]).toMatch(/^Garten\/Garten_.*_DSCF0119\.mp4$/);
    expect(await screen.findByText(/1 Video gespeichert/)).toBeInTheDocument();
  });

  it('deletes all new recordings only after confirming', async () => {
    let now = 1_000_000;
    const api = new DemoGalleryApi(() => now, 20);
    render(
      <GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} now={() => now} pollMs={0} />,
    );

    fireEvent.click(await screen.findByRole('button', { name: 'Alle neuen löschen (16)' }));
    const dialog = screen.getByRole('dialog');
    expect(dialog).toHaveTextContent('Alle 16 neuen Aufnahmen löschen?');
    fireEvent.click(within(dialog).getByRole('button', { name: 'Ja, löschen' }));
    expect(await screen.findAllByText('Wird gelöscht')).toHaveLength(16);

    now += 20;
    fireEvent.click(screen.getByRole('button', { name: 'Wiese' }));
    fireEvent.click(await screen.findByRole('button', { name: 'Garten' }));
    expect(await screen.findByText(/Keine Aufnahmen/)).toBeInTheDocument();
  });

  it('asks for a folder when none is chosen', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={0} />);
    fireEvent.click(await screen.findByRole('button', { name: 'Ordner wählen' }));
    expect(await screen.findByText(/im Ordner „Wildkamera“ gespeichert/)).toBeInTheDocument();
  });
});
