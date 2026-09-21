"""Run configuration and CLI."""
from __future__ import annotations

import argparse
import dataclasses
import json
import math
import os
import sys
from dataclasses import dataclass, fields
from types import SimpleNamespace

from vqlic import presets

_notes_seen = set()


def _note(msg):
    """Print a configuration note once per process.

    Config is instantiated more than once per run -- `build_argparser` builds a
    throwaway instance to read its defaults from, and `Config.from_dict` rebuilds
    one when a checkpoint is loaded. Every note here describes a static property of
    the settings, so printing it twice is noise that trains you to ignore it.
    """
    if msg not in _notes_seen:
        _notes_seen.add(msg)
        print(msg)


@dataclass
class Config:
    # ------------------------------------------------------------------ run id
    # Every artifact this run writes is prefixed with `name`, and out_dir defaults
    # to runs/<name>, so several rungs of the bitrate ladder can share a volume
    # without a single ambiguous file between them. See `Config.file`.
    name: str = "nic"
    preset: str = ""               # provenance only; see vqlic/presets.py

    # ----------------------------------------------------------------- data
    train_dir: str = ""
    val_dir: str = ""
    image_size: int = 224
    batch_size: int = 32
    num_workers: int = 8
    shuffle: bool = False
    val_resize_short: int = 0      # 0 = plain CenterCrop
    # Comma-separated crop sizes sampled per BATCH, e.g. "112,224,336,448".
    # Empty = fixed at image_size.
    multiscale: str = ""
    # Cache of the training directory listing. See dataset.ImageFolderDataset --
    # at 1.74M files, re-listing on every restart is minutes of startup.
    file_list: str = ""
    limit_images: int = 0          # 0 = the whole folder
    # Drop base_v3's RandomResizedCrop half of the augmentation, so every training
    # pixel is at its original scale. See dataset.train_transform.
    crop_only: bool = False

    # ---------------------------------------------------------------- model
    embedding_dim: int = 64        # 4 codebooks x 16 dims
    latent_dim: int = 256
    num_codebooks: int = 4
    codebook_size: int = 256
    nhead: int = 8
    num_layers: int = 6
    window_size: int = 14          # attention window in TOKENS; 0 = global

    # ------------------------------------------------------------ quantizer
    commitment_cost: float = 0.25
    decay: float = 0.99
    dead_thresh: float = 1.0
    revive_size: float = 10.0
    revive_pool_frac: float = 0.10
    rate_beta_norm: bool = False
    rate_beta_final: float = 0.3
    rate_beta_warmup: int = 4000   # steps to ramp rate_beta 0 -> final

    # ----------------------------------------------------------------- loss
    lambda_rate: float = 0.1
    beta_commit: float = 1.0
    alpha: float = 0.84            # weight on (1 - SSIM) vs L1
    ms_ssim_loss: bool = False     # use MS-SSIM instead of single-scale SSIM

    # ---------------------------------------------------------------- optim
    lr: float = 1e-4
    weight_decay: float = 1e-4
    eta_min: float = 1e-6
    grad_clip: float = 1.0
    epochs: int = 1
    max_steps: int = 0             # 0 -> epochs * len(loader)
    lr_warmup: int = 0
    amp: bool = True

    # ------------------------------------------------------------------ run
    out_dir: str = ""              # "" -> runs/<name>
    resume: str = ""               # checkpoint path, or "auto" for <name>_last.pt
    log_every: int = 50
    ckpt_every: int = 2000         # ~80 MB per save; every 1000 steps is disk churn
    val_every: int = 0             # 0 = only at epoch end
    val_batches: int = 8
    vis_every: int = 10000         # reconstruction PNG every N batches (0 = off)
    vis_images: int = 4            # images per preview PNG
    plot_every: int = 10000        # re-render <name>_curves.png every N batches
    seed: int = 0
    device: str = "cuda"
    save_full_model: bool = False
    collapse_warn: float = 0.20

    # ------------------------------------------------------------------ QAT
    # Used only by `vqlic.qat` / `qat.py`. Added to THIS dataclass rather than a
    # separate one so that `to_dict`/`from_dict`, `config.json` and the
    # checkpoint round trip keep working unchanged -- and every field has a
    # default, so `from_dict` still rebuilds the four archived rungs' configs
    # (which predate these names) and `state_dict` is untouched. The CLI flags
    # live in `build_qat_argparser`, not `build_argparser`, so `train.py --help`
    # does not grow options it cannot act on.
    qat_from: str = ""             # fp32 checkpoint to start QAT from
    qat_resume: str = ""           # a previous *_qat_resume_*.pt, or "auto"
    qat_backend: str = "fbgemm"
    qat_max_images: int = 2_000_000   # images-seen budget; 0 -> epochs * loader
    qat_ckpt_every_images: int = 50_000
    # Fraction of the budget after which the fake-quant observers and the BN
    # running stats are frozen. The int32 assignment bias is a function of the
    # encoder's output scale and zero point, so those have to stop moving before
    # it can be computed -- see quantizer.set_latent_quant.
    qat_freeze_observer_frac: float = 0.90
    qat_codebook_bits: int = 8
    qat_quantize_codebook: bool = True
    qat_train_decoder: bool = True
    qat_prior_images: int = 20_000    # 0 = keep the fp32 run's frozen prior
    qat_prior_floor: float = 1.0
    qat_eval_dir: str = ""            # full-resolution eval corpus (env NIC_EVAL_DIR)
    qat_eval_images: int = 16
    qat_eval_decoder_device: str = "cuda"

    def __post_init__(self):
        bad = set(self.name) & set('/\\:*?"<>| \t')
        if not self.name or bad:
            raise ValueError(
                f"--name must be a non-empty filename fragment; {self.name!r} "
                f"contains {sorted(bad)}")
        if not self.out_dir:
            self.out_dir = os.path.join("runs", self.name)

        if self.embedding_dim % self.num_codebooks:
            raise ValueError(
                f"embedding_dim {self.embedding_dim} must be divisible by "
                f"num_codebooks {self.num_codebooks}")
        if self.embedding_dim % 4:
            raise ValueError(
                f"embedding_dim {self.embedding_dim} must be divisible by 4 "
                f"(the encoder's first block is embedding_dim // 4 wide)")
        if self.latent_dim % 8:
            raise ValueError(
                f"latent_dim {self.latent_dim} must be divisible by 8; the "
                f"synthesis ladder is latent_dim//2 -> //4 -> //8")
        if self.latent_dim % 4:
            raise ValueError(
                f"latent_dim {self.latent_dim} must be divisible by 4 (2D "
                f"sinusoidal position encoding splits it into 4 bands)")
        if self.latent_dim % self.nhead:
            raise ValueError(
                f"latent_dim {self.latent_dim} must be divisible by nhead "
                f"{self.nhead}")
        if self.window_size < 0:
            raise ValueError("--window-size must be >= 0 (0 = global attention)")
        if self.revive_size <= self.dead_thresh:
            raise ValueError(
                f"revive_size ({self.revive_size}) must exceed dead_thresh "
                f"({self.dead_thresh}), otherwise a revived code is dead again on "
                f"the next EMA step -- that is the bug this fixes")

        grid = self.image_size // 8
        if self.window_size and grid % self.window_size:
            _note(f"note: token grid {grid} is not a multiple of --window-size "
                  f"{self.window_size}; the last window row/column is padded and "
                  f"masked (correct, slightly wasteful)")
        for s in self.scale_sizes():
            if self.window_size and (s // 8) % self.window_size:
                _note(f"note: --multiscale size {s} gives a {s // 8} token grid, "
                      f"not a multiple of --window-size {self.window_size}")

        # --- revive_size scales with tokens per batch -------------------------
        # cluster_size is an EMA of per-batch hit COUNTS, so its steady state is
        # (tokens per batch) / K -- it grows with batch size. revive_size is an
        # absolute mass, so the same 10.0 that was 20% of a typical live code's
        # mass at batch 16 is only 2.5% at batch 128: the revived code still gets
        # its ~230 steps before decaying under dead_thresh, but it competes
        # against live codes carrying 8x the prior, and with rate_beta > 0 that
        # -beta*log p handicap is what kills it. Scale the grant with the batch or
        # the cold-start fix quietly weakens.
        suggested = 0.2 * self.batch_size * grid ** 2 / self.codebook_size
        if suggested > 2.5 * self.revive_size:
            _note(f"note: --revive-size {self.revive_size:g} is small for batch "
                  f"{self.batch_size} at K={self.codebook_size}; a typical live "
                  f"code will sit near {5 * suggested:.0f}. Consider "
                  f"--revive-size {suggested:.0f} (the default 10 is this same "
                  f"rule at batch 16, 224px, K=256).")

        if not 0.0 <= self.qat_freeze_observer_frac <= 1.0:
            raise ValueError(
                f"--qat-freeze-observer-frac must be in [0, 1], got "
                f"{self.qat_freeze_observer_frac}")
        if not 2 <= self.qat_codebook_bits <= 8:
            raise ValueError(
                f"--qat-codebook-bits must be in [2, 8], got "
                f"{self.qat_codebook_bits}")

        if self.max_steps and self.vis_every and self.max_steps < 5 * self.vis_every:
            _note(f"note: --max-steps {self.max_steps:,} gives only "
                  f"{self.max_steps // self.vis_every} preview(s) at --vis-every "
                  f"{self.vis_every:,}. Previews count BATCHES, so a large "
                  f"--batch-size needs a smaller --vis-every.")

    def scale_sizes(self):
        """Crop sizes for multi-resolution training; empty list when disabled."""
        if not self.multiscale:
            return []
        sizes = sorted({int(v) for v in
                        str(self.multiscale).replace(" ", "").split(",") if v})
        bad = [v for v in sizes if v % 8]
        if bad:
            raise ValueError(
                f"--multiscale sizes must be divisible by 8 (the encoder is /8); "
                f"offenders: {bad}")
        return sizes

    def file(self, suffix, subdir=""):
        """`<out_dir>/[subdir/]<name>_<suffix>` -- every artifact carries the name.

        Prefixing rather than relying on out_dir alone means a stray
        `--out-dir runs/shared` cannot have two rungs of the ladder overwriting
        each other's `last.pt`, and a downloaded file is self-identifying.
        """
        d = os.path.join(self.out_dir, subdir) if subdir else self.out_dir
        return os.path.join(d, f"{self.name}_{suffix}")

    def bits_per_token_ceiling(self):
        """Hard rate ceiling of this codebook shape, in bits per latent token."""
        return self.num_codebooks * math.log2(self.codebook_size)

    def to_dict(self):
        return dataclasses.asdict(self)

    def save(self, path):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self.to_dict(), f, indent=2, sort_keys=True)

    @classmethod
    def from_dict(cls, d):
        known = {f.name for f in fields(cls)}
        return cls(**{k: v for k, v in d.items() if k in known})

    def summary(self):
        bpt = self.bits_per_token_ceiling()
        head = [f"configuration  [{self.name}]"
                + (f"  preset {self.preset}" if self.preset else ""),
                f"  rate ceiling  {self.num_codebooks} x log2({self.codebook_size})"
                f" = {bpt:.0f} bits/token = {bpt / 64:.4f} bpp",
                f"  artifacts     {self.file('*')}"]
        return "\n".join(head + [""]
                         + [f"  {f.name:<20} {getattr(self, f.name)}"
                            for f in fields(self)])


