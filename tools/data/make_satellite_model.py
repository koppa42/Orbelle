"""Generate Orbelle's self-contained, project-authored glTF display fixture."""
import base64
import json
from pathlib import Path
import struct


def main():
    # Unit cube, separate face vertices so the normals are unambiguous.
    faces = [([1, 0, 0], [(1,-1,-1),(1,1,-1),(1,1,1),(1,-1,1)]),
             ([-1, 0, 0], [(-1,1,-1),(-1,-1,-1),(-1,-1,1),(-1,1,1)]),
             ([0, 1, 0], [(-1,1,-1),(-1,1,1),(1,1,1),(1,1,-1)]),
             ([0, -1, 0], [(-1,-1,1),(-1,-1,-1),(1,-1,-1),(1,-1,1)]),
             ([0, 0, 1], [(-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1)]),
             ([0, 0, -1], [(1,-1,-1),(-1,-1,-1),(-1,1,-1),(1,1,-1)])]
    positions, normals, indices = [], [], []
    for normal, corners in faces:
        offset = len(positions) // 3
        positions.extend(v / 2 for corner in corners for v in corner)
        normals.extend(normal * 4)
        indices.extend(offset + k for k in (0, 1, 2, 0, 2, 3))
    pos = struct.pack('<72f', *positions)
    norm = struct.pack('<72f', *normals)
    ind = struct.pack('<36H', *indices)
    blob = pos + norm + ind
    document = {
        'asset': {'version': '2.0', 'generator': 'Orbelle make_satellite_model.py',
                  'copyright': 'Orbelle project-authored schematic model'},
        'extensionsUsed': ['KHR_materials_unlit'],
        'scene': 0, 'scenes': [{'nodes': [0, 1, 2, 3]}],
        'nodes': [{'mesh': 0, 'scale': [1.2, .9, 1.1]},
                  {'mesh': 1, 'translation': [-1.6, 0, 0], 'scale': [1.8, 1.1, .05]},
                  {'mesh': 1, 'translation': [1.6, 0, 0], 'scale': [1.8, 1.1, .05]},
                  {'mesh': 2, 'translation': [0, 0, .65], 'scale': [.8, .7, .2]}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1},
                                    'indices': 2, 'material': material}]} for material in range(3)],
        'materials': [{'pbrMetallicRoughness': {'baseColorFactor': colour,
                                               'metallicFactor': 0, 'roughnessFactor': 1},
                       'doubleSided': True, 'extensions': {'KHR_materials_unlit': {}}}
                      for colour in ([.91, .72, .30, 1], [.08, .27, .58, 1], [.9, .95, 1, 1])],
        'buffers': [{'byteLength': len(blob), 'uri': 'data:application/octet-stream;base64,' +
                    base64.b64encode(blob).decode('ascii')}],
        'bufferViews': [{'buffer': 0, 'byteOffset': offset, 'byteLength': length, 'target': target}
                        for offset, length, target in [(0,len(pos),34962),
                            (len(pos),len(norm),34962),(len(pos)+len(norm),len(ind),34963)]],
        'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 24, 'type': 'VEC3',
                       'min': [-.5,-.5,-.5], 'max': [.5,.5,.5]},
                      {'bufferView': 1, 'componentType': 5126, 'count': 24, 'type': 'VEC3'},
                      {'bufferView': 2, 'componentType': 5123, 'count': 36, 'type': 'SCALAR'}]}
    path = Path(__file__).resolve().parents[2] / 'data/samples/models/satellite_demo.gltf'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(path)


if __name__ == '__main__':
    main()
