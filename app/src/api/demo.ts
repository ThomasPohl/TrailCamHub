/** Night-vision style placeholder thumbnail for the demo, so it needs no binary assets. */
export function placeholderThumb(label: string, hue: number): string {
  const svg =
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 160 120">` +
    `<defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1">` +
    `<stop offset="0" stop-color="hsl(${hue},18%,32%)"/>` +
    `<stop offset="1" stop-color="hsl(${hue},22%,14%)"/></linearGradient></defs>` +
    `<rect width="160" height="120" fill="url(#g)"/>` +
    `<path d="M0 92 Q40 70 80 88 T160 80 V120 H0Z" fill="hsl(${hue},20%,10%)"/>` +
    `<text x="8" y="112" font-family="monospace" font-size="11" fill="#ffffffb0">${label}</text>` +
    `</svg>`;
  return `data:image/svg+xml;charset=utf-8,${encodeURIComponent(svg)}`;
}
