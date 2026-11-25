import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { ActivityDialog } from './ActivityDialog';
import type { GalleryStatus, Recording } from './types';

const NOW = Date.UTC(2026, 10, 24, 12) / 1000;

/** Three recordings in each of the hours 16, 17 and 18 on 20 days, 10 minutes apart. */
function recordings(): Recording[] {
  const list: Recording[] = [];
  for (let d = 0; d < 20; d++) {
    for (const hour of [16, 17, 18]) {
      for (let i = 0; i < 3; i++) {
        const mediaTime = Date.UTC(2026, 9, 25 + d, hour, i * 10) / 1000;
        list.push({ mediaTime, mediaId: mediaTime } as Recording);
      }
    }
  }
  return list;
}

const status: GalleryStatus = {
  lastContact: NOW,
  hub: {
    hubId: 'hub-1',
    cameraPlan: '*=120',
    config: { checkinIntervalS: 1800 },
    deleteTargets: ['hub', 'backend'],
    deletesByTarget: true,
  },
  cameras: [{ camId: 'A', name: 'Garten' }],
};

describe('ActivityDialog (#443)', () => {
  it('shows the busy hours and applies the suggested plan without losing other settings', async () => {
    const setSchedule = vi.fn().mockResolvedValue(undefined);
    const api = { recordings: vi.fn().mockResolvedValue(recordings()), setSchedule };
    render(<ActivityDialog api={api} status={status} now={NOW} onClose={() => undefined} />);

    expect(await screen.findByText(/180 Ereignisse: Meiste Aktivität 16–19 Uhr/)).toBeVisible();
    expect(screen.getByRole('table', { name: /Wochentag und Stunde/ })).toBeInTheDocument();
    expect(screen.getByRole('img', { name: /letzten 90 Tagen/ })).toBeInTheDocument();
    expect(screen.getByText('16:00-19:00=30,*=120')).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'Plan übernehmen' }));
    await waitFor(() =>
      expect(setSchedule).toHaveBeenCalledWith('hub-1', {
        checkinIntervalS: 1800,
        cameraPlan: '16:00-19:00=30,*=120',
      }),
    );
    expect(await screen.findByText(/Der Hub übernimmt den Plan/)).toBeInTheDocument();
  });

  it('says why there is no suggestion when data is thin', async () => {
    const api = {
      recordings: vi.fn().mockResolvedValue(recordings().slice(0, 6)),
      setSchedule: vi.fn(),
    };
    render(<ActivityDialog api={api} status={status} now={NOW} onClose={() => undefined} />);
    expect(await screen.findByText(/Noch zu wenig Aufnahmen/)).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Plan übernehmen' })).not.toBeInTheDocument();
  });

  it('copes with no recordings and with a failing load', async () => {
    const empty = { recordings: vi.fn().mockResolvedValue([]), setSchedule: vi.fn() };
    const { unmount } = render(
      <ActivityDialog api={empty} status={status} now={NOW} onClose={() => undefined} />,
    );
    expect(await screen.findByText(/noch keine Aufnahmen/)).toBeInTheDocument();
    unmount();

    const broken = {
      recordings: vi.fn().mockRejectedValue(new Error('Kein Netz')),
      setSchedule: vi.fn(),
    };
    render(<ActivityDialog api={broken} status={status} now={NOW} onClose={() => undefined} />);
    expect(await screen.findByRole('alert')).toHaveTextContent('Kein Netz');
  });
});
