/**
 * The gallery user's app token (#259). The set-up link carries it in the
 * fragment (`gallery.html#token=…`), which the browser never sends to a
 * server; the page keeps it in localStorage and removes it from the address.
 */
const KEY = 'trailcamhub.gallery.token';
const TOKEN = /^\d{16}\.[A-Za-z0-9_-]{16,128}$/;

export function takeToken(win: Window = window): string | undefined {
  const fromLink = new URLSearchParams(win.location.hash.slice(1)).get('token') ?? undefined;
  try {
    if (fromLink && TOKEN.test(fromLink)) {
      win.localStorage.setItem(KEY, fromLink);
    }
  } catch {
    // Without storage the link has to be opened again next time.
  }
  if (fromLink) {
    win.history.replaceState(null, '', win.location.pathname + win.location.search);
  }
  if (fromLink && TOKEN.test(fromLink)) return fromLink;
  try {
    return win.localStorage.getItem(KEY) ?? undefined;
  } catch {
    return undefined;
  }
}
