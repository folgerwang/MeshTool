"""Learned image inpainting (LaMa) for the terrain orthophoto: filled areas
continue the surface the way a photo editor's object removal does (lane
markings, kerbs, paving) instead of smearing the rim colour inward.

Weights: big-lama.pt (TorchScript, ~200 MB) in the MeshTool cache; downloaded
once from the simple-lama-inpainting release unless offline.
"""
import os
import time
import urllib.request

import numpy as np

CACHE = os.path.join(os.path.expanduser("~"), ".cache", "meshtool")
WEIGHTS = os.path.join(CACHE, "big-lama.pt")
URL = "https://github.com/enesmsahin/simple-lama-inpainting/releases/download/v0.1.0/big-lama.pt"
TILE = 1024          # px; the model is fully convolutional, tiles overlap by MARGIN
MARGIN = 128


class LamaInpainter:
    def __init__(self, log=print):
        import torch
        self.torch = torch
        if not os.path.exists(WEIGHTS):
            if os.environ.get("HF_HUB_OFFLINE") == "1" and os.environ.get("MESHTOOL_DOWNLOAD") != "1":
                raise RuntimeError(f"{WEIGHTS} missing (offline)")
            os.makedirs(CACHE, exist_ok=True)
            log(f"  downloading LaMa weights to {WEIGHTS} ...")
            urllib.request.urlretrieve(URL, WEIGHTS + ".part")
            os.replace(WEIGHTS + ".part", WEIGHTS)
        self.device = "cuda" if torch.cuda.is_available() else "cpu"
        self.model = torch.jit.load(WEIGHTS, map_location=self.device).eval()

    def _run(self, rgb, mask):
        torch = self.torch
        h, w = rgb.shape[:2]
        ph, pw = (8 - h % 8) % 8, (8 - w % 8) % 8
        x = torch.from_numpy(np.pad(rgb, ((0, ph), (0, pw), (0, 0)), mode="reflect")) \
            .permute(2, 0, 1)[None].float().div(255).to(self.device)
        m = torch.from_numpy(np.pad(mask.astype(np.float32), ((0, ph), (0, pw)), mode="reflect"))[None, None].to(self.device)
        with torch.inference_mode():
            out = self.model(x, (m > 0).float())
        return out[0].permute(1, 2, 0).clamp(0, 1).mul(255).byte().cpu().numpy()[:h, :w]

    def fill(self, rgb, mask, log=print):
        """rgb uint8[H, W, 3], mask bool[H, W] -> uint8[H, W, 3] with mask filled."""
        t0 = time.time()
        H, W = mask.shape
        out = rgb.copy()
        step = TILE - 2 * MARGIN
        for y0 in range(0, H, step):
            for x0 in range(0, W, step):
                ya, xa = max(0, y0 - MARGIN), max(0, x0 - MARGIN)
                yb, xb = min(H, y0 + step + MARGIN), min(W, x0 + step + MARGIN)
                sub = mask[ya:yb, xa:xb]
                if not sub.any():
                    continue
                res = self._run(np.ascontiguousarray(rgb[ya:yb, xa:xb]), sub)
                # Keep only this tile's own (non-margin) area.
                iy0, ix0 = y0 - ya, x0 - xa
                iy1, ix1 = min(yb, y0 + step) - ya, min(xb, x0 + step) - xa
                sel = sub[iy0:iy1, ix0:ix1]
                tgt = out[y0:y0 + (iy1 - iy0), x0:x0 + (ix1 - ix0)]
                tgt[sel] = res[iy0:iy1, ix0:ix1][sel]
        log(f"  inpainted {mask.sum() * 1e-3:.0f}k px with LaMa on {self.device} in {time.time() - t0:.1f} s")
        return out
