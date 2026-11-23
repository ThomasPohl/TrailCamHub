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

/** The default view shows every camera; most tests look at one (#431). */
async function openCamera(name: string) {
  fireEvent.click(await screen.findByRole('tab', { name }));
  expect(await screen.findByRole('heading', { name: `Kamera „${name}“` })).toBeInTheDocument();
}

describe('GalleryApp', () => {
  it('requests a video, saves it into the folder after the check-in', async () => {
    let now = 1_000_000;
    const api = new DemoGalleryApi(() => now, 20);
    const folder = fakeFolder({ kind: 'ready', name: 'Wildkamera' });
    render(<GalleryApp api={api} folder={folder} now={() => now} pollMs={0} />);

    await openCamera('Garten');
    const cards = await screen.findAllByRole('listitem');
    expect(cards).toHaveLength(16);
    // The camera's file names are not on the tile any more (#431).
    expect(within(cards[0]!).queryByText('DSCF0120.JPG')).not.toBeInTheDocument();
    fireEvent.click(within(cards[0]!).getByRole('button', { name: 'Video holen' }));
    expect(await within(cards[0]!).findByText(/^Kommt gegen/)).toBeInTheDocument();

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

    await openCamera('Garten');
    await screen.findAllByRole('listitem');
    // "Alle auswählen" of every day, then delete.
    for (const button of screen.getAllByRole('button', { name: /^Alle auswählen:/ })) {
      fireEvent.click(button);
    }
    fireEvent.click(screen.getByRole('button', { name: 'Löschen (16)' }));
    const dialog = screen.getByRole('dialog');
    expect(dialog).toHaveTextContent('16 ausgewählte Aufnahmen löschen?');
    fireEvent.click(within(dialog).getByRole('button', { name: 'Ja, löschen' }));
    expect(await screen.findAllByText('Wird gelöscht')).toHaveLength(16);

    now += 20;
    fireEvent.click(screen.getByRole('tab', { name: 'Wiese' }));
    fireEvent.click(await screen.findByRole('tab', { name: 'Garten' }));
    expect(await screen.findByText(/Keine Aufnahmen/)).toBeInTheDocument();
  });

  it('fetches and deletes the chosen recordings together', async () => {
    const now = 1_000_000;
    const api = new DemoGalleryApi(() => now, 20);
    render(
      <GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} now={() => now} pollMs={0} />,
    );
    await openCamera('Garten');
    const cards = await screen.findAllByRole('listitem');
    expect(screen.queryByRole('region', { name: 'Auswahl' })).not.toBeInTheDocument();

    for (const card of cards.slice(0, 3)) {
      fireEvent.click(within(card).getByRole('checkbox', { name: 'Auswählen' }));
    }
    const bar = screen.getByRole('region', { name: 'Auswahl' });
    expect(bar).toHaveTextContent('3 Aufnahmen ausgewählt');
    fireEvent.click(within(bar).getByRole('button', { name: 'Videos holen (3)' }));
    expect(await screen.findAllByText(/^Kommt gegen/)).toHaveLength(3);
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
    expect(screen.getAllByText(/^Kommt gegen/)).toHaveLength(3);
  });

  it('forgets the selection when it is cancelled', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={0} />);
    await openCamera('Garten');
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
    await openCamera('Garten');
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
    expect(screen.getByRole('tab', { name: 'Futterplatz' })).toBeInTheDocument();
    expect(screen.getByText(/Die Kamera heißt jetzt „Futterplatz“/)).toBeInTheDocument();
    expect(screen.queryByLabelText('Neuer Name der Kamera')).not.toBeInTheDocument();
  });

  it('asks the admin for a code, then rotates and revokes a user (#259)', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20, true, true);
    render(
      <GalleryApp
        api={api}
        folder={fakeFolder({ kind: 'none' })}
        now={() => 1_000_000}
        pollMs={0}
      />,
    );
    fireEvent.click(await screen.findByRole('button', { name: 'Verwaltung' }));
    const field = await screen.findByLabelText('Code aus der Authenticator-App');
    // The field has the focus on arrival, and the sixth digit sends the code.
    expect(field).toHaveFocus();
    fireEvent.change(field, { target: { value: '000000' } });
    expect(await screen.findByRole('alert')).toHaveTextContent('Der Code stimmt nicht');
    await waitFor(() =>
      expect(screen.getByLabelText('Code aus der Authenticator-App')).toHaveFocus(),
    );

    fireEvent.change(screen.getByLabelText('Code aus der Authenticator-App'), {
      target: { value: '123456' },
    });
    const hub = await screen.findByRole('article', { name: 'Hub Garten' });
    const users = within(hub).getByRole('list', { name: 'Nutzer Garten' });
    // Only ordinary users get buttons, never the admin account.
    expect(within(users).getAllByRole('button', { name: 'Neuer Link' })).toHaveLength(1);

    fireEvent.click(within(users).getByRole('button', { name: 'Neuer Link' }));
    fireEvent.click(await screen.findByRole('button', { name: 'Neuen Link erstellen' }));
    const link = await screen.findByLabelText('Neuer Einrichtungslink');
    expect((link as HTMLInputElement).value).toMatch(/#token=1000000000000001\./);

    fireEvent.click(within(users).getByRole('button', { name: 'Sperren' }));
    const dialog = await screen.findByRole('alertdialog', { name: 'Bestätigen' });
    fireEvent.click(within(dialog).getByRole('button', { name: 'Sperren' }));
    expect(await screen.findByText(/ist gesperrt/)).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'Protokoll anzeigen' }));
    expect(await screen.findByText(/Admin angemeldet/)).toBeInTheDocument();
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
    const users = within(hub).getByRole('list', { name: 'Nutzer Garten' });
    expect(users).toHaveTextContent('Anwender');
    expect(users).toHaveTextContent('Admin (Admin)');

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

    // Connection with the mobile network, and the firmware events (#149): the
    // latest in the line, the last five on tap.
    expect(within(hub).getByText('Mobilfunk (Telekom.de), Signal -91 dBm (mittel)')).toBeVisible();
    expect(
      within(hub).getByText(/v0\.1\.13 → v0\.1\.14 .*übersprungen: Akku zu schwach/),
    ).toBeVisible();
    expect(within(hub).queryByRole('list', { name: 'Firmware-Verlauf Garten' })).toBeNull();
    fireEvent.click(within(hub).getByRole('button', { name: 'Verlauf (3)' }));
    expect(within(hub).getByRole('list', { name: 'Firmware-Verlauf Garten' })).toHaveTextContent(
      'v0.1.12 → v0.1.13',
    );
    fireEvent.click(within(hub).getByRole('button', { name: 'Verlauf ausblenden' }));
    expect(within(hub).queryByRole('list', { name: 'Firmware-Verlauf Garten' })).toBeNull();

    // Battery history (#76), loaded when asked for.
    fireEvent.click(within(hub).getByRole('button', { name: 'Akku-Verlauf (30 Tage)' }));
    expect(await within(hub).findByRole('img', { name: /^Hub: Akku/ })).toBeInTheDocument();
    expect(
      within(hub).getByRole('img', { name: 'Kamera „Garten“: Akku 95 % → 66 %' }),
    ).toBeInTheDocument();
    // Consumption per hour and the forecast for the empty battery.
    expect(within(hub).getAllByText(/Verbrauch: \d+,\d %\/h/).length).toBeGreaterThan(0);

    fireEvent.click(screen.getByRole('button', { name: 'Zur Galerie' }));
    expect(await screen.findAllByRole('listitem')).toHaveLength(32);
  });

  it('renames a camera in the manage tab (#273)', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={0} />);
    await screen.findByRole('heading', { name: 'Alle Kameras' });
    fireEvent.click(screen.getByRole('tab', { name: /^Kameras verwalten/ }));
    const panel = screen.getByRole('region', { name: 'Kameras verwalten' });
    fireEvent.click(within(panel).getByRole('button', { name: 'Wiese umbenennen' }));
    fireEvent.change(within(panel).getByLabelText('Neuer Name der Kamera'), {
      target: { value: 'Hecke' },
    });
    fireEvent.click(within(panel).getByRole('button', { name: 'Speichern' }));
    expect(await screen.findByRole('tab', { name: 'Hecke' })).toBeInTheDocument();
    expect(within(panel).getByRole('row', { name: /Hecke/ })).toBeInTheDocument();
    expect(within(panel).queryByLabelText('Neuer Name der Kamera')).not.toBeInTheDocument();
  });

  it('changes camera settings and sends only what changed (#368)', async () => {
    let now = 1_000_000;
    const api = new DemoGalleryApi(() => now, 20);
    const change = vi.spyOn(api, 'changeSettings');
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={0} />);
    await screen.findByRole('heading', { name: 'Alle Kameras' });
    fireEvent.click(screen.getByRole('tab', { name: /^Kameras verwalten/ }));
    const panel = screen.getByRole('region', { name: 'Kameras verwalten' });
    const garten = within(panel).getByRole('row', { name: /Garten/ });
    fireEvent.click(within(garten).getByRole('button', { name: 'Einstellungen' }));

    let dialog = screen.getByRole('dialog');
    expect(dialog).toHaveTextContent('Einstellungen der Kamera „Garten“');
    expect(within(dialog).getByLabelText('Empfindlichkeit')).toHaveValue('2');
    // timeFormat 0 is the factory setting: 24 hours.
    expect(within(dialog).getByLabelText('Uhrzeit')).toHaveDisplayValue('24 Stunden');
    // The camera did not report spkVol: not offered.
    expect(within(dialog).queryByLabelText('Lautsprecher')).not.toBeInTheDocument();
    // Only with working hours switched on.
    expect(within(dialog).queryByLabelText('Von')).not.toBeInTheDocument();
    expect(within(dialog).getByRole('button', { name: 'Speichern' })).toBeDisabled();

    fireEvent.change(within(dialog).getByLabelText('Empfindlichkeit'), { target: { value: '0' } });
    fireEvent.click(within(dialog).getByLabelText('Datum und Uhrzeit im Bild'));
    fireEvent.click(within(dialog).getByLabelText('Nur zu bestimmten Zeiten aufnehmen'));
    fireEvent.change(within(dialog).getByLabelText('Von'), { target: { value: '18:00' } });
    fireEvent.change(within(dialog).getByLabelText('Sprache der Kamera'), {
      target: { value: 'de' },
    });
    fireEvent.click(within(dialog).getByRole('button', { name: 'Speichern' }));

    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(change).toHaveBeenCalledWith('F4C15606ED3B', {
      pirSens: 0,
      osdInfoEnable: false,
      workTimeOnOff: 1,
      workTimeStartHour: 18,
      languageCode: 'de',
    });
    expect(
      await screen.findByText(/beim nächsten Kontakt mit dem Hub an die Kamera/),
    ).toBeInTheDocument();
    expect(within(garten).getByText(/Einstellungen werden geändert/)).toBeInTheDocument();

    // While it waits, the dialog shows the new values and can take it back.
    fireEvent.click(within(garten).getByRole('button', { name: 'Einstellungen' }));
    dialog = screen.getByRole('dialog');
    expect(within(dialog).getByLabelText('Empfindlichkeit')).toHaveValue('0');
    expect(within(dialog).getByLabelText('Von')).toHaveValue('18:00');
    fireEvent.click(within(dialog).getByRole('button', { name: 'Zurücknehmen' }));
    expect(await screen.findByText('Die Änderung ist zurückgenommen.')).toBeInTheDocument();
    fireEvent.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Schließen' }));

    // A change the demo hub delivers ends up in the camera's settings.
    await api.changeSettings('F4C15606ED3B', { pirSens: 1 });
    now += 20;
    expect((await api.status()).cameras[0]?.settings?.pirSens).toBe(1);
  });

  it('hides a camera and shows it again (#273)', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={0} />);
    await screen.findByRole('heading', { name: 'Alle Kameras' });
    fireEvent.click(screen.getByRole('tab', { name: /^Kameras verwalten/ }));
    const panel = screen.getByRole('region', { name: 'Kameras verwalten' });
    const garten = within(panel).getByRole('row', { name: /Garten/ });
    fireEvent.click(within(garten).getByRole('button', { name: 'Ausblenden' }));
    const dialog = screen.getByRole('dialog');
    expect(dialog).toHaveTextContent('Kamera „Garten“ ausblenden?');
    fireEvent.click(within(dialog).getByRole('button', { name: 'Ja, ausblenden' }));

    expect(await within(garten).findByText(/ausgeblendet/)).toBeInTheDocument();
    expect(screen.queryByRole('tab', { name: 'Garten' })).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('tab', { name: 'Wiese' }));
    expect(await screen.findByRole('heading', { name: 'Kamera „Wiese“' })).toBeInTheDocument();

    fireEvent.click(screen.getByRole('tab', { name: /^Kameras verwalten/ }));
    const again = within(screen.getByRole('region', { name: 'Kameras verwalten' })).getByRole(
      'row',
      { name: /Garten/ },
    );
    fireEvent.click(within(again).getByRole('button', { name: 'Wieder anzeigen' }));
    fireEvent.click(await screen.findByRole('tab', { name: 'Garten' }));
    expect(await screen.findByRole('heading', { name: 'Kamera „Garten“' })).toBeInTheDocument();
    expect(screen.getByRole('tab', { name: 'Garten' })).toHaveAttribute('aria-selected', 'true');
  });

  it('takes a found camera in and stops reading one (#298)', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={0} />);
    await screen.findByRole('heading', { name: 'Alle Kameras' });
    // The tab counts the new camera.
    fireEvent.click(await screen.findByRole('tab', { name: /^Kameras verwalten.*1 neu/ }));
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

    const panel = screen.getByRole('region', { name: 'Kameras verwalten' });
    const garten = within(panel).getByRole('row', { name: /Garten/ });
    fireEvent.click(within(garten).getByRole('button', { name: 'Nicht mehr abfragen' }));
    const dialog = screen.getByRole('dialog');
    expect(dialog).toHaveTextContent('Kamera „Garten“ nicht mehr abfragen?');
    fireEvent.click(within(dialog).getByRole('button', { name: 'Ja, nicht mehr abfragen' }));
    expect(await within(garten).findByText(/wird nicht abgefragt/)).toBeInTheDocument();

    fireEvent.click(within(garten).getByRole('button', { name: 'Doch aufnehmen' }));
    expect(
      await within(garten).findByRole('button', { name: 'Nicht mehr abfragen' }),
    ).toBeInTheDocument();
  });

  it('shows a camera taken in before the hub has read it', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={0} />);
    await screen.findByRole('heading', { name: 'Alle Kameras' });
    fireEvent.click(screen.getByRole('tab', { name: /^Kameras verwalten/ }));
    const fresh = await screen.findByRole('group', { name: 'Neue Kamera TC100_77A1' });
    fireEvent.change(within(fresh).getByLabelText(/Name der Kamera/), {
      target: { value: 'Futterplatz' },
    });
    fireEvent.click(within(fresh).getByRole('button', { name: 'Aufnehmen' }));
    expect(await screen.findByText(/ohne Aufnahmen bei Ihren Kameras/)).toBeInTheDocument();

    // Listed with the read cameras, not gone until the next scan.
    const panel = screen.getByRole('region', { name: 'Kameras verwalten' });
    const row = within(panel).getByRole('row', { name: /Futterplatz/ });
    expect(row).toHaveTextContent('noch nicht erreicht');

    fireEvent.click(screen.getByRole('tab', { name: 'Futterplatz noch nicht erreicht' }));
    expect(screen.getByRole('heading', { name: 'Kamera „Futterplatz“' })).toBeInTheDocument();
    expect(screen.getByRole('status', { name: 'Hinweise zur Kamera' })).toHaveTextContent(
      /noch nicht erreicht\. Er liest sie beim nächsten Kontakt \(gegen .*\)/,
    );
    expect(screen.getByText('Noch keine Aufnahmen.')).toBeInTheDocument();
    expect(screen.queryByRole('list', { name: 'Aufnahmen' })).not.toBeInTheDocument();

    // A name given now is the one it gets at its first read.
    fireEvent.click(screen.getByRole('button', { name: 'Umbenennen' }));
    fireEvent.change(screen.getByLabelText('Neuer Name der Kamera'), {
      target: { value: 'Futterstelle' },
    });
    fireEvent.click(screen.getByRole('button', { name: 'Speichern' }));
    expect(
      await screen.findByRole('tab', { name: 'Futterstelle noch nicht erreicht' }),
    ).toHaveAttribute('aria-selected', 'true');

    // Back to a read camera and its recordings.
    fireEvent.click(screen.getByRole('tab', { name: 'Garten' }));
    expect(await screen.findAllByRole('listitem')).toHaveLength(16);
  });

  it('opens the camera with its recordings once the hub has read it', async () => {
    let reached = false;
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    const foundCameras = api.foundCameras.bind(api);
    // Stands in for the first read: the waiting camera gets the id of Wiese.
    api.foundCameras = async () =>
      (await foundCameras()).map((f) =>
        reached && f.bleAddress === 'AA:BB:CC:00:77:A1' ? { ...f, camId: 'A1B2C3D4E5F6' } : f,
      );
    await api.decideCamera('garten-hub', 'AA:BB:CC:00:77:A1', 'approved', 'Futterplatz');
    // Polls fast, as the page does every 15 s.
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={50} />);
    fireEvent.click(await screen.findByRole('tab', { name: 'Futterplatz noch nicht erreicht' }));
    expect(await screen.findByText('Noch keine Aufnahmen.')).toBeInTheDocument();

    reached = true;
    await waitFor(() =>
      expect(screen.queryByRole('tab', { name: /noch nicht erreicht/ })).not.toBeInTheDocument(),
    );
    expect(await screen.findAllByRole('listitem')).toHaveLength(16);
    expect(screen.getByRole('tab', { name: 'Wiese' })).toHaveAttribute('aria-selected', 'true');
  });

  it('shows a recording large and plays its video (#306)', async () => {
    let now = 1_000_000;
    const api = new DemoGalleryApi(() => now, 20);
    render(
      <GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} now={() => now} pollMs={0} />,
    );
    await openCamera('Garten');
    const [card] = await screen.findAllByRole('listitem');
    fireEvent.click(within(card!).getByRole('button', { name: /^Groß anzeigen/ }));
    const viewer = screen.getByRole('dialog', { name: /^Aufnahme vom/ });
    expect(within(viewer).getByRole('img')).toBeInTheDocument();
    fireEvent.keyDown(viewer, { key: 'Escape' });
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();

    fireEvent.click(within(card!).getByRole('button', { name: 'Video holen' }));
    await within(card!).findByText(/^Kommt gegen/);
    now += 20;
    fireEvent.click(screen.getByRole('tab', { name: 'Wiese' }));
    fireEvent.click(await screen.findByRole('tab', { name: 'Garten' }));
    const [ready] = await screen.findAllByRole('listitem');
    fireEvent.click(await within(ready!).findByRole('button', { name: 'Video ansehen' }));
    const player = screen.getByRole('dialog', { name: /^Aufnahme vom/ });
    expect(player).toHaveTextContent('In der Vorführung gibt es kein echtes Video.');
    fireEvent.click(within(player).getByRole('button', { name: 'Schließen' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it("shows the camera's battery and what needs a look (#304)", async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={0} />);
    await screen.findByRole('heading', { name: 'Alle Kameras' });
    // The view of all cameras lists the hints of every camera.
    expect(screen.getByRole('status', { name: 'Hinweise zu den Kameras' })).toHaveTextContent(
      'Kamera „Wiese“: Der Akku der Kamera wird schwach (18 %).',
    );
    await openCamera('Garten');
    expect(screen.getByText(/^Akku 76 % · zuletzt gelesen/)).toBeInTheDocument();
    expect(screen.queryByRole('status', { name: 'Hinweise zur Kamera' })).not.toBeInTheDocument();

    fireEvent.click(screen.getByRole('tab', { name: 'Wiese' }));
    const hints = await screen.findByRole('status', { name: 'Hinweise zur Kamera' });
    expect(hints).toHaveTextContent('Der Akku der Kamera wird schwach (18 %).');
    expect(hints).toHaveTextContent('Die SD-Karte der Kamera ist fast voll.');
  });

  it('asks the hub to look for cameras (#308)', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={0} />);
    await screen.findByRole('heading', { name: 'Alle Kameras' });
    fireEvent.click(screen.getByRole('tab', { name: /^Kameras verwalten/ }));
    fireEvent.click(await screen.findByRole('button', { name: 'Nach Kameras suchen' }));
    expect(
      await screen.findByText(/Der Hub sucht beim nächsten Kontakt \(gegen .*\) nach Kameras/),
    ).toBeInTheDocument();
  });

  it('shows all cameras by day and filters by state (#431)', async () => {
    const now = 1_000_000;
    const api = new DemoGalleryApi(() => now, 20);
    render(
      <GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} now={() => now} pollMs={0} />,
    );
    expect(await screen.findByRole('heading', { name: 'Alle Kameras' })).toBeInTheDocument();
    expect(screen.getByRole('tab', { name: 'Alle Kameras' })).toHaveAttribute(
      'aria-selected',
      'true',
    );
    const tiles = await screen.findAllByRole('listitem');
    expect(tiles).toHaveLength(32);
    // The newest recordings are today's; the camera is named on the tile.
    expect(screen.getByRole('heading', { name: 'Heute' })).toBeInTheDocument();
    expect(within(tiles[0]!).getByText(/^(Garten|Wiese)$/)).toBeInTheDocument();

    // Nothing is ready or failed yet: those filters are not offered.
    const filters = screen.getByRole('group', { name: 'Zustand' });
    expect(within(filters).queryByRole('button', { name: /^Video da/ })).not.toBeInTheDocument();
    fireEvent.click(within(tiles[0]!).getByRole('button', { name: 'Video holen' }));
    fireEvent.click(await within(filters).findByRole('button', { name: /^Wartet/ }));
    expect(screen.getAllByRole('listitem')).toHaveLength(1);
    fireEvent.click(within(filters).getByRole('button', { name: /^Alle$/ }));
    expect(screen.getAllByRole('listitem')).toHaveLength(32);
  });

  it('asks for a folder when none is chosen', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    render(<GalleryApp api={api} folder={fakeFolder({ kind: 'none' })} pollMs={0} />);
    fireEvent.click(await screen.findByRole('button', { name: 'Ordner wählen' }));
    expect(await screen.findByText(/im Ordner „Wildkamera“ gespeichert/)).toBeInTheDocument();
  });
});
