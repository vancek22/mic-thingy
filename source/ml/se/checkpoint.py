"""Checkpoints carry the full config, so inference/export need only the .pt file."""
from __future__ import annotations

import torch

from .config import Config, _merge, to_dict
from .models import build_model
from .stft import Stft


def save(path: str, model, cfg: Config, step: int, opt=None, sched=None, best: float | None = None) -> None:
    torch.save({"model": model.state_dict(), "config": to_dict(cfg), "step": step,
                "opt": opt.state_dict() if opt else None, "sched": sched.state_dict() if sched else None,
                "best": best}, path)


def load(path: str, device: str = "cpu"):
    """-> (model in eval mode, cfg, stft module, raw checkpoint dict)"""
    ck = torch.load(path, map_location=device, weights_only=False)
    cfg = _merge(Config(), ck["config"])
    model = build_model(cfg)
    model.load_state_dict(ck["model"])
    model.to(device).eval()
    stft = Stft(cfg.stft.n_fft, cfg.stft.hop, cfg.stft.window).to(device)
    return model, cfg, stft, ck
