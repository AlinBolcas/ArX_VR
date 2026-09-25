"""
World Labs World API client (Marble): whole 3D worlds as Gaussian splats.

A standalone copy of the one in Arvolve's ArX, so this repo runs on its own.

Official docs: https://docs.worldlabs.ai/api  (rendering notes: /api/rendering-spz)
Read them before changing endpoints or fields.

What the docs say, checked 24 Sep 2026:
  - Auth is the header 'WLT-Api-Key'. Base https://api.worldlabs.ai
  - POST /marble/v1/worlds:generate  -> an operation; GET /marble/v1/operations/{id} until done.
    About 5 minutes a world.
  - Prompts: text, image, multi-image (with azimuths), video; an image or video is a public
    'uri' or an uploaded 'media_asset' (POST /marble/v1/media-assets:prepare_upload, then PUT).
    'is_pano' defaults to auto: an equirectangular image is recognised as a panorama.
  - The finished world carries splats as SPZ at 100k, 500k and full_res, a collider GLB, the
    panorama, and semantics_metadata {metric_scale_factor, ground_plane_offset}.
    POST /marble/v1/worlds/{id}:export {"asset_type": "splats", "format": "ply"} gives a PLY.
  - Splats are in Marble's raw OpenCV frame (y down, z ahead). To metres and the ground:
        centre * metric_scale_factor, then y -= ground_plane_offset; sizes * metric_scale_factor
    then a half turn about x for a y up engine (Marble's own viewer does exactly that).
  - Credits: $1 buys 1250. Roughly 150-250 a draft world, about 1600 on marble-1.1,
    1600 to 3000 on marble-1.1-plus (bigger worlds).

The key is WORLD_LABS_API_KEY: from the environment, else a .env at the repo root (git
ignores it), else the file ARX_ENV_FILE names. It is never printed.
"""

import json
import os
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple, Union

import requests

BASE_URL = "https://api.worldlabs.ai"
REPO_ENV = Path(__file__).resolve().parents[3] / ".env"
KEY_NAMES = ("WORLD_LABS_API_KEY", "WORLDLABS_API_KEY", "WLT_API_KEY")


def _key_from_files() -> Optional[str]:
    for path in (REPO_ENV, Path(os.environ.get("ARX_ENV_FILE", "")).expanduser()):
        if not path.is_file():
            continue
        for line in path.read_text().splitlines():
            name, _, value = line.partition("=")
            if name.strip() in KEY_NAMES and value.strip():
                return value.split("#")[0].strip().strip('"\'')
    return None

Source = Union[str, Path]


