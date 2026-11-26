import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import type { HubSchedule } from '../api/types';
import { hintTip, ScheduleEditor, SettingsPage } from './SettingsPage';
import type { GalleryApi, GalleryStatus } from './types';

const schedule: HubSchedule = {
  cameraPlan: 'sunrise-60..sunrise+90=30,sunset-90..sunset+60=30,*=120',
  cameraPlanOrigin: 'built-in',
  checkinIntervalS: 3600,
  checkinOrigin: 'local',
  sleeps: true,
  quiet: '23:00-05:00',
  quietOrigin: 'local',
  utcOffsetMin: 60,
  location: '51.3,9.5',
  locationOrigin: 'gnss',
  sunriseMin: 460,
  sunsetMin: 985,
};

function fakeApi() {
  return {
    setSchedule: vi.fn().mockResolvedValue(undefined),
    readCamera: vi.fn().mockResolvedValue(undefined),
  };
}

function editor(api: ReturnType<typeof fakeApi>, config = {}) {
  const onSaved = vi.fn();
  render(
    <ScheduleEditor
      api={api as unknown as GalleryApi}
      hubId="hub-1"
      config={config}
      schedule={schedule}
      cameraCount={2}
      now={1_700_000_000}
      onSaved={onSaved}
    />,
  );
  return onSaved;
}

describe('schedule editor (#431, #436)', () => {
  it('sends only what the user changed', async () => {
    const api = fakeApi();
    const onSaved = editor(api);
    fireEvent.click(screen.getByRole('button', { name: 'Ganztägig' }));
    fireEvent.click(screen.getByRole('button', { name: 'Übernehmen' }));
    await waitFor(() => expect(onSaved).toHaveBeenCalled());
    expect(api.setSchedule).toHaveBeenCalledWith('hub-1', { cameraPlan: '*=60' });
  });

  it('keeps the power setting and sends quiet hours, location and contact', async () => {
    const api = fakeApi();
    const onSaved = editor(api, { powerSetting: 'battery' });
    fireEvent.click(screen.getByRole('checkbox'));
    fireEvent.change(screen.getByLabelText('Standort (Breite, Länge)'), {
      target: { value: '52.52, 13.41' },
    });
    fireEvent.change(screen.getByLabelText('Der Hub meldet sich'), { target: { value: '120' } });
    fireEvent.click(screen.getByRole('button', { name: 'Übernehmen' }));
    await waitFor(() => expect(onSaved).toHaveBeenCalled());
    expect(api.setSchedule).toHaveBeenCalledWith('hub-1', {
      powerSetting: 'battery',
      quiet: 'off',
      location: '52.5,13.4',
      checkinIntervalS: 7200,
    });
  });

  it('refuses a plan the hub would not take', () => {
    editor(fakeApi());
    fireEvent.change(screen.getByLabelText('Als Text (für Fortgeschrittene)'), {
      target: { value: '05:00=30' },
    });
    expect(screen.getByRole('button', { name: 'Übernehmen' })).toBeDisabled();
    expect(screen.getByText(/es fehlt „=Minuten“/)).toBeInTheDocument();
  });

  it('gives the schedule back to the hub', async () => {
    const api = fakeApi();
    const onSaved = editor(api, { cameraPlan: '*=60', powerSetting: 'mains' });
    fireEvent.click(screen.getByRole('button', { name: 'Hub-Einstellung verwenden' }));
    await waitFor(() => expect(onSaved).toHaveBeenCalled());
    expect(api.setSchedule).toHaveBeenCalledWith('hub-1', { powerSetting: 'mains' });
  });
});

describe('settings page (#431)', () => {
  const status: GalleryStatus = {
    lastContact: 1_700_000_000,
    nextContact: 1_700_003_600,
    hub: {
      hubId: 'hub-1',
      config: {},
      deleteTargets: ['camera', 'backend'],
      deletesByTarget: true,
      schedule,
    },
    cameras: [{ camId: 'CAM1', name: 'Garten', batPercent: 76 }],
  };

  it('asks the hub to read a camera at the next contact', async () => {
    const api = fakeApi();
    render(
      <SettingsPage
        api={api as unknown as GalleryApi}
        status={status}
        now={1_700_000_100}
        onCameraSettings={vi.fn()}
        onManageCameras={vi.fn()}
        onChanged={vi.fn()}
      />,
    );
    fireEvent.click(screen.getByRole('button', { name: 'Jetzt lesen' }));
    await waitFor(() => expect(api.readCamera).toHaveBeenCalledWith('CAM1'));
    expect(await screen.findByRole('button', { name: 'Angefordert' })).toBeDisabled();
  });

  it('tells the user what to do about a hint', () => {
    expect(hintTip({ kind: 'sdNearlyFull', level: 'warning', value: 900 })).toMatch(/löschen/);
    expect(hintTip({ kind: 'requestsFailed', level: 'error' })).toBeUndefined();
  });
});
