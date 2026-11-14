"""Configuration: one dataclass tree, loaded from YAML (configs/*.yaml) with command-line overrides.

    cfg = load_config("configs/crn48k.yaml", ["train.lr=1e-3", "data.sample_rate=16000"])
"""
from __future__ import annotations

import dataclasses
from dataclasses import dataclass, field
from typing import Any, List

import yaml


@dataclass
class DataConfig:
    sample_rate: int = 48000            # 16000 or 48000
    segment_s: float = 2.0              # training segment length
    speech_dirs: List[str] = field(default_factory=list)   # folders searched recursively for wav/flac
    noise_dirs: List[str] = field(default_factory=list)
    rir_dirs: List[str] = field(default_factory=list)
    val_speech_dirs: List[str] = field(default_factory=list)   # held-out speakers (never in speech_dirs)
    val_noise_dirs: List[str] = field(default_factory=list)    # held-out noise (empty = synthetic only)
    real_dirs: List[str] = field(default_factory=list)         # real noisy recordings, no reference (DNSMOS only)
    speech_max_hz: float = 0.0          # band limit of the speech corpus (LibriSpeech: 8000); 0 = full band
    snr_min: float = -5.0
    snr_max: float = 20.0
    p_rir: float = 0.3                  # reverberant speech (the clean target keeps the early part, see dataset.py)
    p_synth_noise: float = 0.3          # synthetic noise families mixed in (always used if no noise_dirs)
    p_noise_only: float = 0.05          # segments without speech
    p_second_noise: float = 0.3
    level_min_db: float = -45.0         # active speech level (dBFS)
    level_max_db: float = -15.0
    p_mic_eq: float = 0.7               # random microphone colouration
    p_clip: float = 0.02
    dereverb: bool = False              # True: target = direct path + early reflections only
    num_workers: int = 8
    val_items: int = 64


@dataclass
class StftConfig:
    n_fft: int = 960                    # 20 ms at 48 kHz (320 at 16 kHz)
    hop: int = 480                      # 10 ms
    window: str = "vorbis"              # vorbis | hann-sqrt (both perfect-reconstruction at 50 % overlap)


@dataclass
class ModelConfig:
    name: str = "crn"                   # crn | nsnet | erbdf
    # nsnet (GRU mask over spectrum bins; the model mic thingy runs in C)
    hidden: int = 128
    bins: int = 0                       # bins the net sees/predicts, 0 = all (or up to speech_max_hz)
    hf_lo_hz: float = 5000.0            # bins above `bins` take the mean gain of hf_lo_hz..hf_hi_hz
    hf_hi_hz: float = 7000.0
    # crn (causal convolutional recurrent network on the complex spectrum)
    channels: List[int] = field(default_factory=lambda: [16, 32, 48, 64])
    freq_stride: int = 2
    rnn_hidden: int = 256
    rnn_layers: int = 2
    mask: str = "complex"               # complex | magnitude
    compress: float = 0.3               # power-law compression of the input magnitude
    # erbdf (ERB-band gains + deep filtering of the low bins; also uses hidden, compress, hf_*)
    bands: int = 32                     # ERB bands over the visible bins
    df_max_hz: float = 4800.0           # bins below this get the complex multi-frame filter
    df_order: int = 5                   # taps (current + past frames); 0 = band gains only
    conv_ch: int = 16


@dataclass
class LossConfig:
    w_mrstft: float = 1.0
    mrstft_ffts: List[int] = field(default_factory=lambda: [512, 1024, 2048])
    w_sisdr: float = 0.02               # SI-SDR is in dB, ~20x the size of the other terms
    w_spec: float = 1.0                 # compressed complex spectral loss on the model's own STFT
    spec_compress: float = 0.3
    w_vad: float = 0.1                  # nsnet, erbdf: speech-presence head
    band_limit: bool = True             # score only bins the training speech fills (speech_max_hz)


@dataclass
class TrainConfig:
    out_dir: str = "runs/default"
    steps: int = 50000
    batch: int = 16
    lr: float = 1e-3
    weight_decay: float = 1e-4
    warmup: float = 0.03
    grad_clip: float = 5.0
    amp: bool = True                    # CUDA: fp16 autocast + GradScaler; CPU: bf16 autocast if supported
    threads: int = 0                    # torch CPU threads, 0 = default
    log_every: int = 50
    val_every: int = 1000
    ckpt_every: int = 1000
    seed: int = 1
    device: str = "auto"                # auto | cuda | cpu


@dataclass
class Config:
    data: DataConfig = field(default_factory=DataConfig)
    stft: StftConfig = field(default_factory=StftConfig)
    model: ModelConfig = field(default_factory=ModelConfig)
    loss: LossConfig = field(default_factory=LossConfig)
    train: TrainConfig = field(default_factory=TrainConfig)


def _merge(dc: Any, d: dict) -> Any:
    for k, v in d.items():
        if not hasattr(dc, k):
            raise KeyError(f"unknown config key '{k}' in {type(dc).__name__}")
        cur = getattr(dc, k)
        if dataclasses.is_dataclass(cur):
            _merge(cur, v or {})
        else:
            setattr(dc, k, v)
    return dc


def _parse_value(s: str) -> Any:
    return yaml.safe_load(s)


def load_config(path: str | None = None, overrides: List[str] | None = None) -> Config:
    cfg = Config()
    if path:
        with open(path, encoding="utf-8") as f:
            _merge(cfg, yaml.safe_load(f) or {})
    for ov in overrides or []:
        key, _, val = ov.partition("=")
        node = cfg
        parts = key.split(".")
        for p in parts[:-1]:
            node = getattr(node, p)
        if not hasattr(node, parts[-1]):
            raise KeyError(f"unknown config key '{key}'")
        setattr(node, parts[-1], _parse_value(val))
    if cfg.data.sample_rate not in (16000, 48000):
        raise ValueError("data.sample_rate must be 16000 or 48000")
    return cfg


def to_dict(cfg: Config) -> dict:
    return dataclasses.asdict(cfg)
