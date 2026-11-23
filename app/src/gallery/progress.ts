import { clockTime } from './text';
import type { Recording } from './types';

/** One line of the progress list in the large view (#431). */
export interface Step {
  text: string;
  status: 'done' | 'current' | 'todo' | 'failed';
}

/**
 * How far a recording is on its way from the camera to the laptop: thumbnail,
 * video, saved. Explains the hub's way of working (it only fetches at its
 * next contact) in the user's words.
 */
export function progressSteps(
  r: Pick<Recording, 'state' | 'fileType' | 'failReason'>,
  saved: boolean,
  nextContact: number | undefined,
  now: number,
): Step[] {
  const thumb: Step = { text: 'Vorschaubild ist da', status: 'done' };
  if (r.fileType !== 2) return [thumb];
  switch (r.state) {
    case 'new':
      return [
        thumb,
        { text: 'Video noch nicht angefordert', status: 'todo' },
        { text: 'Im Ordner gespeichert', status: 'todo' },
      ];
    case 'videoRequested':
      return [
        thumb,
        {
          text: nextContact
            ? `Video angefordert – der Hub holt es gegen ${clockTime(nextContact, now)}`
            : 'Video angefordert – der Hub holt es beim nächsten Kontakt',
          status: 'current',
        },
        { text: 'Im Ordner gespeichert', status: 'todo' },
      ];
    case 'videoReady':
      return [
        thumb,
        { text: 'Video ist da', status: 'done' },
        saved
          ? { text: 'Im Ordner gespeichert', status: 'done' }
          : { text: 'Wird im Ordner gespeichert', status: 'current' },
      ];
    case 'failed':
      return [
        thumb,
        {
          text: r.failReason ? `Hat nicht geklappt: ${r.failReason}` : 'Hat nicht geklappt',
          status: 'failed',
        },
        { text: 'Im Ordner gespeichert', status: 'todo' },
      ];
    case 'deleteRequested':
    case 'deleted':
      return [
        thumb,
        {
          text: 'Löschen angefordert – der Hub löscht beim nächsten Kontakt',
          status: 'current',
        },
      ];
  }
}