def _peek(argv, flag, default=None):
    """Read one flag before the real parser exists.

    Needed for --preset: a preset has to change the DEFAULTS of other flags so
    that anything given explicitly on the command line still wins. That ordering
    is only possible if the preset is known before `parse_args` runs.
    """
    q = argparse.ArgumentParser(add_help=False)
    q.add_argument(flag, default=default)
    known, _ = q.parse_known_args(list(sys.argv[1:] if argv is None else argv))
    return getattr(known, flag.lstrip("-").replace("-", "_"))


def build_argparser(argv=None):
    p = argparse.ArgumentParser(
        prog="train.py",
        description="Train the neural image codec.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        epilog="Run with --print-config to see the resolved configuration without "
               "touching any data, or --list-presets for the bitrate ladder.",
    )
    # The DECLARED field defaults, not a constructed Config. Instantiating one here
    # would run __post_init__, which (a) resolves out_dir against the default name,
    # pinning every run to runs/nic, and (b) prints its advisory notes for the
    # default codebook shape -- notes about K=256 while the user asked for bpp020.
    d = SimpleNamespace(**{f.name: f.default for f in fields(Config)})

    g = p.add_argument_group("run identity")
    g.add_argument("--name", default=d.name,
                   help="run name; prefixes every checkpoint, log and preview, and "
                        "sets --out-dir to runs/<name>. Defaults to the preset name "
                        "when --preset is given")
    g.add_argument("--preset", default="", metavar="RUNG",
                   help=f"bitrate rung: {', '.join(presets.PRESETS)}. Sets "
                        f"--num-codebooks/--codebook-size/--rate-beta-final; "
                        f"explicit flags still override it")
    g.add_argument("--list-presets", action="store_true",
                   help="print the bitrate ladder, then exit")

    g = p.add_argument_group("data")
    g.add_argument("--train-dir", default=os.environ.get("NIC_TRAIN_DIR", d.train_dir),
                   help="folder of training images (env: NIC_TRAIN_DIR)")
    g.add_argument("--val-dir", default=os.environ.get("NIC_VAL_DIR", d.val_dir),
                   help="folder of validation images (env: NIC_VAL_DIR)")
    g.add_argument("--image-size", type=int, default=d.image_size)
    g.add_argument("--batch-size", type=int, default=d.batch_size)
    g.add_argument("--num-workers", type=int, default=d.num_workers,
                   help="8 suits a cloud box; use 0 on Windows or in a notebook. "
                        "This is usually the throughput bottleneck -- see "
                        "scripts/preflight.py")
    g.add_argument("--shuffle", action="store_true", default=d.shuffle)
    g.add_argument("--val-resize-short", type=int, default=d.val_resize_short,
                   help="0 = plain CenterCrop")
    g.add_argument("--multiscale", default=d.multiscale,
                   help="comma-separated crop sizes sampled per batch, e.g. "
                        "112,224,336,448 -- prefer sizes whose /8 token grid is a "
                        "multiple of --window-size; empty = fixed at --image-size")
    g.add_argument("--file-list", default=d.file_list, metavar="PATH",
                   help="cache the training directory listing here; written on "
                        "first use, read on every later start (saves minutes of "
                        "os.listdir on a 1.74M-file folder)")
    g.add_argument("--limit-images", type=int, default=d.limit_images,
                   help="use only the first N images of the folder (0 = all)")
    g.add_argument("--crop-only", action=argparse.BooleanOptionalAction,
                   default=d.crop_only,
                   help="random-crop only, no RandomResizedCrop. base_v3 resampled "
                        "half of all samples, and RandomResizedCrop can take 8%% of "
                        "the image area and UPSAMPLE it to --image-size, which "
                        "teaches a codec that detail-free content is normal")

    g = p.add_argument_group("model")
    g.add_argument("--embedding-dim", type=int, default=d.embedding_dim)
    g.add_argument("--latent-dim", type=int, default=d.latent_dim)
    g.add_argument("--num-codebooks", type=int, default=d.num_codebooks)
    g.add_argument("--codebook-size", type=int, default=d.codebook_size)
    g.add_argument("--nhead", type=int, default=d.nhead)
    g.add_argument("--num-layers", type=int, default=d.num_layers)
    g.add_argument("--window-size", type=int, default=d.window_size,
                   help="attention window in TOKENS; 0 = global attention, which "
                        "does not generalize across resolution")

    g = p.add_argument_group("quantizer")
    g.add_argument("--commitment-cost", type=float, default=d.commitment_cost)
    g.add_argument("--decay", type=float, default=d.decay)
    g.add_argument("--dead-thresh", type=float, default=d.dead_thresh)
    g.add_argument("--revive-size", type=float, default=d.revive_size,
                   help="EMA mass granted to a revived code; must exceed "
                        "--dead-thresh")
    g.add_argument("--revive-pool-frac", type=float, default=d.revive_pool_frac)
    g.add_argument("--rate-beta-norm", action="store_true", default=d.rate_beta_norm,
                   help="make rate_beta scale-free (divide by mean NN error)")
    g.add_argument("--rate-beta-final", type=float, default=d.rate_beta_final)
    g.add_argument("--rate-beta-warmup", type=int, default=d.rate_beta_warmup)

    g = p.add_argument_group("loss")
    g.add_argument("--lambda-rate", type=float, default=d.lambda_rate)
    g.add_argument("--beta-commit", type=float, default=d.beta_commit)
    g.add_argument("--alpha", type=float, default=d.alpha,
                   help="weight on (1-SSIM) vs L1")
    g.add_argument("--ms-ssim-loss", action="store_true", default=d.ms_ssim_loss,
                   help="use MS-SSIM instead of single-scale SSIM")

    g = p.add_argument_group("optimization")
    g.add_argument("--lr", type=float, default=d.lr)
    g.add_argument("--weight-decay", type=float, default=d.weight_decay)
    g.add_argument("--eta-min", type=float, default=d.eta_min)
    g.add_argument("--grad-clip", type=float, default=d.grad_clip)
    g.add_argument("--epochs", type=int, default=d.epochs)
    g.add_argument("--max-steps", type=int, default=d.max_steps,
                   help="0 = epochs * len(loader)")
    g.add_argument("--lr-warmup", type=int, default=d.lr_warmup)
    # BooleanOptionalAction, not store_true: amp defaults to ON, and a store_true
    # flag whose default is True can never be turned off from the command line.
    # This gives both --amp and --no-amp. Worth having -- preflight.py reports
    # cases where fp32 is the faster of the two for a given shape.
    g.add_argument("--amp", action=argparse.BooleanOptionalAction, default=d.amp,
                   help="mixed precision on cuda")

    g = p.add_argument_group("run")
    # Empty, NOT d.out_dir: the sample Config above has already resolved its own
    # out_dir to runs/nic, and using that as the default would pin every run to
    # the default name's directory. "" lets __post_init__ derive it from --name.
    g.add_argument("--out-dir", default="",
                   help="default: runs/<name>")
    g.add_argument("--resume", default=d.resume,
                   help="checkpoint path, or 'auto' for <out-dir>/<name>_last.pt")
    g.add_argument("--log-every", type=int, default=d.log_every)
    g.add_argument("--ckpt-every", type=int, default=d.ckpt_every)
    g.add_argument("--val-every", type=int, default=d.val_every)
    g.add_argument("--val-batches", type=int, default=d.val_batches)
    g.add_argument("--vis-every", type=int, default=d.vis_every,
                   help="write a reconstruction preview PNG every N batches (0=off)")
    g.add_argument("--vis-images", type=int, default=d.vis_images,
                   help="images per preview PNG; capped by --batch-size, since the "
                        "preview comes from one batch")
    g.add_argument("--plot-every", type=int, default=d.plot_every,
                   help="re-render <name>_curves.png every N batches (0=off)")
    g.add_argument("--seed", type=int, default=d.seed)
    g.add_argument("--device", default=d.device)
    g.add_argument("--collapse-warn", type=float, default=d.collapse_warn)
    g.add_argument("--save-full-model", action="store_true",
                   default=d.save_full_model,
                   help="also torch.save(model) alongside the state_dict")
    g.add_argument("--print-config", action="store_true",
                   help="print the resolved config, then exit")

    # A preset is applied as DEFAULTS, after every add_argument, so that an
    # explicit flag on the same command line still overrides it. The run name
    # follows the preset unless --name says otherwise.
    rung = _peek(argv, "--preset", "")
    if rung:
        p.set_defaults(name=rung, **presets.apply(rung))

    return p