class WorldLabsAPI:
    # Kept in step with ArX's copy, which is where Arvolve's model names are decided
    MODELS: Dict[str, Dict[str, Any]] = {
        "marble-1.0-draft": {"credits": "150-250", "notes": "fast draft, for trying prompts"},
        "marble-1.1": {"credits": "about 1600", "notes": "standard world"},
        "marble-1.1-plus": {"credits": "1600-3000", "notes": "bigger worlds, outdoor or large indoor"},
    }
    DEFAULT_MODEL = "marble-1.1"
    CREDITS_PER_DOLLAR = 1250

    def __init__(self, api_key: Optional[str] = None, timeout: int = 60):
        self.api_key = api_key or next((os.environ[k] for k in KEY_NAMES if os.environ.get(k)), None) or _key_from_files()
        if not self.api_key:
            raise RuntimeError("No World Labs key: set WORLD_LABS_API_KEY, or put it in a .env at the "
                               "repo root (from platform.worldlabs.ai).")
        self.timeout = timeout

    @classmethod
    def catalog(cls) -> Dict[str, Any]:
        return {"provider": "worldlabs", "default": cls.DEFAULT_MODEL, "models": cls.MODELS}

    # ------------------------------------------------------------ plumbing

    def _request(self, method: str, path: str, body: Optional[dict] = None) -> Dict[str, Any]:
        response = requests.request(method, BASE_URL + path, json=body, timeout=self.timeout,
                                    headers={"WLT-Api-Key": self.api_key, "Content-Type": "application/json"})
        if response.status_code >= 400:
            raise RuntimeError(f"World API {method} {path}: {response.status_code} {response.text[:400]}")
        return response.json() if response.content else {}

    def credits(self) -> Dict[str, Any]:
        return self._request("GET", "/marble/v1/credits")

    def upload(self, path: Source, kind: str = "image") -> str:
        """A local image or video into World Labs' storage; returns its media asset id."""
        path = Path(path)
        extension = path.suffix.lstrip(".").lower()
        prepared = self._request("POST", "/marble/v1/media-assets:prepare_upload",
                                 {"file_name": path.name, "kind": kind, "extension": extension})
        info = prepared["upload_info"]
        # Only the headers the signed URL was made with; anything more can break its signature
        headers = dict(info.get("required_headers") or {})
        with path.open("rb") as data:
            put = requests.request(info.get("upload_method", "PUT"), info["upload_url"], data=data,
                                   headers=headers, timeout=600)
        if put.status_code >= 400:
            raise RuntimeError(f"Upload of {path.name} failed: {put.status_code} {put.text[:300]}")
        asset = prepared["media_asset"]
        return asset.get("media_asset_id") or asset["id"]

    def _content(self, source: Source, kind: str) -> Dict[str, str]:
        text = str(source)
        if text.startswith(("http://", "https://")):
            return {"source": "uri", "uri": text}
        return {"source": "media_asset", "media_asset_id": self.upload(source, kind)}

    # ------------------------------------------------------------ worlds

    def generate(self, text: Optional[str] = None, image: Optional[Source] = None,
                 images: Optional[List[Tuple[float, Source]]] = None, video: Optional[Source] = None,
                 model: str = DEFAULT_MODEL, name: str = "arxVR world",
                 is_pano: Optional[bool] = None) -> Dict[str, Any]:
        """Starts a world. Give text alone, or an image, several (azimuth, image) pairs, or a
        video, each with optional text. Local files are uploaded first. Returns the operation."""
        if model not in self.MODELS:
            raise ValueError(f"Unknown Marble model {model!r}; known: {', '.join(self.MODELS)}")
        if image is not None:
            prompt = {"type": "image", "image_prompt": self._content(image, "image")}
            if is_pano is not None:
                prompt["is_pano"] = is_pano
        elif images:
            prompt = {"type": "multi-image", "multi_image_prompt": [
                {"azimuth": azimuth, "content": self._content(source, "image")} for azimuth, source in images]}
        elif video is not None:
            prompt = {"type": "video", "video_prompt": self._content(video, "video")}
        elif text:
            prompt = {"type": "text"}
        else:
            raise ValueError("A world needs text, an image, images or a video.")
        if text:
            prompt["text_prompt"] = text
        return self._request("POST", "/marble/v1/worlds:generate",
                             {"display_name": name, "model": model, "world_prompt": prompt})

    def operation(self, operation_id: str) -> Dict[str, Any]:
        return self._request("GET", f"/marble/v1/operations/{operation_id}")

    def wait(self, operation_id: str, poll: float = 10.0, timeout: float = 1800.0,
             on_progress=None) -> Dict[str, Any]:
        """Polls until the world is done; returns the world."""
        start = time.time()
        while True:
            op = self.operation(operation_id)
            if op.get("error"):
                raise RuntimeError(f"World generation failed: {op['error']}")
            if op.get("done"):
                return op.get("response") or self.world(op["metadata"]["world_id"])
            if on_progress:
                on_progress(((op.get("metadata") or {}).get("progress") or {}), time.time() - start)
            if time.time() - start > timeout:
                raise TimeoutError(f"World {operation_id} still not done after {timeout:.0f} s")
            time.sleep(poll)

    def world(self, world_id: str) -> Dict[str, Any]:
        data = self._request("GET", f"/marble/v1/worlds/{world_id}")
        return data.get("world", data)

    def export_ply(self, world_id: str) -> str:
        """The world's splats as a PLY, full precision; returns its download URL."""
        data = self._request("POST", f"/marble/v1/worlds/{world_id}:export",
                             {"asset_type": "splats", "format": "ply"})
        url = _first_url(data)
        if not url:
            raise RuntimeError(f"Export gave no URL: {json.dumps(data)[:400]}")
        return url

    @staticmethod
    def assets(world: Dict[str, Any]) -> Dict[str, Any]:
        """The parts of a finished world a renderer needs, flattened."""
        a = world.get("assets") or {}
        splats = a.get("splats") or {}
        meta = splats.get("semantics_metadata") or {}
        return {"spz": splats.get("spz_urls") or {}, "panorama": (a.get("imagery") or {}).get("pano_url"),
                "collider": (a.get("mesh") or {}).get("collider_mesh_url"), "thumbnail": a.get("thumbnail_url"),
                "caption": a.get("caption"), "metric_scale_factor": meta.get("metric_scale_factor"),
                "ground_plane_offset": meta.get("ground_plane_offset"), "marble_url": world.get("world_marble_url")}

    @staticmethod
    def download(url: str, path: Source) -> Path:
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        with requests.get(url, stream=True, timeout=600) as response:
            response.raise_for_status()
            with path.open("wb") as out:
                for chunk in response.iter_content(1 << 20):
                    out.write(chunk)
        return path


def _first_url(value: Any) -> Optional[str]:
    if isinstance(value, str):
        return value if value.startswith("https://") else None
    if isinstance(value, dict):
        value = list(value.values())
    if isinstance(value, list):
        for item in value:
            found = _first_url(item)
            if found:
                return found
    return None
