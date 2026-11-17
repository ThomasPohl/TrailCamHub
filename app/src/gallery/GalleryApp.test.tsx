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

  it('fetches and deletes the chosen recordings together', async () => {
    const now = 1_000_000;
    const api = new DemoGalleryApi(() => now, 20);
    render(
      <GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} now={() => now} pollMs={0} />,
    );
    const cards = await screen.findAllByRole('listitem');
    expect(screen.queryByRole('region', { name: 'Auswahl' })).not.toBeInTheDocument();

    for (const card of cards.slice(0, 3)) {
      fireEvent.click(within(card).getByRole('checkbox', { name: 'Auswählen' }));
    }
    const bar = screen.getByRole('region', { name: 'Auswahl' });
    expect(bar).toHaveTextContent('3 Aufnahmen ausgewählt');
    fireEvent.click(within(bar).getByRole('button', { name: 'Videos holen (3)' }));
    expect(await screen.findAllByText(/Video kommt/)).toHaveLength(3);
    // Asked-for recordings cannot be chosen; the selection is gone.
    expect(screen.queryByRole('region', { name: 'Auswahl' })).not.toBeInTheDocument();
    expect(within(cards[0]!).queryByRole('checkbox')).not.toBeInTheDocument();

    const rest = screen.getAllByRole('listitem').slice(3, 5);
    for (const card of rest) {
      fireEvent.click(within(card).getByRole('checkbox', { name: 'Auswählen' }));
    }
    fireEvent.click(screen.getByRole('button', { name: 'Löschen (2)' }));
    const dialog = screen.getByRole('dialog');
    expect(dialog).toHaveTextContent('2 ausgewählte Aufnahmen löschen?');
    fireEvent.click(within(dialog).getByRole('button', { name: 'Ja, löschen' }));
    expect(await screen.findAllByText('Wird gelöscht')).toHaveLength(2);
    expect(screen.getAllByText(/Video kommt/)).toHaveLength(3);
  });

  it('forgets the selection when it is cancelled', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={0} />);
    const [card] = await screen.findAllByRole('listitem');
    const box = within(card!).getByRole('checkbox', { name: 'Auswählen' });
    fireEvent.click(box);
    expect(box).toBeChecked();
    fireEvent.click(screen.getByRole('button', { name: 'Auswahl aufheben' }));
    expect(box).not.toBeChecked();
    expect(screen.queryByRole('region', { name: 'Auswahl' })).not.toBeInTheDocument();
  });

  it('renames the camera', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={0} />);
    await screen.findByRole('heading', { name: 'Kamera „Garten“' });
    fireEvent.click(screen.getByRole('button', { name: 'Umbenennen' }));

    const input = screen.getByLabelText('Neuer Name der Kamera');
    const save = screen.getByRole('button', { name: 'Speichern' });
    expect(save).toBeDisabled(); // unchanged name
    fireEvent.change(input, { target: { value: '   ' } });
    expect(save).toBeDisabled();
    fireEvent.change(input, { target: { value: ' Futterplatz ' } });
    fireEvent.click(save);

    expect(
      await screen.findByRole('heading', { name: 'Kamera „Futterplatz“' }),
    ).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Futterplatz' })).toBeInTheDocument(); // tab
    expect(screen.getByText(/Die Kamera heißt jetzt „Futterplatz“/)).toBeInTheDocument();
    expect(screen.queryByLabelText('Neuer Name der Kamera')).not.toBeInTheDocument();
  });

  it('offers the admin view only to the admin', async () => {
    const user = new DemoGalleryApi(() => 1_000_000, 20);
    const { unmount } = render(
      <GalleryApp api={user} folder={fakeFolder({ kind: 'none' })} pollMs={0} />,
    );
    await screen.findAllByRole('listitem');
    expect(screen.queryByRole('button', { name: 'Verwaltung' })).not.toBeInTheDocument();
    unmount();

    const admin = new DemoGalleryApi(() => 1_000_000, 20, true);
    render(
      <GalleryApp
        api={admin}
        folder={fakeFolder({ kind: 'none' })}
        now={() => 1_000_000}
        pollMs={0}
      />,
    );
    // Two hints in the demo: the button counts them (#265).
    fireEvent.click(await screen.findByRole('button', { name: 'Verwaltung (2)' }));
    expect(await screen.findByText(/2 Hinweise/)).toBeInTheDocument();
    const hub = await screen.findByRole('article', { name: 'Hub Garten' });
    const faults = within(hub).getByRole('list', { name: 'Störungen Garten' });
    expect(faults).toHaveTextContent('Kamera „Wiese“: Akku schwach (18 %).');
    expect(within(hub).getByText('84 %')).toBeInTheDocument();
    expect(within(hub).getByText(/Anwender, Admin \(Admin\)/)).toBeInTheDocument();

    const row = within(hub).getByRole('row', { name: /Wiese/ });
    fireEvent.click(within(row).getByRole('button', { name: 'Jetzt lesen' }));
    expect(await within(row).findByText('Lesen angefordert')).toBeInTheDocument();

    const interval = within(hub).getByLabelText(/Kontakt alle/);
    fireEvent.change(interval, { target: { value: '3' } });
    const apply = within(hub).getByRole('button', { name: 'Übernehmen' });
    expect(apply).toBeDisabled();
    fireEvent.change(interval, { target: { value: '60' } });
    fireEvent.click(apply);
    expect(await screen.findByText(/übernimmt den Plan/)).toBeInTheDocument();
    expect(await within(hub).findByText(/alle 1 h \(aus der App\)/)).toBeInTheDocument();

    // Firmware changes (#149).
    expect(within(hub).getByRole('list', { name: 'Firmware-Wechsel Garten' })).toHaveTextContent(
      'v0.1.12 → v0.1.13',
    );

    // Battery history (#76), loaded when asked for.
    fireEvent.click(within(hub).getByRole('button', { name: 'Akku-Verlauf (30 Tage)' }));
    expect(await within(hub).findByRole('img', { name: /^Hub: Akku/ })).toBeInTheDocument();
    expect(
      within(hub).getByRole('img', { name: 'Kamera „Garten“: Akku 95 % → 66 %' }),
    ).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'Zur Galerie' }));
    expect(await screen.findAllByRole('listitem')).toHaveLength(16);
  });

  it('hides a camera and shows it again (#273)', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={0} />);
    await screen.findByRole('heading', { name: 'Kamera „Garten“' });
    fireEvent.click(screen.getByRole('button', { name: 'Ausblenden' }));
    const dialog = screen.getByRole('dialog');
    expect(dialog).toHaveTextContent('Kamera „Garten“ ausblenden?');
    fireEvent.click(within(dialog).getByRole('button', { name: 'Ja, ausblenden' }));

    expect(await screen.findByRole('heading', { name: 'Kamera „Wiese“' })).toBeInTheDocument();
    expect(screen.queryByRole('navigation', { name: 'Kameras' })).not.toBeInTheDocument();
    const hidden = screen.getByRole('region', { name: 'Ausgeblendete Kameras' });
    expect(hidden).toHaveTextContent('Garten');

    fireEvent.click(within(hidden).getByRole('button', { name: 'Wieder anzeigen' }));
    expect(await screen.findByRole('heading', { name: 'Kamera „Garten“' })).toBeInTheDocument();
    expect(screen.queryByRole('region', { name: 'Ausgeblendete Kameras' })).not.toBeInTheDocument();
  });

  it('takes a found camera in and stops reading one (#298)', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={0} />);
    const fresh = await screen.findByRole('group', { name: 'Neue Kamera TC100_77A1' });
    expect(fresh).toHaveTextContent('mittleres Signal');
    fireEvent.change(within(fresh).getByLabelText(/Name der Kamera/), {
      target: { value: ' Futterplatz ' },
    });
    fireEvent.click(within(fresh).getByRole('button', { name: 'Aufnehmen' }));
    expect(
      await screen.findByText(/„Futterplatz“ wird ab dem nächsten Kontakt mit dem Hub gelesen/),
    ).toBeInTheDocument();
    expect(screen.queryByRole('group', { name: /Neue Kamera/ })).not.toBeInTheDocument();

    await screen.findByRole('heading', { name: 'Kamera „Garten“' });
    fireEvent.click(await screen.findByRole('button', { name: 'Nicht mehr abfragen' }));
    const dialog = screen.getByRole('dialog');
    expect(dialog).toHaveTextContent('Kamera „Garten“ nicht mehr abfragen?');
    fireEvent.click(within(dialog).getByRole('button', { name: 'Ja, nicht mehr abfragen' }));
    const section = await screen.findByRole('region', { name: 'Gefundene Kameras' });
    expect(await within(section).findByText('Nicht abgefragte Kameras')).toBeInTheDocument();
    expect(section).toHaveTextContent('Garten');
    expect(screen.queryByRole('button', { name: 'Nicht mehr abfragen' })).not.toBeInTheDocument();

    fireEvent.click(within(section).getByRole('button', { name: 'Doch aufnehmen' }));
    expect(await screen.findByRole('button', { name: 'Nicht mehr abfragen' })).toBeInTheDocument();
    expect(screen.queryByRole('region', { name: 'Gefundene Kameras' })).not.toBeInTheDocument();
  });

  it("shows the camera's battery and what needs a look (#304)", async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={0} />);
    await screen.findByRole('heading', { name: 'Kamera „Garten“' });
    expect(screen.getByText(/^Akku 76 % · zuletzt gelesen/)).toBeInTheDocument();
    expect(screen.queryByRole('status', { name: 'Hinweise zur Kamera' })).not.toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'Wiese' }));
    const hints = await screen.findByRole('status', { name: 'Hinweise zur Kamera' });
    expect(hints).toHaveTextContent('Der Akku der Kamera wird schwach (18 %).');
    expect(hints).toHaveTextContent('Die SD-Karte der Kamera ist fast voll.');
  });

  it('asks for a folder when none is chosen', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={0} />);
    fireEvent.click(await screen.findByRole('button', { name: 'Ordner wählen' }));
    expect(await screen.findByText(/im Ordner „Wildkamera“ gespeichert/)).toBeInTheDocument();
  });
});
