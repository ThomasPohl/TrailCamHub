/**
 * The admin's fault count on the app icon (#265), where the browser has the
 * Badging API (installed web apps in Chromium, Safari on macOS and iOS).
 * Elsewhere nothing happens. Errors (no permission, not installed) are
 * ignored: the count is also shown in the page.
 */
export interface BadgeNavigator {
  setAppBadge?: (count?: number) => Promise<void>;
  clearAppBadge?: () => Promise<void>;
}

export function showFaultBadge(
  count: number,
  nav: BadgeNavigator | undefined = typeof navigator === 'undefined'
    ? undefined
    : (navigator as BadgeNavigator),
): void {
  if (!nav) return;
  const done = count > 0 ? nav.setAppBadge?.(count) : nav.clearAppBadge?.();
  done?.catch(() => {});
}
