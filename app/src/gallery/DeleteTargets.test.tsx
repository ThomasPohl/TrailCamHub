import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { DeleteDialog, DeleteTargetsDialog, losesVideo, targetNames } from './DeleteTargets';
import type { GalleryStatus, Recording } from './types';

type Hub = NonNullable<GalleryStatus['hub']>;

const hub = (over: Partial<Hub> = {}): Hub => ({
  hubId: 'hub-1',
  config: {},
  deleteTargets: ['hub', 'backend'],
  deletesByTarget: true,
  ...over,
});

const recording = (over: Partial<Recording>): Recording => ({
  mediaId: 1,
  mediaIds: [1],
  camId: 'CAM1',
  fileType: 2,
  cameraFiles: ['DSCF0001.AVI'],
  mediaTime: 0,
  thumbUrl: '',
  state: 'new',
  fileName: 'x.mp4',
  ...over,
});

describe('delete targets (#461)', () => {
  it('names the targets in a fixed order', () => {
    expect(targetNames(['backend', 'camera'])).toBe('Kamera, Galerie');
  });

  it('only a video that is not in the gallery yet is lost', () => {
    expect(losesVideo(recording({ state: 'new' }))).toBe(true);
    expect(losesVideo(recording({ state: 'videoReady' }))).toBe(false);
    expect(losesVideo(recording({ fileType: 0, state: 'new' }))).toBe(false);
  });

  it('shows where it deletes and confirms with the targets', () => {
    const onConfirm = vi.fn();
    render(
      <DeleteDialog
        question="1 Aufnahme löschen?"
        items={[recording({})]}
        hub={hub()}
        onCancel={() => undefined}
        onConfirm={onConfirm}
      />,
    );
    expect(screen.getByText('Hub, Galerie')).toBeInTheDocument();
    // No camera among the targets: nothing is lost.
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Ja, löschen' }));
    expect(onConfirm).toHaveBeenCalledWith(['hub', 'backend']);
  });

  it('warns about a video that is not in the gallery when the camera is a target', () => {
    const onConfirm = vi.fn();
    render(
      <DeleteDialog
        question="1 Aufnahme löschen?"
        items={[recording({})]}
        hub={hub()}
        onCancel={() => undefined}
        onConfirm={onConfirm}
      />,
    );
    fireEvent.click(screen.getByRole('button', { name: 'Ändern' }));
    fireEvent.click(screen.getByRole('checkbox', { name: /Kamera/ }));
    expect(screen.getByRole('alert')).toHaveTextContent(/noch nicht in der Galerie/);
    fireEvent.click(screen.getByRole('button', { name: 'Trotzdem löschen' }));
    expect(onConfirm).toHaveBeenCalledWith(['camera', 'hub', 'backend']);
  });

  it('does not delete without a target', () => {
    render(
      <DeleteDialog
        question="1 Aufnahme löschen?"
        items={[recording({ state: 'videoReady' })]}
        hub={hub({ deleteTargets: ['backend'] })}
        onCancel={() => undefined}
        onConfirm={() => undefined}
      />,
    );
    fireEvent.click(screen.getByRole('button', { name: 'Ändern' }));
    fireEvent.click(screen.getByRole('checkbox', { name: /Galerie/ }));
    expect(screen.getByRole('button', { name: 'Ja, löschen' })).toBeDisabled();
  });

  it('offers no hub target while the hub has the old firmware', () => {
    render(
      <DeleteDialog
        question="1 Aufnahme löschen?"
        items={[recording({ state: 'videoReady' })]}
        hub={hub({ deleteTargets: ['camera', 'backend'], deletesByTarget: false })}
        onCancel={() => undefined}
        onConfirm={() => undefined}
      />,
    );
    fireEvent.click(screen.getByRole('button', { name: 'Ändern' }));
    expect(screen.getByRole('checkbox', { name: /Hub/ })).toBeDisabled();
    expect(screen.getByText(/neue Firmware/)).toBeInTheDocument();
  });

  it('saves the hub setting', async () => {
    const setDeleteTargets = vi.fn().mockResolvedValue(undefined);
    const onSaved = vi.fn();
    const onClose = vi.fn();
    render(
      <DeleteTargetsDialog
        api={{ setDeleteTargets }}
        hub={hub()}
        onSaved={onSaved}
        onClose={onClose}
      />,
    );
    fireEvent.click(screen.getByRole('checkbox', { name: /Hub/ }));
    fireEvent.click(screen.getByRole('button', { name: 'Speichern' }));
    await waitFor(() => expect(onClose).toHaveBeenCalled());
    expect(setDeleteTargets).toHaveBeenCalledWith('hub-1', ['backend']);
    expect(onSaved).toHaveBeenCalled();
  });

  it('shows why saving failed', async () => {
    const setDeleteTargets = vi.fn().mockRejectedValue(new Error('409 old firmware'));
    render(
      <DeleteTargetsDialog
        api={{ setDeleteTargets }}
        hub={hub()}
        onSaved={() => undefined}
        onClose={() => undefined}
      />,
    );
    fireEvent.click(screen.getByRole('button', { name: 'Speichern' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('409 old firmware');
  });
});
