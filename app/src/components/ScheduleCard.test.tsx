import { cleanup, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { DemoApiClient } from '../api/demo';
import type { Camera, ScheduleStretch } from '../api/types';
import { ScheduleCard } from './ScheduleCard';

const NOW = 1_000_000;

async function demoCamera(): Promise<Camera> {
  const [camera] = await new DemoApiClient(() => NOW, 0).listCameras();
  return camera;
}

function show(camera: Camera) {
  render(
    <ScheduleCard
      camera={camera}
      now={NOW}
      busy={false}
      onReadNow={vi.fn()}
      onSaveSchedule={vi.fn()}
    />,
  );
}

describe('ScheduleCard', () => {
  afterEach(cleanup);

  it('says nothing about saving power while the schedule is not stretched', async () => {
    show(await demoCamera());
    expect(screen.queryByText('Saving power')).not.toBeInTheDocument();
  });

  it.each<[ScheduleStretch, RegExp]>([
    ['hubBatteryCritical', /Hub battery critical/],
    ['hubBatteryLow', /Hub battery low/],
    ['cameraBatteryLow', /Camera battery low/],
  ])('says why the schedule is stretched (%s)', async (stretch, text) => {
    const camera = await demoCamera();
    const schedule = camera.hubSchedule;
    if (!schedule) throw new Error('the demo hub has a schedule');
    show({ ...camera, hubSchedule: { ...schedule, stretch } });
    expect(screen.getByText('Saving power')).toBeInTheDocument();
    expect(screen.getByText(text)).toBeInTheDocument();
  });
});
