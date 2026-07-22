import { mergeLiveDataUpdate } from './live-data-merge';

describe('mergeLiveDataUpdate', () => {
  it('preserves unchanged ASIC health fields across nested WebSocket diffs', () => {
    const initial = {
      asicHealth: {
        boardTemperatureC: 44.3,
        fanPercent: 100,
        fanRPM: 3540,
        bridgeVersion: '0.0.1+g42f14c1',
      },
    };

    const temperatureUpdate = mergeLiveDataUpdate(initial, {
      asicHealth: { boardTemperatureC: 44.4 },
    });
    const fanUpdate = mergeLiveDataUpdate(temperatureUpdate, {
      asicHealth: { fanRPM: 3600 },
    });

    expect(fanUpdate.asicHealth).toEqual({
      boardTemperatureC: 44.4,
      fanPercent: 100,
      fanRPM: 3600,
      bridgeVersion: '0.0.1+g42f14c1',
    });
  });

  it('replaces arrays from a WebSocket diff', () => {
    const current = { blockSignals: [{ bit: 1 }] };
    const update = { blockSignals: [{ bit: 2 }] };

    expect(mergeLiveDataUpdate(current, update)).toEqual(update);
  });
});
