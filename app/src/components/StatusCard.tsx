import type { Camera } from '../api/types';
import { batteryLevel, chargingText, formatRelative, formatStorage, storageLevel } from '../format';

interface Props {
  camera: Camera;
  now: number;
}

export function StatusCard({ camera, now }: Props) {
  const noCard = camera.sdCard === 'missing';
  const storage = noCard ? 'critical' : storageLevel(camera.sdFreeMB, camera.sdTotalMB);
  return (
    <section className="card status" aria-label={`Status of ${camera.name}`}>
      <header className="status__header">
        <div>
          <h2 className="status__name">{camera.name}</h2>
          <p className="hint">
            {camera.model} · FW {camera.firmware} · via {camera.hubName}
          </p>
        </div>
        <p className="hint status__seen">Seen {formatRelative(camera.lastSeen, now)}</p>
      </header>
      {noCard && (
        <p className="status__alert" role="alert" data-level="critical">
          No SD card in the camera. It records nothing until a card is inserted.
          {camera.sdError && <span className="hint"> Camera: “{camera.sdError}”</span>}
        </p>
      )}
      {!noCard && storage !== 'ok' && (
        <p className="status__alert" role="alert" data-level={storage}>
          The camera&apos;s SD card is almost full: {formatStorage(camera.sdFreeMB)} free.
        </p>
      )}
      <dl className="metrics">
        <Metric
          label="Camera battery"
          value={`${camera.batPercent} %${chargingText(camera)}`}
          level={batteryLevel(camera.batPercent)}
        />
        {camera.hubBatPercent !== undefined && (
          <Metric
            label="Hub battery"
            value={`${camera.hubBatPercent} %`}
            level={batteryLevel(camera.hubBatPercent)}
          />
        )}
        {noCard ? (
          <Metric label="SD card" value="Missing" level="critical" />
        ) : (
          <Metric label="SD free" value={formatStorage(camera.sdFreeMB)} level={storage} />
        )}
        {camera.temperature !== undefined && (
          <Metric label="Temperature" value={`${camera.temperature} °C`} />
        )}
      </dl>
    </section>
  );
}

function Metric({ label, value, level }: { label: string; value: string; level?: string }) {
  return (
    <div className="metric" data-level={level}>
      <dt>{label}</dt>
      <dd>{value}</dd>
    </div>
  );
}
