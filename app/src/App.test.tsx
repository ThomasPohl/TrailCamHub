import { cleanup, fireEvent, render, screen, within } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { DemoApiClient } from './api/demo';
import type { ApiClient, Camera } from './api/types';
import { App } from './App';
import { fakeWebApp } from './test-utils';

const NOW = 1_000_000;
const demoApi = () => new DemoApiClient(() => NOW, 0);

describe('App', () => {
  it('shows camera status and recordings', async () => {
    render(<App api={demoApi()} now={() => NOW} />);

    expect(await screen.findByRole('heading', { name: 'Garden' })).toBeInTheDocument();
    const status = screen.getByRole('region', { name: 'Status of Garden' });
    expect(within(status).getByText('78 % · solar charging')).toBeInTheDocument();
    expect(within(status).getByText('14 °C')).toBeInTheDocument();
    expect(within(status).getByText('Seen 17 min ago')).toBeInTheDocument();
    expect(screen.getByRole('note')).toHaveTextContent('Demo mode');
    expect(await screen.findAllByRole('img', { name: /Recording 100_/ })).toHaveLength(12);
  });

  it('switches between cameras and remembers the choice', async () => {
    render(<App api={demoApi()} now={() => NOW} />);

    const garden = await screen.findByRole('button', { name: /Garden/ });
    expect(garden).toHaveAttribute('aria-pressed', 'true');
    expect(screen.getByText(/via Garden hub/)).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: /Meadow/ }));
    expect(await screen.findByRole('heading', { name: 'Meadow' })).toBeInTheDocument();
    expect(await screen.findAllByRole('img', { name: /Recording 100_12/ })).toHaveLength(1);
    expect(screen.queryByRole('img', { name: /Recording 100_36/ })).not.toBeInTheDocument();
    expect(window.localStorage.getItem('trailcamhub.camera')).toBe('A1B2C3D4E5F6');

    // A new session opens the remembered camera.
    cleanup();
    render(<App api={demoApi()} now={() => NOW} />);
    expect(await screen.findByRole('heading', { name: 'Meadow' })).toBeInTheDocument();
  });

  it('hides the picker with a single camera', async () => {
    const api = demoApi();
    const single: ApiClient = {
      isDemo: false,
      listCameras: async () => (await api.listCameras()).slice(0, 1),
      listMedia: (id) => api.listMedia(id),
      requestVideo: (id, item) => api.requestVideo(id, item),
      readCamera: (id) => api.readCamera(id),
      setSchedule: (id, config) => api.setSchedule(id, config),
    };
    render(<App api={single} now={() => NOW} />);
    expect(await screen.findByRole('heading', { name: 'Garden' })).toBeInTheDocument();
    expect(screen.queryByRole('navigation', { name: 'Cameras' })).not.toBeInTheDocument();
  });

  it('warns about a camera without an SD card and about a full card', async () => {
    const api = demoApi();
    const withCards = (cards: Array<Partial<Camera>>): ApiClient => ({
      isDemo: false,
      listCameras: async () =>
        (await api.listCameras()).map((camera, i) => ({ ...camera, ...cards[i] })),
      listMedia: (id) => api.listMedia(id),
      requestVideo: (id, item) => api.requestVideo(id, item),
      readCamera: (id) => api.readCamera(id),
      setSchedule: (id, config) => api.setSchedule(id, config),
    });
    render(
      <App
        api={withCards([
          { sdCard: 'missing', sdError: 'Memory card not exist', sdFreeMB: 0, sdTotalMB: 0 },
          { sdFreeMB: 900, sdTotalMB: 15_193 },
        ])}
        now={() => NOW}
      />,
    );

    const garden = await screen.findByRole('region', { name: 'Status of Garden' });
    expect(within(garden).getByRole('alert')).toHaveTextContent('No SD card in the camera');
    expect(within(garden).getByRole('alert')).toHaveTextContent('Memory card not exist');
    expect(within(garden).getByText('Missing')).toBeInTheDocument();
    expect(within(garden).queryByText('SD free')).not.toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: /Meadow/ }));
    const meadow = await screen.findByRole('region', { name: 'Status of Meadow' });
    expect(within(meadow).getByRole('alert')).toHaveTextContent('almost full: 900 MB free');
  });

  it('greets the Telegram user', async () => {
    render(<App api={demoApi()} webApp={fakeWebApp()} now={() => NOW} />);
    expect(await screen.findByText('Hi Thomas')).toBeInTheDocument();
  });

  it('requests a video from the detail view and goes back', async () => {
    render(<App api={demoApi()} now={() => NOW} />);

    fireEvent.click(await screen.findByRole('button', { name: /Recording 100_33/ }));
    fireEvent.click(screen.getByRole('button', { name: 'Download video' }));

    expect(await screen.findByText(/Requested/)).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Download video' })).not.toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: '← All recordings' }));
    expect(await screen.findByRole('heading', { name: 'Recordings' })).toBeInTheDocument();
  });

  it('uses the native Telegram back button inside Telegram', async () => {
    const webApp = fakeWebApp();
    render(<App api={demoApi()} webApp={webApp} now={() => NOW} />);

    fireEvent.click(await screen.findByRole('button', { name: /Recording 100_36/ }));

    expect(webApp.BackButton.show).toHaveBeenCalled();
    expect(screen.queryByRole('button', { name: '← All recordings' })).not.toBeInTheDocument();
  });

  it('shows the schedule and asks for a camera read', async () => {
    render(<App api={demoApi()} now={() => NOW} />);

    const schedule = await screen.findByRole('region', { name: 'Schedule of Garden hub' });
    expect(within(schedule).getByText(/in 1 h 43 min/)).toBeInTheDocument();
    expect(within(schedule).getByText(/in 26 min/)).toBeInTheDocument();
    expect(within(schedule).getByText(/sleeps between cycles/)).toBeInTheDocument();

    fireEvent.click(within(schedule).getByRole('button', { name: 'Read camera now' }));
    expect(await within(schedule).findByText(/Read requested/)).toBeInTheDocument();
    expect(within(schedule).getByRole('button', { name: 'Read camera now' })).toBeDisabled();
  });

  it('changes the schedule of the hub', async () => {
    const api = demoApi();
    render(<App api={api} now={() => NOW} />);

    const schedule = await screen.findByRole('region', { name: 'Schedule of Garden hub' });
    fireEvent.click(within(schedule).getByRole('button', { name: 'Change schedule' }));
    fireEvent.change(within(schedule).getByLabelText(/Camera read plan/), {
      target: { value: '*=60' },
    });
    fireEvent.change(within(schedule).getByLabelText(/Check-in every/), {
      target: { value: '15' },
    });
    fireEvent.click(within(schedule).getByRole('button', { name: 'Save for Garden hub' }));

    expect(await within(schedule).findByText(/New schedule saved/)).toBeInTheDocument();
    const cameras = await api.listCameras();
    expect(cameras[0]?.hubScheduleConfig).toEqual({ cameraPlan: '*=60', checkinIntervalS: 900 });
  });

  it('shows an error with retry when loading fails', async () => {
    const failing: ApiClient = {
      isDemo: false,
      listCameras: () => Promise.reject(new Error('HTTP 503')),
      listMedia: () => Promise.resolve([]),
      requestVideo: () => Promise.reject(new Error('unused')),
      readCamera: () => Promise.reject(new Error('unused')),
      setSchedule: () => Promise.reject(new Error('unused')),
    };
    render(<App api={failing} />);

    expect(await screen.findByRole('alert')).toHaveTextContent('HTTP 503');
    expect(screen.getByRole('button', { name: 'Try again' })).toBeInTheDocument();
  });
});
