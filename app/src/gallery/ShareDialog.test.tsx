import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { DemoGalleryApi } from './demo';
import { ShareDialog } from './ShareDialog';

async function firstRecording(api: DemoGalleryApi) {
  const status = await api.status();
  const camera = status.cameras[0]!;
  const recording = (await api.recordings(camera.camId))[0]!;
  return { camera, recording };
}

describe('ShareDialog (#444)', () => {
  it('creates a link, lists it and takes it back', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    const { camera, recording } = await firstRecording(api);
    const onClose = vi.fn();
    render(
      <ShareDialog
        api={api}
        recording={recording}
        cameraName={camera.name}
        now={1_000_000}
        onClose={onClose}
      />,
    );

    // A video is only offered once it is in the cloud.
    expect(screen.getByLabelText(/Das Video/)).toBeDisabled();
    fireEvent.change(screen.getByLabelText('Gültig für'), { target: { value: '30' } });
    fireEvent.click(screen.getByRole('button', { name: 'Link erstellen' }));

    const url = await screen.findByLabelText(/Dein Link/);
    expect((url as HTMLInputElement).value).toMatch(/\/v1\/s\/demo1$/);
    expect(await screen.findByText(/^Bild, bis/)).toBeInTheDocument();
    expect((await api.shares())[0]).toMatchObject({
      kind: 'thumb',
      showCamera: false,
      expiresAt: 1_000_000 + 30 * 24 * 60 * 60,
    });

    fireEvent.click(screen.getByRole('button', { name: 'Zurückziehen' }));
    await waitFor(() => expect(screen.queryByText(/^Bild, bis/)).not.toBeInTheDocument());
    expect(await api.shares()).toEqual([]);
    expect(await screen.findByText('Link zurückgezogen.')).toBeInTheDocument();
  });

  it('passes the camera name switch on and shows errors', async () => {
    const api = new DemoGalleryApi(() => 1_000_000, 20);
    const { camera, recording } = await firstRecording(api);
    vi.spyOn(api, 'shareRecording').mockRejectedValueOnce(new Error('Kein Netz'));
    render(
      <ShareDialog
        api={api}
        recording={recording}
        cameraName={camera.name}
        now={1_000_000}
        onClose={() => undefined}
      />,
    );
    fireEvent.click(screen.getByLabelText(/Namen der Kamera zeigen/));
    fireEvent.click(screen.getByRole('button', { name: 'Link erstellen' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('Kein Netz');

    fireEvent.click(screen.getByRole('button', { name: 'Link erstellen' }));
    await screen.findByLabelText(/Dein Link/);
    expect((await api.shares())[0]?.showCamera).toBe(true);
  });
});
