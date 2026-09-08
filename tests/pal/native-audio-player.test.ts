import type { AudioPlayer as NativeAudioPlayer } from '../../pal/audio/native/player';
import { game } from '../../cocos/game';

const nativeAudio = {
    preload: jest.fn(),
    getPCMHeader: jest.fn(),
    getMaxAudioInstance: jest.fn(() => 32),
    play2d: jest.fn(() => 7),
    getDuration: jest.fn(() => 2.5),
    setFinishCallback: jest.fn(),
    stop: jest.fn(),
    uncache: jest.fn(),
};

let AudioPlayer: typeof NativeAudioPlayer;

beforeAll(() => {
    Object.assign(globalThis, { jsb: { AudioEngine: nativeAudio } });
    AudioPlayer = jest.requireActual('../../pal/audio/native/player').AudioPlayer;
});

beforeEach(() => {
    jest.clearAllMocks();
    nativeAudio.preload.mockImplementation((_url: string, complete: (success: boolean) => void) => complete(true));
    nativeAudio.getPCMHeader.mockReturnValue({ totalFrames: 120000, sampleRate: 48000 });
});

afterEach(() => {
    jest.restoreAllMocks();
});

test('preloaded native audio reports its duration without starting playback', async () => {
    const player = await AudioPlayer.load('music.mp3');
    expect(player.duration).toBe(2.5);
    expect(player.duration).toBe(2.5);
    expect(nativeAudio.getPCMHeader).toHaveBeenCalledTimes(1);
    expect(nativeAudio.getPCMHeader).toHaveBeenCalledWith('music.mp3');
    expect(nativeAudio.play2d).not.toHaveBeenCalled();
    expect(player.sampleRate).toBe(48000);
    player.destroy();
});

test('duration survives playback and stopping', async () => {
    const player = await AudioPlayer.load('music.mp3');
    expect(player.duration).toBe(2.5);
    await player.play();
    expect(player.duration).toBe(2.5);
    await player.stop();
    expect(player.duration).toBe(2.5);
    player.destroy();
});

test('shared native cache remains until the last player is destroyed', async () => {
    const first = await AudioPlayer.load('shared.mp3');
    const second = await AudioPlayer.load('shared.mp3');

    first.destroy();
    expect(nativeAudio.uncache).not.toHaveBeenCalled();
    second.destroy();
    expect(nativeAudio.uncache).toHaveBeenCalledTimes(1);
    expect(nativeAudio.uncache).toHaveBeenCalledWith('shared.mp3');
    first.destroy();
    expect(nativeAudio.uncache).toHaveBeenCalledTimes(1);
});

test('one-shot cache is released on stop and finish', async () => {
    const stopped = await AudioPlayer.loadOneShotAudio('stopped.mp3', 0.5);
    stopped.play();
    stopped.stop();
    expect(nativeAudio.uncache).toHaveBeenCalledTimes(1);
    expect(nativeAudio.uncache).toHaveBeenCalledWith('stopped.mp3');

    const finished = await AudioPlayer.loadOneShotAudio('finished.mp3', 0.5);
    finished.play();
    const finish = nativeAudio.setFinishCallback.mock.calls.at(-1)?.[1] as (() => void) | undefined;
    expect(finish).toBeDefined();
    finish?.();
    expect(nativeAudio.uncache).toHaveBeenCalledTimes(2);
    expect(nativeAudio.uncache).toHaveBeenLastCalledWith('finished.mp3');
});

test.each([
    { totalFrames: 0, sampleRate: 0 },
    { totalFrames: 120000, sampleRate: 0 },
    { totalFrames: -1, sampleRate: 48000 },
    { totalFrames: Number.NaN, sampleRate: 48000 },
    { totalFrames: 120000, sampleRate: Number.POSITIVE_INFINITY },
])('invalid PCM timing refuses a fabricated duration: %p', async (header) => {
    const subscribe = jest.spyOn(game, 'on');
    nativeAudio.getPCMHeader.mockReturnValue(header);
    await expect(AudioPlayer.load('invalid.mp3')).rejects.toThrow('Invalid native audio timing metadata: invalid.mp3');
    expect(nativeAudio.play2d).not.toHaveBeenCalled();
    expect(subscribe).not.toHaveBeenCalled();
    expect(nativeAudio.uncache).not.toHaveBeenCalled();
});

test('preload failure rejects before querying metadata or starting playback', async () => {
    nativeAudio.preload.mockImplementation((_url: string, complete: (success: boolean) => void) => complete(false));
    await expect(AudioPlayer.load('missing.mp3')).rejects.toThrow('load audio failed');
    expect(nativeAudio.getPCMHeader).not.toHaveBeenCalled();
    expect(nativeAudio.play2d).not.toHaveBeenCalled();
});

test('native metadata errors propagate to the caller', async () => {
    const subscribe = jest.spyOn(game, 'on');
    nativeAudio.getPCMHeader.mockImplementation(() => { throw new Error('decoder failure'); });
    await expect(AudioPlayer.load('broken.mp3')).rejects.toThrow('decoder failure');
    expect(subscribe).not.toHaveBeenCalled();
    expect(nativeAudio.uncache).not.toHaveBeenCalled();
});
