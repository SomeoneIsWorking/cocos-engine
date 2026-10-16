import { LetterAtlas, LetterTexture } from '../../cocos/2d/assembler/label/font-utils';
import { UIRendererManager } from '../../cocos/2d/framework/ui-renderer-manager';
import { UIRenderer } from '../../cocos/2d/framework/ui-renderer';

function letterOf (hash: string, size: number): LetterTexture {
    return { image: { width: size, height: size }, width: size, height: size, hash, offsetY: 0 } as unknown as LetterTexture;
}

test('a full letter atlas empties itself and places the letter', () => {
    const atlas = new LetterAtlas(64, 64);
    let emptied = 0;
    atlas.setOnEmptied(() => { emptied++; });
    let placed = 0;
    while (atlas.insertLetterTexture(letterOf(`a${placed}`, 20)) && atlas.generation === 0) {
        placed++;
    }
    expect(placed).toBeGreaterThan(0);
    expect(atlas.generation).toBe(1);
    expect(emptied).toBe(1);
    expect(atlas.getLetter(`a0`)).toBeUndefined();
    expect(atlas.getLetter(`a${placed}`)).toBeDefined();
});

test('a letter atlas empties at most once a frame', () => {
    const atlas = new LetterAtlas(64, 64);
    let inserted = 0;
    while (atlas.insertLetterTexture(letterOf(`b${inserted}`, 20))) {
        inserted++;
    }
    expect(atlas.generation).toBe(1);
});

test('a letter bigger than an empty atlas is refused without emptying it', () => {
    const atlas = new LetterAtlas(64, 64);
    expect(atlas.insertLetterTexture(letterOf('c', 100))).toBeNull();
    expect(atlas.generation).toBe(0);
});

test('a renderer marked by a later renderer in the same pass is updated again in that pass', () => {
    const manager = new UIRendererManager();
    const updates: string[] = [];
    const first = { _internalId: -1, _dirtyVersion: -1, updateRenderer: () => { updates.push('first'); } } as unknown as UIRenderer;
    const second = {
        _internalId: -1,
        _dirtyVersion: -1,
        updateRenderer: () => { updates.push('second'); manager.markDirtyRenderer(first); },
    } as unknown as UIRenderer;
    manager.addRenderer(first);
    manager.addRenderer(second);
    manager.markDirtyRenderer(first);
    manager.markDirtyRenderer(second);
    manager.updateAllDirtyRenderers();
    expect(updates).toEqual(['first', 'second', 'first']);
});
