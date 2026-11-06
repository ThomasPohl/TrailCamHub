import { cleanup, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { MediaItem } from '../api/types';
import { MediaDetail } from './MediaDetail';

const video: MediaItem = {
  mediaId: 4136037235,
  dir: 100,
  num: 35,
  fileType: 2,
  mediaTime: 1_766_505_692,
  thumbUrl: 'https://media.test/get/F4/4136037235.jpg',
  videoState: 'none',
};

function show(item: MediaItem) {
  render(
    <MediaDetail
      item={item}
      busy={false}
      showBackLink={false}
      onBack={vi.fn()}
      onRequestVideo={vi.fn()}
    />,
  );
}

describe('MediaDetail', () => {
  afterEach(cleanup);

  it('offers the download of a video that is not there yet', () => {
    show(video);
    expect(screen.getByRole('img', { name: 'Recording 100_35' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Download video' })).toBeInTheDocument();
    expect(screen.queryByRole('link', { name: 'Open video' })).not.toBeInTheDocument();
  });

  it('plays an uploaded video', () => {
    const url = 'https://media.test/get/F4/4136037235.mp4';
    show({ ...video, videoState: 'available', videoUrl: url });
    const player = screen.getByLabelText('Video 100_35');
    expect(player.tagName).toBe('VIDEO');
    expect(player).toHaveAttribute('src', url);
    expect(player).toHaveAttribute('poster', video.thumbUrl);
    expect(screen.getByRole('link', { name: 'Open video' })).toHaveAttribute('href', url);
    expect(screen.queryByRole('button', { name: 'Download video' })).not.toBeInTheDocument();
  });
});
