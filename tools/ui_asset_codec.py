"""Lossless helpers for generated UI assets; no rasterization dependencies."""

def crop_glyph(pixels, width, height):
    """Return tightly bounded pixels, dimensions and LVGL baseline offsets.

    Call after bpp quantization: every nonzero alpha is retained. The advance
    and font line height remain the caller's original values.
    """
    assert len(pixels) == width * height
    nonzero = [i for i, p in enumerate(pixels) if p]
    if not nonzero:
        return [], 0, 0, 0, 0
    left = min(i % width for i in nonzero)
    right = max(i % width for i in nonzero) + 1
    top = min(i // width for i in nonzero)
    bottom = max(i // width for i in nonzero) + 1
    cropped = [pixels[y * width + x] for y in range(top, bottom) for x in range(left, right)]
    return cropped, right - left, bottom - top, left, height - bottom

def packbits(data):
    data = bytes(data)
    out = bytearray()
    i = 0
    while i < len(data):
        run = 1
        while i + run < len(data) and run < 128 and data[i + run] == data[i]:
            run += 1
        if run >= 3:
            out.extend((257 - run, data[i]))
            i += run
            continue
        start = i
        i += run
        while i < len(data) and i - start < 128:
            run = 1
            while i + run < len(data) and run < 128 and data[i + run] == data[i]:
                run += 1
            if run >= 3:
                break
            i += min(run, 128 - (i - start))
        out.append(i - start - 1)
        out.extend(data[start:i])
    return bytes(out)

def byte_array(name, data, linkage='static const'):
    return f'{linkage} uint8_t {name}[] = {{\n' + '\n'.join(
        '    ' + ','.join(str(v) for v in data[i:i+24]) + ','
        for i in range(0, len(data), 24)) + '\n};\n'

def emit_frames(name, frames, linkage='static const'):
    assert frames and frames[0] and all(len(f) == len(frames[0]) for f in frames)
    packed = bytearray()
    offsets = [0]
    for frame in frames:
        packed.extend(packbits(frame))
        offsets.append(len(packed))
    return (byte_array(name + '_data', packed) +
            f'static const uint32_t {name}_offsets[] = {{' + ','.join(map(str, offsets)) + '};\n' +
            f'{linkage} nuwax_asset_rle_t {name} = {{.data={name}_data, .offsets={name}_offsets, '
            f'.data_size=sizeof({name}_data), .frame_count={len(frames)}, .frame_bytes={len(frames[0])}}};\n')
