"""Model registry: build_model(cfg) -> EnhancerBase."""
from ..config import Config
from .crn import CRN
from .erbdf import ErbDf
from .nsnet import NSNet


def build_model(cfg: Config):
    m, d = cfg.model, cfg.data
    if m.name == "nsnet":
        return NSNet(cfg.stft.n_fft, d.sample_rate, d.speech_max_hz, m.hidden, m.bins, m.hf_lo_hz, m.hf_hi_hz)
    if m.name == "crn":
        return CRN(cfg.stft.n_fft, d.sample_rate, d.speech_max_hz, m.channels, m.freq_stride, m.rnn_hidden,
                   m.rnn_layers, m.mask, m.compress, m.hf_lo_hz, m.hf_hi_hz)
    if m.name == "erbdf":
        return ErbDf(cfg.stft.n_fft, d.sample_rate, d.speech_max_hz, m.hidden, m.bands, m.df_max_hz, m.df_order,
                     m.conv_ch, m.compress, m.hf_lo_hz, m.hf_hi_hz)
    raise ValueError(f"unknown model '{m.name}' (crn | nsnet | erbdf)")
