import { useState, type FormEvent } from 'react';
import type { Camera, ScheduleConfig, ScheduleOrigin, ScheduleStretch } from '../api/types';
import { formatRelative, formatUpcoming } from '../format';

interface Props {
  camera: Camera;
  now: number;
  busy: boolean;
  onReadNow(): void;
  onSaveSchedule(config: ScheduleConfig): void;
}

const ORIGIN_TEXT: Record<ScheduleOrigin, string> = {
  'built-in': 'hub default',
  local: 'set on the hub',
  remote: 'set in the app',
};

const STRETCH_TEXT: Record<ScheduleStretch, string> = {
  hubBatteryCritical: 'Hub battery critical · no camera reads, the hub only checks in now and then',
  hubBatteryLow: 'Hub battery low · camera reads and check-ins are less frequent',
  cameraBatteryLow: 'Camera battery low · camera reads are less frequent',
};

function minutesText(seconds: number): string {
  if (seconds <= 0) return 'only after camera reads';
  const minutes = Math.round(seconds / 60);
  return minutes % 60 === 0 && minutes >= 60 ? `every ${minutes / 60} h` : `every ${minutes} min`;
}

/**
 * True while the schedule set in the app differs from what the hub reported:
 * the hub takes it at its next check-in.
 */
export function schedulePending(camera: Camera): boolean {
  const schedule = camera.hubSchedule;
  const config = camera.hubScheduleConfig;
  if (!schedule) return config.cameraPlan !== undefined || config.checkinIntervalS !== undefined;
  const planPending =
    config.cameraPlan !== undefined
      ? schedule.cameraPlanOrigin !== 'remote' || schedule.cameraPlan !== config.cameraPlan
      : schedule.cameraPlanOrigin === 'remote';
  const checkinPending =
    config.checkinIntervalS !== undefined
      ? schedule.checkinOrigin !== 'remote' ||
        schedule.checkinIntervalS !== Math.ceil(config.checkinIntervalS / 60) * 60
      : schedule.checkinOrigin === 'remote';
  return planPending || checkinPending;
}

/**
 * When the hub reads this camera and checks in next, a button to read the
 * camera at the next check-in, and the hub's schedule, which can be changed
 * here (it applies to all cameras of the hub).
 */
export function ScheduleCard({ camera, now, busy, onReadNow, onSaveSchedule }: Props) {
  const schedule = camera.hubSchedule;
  const config = camera.hubScheduleConfig;
  const [editing, setEditing] = useState(false);
  const [plan, setPlan] = useState(config.cameraPlan ?? '');
  const [checkinMin, setCheckinMin] = useState(
    config.checkinIntervalS ? String(Math.round(config.checkinIntervalS / 60)) : '',
  );

  const startEditing = () => {
    setPlan(config.cameraPlan ?? '');
    setCheckinMin(config.checkinIntervalS ? String(Math.round(config.checkinIntervalS / 60)) : '');
    setEditing(true);
  };

  const save = (event: FormEvent) => {
    event.preventDefault();
    const next: ScheduleConfig = {};
    if (plan.trim()) next.cameraPlan = plan.trim();
    if (checkinMin.trim()) next.checkinIntervalS = Number(checkinMin) * 60;
    onSaveSchedule(next);
    setEditing(false);
  };

  return (
    <section className="card schedule" aria-label={`Schedule of ${camera.hubName}`}>
      <h2 className="schedule__title">Schedule</h2>
      {schedule ? (
        <dl className="facts">
          <dt>Next camera read</dt>
          <dd>
            {schedule.nextCameraReadAt
              ? formatUpcoming(schedule.nextCameraReadAt, now)
              : 'none planned'}
          </dd>
          <dt>Next check-in</dt>
          <dd>
            {schedule.nextCheckinAt ? formatUpcoming(schedule.nextCheckinAt, now) : 'none planned'}
          </dd>
          <dt>Camera reads</dt>
          <dd>
            <code>{schedule.cameraPlan}</code> · {ORIGIN_TEXT[schedule.cameraPlanOrigin]}
          </dd>
          <dt>Check-ins</dt>
          <dd>
            {minutesText(schedule.checkinIntervalS)} · {ORIGIN_TEXT[schedule.checkinOrigin]}
          </dd>
          <dt>Hub</dt>
          <dd>
            checked in {formatRelative(camera.hubLastSeen, now)}
            {schedule.sleeps ? ' · sleeps between cycles' : ''}
          </dd>
          {schedule.stretch && (
            <>
              <dt>Saving power</dt>
              <dd>{STRETCH_TEXT[schedule.stretch]}</dd>
            </>
          )}
        </dl>
      ) : (
        <p className="hint">The hub has not reported a schedule yet.</p>
      )}

      <button
        type="button"
        className="primary"
        disabled={busy || camera.readRequested}
        onClick={onReadNow}
      >
        Read camera now
      </button>
      {camera.readRequested && (
        <p className="hint schedule__note">
          Read requested · the hub reads the camera after its next check-in.
        </p>
      )}

      {schedulePending(camera) && (
        <p className="hint schedule__note">
          New schedule saved · the hub takes it at its next check-in.
        </p>
      )}
      {!editing && (
        <button type="button" className="link" onClick={startEditing}>
          Change schedule
        </button>
      )}
      {editing && (
        <form className="schedule__form" onSubmit={save}>
          <label>
            Camera read plan
            <input
              name="cameraPlan"
              value={plan}
              placeholder="the hub's own"
              spellCheck={false}
              onChange={(e) => setPlan(e.target.value)}
            />
            <small className="hint">
              Minutes per time window, <code>*</code> for the rest of the day, <code>off</code> for
              none, e.g. <code>05:00-09:00=30,17:00-22:00=30,*=120</code>. Every read costs camera
              battery.
            </small>
          </label>
          <label>
            Check-in every (minutes)
            <input
              name="checkinMin"
              type="number"
              min={5}
              max={1440}
              value={checkinMin}
              placeholder="the hub's own"
              onChange={(e) => setCheckinMin(e.target.value)}
            />
            <small className="hint">
              How long a request from the app waits at most. Empty fields leave the setting to the
              hub.
            </small>
          </label>
          <div className="schedule__actions">
            <button type="submit" className="primary" disabled={busy}>
              Save for {camera.hubName}
            </button>
            <button type="button" className="link" onClick={() => setEditing(false)}>
              Cancel
            </button>
          </div>
        </form>
      )}
    </section>
  );
}
