// Developer-only exporter. Requires Node.js and sharp; exported assets are committed.
// Run from the repository root: node tools/dev/export-application-icon.cjs
const fs = require('node:fs/promises');
const path = require('node:path');
const sharp = require('sharp');

async function main() {
    const directory = path.resolve(__dirname, '../../apps/desktop/resources/icons');
    const source = await fs.readFile(path.join(directory, 'orbelle.svg'));
    // Supersampling preserves the thin orbit and the petals at taskbar/title-bar sizes.
    const render = size => sharp(source, { density: 72 * size * 4 / 256 })
        .resize(size, size, { kernel: 'lanczos3' }).png().toBuffer();
    await fs.writeFile(path.join(directory, 'orbelle.png'), await render(512));

    const sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256];
    const frames = await Promise.all(sizes.map(render));
    const header = Buffer.alloc(6 + sizes.length * 16);
    header.writeUInt16LE(1, 2); // ICO, with PNG-compressed 32-bit RGBA frames.
    header.writeUInt16LE(sizes.length, 4);
    let offset = header.length;
    sizes.forEach((size, index) => {
        const entry = 6 + index * 16;
        header[entry] = header[entry + 1] = size === 256 ? 0 : size;
        header.writeUInt16LE(1, entry + 4);
        header.writeUInt16LE(32, entry + 6);
        header.writeUInt32LE(frames[index].length, entry + 8);
        header.writeUInt32LE(offset, entry + 12);
        offset += frames[index].length;
    });
    await fs.writeFile(path.join(directory, 'orbelle.ico'), Buffer.concat([header, ...frames]));
    console.log(`Exported 512px PNG and ICO frames: ${sizes.join(', ')}px`);
}

main().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
