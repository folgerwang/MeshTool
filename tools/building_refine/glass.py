"""Glass facade detection: CLIP zero-shot on cells of baked wall textures."""
import os

import cv2
import numpy as np

CELL_M = 3.0          # cell edge (m)
CONTEXT_M = 9.0       # the classifier sees this much facade around each cell
THRESHOLD = 0.30      # p(glass) above this -> glass (curtain walls score 0.3-0.9, brick < 0.1)
FACADE_SHARE = 0.5    # and at least this share of the wall's cells must be glass
INTERIOR_DEPTH = 1.5  # dark backing behind glass (m), so it never shows an empty shell

GLASS_PROMPTS = [
    "a glass curtain wall of an office tower",
    "a facade made of reflective glass panels",
    "floor-to-ceiling glass windows covering a building facade",
    "a modern glass skyscraper exterior",
]
OTHER_PROMPTS = [
    "a concrete facade with small windows",
    "a brick building facade with windows",
    "a stone building facade",
    "a plain painted wall",
    "an apartment facade with balconies",
    "a blank concrete wall",
    "a blurry grey surface",
    "a red brick loft building with large windows",
    "a brick facade with a grid of windows",
    "a building facade with punched windows in a masonry wall",
]


class GlassClassifier:
    def __init__(self, device=None):
        os.environ.setdefault("HF_HUB_OFFLINE", "1")
        import torch
        from transformers import CLIPModel, CLIPTokenizer
        self.torch = torch
        self.device = device or ("cuda" if torch.cuda.is_available() else "cpu")
        dtype = torch.float16 if self.device == "cuda" else torch.float32
        self.model = CLIPModel.from_pretrained("openai/clip-vit-large-patch14", torch_dtype=dtype).to(self.device).eval()
        tok = CLIPTokenizer.from_pretrained("openai/clip-vit-large-patch14")
        with torch.inference_mode():
            t = tok(GLASS_PROMPTS + OTHER_PROMPTS, return_tensors="pt", padding=True).to(self.device)
            e = _embedding(self.model.get_text_features(**t))
            self.text = e / e.norm(dim=-1, keepdim=True)
        self.n_glass = len(GLASS_PROMPTS)

    def p_glass(self, images):
        """images: list of uint8 RGB arrays -> p(glass) per image."""
        if not images:
            return np.zeros(0)
        torch = self.torch
        out = []
        with torch.inference_mode():
            for i in range(0, len(images), 64):
                x = torch.from_numpy(np.stack([_preprocess(im) for im in images[i:i + 64]]))
                e = _embedding(self.model.get_image_features(pixel_values=x.to(self.device, self.model.dtype)))
                e = e / e.norm(dim=-1, keepdim=True)
                p = (100.0 * e @ self.text.T).float().softmax(-1)
                out.append(p[:, :self.n_glass].sum(-1).cpu().numpy())
        return np.concatenate(out)


def _embedding(out):
    """Projected CLIP embedding (transformers 5 wraps it in an output object)."""
    return out if hasattr(out, "norm") else out.pooler_output


_MEAN = np.array([0.48145466, 0.4578275, 0.40821073], np.float32)
_STD = np.array([0.26862954, 0.26130258, 0.27577711], np.float32)


def _preprocess(img):
    """CLIP's own preprocessing: shortest side to 224 (bicubic), centre crop,
    normalize. Returns float32[3, 224, 224]."""
    h, w = img.shape[:2]
    s = 224.0 / min(h, w)
    img = cv2.resize(img, (max(224, round(w * s)), max(224, round(h * s))), interpolation=cv2.INTER_CUBIC)
    h, w = img.shape[:2]
    y, x = (h - 224) // 2, (w - 224) // 2
    img = img[y:y + 224, x:x + 224].astype(np.float32) / 255.0
    return ((img - _MEAN) / _STD).transpose(2, 0, 1)


def cell_grid(width_m, height_m):
    """Cell edges along a face: (u edges, v edges) in metres."""
    nu = max(1, int(round(width_m / CELL_M)))
    nv = max(1, int(round(height_m / CELL_M)))
    return np.linspace(0, width_m, nu + 1), np.linspace(0, height_m, nv + 1)


def context_crops(img, texel, ue, ve):
    """One crop per cell (row-major, v from the bottom), CONTEXT_M wide."""
    h, w = img.shape[:2]
    half = CONTEXT_M / 2 / texel
    crops = []
    for j in range(len(ve) - 1):
        for i in range(len(ue) - 1):
            cx = (ue[i] + ue[i + 1]) / 2 / texel
            cy = h - (ve[j] + ve[j + 1]) / 2 / texel
            x0, x1 = int(max(0, cx - half)), int(min(w, cx + half))
            y0, y1 = int(max(0, cy - half)), int(min(h, cy + half))
            c = img[y0:y1, x0:x1]
            if c.size == 0:
                c = img
            crops.append(np.ascontiguousarray(c))
    return crops


def smooth(decide, nu, nv):
    """Majority of each cell's 3x3 neighbourhood: no salt-and-pepper glass."""
    g = decide.reshape(nv, nu).astype(np.float32)
    k = cv2.blur(g, (3, 3), borderType=cv2.BORDER_REPLICATE)
    return (k > 0.5).ravel()