def config_from_args(args):
    known = {f.name for f in fields(Config)}
    return Config(**{k: v for k, v in vars(args).items() if k in known})


# Fields that describe the SHAPE of a trained model, or the rule its codebook was
# fitted under. QAT continues an existing checkpoint, so these come from that
# checkpoint and never from the command line: `build_model` has to reproduce the
# exact module tree `load_state_dict(strict=True)` expects, and `rate_beta_final`
# in particular is a plain attribute absent from the state_dict, so getting it
# from anywhere else silently changes the assignment rule the codebook and the
# decoder were trained under. See `vqlic.qat.config_for_qat`.
CHECKPOINT_SHAPE_FIELDS = (
    "embedding_dim", "latent_dim", "num_codebooks", "codebook_size", "nhead",
    "num_layers", "window_size", "commitment_cost", "decay", "dead_thresh",
    "revive_size", "revive_pool_frac", "rate_beta_norm", "rate_beta_final",
    "rate_beta_warmup",
)


def build_qat_argparser(argv=None):
    """`build_argparser` plus the QAT group, and QAT-appropriate defaults.

    A wrapper rather than an extension of `build_argparser` so that `train.py`
    keeps exactly the flags it had -- QAT options on the fp32 trainer's `--help`
    would be options it silently ignores.

    The defaults it overrides are the ones where the fp32 trainer's answer is
    wrong for QAT rather than merely different:

    * `--image-size 448 --crop-only --batch-size 8`: the point of the crop-only
      448 recipe is that the decoder's 14-token attention windows see a 4x4 grid
      instead of 224's 2x2, at native pixel scale, which is the regime full-res
      CLIC and Kodak actually exercise. 448 is 4x the pixels, hence the batch.
    * `--no-amp`: fake-quant observers plus GradScaler is a fragile combination,
      and the QAT step is dominated by the decoder anyway.
    * `--vis-every 0 --ckpt-every 0`: QAT checkpoints on an images-seen cadence
      (`--qat-ckpt-every-images`), so the step-based one would only duplicate.
    """
    p = build_argparser(argv)
    p.prog = "qat.py"
    p.description = ("INT8 quantization-aware training of the edge encoder, "
                     "against a frozen INT8-quantized codebook.")
    d = SimpleNamespace(**{f.name: f.default for f in fields(Config)})

    g = p.add_argument_group("QAT")
    g.add_argument("--qat-from", default=d.qat_from, metavar="CKPT",
                   help="fp32 checkpoint to start from (required); its config "
                        "supplies the model shape and rate_beta_final")
    g.add_argument("--qat-resume", default=d.qat_resume, metavar="CKPT",
                   help="resume a previous QAT run from a *_qat_resume_*.pt, or "
                        "'auto' for <out-dir>/<name>_qat_last.pt")
    g.add_argument("--qat-backend", default=d.qat_backend,
                   choices=["fbgemm", "qnnpack"],
                   help="quantization backend; fbgemm is x86, qnnpack is ARM")
    g.add_argument("--qat-max-images", type=int, default=d.qat_max_images,
                   help="images-seen budget, which is also what the LR cosine "
                        "anneals over (0 = epochs * len(loader))")
    g.add_argument("--qat-ckpt-every-images", type=int,
                   default=d.qat_ckpt_every_images,
                   help="checkpoint + eval every N images seen")
    g.add_argument("--qat-freeze-observer-frac", type=float,
                   default=d.qat_freeze_observer_frac, metavar="FRAC",
                   help="freeze fake-quant observers and BN stats after this "
                        "fraction of the budget (1.0 = never)")
    g.add_argument("--qat-codebook-bits", type=int, default=d.qat_codebook_bits,
                   help="bit width of the quantized codebook")
    g.add_argument("--qat-quantize-codebook",
                   action=argparse.BooleanOptionalAction,
                   default=d.qat_quantize_codebook,
                   help="quantize the frozen codebook to int8 so the assignment "
                        "search is an integer dot product; --no- reproduces the "
                        "earlier QAT runs, which kept an fp32 codebook")
    g.add_argument("--qat-train-decoder", action=argparse.BooleanOptionalAction,
                   default=d.qat_train_decoder,
                   help="also train the decoder (the codebook is frozen either "
                        "way)")
    g.add_argument("--qat-prior-images", type=int, default=d.qat_prior_images,
                   help="re-estimate the frozen prior over N images at the end "
                        "of the run (0 = keep the fp32 run's prior)")
    g.add_argument("--qat-prior-floor", type=float, default=d.qat_prior_floor,
                   help="add-one smoothing applied to the re-estimated counts")
    g.add_argument("--qat-eval-dir",
                   default=os.environ.get("NIC_EVAL_DIR", d.qat_eval_dir),
                   metavar="DIR",
                   help="full-resolution eval corpus, e.g. the CLIC test folder "
                        "(env: NIC_EVAL_DIR). Empty disables the in-loop eval")
    g.add_argument("--qat-eval-images", type=int, default=d.qat_eval_images,
                   help="how many images of --qat-eval-dir to score; the INT8 "
                        "encoder is CPU-only, so full resolution is slow")
    g.add_argument("--qat-eval-decoder-device",
                   default=d.qat_eval_decoder_device,
                   help="the encoder and quantizer always run on CPU at eval "
                        "time (fbgemm has no CUDA kernels)")

    p.set_defaults(name="qat", image_size=448, batch_size=8, crop_only=True,
                   amp=False, vis_every=0, ckpt_every=0)
    return p
