import { describe, expect, it } from 'vitest';
import { meanDifference, toGrey, type Pixels } from './stamp';

function pixels(values: number[], width = 2): Pixels {
  return { width, height: values.length / width, grey: Uint8ClampedArray.from(values) };
}

describe('time stamp difference (#347)', () => {
  it('is 0 for the same picture', () => {
    expect(meanDifference(pixels([0, 10, 20, 30]), pixels([0, 10, 20, 30]))).toBe(0);
  });

  it('is the mean absolute difference of the grey values', () => {
    expect(meanDifference(pixels([0, 0, 0, 0]), pixels([10, 20, 30, 40]))).toBe(25);
    expect(meanDifference(pixels([10, 20, 30, 40]), pixels([0, 0, 0, 0]))).toBe(25);
  });

  it('refuses pictures of different sizes', () => {
    expect(() => meanDifference(pixels([0, 0]), pixels([0, 0, 0, 0]))).toThrow();
  });

  it('converts RGBA to grey', () => {
    const grey = toGrey(Uint8ClampedArray.from([255, 255, 255, 255, 0, 0, 0, 255, 255, 0, 0, 255]));
    expect(Array.from(grey)).toEqual([255, 0, 76]);
  });
});
