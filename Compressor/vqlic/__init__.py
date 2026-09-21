"""Neural image codec (base_v3 lineage), packaged for reproducible runs."""

__version__ = "0.3.0"

from vqlic import presets
from vqlic.config import Config, build_argparser, config_from_args
from vqlic.encoder import ImageEncoder
from vqlic.quantizer import (
    CB_BIAS_FRAC_BITS_CAP, MultiCodebookEMAQuantizer, format_util,
)
from vqlic.decoder import (
    SelfAttentionDecoder, SynthesisHead, sinusoidal_pos_2d, window_partition,
    window_reverse,
)
from vqlic.codec import (
    HEADER_BYTES, MODE_NAMES, MODE_PRIOR, MODE_RAW, MODE_TABLE,
    NeuralImageCodec, build_model, pad_to_multiple, payload_info, token_grid,
    unpad,
)
from vqlic.entropy import (
    cdf_from_freqs, pack_uints, packed_size, quantize_freqs, rans_decode,
    rans_encode, unpack_uints,
)
from vqlic.losses import RateDistortionLoss
from vqlic.dataset import (
    ImageFolderDataset, RotatingSequentialSampler, build_trainloader,
    build_valloader, collate_fn,
)
from vqlic.metrics import (
    ms_ssim_db, model_size_table, prior_cross_entropy_bpp, psnr_uint8,
    shannon_bpp_multi, to_uint8,
)
from vqlic.checkpoint import (
    apply_qat_payload, load_checkpoint, load_qat_payload, qat_tables,
    save_checkpoint, save_qat_checkpoint,
)
from vqlic.plots import load_log, plot_log
from vqlic.engine import save_preview, train, validate
from vqlic.qat import (
    convert_int8_encoder, evaluate as evaluate_qat, freeze_codebook,
    freeze_quant_params, latent_quant_params, load_fp32_codec, prepare_encoder,
    reestimate_prior, train_qat, usage_stats, verify_int_search,
)
# The loaders every measurement goes through. `load_fp32` / `load_qat_int8` are
# the only place `rate_beta` is restored -- see vqlic/load.py.
from vqlic.load import CpuEncoderBridge, load_fp32, load_qat_int8, place
from vqlic.keepn import install_keep_mask, keep_topn, remove_keep_mask
from vqlic.context_fit import ContextCounter, truncate_to_top_n

__all__ = [
    "presets", "load_log", "plot_log", "save_preview",
    "Config", "build_argparser", "config_from_args",
    "ImageEncoder", "MultiCodebookEMAQuantizer", "format_util",
    "CB_BIAS_FRAC_BITS_CAP",
    "SelfAttentionDecoder", "SynthesisHead", "sinusoidal_pos_2d",
    "window_partition", "window_reverse",
    "NeuralImageCodec", "build_model", "pad_to_multiple", "unpad",
    "payload_info", "token_grid", "HEADER_BYTES", "MODE_NAMES", "MODE_RAW",
    "MODE_PRIOR", "MODE_TABLE",
    "cdf_from_freqs", "pack_uints", "packed_size", "quantize_freqs",
    "rans_decode", "rans_encode", "unpack_uints",
    "RateDistortionLoss",
    "ImageFolderDataset", "RotatingSequentialSampler", "build_trainloader",
    "build_valloader", "collate_fn",
    "ms_ssim_db", "model_size_table", "prior_cross_entropy_bpp", "psnr_uint8",
    "shannon_bpp_multi", "to_uint8",
    "load_checkpoint", "save_checkpoint", "train", "validate",
    "apply_qat_payload", "load_qat_payload", "qat_tables",
    "save_qat_checkpoint",
    "convert_int8_encoder", "evaluate_qat", "freeze_codebook",
    "freeze_quant_params", "latent_quant_params", "load_fp32_codec",
    "prepare_encoder", "reestimate_prior", "train_qat", "usage_stats",
    "verify_int_search",
    "CpuEncoderBridge", "load_fp32", "load_qat_int8", "place",
    "install_keep_mask", "keep_topn", "remove_keep_mask",
    "ContextCounter", "truncate_to_top_n",
]
