"""Permanent capture identity shared by the scene, planting records and exports."""
import hashlib
import re
import uuid
from datetime import datetime, timezone


def ensure_capture(scene, source=None):
    if not scene.capture:
        now = datetime.now(timezone.utc)
        if source is not None:
            # Legacy files have no capture identity. Identical copies acquire the
            # same identity without inventing their historical capture date.
            with open(source, 'rb') as stream:
                identity = hashlib.file_digest(stream, 'sha256').hexdigest()[:32]
            name = 'capture_legacy_' + identity[:12]
        else:
            identity = uuid.uuid4().hex
            name = f'capture_{now:%Y%m%dT%H%M%SZ}_{identity[:8]}'
        scene.capture = dict(id=identity, name=name, created_utc=now.strftime('%Y-%m-%dT%H:%M:%SZ'))
    if not re.fullmatch(r'capture_[A-Za-z0-9_-]{1,112}', scene.capture['name']):
        raise ValueError('Invalid capture folder name')
    if not scene.capture.get('id'):
        raise ValueError('Missing capture ID')
    # Validate all bindings before changing any record.
    records = [r for b in scene for g in b.groups for r in g.tree_instances]
    if any(r.get('capture_id', scene.capture['id']) != scene.capture['id'] for r in records):
        raise ValueError('Tree records belong to a different capture')
    for record in records:
        record['capture_id'] = scene.capture['id']
    return scene.capture
